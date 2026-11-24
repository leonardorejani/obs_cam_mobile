// visor: cliente de linha de comando do OBS Cam Mobile. Conecta (TCP ou cabo), decodifica e mede, sem janela.
// Varios aparelhos ao mesmo tempo (--todos ou varios --tcp), cada um com a propria Conexao e decodificador.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "conexao.h"
#include "decodificador.h"
#include "usbmux.h"

using namespace fmappcam;

namespace {

struct Config {
  std::vector<std::string> tcp;  // host:porta
  bool usb = false, listar = false, todos = false;
  std::vector<std::string> udids;
  std::string usbmuxSocket;
  int segundos = 10;
  std::string pngDir;
  int pngCada = 0;
  bool permitirPerdas = false, permitirErros = false, permitirQuedas = false;
  std::string preset = "1080p30";  // "iphone" = nao manda (vale a qualidade salva no iPhone)
  int64_t bitrate = 10000000;      // 0 = nao manda
};

void criarPasta(const std::string& d) {
#ifdef _WIN32
  _mkdir(d.c_str());
#else
  mkdir(d.c_str(), 0755);
#endif
}

std::string limpar(const std::string& s) {
  std::string o;
  for (char c : s) o.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ? c : '_');
  return o;
}

double percentil(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t i = static_cast<size_t>(std::ceil(p * static_cast<double>(v.size()))) ;
  if (i > 0) --i;
  return v[std::min(i, v.size() - 1)];
}

bool gravarPng(const AVFrame* f, const std::string& caminho) {
  SwsContext* sws = sws_getContext(f->width, f->height, static_cast<AVPixelFormat>(f->format), f->width, f->height,
                                   AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!sws) return false;
  AVFrame* rgb = av_frame_alloc();
  rgb->format = AV_PIX_FMT_RGB24;
  rgb->width = f->width;
  rgb->height = f->height;
  bool ok = av_frame_get_buffer(rgb, 0) >= 0;
  if (ok) sws_scale(sws, f->data, f->linesize, 0, f->height, rgb->data, rgb->linesize);
  sws_freeContext(sws);
  if (ok) {
    const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_PNG);
    AVCodecContext* e = c ? avcodec_alloc_context3(c) : nullptr;
    ok = e != nullptr;
    if (ok) {
      e->pix_fmt = AV_PIX_FMT_RGB24;
      e->width = f->width;
      e->height = f->height;
      e->time_base = AVRational{1, 1};
      e->compression_level = 1;  // rapido: e so prova
      ok = avcodec_open2(e, c, nullptr) >= 0;
    }
    if (ok) {
      AVPacket* pk = av_packet_alloc();
      ok = avcodec_send_frame(e, rgb) >= 0 && avcodec_receive_packet(e, pk) >= 0;
      if (ok) {
        FILE* fp = std::fopen(caminho.c_str(), "wb");
        ok = fp != nullptr;
        if (fp) {
          ok = std::fwrite(pk->data, 1, static_cast<size_t>(pk->size), fp) == static_cast<size_t>(pk->size);
          std::fclose(fp);
        }
      }
      av_packet_free(&pk);
    }
    avcodec_free_context(&e);
  }
  av_frame_free(&rgb);
  return ok;
}

struct Janela {
  uint64_t quadros = 0;
  std::vector<double> atrasos;  // ms, captura ate a chegada
  std::vector<double> entregas;  // ms, captura ate o quadro decodificado (o que a fonte do OBS entregaria)
  double decodSoma = 0, decodMax = 0;
  void zerar() { *this = Janela(); }
};

// Tudo de um aparelho: conexao, decodificador, medidas e gravador de PNG.
class Alvo {
 public:
  Alvo(std::string rotulo, OpcoesConexao op, const Config& cfg) : rotulo_(std::move(rotulo)), cfg_(cfg), con_(std::move(op)) {
    std::string erro;
    if (!dec_.abrir(erro)) std::fprintf(stderr, "[%s] decodificador: %s\n", rotulo_.c_str(), erro.c_str());
    if (!cfg_.pngDir.empty()) {
      pasta_ = cfg_.pngDir;
      criarPasta(pasta_);
    }
    con_.definirCallbacks([this](const Pacote& p, uint64_t t) { aoPacote(p, t); },
                          [this](EstadoConexao e, const std::string& m) { aoEstado(e, m); });
  }
  ~Alvo() { parar(); }

