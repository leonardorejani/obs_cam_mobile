// Registro dos iPhones vistos pelo processo: o observador do usbmuxd (uma unica conexao Listen para todas as
// fontes), o que cada sessao aprendeu no handshake (nome, modelo) e o estado de cada aparelho.
// Sem nada do libobs: testavel com iphone-falso e usbmuxd-falso.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "descoberta_wifi.h"
#include "usbmux.h"  // core/

namespace fmcam {

enum class EstadoAparelho {
  SemFonte,   // ligado no cabo, nenhuma fonte usando
  Procurando, // fonte tentando conectar
  Conectado,
  SemApp,     // cabo ok, mas o app OBS Cam Mobile nao esta aberto no iPhone
  Ocupado,    // outra fonte ou outro computador ja esta com o iPhone
  SemCabo,    // fonte configurada para um aparelho que nao esta no cabo
  Falha,      // erro que nao se resolve sozinho (ex.: versao do protocolo)
};
const char* chaveEstado(EstadoAparelho e);  // ASCII estavel, usado em log e no teste

struct StatusIphone {
  double bateria = -1;  // 0..1, -1 = desconhecida
  bool carregando = false;
  std::string temp;  // nominal, fair, serious, critical
  double fps = 0;
  int kbps = 0;
  // Campos de camera do status tipo 3 (docs/PROTOCOLO.md), todos opcionais: lente, zoom, foco, exposicao, branco,
  // bias, iso, obturador, noar, w, h, fps_alvo.
  std::string lente, foco, exposicao, branco, obturador;
  double zoom = 0;
  double bias = 0;
  bool temBias = false;
  int iso = 0;
  int w = 0, h = 0;
  double fpsAlvo = 0;
  // Qualidade em uso (status tipo 3, opcionais: iPhone antigo nao manda e ficam 0/false).
  int64_t bitrate = 0;      // bps configurado no codificador
  bool qualidadeObs = false;  // o OBS fixou preset ou bitrate nesta conexao (= presetObs ou bitrateObs)
  bool presetObs = false;     // o OBS fixou o preset (campo "preset_obs", ausente = false)
  bool bitrateObs = false;    // o OBS fixou o bitrate (campo "bitrate_obs", ausente = false)
  bool noAr = false;        // tally que a extensao mandou
  bool noArIphone = false;  // "noar" que o iPhone devolveu no status (confirma que recebeu)
  // Controle remoto (status de app 1.0.3+, ausentes em app antigo): botoes de lente que o aparelho oferece, o botao em
  // uso, faixa do EV, preset em uso e o estado das tres travas do trilho como a tela do iPhone mostra.
  std::vector<double> lentes;
  double fator = 0;
  double evMin = 0, evMax = 0;
  bool temFaixaEv = false;
  std::string preset;
  bool temTravas = false;
  bool travaAe = false, travaAf = false, travaBb = false;
  // Medido pela extensao (nao vem do iPhone): atraso da captura no iPhone ate o quadro entregue ao OBS, nos ultimos
  // ~3 s (p50 e p95), ida e volta do ping, quadros que o decodificador pulou e se a conexao e pelo cabo.
  double atrasoMs = -1, atrasoP95Ms = -1;
  double idaEVoltaMs = -1;
  uint64_t descartados = 0;
  bool canalCabo = false;  // pelo cabo (entrada USB do usbmuxd); false = Wi-Fi ou entrada de rede
};

// Resultado do botao "Testar" do painel, a partir das medidas acima (pura, testavel).
struct ResultadoTeste {
  enum class Nivel { Ok, Atencao, Ruim, SemDados };
  Nivel nivel = Nivel::SemDados;
  std::vector<std::string> avisos;  // chaves de locale ("Teste.Wifi", "Teste.AtrasoAlto", ...)
};
// Criterios (documentados em docs/GUIA-OPERADOR.md): sem conexao ou sem medida = SemDados. Pelo Wi-Fi ou entrada de rede
// = Atencao ("Teste.Wifi"). Atraso p95 acima de 150 ms = Atencao, acima de 300 ms = Ruim. Ida e volta do ping acima de
// 20 ms no cabo = Atencao. fps abaixo de 90% do alvo = Atencao. Quadros pulados pelo decodificador = Atencao.
ResultadoTeste avaliarTeste(const StatusIphone& s, bool conectado);

struct InfoAparelho {
  std::string udid;
  std::string nome;    // nome dado ao aparelho (handshake)
  std::string modelo;  // "iPhone 13"
  std::string idApp;   // id fixo do app naquele aparelho
  bool noCabo = false;
  bool wifi = false;  // achado por Bonjour (udid = "wifi:<id>")
  bool pelaRede = false;  // conectado pela entrada "Network" do usbmuxd (o macOS apagou a USB com o cabo ligado)
  std::string ip;
  int fontes = 0;  // quantas sessoes seguram este aparelho
  EstadoAparelho estado = EstadoAparelho::SemFonte;
  std::string mensagem;
  StatusIphone status;
  int largura = 0, altura = 0;
};

std::string udidCurto(const std::string& udid);  // "00008130..001C"
// "iPhone Palco 1 (iPhone 13)" ou, sem handshake ainda, "iPhone 00008130..001C".
std::string rotuloAparelho(const InfoAparelho& i);

class RegistroAparelhos {
 public:
  using AoMudar = std::function<void()>;
  // `wifi` (opcional): descoberta Bonjour dos iPhones no Wi-Fi.
  explicit RegistroAparelhos(std::string caminhoUsbmux, std::unique_ptr<DescobertaWifi> wifi = nullptr);
  ~RegistroAparelhos();
  RegistroAparelhos(const RegistroAparelhos&) = delete;
  RegistroAparelhos& operator=(const RegistroAparelhos&) = delete;

