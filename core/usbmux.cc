#include "usbmux.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>

namespace fmappcam {

namespace {

constexpr uint32_t kMaxMensagem = 1u << 20;

std::string escaparXml(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      default: o.push_back(c);
    }
  }
  return o;
}

std::string desescaparXml(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '&') {
      struct { const char* e; char c; } t[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
      bool achou = false;
      for (auto& x : t) {
        size_t n = std::strlen(x.e);
        if (s.compare(i, n, x.e) == 0) {
          o.push_back(x.c);
          i += n;
          achou = true;
          break;
        }
      }
      if (achou) continue;
    }
    o.push_back(s[i++]);
  }
  return o;
}

std::string envolver(const std::string& corpoDict) {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
         "<plist version=\"1.0\">\n<dict>\n" +
         corpoDict + "</dict>\n</plist>\n";
}
std::string chaveTexto(const char* k, const std::string& v) {
  return std::string("\t<key>") + k + "</key>\n\t<string>" + escaparXml(v) + "</string>\n";
}
std::string chaveInt(const char* k, long long v) {
  return std::string("\t<key>") + k + "</key>\n\t<integer>" + std::to_string(v) + "</integer>\n";
}
std::string identidade() {
  return chaveTexto("ClientVersionString", "fmcam") + chaveTexto("ProgName", kProgNameUsbmux);
}

// ---- leitor de plist ----
struct LeitorPlist {
  const std::string& t;
  size_t i = 0;
  explicit LeitorPlist(const std::string& s) : t(s) {}

  void ws() {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) ++i;
  }
  // Pula declaracao, doctype e comentarios. Para em uma tag de verdade ou no fim.
  void pularCabecalhos() {
    for (;;) {
      ws();
      if (t.compare(i, 2, "<?") == 0) {
        size_t f = t.find("?>", i);
        if (f == std::string::npos) { i = t.size(); return; }
        i = f + 2;
      } else if (t.compare(i, 4, "<!--") == 0) {
        size_t f = t.find("-->", i);
        if (f == std::string::npos) { i = t.size(); return; }
        i = f + 3;
      } else if (t.compare(i, 9, "<!DOCTYPE") == 0) {
        size_t f = t.find('>', i);
        if (f == std::string::npos) { i = t.size(); return; }
        i = f + 1;
      } else {
        return;
      }
    }
  }
  // Le "<nome>" / "<nome/>" / "</nome>". vazia = auto-fechada.
  bool tag(std::string& nome, bool& fecha, bool& vazia) {
    pularCabecalhos();
    if (i >= t.size() || t[i] != '<') return false;
    size_t f = t.find('>', i);
    if (f == std::string::npos) return false;
    std::string c = t.substr(i + 1, f - i - 1);
    i = f + 1;
    fecha = !c.empty() && c[0] == '/';
    if (fecha) c.erase(0, 1);
    vazia = !c.empty() && c.back() == '/';
    if (vazia) c.pop_back();
    size_t sp = c.find_first_of(" \t\r\n");
    nome = sp == std::string::npos ? c : c.substr(0, sp);
    return !nome.empty();
  }
  bool conteudoAte(const std::string& nome, std::string& out) {
    size_t f = t.find("</" + nome + ">", i);
    if (f == std::string::npos) return false;
    out = desescaparXml(t.substr(i, f - i));
    i = f + nome.size() + 3;
    return true;
  }
  bool no(NoPlist& n, int prof) {
    if (prof > 16) return false;
    std::string nome;
    bool fecha, vazia;
    if (!tag(nome, fecha, vazia) || fecha) return false;
    if (nome == "dict") {
      n.tipo = NoPlist::Tipo::Dict;
      if (vazia) return true;
      for (;;) {
        size_t salvo = i;
        std::string nm;
        bool f2, v2;
        if (!tag(nm, f2, v2)) return false;
        if (f2) return nm == "dict";
        if (nm != "key" || v2) { i = salvo; return false; }
        std::string chave;
        if (!conteudoAte("key", chave)) return false;
        NoPlist filho;
        if (!no(filho, prof + 1)) return false;
        n.dict.emplace_back(std::move(chave), std::move(filho));
      }
    }
    if (nome == "array") {
      n.tipo = NoPlist::Tipo::Array;
      if (vazia) return true;
      for (;;) {
        size_t salvo = i;
        std::string nm;
        bool f2, v2;
        if (!tag(nm, f2, v2)) return false;
        if (f2) return nm == "array";
        i = salvo;
        NoPlist filho;
        if (!no(filho, prof + 1)) return false;
        n.itens.push_back(std::move(filho));
      }
    }
    if (nome == "true" || nome == "false") {
      n.tipo = NoPlist::Tipo::Bool;
      n.b = nome == "true";
      n.inteiro = n.b ? 1 : 0;
      return true;
    }
    std::string c;
    if (!vazia && !conteudoAte(nome, c)) return false;
    if (nome == "string") { n.tipo = NoPlist::Tipo::Texto; n.texto = c; return true; }
    if (nome == "integer") {
      n.tipo = NoPlist::Tipo::Inteiro;
      char* fim = nullptr;
      n.inteiro = std::strtoll(c.c_str(), &fim, 10);
      return fim != c.c_str();
    }
    if (nome == "real") { n.tipo = NoPlist::Tipo::Real; n.real = std::strtod(c.c_str(), nullptr); return true; }
    if (nome == "data" || nome == "date") { n.tipo = NoPlist::Tipo::Dados; n.texto = c; return true; }
    return false;
  }
};

}  // namespace