  void definirPasta(const std::string& p) {
    pasta_ = p;
    criarPasta(pasta_);
  }
  void iniciar() {
    if (!pasta_.empty()) png_ = std::thread([this] { lacoPng(); });
    con_.iniciar();
  }
  void parar() {
    con_.parar();
    {
      std::lock_guard<std::mutex> l(mPng_);
      pararPng_ = true;
    }
    cvPng_.notify_all();
    if (png_.joinable()) png_.join();
    av_frame_free(&pendente_);
  }

  const std::string& rotulo() const { return rotulo_; }

  Janela pegarJanela() {
    std::lock_guard<std::mutex> l(m_);
    Janela j = std::move(janela_);
    janela_.zerar();
    return j;
  }
  struct Total {
    uint64_t quadros = 0, perdidos = 0, errosDec = 0, pngs = 0;
    double fps = 0, p50 = 0, p95 = 0, max = 0, decodMedio = 0, decodMax = 0, entregaP50 = 0, entregaP95 = 0;
    int w = 0, h = 0;
    std::string nome, id, aparelho;
    int reconexoes = 0;
    size_t amostrasAtraso = 0;
  };
  Total total() {
    std::lock_guard<std::mutex> l(m_);
    Total t;
    t.quadros = quadros_;
    t.perdidos = perdidos_;
    t.errosDec = errosDec_;
    t.pngs = pngs_;
    if (quadros_ > 1 && ultimaChegada_ > primeiraChegada_) t.fps = (quadros_ - 1) * 1e6 / double(ultimaChegada_ - primeiraChegada_);
    t.p50 = percentil(atrasos_, 0.5);
    t.p95 = percentil(atrasos_, 0.95);
    t.max = atrasos_.empty() ? 0 : *std::max_element(atrasos_.begin(), atrasos_.end());
    t.amostrasAtraso = atrasos_.size();
    t.entregaP50 = percentil(entregas_, 0.5);
    t.entregaP95 = percentil(entregas_, 0.95);
    t.decodMedio = quadros_ ? decodSoma_ / double(quadros_) : 0;
    t.decodMax = decodMax_;
    auto r = con_.resposta();
    t.w = r.w;
    t.h = r.h;
    t.nome = r.nome;
    t.id = r.id;
    t.aparelho = r.aparelho;
    t.reconexoes = con_.reconexoes();
    return t;
  }
  std::string ultimoStatus() {
    std::lock_guard<std::mutex> l(m_);
    return status_;
  }
  std::string ultimoEstado() {
    std::lock_guard<std::mutex> l(m_);
    return estado_;
  }

 private:
  void aoEstado(EstadoConexao e, const std::string& msg) {
    std::lock_guard<std::mutex> l(m_);
    temAnterior_ = false;  // depois de reconectar o tempo recomeca
    switch (e) {
      case EstadoConexao::Conectando: estado_ = "conectando"; break;
      case EstadoConexao::Conectado: estado_ = "conectado " + msg; break;
      case EstadoConexao::Desconectado: estado_ = "desconectado " + msg; break;
      case EstadoConexao::ErroFatal: estado_ = "erro " + msg; break;
    }
    std::fprintf(stderr, "[%s] %s\n", rotulo_.c_str(), estado_.c_str());
  }

