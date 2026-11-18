// Cliente do usbmuxd (servico USB da Apple), escrito do zero a partir do formato de mensagem:
// cabecalho de 16 bytes little-endian + plist XML. Sem estado global: cada chamada abre o proprio soquete.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rede.h"

namespace fmappcam {

constexpr const char* kProgNameUsbmux = "fmcam";

// ---- plist XML minimo ----
struct NoPlist {
  enum class Tipo { Dict, Array, Texto, Inteiro, Real, Bool, Dados };
  Tipo tipo = Tipo::Texto;
  std::string texto;
  int64_t inteiro = 0;
  double real = 0;
  bool b = false;
  std::vector<std::pair<std::string, NoPlist>> dict;
  std::vector<NoPlist> itens;
  const NoPlist* buscar(const std::string& chave) const;
  // Ints e bools como inteiro; devolve false se nao existir ou tiver outro tipo.
  bool buscarInteiro(const std::string& chave, int64_t& v) const;
  bool buscarTexto(const std::string& chave, std::string& v) const;
};
bool lerPlist(const std::string& xml, NoPlist& raiz);

// ---- mensagens ----
std::vector<uint8_t> montarMensagemUsbmux(uint32_t tag, const std::string& plistXml);
// Cabecalho little-endian: tamanho total, versao, tipo, tag. Devolve false se invalido.
bool lerCabecalhoUsbmux(const uint8_t* b16, uint32_t& tamanho, uint32_t& versao, uint32_t& tipo, uint32_t& tag);

std::string montarPlistListDevices();
std::string montarPlistListen();
std::string montarPlistConnect(int deviceId, uint16_t portaHost);  // PortNumber = htons(porta)
std::string montarPlistResult(int numero);
uint16_t portaEmOrdemDeRede(uint16_t porta);

struct DispositivoUsb {
  int id = -1;
  std::string serial;  // UDID
  std::string conexao;  // "USB" ou "Network"
};
bool operator==(const DispositivoUsb& a, const DispositivoUsb& b);
std::string montarPlistDeviceList(const std::vector<DispositivoUsb>& d);
std::string montarPlistAttached(const DispositivoUsb& d);
std::string montarPlistDetached(int id);

bool lerResultUsbmux(const std::string& xml, int& numero);
bool lerListaDispositivos(const std::string& xml, std::vector<DispositivoUsb>& saida, bool soUsb = true);

// ---- cliente ----
std::string caminhoUsbmuxPadrao();  // Mac/Linux: /var/run/usbmuxd, Windows: 127.0.0.1:27015
Soquete abrirUsbmux(const std::string& caminho, std::string& erro);
bool lerMensagemUsbmux(Soquete s, std::string& xml, uint32_t* tag, int timeoutMs, std::string& erro);
bool enviarMensagemUsbmux(Soquete s, uint32_t tag, const std::string& plistXml);

bool listarDispositivosUsb(const std::string& caminho, std::vector<DispositivoUsb>& saida, std::string& erro);
// Todas as entradas do usbmuxd, USB e Network. O macOS (e o Apple Mobile Device Service do Windows) tem duas entradas
// para o mesmo iPhone quando o Wi-Fi sync esta ligado, e as vezes apaga a USB mesmo com o cabo ligado.
bool listarDispositivos(const std::string& caminho, std::vector<DispositivoUsb>& saida, std::string& erro);
// A entrada USB tem prioridade. Sem ela, uma entrada Network do MESMO UDID serve (o Connect funciona igual, o usbmuxd
// encaminha pela rede). `udid` vazio so aceita USB (primeiro da lista). false se nao ha nenhuma.
bool escolherDispositivo(const std::vector<DispositivoUsb>& lista, const std::string& udid, DispositivoUsb& saida);
// Em caso de sucesso o soquete devolvido ja e o tunel TCP para a porta do aparelho.
Soquete conectarDispositivoUsb(const std::string& caminho, int deviceId, uint16_t porta, std::string& erro,
                               int timeoutMs = 5000);
// Acha o aparelho pelo UDID (SerialNumber) e conecta.
Soquete conectarUsbPorUdid(const std::string& caminho, const std::string& udid, uint16_t porta, std::string& erro);

// Mantem a lista de aparelhos USB atualizada com MessageType=Listen (eventos Attached/Detached).
class ObservadorUsbmux {
 public:
  using AoMudar = std::function<void()>;
  explicit ObservadorUsbmux(std::string caminho) : caminho_(std::move(caminho)) {}
  ~ObservadorUsbmux() { parar(); }
  void iniciar(AoMudar aoMudar = nullptr);
  void parar();
  std::vector<DispositivoUsb> dispositivos() const;  // so as entradas USB
  std::vector<DispositivoUsb> todos() const;         // USB e Network
  bool ouvindo() const { return ouvindo_; }

 private:
  void laco();
  std::string caminho_;
  AoMudar aoMudar_;
  std::thread th_;
  std::atomic<bool> parar_{false};
  std::atomic<bool> ouvindo_{false};
  mutable std::mutex m_;
  Soquete sock_ = kSoqueteInvalido;
  std::vector<DispositivoUsb> lista_;
};

}  // namespace fmappcam
