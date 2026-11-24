#include "decodificador.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace fmappcam {

namespace {
std::atomic<int> g_atrasoTesteUs{0};

// Acima de 1080p o software de uma thread so passa a ser o gargalo (e atrasa tudo: o iPhone enfileira ate 1 s de video).
constexpr int kPixelsParaHardware = 1920 * 1080;

AVHWDeviceType tipoDeHardware(int tentativa) {
#if defined(__APPLE__)
  return tentativa == 0 ? AV_HWDEVICE_TYPE_VIDEOTOOLBOX : AV_HWDEVICE_TYPE_NONE;
#elif defined(_WIN32)
  return tentativa == 0 ? AV_HWDEVICE_TYPE_D3D11VA : tentativa == 1 ? AV_HWDEVICE_TYPE_DXVA2 : AV_HWDEVICE_TYPE_NONE;
#else
  (void)tentativa;
  return AV_HWDEVICE_TYPE_NONE;
#endif
}

AVPixelFormat escolherFormato(AVCodecContext* c, const AVPixelFormat* fmts) {
  const int alvo = *static_cast<int*>(c->opaque);
  for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
    if (static_cast<int>(*p) == alvo) return *p;
  for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
    if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;  // sem hardware nesta hora: software
  }
  return AV_PIX_FMT_NONE;
}
}  // namespace

void Decodificador::definirAtrasoDeTesteUs(int us) { g_atrasoTesteUs = us; }

Decodificador::Decodificador() {
  if (const char* e = std::getenv("FMCAM_HW")) {
    if (e[0] == '0') modoHw_ = 0;
    else if (e[0] == '1') modoHw_ = 1;
    modoPorAmbiente_ = modoHw_ != -1;
  }
}

void Decodificador::definirModoHardware(int modo) {
  if (!modoPorAmbiente_) modoHw_ = modo < 0 ? -1 : modo > 0 ? 1 : 0;
}

Decodificador::~Decodificador() {
  av_packet_free(&pacote_);
  av_frame_free(&quadro_);
  av_frame_free(&sw_);
  avcodec_free_context(&ctx_);
  av_buffer_unref(&hwDev_);
}

bool Decodificador::abrirContexto(bool hardware, std::string& erro) {
  const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!c) {
    erro = "decodificador H.264 indisponivel";
    return false;
  }
  AVCodecContext* novo = avcodec_alloc_context3(c);
  if (!novo) {
    erro = "sem memoria";
    return false;
  }
  novo->flags |= AV_CODEC_FLAG_LOW_DELAY;
  novo->thread_count = 1;  // threads de quadro somam quadros de atraso
  novo->has_b_frames = 0;
  if (hardware) {
    // acha um dispositivo de hardware que o H.264 do FFmpeg sabe usar nesta maquina
    AVBufferRef* dev = nullptr;
    int fmt = -1;
    for (int t = 0; fmt < 0; ++t) {
      AVHWDeviceType tipo = tipoDeHardware(t);
      if (tipo == AV_HWDEVICE_TYPE_NONE) break;
      for (int i = 0;; ++i) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(c, i);
        if (!cfg) break;
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == tipo) {
          if (av_hwdevice_ctx_create(&dev, tipo, nullptr, nullptr, 0) >= 0) fmt = static_cast<int>(cfg->pix_fmt);
          break;
        }
      }
    }
    if (fmt < 0) {
      avcodec_free_context(&novo);
      erro = "sem decodificador de hardware";
      return false;
    }
    hwFmt_ = fmt;
    novo->opaque = &hwFmt_;
    novo->get_format = escolherFormato;
    novo->hw_device_ctx = av_buffer_ref(dev);
    av_buffer_unref(&hwDev_);
    hwDev_ = dev;
  }
  if (avcodec_open2(novo, c, nullptr) < 0) {
    avcodec_free_context(&novo);
    erro = "avcodec_open2 falhou";
    return false;
  }
  avcodec_free_context(&ctx_);
  ctx_ = novo;
  hwAtivo_ = hardware;
  return true;
}

bool Decodificador::abrir(std::string& erro) {
  if (!abrirContexto(false, erro)) return false;
  quadro_ = av_frame_alloc();
  sw_ = av_frame_alloc();
  pacote_ = av_packet_alloc();
  if (!quadro_ || !sw_ || !pacote_) return false;
  if (modoHw_ == 1) hwPendente_ = true;
  return true;
}

void Decodificador::definirConfig(const uint8_t* d, size_t n) {
  config_.assign(d, d + n);
  configPendente_ = !config_.empty();
}

void Decodificador::voltarParaSoftware() {
  std::string e;
  hwFalhou_ = true;
  hwPendente_ = false;
  if (abrirContexto(false, e)) {
    hwAtivo_ = false;
    configPendente_ = !config_.empty();  // o software recomeca no proximo quadro-chave
  }
}

// Quadro do hardware (VideoToolbox, D3D11) para memoria de sistema, em NV12.
bool Decodificador::transferir() {
  av_frame_unref(sw_);
  if (av_hwframe_transfer_data(sw_, quadro_, 0) < 0) return false;
  av_frame_copy_props(sw_, quadro_);
  saida_ = sw_;
  return true;
}

Decodificador::Resultado Decodificador::decodificar(const uint8_t* dados, size_t n, bool chave) {
  if (!ctx_) return Resultado::Erro;
  if (hwPendente_ && chave && !hwAtivo_ && !hwFalhou_ && modoHw_ != 0) {
    std::string e;
    hwPendente_ = false;
    if (abrirContexto(true, e)) configPendente_ = !config_.empty();  // o quadro-chave leva SPS/PPS e recomeca limpo
    else hwFalhou_ = true;
  }
  const size_t extra = configPendente_ ? config_.size() : 0;
  if (av_new_packet(pacote_, static_cast<int>(extra + n)) < 0) return Resultado::Erro;
  if (extra) std::memcpy(pacote_->data, config_.data(), extra);
  std::memcpy(pacote_->data + extra, dados, n);
  configPendente_ = false;
  if (int us = g_atrasoTesteUs.load()) std::this_thread::sleep_for(std::chrono::microseconds(us));
  saida_ = nullptr;
  int rc = avcodec_send_packet(ctx_, pacote_);
  av_packet_unref(pacote_);
  if (rc < 0 && rc != AVERROR(EAGAIN)) {
    ++erros_;
    if (hwAtivo_) voltarParaSoftware();
    return Resultado::Erro;
  }
  av_frame_unref(quadro_);
  rc = avcodec_receive_frame(ctx_, quadro_);
  if (rc == 0) {
    if (hwAtivo_ && static_cast<int>(quadro_->format) == hwFmt_) {
      if (!transferir()) {
        ++erros_;
        voltarParaSoftware();
        return Resultado::Erro;
      }
    }
    ++quadros_;
    if (!hwAtivo_ && !hwFalhou_ && modoHw_ != 0 && quadro_->width * quadro_->height > kPixelsParaHardware) hwPendente_ = true;
    return Resultado::Quadro;
  }
  if (rc == AVERROR(EAGAIN)) return Resultado::SemQuadro;
  ++erros_;
  if (hwAtivo_) voltarParaSoftware();
  return Resultado::Erro;
}

}  // namespace fmappcam