  void aoPacote(const Pacote& p, uint64_t chegada) {
    if (p.tipo == kTipoConfig) {
      dec_.definirConfig(p.dados.data(), p.dados.size());
      return;
    }
    if (p.tipo == kTipoStatus) {
      std::lock_guard<std::mutex> l(m_);
      status_.assign(p.dados.begin(), p.dados.end());
      return;
    }
    if (p.tipo != kTipoQuadro || p.dados.empty()) return;

    double fps = con_.resposta().fps;
    if (fps <= 0) fps = 30;
    const double intervalo = 1e6 / fps;
    bool pedirChave = false;
    uint64_t novosPerdidos = 0;
    {
      std::lock_guard<std::mutex> l(m_);
      if (temAnterior_) {
        double gap = double(int64_t(p.tempo) - int64_t(tempoAnterior_));
        if (gap > 1.5 * intervalo) {
          novosPerdidos = std::max<uint64_t>(1, static_cast<uint64_t>(std::llround(gap / intervalo)) - 1);
          perdidos_ += novosPerdidos;
          pedirChave = true;
        }
      }
      temAnterior_ = true;
      tempoAnterior_ = p.tempo;
    }

    const uint64_t d0 = agoraMonotonicoUs();
    auto r = dec_.decodificar(p.dados.data() + 1, p.dados.size() - 1, (p.dados[0] & 1) != 0);
    if (dec_.usaHardware() && !avisouHw_) {
      avisouHw_ = true;
      std::printf("[%s] decodificador: hardware\n", rotulo_.c_str());
    }
    if (dec_.aguardaChaveParaHardware() && !pediuChaveHw_) {
      pediuChaveHw_ = true;
      con_.enviarComando("{\"cmd\":\"keyframe\"}");
    }
    const double decMs = double(agoraMonotonicoUs() - d0) / 1000.0;

    int64_t atrasoUs = 0, entregaUs = 0;
    bool temAtraso = con_.atrasoUs(chegada, p.tempo, atrasoUs);
    bool temEntrega = con_.atrasoUs(agoraMonotonicoUs(), p.tempo, entregaUs);
    AVFrame* clone = nullptr;
    {
      std::lock_guard<std::mutex> l(m_);
      if (r == Decodificador::Resultado::Erro) {
        ++errosDec_;
        pedirChave = true;
      } else if (r == Decodificador::Resultado::Quadro) {
        ++quadros_;
        ++janela_.quadros;
        if (quadros_ == 1) primeiraChegada_ = chegada;
        ultimaChegada_ = chegada;
        decodSoma_ += decMs;
        decodMax_ = std::max(decodMax_, decMs);
        janela_.decodSoma += decMs;
        janela_.decodMax = std::max(janela_.decodMax, decMs);
        if (temAtraso) {
          atrasos_.push_back(double(atrasoUs) / 1000.0);
          janela_.atrasos.push_back(double(atrasoUs) / 1000.0);
        }
        if (temEntrega) {
          entregas_.push_back(double(entregaUs) / 1000.0);
          janela_.entregas.push_back(double(entregaUs) / 1000.0);
        }
        if (!pasta_.empty() && cfg_.pngCada > 0 && (quadros_ - 1) % static_cast<uint64_t>(cfg_.pngCada) == 0)
          clone = av_frame_clone(dec_.quadro());
      }
    }
    if (clone) {
      std::lock_guard<std::mutex> l(mPng_);
      if (pendente_) {
        av_frame_free(&clone);  // gravador ocupado: pula este
      } else {
        pendente_ = clone;
        numeroPng_ = quadros_;
      }
      cvPng_.notify_one();
    }
    if (pedirChave) {
      uint64_t agora = agoraMonotonicoUs();
      if (agora - ultimoPedidoChave_ > 1000000) {
        ultimoPedidoChave_ = agora;
        con_.enviarComando("{\"cmd\":\"keyframe\"}");
      }
    }
  }

  void lacoPng() {
    for (;;) {
      AVFrame* f = nullptr;
      uint64_t numero = 0;
      {
        std::unique_lock<std::mutex> l(mPng_);
        cvPng_.wait(l, [&] { return pendente_ || pararPng_; });
        if (!pendente_) return;
        f = pendente_;
        pendente_ = nullptr;
        numero = numeroPng_;
      }
      char nome[64];
      std::snprintf(nome, sizeof nome, "/quadro_%06llu.png", static_cast<unsigned long long>(numero));
      if (gravarPng(f, pasta_ + nome)) {
        std::lock_guard<std::mutex> l(m_);
        ++pngs_;
      }
      av_frame_free(&f);
    }
  }

  std::string rotulo_;
  Config cfg_;
  Conexao con_;
  Decodificador dec_;
  bool pediuChaveHw_ = false;
  bool avisouHw_ = false;
  std::string pasta_;

  std::mutex m_;
  Janela janela_;
  std::vector<double> atrasos_, entregas_;
  uint64_t quadros_ = 0, perdidos_ = 0, errosDec_ = 0, pngs_ = 0;
  uint64_t primeiraChegada_ = 0, ultimaChegada_ = 0;
  double decodSoma_ = 0, decodMax_ = 0;
  bool temAnterior_ = false;
  uint64_t tempoAnterior_ = 0;
  uint64_t ultimoPedidoChave_ = 0;
  std::string status_, estado_;

