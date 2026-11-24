// Uma sessao = um iPhone puxando video para uma fonte: Conexao + Decodificador + estado, tudo dela.
// Nada global alem do RegistroAparelhos compartilhado. Sem libobs: o quadro sai por callback.
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "aparelhos.h"
#include "config_fonte.h"
#include "quadro.h"

namespace fmcam {

struct EstadoSessao {
  EstadoAparelho estado = EstadoAparelho::Procurando;
  std::string mensagem;  // texto tecnico curto (para log)
  std::string udid;      // do aparelho em uso (descoberto na conexao quando a config nao tinha)
  std::string nome, modelo;
  uint64_t quadros = 0;       // entregues
  uint64_t errosDecodificacao = 0;
  uint64_t quadrosDescartados = 0;  // pulados porque o decodificador nao dava conta (o atraso passou do limite)
  bool hardware = false;            // decodificando por hardware (VideoToolbox, D3D11VA, DXVA2)
  double fps = 0;
  int largura = 0, altura = 0;
  int reconexoes = 0;
  double atrasoCanoMs = -1;
  int respLargura = 0, respAltura = 0;  // o que o iPhone disse que entrega (handshake)
  double respFps = 0;
  bool noAr = false;
  bool pelaRede = false;  // conectado pela entrada de rede do usbmuxd (cabo ligado, mas o macOS apagou a entrada USB)
  bool jaTeveQuadro = false;  // a fonte esta segurando o ultimo quadro quando nao esta Conectado
  // Ultimo status do iPhone (bateria, temperatura, kbps, ...) e as medidas da extensao (atraso ate o OBS, ida e volta),
  // atualizado a cada segundo com video. Base do status ao vivo nas propriedades e do botao Testar.
  StatusIphone status;
};

class SessaoFonte {
 public:
  using AoQuadro = std::function<void(const QuadroSaida&)>;  // na thread da conexao; os planos valem so durante a chamada
  using AoEstado = std::function<void(const EstadoSessao&)>;  // idem; so quando o estado ou o aparelho mudam

  SessaoFonte(ConfigFonte cfg, std::string caminhoUsbmux, RegistroAparelhos* registro);
  ~SessaoFonte();  // parar(1500)
  SessaoFonte(const SessaoFonte&) = delete;
  SessaoFonte& operator=(const SessaoFonte&) = delete;

  void definirCallbacks(AoQuadro q, AoEstado e);
  void iniciar();
  // Desliga os callbacks na hora e fecha tudo. Se a thread da conexao nao sair em `timeoutMs`, ela termina
  // sozinha em segundo plano (dona do proprio estado) e a funcao devolve false: o chamador nunca fica preso.
  bool parar(int timeoutMs);
  // So teste: segura cada conexao recem aceita por `ms` antes de reservar o iPhone, e conta quantas entraram nessa espera.
  static void definirAtrasoDeTesteConectadoMs(int ms);
  static int entradasNoAtrasoDeTeste();
  // Tally: a cena com esta fonte esta no Programa. Manda {"cmd":"noar","valor":...} e repete ao reconectar.
  void definirNoAr(bool noAr);
  EstadoSessao estado() const;
  const ConfigFonte& config() const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

// Quantas sessoes ainda estao encerrando em segundo plano; espera ate `ms` que cheguem a zero.
int sessoesEncerrando();
bool aguardarEncerramentos(int ms);

}  // namespace fmcam
