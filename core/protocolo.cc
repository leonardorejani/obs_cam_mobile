#include "protocolo.h"

#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace fmappcam {

bool tipoConhecido(uint8_t t) {
  return t == kTipoConfig || t == kTipoQuadro || t == kTipoStatus || t == kTipoComando || t == kTipoPing ||
         t == kTipoPong;
}

void escreverU32BE(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 3; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
void escreverU64BE(std::vector<uint8_t>& v, uint64_t x) {
  for (int i = 7; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
uint32_t lerU32BE(const uint8_t* b) {
  return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
}
uint64_t lerU64BE(const uint8_t* b) { return (uint64_t(lerU32BE(b)) << 32) | lerU32BE(b + 4); }

std::vector<uint8_t> serializarPacote(const Pacote& p) {
  std::vector<uint8_t> v;
  v.reserve(kTamanhoCabecalho + p.dados.size());
  v.push_back(p.tipo);
  escreverU64BE(v, p.tempo);
  escreverU32BE(v, static_cast<uint32_t>(p.dados.size()));
  v.insert(v.end(), p.dados.begin(), p.dados.end());
  return v;
}

bool lerCabecalho(const uint8_t* b, size_t n, uint8_t& tipo, uint64_t& tempo, uint32_t& tamanho) {
  if (n < kTamanhoCabecalho) return false;
  tipo = b[0];
  tempo = lerU64BE(b + 1);
  tamanho = lerU32BE(b + 9);
  return true;
}

Pacote criarPing(uint64_t t) { return Pacote{kTipoPing, t, {}}; }
Pacote criarPong(uint64_t t, uint64_t tempoDoPing) {
  Pacote p{kTipoPong, t, {}};
  escreverU64BE(p.dados, tempoDoPing);
  return p;
}
Pacote criarComando(uint64_t t, const std::string& json) {
  return Pacote{kTipoComando, t, std::vector<uint8_t>(json.begin(), json.end())};
}
Pacote criarConfig(uint64_t t, const std::vector<uint8_t>& d) { return Pacote{kTipoConfig, t, d}; }
Pacote criarQuadro(uint64_t t, bool chave, const uint8_t* annexb, size_t n) {
  Pacote p{kTipoQuadro, t, {}};
  p.dados.reserve(n + 1);
  p.dados.push_back(chave ? 1 : 0);
  p.dados.insert(p.dados.end(), annexb, annexb + n);
  return p;
}

// ---- Parser ----
void ParserPacotes::reiniciar() {
  buf_.clear();
  pos_ = 0;
  pular_ = 0;
  erro_ = false;
}

void ParserPacotes::alimentar(const uint8_t* dados, size_t n) {
  if (erro_) return;
  if (pos_ > 0 && pos_ >= buf_.size() / 2) {
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(pos_));
    pos_ = 0;
  }
  buf_.insert(buf_.end(), dados, dados + n);
}

bool ParserPacotes::proximo(Pacote& saida) {
  while (!erro_) {
    size_t disp = buf_.size() - pos_;
    if (pular_ > 0) {
      size_t k = static_cast<size_t>(pular_ < disp ? pular_ : disp);
      pos_ += k;
      pular_ -= k;
      if (pular_ > 0) return false;
      continue;
    }
    uint8_t tipo;
    uint64_t tempo;
    uint32_t tam;
    if (!lerCabecalho(buf_.data() + pos_, disp, tipo, tempo, tam)) return false;
    if (tam > kTamanhoMaximoPacote) {
      erro_ = true;
      return false;
    }
    if (!tipoConhecido(tipo)) {
      pos_ += kTamanhoCabecalho;
      pular_ = tam;
      continue;
    }
    if (disp < kTamanhoCabecalho + tam) return false;
    const uint8_t* d = buf_.data() + pos_ + kTamanhoCabecalho;
    saida.tipo = tipo;
    saida.tempo = tempo;
    saida.dados.assign(d, d + tam);
    pos_ += kTamanhoCabecalho + tam;
    return true;
  }
  return false;
}

// ---- JSON plano ----
namespace {

constexpr int kProfundidadeMaxima = 64;
constexpr size_t kTamanhoMaximoJson = 1u << 20;
constexpr size_t kArrayNumerosMaximo = 1024;

// double -> int sem UB: fora da faixa satura, NaN vira 0.
int paraIntSat(double d) {
  if (!(d == d)) return 0;
  if (d >= static_cast<double>(INT_MAX)) return INT_MAX;
  if (d <= static_cast<double>(INT_MIN)) return INT_MIN;
  return static_cast<int>(d);
}

// UTF-8 estrito: sem sobrelongo, sem substituto (U+D800..DFFF), ate U+10FFFF.
bool utf8Valido(const std::string& t) {
  size_t i = 0, n = t.size();
  while (i < n) {
    unsigned char c = static_cast<unsigned char>(t[i]);
    if (c < 0x80) { ++i; continue; }
    size_t extra;
    uint32_t cp, minimo;
    if (c >= 0xC2 && c <= 0xDF) { extra = 1; cp = c & 0x1F; minimo = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { extra = 2; cp = c & 0x0F; minimo = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { extra = 3; cp = c & 0x07; minimo = 0x10000; }
    else return false;
    if (i + extra >= n) return false;
    for (size_t k = 1; k <= extra; ++k) {
      unsigned char d = static_cast<unsigned char>(t[i + k]);
      if ((d & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (d & 0x3F);
    }
    if (cp < minimo || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += extra + 1;
  }
  return true;
}

struct LeitorJson {
  const std::string& t;
  size_t i = 0;
  explicit LeitorJson(const std::string& s) : t(s) {}

  void ws() {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) ++i;
  }
  bool consome(char c) {
    ws();
    if (i < t.size() && t[i] == c) {
      ++i;
      return true;
    }
    return false;
  }
  static void utf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
      s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  bool hex4(uint32_t& v) {
    if (i + 4 > t.size()) return false;
    v = 0;
    for (int k = 0; k < 4; ++k) {
      char c = t[i + static_cast<size_t>(k)];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
      else return false;
    }
    i += 4;
    return true;
  }
  bool texto(std::string& out) {
    ws();
    if (i >= t.size() || t[i] != '"') return false;
    ++i;
    out.clear();
    while (i < t.size()) {
      unsigned char c = static_cast<unsigned char>(t[i++]);
      if (c == '"') return true;
      if (c < 0x20) return false;
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        continue;
      }
      if (i >= t.size()) return false;
      char e = t[i++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00) {  // par substituto
            uint32_t lo;
            if (i + 2 > t.size() || t[i] != '\\' || t[i + 1] != 'u') return false;
            i += 2;
            if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false;
          }
          utf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;  // sem aspas de fechamento
  }
  // Sintaxe de numero JSON. Expoente absurdo (1e999) e sintaxe valida: o valor satura em +-DBL_MAX.
  bool numero(double& v) {
    ws();
    size_t ini = i;
    if (i < t.size() && t[i] == '-') ++i;
    size_t dig = 0;
    while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i, ++dig;
    if (dig == 0) return false;
    if (i < t.size() && t[i] == '.') {
      ++i;
      size_t f = 0;
      while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i, ++f;
      if (f == 0) return false;
    }
    if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
      ++i;
      if (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
      size_t f = 0;
      while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i, ++f;
      if (f == 0) return false;
    }
    std::string s = t.substr(ini, i - ini);
    v = std::strtod(s.c_str(), nullptr);
    if (std::isnan(v)) return false;
    if (std::isinf(v)) v = v > 0 ? DBL_MAX : -DBL_MAX;
    return true;
  }
  bool palavra(const char* p) {
    size_t n = std::strlen(p);
    if (t.compare(i, n, p) == 0) {
      i += n;
      return true;
    }
    return false;
  }
  // Le e descarta qualquer valor JSON (so confere a sintaxe). `prof` limita a recursao.
  bool pular(int prof) {
    ws();
    if (i >= t.size()) return false;
    char c = t[i];
    if (c == '"') {
      std::string lixo;
      return texto(lixo);
    }
    if (c == '[' || c == '{') {
      if (prof >= kProfundidadeMaxima) return false;
      const bool objeto = c == '{';
      const char fecho = objeto ? '}' : ']';
      ++i;
      if (consome(fecho)) return true;
      for (;;) {
        if (objeto) {
          std::string chave;
          if (!texto(chave) || !consome(':')) return false;
        }
        if (!pular(prof + 1)) return false;
        if (consome(',')) continue;
        return consome(fecho);
      }
    }
    if (c == 't') return palavra("true");
    if (c == 'f') return palavra("false");
    if (c == 'n') return palavra("null");
    double d;
    return numero(d);
  }
  bool valor(ValorJson& v) {
    ws();
    if (i >= t.size()) return false;
    char c = t[i];
    if (c == '"') {
      v.tipo = ValorJson::Tipo::Texto;
      return texto(v.s);
    }
    if (c == '[') {
      // Tenta como lista de numeros. Se tiver outra coisa ou passar de 1024, e so um valor desconhecido: pula.
      const size_t ini = i;
      ++i;
      v.tipo = ValorJson::Tipo::ArrayNumeros;
      v.arr.clear();
      bool numeros = true;
      if (!consome(']')) {
        for (;;) {
          double d;
          if (!numero(d) || v.arr.size() >= kArrayNumerosMaximo) { numeros = false; break; }
          v.arr.push_back(d);
          if (consome(',')) continue;
          if (!consome(']')) numeros = false;
          break;
        }
      }
      if (numeros) return true;
      i = ini;
      v.arr.clear();
      v.tipo = ValorJson::Tipo::Outro;
      return pular(0);
    }
    if (c == '{') {
      v.tipo = ValorJson::Tipo::Outro;
      return pular(0);
    }
    if (c == 't' && palavra("true")) {
      v.tipo = ValorJson::Tipo::Bool;
      v.b = true;
      return true;
    }
    if (c == 'f' && palavra("false")) {
      v.tipo = ValorJson::Tipo::Bool;
      v.b = false;
      return true;
    }
    if (c == 'n' && palavra("null")) {
      v.tipo = ValorJson::Tipo::Nulo;
      return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      v.tipo = ValorJson::Tipo::Numero;
      return numero(v.n);
    }
    return false;
  }
};

}  // namespace

bool lerObjetoJson(const std::string& texto, ObjetoJson& saida) {
  saida.clear();
  if (texto.size() > kTamanhoMaximoJson) return false;
  if (texto.compare(0, 3, "\xEF\xBB\xBF") == 0) return false;  // BOM
  if (!utf8Valido(texto)) return false;
  LeitorJson l(texto);
  ObjetoJson tmp;
  if (!l.consome('{')) return false;
  if (!l.consome('}')) {
    for (;;) {
      std::string chave;
      if (!l.texto(chave)) return false;
      if (!l.consome(':')) return false;
      ValorJson v;
      if (!l.valor(v)) return false;
      if (!tmp.emplace(std::move(chave), std::move(v)).second) return false;  // chave repetida
      if (l.consome(',')) continue;
      if (!l.consome('}')) return false;
      break;
    }
  }
  l.ws();
  if (l.i != texto.size()) return false;
  saida = std::move(tmp);
  return true;
}

std::string escaparJson(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else {
          o.push_back(static_cast<char>(c));
        }
    }
  }
  return o;
}

const ValorJson* buscarJson(const ObjetoJson& o, const std::string& chave, ValorJson::Tipo tipo) {
  auto it = o.find(chave);
  if (it == o.end() || it->second.tipo != tipo) return nullptr;
  return &it->second;
}

// ---- Handshake ----
bool versaoSuportada(int v) { return v == 1; }

std::string montarPedidoHandshake(const PedidoHandshake& p) {
  std::string s = "{\"v\":" + std::to_string(p.v);
  if (!p.cliente.empty()) s += ",\"cliente\":\"" + escaparJson(p.cliente) + "\"";
  if (!p.preset.empty()) s += ",\"preset\":\"" + escaparJson(p.preset) + "\"";  // ausente = qualidade salva no iPhone
  if (!p.lente.empty()) s += ",\"lente\":\"" + escaparJson(p.lente) + "\"";  // ausente = manter a lente escolhida no iPhone
  if (p.bitrate > 0) s += ",\"bitrate\":" + std::to_string(p.bitrate);  // idem
  s += "}\n";
  return s;
}

ErroHandshake lerPedidoHandshake(const std::string& linhaOrig, PedidoHandshake& saida) {
  std::string linha = linhaOrig;
  if (!linha.empty() && linha.back() == '\r') linha.pop_back();
  if (linha.size() > kLinhaHandshakeMaxima) return ErroHandshake::Handshake;  // o \n nao conta
  ObjetoJson o;
  if (!lerObjetoJson(linha, o)) return ErroHandshake::Handshake;
  const ValorJson* v = buscarJson(o, "v", ValorJson::Tipo::Numero);
  if (!v || v->n != std::floor(v->n)) return ErroHandshake::Handshake;  // ausente ou nao inteiro
  PedidoHandshake p;
  p.preset.clear();  // ausente = qualidade salva no iPhone (preset vazio, bitrate 0)
  p.bitrate = 0;
  p.v = paraIntSat(v->n);  // 0, negativo e enorme tambem chegam aqui: viram "versao"
  if (!versaoSuportada(p.v)) {
    saida = p;
    return ErroHandshake::Versao;
  }
  if (auto* c = buscarJson(o, "cliente", ValorJson::Tipo::Texto)) p.cliente = c->s.substr(0, 128);
  if (auto* x = buscarJson(o, "preset", ValorJson::Tipo::Texto)) {
    p.preset = "1080p30";  // presente mas invalido: padrao
    if (x->s == "1080p30" || x->s == "1080p60" || x->s == "720p30" || x->s == "720p60") p.preset = x->s;
  }
  if (auto* x = buscarJson(o, "lente", ValorJson::Tipo::Texto)) {
    if (x->s == "wide" || x->s == "ultra" || x->s == "tele") p.lente = x->s;
  }
  if (auto* x = buscarJson(o, "bitrate", ValorJson::Tipo::Numero)) {
    p.bitrate = 10000000;  // presente mas invalido: padrao
    if (x->n >= 1000000 && x->n <= 50000000 && x->n == std::floor(x->n)) p.bitrate = static_cast<int64_t>(x->n);
  }
  saida = p;
  return ErroHandshake::Nenhum;
}

std::string montarRespostaHandshake(const RespostaHandshake& r) {
  std::string s;
  if (r.ok) {
    s = "{\"ok\":true,\"v\":" + std::to_string(r.v);
    s += ",\"aparelho\":\"" + escaparJson(r.aparelho) + "\"";
    if (!r.nome.empty()) s += ",\"nome\":\"" + escaparJson(r.nome) + "\"";
    if (!r.id.empty()) s += ",\"id\":\"" + escaparJson(r.id) + "\"";
    s += ",\"w\":" + std::to_string(r.w) + ",\"h\":" + std::to_string(r.h);
    char b[32];
    std::snprintf(b, sizeof b, "%g", r.fps);
    s += std::string(",\"fps\":") + b + ",\"codec\":\"" + escaparJson(r.codec.empty() ? "h264" : r.codec) + "\"}";
  } else {
    s = "{\"ok\":false,\"erro\":\"" + escaparJson(r.erro) + "\"";
    if (r.erro == "versao") {
      s += ",\"v_suportadas\":[";
      for (size_t i = 0; i < r.vSuportadas.size(); ++i) s += (i ? "," : "") + std::to_string(r.vSuportadas[i]);
      s += "]";
    }
    s += "}";
  }
  return s + "\n";
}

bool lerRespostaHandshake(const std::string& linha, RespostaHandshake& saida) {
  if (linha.size() > kLinhaHandshakeMaxima) return false;
  ObjetoJson o;
  if (!lerObjetoJson(linha, o)) return false;
  const ValorJson* ok = buscarJson(o, "ok", ValorJson::Tipo::Bool);
  if (!ok) return false;
  RespostaHandshake r;
  r.ok = ok->b;
  if (auto* x = buscarJson(o, "v", ValorJson::Tipo::Numero)) r.v = paraIntSat(x->n);
  if (auto* x = buscarJson(o, "aparelho", ValorJson::Tipo::Texto)) r.aparelho = x->s;
  if (auto* x = buscarJson(o, "nome", ValorJson::Tipo::Texto)) r.nome = x->s;
  if (auto* x = buscarJson(o, "id", ValorJson::Tipo::Texto)) r.id = x->s;
  if (auto* x = buscarJson(o, "w", ValorJson::Tipo::Numero)) r.w = paraIntSat(x->n);
  if (auto* x = buscarJson(o, "h", ValorJson::Tipo::Numero)) r.h = paraIntSat(x->n);
  if (auto* x = buscarJson(o, "fps", ValorJson::Tipo::Numero)) r.fps = x->n;
  if (auto* x = buscarJson(o, "codec", ValorJson::Tipo::Texto)) r.codec = x->s;
  if (auto* x = buscarJson(o, "erro", ValorJson::Tipo::Texto)) r.erro = x->s;
  if (auto* x = buscarJson(o, "v_suportadas", ValorJson::Tipo::ArrayNumeros))
    for (double d : x->arr) r.vSuportadas.push_back(paraIntSat(d));
  if (!r.ok && r.erro.empty()) return false;
  saida = r;
  return true;
}

// ---- Sincronia ----
namespace {
constexpr uint64_t kIdaEVoltaMaximaUs = 2000000;
constexpr uint64_t kMaiorI64 = static_cast<uint64_t>(INT64_MAX);

// a - b em int64, saturado (sem estouro com sinal, seja qual for o par).
int64_t subSat(uint64_t a, uint64_t b) {
  if (a >= b) {
    uint64_t d = a - b;
    return d > kMaiorI64 ? INT64_MAX : static_cast<int64_t>(d);
  }
  uint64_t d = b - a;
  return d > kMaiorI64 ? INT64_MIN : -static_cast<int64_t>(d);
}
int64_t somaSat(int64_t a, int64_t b) {
  if (b > 0 && a > INT64_MAX - b) return INT64_MAX;
  if (b < 0 && a < INT64_MIN - b) return INT64_MIN;
  return a + b;
}
}  // namespace

void SincroniaRelogio::registrar(uint64_t t0, uint64_t t1, uint64_t ts) {
  if (t0 > t1) return;  // eco no futuro: ida e volta negativa
  const uint64_t ida = t1 - t0;
  if (ida > kIdaEVoltaMaximaUs) return;
  Amostra a;
  a.ida = static_cast<int64_t>(ida);
  // ts - (t0 + t1) / 2 = ts - (t0 + ida / 2), tudo saturado (os relogios nao tem a mesma origem)
  a.dif = subSat(ts, t0 + ida / 2);
  amostras_.push_back(a);
  while (amostras_.size() > kAmostras) amostras_.pop_front();
}

const SincroniaRelogio::Amostra* SincroniaRelogio::melhor() const {
  const Amostra* m = nullptr;
  for (const auto& a : amostras_)
    if (!m || a.ida < m->ida) m = &a;
  return m;
}

int64_t SincroniaRelogio::diferenca() const {
  auto* m = melhor();
  return m ? m->dif : 0;
}
int64_t SincroniaRelogio::idaEVolta() const {
  auto* m = melhor();
  return m ? m->ida : 0;
}
int64_t SincroniaRelogio::atraso(uint64_t chegada, uint64_t tempoQuadro) const {
  // chegada - tempoQuadro + diferenca, saturado
  return somaSat(subSat(chegada, tempoQuadro), diferenca());
}

}  // namespace fmappcam
