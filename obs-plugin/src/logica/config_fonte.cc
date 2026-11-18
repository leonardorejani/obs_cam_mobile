#include "config_fonte.h"

#include <algorithm>
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

ConfigFonte normalizar(ConfigFonte c) {
  if (!presetValido(c.preset)) c.preset.clear();
  if (!lenteValida(c.lente)) c.lente.clear();
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

ResultadoMigracao migrarValores(ValoresSalvos& v) {
  ResultadoMigracao r;
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
         a.wifi != b.wifi || a.wifiId != b.wifiId || (a.wifi && a.wifiIp != b.wifiIp);
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
