// Protocolo OBS Cam Mobile v1 (ver docs/PROTOCOLO.md): pacotes, handshake, JSON plano e sincronia de relogio.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace fmappcam {

constexpr uint16_t kPortaPadrao = 4848;
constexpr size_t kTamanhoCabecalho = 13;
constexpr uint32_t kTamanhoMaximoPacote = 8u * 1024u * 1024u;
constexpr size_t kLinhaHandshakeMaxima = 4096;

enum TipoPacote : uint8_t {
  kTipoConfig = 1,
  kTipoQuadro = 2,
  kTipoStatus = 3,
  kTipoComando = 4,
  kTipoPing = 9,
  kTipoPong = 10,
};

struct Pacote {
  uint8_t tipo = 0;
  uint64_t tempo = 0;  // microssegundos, relogio de quem enviou
  std::vector<uint8_t> dados;
};

bool tipoConhecido(uint8_t tipo);
std::vector<uint8_t> serializarPacote(const Pacote& p);
// Le so o cabecalho (13 bytes). Devolve false se faltarem bytes.
bool lerCabecalho(const uint8_t* b, size_t n, uint8_t& tipo, uint64_t& tempo, uint32_t& tamanho);

void escreverU32BE(std::vector<uint8_t>& v, uint32_t x);
void escreverU64BE(std::vector<uint8_t>& v, uint64_t x);
uint32_t lerU32BE(const uint8_t* b);
uint64_t lerU64BE(const uint8_t* b);

Pacote criarPing(uint64_t tempoCliente);
Pacote criarPong(uint64_t tempoIphone, uint64_t tempoDoPing);
Pacote criarComando(uint64_t tempo, const std::string& json);
Pacote criarConfig(uint64_t tempo, const std::vector<uint8_t>& spsPps);
Pacote criarQuadro(uint64_t tempo, bool chave, const uint8_t* annexb, size_t n);

// Parser incremental: alimenta bytes soltos, tira pacotes completos.
// Tipo desconhecido e pulado sem guardar os dados. Tamanho acima do maximo e erro permanente.
class ParserPacotes {
 public:
  void alimentar(const uint8_t* dados, size_t n);
  bool proximo(Pacote& saida);
  bool erro() const { return erro_; }
  void reiniciar();

 private:
  std::vector<uint8_t> buf_;
  size_t pos_ = 0;
  uint64_t pular_ = 0;
  bool erro_ = false;
};

// ---- JSON plano (objeto com texto, numero, bool, null e array de ate 1024 numeros) ----
// Qualquer outro valor (objeto, lista de outra coisa ou maior, aninhado) e lido e pulado: vira Tipo::Outro, sem dados,
// e buscarJson nunca o devolve. E assim que um campo novo de uma versao futura nao quebra quem le (docs/PROTOCOLO.md, 1).
// lerObjetoJson recusa: UTF-8 invalido, BOM, chave repetida no objeto, JSON quebrado, lixo depois do objeto e
// aninhamento acima de 64 niveis.
struct ValorJson {
  enum class Tipo { Nulo, Bool, Numero, Texto, ArrayNumeros, Outro };
  Tipo tipo = Tipo::Nulo;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<double> arr;
};
using ObjetoJson = std::map<std::string, ValorJson>;

bool lerObjetoJson(const std::string& texto, ObjetoJson& saida);
std::string escaparJson(const std::string& s);
const ValorJson* buscarJson(const ObjetoJson& o, const std::string& chave, ValorJson::Tipo tipo);

// ---- Handshake ----
struct PedidoHandshake {
  int v = 1;
  std::string cliente;
  // preset vazio e bitrate <= 0 = nao manda o campo: o iPhone usa a qualidade salva nele (docs/PROTOCOLO.md, 1).
  // lerPedidoHandshake devolve vazio/0 quando o campo esta ausente.
  std::string preset = "1080p30";
  std::string lente = "wide";
  int64_t bitrate = 10000000;
};
enum class ErroHandshake { Nenhum, Handshake, Versao };

bool versaoSuportada(int v);
std::string montarPedidoHandshake(const PedidoHandshake& p);  // termina em \n
// `linha` sem o \n. Valor invalido em campo opcional cai no padrao.
ErroHandshake lerPedidoHandshake(const std::string& linha, PedidoHandshake& saida);

struct RespostaHandshake {
  bool ok = false;
  int v = 1;
  std::string aparelho;  // modelo
  std::string nome;      // nome dado pelo dono ao aparelho
  std::string id;        // uuid do aparelho
  int w = 0, h = 0;
  double fps = 0;
  std::string codec;
  std::string erro;
  std::vector<int> vSuportadas;
};
std::string montarRespostaHandshake(const RespostaHandshake& r);  // termina em \n
bool lerRespostaHandshake(const std::string& linha, RespostaHandshake& saida);

// ---- Sincronia de relogio (secao 3) ----
class SincroniaRelogio {
 public:
  static constexpr size_t kAmostras = 10;
  void registrar(uint64_t t0, uint64_t t1, uint64_t ts);
  bool pronta() const { return !amostras_.empty(); }
  int64_t diferenca() const;    // iPhone menos cliente, da amostra de menor ida e volta
  int64_t idaEVolta() const;    // a menor das ultimas 10
  // Pong com eco no futuro (t0 > t1), ida e volta acima de 2 s ou t0/t1/ts absurdos e descartado. Sem estouro
  // de inteiro: toda conta e saturada em int64 (o `tempo` vem de outro aparelho e pode ser qualquer valor).
  // atraso = chegada - (tempoQuadro - diferenca)
  int64_t atraso(uint64_t chegadaCliente, uint64_t tempoQuadro) const;
  void reiniciar() { amostras_.clear(); }

 private:
  struct Amostra {
    int64_t ida;
    int64_t dif;
  };
  std::deque<Amostra> amostras_;
  const Amostra* melhor() const;
};

}  // namespace fmappcam
