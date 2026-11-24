#include "config_fonte.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "usbmux.h"

namespace fmcam {

bool presetValido(const std::string& p) {
  return p == "1080p30" || p == "1080p60" || p == "1080p120" || p == "2160p30" || p == "720p30" || p == "720p60";
}

bool infoPredefinicao(const std::string& p, PredefinicaoInfo& r) {
  if (!presetValido(p)) return false;
  size_t i = p.find('p');
  r.altura = std::atoi(p.substr(0, i).c_str());
  r.fps = std::atof(p.substr(i + 1).c_str());
  r.largura = r.altura * 16 / 9;
  return true;
}
bool lenteValida(const std::string& l) { return l.empty() || l == "wide" || l == "ultra" || l == "tele"; }

bool entregaMenorQuePedido(const std::string& preset, int largura, int altura, double fps) {
  PredefinicaoInfo pedido;
  if (!infoPredefinicao(preset, pedido) || largura <= 0 || altura <= 0) return false;
  return std::min(largura, altura) != pedido.altura || std::abs(pedido.fps - fps) > 0.5;
}

bool aplicarQuedaAgora(const std::string& antes, const std::string& depois, bool conectado, bool jaTeveQuadro) {
  return !conectado && jaTeveQuadro && antes != depois && (depois == "preta" || depois == "imagem");
}

std::vector<int64_t> opcoesDeBitrate(int64_t emUso) {
  std::vector<int64_t> v = {6000000, 10000000, 15000000, 20000000, 30000000};
  if (emUso > 0 && std::find(v.begin(), v.end(), emUso) == v.end()) v.insert(std::upper_bound(v.begin(), v.end(), emUso), emUso);
  return v;
}

const char* chaveTextoQueda(const std::string& aoCair) {
  if (aoCair == "preta") return "Estado.MostrandoPreta";
  if (aoCair == "imagem") return "Estado.MostrandoImagem";
  return "Estado.SegurandoQuadro";
}

ConfigFonte normalizar(ConfigFonte c) {
  if (!presetValido(c.preset)) c.preset.clear();
  if (!lenteValida(c.lente)) c.lente.clear();
  if (c.recepcao != "estavel") c.recepcao.clear();
  if (c.aoCair != "preta" && c.aoCair != "imagem") c.aoCair.clear();
  if (c.decodificacao != "hw" && c.decodificacao != "sw") c.decodificacao.clear();
  c.bitrateKbps = c.bitrateKbps <= 0 ? 0 : std::clamp(c.bitrateKbps, 1000, 50000);
  auto corta = [](std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    s.erase(0, i);
  };
  corta(c.wifiIp);
  corta(c.udid);
  if (c.udid.rfind("wifi:", 0) == 0) {  // escolhido na lista: iPhone do Wi-Fi
    c.wifiId = c.udid.substr(5);
    c.udid.clear();
  }
  return c;
}

int modoHardware(const std::string& d) { return d == "hw" ? 1 : d == "sw" ? 0 : -1; }

ResultadoMigracao migrarValores(ValoresSalvos& v) {
  ResultadoMigracao r;
  if (v.versao < 4 && v.existente && !v.temEncaixar) r.encaixarDesligado = true;
  if (v.versao < 2 && v.temLente && v.lente == "wide") {
    v.lente.clear();
    r.lente = true;
  }
  if (v.versao < 3) {
    if (v.temPreset && v.preset == "1080p30") {
      v.preset.clear();
      r.preset = true;
    }
    if (v.temBitrate && v.bitrateKbps == 10000) {
      v.bitrateKbps = 0;
      r.bitrate = true;
    }
  }
  v.versao = kVersaoConfig;
  return r;
}

bool mudouConexao(const ConfigFonte& a, const ConfigFonte& b) {
  return a.udid != b.udid || a.preset != b.preset || a.lente != b.lente || a.bitrateKbps != b.bitrateKbps ||
         a.wifi != b.wifi || a.wifiId != b.wifiId || (a.wifi && a.wifiIp != b.wifiIp) ||
         a.decodificacao != b.decodificacao || a.recepcao != b.recepcao;
}

std::string caminhoUsbmuxConfigurado() {
  const char* e = std::getenv("FMCAM_USBMUXD");
  if (e && *e) return e;
  return fmappcam::caminhoUsbmuxPadrao();
}

fmappcam::OpcoesConexao montarOpcoes(const ConfigFonte& c, const std::string& usbmuxCaminho) {
  fmappcam::OpcoesConexao op;
  op.usb = !c.wifi;
  op.usbmuxCaminho = usbmuxCaminho;
  op.udid = c.udid;
  op.host = c.wifiIp;
  op.porta = c.wifiPorta;
  op.pedido.v = 1;
  op.pedido.cliente = kNomeCliente;
  op.pedido.preset = c.preset;
  op.pedido.lente = c.lente;
  op.pedido.bitrate = static_cast<int64_t>(c.bitrateKbps) * 1000;  // 0 = nao manda (qualidade do iPhone)
  op.atrasoReconexaoMs = 1000;
  return op;
}

}  // namespace fmcam
