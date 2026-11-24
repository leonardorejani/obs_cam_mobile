// Decodificador H.264 (libavcodec) em modo de baixa latencia: sem fila de quadros, uma thread.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;

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
  // Um quadro Annex B (dados do pacote tipo 2 sem o byte de flags). `chave`: e quadro-chave (a troca para o decodificador
  // de hardware so acontece num quadro-chave, que recomeca o fluxo sem erro).
  Resultado decodificar(const uint8_t* dados, size_t n, bool chave = false);
  const AVFrame* quadro() const { return saida_ ? saida_ : quadro_; }  // valido ate a proxima chamada (NV12 se veio do hardware)
  // Decodificacao por hardware (VideoToolbox no Mac, D3D11VA ou DXVA2 no Windows). Liga sozinha quando o video passa de
  // 1080p, onde um nucleo so de software nao da 30 quadros por segundo em PC modesto; cai para software se falhar.
  // FMCAM_HW=0 desliga, FMCAM_HW=1 liga em qualquer tamanho (teste e diagnostico).
  bool usaHardware() const { return hwAtivo_; }
  // O video ja pede hardware mas esperamos o proximo quadro-chave para trocar (quem usa pode pedir um na hora).
  bool aguardaChaveParaHardware() const { return hwPendente_; }
  // Opcao da fonte: -1 automatico (padrao), 1 sempre hardware, 0 sempre software. Chamar antes de abrir().
  // FMCAM_HW, quando definida, vale mais (diagnostico).
  void definirModoHardware(int modo);
  int modoHardware() const { return modoHw_; }
  static void definirAtrasoDeTesteUs(int us);  // so teste: simula um decodificador lento
  uint64_t erros() const { return erros_; }
  uint64_t quadrosDecodificados() const { return quadros_; }

 private:
  bool abrirContexto(bool hardware, std::string& erro);
  void voltarParaSoftware();
  bool transferir();

  AVCodecContext* ctx_ = nullptr;
  AVFrame* quadro_ = nullptr;
  AVFrame* sw_ = nullptr;
  const AVFrame* saida_ = nullptr;
  AVBufferRef* hwDev_ = nullptr;
  int hwFmt_ = -1;  // AVPixelFormat do hardware, em int para nao puxar o cabecalho do FFmpeg
  bool hwAtivo_ = false, hwPendente_ = false, hwFalhou_ = false;
  int modoHw_ = -1;  // -1 automatico, 0 desligado, 1 sempre
  bool modoPorAmbiente_ = false;
  AVPacket* pacote_ = nullptr;
  std::vector<uint8_t> config_;
  bool configPendente_ = false;
  uint64_t erros_ = 0;
  uint64_t quadros_ = 0;
};

}  // namespace fmappcam
