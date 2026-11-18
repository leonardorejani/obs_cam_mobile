#include "quadro.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace fmcam {

bool converterQuadro(const AVFrame* f, QuadroSaida& q, std::string& erro) {
  if (!f || f->width <= 0 || f->height <= 0) {
    erro = "quadro vazio";
    return false;
  }
  q = QuadroSaida{};
  q.largura = f->width;
  q.altura = f->height;
  switch (f->format) {
    case AV_PIX_FMT_NV12:
      q.formato = QuadroSaida::Formato::NV12;
      q.plano[0] = f->data[0];
      q.plano[1] = f->data[1];
      q.passo[0] = f->linesize[0];
      q.passo[1] = f->linesize[1];
      break;
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
      q.formato = QuadroSaida::Formato::I420;
      for (int i = 0; i < 3; ++i) {
        q.plano[i] = f->data[i];
        q.passo[i] = f->linesize[i];
      }
      break;
    default:
      erro = "formato de pixel nao suportado: " + std::to_string(f->format);
      return false;
  }
  for (int i = 0; i < 3; ++i)
    if (q.formato == QuadroSaida::Formato::I420 || i < 2)
      if (!q.plano[i] || q.passo[i] <= 0) {
        erro = "quadro sem planos";
        return false;
      }
  q.faixaCompleta = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
  switch (f->colorspace) {
    case AVCOL_SPC_BT709: q.matriz = QuadroSaida::Matriz::BT709; break;
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_BT470BG: q.matriz = QuadroSaida::Matriz::BT601; break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: q.matriz = QuadroSaida::Matriz::BT2020; break;
    default:  // sem marca no fluxo: HD usa 709
      q.matriz = f->height >= 720 ? QuadroSaida::Matriz::BT709 : QuadroSaida::Matriz::BT601;
  }
  return true;
}

}  // namespace fmcam