  std::thread png_;
  std::mutex mPng_;
  std::condition_variable cvPng_;
  AVFrame* pendente_ = nullptr;
  uint64_t numeroPng_ = 0;
  bool pararPng_ = false;
};

void uso() {
  std::fputs(
      "uso: visor (--tcp host:porta [--tcp ...] | --usb [--udid X ...] [--todos] [--listar]) [--usbmux-socket CAMINHO]\n"
      "       [--segundos S] [--png-dir DIR --png-cada N] [--permitir-perdas] [--permitir-erros] [--permitir-quedas]\n"
      "       [--preset 1080p30|2160p30|...|iphone] [--bitrate KBPS|0]\n"
      "  --usb sozinho lista os aparelhos do cabo e sai. --todos conecta em todos ao mesmo tempo.\n",
      stderr);
}

std::string jsonTexto(const std::string& s) { return "\"" + escaparJson(s) + "\""; }

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) {
        uso();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--tcp") cfg.tcp.push_back(val());
    else if (a == "--usb") cfg.usb = true;
    else if (a == "--listar") cfg.listar = true;
    else if (a == "--todos") cfg.todos = true;
    else if (a == "--udid") cfg.udids.push_back(val());
    else if (a == "--usbmux-socket") cfg.usbmuxSocket = val();
    else if (a == "--segundos") cfg.segundos = std::atoi(val().c_str());
    else if (a == "--png-dir") cfg.pngDir = val();
    else if (a == "--png-cada") cfg.pngCada = std::atoi(val().c_str());
    else if (a == "--permitir-perdas") cfg.permitirPerdas = true;
    else if (a == "--permitir-erros") cfg.permitirErros = true;
    else if (a == "--permitir-quedas") cfg.permitirQuedas = true;
    else if (a == "--preset") cfg.preset = val();
    else if (a == "--bitrate") cfg.bitrate = std::atoll(val().c_str()) * 1000;
    else { uso(); return 2; }
  }
  if (cfg.tcp.empty() && !cfg.usb && !cfg.listar) { uso(); return 2; }
  if (!cfg.pngDir.empty() && cfg.pngCada <= 0) cfg.pngCada = 30;
  iniciarRede();
  const std::string caminhoUsb = cfg.usbmuxSocket.empty() ? caminhoUsbmuxPadrao() : cfg.usbmuxSocket;

  // Sem aparelho escolhido, --usb so lista.
  if (cfg.listar || (cfg.usb && !cfg.todos && cfg.udids.empty() && cfg.tcp.empty())) {
    std::vector<DispositivoUsb> lista;
    std::string erro;
    if (!listarDispositivosUsb(caminhoUsb, lista, erro)) {
      std::fprintf(stderr, "erro: %s\n", erro.c_str());
      return 3;
    }
    std::printf("%zu aparelho(s) no cabo\n", lista.size());
    for (auto& d : lista) std::printf("  id=%d udid=%s\n", d.id, d.serial.c_str());
    return 0;
  }

  std::vector<std::unique_ptr<Alvo>> alvos;
  std::map<std::string, bool> jaTem;
  const bool varios = cfg.tcp.size() + cfg.udids.size() > 1 || cfg.todos;
  auto novoAlvo = [&](const std::string& rotulo, OpcoesConexao op) {
    op.pedido.cliente = "fmcam-visor";
    op.pedido.preset = cfg.preset == "iphone" ? "" : cfg.preset;
    op.pedido.bitrate = cfg.bitrate;
    auto a = std::make_unique<Alvo>(rotulo, std::move(op), cfg);
    if (!cfg.pngDir.empty() && varios) a->definirPasta(cfg.pngDir + "/" + limpar(rotulo));
    a->iniciar();
    alvos.push_back(std::move(a));
  };

  for (auto& t : cfg.tcp) {
    size_t dp = t.rfind(':');
    OpcoesConexao op;
    op.host = dp == std::string::npos ? t : t.substr(0, dp);
    op.porta = dp == std::string::npos ? kPortaPadrao : static_cast<uint16_t>(std::atoi(t.c_str() + dp + 1));
    novoAlvo(t, op);
  }
  auto opUsb = [&](const std::string& udid) {
    OpcoesConexao op;
    op.usb = true;
    op.usbmuxCaminho = caminhoUsb;
    op.udid = udid;
    return op;
  };
  for (auto& u : cfg.udids) {
    jaTem[u] = true;
    novoAlvo(u, opUsb(u));
  }
  ObservadorUsbmux obs(caminhoUsb);
  if (cfg.todos) {
    obs.iniciar();
    for (int i = 0; i < 50 && !obs.ouvindo(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // deixa chegarem os Attached iniciais
  }
  auto sincronizarTodos = [&] {
    if (!cfg.todos) return;
    for (auto& d : obs.dispositivos()) {
      if (jaTem[d.serial]) continue;
      jaTem[d.serial] = true;
      novoAlvo(d.serial, opUsb(d.serial));
    }
  };
  sincronizarTodos();
  if (alvos.empty() && !cfg.todos) return 3;

  using Rel = std::chrono::steady_clock;
  const auto inicio = Rel::now();
  auto proximo = inicio + std::chrono::seconds(1);
  // Segundo sem nenhum quadro depois que o vídeo começou = tempo sem imagem no OBS.
  // Queda não aparece em "perdidos" (o buraco é entre conexões), por isso conta à parte.
  std::vector<int> semVideo(alvos.size(), 0);
  std::vector<bool> comecou(alvos.size(), false);
  for (int s = 1; s <= cfg.segundos; ++s) {
    std::this_thread::sleep_until(proximo);
    proximo += std::chrono::seconds(1);
    sincronizarTodos();
    for (size_t i = 0; i < alvos.size(); ++i) {
      auto& a = alvos[i];
      Janela j = a->pegarJanela();
      if (j.quadros > 0) comecou[i] = true;
      else if (comecou[i] && s < cfg.segundos) ++semVideo[i];
      auto t = a->total();
      std::printf("[%s] t=%ds fps=%.1f perdidos=%llu erros_dec=%llu atraso_ms p50=%.1f p95=%.1f max=%.1f decod_ms=%.2f "
                  "entrega_ms p50=%.1f\n",
                  a->rotulo().c_str(), s, double(j.quadros), (unsigned long long)t.perdidos,
                  (unsigned long long)t.errosDec, percentil(j.atrasos, 0.5), percentil(j.atrasos, 0.95),
                  j.atrasos.empty() ? 0.0 : *std::max_element(j.atrasos.begin(), j.atrasos.end()),
                  j.quadros ? j.decodSoma / double(j.quadros) : 0.0, percentil(j.entregas, 0.5));
    }
    std::fflush(stdout);
  }
  obs.parar();
  for (auto& a : alvos) a->parar();

  int saida = 0;
  for (size_t i = 0; i < alvos.size(); ++i) {
    auto& a = alvos[i];
    auto t = a->total();
    std::printf(
        "RESUMO {\"aparelho\":%s,\"nome\":%s,\"id\":%s,\"modelo\":%s,\"segundos\":%d,\"quadros\":%llu,\"fps\":%.2f,"
        "\"perdidos\":%llu,\"erros_decodificacao\":%llu,\"atraso_ms_p50\":%.2f,\"atraso_ms_p95\":%.2f,"
        "\"atraso_ms_max\":%.2f,\"amostras_atraso\":%zu,\"decod_ms_medio\":%.2f,\"decod_ms_max\":%.2f,"
        "\"entrega_ms_p50\":%.2f,\"entrega_ms_p95\":%.2f,\"pngs\":%llu,\"reconexoes\":%d,\"segundos_sem_video\":%d,\"largura\":%d,\"altura\":%d}\n",
        jsonTexto(a->rotulo()).c_str(), jsonTexto(t.nome).c_str(), jsonTexto(t.id).c_str(), jsonTexto(t.aparelho).c_str(),
        cfg.segundos, (unsigned long long)t.quadros, t.fps, (unsigned long long)t.perdidos,
        (unsigned long long)t.errosDec, t.p50, t.p95, t.max, t.amostrasAtraso, t.decodMedio, t.decodMax,
        t.entregaP50, t.entregaP95, (unsigned long long)t.pngs, t.reconexoes, semVideo[i], t.w, t.h);
    if (t.quadros == 0) saida = std::max(saida, 3);
    else if ((t.perdidos > 0 && !cfg.permitirPerdas) || (t.errosDec > 0 && !cfg.permitirErros) ||
             ((t.reconexoes > 0 || semVideo[i] > 0) && !cfg.permitirQuedas))
      saida = std::max(saida, 1);
  }
  std::fflush(stdout);
  return saida;
}
