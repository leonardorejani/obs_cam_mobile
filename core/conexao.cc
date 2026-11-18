#include "conexao.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>

#include "usbmux.h"

namespace fmappcam {

namespace {
constexpr auto kEsperaCallbackNoParar = std::chrono::milliseconds(50);
}

// Estado compartilhado entre a Conexao e a thread. A thread segura um shared_ptr proprio, entao pode terminar
// depois da Conexao ser destruida (callback lento) sem tocar em memoria liberada.
struct Conexao::Nucleo {
  OpcoesConexao op;
  AoPacote aoPacote;
  AoEstado aoEstado;
  std::atomic<bool> parar{false};
  std::atomic<bool> conectada{false};
  std::atomic<int> reconexoes{0};
  std::mutex mEnvio;
  std::mutex mDados;
  Soquete sock = kSoqueteInvalido;
  RespostaHandshake resp;
  std::string udidAtual;
  bool viaRede = false;      // com mDados: a conexao atual usa a entrada Network do usbmuxd
  bool migrarParaUsb = false;  // so a thread da conexao: a sessao saiu porque a entrada USB voltou
  SincroniaRelogio sinc;

  // Porta dos callbacks (ver Conexao::parar).
  std::mutex mPorta;
  std::condition_variable cvPorta;
  bool parando = false;   // parar() comecou: so a notificacao final ainda passa
  bool liberado = false;  // parar() vai retornar (ou retornou): nada mais passa
  bool terminou = false;  // a thread acabou o que tinha para entregar

  void laco();
  Soquete abrir(std::string& erro);
  bool sessao(Soquete s, std::string& motivo, bool& fatal);
  void dormir(int ms);

