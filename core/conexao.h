// Cliente de um aparelho: conecta (TCP direto ou cabo via usbmuxd), faz o handshake, manda ping a cada 1 s,
// considera morta a conexao sem dados por 3 s e reconecta. Uma instancia por aparelho, sem estado global:
// varias instancias rodam ao mesmo tempo, cada uma na propria thread.
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "protocolo.h"
#include "rede.h"

namespace fmappcam {

enum class EstadoConexao { Conectando, Conectado, Desconectado, ErroFatal };

struct OpcoesConexao {
  std::string host = "127.0.0.1";  // modo TCP direto
  uint16_t porta = kPortaPadrao;
  bool usb = false;  // modo cabo
  std::string usbmuxCaminho;  // vazio = padrao do sistema
  std::string udid;  // vazio = primeiro aparelho USB da lista
  PedidoHandshake pedido;
  bool reconectar = true;
  int atrasoReconexaoMs = 500;
  int timeoutSemDadosMs = 3000;
  int intervaloPingMs = 1000;
  // Consultado na thread da conexao antes de CADA tentativa (nunca no meio de uma). Falso = nao toca no iPhone agora:
  // espera 200 ms e pergunta de novo, sem contar reconexao nem avisar queda. Serve para a extensao nao brigar por um
  // iPhone que outra fonte do mesmo processo ja segura. Tem que ser rapido e nao pode chamar a propria Conexao.
  std::function<bool()> podeConectar;
};

class Conexao {
 public:
  // O pacote chega com o instante (relogio monotonico do cliente, us) em que os bytes foram lidos.
  // Pong nao e entregue: alimenta a sincronia. Os callbacks rodam na thread da conexao.
  // Defina os callbacks ANTES de iniciar().
  using AoPacote = std::function<void(const Pacote&, uint64_t chegadaUs)>;
  using AoEstado = std::function<void(EstadoConexao, const std::string& mensagem)>;

  explicit Conexao(OpcoesConexao op);
  ~Conexao();
  Conexao(const Conexao&) = delete;
  Conexao& operator=(const Conexao&) = delete;

  void definirCallbacks(AoPacote aoPacote, AoEstado aoEstado);
  void iniciar();
  // Retorna rapido (meta < 100 ms) em qualquer situacao: connect em IP mudo, usbmuxd mudo, leitura parada.
  // Garantias, depois que parar() retorna:
  //  - nenhum callback NOVO comeca (nem aoPacote nem aoEstado);
  //  - a thread interna so mexe em estado proprio, que ela mesma mantem vivo (sobrevive ao destrutor), entao
  //    nao ha acesso a memoria liberada pela Conexao.
  // Um callback que ja estava rodando e esperado por ate ~50 ms. Se passar disso, parar() retorna mesmo assim e
  // o callback termina sozinho depois: o que ele usa do chamador (variaveis capturadas) tem que continuar valido
  // ate ele acabar. Callback curto (o normal) termina antes e a espera nem e sentida.
  // A notificacao final Desconectado("parado") e entregue antes de parar() retornar, salvo o caso do callback lento.
  // Pode ser chamado de qualquer thread, mais de uma vez, e nunca de dentro de um callback desta mesma Conexao.
  void parar();

  bool enviarComando(const std::string& json);  // {"cmd":"keyframe"} etc.
  RespostaHandshake resposta() const;           // da conexao atual (ou da ultima)
  // Ultima recusa "ocupado" (o iPhone manda o `id` dele nela, quando o app e novo): quem segura o aparelho.
  RespostaHandshake recusa() const;
  bool sincronizada() const;
  // Atraso de cano (us) de um quadro, ou false se ainda nao houve pong.
  bool atrasoUs(uint64_t chegadaUs, uint64_t tempoQuadro, int64_t& atraso) const;
  int64_t idaEVoltaUs() const;
  int reconexoes() const;
  // UDID do aparelho da conexao atual (ou da ultima). No modo cabo sem UDID configurado, e o aparelho que foi escolhido.
  std::string udidAtual() const;
  bool conectada() const;
  // No modo cabo: true quando a conexao atual passa pela entrada "Network" do usbmuxd (o macOS apagou a entrada USB
  // com o cabo ligado). Volta ao cabo sozinha assim que a entrada USB reaparece.
  bool pelaRede() const;

 private:
  struct Nucleo;  // todo o estado que a thread usa: vive no shared_ptr, nao em `this`
  std::shared_ptr<Nucleo> nucleo() const;

  OpcoesConexao op_;
  AoPacote aoPacote_;
  AoEstado aoEstado_;
  mutable std::mutex mN_;
  std::shared_ptr<Nucleo> n_;
  std::thread th_;
};

// Threads que a Conexao solta (detach): o vigia da entrada USB e a thread de uma conexao cujo callback demorou no
// parar(). Quem descarrega uma DLL/plugin espera ate `ms` que todas terminem (no Windows, thread viva com o codigo da
// DLL descarregado derruba o processo ao fechar o OBS).
int threadsSoltas();
bool aguardarThreadsSoltas(int ms);

}  // namespace fmappcam
