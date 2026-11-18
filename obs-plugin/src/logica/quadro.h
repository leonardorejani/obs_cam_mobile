// Ponte entre o AVFrame do decodificador e o quadro que a fonte entrega ao OBS, sem copiar pixels e sem
// conversao de formato: NV12 e I420 passam como estao.
#pragma once
#include <cstdint>
#include <string>

struct AVFrame;

namespace fmcam {

struct QuadroSaida {
  enum class Formato { Nenhum, NV12, I420 };
  enum class Matriz { BT601, BT709, BT2020 };
  Formato formato = Formato::Nenhum;
  Matriz matriz = Matriz::BT709;
  bool faixaCompleta = false;
  int largura = 0, altura = 0;
  const uint8_t* plano[3] = {nullptr, nullptr, nullptr};
  int passo[3] = {0, 0, 0};
};

// Aponta para os planos do AVFrame (validos ate o proximo decodificar). Falso com formato que a fonte nao entrega.
bool converterQuadro(const AVFrame* f, QuadroSaida& saida, std::string& erro);

}  // namespace fmcam
