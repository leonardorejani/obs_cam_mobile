// Configuracao de uma fonte "OBS Cam Mobile Extension" (o que o OBS guarda por fonte) e sua traducao para o core.
#pragma once
#include <cstdint>
#include <string>

#include "conexao.h"

namespace fmcam {

constexpr const char* kNomeCliente = "OBS Cam Mobile Extension 1.0.0";

struct ConfigFonte {
  std::string udid;       // vazio = primeiro iPhone no cabo (so serve com um aparelho)
  std::string nomeCache;  // rotulo visto na ultima conexao, para mostrar mesmo com o iPhone fora do cabo
  std::string preset;  // vazio = qualidade salva no iPhone (nao manda o campo); 1080p30 1080p60 1080p120 2160p30 720p30 720p60
  std::string lente;  // vazio = manter a lente que o operador escolheu no iPhone (nao manda o campo); wide ultra tele
  int bitrateKbps = 0;  // 0 = bitrate salvo no iPhone (nao manda o campo); senao 1000 a 50000
  bool wifi = false;
  std::string wifiIp;  // so vale com wifi = true (reserva manual)
  uint16_t wifiPorta = 4848;  // porta anunciada pelo Bonjour
  std::string wifiId;  // iPhone achado por Bonjour (valor "wifi:<id>" na lista de aparelhos); o IP vem da descoberta
};

bool presetValido(const std::string& p);  // vazio nao e valido aqui: so as 6 predefinicoes. 1080p30 1080p60 1080p120 2160p30 720p30 720p60
struct PredefinicaoInfo {
  int largura = 0, altura = 0;
  double fps = 0;
};
// "2160p30" -> 3840x2160 a 30. Falso se nao for uma predefinicao conhecida.
bool infoPredefinicao(const std::string& p, PredefinicaoInfo& saida);
bool lenteValida(const std::string& l);  // "" (manter a do iPhone), wide, ultra ou tele
// Corrige valores fora da faixa para o padrao (preset e lente invalidos viram "do iPhone", bitrate acima ou abaixo da
// faixa vai para o limite, 0 ou negativo vira "do iPhone").
ConfigFonte normalizar(ConfigFonte c);

// Versao do formato das configuracoes salvas pelo OBS ("versao_cfg"). 2: lente "wide" salva virou "do iPhone".
// 3: predefinicao 1080p30 e bitrate 10000 kbps salvos (o padrao antigo) viram "do iPhone".
constexpr int kVersaoConfig = 3;
struct ValoresSalvos {
  int versao = 0;
  bool temLente = false, temPreset = false, temBitrate = false;  // o OBS so guarda o que o usuario mexeu ou o que a fonte gravou
  std::string lente, preset;
  int bitrateKbps = 0;
};
struct ResultadoMigracao {
  bool lente = false, preset = false, bitrate = false;
  bool alterou() const { return lente || preset || bitrate; }
};
// Valor salvo igual ao padrao antigo nao e escolha: nao da para saber se o operador mexeu, e fixar 1080p30/10000 kbps
// sobrescrevia a qualidade escolhida no iPhone. Qualquer outro valor salvo e respeitado. Sempre deixa versao = kVersaoConfig.
ResultadoMigracao migrarValores(ValoresSalvos& v);
// Mudanca que obriga reconectar (todo o resto so muda o texto exibido).
bool mudouConexao(const ConfigFonte& a, const ConfigFonte& b);

// Caminho do usbmuxd: variavel FMCAM_USBMUXD (so para teste e diagnostico) ou o padrao do sistema.
std::string caminhoUsbmuxConfigurado();

fmappcam::OpcoesConexao montarOpcoes(const ConfigFonte& c, const std::string& usbmuxCaminho);

}  // namespace fmcam
