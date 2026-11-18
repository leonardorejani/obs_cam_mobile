// Fonte de video assincrona "OBS Cam Mobile Extension" (id fmcam_fonte). Cola do libobs: toda a logica de conexao,
// estado e escolha do aparelho esta em src/logica.
#include <obs-module.h>
#include <util/base.h>
#include <util/platform.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>

#include "ajuste_itens.h"
#include "config_fonte.h"
#include "modelo_iphone.h"
#include "modulo.h"
#include "protocolo.h"
#include "sessao.h"

namespace fmcam {
namespace {

#define T(s) obs_module_text(s)

struct Fonte {
  obs_source_t* src = nullptr;
  std::mutex m;
  ConfigFonte cfg;
  std::unique_ptr<SessaoFonte> sessao;
  EstadoSessao ultimo;
  std::string ultimoLog;
  int largura = 0, altura = 0;  // do ultimo quadro entregue (so a thread da conexao mexe)
};

bool tamanhoCanvas(int& w, int& h) {
  obs_video_info ovi;
  if (obs_get_video_info(&ovi)) {
    w = static_cast<int>(ovi.base_width);
    h = static_cast<int>(ovi.base_height);
    return w > 0 && h > 0;
  }
  // Sem video iniciado (teste sem janela): FMCAM_CANVAS=1920x1080
  if (const char* e = std::getenv("FMCAM_CANVAS")) return std::sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0;
  return false;
}

void aplicarBounds(obs_sceneitem_t* it, const AjusteBounds& a) {
  vec2 pos = {static_cast<float>(a.posX), static_cast<float>(a.posY)};
  vec2 b = {static_cast<float>(a.boundsLargura), static_cast<float>(a.boundsAltura)};
  obs_sceneitem_set_alignment(it, OBS_ALIGN_CENTER);
  obs_sceneitem_set_bounds_type(it, OBS_BOUNDS_SCALE_INNER);
  obs_sceneitem_set_bounds_alignment(it, OBS_ALIGN_CENTER);
  obs_sceneitem_set_bounds(it, &b);
  obs_sceneitem_set_pos(it, &pos);
}

struct CtxAjuste {
  obs_source_t* src;
  int antesL, antesA, canvasL, canvasA;
  int ajustados = 0;
};

bool aoItem(obs_scene_t*, obs_sceneitem_t* it, void* p) {
  auto* c = static_cast<CtxAjuste*>(p);
  if (obs_sceneitem_get_source(it) != c->src) return true;
  TransformacaoItem t;
  vec2 v;
  obs_sceneitem_get_pos(it, &v);
  t.posX = v.x;
  t.posY = v.y;
  obs_sceneitem_get_scale(it, &v);
  t.escalaX = v.x;
  t.escalaY = v.y;
  t.rotacao = obs_sceneitem_get_rot(it);
  t.temBounds = obs_sceneitem_get_bounds_type(it) != OBS_BOUNDS_NONE;
  t.alinhadoTopoEsquerda = obs_sceneitem_get_alignment(it) == (OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
  obs_sceneitem_crop cr;
  obs_sceneitem_get_crop(it, &cr);
  t.recortado = cr.left || cr.right || cr.top || cr.bottom;
  AjusteBounds a = decidirAjuste(t, c->antesL, c->antesA, c->canvasL, c->canvasA);
  if (a.aplicar) {
    aplicarBounds(it, a);
    ++c->ajustados;
  }
  return true;
}

bool aoCena(void* p, obs_source_t* cena) {
  obs_scene_enum_items(obs_scene_from_source(cena), aoItem, p);
  return true;
}

// Chamado ANTES de entregar um quadro de tamanho novo: itens que enchiam o canvas com o tamanho antigo passam a
// ter bounds do canvas, entao a troca de predefinicao nao encolhe a imagem no canto.
void acompanharResolucao(Fonte* f, int w, int h) {
  if (w == f->largura && h == f->altura) return;
  int antesL = f->largura ? f->largura : w, antesA = f->altura ? f->altura : h;
  f->largura = w;
  f->altura = h;
  CtxAjuste c{f->src, antesL, antesA, 0, 0};
  if (!tamanhoCanvas(c.canvasL, c.canvasA)) return;
  obs_enum_scenes(aoCena, &c);
  if (c.ajustados)
    blog(LOG_INFO, "[fmcam] '%s': %dx%d, %d item(ns) de cena que enchiam o canvas agora usam bounds do canvas",
         obs_source_get_name(f->src), w, h, c.ajustados);
}

ConfigFonte lerConfig(obs_data_t* d) {
  ConfigFonte c;
  c.udid = obs_data_get_string(d, "udid");
  c.nomeCache = obs_data_get_string(d, "nome_cache");
  c.preset = obs_data_get_string(d, "predefinicao");
  c.lente = obs_data_get_string(d, "lente");
  c.bitrateKbps = static_cast<int>(obs_data_get_int(d, "bitrate"));
  c.wifi = obs_data_get_bool(d, "wifi");
  c.wifiIp = obs_data_get_string(d, "wifi_ip");
  return normalizar(c);
}

const char* textoEstado(EstadoAparelho e) {
  switch (e) {
    case EstadoAparelho::SemFonte: return T("Estado.SemFonte");
    case EstadoAparelho::Procurando: return T("Estado.Procurando");
    case EstadoAparelho::Conectado: return T("Estado.Conectado");
    case EstadoAparelho::SemApp: return T("Estado.SemApp");
    case EstadoAparelho::Ocupado: return T("Estado.Ocupado");
    case EstadoAparelho::SemCabo: return T("Estado.SemCabo");
    case EstadoAparelho::Falha: return T("Estado.Falha");
  }
  return "";
}

void entregarQuadro(Fonte* f, const QuadroSaida& q) {
  acompanharResolucao(f, q.largura, q.altura);
  obs_source_frame fr = {};
  fr.width = static_cast<uint32_t>(q.largura);
  fr.height = static_cast<uint32_t>(q.altura);
  fr.format = q.formato == QuadroSaida::Formato::NV12 ? VIDEO_FORMAT_NV12 : VIDEO_FORMAT_I420;
  fr.full_range = q.faixaCompleta;
  for (int i = 0; i < 3; ++i) {
    fr.data[i] = const_cast<uint8_t*>(q.plano[i]);
    fr.linesize[i] = static_cast<uint32_t>(q.passo[i]);
  }
  fr.timestamp = os_gettime_ns();
  video_colorspace cs = q.matriz == QuadroSaida::Matriz::BT601 ? VIDEO_CS_601 : VIDEO_CS_709;
  video_format_get_parameters_for_format(cs, q.faixaCompleta ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL, fr.format,
                                         fr.color_matrix, fr.color_range_min, fr.color_range_max);
  obs_source_output_video(f->src, &fr);  // copia os planos: o AVFrame pode ser reaproveitado ja
}

// "iPhone 15 Pro Max" (ou "... (2)" se houver dois do mesmo modelo). Sem registro, so o nome comercial.
std::string rotuloIphone(const std::string& valor, const std::string& modelo) {
  if (auto* reg = registroGlobal()) {
    std::string r = reg->rotulo(valor);
    if (!r.empty()) return r;
  }
  std::string n = nomeComercialIphone(modelo);
  return n.empty() ? "iPhone" : n;
}

// nome_cache de versoes antigas guardava "Nome da camera (iPhone16,2)": vira o nome comercial do modelo.
std::string rotuloDoCache(const std::string& c) {
  if (c.empty()) return "iPhone";
  size_t a = c.rfind(" (");
  if (a != std::string::npos && c.back() == ')') {
    std::string interno = c.substr(a + 2, c.size() - a - 3);
    if (interno.find(',') != std::string::npos) return nomeComercialIphone(interno);
  }
  return c;
}

void aoEstado(Fonte* f, const EstadoSessao& e) {
  std::string nomeFonte = obs_source_get_name(f->src) ? obs_source_get_name(f->src) : "?";
  // mesmo rotulo da lista e do painel: "iPhone 15 Pro Max" (modelo comercial), sem o nome da camera nem o codigo
  std::string rot = e.nome.empty() && e.modelo.empty() ? e.udid : rotuloIphone(e.udid, e.modelo);
  std::string linha = std::string(chaveEstado(e.estado)) + "|" + e.mensagem + "|" + rot;
  bool novo;
  {
    std::lock_guard<std::mutex> l(f->m);
    f->ultimo = e;
    novo = linha != f->ultimoLog;
    f->ultimoLog = linha;
    // Primeira conexao com a config sem aparelho escolhido (fonte nova ou antiga): fixa o iPhone que ligou, e a lista passa
    // a mostrar esse mesmo iPhone (nao existe mais a opcao "primeiro iPhone no cabo").
    if (e.estado == EstadoAparelho::Conectado && !e.udid.empty() && !f->cfg.wifi && f->cfg.wifiId.empty()) {
      obs_data_t* s = obs_source_get_settings(f->src);
      if (f->cfg.udid.empty()) {
        f->cfg.udid = e.udid;
        obs_data_set_string(s, "udid", e.udid.c_str());
      }
      std::string rotulo = e.nome.empty() && e.modelo.empty() ? "" : rot;
      if (!rotulo.empty() && rotulo != f->cfg.nomeCache) {
        f->cfg.nomeCache = rotulo;
        obs_data_set_string(s, "nome_cache", rotulo.c_str());
      }
      obs_data_release(s);
    }
  }
  if (novo) {
    int nivel = (e.estado == EstadoAparelho::Conectado || e.estado == EstadoAparelho::Procurando) ? LOG_INFO : LOG_WARNING;
    if (e.estado == EstadoAparelho::Conectado)
      blog(nivel, "[fmcam] '%s': conectado a %s (%dx%d)", nomeFonte.c_str(), rot.c_str(), e.largura, e.altura);
    else if (e.jaTeveQuadro)
      blog(nivel, "[fmcam] '%s': %s, segurando o ultimo quadro (%s)", nomeFonte.c_str(), chaveEstado(e.estado), e.mensagem.c_str());
    else
      blog(nivel, "[fmcam] '%s': %s (%s)", nomeFonte.c_str(), chaveEstado(e.estado), e.mensagem.c_str());
  }
  if (novo) recarregarPropriedades();
}

// "activate"/"deactivate" do libobs: a fonte entrou ou saiu do Programa. Em modo estudio a Previa NAO ativa,
// so o Programa. Roda em thread qualquer do OBS: so repassa o tally para a sessao.
void aoAtivar(void* d, calldata_t*) {
  auto* f = static_cast<Fonte*>(d);
  std::lock_guard<std::mutex> l(f->m);
  if (f->sessao) f->sessao->definirNoAr(true);
}
void aoDesativar(void* d, calldata_t*) {
  auto* f = static_cast<Fonte*>(d);
  std::lock_guard<std::mutex> l(f->m);
  if (f->sessao) f->sessao->definirNoAr(false);
}

void iniciarSessao(Fonte* f) {
  auto* reg = registroGlobal();
  if (!reg) return;
  auto s = std::make_unique<SessaoFonte>(f->cfg, reg->caminhoUsbmux(), reg);
  s->definirCallbacks([f](const QuadroSaida& q) { entregarQuadro(f, q); }, [f](const EstadoSessao& e) { aoEstado(f, e); });
  f->sessao = std::move(s);
  f->sessao->iniciar();
  f->sessao->definirNoAr(obs_source_active(f->src));  // sessao nova (troca de predefinicao) herda o estado do Programa
}

// Fecha a sessao velha SEM esperar: Conexao::parar() pode bloquear por segundos (connect lento, usbmuxd mudo) e
// esta funcao roda na thread da interface do OBS. A parada acontece numa thread de fundo (SessaoFonte::parar(0)).
void pararSessao(Fonte* f) {
  std::unique_ptr<SessaoFonte> s;
  {
    std::lock_guard<std::mutex> l(f->m);
    s = std::move(f->sessao);
  }
  if (s) s->parar(0);
}

const char* nomeFonteTipo(void*) { return T("FonteNome"); }

void estadoProc(void* dados, calldata_t* cd) {
  auto* f = static_cast<Fonte*>(dados);
  EstadoSessao e;
  {
    std::lock_guard<std::mutex> l(f->m);
    if (f->sessao) e = f->sessao->estado();
  }
  char b[768];
  std::snprintf(b, sizeof b,
                "{\"estado\":\"%s\",\"udid\":\"%s\",\"nome\":\"%s\",\"modelo\":\"%s\",\"quadros\":%llu,\"fps\":%.1f,"
                "\"w\":%d,\"h\":%d,\"reconexoes\":%d,\"erros\":%llu,\"noar\":%s}",
                chaveEstado(e.estado), fmappcam::escaparJson(e.udid).c_str(), fmappcam::escaparJson(e.nome).c_str(),
                fmappcam::escaparJson(e.modelo).c_str(), static_cast<unsigned long long>(e.quadros), e.fps, e.largura,
                e.altura, e.reconexoes, static_cast<unsigned long long>(e.errosDecodificacao), e.noAr ? "true" : "false");
  calldata_set_string(cd, "json", b);
}

// Fontes salvas antes desta versao guardam "wide", 1080p30 e 10000 kbps so porque eram os padroes antigos, e isso
// sobrescrevia a qualidade e a lente que o operador escolheu no iPhone a cada reconexao. Nao da para saber se foi
// escolha, entao o valor igual ao padrao antigo vira "do iPhone". Qualquer outro valor salvo e respeitado.
void migrarConfig(obs_data_t* settings, obs_source_t* src) {
  ValoresSalvos v;
  v.versao = static_cast<int>(obs_data_get_int(settings, "versao_cfg"));
  if (v.versao >= kVersaoConfig) return;
  v.temLente = obs_data_has_user_value(settings, "lente");
  v.lente = obs_data_get_string(settings, "lente");
  v.temPreset = obs_data_has_user_value(settings, "predefinicao");
  v.preset = obs_data_get_string(settings, "predefinicao");
  v.temBitrate = obs_data_has_user_value(settings, "bitrate");
  v.bitrateKbps = static_cast<int>(obs_data_get_int(settings, "bitrate"));
  ResultadoMigracao r = migrarValores(v);
  const char* nome = obs_source_get_name(src);
  if (r.lente) {
    obs_data_set_string(settings, "lente", "");
    blog(LOG_INFO, "[fmcam] '%s': lente 'wide' salva era so o padrao antigo, migrada para 'Manter a do iPhone'", nome);
  }
  if (r.preset) {
    obs_data_set_string(settings, "predefinicao", "");
    blog(LOG_INFO, "[fmcam] '%s': predefinicao 1080p30 salva era so o padrao antigo, migrada para 'Definida no iPhone'", nome);
  }
  if (r.bitrate) {
    obs_data_set_int(settings, "bitrate", 0);
    blog(LOG_INFO, "[fmcam] '%s': bitrate 10000 kbps salvo era so o padrao antigo, migrado para 'Definido no iPhone'", nome);
  }
  obs_data_set_int(settings, "versao_cfg", v.versao);
}

void* criar(obs_data_t* settings, obs_source_t* src) {
  migrarConfig(settings, src);
  auto* f = new Fonte();
  f->src = src;
  f->cfg = lerConfig(settings);
  obs_source_set_async_unbuffered(src, true);
  obs_source_set_async_decoupled(src, false);
  proc_handler_add(obs_source_get_proc_handler(src), "void fmcam_estado(out string json)", estadoProc, f);
  signal_handler_t* sh = obs_source_get_signal_handler(src);
  signal_handler_connect(sh, "activate", aoAtivar, f);
  signal_handler_connect(sh, "deactivate", aoDesativar, f);
  fonteViva(src, true);
  {
    std::lock_guard<std::mutex> l(f->m);
    iniciarSessao(f);
  }
  return f;
}

void destruir(void* dados) {
  auto* f = static_cast<Fonte*>(dados);
  fonteViva(f->src, false);
  signal_handler_t* sh = obs_source_get_signal_handler(f->src);
  signal_handler_disconnect(sh, "activate", aoAtivar, f);
  signal_handler_disconnect(sh, "deactivate", aoDesativar, f);
  std::string nome = obs_source_get_name(f->src) ? obs_source_get_name(f->src) : "?";
  pararSessao(f);
  delete f;
  blog(LOG_INFO, "[fmcam] fonte removida: '%s'", nome.c_str());
}

void atualizar(void* dados, obs_data_t* settings) {
  auto* f = static_cast<Fonte*>(dados);
  ConfigFonte nova = lerConfig(settings);
  std::unique_ptr<SessaoFonte> velha;
  {
    std::lock_guard<std::mutex> l(f->m);
    bool reconectar = mudouConexao(f->cfg, nova) || !f->sessao;
    f->cfg = nova;
    if (!reconectar) return;
    velha = std::move(f->sessao);
    f->ultimoLog.clear();
    iniciarSessao(f);  // a nova ja nasce; se o iPhone ainda estiver fechando a velha, ela tenta de novo sozinha
  }
  if (velha) velha->parar(0);
}

void padroes(obs_data_t* d) {
  obs_data_set_default_string(d, "udid", "");
  obs_data_set_default_string(d, "predefinicao", "");  // definida no iPhone
  obs_data_set_default_string(d, "lente", "");  // manter a lente do iPhone
  obs_data_set_default_int(d, "bitrate", 0);  // definido no iPhone
  obs_data_set_default_bool(d, "wifi", false);
}

bool botaoAtualizar(obs_properties_t*, obs_property_t*, void*) { return true; }

obs_properties_t* propriedades(void* dados) {
  auto* f = static_cast<Fonte*>(dados);
  ConfigFonte cfg;
  EstadoSessao est;
  {
    std::lock_guard<std::mutex> l(f->m);
    cfg = f->cfg;
    if (f->sessao) est = f->sessao->estado();
  }
  obs_properties_t* p = obs_properties_create();
  obs_properties_set_flags(p, OBS_PROPERTIES_DEFER_UPDATE);

  obs_property_t* info = obs_properties_add_text(p, "estado", "", OBS_TEXT_INFO);
  std::string txt = std::string(T("Estado")) + ": " + (est.estado == EstadoAparelho::Conectado && est.pelaRede ? T("Estado.ConectadoPelaRede") : textoEstado(est.estado));
  if (est.estado == EstadoAparelho::Conectado) {
    // o que o iPhone realmente entrega (pode cair para um formato menor que o pedido)
    char b[96];
    std::snprintf(b, sizeof b, " - %s %dx%d @ %g fps", T("Estado.Entrega"), est.respLargura, est.respAltura, est.respFps);
    txt += b;
    PredefinicaoInfo pedido;
    if (infoPredefinicao(cfg.preset, pedido) &&
        (pedido.altura != est.respAltura || std::abs(pedido.fps - est.respFps) > 0.5))
      txt += " (" + std::string(T("Estado.Pedido")) + " " + cfg.preset + ")";
    if (est.noAr) txt += " - " + std::string(T("Estado.NoAr"));
  }
  else if (est.jaTeveQuadro)
    txt += " - " + std::string(T("Estado.SegurandoQuadro"));
  obs_property_set_description(info, txt.c_str());
  obs_property_text_set_info_type(info, est.estado == EstadoAparelho::Conectado ? OBS_TEXT_INFO_NORMAL
                                        : (est.estado == EstadoAparelho::Procurando || est.estado == EstadoAparelho::SemFonte)
                                            ? OBS_TEXT_INFO_NORMAL : OBS_TEXT_INFO_WARNING);

  obs_property_t* lista = obs_properties_add_list(p, "udid", T("Aparelho"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  const std::string sel = cfg.wifiId.empty() ? cfg.udid : "wifi:" + cfg.wifiId;
  bool achou = sel.empty();
  // Duas opcoes por iPhone: "<modelo comercial> · Cabo" e "<modelo comercial> · Wi-Fi".
  if (auto* reg = registroGlobal()) {
    std::set<std::string> vistos;
    auto emUsoPorOutra = [&](const std::string& valor) {
      InfoAparelho x;
      return reg->buscar(valor, x) && x.fontes > 0 && valor != est.udid;
    };
    auto item = [&](const std::string& valor, const std::string& base, bool cabo) {
      std::string rot = base + " \xC2\xB7 " + T(cabo ? "Aparelho.Cabo" : "Aparelho.Wifi");
      if (emUsoPorOutra(valor)) rot += std::string(" - ") + T("Aparelho.EmUso");
      if (valor == sel) achou = true;
      vistos.insert(valor);
      obs_property_list_add_string(lista, rot.c_str(), valor.c_str());
    };
    auto servicos = reg->servicosWifi();
    for (auto& i : reg->listar()) {
      std::string base = reg->rotulo(i.udid);
      if (base.empty()) base = "iPhone";
      item(i.udid, base, !i.wifi);
      if (!i.wifi && !i.idApp.empty())
        for (auto& sv : servicos)
          if (sv.id == i.idApp) item("wifi:" + sv.id, base, false);
    }
    for (auto& sv : servicos) {
      if (vistos.count("wifi:" + sv.id)) continue;
      std::string base = reg->rotulo("wifi:" + sv.id);
      item("wifi:" + sv.id, base.empty() ? "iPhone" : base, false);
    }
  }
  if (!achou) {
    bool eWifi = !cfg.wifiId.empty();
    std::string rot = rotuloDoCache(cfg.nomeCache) + " \xC2\xB7 " + T(eWifi ? "Aparelho.Wifi" : "Aparelho.Cabo");
    rot += std::string(" - ") + T(eWifi ? "Aparelho.ForaDaRede" : "Aparelho.ForaDoCabo");
    obs_property_list_add_string(lista, rot.c_str(), sel.c_str());
  }
  obs_properties_add_button2(p, "atualizar", T("AtualizarLista"), botaoAtualizar, f);

  obs_property_t* pre = obs_properties_add_list(p, "predefinicao", T("Predefinicao"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(pre, T("Predefinicao.IPhone"), "");
  obs_property_list_add_string(pre, "1080p30", "1080p30");
  obs_property_list_add_string(pre, "1080p60", "1080p60");
  obs_property_list_add_string(pre, "1080p120", "1080p120");
  obs_property_list_add_string(pre, "2160p30", "2160p30");
  obs_property_list_add_string(pre, "720p30", "720p30");
  obs_property_list_add_string(pre, "720p60", "720p60");
  obs_property_t* len = obs_properties_add_list(p, "lente", T("Lente"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(len, T("Lente.Manter"), "");
  obs_property_list_add_string(len, T("Lente.Grande"), "wide");
  obs_property_list_add_string(len, T("Lente.Ultra"), "ultra");
  obs_property_list_add_string(len, T("Lente.Tele"), "tele");
  obs_property_t* bit = obs_properties_add_list(p, "bitrate", T("Bitrate"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(bit, T("Bitrate.IPhone"), 0);
  static const int kBitrates[] = {6000, 10000, 15000, 20000, 30000};
  bool bitrateNaLista = cfg.bitrateKbps == 0;
  for (int kbps : kBitrates) {
    if (kbps == cfg.bitrateKbps) bitrateNaLista = true;
    obs_property_list_add_int(bit, (std::to_string(kbps) + " kbps").c_str(), kbps);
  }
  // valor salvo fora da lista (ex.: 12000 do slider antigo): continua aparecendo e valendo
  if (!bitrateNaLista) obs_property_list_insert_int(bit, 1, (std::to_string(cfg.bitrateKbps) + " kbps").c_str(), cfg.bitrateKbps);

  obs_properties_add_bool(p, "wifi", T("Wifi"));
  obs_properties_add_text(p, "wifi_ip", T("Wifi.Ip"), OBS_TEXT_DEFAULT);
  return p;
}

}  // namespace

void registrarFonte() {
  obs_source_info i = {};
  i.id = "fmcam_fonte";
  i.type = OBS_SOURCE_TYPE_INPUT;
  i.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
  i.get_name = nomeFonteTipo;
  i.create = criar;
  i.destroy = destruir;
  i.update = atualizar;
  i.get_defaults = padroes;
  i.get_properties = propriedades;
  i.icon_type = OBS_ICON_TYPE_CAMERA;
  obs_register_source(&i);
}

}  // namespace fmcam
