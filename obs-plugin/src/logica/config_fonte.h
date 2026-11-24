// Configuracao de uma fonte "OBS Cam Mobile Extension" (o que o OBS guarda por fonte) e sua traducao para o core.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "conexao.h"

namespace fmcam {

constexpr const char* kNomeCliente = "OBS Cam Mobile Extension 1.0.4";

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
  // 1.0.3. Os padroes sao o comportamento de antes.
  std::string recepcao;      // "" = menor atraso (cada quadro vai ao OBS assim que decodifica), "estavel" = pequena reserva
  std::string aoCair;        // "" = manter o ultimo quadro, "preta" = tela preta, "imagem" = imagem do arquivo `imagemQueda`
  std::string imagemQueda;   // caminho da imagem (PNG, JPG) usada com aoCair = "imagem"
  std::string decodificacao; // "" = automatica (hardware acima de 1080p), "hw" = sempre hardware, "sw" = sempre software
  bool encaixar = true;      // item de cena sem bounds mantem o retangulo na tela quando o tamanho do video muda
};

// Reserva do modo "Mais estavel": cada quadro sai no instante captura + (menor atraso visto nos ultimos 2 s) + isto.
constexpr int kReservaEstavelMs = 120;
// Modo de hardware do decodificador para a opcao "decodificacao": -1 automatico, 1 sempre, 0 nunca.
int modoHardware(const std::string& decodificacao);

bool presetValido(const std::string& p);  // vazio nao e valido aqui: so as 6 predefinicoes. 1080p30 1080p60 1080p120 2160p30 720p30 720p60
struct PredefinicaoInfo {
  int largura = 0, altura = 0;
  double fps = 0;
};
// "2160p30" -> 3840x2160 a 30. Falso se nao for uma predefinicao conhecida.
bool infoPredefinicao(const std::string& p, PredefinicaoInfo& saida);
bool lenteValida(const std::string& l);  // "" (manter a do iPhone), wide, ultra ou tele
// O iPhone entrega menos que a predefinicao fixada na fonte? Compara o lado menor do quadro (em pe o video sai 1080x1920,
// e isso e o mesmo 1080p) e os quadros por segundo. Falso sem predefinicao fixada ou sem tamanho ainda.
bool entregaMenorQuePedido(const std::string& preset, int largura, int altura, double fps);
// "Quando o iPhone cair" trocado nas propriedades com o iPhone ja caido (ja teve video e nao esta conectado): a opcao nova
// vale na hora (tela preta ou imagem). Voltar para "manter" nao tem o que mostrar (o ultimo quadro ja saiu da tela).
bool aplicarQuedaAgora(const std::string& antes, const std::string& depois, bool conectado, bool jaTeveQuadro);
// Chave do texto que diz o que a fonte mostra com o iPhone caido: "Estado.SegurandoQuadro", "Estado.MostrandoPreta" ou
// "Estado.MostrandoImagem".
const char* chaveTextoQueda(const std::string& aoCair);
// Bitrates (bps) do combo de qualidade do painel: os mesmos do iPhone (Ajustes > Qualidade) e das propriedades da fonte, mais
// o bitrate em uso no iPhone quando ele nao esta na lista (assim o combo mostra o valor real e o "Aplicar" nao o troca).
std::vector<int64_t> opcoesDeBitrate(int64_t emUso);
// Corrige valores fora da faixa para o padrao (preset e lente invalidos viram "do iPhone", bitrate acima ou abaixo da
// faixa vai para o limite, 0 ou negativo vira "do iPhone").
ConfigFonte normalizar(ConfigFonte c);

// Versao do formato das configuracoes salvas pelo OBS ("versao_cfg"). 2: lente "wide" salva virou "do iPhone".
// 3: predefinicao 1080p30 e bitrate 10000 kbps salvos (o padrao antigo) viram "do iPhone".
// 4 (1.0.3): "encaixar" nasce ligado so em fonte nova. Fonte salva por versao anterior fica com ele desligado (o
// comportamento de antes) ate o operador ligar.
constexpr int kVersaoConfig = 4;
struct ValoresSalvos {
  int versao = 0;
  bool existente = false;  // a fonte ja foi salva antes (tem versao_cfg ou algum valor do usuario)
  bool temEncaixar = false;
  bool temLente = false, temPreset = false, temBitrate = false;  // o OBS so guarda o que o usuario mexeu ou o que a fonte gravou
  std::string lente, preset;
  int bitrateKbps = 0;
};
struct ResultadoMigracao {
  bool lente = false, preset = false, bitrate = false;
  bool encaixarDesligado = false;  // fonte de versao anterior: grava encaixar = false
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
