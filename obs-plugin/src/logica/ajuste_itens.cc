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

}  // namespace fmcam