const NoPlist* NoPlist::buscar(const std::string& chave) const {
  for (auto& p : dict)
    if (p.first == chave) return &p.second;
  return nullptr;
}
bool NoPlist::buscarInteiro(const std::string& chave, int64_t& v) const {
  auto* n = buscar(chave);
  if (!n || (n->tipo != Tipo::Inteiro && n->tipo != Tipo::Bool)) return false;
  v = n->inteiro;
  return true;
}
bool NoPlist::buscarTexto(const std::string& chave, std::string& v) const {
  auto* n = buscar(chave);
  if (!n || n->tipo != Tipo::Texto) return false;
  v = n->texto;
  return true;
}

bool lerPlist(const std::string& xml, NoPlist& raiz) {
  LeitorPlist l(xml);
  std::string nome;
  bool fecha, vazia;
  if (!l.tag(nome, fecha, vazia) || fecha || nome != "plist" || vazia) return false;
  NoPlist r;
  if (!l.no(r, 0)) return false;
  raiz = std::move(r);
  return raiz.tipo == NoPlist::Tipo::Dict;
}

std::vector<uint8_t> montarMensagemUsbmux(uint32_t tag, const std::string& xml) {
  std::vector<uint8_t> v;
  auto le32 = [&v](uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
  };
  le32(static_cast<uint32_t>(16 + xml.size()));
  le32(1);  // versao do protocolo (plist)
  le32(8);  // tipo: plist
  le32(tag);
  v.insert(v.end(), xml.begin(), xml.end());
  return v;
}

bool lerCabecalhoUsbmux(const uint8_t* b, uint32_t& tam, uint32_t& ver, uint32_t& tipo, uint32_t& tag) {
  auto le32 = [](const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  };
  tam = le32(b);
  ver = le32(b + 4);
  tipo = le32(b + 8);
  tag = le32(b + 12);
  return tam >= 16 && tam <= kMaxMensagem;
}

uint16_t portaEmOrdemDeRede(uint16_t p) { return static_cast<uint16_t>(((p & 0xFF) << 8) | (p >> 8)); }

std::string montarPlistListDevices() {
  return envolver(identidade() + chaveTexto("MessageType", "ListDevices"));
}
std::string montarPlistListen() {
  return envolver(identidade() + chaveTexto("MessageType", "Listen") + chaveInt("kLibUSBMuxVersion", 3));
}
std::string montarPlistConnect(int id, uint16_t porta) {
  return envolver(identidade() + chaveTexto("MessageType", "Connect") + chaveInt("DeviceID", id) +
                  chaveInt("PortNumber", portaEmOrdemDeRede(porta)));
}
std::string montarPlistResult(int n) {
  return envolver(chaveTexto("MessageType", "Result") + chaveInt("Number", n));
}