  void iniciar();
  void parar();
  bool ouvindo() const;
  const std::string& caminhoUsbmux() const { return caminho_; }

  // Assinatura para "a lista mudou" (plugou, desplugou, nome novo, estado novo). Status de fps/bateria nao notifica.
  // O callback pode rodar em qualquer thread e nao deve chamar de volta o registro durante a assinatura.
  int assinar(AoMudar cb);
  void desassinar(int id);

  // Cabo primeiro, depois os do Wi-Fi (sem repetir o iPhone que ja esta no cabo), depois os que alguma fonte segura.
  std::vector<InfoAparelho> listar() const;
  bool buscarWifi(const std::string& id, ServicoWifi& saida) const;
  bool buscar(const std::string& udid, InfoAparelho& saida) const;
  std::vector<ServicoWifi> servicosWifi() const;  // todos os iPhones anunciados por Bonjour (inclusive os que tambem estao no cabo)
  // Rotulo do iPhone na interface, sem o "Cabo"/"Wi-Fi": nome comercial do modelo ("iPhone 15 Pro Max"), com " (2)" no
  // segundo do mesmo modelo (ordem por UDID, estavel). `valor` = UDID do cabo ou "wifi:<id>". "" se o registro nao conhece.
  std::string rotulo(const std::string& valor) const;
  std::string nomeConhecido(const std::string& udid) const;  // rotulo guardado de uma conexao anterior ("" se nunca vi)

  // Uma fonte por iPhone dentro do processo. Devolve false se outra dona ja segura o aparelho.
  // O aparelho e o iPhone, nao o caminho: "UDID" (cabo), "wifi:<id>" (Bonjour) e "wifi:<ip>" (manual) do mesmo iPhone
  // disputam a mesma vaga quando o registro sabe que sao o mesmo (id do app aprendido no handshake, ou `idApp` dado
  // por quem ja descobriu, ex.: o id que o iPhone devolve na recusa "ocupado").
  bool reservar(const std::string& udid, const void* dona, const std::string& idApp = "");
  void liberar(const std::string& udid, const void* dona);
  bool ehDona(const std::string& udid, const void* dona) const;

  // Chamados pelas sessoes.
  void definirIdentidade(const std::string& udid, const std::string& nome, const std::string& modelo,
                         const std::string& idApp, int w, int h);
  void definirEstado(const std::string& udid, EstadoAparelho e, const std::string& mensagem, bool pelaRede = false);
  void definirStatus(const std::string& udid, const StatusIphone& s);
  // Canal de comando da sessao dona (controle remoto do painel). So a dona define, e liberar() apaga.
  void definirCanal(const std::string& udid, const void* dona, std::function<bool(const std::string&)> canal);
  // Manda um comando tipo 4 ao iPhone pela sessao que o segura. Falso se nenhuma sessao conectada o segura.
  bool enviarComando(const std::string& udid, const std::string& json) const;

 private:
  struct Dados {
    InfoAparelho info;
    const void* dona = nullptr;
    std::function<bool(const std::string&)> canal;
  };
  void notificar();
  InfoAparelho montar(const std::string& udid, const Dados* d, bool noCabo) const;
  bool temEntradaRede(const std::string& udid) const;

  std::string caminho_;
  fmappcam::ObservadorUsbmux observador_;
  std::unique_ptr<DescobertaWifi> wifi_;
  mutable std::mutex m_;
  std::map<std::string, Dados> dados_;
  std::map<int, AoMudar> assinantes_;
  int proximaAssinatura_ = 1;
};

}  // namespace fmcam
