// Decodificador H.264 (libavcodec) em modo de baixa latencia: sem fila de quadros, uma thread.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace fmappcam {

class Decodificador {
 public:
  enum class Resultado { SemQuadro, Quadro, Erro };

  Decodificador();
  ~Decodificador();
  Decodificador(const Decodificador&) = delete;
  Decodificador& operator=(const Decodificador&) = delete;

  bool abrir(std::string& erro);
  // SPS/PPS em Annex B (pacote tipo 1). Vai junto do proximo quadro, na mesma unidade de acesso.
  void definirConfig(const uint8_t* dados, size_t n);
  // Um quadro Annex B (dados do pacote tipo 2 sem o byte de flags).
  Resultado decodificar(const uint8_t* dados, size_t n);
  const AVFrame* quadro() const { return quadro_; }  // valido ate a proxima chamada
  uint64_t erros() const { return erros_; }
  uint64_t quadrosDecodificados() const { return quadros_; }

 private:
  AVCodecContext* ctx_ = nullptr;
  AVFrame* quadro_ = nullptr;
  AVPacket* pacote_ = nullptr;
  std::vector<uint8_t> config_;
  bool configPendente_ = false;
  uint64_t erros_ = 0;
  uint64_t quadros_ = 0;
};

}  // namespace fmappcam