bool operator==(const DispositivoUsb& a, const DispositivoUsb& b) {
  return a.id == b.id && a.serial == b.serial && a.conexao == b.conexao;
}

namespace {
std::string propriedades(const DispositivoUsb& d) {
  return "\t<key>Properties</key>\n\t<dict>\n\t\t<key>ConnectionType</key>\n\t\t<string>" + escaparXml(d.conexao) +
         "</string>\n\t\t<key>DeviceID</key>\n\t\t<integer>" + std::to_string(d.id) +
         "</integer>\n\t\t<key>SerialNumber</key>\n\t\t<string>" + escaparXml(d.serial) + "</string>\n\t</dict>\n";
}
}  // namespace

std::string montarPlistDeviceList(const std::vector<DispositivoUsb>& lista) {
  std::string s = "\t<key>DeviceList</key>\n\t<array>\n";
  for (auto& d : lista) {
    s += "\t<dict>\n\t\t<key>DeviceID</key>\n\t\t<integer>" + std::to_string(d.id) + "</integer>\n";
    s += "\t\t<key>MessageType</key>\n\t\t<string>Attached</string>\n";
    s += "\t\t<key>Properties</key>\n\t\t<dict>\n\t\t\t<key>ConnectionType</key>\n\t\t\t<string>" +
         escaparXml(d.conexao) + "</string>\n\t\t\t<key>DeviceID</key>\n\t\t\t<integer>" + std::to_string(d.id) +
         "</integer>\n\t\t\t<key>SerialNumber</key>\n\t\t\t<string>" + escaparXml(d.serial) +
         "</string>\n\t\t</dict>\n\t</dict>\n";
  }
  s += "\t</array>\n";
  return envolver(s);
}
std::string montarPlistAttached(const DispositivoUsb& d) {
  return envolver(chaveTexto("MessageType", "Attached") + chaveInt("DeviceID", d.id) + propriedades(d));
}
std::string montarPlistDetached(int id) {
  return envolver(chaveTexto("MessageType", "Detached") + chaveInt("DeviceID", id));
}

bool lerResultUsbmux(const std::string& xml, int& numero) {
  NoPlist r;
  if (!lerPlist(xml, r)) return false;
  std::string tipo;
  int64_t n;
  if (!r.buscarTexto("MessageType", tipo) || tipo != "Result" || !r.buscarInteiro("Number", n)) return false;
  if (n < INT_MIN || n > INT_MAX) return false;  // nao truncar: 4294967296 nao pode virar 0 (= sucesso)
  numero = static_cast<int>(n);
  return true;
}

namespace {
bool lerDispositivo(const NoPlist& d, DispositivoUsb& out) {
  int64_t id;
  if (!d.buscarInteiro("DeviceID", id)) return false;
  if (id < INT_MIN || id > INT_MAX) return false;
  out.id = static_cast<int>(id);
  const NoPlist* p = d.buscar("Properties");
  if (!p || p->tipo != NoPlist::Tipo::Dict) return false;
  p->buscarTexto("SerialNumber", out.serial);
  if (!p->buscarTexto("ConnectionType", out.conexao)) out.conexao.clear();
  return true;
}
}  // namespace

bool lerListaDispositivos(const std::string& xml, std::vector<DispositivoUsb>& saida, bool soUsb) {
  NoPlist r;
  if (!lerPlist(xml, r)) return false;
  const NoPlist* lista = r.buscar("DeviceList");
  if (!lista || lista->tipo != NoPlist::Tipo::Array) return false;
  std::vector<DispositivoUsb> v;
  for (auto& d : lista->itens) {
    DispositivoUsb x;
    if (d.tipo != NoPlist::Tipo::Dict || !lerDispositivo(d, x)) continue;
    if (soUsb && x.conexao != "USB") continue;
    v.push_back(x);
  }
  saida = std::move(v);
  return true;
}

std::string caminhoUsbmuxPadrao() {
#ifdef _WIN32
  return "127.0.0.1:27015";
#else
  return "/var/run/usbmuxd";
#endif
}

