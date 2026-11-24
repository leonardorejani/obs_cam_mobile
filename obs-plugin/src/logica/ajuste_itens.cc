#include "ajuste_itens.h"

#include <cmath>

namespace fmcam {

AjusteBounds decidirAjuste(const TransformacaoItem& t, int lw, int lh, int cw, int ch) {
  AjusteBounds r;
  if (cw <= 0 || ch <= 0 || lw <= 0 || lh <= 0) return r;
  if (t.temBounds || t.recortado || !t.alinhadoTopoEsquerda || std::fabs(t.rotacao) > 0.01) return r;
  constexpr double tol = 1.5;  // pixels
  bool enche = std::fabs(t.posX) < tol && std::fabs(t.posY) < tol && std::fabs(t.escalaX * lw - cw) < tol &&
               std::fabs(t.escalaY * lh - ch) < tol;
  if (!enche) return r;
  r.aplicar = true;
  r.posX = cw / 2.0;
  r.posY = ch / 2.0;
  r.boundsLargura = cw;
  r.boundsAltura = ch;
  return r;
}

AjusteBounds decidirEncaixe(const TransformacaoItem& t, int lw, int lh) {
  AjusteBounds r;
  if (lw <= 0 || lh <= 0 || t.temBounds || t.recortado || t.escalaX <= 0 || t.escalaY <= 0) return r;
  r.aplicar = true;
  r.posX = t.posX;
  r.posY = t.posY;
  r.boundsLargura = t.escalaX * lw;
  r.boundsAltura = t.escalaY * lh;
  return r;
}

}  // namespace fmcam
