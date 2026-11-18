// Quando a resolucao da fonte muda (troca de predefinicao, 1080p para 720p), um item de cena que enchia o canvas
// sem bounds fica pequeno no canto. Esta regra decide, sem libobs, se o item enchia o canvas e deve passar a
// ter bounds do tamanho do canvas (SCALE_INNER, centralizado), de modo que trocar de resolucao nao mude a tela.
// Item que o usuario redimensionou ou posicionou (ex.: picture-in-picture) nunca e tocado.
#pragma once

namespace fmcam {

struct TransformacaoItem {
  double posX = 0, posY = 0;
  double escalaX = 1, escalaY = 1;
  double rotacao = 0;
  bool temBounds = false;
  bool alinhadoTopoEsquerda = true;  // o padrao de item novo no OBS
  bool recortado = false;
};

struct AjusteBounds {
  bool aplicar = false;
  double posX = 0, posY = 0;          // centro do canvas (alinhamento central)
  double boundsLargura = 0, boundsAltura = 0;  // tamanho do canvas
};

// larguraAntes/alturaAntes: tamanho com que o item foi visto enchendo o canvas (o do quadro anterior; no primeiro
// quadro, o do proprio quadro).
AjusteBounds decidirAjuste(const TransformacaoItem& t, int larguraAntes, int alturaAntes, int canvasLargura, int canvasAltura);

}  // namespace fmcam