Soquete abrirUsbmux(const std::string& caminho, std::string& erro) {
#ifdef _WIN32
  size_t dp = caminho.rfind(':');
  if (dp == std::string::npos) {
    erro = "endereco do usbmuxd precisa ser host:porta";
    return kSoqueteInvalido;
  }
  return conectarTcp(caminho.substr(0, dp), static_cast<uint16_t>(std::atoi(caminho.c_str() + dp + 1)), 3000, erro);
#else
  return conectarUnix(caminho, erro);
#endif
}

bool enviarMensagemUsbmux(Soquete s, uint32_t tag, const std::string& xml) {
  auto m = montarMensagemUsbmux(tag, xml);
  return enviarTudo(s, m.data(), m.size());
}

bool lerMensagemUsbmux(Soquete s, std::string& xml, uint32_t* tagOut, int timeoutMs, std::string& erro) {
  uint8_t h[16];
  ResultadoIO r = lerExato(s, h, sizeof h, timeoutMs);
  if (r != ResultadoIO::Ok) {
    erro = r == ResultadoIO::Timeout ? "usbmuxd nao respondeu" : "usbmuxd fechou a conexao";
    return false;
  }
  uint32_t tam, ver, tipo, tag;
  if (!lerCabecalhoUsbmux(h, tam, ver, tipo, tag) || tipo != 8) {
    erro = "mensagem invalida do usbmuxd";
    return false;
  }
  xml.assign(tam - 16, '\0');
  if (tam > 16) {
    r = lerExato(s, reinterpret_cast<uint8_t*>(&xml[0]), tam - 16, timeoutMs);
    if (r != ResultadoIO::Ok) {
      erro = "mensagem do usbmuxd cortada";
      return false;
    }
  }
  if (tagOut) *tagOut = tag;
  return true;
}

bool listarDispositivosUsb(const std::string& caminho, std::vector<DispositivoUsb>& saida, std::string& erro) {
  Soquete s = abrirUsbmux(caminho, erro);
  if (s == kSoqueteInvalido) return false;
  std::string xml;
  bool ok = enviarMensagemUsbmux(s, 1, montarPlistListDevices()) && lerMensagemUsbmux(s, xml, nullptr, 3000, erro);
  fecharSoquete(s);
  if (!ok) {
    if (erro.empty()) erro = "falha ao falar com o usbmuxd";
    return false;
  }
  if (!lerListaDispositivos(xml, saida, true)) {
    erro = "resposta ListDevices invalida";
    return false;
  }
  return true;
}

bool listarDispositivos(const std::string& caminho, std::vector<DispositivoUsb>& saida, std::string& erro) {
  Soquete s = abrirUsbmux(caminho, erro);
  if (s == kSoqueteInvalido) return false;
  std::string xml;
  bool ok = enviarMensagemUsbmux(s, 1, montarPlistListDevices()) && lerMensagemUsbmux(s, xml, nullptr, 3000, erro);
  fecharSoquete(s);
  if (!ok) {
    if (erro.empty()) erro = "falha ao falar com o usbmuxd";
    return false;
  }
  if (!lerListaDispositivos(xml, saida, false)) {
    erro = "resposta ListDevices invalida";
    return false;
  }
  return true;
}

bool escolherDispositivo(const std::vector<DispositivoUsb>& lista, const std::string& udid, DispositivoUsb& saida) {
  for (auto& d : lista)
    if (d.conexao == "USB" && (udid.empty() || d.serial == udid)) {
      saida = d;
      return true;
    }
  if (udid.empty()) return false;
  for (auto& d : lista)
    if (d.conexao == "Network" && d.serial == udid) {
      saida = d;
      return true;
    }
  return false;
}

Soquete conectarDispositivoUsb(const std::string& caminho, int deviceId, uint16_t porta, std::string& erro,
                               int timeoutMs) {
  Soquete s = abrirUsbmux(caminho, erro);
  if (s == kSoqueteInvalido) return kSoqueteInvalido;
  std::string xml;
  if (!enviarMensagemUsbmux(s, 2, montarPlistConnect(deviceId, porta)) ||
      !lerMensagemUsbmux(s, xml, nullptr, timeoutMs, erro)) {
    if (erro.empty()) erro = "falha ao pedir Connect";
    fecharSoquete(s);
    return kSoqueteInvalido;
  }
  int n = -1;
  if (!lerResultUsbmux(xml, n)) {
    erro = "resposta Connect invalida";
    fecharSoquete(s);
    return kSoqueteInvalido;
  }
  if (n != 0) {
    erro = n == 3 ? "aparelho recusou a conexao (app OBS Cam Mobile aberto no iPhone?)"
                  : "usbmuxd recusou o Connect (codigo " + std::to_string(n) + ")";
    fecharSoquete(s);
    return kSoqueteInvalido;
  }
  return s;
}

