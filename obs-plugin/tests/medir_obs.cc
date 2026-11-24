// medir_obs: mede o atraso do iPhone ate o quadro desenhado no canvas de um libobs de verdade, sem janela.
// Inicia o video do libobs (OpenGL fora da tela), carrega a extensao, cria uma fonte apontada para um iPhone e,
// a cada quadro desenhado pelo libobs, ve qual quadro da fonte estava na tela e ha quanto tempo ele foi entregue.
// Nao e teste do ctest (precisa de iPhone de verdade): e a ferramenta da varredura do 4K.
// Uso: medir_obs <fmcam-extension.plugin> <udid> <predefinicao|iphone> <segundos> [LxA do canvas] [bitrate kbps]
//      [recepcao: estavel] [decodificacao: hw|sw]
#include <obs.h>
#include <sys/resource.h>
#include <util/base.h>
#include <util/platform.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
std::mutex g_m;
std::vector<double> g_render;  // ms: entrega do quadro ao OBS ate o libobs terminar de desenhar o canvas com ele
uint64_t g_ultimoTs = 0;
uint64_t g_quadrosDiferentes = 0;
obs_source_t* g_fonte = nullptr;
// Partida e congelamento (a corrida toda, sem descartar o aquecimento): instante do primeiro quadro novo no canvas e o
// maior intervalo entre dois quadros novos (entregas diferentes) visto no canvas.
uint64_t g_criadaNs = 0, g_primeiroNs = 0, g_ultimaEntregaVista = 0;
double g_maiorVazioMs = 0, g_maiorVazioDepoisMs = 0;
bool g_medindo = false;

std::string estado(obs_source_t* s);
uint64_t numeroU64(const std::string& json, const std::string& chave) {
  size_t p = json.find("\"" + chave + "\":");
  return p == std::string::npos ? 0 : std::strtoull(json.c_str() + p + chave.size() + 3, nullptr, 10);
}

// Depois de cada canvas desenhado: o quadro da fonte que estava na tela e o ultimo entregue antes do tick (fonte sem
// buffer). Mede da entrega ao fim do desenho. Pode subestimar em ate um desenho (quadro entregue durante o desenho).
void aoDesenhar(void*) {
  uint64_t ts = numeroU64(estado(g_fonte), "ultima_entrega_ns");
  if (!ts) return;
  uint64_t agora = os_gettime_ns();
  std::lock_guard<std::mutex> l(g_m);
  if (ts != g_ultimoTs) {  // so a primeira vez que cada quadro aparece
    if (!g_primeiroNs) g_primeiroNs = agora;
    if (g_ultimaEntregaVista && ts > g_ultimaEntregaVista) {
      double vazio = (ts - g_ultimaEntregaVista) / 1e6;
      g_maiorVazioMs = std::max(g_maiorVazioMs, vazio);
      if (g_medindo) g_maiorVazioDepoisMs = std::max(g_maiorVazioDepoisMs, vazio);
    }
    g_ultimaEntregaVista = ts;
    g_ultimoTs = ts;
    ++g_quadrosDiferentes;
    if (agora > ts) g_render.push_back((agora - ts) / 1e6);
  }
}

void logObs(int nivel, const char* fmt, va_list args, void*) {
  char b[1024];
  vsnprintf(b, sizeof b, fmt, args);
  if (nivel <= LOG_WARNING || std::string(b).find("[fmcam]") != std::string::npos) std::fprintf(stderr, "obs: %s\n", b);
}

std::string estado(obs_source_t* s) {
  calldata_t cd;
  calldata_init(&cd);
  proc_handler_call(obs_source_get_proc_handler(s), "fmcam_estado", &cd);
  const char* j = nullptr;
  calldata_get_string(&cd, "json", &j);
  std::string r = j ? j : "";
  calldata_free(&cd);
  return r;
}

double numero(const std::string& json, const std::string& chave) {
  size_t p = json.find("\"" + chave + "\":");
  return p == std::string::npos ? -1 : std::atof(json.c_str() + p + chave.size() + 3);
}

