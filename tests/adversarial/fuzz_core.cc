// Ataque ao parser do core (protocolo, JSON, plist, sincronia). Compilar com ASan+UBSan.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include "protocolo.h"
#include "usbmux.h"
using namespace fmappcam;
static std::mt19937_64 rng(12345);
static int falhas = 0;
#define CHECK(c, ...) do { if (!(c)) { ++falhas; std::printf("FALHA %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::vector<uint8_t> aleat(size_t n) { std::vector<uint8_t> v(n); for (auto& b : v) b = rng() & 0xFF; return v; }
static std::string mutar(std::string s) {
  int k = 1 + rng() % 4;
  while (k--) {
    if (s.empty()) { s.push_back(char(rng())); continue; }
    size_t i = rng() % s.size();
    switch (rng() % 4) { case 0: s[i] = char(rng()); break; case 1: s.erase(i, 1 + rng() % 3); break; case 2: s.insert(i, 1, char(rng())); break; default: s.insert(i, s.substr(rng() % s.size(), rng() % 8)); }
  }
  return s;
}

static std::vector<Pacote> parseTudo(const std::vector<uint8_t>& b, size_t passo, bool& erro) {
  ParserPacotes p; std::vector<Pacote> out; Pacote x;
  for (size_t i = 0; i < b.size(); i += passo) { p.alimentar(b.data() + i, std::min(passo, b.size() - i)); while (p.proximo(x)) out.push_back(x); }
  erro = p.erro(); return out;
}

