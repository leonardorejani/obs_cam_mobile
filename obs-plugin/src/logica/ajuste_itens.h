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

// Opcao "Encaixar na tela sozinho" (1.0.3). Regra: item SEM bounds, sem recorte e com escala positiva (sem espelhar)
// passa a ter bounds do retangulo que ele ocupava na tela com o tamanho antigo (escala x tamanho antigo), com a mesma
// posicao, alinhamento e rotacao. O video novo (girado ou de outra resolucao) cabe dentro desse retangulo
// (SCALE_INNER, centralizado), entao o item nao muda de lugar nem de tamanho. Item com bounds ja se comporta assim no
// OBS e nunca e tocado. Item recortado ou espelhado tambem nao (o recorte e em pixels do video antigo).
// posX/posY do resultado ficam iguais aos do item.
AjusteBounds decidirEncaixe(const TransformacaoItem& t, int larguraAntes, int alturaAntes);

}  // namespace fmcam