double pct(std::vector<double> v, double p) {
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5))];
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fputs("uso: medir_obs <plugin> <udid> <predefinicao|iphone> <segundos> [LxA] [kbps] [estavel] [hw|sw]\n", stderr);
    return 2;
  }
  std::string bundle = argv[1], udid = argv[2], preset = argv[3];
  int segundos = std::atoi(argv[4]);
  int cw = 1920, ch = 1080;
  if (argc > 5) std::sscanf(argv[5], "%dx%d", &cw, &ch);
  int kbps = argc > 6 ? std::atoi(argv[6]) : 0;
  std::string recepcao = argc > 7 && std::string(argv[7]) == "estavel" ? "estavel" : "";
  std::string decod = argc > 8 ? argv[8] : "";
  setenv("FMCAM_BONJOUR", "off", 1);
  base_set_log_handler(logObs, nullptr);
  if (!obs_startup("pt-BR", nullptr, nullptr)) return 3;
  obs_video_info ovi = {};
  ovi.graphics_module = "/Applications/OBS.app/Contents/Frameworks/libobs-opengl.dylib";
  ovi.fps_num = 30;
  ovi.fps_den = 1;
  ovi.base_width = ovi.output_width = static_cast<uint32_t>(cw);
  ovi.base_height = ovi.output_height = static_cast<uint32_t>(ch);
  ovi.output_format = VIDEO_FORMAT_NV12;
  ovi.colorspace = VIDEO_CS_709;
  ovi.range = VIDEO_RANGE_PARTIAL;
  ovi.adapter = 0;
  ovi.gpu_conversion = true;
  ovi.scale_type = OBS_SCALE_BICUBIC;
  int rv = obs_reset_video(&ovi);
  if (rv != OBS_VIDEO_SUCCESS) {
    std::fprintf(stderr, "obs_reset_video falhou: %d\n", rv);
    return 4;
  }
  obs_module_t* mod = nullptr;
  if (obs_open_module(&mod, (bundle + "/Contents/MacOS/fmcam-extension").c_str(), (bundle + "/Contents/Resources").c_str()) !=
          MODULE_SUCCESS ||
      !obs_init_module(mod))
    return 5;
  obs_data_t* d = obs_data_create();
  obs_data_set_string(d, "udid", udid.c_str());
  obs_data_set_int(d, "versao_cfg", 4);  // fonte nova: sem migracao de valores antigos
  if (preset != "iphone") obs_data_set_string(d, "predefinicao", preset.c_str());
  obs_data_set_int(d, "bitrate", kbps);
  obs_data_set_string(d, "recepcao", recepcao.c_str());
  obs_data_set_string(d, "decodificacao", decod.c_str());
  g_criadaNs = os_gettime_ns();
  g_fonte = obs_source_create("fmcam_fonte", "medir", d, nullptr);
  obs_data_release(d);
  obs_scene_t* cena = obs_scene_create("medir");
  obs_sceneitem_t* it = obs_scene_add(cena, g_fonte);
  vec2 b = {static_cast<float>(cw), static_cast<float>(ch)};
  obs_sceneitem_set_bounds_type(it, OBS_BOUNDS_SCALE_INNER);
  obs_sceneitem_set_bounds(it, &b);
  // FMCAM_OCULTA=1: a cena com a fonte nao vai para a saida (fonte oculta), para medir o custo de quem nao aparece
  const bool oculta = std::getenv("FMCAM_OCULTA") && std::string(std::getenv("FMCAM_OCULTA")) == "1";
  if (!oculta) obs_set_output_source(0, obs_scene_get_source(cena));
  obs_add_main_rendered_callback(aoDesenhar, nullptr);

  // espera conectar e descarta os primeiros 3 s (troca para o decodificador de hardware, sincronia do relogio)
  for (int i = 0; i < 200 && numero(estado(g_fonte), "quadros") < 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::this_thread::sleep_for(std::chrono::seconds(3));
  {
    std::lock_guard<std::mutex> l(g_m);
    g_render.clear();
    g_quadrosDiferentes = 0;
    g_medindo = true;
  }
  // FMCAM_COMANDOS_JUNTO=1: os comandos de FMCAM_COMANDOS saem durante a medicao (um a cada FMCAM_COMANDOS_INTERVALO s,
  // padrao 3), para medir atraso e congelamento enquanto a qualidade troca.
  std::thread cmdJunto;
  const bool junto = std::getenv("FMCAM_COMANDOS_JUNTO") && std::string(std::getenv("FMCAM_COMANDOS_JUNTO")) == "1";
  if (junto && std::getenv("FMCAM_COMANDOS")) {
    std::string todos = std::getenv("FMCAM_COMANDOS");
    int intervalo = std::getenv("FMCAM_COMANDOS_INTERVALO") ? std::atoi(std::getenv("FMCAM_COMANDOS_INTERVALO")) : 3;
    cmdJunto = std::thread([todos, intervalo] {
      size_t a = 0;
      while (a < todos.size()) {
        std::this_thread::sleep_for(std::chrono::seconds(intervalo));
        size_t b2 = todos.find('|', a);
        std::string c = todos.substr(a, b2 == std::string::npos ? std::string::npos : b2 - a);
        a = b2 == std::string::npos ? todos.size() : b2 + 1;
        calldata_t cd;
        calldata_init(&cd);
        calldata_set_string(&cd, "json", c.c_str());
        proc_handler_call(obs_source_get_proc_handler(g_fonte), "fmcam_comando", &cd);
        bool ok = false;
        calldata_get_bool(&cd, "ok", &ok);
        calldata_free(&cd);
        std::fprintf(stderr, "COMANDO %s enviado=%s\n", c.c_str(), ok ? "sim" : "nao");
      }
    });
  }
  std::vector<double> entregaP50, entregaP95, ida;
  auto cpuMs = [] {
    rusage u;
    getrusage(RUSAGE_SELF, &u);
    return (u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1000.0 + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1000.0;
  };
  const double cpu0 = cpuMs();
  for (int s = 0; s < segundos; ++s) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::string j = estado(g_fonte);
    double e50 = numero(j, "atraso_ms"), e95 = numero(j, "atraso_p95_ms"), rt = numero(j, "ida_e_volta_ms");
    if (e50 >= 0) entregaP50.push_back(e50);
    if (e95 >= 0) entregaP95.push_back(e95);
    if (rt >= 0) ida.push_back(rt);
    std::fprintf(stderr, "t=%ds %s\n", s + 1, j.c_str());
  }
  // FMCAM_COMANDOS="json|json|...": controle remoto de ponta a ponta, um comando a cada 3 s, com o estado depois de cada
  if (cmdJunto.joinable()) cmdJunto.join();
  if (const char* cs = junto ? nullptr : std::getenv("FMCAM_COMANDOS")) {
    std::string todos = cs;
    size_t a = 0;
    int n = 0;
    while (a < todos.size()) {
      size_t b2 = todos.find('|', a);
      std::string c = todos.substr(a, b2 == std::string::npos ? std::string::npos : b2 - a);
      a = b2 == std::string::npos ? todos.size() : b2 + 1;
      calldata_t cd;
      calldata_init(&cd);
      calldata_set_string(&cd, "json", c.c_str());
      proc_handler_call(obs_source_get_proc_handler(g_fonte), "fmcam_comando", &cd);
      bool ok = false;
      calldata_get_bool(&cd, "ok", &ok);
      calldata_free(&cd);
      std::this_thread::sleep_for(std::chrono::seconds(3));
      std::printf("COMANDO %s enviado=%s estado=%s\n", c.c_str(), ok ? "sim" : "nao", estado(g_fonte).c_str());
      std::fflush(stdout);
      if (const char* sh = std::getenv("FMCAM_FOTO_CMD")) std::system((std::string(sh) + " " + std::to_string(++n)).c_str());
    }
  }
  const double cpuPorSeg = (cpuMs() - cpu0) / segundos;
  std::string j = estado(g_fonte);
  std::vector<double> r;
  uint64_t dif;
  {
    std::lock_guard<std::mutex> l(g_m);
    r = g_render;
    dif = g_quadrosDiferentes;
  }
  double e = pct(entregaP50, 0.5), rr = pct(r, 0.5);
  std::printf(
      "RESUMO {\"predefinicao\":\"%s\",\"canvas\":\"%dx%d\",\"recepcao\":\"%s\",\"decodificacao\":\"%s\",\"w\":%g,\"h\":%g,"
      "\"fps\":%g,\"hardware\":%s,\"captura_ate_obs_ms_p50\":%.1f,\"captura_ate_obs_ms_p95\":%.1f,"
      "\"obs_ate_canvas_ms_p50\":%.1f,\"obs_ate_canvas_ms_p95\":%.1f,\"captura_ate_canvas_ms_p50\":%.1f,"
      "\"ida_e_volta_ms\":%.1f,\"quadros_no_canvas\":%llu,\"segundos\":%d,\"cpu_ms_por_s\":%.0f,\"oculta\":%s,"
      "\"primeiro_quadro_ms\":%.0f,\"maior_vazio_ms\":%.0f,\"maior_vazio_medindo_ms\":%.0f}\n",
      preset.c_str(), cw, ch, recepcao.c_str(), decod.c_str(), numero(j, "w"), numero(j, "h"), numero(j, "fps"),
      j.find("\"hardware\":true") != std::string::npos ? "true" : "false", e, pct(entregaP95, 0.5), rr, pct(r, 0.95),
      e >= 0 && rr >= 0 ? e + rr : -1.0, pct(ida, 0.5), static_cast<unsigned long long>(dif), segundos, cpuPorSeg, oculta ? "true" : "false",
      g_primeiroNs ? (g_primeiroNs - g_criadaNs) / 1e6 : -1.0, g_maiorVazioMs, g_maiorVazioDepoisMs);
  obs_remove_main_rendered_callback(aoDesenhar, nullptr);
  obs_set_output_source(0, nullptr);
  obs_sceneitem_remove(it);
  obs_scene_release(cena);
  obs_source_remove(g_fonte);
  obs_source_release(g_fonte);
  obs_wait_for_destroy_queue();  // como o OBS faz ao fechar: fontes destruidas antes de descarregar os modulos
  obs_wait_for_destroy_queue();
  obs_shutdown();
  return 0;
}