int main() {
  // 1. Parser: fluxo aleatorio cru, fatiado de varias formas, tem que dar o mesmo resultado
  for (int it = 0; it < 20000; ++it) {
    std::vector<uint8_t> b;
    int partes = 1 + rng() % 6;
    for (int k = 0; k < partes; ++k) {
      if (rng() % 3 == 0) { auto r = aleat(rng() % 40); b.insert(b.end(), r.begin(), r.end()); continue; }
      Pacote p; p.tipo = (rng() % 4 == 0) ? uint8_t(rng()) : uint8_t((uint8_t[]){1,2,3,4,9,10}[rng() % 6]);
      p.tempo = rng(); p.dados = aleat(rng() % 300);
      auto s = serializarPacote(p); b.insert(b.end(), s.begin(), s.end());
    }
    bool e1, e2, e3;
    auto a = parseTudo(b, b.size() ? b.size() : 1, e1);
    auto c = parseTudo(b, 1, e2);
    auto d = parseTudo(b, 1 + rng() % 17, e3);
    CHECK(e1 == e2 && e2 == e3, "erro diverge entre fatiamentos it=%d (%d %d %d)", it, e1, e2, e3);
    CHECK(a.size() == c.size() && c.size() == d.size(), "n pacotes diverge it=%d %zu %zu %zu", it, a.size(), c.size(), d.size());
    if (a.size() == c.size()) for (size_t i = 0; i < a.size(); ++i) CHECK(a[i].tipo == c[i].tipo && a[i].tempo == c[i].tempo && a[i].dados == c[i].dados, "pacote %zu diverge", i);
  }
  // 2. limites de tamanho
  for (uint32_t tam : {0u, 1u, kTamanhoMaximoPacote - 1, kTamanhoMaximoPacote, kTamanhoMaximoPacote + 1, 0xFFFFFFFFu}) {
    for (uint8_t tipo : {uint8_t(2), uint8_t(77)}) {
      std::vector<uint8_t> h; h.push_back(tipo); escreverU64BE(h, 5); escreverU32BE(h, tam);
      ParserPacotes p; p.alimentar(h.data(), h.size()); Pacote x; bool got = p.proximo(x);
      std::printf("tipo=%u tam=%u -> got=%d erro=%d\n", tipo, tam, got, p.erro());
      if (tam > kTamanhoMaximoPacote) CHECK(p.erro(), "tam %u deveria dar erro", tam);
      else CHECK(!p.erro(), "tam %u nao deveria dar erro", tam);
    }
  }
  // desconhecido de 8MiB seguido de pacote valido: tem que re-sincronizar
  {
    std::vector<uint8_t> b; b.push_back(77); escreverU64BE(b, 1); escreverU32BE(b, kTamanhoMaximoPacote);
    b.insert(b.end(), kTamanhoMaximoPacote, 0xAB);
    auto v = serializarPacote(criarPing(42)); b.insert(b.end(), v.begin(), v.end());
    bool e; auto r = parseTudo(b, 65536, e);
    CHECK(!e && r.size() == 1 && r[0].tipo == kTipoPing && r[0].tempo == 42, "8MiB desconhecido + ping: n=%zu erro=%d", r.size(), e);
  }
  // 3. JSON / handshake
  const char* sementes[] = {R"({"v":1,"cliente":"x","preset":"1080p30","lente":"wide","bitrate":10000000})",
                            R"({"ok":true,"v":1,"aparelho":"a","nome":"n","id":"i","w":1920,"h":1080,"fps":30,"codec":"h264"})",
                            R"({"ok":false,"erro":"versao","v_suportadas":[1,2]})", R"({"v":1})"};
  for (int it = 0; it < 200000; ++it) {
    std::string s = mutar(sementes[rng() % 4]);
    PedidoHandshake p; lerPedidoHandshake(s, p);
    RespostaHandshake r; lerRespostaHandshake(s, r);
    ObjetoJson o; lerObjetoJson(s, o);
  }
  for (int it = 0; it < 2000; ++it) { std::string s(rng() % 300, 0); for (auto& c : s) c = char(rng()); PedidoHandshake p; lerPedidoHandshake(s, p); RespostaHandshake r; lerRespostaHandshake(s, r); }
  // aninhamento profundo e arrays enormes
  for (int d : {10, 1000, 100000, 1000000}) {
    std::string s = "{\"v\":1,\"x\":" + std::string(d, '[') + std::string(d, ']') + "}"; PedidoHandshake p; auto e = lerPedidoHandshake(s, p); std::printf("aninhamento %d -> %d\n", d, int(e));
    std::string o = "{\"v\":1,\"x\":"; for (int i = 0; i < d; ++i) o += "{\"a\":"; o += "1"; for (int i = 0; i < d; ++i) o += "}"; o += "}"; e = lerPedidoHandshake(o, p); std::printf("objetos aninhados %d -> %d\n", d, int(e));
  }
  { std::string s = "{\"v\":1,\"b\":[" ; for (int i = 0; i < 2000; ++i) s += "1,"; s += "1]}"; PedidoHandshake p; std::printf("array 2001 -> %d\n", int(lerPedidoHandshake(s, p))); }
  // valores extremos
  for (const char* j : {R"({"v":1e400})", R"({"v":-0})", R"({"v":1.0})", R"({"v":1.5})", R"({"v":true})", R"({"v":"1"})", R"({"v":2000000})", R"({"v":-1})", R"({"v":1e15})", R"({"v":1,"bitrate":1e400})", R"({"v":1,"bitrate":50000000.0})", R"({"v":1,"bitrate":50000001})", R"({"v":1,"bitrate":999999})", R"({"v":1,"bitrate":"10000000"})", R"({"v":1,"preset":"4k"})", R"({"v":1,"v":2})", R"({"v":1,"cliente":"\ud800"})", R"({"v":1,"cliente":"\u0000x"})", "{\"v\":1,\"cliente\":\"\xff\xfe\"}", "{\"v\":1}\r", "{\"v\":1} x", "", "{}", "[]", "null"}) {
    PedidoHandshake p; auto e = lerPedidoHandshake(j, p);
    std::printf("handshake %-40s -> erro=%d v=%d bitrate=%lld preset=%s\n", j, int(e), p.v, (long long)p.bitrate, p.preset.c_str());
  }
  // linha de handshake com \0 no meio
  { std::string s("{\"v\":1,\"cliente\":\"a\0b\"}", 24); PedidoHandshake p; std::printf("NUL cru -> %d\n", int(lerPedidoHandshake(s, p))); }
  // tamanho de 4096 exato
  for (size_t n : {4094u, 4095u, 4096u, 4097u}) {
    std::string s = "{\"v\":1,\"cliente\":\""; s += std::string(n - s.size() - 2, 'a'); s += "\"}";
    // s.size()==n
    PedidoHandshake p; std::printf("linha de %zu bytes (sem \\n) -> %d\n", s.size(), int(lerPedidoHandshake(s, p)));
  }
  // 4. plist
  const char* plists[] = {montarPlistListDevices().c_str()};
  (void)plists;
  std::vector<std::string> sem = {montarPlistListDevices(), montarPlistResult(0), montarPlistAttached({3, "UDID", "USB"}), montarPlistDetached(3),
                                  montarPlistDeviceList({{1, "A", "USB"}, {2, "B", "Network"}}), montarPlistConnect(1, 4848)};
  for (int it = 0; it < 200000; ++it) {
    std::string s = mutar(sem[rng() % sem.size()]);
    NoPlist n; lerPlist(s, n); int num; lerResultUsbmux(s, num); std::vector<DispositivoUsb> l; lerListaDispositivos(s, l);
  }
  for (int d : {10, 17, 1000, 200000}) {
    std::string s = "<plist version=\"1.0\">"; for (int i = 0; i < d; ++i) s += "<array>"; for (int i = 0; i < d; ++i) s += "</array>"; s += "</plist>";
    NoPlist n; std::printf("plist profundidade %d -> %d\n", d, int(lerPlist(s, n)));
    std::string t = "<plist version=\"1.0\"><dict>"; for (int i = 0; i < d; ++i) t += "<key>a</key><dict>"; for (int i = 0; i < d; ++i) t += "</dict>"; t += "</dict></plist>";
    std::printf("plist dict profundidade %d -> %d\n", d, int(lerPlist(t, n)));
  }
  { // Result gigante truncando para int
    for (const char* num : {"4294967296", "-4294967296", "9223372036854775808", "99999999999999999999", "0x0", "00", " 0", "0abc", "-0"}) {
      std::string x = std::string("<plist version=\"1.0\"><dict><key>MessageType</key><string>Result</string><key>Number</key><integer>") + num + "</integer></dict></plist>";
      int n = -99; bool ok = lerResultUsbmux(x, n); std::printf("Result Number=%s -> ok=%d n=%d\n", num, ok, n);
    }
  }
  // 5. sincronia com valores hostis
  {
    SincroniaRelogio s; uint64_t ext[] = {0, 1, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 1000000};
    for (uint64_t a : ext) for (uint64_t b : ext) for (uint64_t c : ext) { s.registrar(a, b, c); (void)s.diferenca(); (void)s.atraso(a, b); }
  }
  std::printf("FALHAS=%d\n", falhas);
  return falhas ? 1 : 0;
}