  bool podeChamar(bool final) {
    std::lock_guard<std::mutex> l(mPorta);
    return !liberado && (final || !parando);
  }
  void avisar(EstadoConexao e, const std::string& m, bool final = false) {
    if (aoEstado && podeChamar(final)) aoEstado(e, m);
  }
  void entregar(const Pacote& p, uint64_t chegada) {
    if (aoPacote && podeChamar(false)) aoPacote(p, chegada);
  }
};

Conexao::Conexao(OpcoesConexao op) : op_(std::move(op)) {
  if (op_.usb && op_.usbmuxCaminho.empty()) op_.usbmuxCaminho = caminhoUsbmuxPadrao();
}

Conexao::~Conexao() { parar(); }

void Conexao::definirCallbacks(AoPacote p, AoEstado e) {
  aoPacote_ = std::move(p);
  aoEstado_ = std::move(e);
}

std::shared_ptr<Conexao::Nucleo> Conexao::nucleo() const {
  std::lock_guard<std::mutex> l(mN_);
  return n_;
}

void Conexao::iniciar() {
  std::lock_guard<std::mutex> l(mN_);
  if (th_.joinable()) return;
  iniciarRede();
  auto n = std::make_shared<Nucleo>();
  n->op = op_;
  n->aoPacote = aoPacote_;
  n->aoEstado = aoEstado_;
  if (n_) {  // reinicio depois de parar(): a resposta e o aparelho da ultima conexao continuam consultaveis
    std::lock_guard<std::mutex> ld(n_->mDados);
    n->resp = n_->resp;
    n->udidAtual = n_->udidAtual;
  }
  n_ = n;
  th_ = std::thread([n] { n->laco(); });
}

void Conexao::parar() {
  std::shared_ptr<Nucleo> n;
  std::thread th;
  {
    std::lock_guard<std::mutex> l(mN_);
    n = n_;
    th = std::move(th_);
  }
  if (!n || !th.joinable()) return;
  {
    std::lock_guard<std::mutex> l(n->mPorta);
    n->parando = true;
  }
  n->parar = true;  // acorda connect, usbmux, leituras (ate ~10 ms) e o dormir
  {
    std::lock_guard<std::mutex> l(n->mEnvio);
    if (n->sock != kSoqueteInvalido) desligarSoquete(n->sock);
  }
  bool terminou;
  {
    std::unique_lock<std::mutex> l(n->mPorta);
    n->cvPorta.wait_for(l, kEsperaCallbackNoParar, [&] { return n->terminou; });
    n->liberado = true;
    terminou = n->terminou;
  }
  if (terminou) th.join();
  else th.detach();  // callback lento em andamento: acaba sozinho, sem chamar mais nada
}

void Conexao::Nucleo::dormir(int ms) {
  for (int i = 0; i < ms && !parar; i += 20) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

bool Conexao::enviarComando(const std::string& json) {
  auto n = nucleo();
  if (!n) return false;
  std::lock_guard<std::mutex> l(n->mEnvio);
  if (n->sock == kSoqueteInvalido || !n->conectada) return false;
  auto b = serializarPacote(criarComando(agoraMonotonicoUs(), json));
  return enviarTudo(n->sock, b.data(), b.size());
}

std::string Conexao::udidAtual() const {
  auto n = nucleo();
  if (!n) return {};
  std::lock_guard<std::mutex> l(n->mDados);
  return n->udidAtual;
}
RespostaHandshake Conexao::resposta() const {
  auto n = nucleo();
  if (!n) return {};
  std::lock_guard<std::mutex> l(n->mDados);
  return n->resp;
}
bool Conexao::sincronizada() const {
  auto n = nucleo();
  if (!n) return false;
  std::lock_guard<std::mutex> l(n->mDados);
  return n->sinc.pronta();
}
bool Conexao::atrasoUs(uint64_t chegada, uint64_t tempoQuadro, int64_t& atraso) const {
  auto n = nucleo();
  if (!n) return false;
  std::lock_guard<std::mutex> l(n->mDados);
  if (!n->sinc.pronta()) return false;
  atraso = n->sinc.atraso(chegada, tempoQuadro);
  return true;
}
int64_t Conexao::idaEVoltaUs() const {
  auto n = nucleo();
  if (!n) return 0;
  std::lock_guard<std::mutex> l(n->mDados);
  return n->sinc.idaEVolta();
}
bool Conexao::pelaRede() const {
  auto n = nucleo();
  if (!n) return false;
  std::lock_guard<std::mutex> l(n->mDados);
  return n->viaRede;
}
int Conexao::reconexoes() const {
  auto n = nucleo();
  return n ? n->reconexoes.load() : 0;
}
bool Conexao::conectada() const {
  auto n = nucleo();
  return n && n->conectada;
}

Soquete Conexao::Nucleo::abrir(std::string& erro) {
  if (!op.usb) return conectarTcp(op.host, op.porta, 3000, erro);
  std::vector<DispositivoUsb> lista;
  if (!listarDispositivos(op.usbmuxCaminho, lista, erro)) return kSoqueteInvalido;
  DispositivoUsb d;
  if (escolherDispositivo(lista, op.udid, d)) {  // USB primeiro; sem ela, a entrada Network do mesmo UDID
    {
      std::lock_guard<std::mutex> l(mDados);
      udidAtual = d.serial;
      viaRede = d.conexao != "USB";
    }
    return conectarDispositivoUsb(op.usbmuxCaminho, d.id, op.porta, erro);
  }
  erro = op.udid.empty() ? "nenhum iPhone no cabo" : "aparelho " + op.udid + " nao esta no cabo";
  return kSoqueteInvalido;
}

void Conexao::Nucleo::laco() {
  definirCancelamentoDaThread(&parar);
  while (!parar) {
    avisar(EstadoConexao::Conectando, "");
    std::string erro;
    Soquete s = abrir(erro);
    bool fatal = false;
    if (s != kSoqueteInvalido) {
      desativarNagle(s);
      ajustarTimeoutEnvio(s, 2000);
      {
        std::lock_guard<std::mutex> l(mEnvio);
        sock = s;
      }
      // parar() pode ter passado entre o abrir e o registro do soquete: sem isto ele nao o veria
      if (parar) desligarSoquete(s);
      sessao(s, erro, fatal);
      conectada = false;
      {
        std::lock_guard<std::mutex> l(mEnvio);
        sock = kSoqueteInvalido;
      }
      fecharSoquete(s);
    }
    if (parar) break;
    if (migrarParaUsb) {  // a entrada USB voltou: a rede ja foi solta, abre o cabo na hora (sem avisar queda nem esperar)
      migrarParaUsb = false;
      ++reconexoes;
      continue;
    }
    avisar(fatal ? EstadoConexao::ErroFatal : EstadoConexao::Desconectado, erro);
    if (fatal || !op.reconectar) break;
    ++reconexoes;
    dormir(op.atrasoReconexaoMs);
  }
  avisar(EstadoConexao::Desconectado, "parado", /*final=*/true);
  definirCancelamentoDaThread(nullptr);
  {
    std::lock_guard<std::mutex> l(mPorta);
    terminou = true;
  }
  cvPorta.notify_all();
}

// Roda handshake e o laco de recepcao. Devolve ao sair com o motivo.
bool Conexao::Nucleo::sessao(Soquete s, std::string& motivo, bool& fatal) {
  std::string pedido = montarPedidoHandshake(op.pedido);
  if (!enviarTudo(s, pedido.data(), pedido.size())) {
    motivo = "falha ao enviar handshake";
    return false;
  }
  std::string linha;
  ResultadoIO r = lerLinha(s, kLinhaHandshakeMaxima, 5000, linha);
  if (r != ResultadoIO::Ok) {
    motivo = r == ResultadoIO::Timeout ? "iPhone nao respondeu ao handshake" : "handshake interrompido";
    return false;
  }
  RespostaHandshake resp_;
  if (!lerRespostaHandshake(linha, resp_)) {
    motivo = "resposta de handshake invalida";
    return false;
  }
  if (!resp_.ok) {
    motivo = resp_.erro;
    if (resp_.erro == "versao") {
      int maior = 0;
      for (int v : resp_.vSuportadas) maior = std::max(maior, v);
      motivo = op.pedido.v > maior ? "versao: atualize o app do iPhone" : "versao: atualize a extensao";
      fatal = true;
    }
    return false;
  }
  {
    std::lock_guard<std::mutex> l(mDados);
    resp = resp_;
    sinc.reiniciar();
  }
  conectada = true;
  avisar(EstadoConexao::Conectado, resp_.nome.empty() ? resp_.aparelho : resp_.nome);

  // Conectado pela entrada Network: um vigia olha a lista do usbmuxd e avisa quando a entrada USB do mesmo UDID volta.
  // Roda em thread propria (um usbmuxd mudo pode segurar a consulta por segundos e o ping nao pode parar).
  bool comVigia = false;
  std::shared_ptr<std::atomic<bool>> vigiaParar, usbVoltou;
  {
    std::lock_guard<std::mutex> l(mDados);
    comVigia = viaRede && op.usb;
  }
  if (comVigia) {
    vigiaParar = std::make_shared<std::atomic<bool>>(false);
    usbVoltou = std::make_shared<std::atomic<bool>>(false);
    std::thread([caminho = op.usbmuxCaminho, udid = udidAtual, vigiaParar, usbVoltou] {
      definirCancelamentoDaThread(vigiaParar.get());
      while (!*vigiaParar) {
        std::vector<DispositivoUsb> l;
        std::string e;
        if (listarDispositivos(caminho, l, e))
          for (auto& d : l)
            if (d.conexao == "USB" && d.serial == udid) *usbVoltou = true;
        for (int i = 0; i < 10 && !*vigiaParar && !*usbVoltou; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (*usbVoltou) break;
      }
    }).detach();
  }
  struct FimVigia {
    std::shared_ptr<std::atomic<bool>> p;
    ~FimVigia() {
      if (p) *p = true;
    }
  } fimVigia{vigiaParar};

  ParserPacotes parser;
  std::vector<uint8_t> buf(256 * 1024);
  const uint64_t intervalo = static_cast<uint64_t>(op.intervaloPingMs) * 1000;
  const uint64_t limiteSemDados = static_cast<uint64_t>(op.timeoutSemDadosMs) * 1000;
  uint64_t ultimoRx = agoraMonotonicoUs();
  uint64_t proximoPing = ultimoRx;
  while (!parar) {
    uint64_t agora = agoraMonotonicoUs();
    if (agora >= proximoPing) {
      auto b = serializarPacote(criarPing(agora));
      bool ok;
      {
        std::lock_guard<std::mutex> l(mEnvio);
        ok = enviarTudo(s, b.data(), b.size());
      }
      if (!ok) {
        motivo = "falha ao enviar ping";
        return false;
      }
      proximoPing += intervalo;
      if (proximoPing <= agora) proximoPing = agora + intervalo;
    }
    if (usbVoltou && *usbVoltou) {
      migrarParaUsb = true;  // fecha a rede AGORA (o iPhone atende um cliente so) e o laco abre o cabo em seguida
      motivo = "migrando para o cabo";
      return false;
    }
    if (agora - ultimoRx > limiteSemDados) {
      motivo = "3 s sem dados";
      return false;
    }
    uint64_t ateping = proximoPing > agora ? proximoPing - agora : 0;
    uint64_t espera = std::min<uint64_t>({ateping, limiteSemDados - (agora - ultimoRx), 100000});
    int rr = esperarLeitura(s, static_cast<int>((espera + 999) / 1000));
    if (rr < 0) {
      motivo = "erro de leitura";
      return false;
    }
    if (rr == 0) continue;
    long n = lerAlgum(s, buf.data(), buf.size());
    if (n <= 0) {
      motivo = n == 0 ? "iPhone fechou a conexao" : "erro de leitura";
      return false;
    }
    const uint64_t chegada = agoraMonotonicoUs();
    ultimoRx = chegada;
    parser.alimentar(buf.data(), static_cast<size_t>(n));
    Pacote p;
    while (parser.proximo(p)) {
      if (p.tipo == kTipoPong) {
        if (p.dados.size() == 8) {
          std::lock_guard<std::mutex> l(mDados);
          sinc.registrar(lerU64BE(p.dados.data()), chegada, p.tempo);
        }
        continue;
      }
      entregar(p, chegada);
      if (parar) break;
    }
    if (parser.erro()) {
      motivo = "erro de protocolo";
      return false;
    }
  }
  motivo = "parado";
  return true;
}

}  // namespace fmappcam
