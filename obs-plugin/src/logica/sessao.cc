#include "sessao.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <vector>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <set>
#include <thread>

#include "conexao.h"
#include "decodificador.h"

extern "C" {
#include <libavutil/frame.h>
}

using namespace fmappcam;

namespace fmcam {

namespace {
std::atomic<int> g_encerrando{0};
std::atomic<int> g_atrasoConectadoMs{0};  // so teste: segura a conexao recem aceita antes de reservar o iPhone
std::atomic<int> g_noAtrasoConectado{0};
// Se o quadro que acabou de chegar ja tem mais que isso de atraso (captura ate agora), o decodificador nao esta dando conta:
// quadros que nao sao chave sao pulados e um quadro-chave e pedido. Sem isso o iPhone enfileira ate 1 s de video (limite da
// fila dele) e o atraso so cresce. Nao pula nunca quadro-chave.
constexpr int64_t kLimiteAtrasoUs = 400000;
// FMCAM_LIMITE_ATRASO_MS (so diagnostico e teste): outro limite, 0 desliga.
int64_t limiteDeAtrasoUs() {
  static const int64_t v = [] {
    if (const char* e = std::getenv("FMCAM_LIMITE_ATRASO_MS")) return static_cast<int64_t>(std::atoll(e)) * 1000;
    return kLimiteAtrasoUs;
  }();
  return v;
}

std::string textoOuNumero(const ObjetoJson& o, const char* k, const std::string& padrao) {
  if (auto* v = buscarJson(o, k, ValorJson::Tipo::Texto)) return v->s;
  if (auto* v = buscarJson(o, k, ValorJson::Tipo::Numero)) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", v->n);
    return b;
  }
  return padrao;
}

bool contem(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

double percentilMs(std::vector<int64_t> v, double p) {
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  size_t i = static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
  return static_cast<double>(v[std::min(i, v.size() - 1)]) / 1000.0;
}

void classificar(const std::string& m, EstadoAparelho& e, std::string& msg) {
  if (contem(m, "ocupado")) {
    e = EstadoAparelho::Ocupado;
    msg = "ocupado: iPhone em uso por outro computador";
  } else if (contem(m, "recusou a conexao")) {
    e = EstadoAparelho::SemApp;
    msg = "sem app: abra o OBS Cam Mobile no iPhone";
  } else if (contem(m, "nao esta ligado") || contem(m, "nao esta no cabo") || contem(m, "nenhum iPhone")) {
    e = EstadoAparelho::SemCabo;
    msg = "iPhone fora do cabo";
  } else if (contem(m, "camera")) {
    e = EstadoAparelho::Falha;
    msg = "camera do iPhone indisponivel";
  } else {
    e = EstadoAparelho::Procurando;
    msg = "reconectando (" + m + ")";
  }
}
}  // namespace

int sessoesEncerrando() { return g_encerrando; }
void SessaoFonte::definirAtrasoDeTesteConectadoMs(int ms) { g_atrasoConectadoMs = ms; }
int SessaoFonte::entradasNoAtrasoDeTeste() { return g_noAtrasoConectado; }
bool aguardarEncerramentos(int ms) {
  for (int i = 0; i < ms / 10 && g_encerrando > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return g_encerrando == 0;
}

struct SessaoFonte::Impl {
  std::weak_ptr<Impl> eu;  // para callbacks que podem rodar depois da sessao acabar (canal do painel)
  ConfigFonte cfg;
  std::string usbmux;
  RegistroAparelhos* reg;

  std::mutex mCb;
  AoQuadro aoQuadro;
  AoEstado aoEstado;

  mutable std::mutex mEst;
  EstadoSessao est;

  std::atomic<bool> parando{false};
  std::thread espera;  // so reserva o aparelho e liga a conexao
  std::unique_ptr<Conexao> con;
  std::unique_ptr<Decodificador> dec;  // so a thread da conexao toca depois de ligada
  bool iniciada = false;

  // janela de fps
  uint64_t janelaIni = 0;
  uint64_t janelaQuadros = 0;
  StatusIphone ultimoStatus;  // com mEst
  std::atomic<bool> noAr{false};
  bool descartando = false;          // so a thread da conexao: pulando ate o proximo quadro-chave
  uint64_t ultimoPedidoChaveUs = 0;  // idem
  bool pediuChaveHw = false;         // idem: ja pediu o quadro-chave da troca para o decodificador de hardware
  // Medidas para o status ao vivo e o Testar (so a thread que entrega o quadro mexe): atraso captura ate o quadro
  // entregue ao OBS, por janela de 1 s, guardando as 3 ultimas janelas.
  std::vector<int64_t> entregasJanela;
  std::deque<std::vector<int64_t>> entregasJanelas;
  uint64_t descartadosAntes = 0;
  std::deque<uint64_t> descartadosJanelas;

  // Modo "Mais estavel": os quadros decodificados esperam numa fila curta e saem no instante captura + menor atraso
  // visto nos ultimos 2 s + reserva. Um quadro atrasado sai na hora (nunca segura mais que a reserva).
  struct Pendente {
    AVFrame* f = nullptr;
    uint64_t vence = 0;
    uint64_t tempoIphone = 0;
  };
  std::mutex mFila;
  std::condition_variable cvFila;
  std::deque<Pendente> fila;
  std::thread apresentador;
  std::deque<std::pair<uint64_t, int64_t>> atrasosRecentes;  // (instante, atraso decodificado) dos ultimos 2 s

  bool estavel() const { return cfg.recepcao == "estavel"; }
  std::mutex mCon;  // protege a troca de `con` (modo Wi-Fi por id) contra envio de comando de outra thread

  // O tally sai por uma thread propria: o sinal "activate" do OBS chega na thread de video, e um envio em soquete
  // parado (ate 2 s de timeout) nao pode segurar o render.
  std::thread tally;
  std::mutex mT;
  std::condition_variable cvT;
  std::atomic<bool> ultimoEnviado{false};  // o que o iPhone da conexao atual sabe (nasce false)

  bool enviarNoAr(bool v) {
    std::lock_guard<std::mutex> l(mCon);
    return con && con->enviarComando(std::string("{\"cmd\":\"noar\",\"valor\":") + (v ? "true" : "false") + "}");
  }
  void cutucarTally() {
    std::lock_guard<std::mutex> l(mT);
    cvT.notify_all();
  }
  void lacoTally() {
    std::unique_lock<std::mutex> l(mT);
    while (!parando) {
      bool quer = noAr;
      if (quer != ultimoEnviado) {
        l.unlock();
        bool ok = enviarNoAr(quer);
        l.lock();
        if (ok) ultimoEnviado = quer;
      }
      cvT.wait_for(l, std::chrono::milliseconds(300));
    }
  }

  // ---- reserva do iPhone dentro do processo ----
  std::mutex mRes;
  std::set<std::string> reservadas;  // chaves que esta sessao segura no registro
  std::string udidTentado;           // iPhone que o cabo escolheu (config sem UDID), mesmo recusado
  std::string idRecusado;            // id do app que o iPhone devolveu na recusa "ocupado"
  std::atomic<bool> bloqueada{false};  // esperando outra fonte do OBS soltar o mesmo iPhone
  static constexpr const char* kMsgOutraFonte = "ocupado: outra fonte do OBS ja usa este iPhone";

  std::string chaveReserva() {
    if (!cfg.wifiId.empty()) return "wifi:" + cfg.wifiId;
    if (cfg.wifi) return "wifi:" + cfg.wifiIp;
    if (!cfg.udid.empty()) return cfg.udid;
    std::lock_guard<std::mutex> l(mRes);
    return udidTentado;
  }
  void guardarReserva(const std::string& k) {
    std::lock_guard<std::mutex> l(mRes);
    reservadas.insert(k);
  }
  void soltarReservas() {
    std::set<std::string> r;
    {
      std::lock_guard<std::mutex> l(mRes);
      r.swap(reservadas);
    }
    for (auto& k : r) reg->liberar(k, this);
  }
  // true = o iPhone e nosso (ou de ninguem do processo). Falso = outra fonte do OBS ja o segura, por qualquer caminho
  // (cabo, Wi-Fi por id ou IP): nao solta mais nada na rede ate ela sair.
  bool reservaLivre(const std::string& idExtra = "") {
    std::string k = chaveReserva();
    if (k.empty()) return true;
    std::string id = idExtra;
    {
      std::lock_guard<std::mutex> l(mRes);
      if (id.empty()) id = idRecusado;
    }
    if (reg->reservar(k, this, id)) {
      guardarReserva(k);
      return true;
    }
    soltarReservas();  // quem espera nao segura nada: senao ela mesma viraria "dona" fantasma de um caminho
    return false;
  }
  bool podeConectar() {
    if (parando) return false;
    if (!reservaLivre()) {
      if (!bloqueada.exchange(true)) setEstado(EstadoAparelho::Ocupado, kMsgOutraFonte);
      return false;
    }
    if (bloqueada.exchange(false)) setEstado(EstadoAparelho::Procurando, "procurando");
    return true;
  }
  fmappcam::OpcoesConexao opcoes(const ConfigFonte& c) {
    auto op = montarOpcoes(c, usbmux);
    op.podeConectar = [this] { return podeConectar(); };
    return op;
  }

  void publicarStatus() {
    StatusIphone s;
    {
      std::lock_guard<std::mutex> l(mEst);
      s = ultimoStatus;
      est.status = ultimoStatus;
    }
    std::string k = chave();
    if (!k.empty() && !parando && reg->ehDona(k, this)) reg->definirStatus(k, s);
  }

  std::string chave() const {
    if (!cfg.wifiId.empty()) return "wifi:" + cfg.wifiId;
    if (cfg.wifi) return "wifi:" + cfg.wifiIp;
    std::lock_guard<std::mutex> l(mEst);
    return cfg.udid.empty() ? est.udid : cfg.udid;
  }

  void publicar(bool soSeMudou, const EstadoSessao& antes) {
    EstadoSessao copia;
    {
      std::lock_guard<std::mutex> l(mEst);
      copia = est;
    }
    if (soSeMudou && copia.estado == antes.estado && copia.mensagem == antes.mensagem && copia.nome == antes.nome &&
        copia.pelaRede == antes.pelaRede &&
        copia.udid == antes.udid)
      return;
    std::lock_guard<std::mutex> l(mCb);
    if (aoEstado && !parando) aoEstado(copia);
  }

  void setEstado(EstadoAparelho e, const std::string& msg, bool pelaRede = false) {
    EstadoSessao antes;
    {
      std::lock_guard<std::mutex> l(mEst);
      antes = est;
      est.estado = e;
      est.mensagem = msg;
      est.pelaRede = e == EstadoAparelho::Conectado && pelaRede;
    }
    // So quem segura o iPhone escreve o estado dele no registro: a fonte que esta esperando a outra soltar nao pode
    // sobrescrever o "conectado" da dona.
    std::string k = chave();
    if (!k.empty() && !parando && reg->ehDona(k, this)) reg->definirEstado(k, e, msg, pelaRede);
    publicar(true, antes);
  }

  void aoEstadoCon(EstadoConexao e, const std::string& m) {
    if (parando) return;
    switch (e) {
      case EstadoConexao::Conectando:
        return;  // a cada tentativa; o estado de erro anterior continua valendo
      case EstadoConexao::Conectado: {
        std::string erro;
        auto novo = std::make_unique<Decodificador>();  // referencia limpa a cada conexao
        novo->definirModoHardware(modoHardware(cfg.decodificacao));
        if (!novo->abrir(erro)) {
          setEstado(EstadoAparelho::Falha, "decodificador: " + erro);
          return;
        }
        dec = std::move(novo);
        descartando = false;
        pediuChaveHw = false;
        if (int ms = g_atrasoConectadoMs.load()) {
          ++g_noAtrasoConectado;
          std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
        auto r = con->resposta();
        std::string udid = !cfg.wifiId.empty() ? "wifi:" + cfg.wifiId : cfg.wifi ? "wifi:" + cfg.wifiIp : (cfg.udid.empty() ? con->udidAtual() : cfg.udid);
        {
          std::lock_guard<std::mutex> l(mEst);
          est.udid = udid;
          est.nome = r.nome.empty() ? r.aparelho : r.nome;
          est.modelo = r.aparelho;
          est.largura = r.w;
          est.altura = r.h;
          est.respLargura = r.w;
          est.respAltura = r.h;
          est.respFps = r.fps;
          est.reconexoes = con->reconexoes();
          ultimoStatus.bitrate = 0;  // vale o status desta conexao, nao o da anterior
          ultimoStatus.qualidadeObs = false;
          ultimoStatus.presetObs = false;
          ultimoStatus.bitrateObs = false;
        }
        if (!udid.empty()) {
          if (reg->reservar(udid, this, r.id)) guardarReserva(udid);
          reg->definirIdentidade(udid, r.nome.empty() ? r.aparelho : r.nome, r.aparelho, r.id, r.w, r.h);
          std::weak_ptr<Impl> w = eu;
          reg->definirCanal(udid, this, [w](const std::string& json) {
            auto im = w.lock();
            if (!im || im->parando) return false;
            std::lock_guard<std::mutex> l(im->mCon);
            return im->con && im->con->enviarComando(json);
          });
          // A fonte pode ter sido removida depois da checagem de `parando` no topo: o parar() ja limpou as reservas e esta
          // reserva nova ficaria presa (painel "em uso" e outra fonte no mesmo iPhone "ocupado" ate reiniciar o OBS).
          if (parando) {
            reg->liberar(udid, this);
            soltarReservas();
            return;
          }
        }
        janelaIni = agoraMonotonicoUs();
        janelaQuadros = 0;
        const bool pelaRede = con->pelaRede();
        setEstado(EstadoAparelho::Conectado, pelaRede ? "conectado pelo Wi-Fi (cabo indisponivel)" : "conectado", pelaRede);
        ultimoEnviado = false;  // o iPhone nasce "fora do ar" a cada conexao
        cutucarTally();
        return;
      }
      case EstadoConexao::Desconectado: {
        if (m == "parado") return;
        EstadoAparelho ea;
        std::string msg;
        classificar(m, ea, msg);
        if (con) {
          // Config sem UDID: o iPhone que o cabo escolheu (mesmo recusado) passa a ser a chave da reserva.
          std::string u = con->udidAtual();
          std::lock_guard<std::mutex> l(mRes);
          if (!u.empty() && cfg.udid.empty() && !cfg.wifi && cfg.wifiId.empty()) udidTentado = u;
          // Recusado por "ocupado": o app novo diz de quem e a vaga, e se for outra fonte nossa paramos de bater na porta.
          if (ea == EstadoAparelho::Ocupado) {
            auto rec = con->recusa();
            if (!rec.id.empty()) idRecusado = rec.id;
          }
        }
        if (ea == EstadoAparelho::Ocupado && !reservaLivre()) {
          bloqueada = true;
          msg = kMsgOutraFonte;
        }
        {
          std::lock_guard<std::mutex> l(mEst);
          est.reconexoes = con ? con->reconexoes() : est.reconexoes;
          est.fps = 0;
        }
        setEstado(ea, msg);
        return;
      }
      case EstadoConexao::ErroFatal: {
        std::string msg = contem(m, "extensao") ? "atualize a extensao" : contem(m, "app do iPhone") ? "atualize o app do iPhone" : m;
        setEstado(EstadoAparelho::Falha, msg);
        return;
      }
    }
  }

  void aoPacote(const Pacote& p, uint64_t chegadaUs) {
    if (parando) return;
    if (p.tipo == kTipoConfig) {
      if (dec) dec->definirConfig(p.dados.data(), p.dados.size());
    } else if (p.tipo == kTipoQuadro) {
      if (!dec || p.dados.size() < 2) return;
      const bool chave = (p.dados[0] & 1) != 0;
      int64_t atrasoAgora = 0;
      const bool sabeAtraso = con && con->atrasoUs(chegadaUs, p.tempo, atrasoAgora);
      const int64_t limite = limiteDeAtrasoUs();
      if (limite > 0 && !chave && ((sabeAtraso && atrasoAgora > limite) || descartando)) {
        descartando = true;
        {
          std::lock_guard<std::mutex> l(mEst);
          ++est.quadrosDescartados;
        }
        if (con && chegadaUs - ultimoPedidoChaveUs > 1000000) {
          ultimoPedidoChaveUs = chegadaUs;
          con->enviarComando("{\"cmd\":\"keyframe\"}");
        }
        return;
      }
      descartando = false;
      auto r = dec->decodificar(p.dados.data() + 1, p.dados.size() - 1, chave);
      if (r == Decodificador::Resultado::Erro) {
        std::lock_guard<std::mutex> l(mEst);
        est.errosDecodificacao = dec->erros();
        if (con) con->enviarComando("{\"cmd\":\"keyframe\"}");
        return;
      }
      if (dec->aguardaChaveParaHardware() && !pediuChaveHw && con) {
        pediuChaveHw = true;  // video acima de 1080p: troca para o hardware no proximo quadro-chave, que pedimos agora
        con->enviarComando("{\"cmd\":\"keyframe\"}");
      }
      if (r != Decodificador::Resultado::Quadro) return;
      int64_t atraso = 0;
      bool temAtraso = con && con->atrasoUs(chegadaUs, p.tempo, atraso);
      {
        std::lock_guard<std::mutex> l(mEst);
        est.hardware = dec->usaHardware();
        if (temAtraso) est.atrasoCanoMs = atraso / 1000.0;
      }
      if (estavel()) enfileirar(dec->quadro(), p.tempo);
      else entregar(dec->quadro(), p.tempo);
      ++janelaQuadros;
      uint64_t dt = chegadaUs - janelaIni;
      if (dt >= 1000000) {
        double fps = janelaQuadros * 1e6 / double(dt);
        {
          std::lock_guard<std::mutex> l(mEst);
          est.fps = fps;
        }
        janelaIni = chegadaUs;
        janelaQuadros = 0;
        {
          std::lock_guard<std::mutex> l(mEst);
          ultimoStatus.fps = fps;
        }
        fecharJanelaMedidas();
        publicarStatus();
      }
    } else if (p.tipo == kTipoStatus) {
      ObjetoJson o;
      if (!lerObjetoJson(std::string(p.dados.begin(), p.dados.end()), o)) return;
      {
        std::lock_guard<std::mutex> l(mEst);
        if (auto* v = buscarJson(o, "bateria", ValorJson::Tipo::Numero)) ultimoStatus.bateria = v->n;
        if (auto* v = buscarJson(o, "carregando", ValorJson::Tipo::Bool)) ultimoStatus.carregando = v->b;
        if (auto* v = buscarJson(o, "temp", ValorJson::Tipo::Texto)) ultimoStatus.temp = v->s;
        if (auto* v = buscarJson(o, "kbps", ValorJson::Tipo::Numero)) ultimoStatus.kbps = static_cast<int>(v->n);
        if (auto* v = buscarJson(o, "fps", ValorJson::Tipo::Numero); v && ultimoStatus.fps == 0) ultimoStatus.fps = v->n;
        if (auto* v = buscarJson(o, "iso", ValorJson::Tipo::Numero)) ultimoStatus.iso = static_cast<int>(v->n);
        if (auto* v = buscarJson(o, "zoom", ValorJson::Tipo::Numero)) ultimoStatus.zoom = v->n;
        if (auto* v = buscarJson(o, "bias", ValorJson::Tipo::Numero)) {
          ultimoStatus.bias = v->n;
          ultimoStatus.temBias = true;
        }
        if (auto* v = buscarJson(o, "w", ValorJson::Tipo::Numero)) ultimoStatus.w = static_cast<int>(v->n);
        if (auto* v = buscarJson(o, "h", ValorJson::Tipo::Numero)) ultimoStatus.h = static_cast<int>(v->n);
        if (auto* v = buscarJson(o, "fps_alvo", ValorJson::Tipo::Numero)) {
          ultimoStatus.fpsAlvo = v->n;
          if (v->n > 0) est.respFps = v->n;  // troca de qualidade feita no iPhone: o formato entregue muda sem reconectar
        }
        if (auto* v = buscarJson(o, "bitrate", ValorJson::Tipo::Numero); v && v->n >= 0 && v->n < 4e18)
          ultimoStatus.bitrate = static_cast<int64_t>(v->n);
        if (auto* v = buscarJson(o, "qualidade_obs", ValorJson::Tipo::Bool)) ultimoStatus.qualidadeObs = v->b;
        if (auto* v = buscarJson(o, "preset_obs", ValorJson::Tipo::Bool)) ultimoStatus.presetObs = v->b;
        if (auto* v = buscarJson(o, "bitrate_obs", ValorJson::Tipo::Bool)) ultimoStatus.bitrateObs = v->b;
        if (auto* v = buscarJson(o, "noar", ValorJson::Tipo::Bool)) ultimoStatus.noArIphone = v->b;
        if (auto* v = buscarJson(o, "lentes", ValorJson::Tipo::ArrayNumeros)) {
          ultimoStatus.lentes.clear();
          for (double x : v->arr)
            if (x > 0 && x < 100 && ultimoStatus.lentes.size() < 8) ultimoStatus.lentes.push_back(x);
        }
        if (auto* v = buscarJson(o, "fator", ValorJson::Tipo::Numero)) ultimoStatus.fator = v->n;
        auto* evMin = buscarJson(o, "ev_min", ValorJson::Tipo::Numero);
        auto* evMax = buscarJson(o, "ev_max", ValorJson::Tipo::Numero);
        if (evMin && evMax && evMin->n <= evMax->n && evMin->n > -100 && evMax->n < 100) {
          ultimoStatus.evMin = evMin->n;
          ultimoStatus.evMax = evMax->n;
          ultimoStatus.temFaixaEv = true;
        }
        if (auto* v = buscarJson(o, "preset", ValorJson::Tipo::Texto)) ultimoStatus.preset = v->s.substr(0, 16);
        auto* tae = buscarJson(o, "trava_ae", ValorJson::Tipo::Bool);
        auto* taf = buscarJson(o, "trava_af", ValorJson::Tipo::Bool);
        auto* tbb = buscarJson(o, "trava_bb", ValorJson::Tipo::Bool);
        if (tae || taf || tbb) {
          ultimoStatus.temTravas = true;
          if (tae) ultimoStatus.travaAe = tae->b;
          if (taf) ultimoStatus.travaAf = taf->b;
          if (tbb) ultimoStatus.travaBb = tbb->b;
        }
        ultimoStatus.lente = textoOuNumero(o, "lente", ultimoStatus.lente);
        ultimoStatus.foco = textoOuNumero(o, "foco", ultimoStatus.foco);
        ultimoStatus.exposicao = textoOuNumero(o, "exposicao", ultimoStatus.exposicao);
        ultimoStatus.branco = textoOuNumero(o, "branco", ultimoStatus.branco);
        ultimoStatus.obturador = textoOuNumero(o, "obturador", ultimoStatus.obturador);  // texto "1/60"
        ultimoStatus.noAr = noAr;
      }
      publicarStatus();
    }
  }

  // Entrega um quadro decodificado ao OBS (thread da conexao no modo normal, apresentador no "Mais estavel").
  void entregar(const AVFrame* f, uint64_t tempoIphone, bool outraThread = false) {
    QuadroSaida q;
    std::string erro;
    if (!converterQuadro(f, q, erro)) return;
    {
      std::lock_guard<std::mutex> l(mCb);
      if (aoQuadro && !parando) aoQuadro(q);
    }
    int64_t entrega = 0;
    bool temEntrega = false;
    if (outraThread) {  // apresentador: `con` pode estar sendo trocado (Wi-Fi por id)
      std::lock_guard<std::mutex> l(mCon);
      temEntrega = con && con->atrasoUs(agoraMonotonicoUs(), tempoIphone, entrega);
    } else {  // thread da conexao: o mesmo acesso sem trava que o resto de aoPacote faz
      temEntrega = con && con->atrasoUs(agoraMonotonicoUs(), tempoIphone, entrega);
    }
    std::lock_guard<std::mutex> l(mEst);
    ++est.quadros;
    est.jaTeveQuadro = true;
    est.largura = q.largura;
    est.altura = q.altura;
    est.respLargura = q.largura;  // o que o iPhone entrega agora (ele pode trocar a qualidade no meio da conexao)
    est.respAltura = q.altura;
    if (temEntrega && entregasJanela.size() < 1000) entregasJanela.push_back(entrega);
  }

  // A cada segundo com video: p50 e p95 do atraso ate o OBS nas ultimas 3 janelas, ida e volta e canal.
  void fecharJanelaMedidas() {
    std::lock_guard<std::mutex> l(mEst);
    entregasJanelas.push_back(std::move(entregasJanela));
    entregasJanela.clear();
    while (entregasJanelas.size() > 3) entregasJanelas.pop_front();
    std::vector<int64_t> todas;
    for (auto& j : entregasJanelas) todas.insert(todas.end(), j.begin(), j.end());
    ultimoStatus.atrasoMs = percentilMs(todas, 0.5);
    ultimoStatus.atrasoP95Ms = percentilMs(todas, 0.95);
    descartadosJanelas.push_back(est.quadrosDescartados - descartadosAntes);
    descartadosAntes = est.quadrosDescartados;
    while (descartadosJanelas.size() > 3) descartadosJanelas.pop_front();
    ultimoStatus.descartados = 0;
    for (auto d : descartadosJanelas) ultimoStatus.descartados += d;
    bool pelaRede = false;
    int64_t ida = 0;
    if (con) {  // so a thread da conexao troca `con`, e e ela que esta aqui (ou o apresentador, que nao chama isto)
      pelaRede = con->pelaRede();
      ida = con->idaEVoltaUs();
    }
    ultimoStatus.idaEVoltaMs = ida > 0 ? ida / 1000.0 : -1;
    ultimoStatus.canalCabo = !cfg.wifi && cfg.wifiId.empty() && !pelaRede;
  }

  void enfileirar(const AVFrame* f, uint64_t tempoIphone) {
    AVFrame* c = av_frame_clone(f);  // so referencia os buffers: sem copia de pixels
    if (!c) return;
    const uint64_t agora = agoraMonotonicoUs();
    int64_t atraso = 0;
    uint64_t vence = agora + static_cast<uint64_t>(kReservaEstavelMs) * 1000;
    bool sabe = con && con->atrasoUs(agora, tempoIphone, atraso);  // thread da conexao
    if (sabe && atraso >= 0) {
      atrasosRecentes.emplace_back(agora, atraso);
      while (!atrasosRecentes.empty() && agora - atrasosRecentes.front().first > 2000000) atrasosRecentes.pop_front();
      int64_t menor = atraso;
      for (auto& a : atrasosRecentes) menor = std::min(menor, a.second);
      // captura (no relogio daqui) + menor atraso recente + reserva; nunca antes de agora
      int64_t alvo = static_cast<int64_t>(agora) - atraso + menor + static_cast<int64_t>(kReservaEstavelMs) * 1000;
      vence = alvo > static_cast<int64_t>(agora) ? static_cast<uint64_t>(alvo) : agora;
    }
    {
      std::lock_guard<std::mutex> l(mFila);
      while (fila.size() >= 8) {  // nunca acumula: o mais velho sai da fila sem ir ao OBS
        av_frame_free(&fila.front().f);
        fila.pop_front();
      }
      fila.push_back({c, vence, tempoIphone});
      if (!apresentador.joinable())
        apresentador = std::thread([this] { lacoApresentador(); });
    }
    cvFila.notify_all();
  }

  void lacoApresentador() {
    std::unique_lock<std::mutex> l(mFila);
    while (!parando) {
      if (fila.empty()) {
        cvFila.wait_for(l, std::chrono::milliseconds(200));
        continue;
      }
      uint64_t agora = agoraMonotonicoUs();
      if (fila.front().vence > agora) {
        cvFila.wait_for(l, std::chrono::microseconds(fila.front().vence - agora));
        continue;
      }
      Pendente p = fila.front();
      fila.pop_front();
      l.unlock();
      entregar(p.f, p.tempoIphone, true);
      av_frame_free(&p.f);
      l.lock();
    }
    for (auto& p : fila) av_frame_free(&p.f);
    fila.clear();
  }

  // iPhone escolhido na lista do Wi-Fi: o IP vem do Bonjour e acompanha se o iPhone mudar de IP.
  void ligarWifiPorId() {
    bool avisou = false;
    while (!parando && !reservaLivre()) {
      if (!avisou) {
        setEstado(EstadoAparelho::Ocupado, kMsgOutraFonte);
        bloqueada = true;
        avisou = true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (bloqueada.exchange(false) && !parando) setEstado(EstadoAparelho::Procurando, "procurando");
    std::string ipAtual;
    uint16_t portaAtual = 0;
    bool avisouBusca = false;
    while (!parando) {
      ServicoWifi sv;
      bool achou = reg->buscarWifi(cfg.wifiId, sv);
      if (achou && (sv.ip != ipAtual || sv.porta != portaAtual)) {
        std::unique_ptr<Conexao> velha;
        {
          std::lock_guard<std::mutex> l(mCon);
          velha = std::move(con);
        }
        if (velha) velha->parar();  // esta thread nao e a da interface: pode esperar
        velha.reset();
        ConfigFonte c = cfg;
        c.wifi = true;
        c.wifiIp = sv.ip;
        c.wifiPorta = sv.porta;
        auto nova = std::make_unique<Conexao>(opcoes(c));
        nova->definirCallbacks([this](const Pacote& p, uint64_t t) { aoPacote(p, t); },
                               [this](EstadoConexao e, const std::string& m) { aoEstadoCon(e, m); });
        {
          std::lock_guard<std::mutex> l(mCon);
          con = std::move(nova);
        }
        con->iniciar();
        ipAtual = sv.ip;
        portaAtual = sv.porta;
      } else if (!achou && !con && !avisouBusca) {
        setEstado(EstadoAparelho::Procurando, "procurando o iPhone no Wi-Fi");
        avisouBusca = true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
  }

  void ligar() {
    if (!cfg.wifiId.empty()) {
      ligarWifiPorId();
      return;
    }
    // Um iPhone por fonte dentro do processo: a reserva e conferida pela propria Conexao antes de cada tentativa
    // (podeConectar), entao a segunda fonte espera quieta, sem tocar no iPhone, seja qual for o caminho dela.
    if (parando) return;
    auto nova = std::make_unique<Conexao>(opcoes(cfg));
    nova->definirCallbacks([this](const Pacote& p, uint64_t t) { aoPacote(p, t); },
                           [this](EstadoConexao e, const std::string& m) { aoEstadoCon(e, m); });
    {
      std::lock_guard<std::mutex> l(mCon);
      con = std::move(nova);
    }
    con->iniciar();
  }

  void pararBloqueante() {
    cutucarTally();
    if (tally.joinable()) tally.join();
    {
      std::lock_guard<std::mutex> l(mFila);
      cvFila.notify_all();
    }
    std::thread ap;
    {
      std::lock_guard<std::mutex> l(mFila);
      ap = std::move(apresentador);
    }
    if (espera.joinable()) espera.join();
    if (con) con->parar();
    if (ap.joinable()) ap.join();  // depois do parar: a thread da conexao nao cria mais apresentador
    {
      std::unique_lock<std::mutex> l(mFila);
      ap = std::move(apresentador);  // criado entre o primeiro move e o parar (raro)
      l.unlock();
      cvFila.notify_all();
      if (ap.joinable()) ap.join();
      l.lock();
      for (auto& p : fila) av_frame_free(&p.f);
      fila.clear();
    }
    con.reset();
  }
};

SessaoFonte::SessaoFonte(ConfigFonte cfg, std::string caminho, RegistroAparelhos* reg) : impl_(std::make_shared<Impl>()) {
  impl_->eu = impl_;
  impl_->cfg = normalizar(std::move(cfg));
  impl_->usbmux = std::move(caminho);
  impl_->reg = reg;
  impl_->est.udid = impl_->cfg.udid;
}

SessaoFonte::~SessaoFonte() { parar(1500); }

void SessaoFonte::definirCallbacks(AoQuadro q, AoEstado e) {
  std::lock_guard<std::mutex> l(impl_->mCb);
  impl_->aoQuadro = std::move(q);
  impl_->aoEstado = std::move(e);
}

void SessaoFonte::definirNoAr(bool v) {
  auto& im = *impl_;
  if (im.noAr.exchange(v) == v || im.parando) return;
  {
    std::lock_guard<std::mutex> l(im.mEst);
    im.est.noAr = v;
    im.ultimoStatus.noAr = v;
  }
  im.cutucarTally();  // sem conexao ainda: a Conectado manda
  im.publicarStatus();
}

void SessaoFonte::iniciar() {
  if (impl_->iniciada) return;
  impl_->iniciada = true;
  Impl* im = impl_.get();
  im->espera = std::thread([im] { im->ligar(); });
  im->tally = std::thread([im] { im->lacoTally(); });
}

bool SessaoFonte::parar(int timeoutMs) {
  auto im = impl_;
  if (im->parando.exchange(true)) return true;
  {
    std::lock_guard<std::mutex> l(im->mCb);  // quem esta dentro de um callback termina antes
    im->aoQuadro = nullptr;
    im->aoEstado = nullptr;
  }
  std::string k = im->chave();
  if (!k.empty()) im->reg->liberar(k, im.get());
  im->soltarReservas();
  if (!im->iniciada) return true;

  struct Fim {
    std::mutex m;
    std::condition_variable cv;
    bool pronto = false;
  };
  auto fim = std::make_shared<Fim>();
  ++g_encerrando;
  std::thread([im, fim] {
    im->pararBloqueante();
    {
      std::lock_guard<std::mutex> l(fim->m);
      fim->pronto = true;
    }
    fim->cv.notify_all();
    --g_encerrando;
  }).detach();
  std::unique_lock<std::mutex> l(fim->m);
  return fim->cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return fim->pronto; });
}

EstadoSessao SessaoFonte::estado() const {
  std::lock_guard<std::mutex> l(impl_->mEst);
  return impl_->est;
}

const ConfigFonte& SessaoFonte::config() const { return impl_->cfg; }

}  // namespace fmcam