Soquete conectarUsbPorUdid(const std::string& caminho, const std::string& udid, uint16_t porta, std::string& erro) {
  std::vector<DispositivoUsb> lista;
  if (!listarDispositivosUsb(caminho, lista, erro)) return kSoqueteInvalido;
  for (auto& d : lista)
    if (d.serial == udid) return conectarDispositivoUsb(caminho, d.id, porta, erro);
  erro = "aparelho " + udid + " nao esta ligado no cabo";
  return kSoqueteInvalido;
}

// ---- observador (Listen) ----
void ObservadorUsbmux::iniciar(AoMudar cb) {
  if (th_.joinable()) return;
  aoMudar_ = std::move(cb);
  parar_ = false;
  th_ = std::thread([this] { laco(); });
}

void ObservadorUsbmux::parar() {
  parar_ = true;
  {
    std::lock_guard<std::mutex> l(m_);
    if (sock_ != kSoqueteInvalido) desligarSoquete(sock_);
  }
  if (th_.joinable()) th_.join();
}

std::vector<DispositivoUsb> ObservadorUsbmux::dispositivos() const {
  std::lock_guard<std::mutex> l(m_);
  std::vector<DispositivoUsb> r;
  for (auto& d : lista_)
    if (d.conexao == "USB") r.push_back(d);
  return r;
}

std::vector<DispositivoUsb> ObservadorUsbmux::todos() const {
  std::lock_guard<std::mutex> l(m_);
  return lista_;
}

void ObservadorUsbmux::laco() {
  definirCancelamentoDaThread(&parar_);  // parar() acorda connect e leituras em ate ~10 ms
  while (!parar_) {
    std::string erro;
    Soquete s = abrirUsbmux(caminho_, erro);
    if (s != kSoqueteInvalido) {
      {
        std::lock_guard<std::mutex> l(m_);
        sock_ = s;
      }
      std::string xml;
      int n = -1;
      if (enviarMensagemUsbmux(s, 1, montarPlistListen()) && lerMensagemUsbmux(s, xml, nullptr, 3000, erro) &&
          lerResultUsbmux(xml, n) && n == 0) {
        ouvindo_ = true;
        while (!parar_) {
          if (esperarLeitura(s, 200) == 0) continue;
          if (!lerMensagemUsbmux(s, xml, nullptr, 3000, erro)) break;
          NoPlist r;
          std::string tipo;
          if (!lerPlist(xml, r) || !r.buscarTexto("MessageType", tipo)) continue;
          bool mudou = false;
          {
            std::lock_guard<std::mutex> l(m_);
            if (tipo == "Attached") {
              DispositivoUsb d;
              if (lerDispositivo(r, d)) {  // USB e Network: a Network e o que sobra quando o macOS apaga a USB
                auto it = std::find_if(lista_.begin(), lista_.end(), [&](auto& x) { return x.id == d.id; });
                if (it == lista_.end()) lista_.push_back(d); else *it = d;
                mudou = true;
              }
            } else if (tipo == "Detached") {
              int64_t id;
              if (r.buscarInteiro("DeviceID", id)) {
                auto it = std::remove_if(lista_.begin(), lista_.end(), [&](auto& x) { return x.id == id; });
                if (it != lista_.end()) {
                  lista_.erase(it, lista_.end());
                  mudou = true;
                }
              }
            }
          }
          if (mudou && aoMudar_) aoMudar_();
        }
        ouvindo_ = false;
      }
      {
        std::lock_guard<std::mutex> l(m_);
        sock_ = kSoqueteInvalido;
        lista_.clear();  // sem usbmuxd nao sabemos de ninguem
      }
      fecharSoquete(s);
      if (aoMudar_ && !parar_) aoMudar_();
    }
    for (int i = 0; i < 10 && !parar_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

}  // namespace fmappcam
