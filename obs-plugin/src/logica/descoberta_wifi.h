// Descoberta dos iPhones no Wi-Fi por Bonjour ("_fmcam._tcp", nome do servico = nome da camera, TXT id e v).
// Uma interface, uma implementacao por plataforma (Mac: dns_sd.h; Windows: windns.h, sem o Bonjour da Apple).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fmcam {

constexpr const char* kTipoServicoBonjour = "_fmcam._tcp";

struct ServicoWifi {
  std::string id;    // TXT "id": o mesmo id que o handshake devolve
  std::string nome;  // nome do servico = nome da camera
  std::string ip;    // IPv4 em texto
  uint16_t porta = 4848;
  int versao = 1;    // TXT "v"
};

class DescobertaWifi {
 public:
  using AoMudar = std::function<void()>;
  virtual ~DescobertaWifi() = default;
  virtual void iniciar(AoMudar aoMudar) = 0;
  virtual void parar() = 0;
  virtual std::vector<ServicoWifi> servicos() const = 0;  // ja sem duplicatas (mesmo id em varias interfaces)
};

// interfaceIndex 0 = todas as redes. Nos testes, 0xFFFFFFFF (so local, sem tocar a rede nem pedir permissao).
std::unique_ptr<DescobertaWifi> criarDescobertaWifi(uint32_t interfaceIndex = 0);

}  // namespace fmcam
