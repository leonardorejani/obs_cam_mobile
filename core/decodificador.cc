#include "decodificador.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

#include <cstring>

namespace fmappcam {

Decodificador::Decodificador() = default;

Decodificador::~Decodificador() {
  av_packet_free(&pacote_);
  av_frame_free(&quadro_);
  avcodec_free_context(&ctx_);
}

bool Decodificador::abrir(std::string& erro) {
  const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!c) {
    erro = "decodificador H.264 indisponivel";
    return false;
  }
  ctx_ = avcodec_alloc_context3(c);
  if (!ctx_) {
    erro = "sem memoria";
    return false;
  }
  ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
  ctx_->thread_count = 1;  // threads de quadro somam quadros de atraso
  ctx_->has_b_frames = 0;
  if (avcodec_open2(ctx_, c, nullptr) < 0) {
    erro = "avcodec_open2 falhou";
    return false;
  }
  quadro_ = av_frame_alloc();
  pacote_ = av_packet_alloc();
  return quadro_ && pacote_;
}

void Decodificador::definirConfig(const uint8_t* d, size_t n) {
  config_.assign(d, d + n);
  configPendente_ = !config_.empty();
}

Decodificador::Resultado Decodificador::decodificar(const uint8_t* dados, size_t n) {
  if (!ctx_) return Resultado::Erro;
  const size_t extra = configPendente_ ? config_.size() : 0;
  if (av_new_packet(pacote_, static_cast<int>(extra + n)) < 0) return Resultado::Erro;
  if (extra) std::memcpy(pacote_->data, config_.data(), extra);
  std::memcpy(pacote_->data + extra, dados, n);
  configPendente_ = false;
  int rc = avcodec_send_packet(ctx_, pacote_);
  av_packet_unref(pacote_);
  if (rc < 0 && rc != AVERROR(EAGAIN)) {
    ++erros_;
    return Resultado::Erro;
  }
  av_frame_unref(quadro_);
  rc = avcodec_receive_frame(ctx_, quadro_);
  if (rc == 0) {
    ++quadros_;
    return Resultado::Quadro;
  }
  if (rc == AVERROR(EAGAIN)) return Resultado::SemQuadro;
  ++erros_;
  return Resultado::Erro;
}

}  // namespace fmappcam
