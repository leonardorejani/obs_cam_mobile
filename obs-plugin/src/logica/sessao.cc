#include "sessao.h"

#include <atomic>
#include <cstdio>
#include <chrono>
#include <condition_variable>
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
bool aguardarEncerramentos(int ms) {
  for (int i = 0; i < ms / 10 && g_encerrando > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return g_encerrando == 0;
}

struct SessaoFonte::Impl {
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

  void publicarStatus() {
    StatusIphone s;
    {
      std::lock_guard<std::mutex> l(mEst);
      s = ultimoStatus;
    }
    std::string k = chave();
    if (!k.empty() && !parando) reg->definirStatus(k, s);
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
    std::string k = chave();
    if (!k.empty() && !parando) reg->definirEstado(k, e, msg, pelaRede);
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
        if (!novo->abrir(erro)) {
          setEstado(EstadoAparelho::Falha, "decodificador: " + erro);
          return;
        }
        dec = std::move(novo);
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
          reg->reservar(udid, this);
          reg->definirIdentidade(udid, r.nome.empty() ? r.aparelho : r.nome, r.aparelho, r.id, r.w, r.h);
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
      auto r = dec->decodificar(p.dados.data() + 1, p.dados.size() - 1);
      if (r == Decodificador::Resultado::Erro) {
        std::lock_guard<std::mutex> l(mEst);
        est.errosDecodificacao = dec->erros();
        if (con) con->enviarComando("{\"cmd\":\"keyframe\"}");
        return;
      }
      if (r != Decodificador::Resultado::Quadro) return;
      QuadroSaida q;
      std::string erro;
      if (!converterQuadro(dec->quadro(), q, erro)) return;
      {
        std::lock_guard<std::mutex> l(mCb);
        if (aoQuadro && !parando) aoQuadro(q);
      }
      int64_t atraso = 0;
      bool temAtraso = con && con->atrasoUs(chegadaUs, p.tempo, atraso);
      {
        std::lock_guard<std::mutex> l(mEst);
        ++est.quadros;
        est.jaTeveQuadro = true;
        est.largura = q.largura;
        est.altura = q.altura;
        est.respLargura = q.largura;  // o que o iPhone entrega agora (ele pode trocar a qualidade no meio da conexao)
        est.respAltura = q.altura;
        if (temAtraso) est.atrasoCanoMs = atraso / 1000.0;
      }
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

  // iPhone escolhido na lista do Wi-Fi: o IP vem do Bonjour e acompanha se o iPhone mudar de IP.
  void ligarWifiPorId() {
    const std::string k = "wifi:" + cfg.wifiId;
    bool avisou = false;
    while (!parando && !reg->reservar(k, this)) {
      if (!avisou) {
        setEstado(EstadoAparelho::Ocupado, "ocupado: outra fonte do OBS ja usa este iPhone");
        avisou = true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
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
        auto op = montarOpcoes(c, usbmux);
        auto nova = std::make_unique<Conexao>(op);
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
    // Um iPhone por fonte dentro do processo: espera a outra fonte soltar em vez de brigar por ele.
    if (!cfg.wifi && !cfg.udid.empty()) {
      bool avisou = false;
      while (!parando && !reg->reservar(cfg.udid, this)) {
        if (!avisou) {
          setEstado(EstadoAparelho::Ocupado, "ocupado: outra fonte do OBS ja usa este iPhone");
          avisou = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    } else if (cfg.wifi) {
      reg->reservar("wifi:" + cfg.wifiIp, this);
    }
    if (parando) return;
    auto nova = std::make_unique<Conexao>(montarOpcoes(cfg, usbmux));
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
    if (espera.joinable()) espera.join();
    if (con) con->parar();
    con.reset();
  }
};

SessaoFonte::SessaoFonte(ConfigFonte cfg, std::string caminho, RegistroAparelhos* reg) : impl_(std::make_shared<Impl>()) {
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
  if (im->cfg.wifi) im->reg->liberar("wifi:" + im->cfg.wifiIp, im.get());
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
