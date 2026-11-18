// libobs de verdade, sem video e sem janela: obs_startup, carrega o modulo instalado, cria 4 fontes
// "fmcam_fonte" apontadas para 4 iphone-falso (pelo usbmuxd-falso) e confere quadros, lista de aparelhos
// nas propriedades, queda e volta de um iPhone e o fechamento sem travar.
// Uso: teste_headless_obs <fmcam-extension.plugin> <iphone-falso> <amostra.h264> <amostra720.h264>
#include <obs.h>
#include <util/base.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "proc.h"
#include "usbmuxd_falso.h"

using namespace fmappcam;
using namespace std::chrono_literals;

static int falhas = 0;
static void checar(bool ok, const std::string& m) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FALHOU", m.c_str());
  if (!ok) ++falhas;
}

static std::mutex g_mLog;
static std::atomic<int> g_removidas{0};
static void logObs(int nivel, const char* fmt, va_list args, void*) {
  char b[1024];
  vsnprintf(b, sizeof b, fmt, args);
  if (std::string(b).find("[fmcam]") != std::string::npos) {
    std::lock_guard<std::mutex> l(g_mLog);  // o OBS loga de varias threads
    if (std::string(b).find("fonte removida") != std::string::npos) ++g_removidas;
    std::printf("    obs-log(%d): %s\n", nivel, b);
  }
}

static std::string estado(obs_source_t* s) {
  calldata_t cd;
  calldata_init(&cd);
  proc_handler_call(obs_source_get_proc_handler(s), "fmcam_estado", &cd);
  const char* j = nullptr;
  calldata_get_string(&cd, "json", &j);
  std::string r = j ? j : "";
  calldata_free(&cd);
  return r;
}
static long numero(const std::string& json, const std::string& chave) {
  size_t p = json.find("\"" + chave + "\":");
  return p == std::string::npos ? -1 : std::atol(json.c_str() + p + chave.size() + 3);
}
static bool contem(const std::string& s, const std::string& t) { return s.find(t) != std::string::npos; }

template <class F>
static bool esperar(F cond, int ms = 10000) {
  for (int i = 0; i < ms / 25; ++i) {
    if (cond()) return true;
    std::this_thread::sleep_for(25ms);
  }
  return cond();
}

int main(int argc, char** argv) {
  if (argc < 5) return 2;
  std::string bundle = argv[1], falso = argv[2], amostra = argv[3], amostra720 = argv[4];
  std::string sock = "/tmp/fmcam_headless_" + std::to_string(getpid()) + ".sock";
  UsbmuxdFalso fake(sock);
  std::string erro;
  if (!fake.iniciar(erro)) return 97;
  std::vector<fmtest::Proc> ips(4);
  uint16_t portas[4];
  for (int i = 0; i < 4; ++i) {
    ips[i] = fmtest::iphoneFalso(falso, amostra, "Camera " + std::to_string(i + 1), "iPhone " + std::to_string(11 + i),
                                 "uuid-" + std::to_string(i + 1), portas[i]);
    fake.anexar({i + 1, "UDID-" + std::to_string(i + 1), "USB"}, "127.0.0.1", portas[i]);
  }
  setenv("FMCAM_BONJOUR", "local", 1);  // so anuncios locais: o teste nao toca a rede nem pede permissao
  setenv("FMCAM_USBMUXD", sock.c_str(), 1);
  setenv("FMCAM_CANVAS", "1920x1080", 1);  // sem video iniciado nao ha canvas: o modulo le daqui  // nunca o usbmuxd real: este teste nao toca em iPhone de verdade

  base_set_log_handler(logObs, nullptr);
  checar(obs_startup("pt-BR", nullptr, nullptr), "obs_startup (sem video, sem janela)");
  obs_module_t* mod = nullptr;
  int rc = obs_open_module(&mod, (bundle + "/Contents/MacOS/fmcam-extension").c_str(), (bundle + "/Contents/Resources").c_str());
  checar(rc == MODULE_SUCCESS, "obs_open_module (rc=" + std::to_string(rc) + ")");
  if (rc != MODULE_SUCCESS) return 1;
  checar(obs_init_module(mod), "obs_init_module chama obs_module_load");
  obs_log_loaded_modules();

  bool registrada = false;
  for (size_t i = 0;; ++i) {
    const char* id;
    if (!obs_enum_input_types(i, &id)) break;
    if (std::string(id) == "fmcam_fonte") registrada = true;
  }
  checar(registrada, "fonte fmcam_fonte registrada");
  const char* nomeTipo = obs_source_get_display_name("fmcam_fonte");
  checar(nomeTipo && std::string(nomeTipo) == "OBS Cam Mobile Extension", "nome exibido da fonte");
  checar((obs_get_source_output_flags("fmcam_fonte") & OBS_SOURCE_ASYNC_VIDEO) == OBS_SOURCE_ASYNC_VIDEO, "flag ASYNC_VIDEO");

  obs_source_t* fontes[4];
  for (int i = 0; i < 4; ++i) {
    obs_data_t* d = obs_data_create();
    obs_data_set_string(d, "udid", ("UDID-" + std::to_string(i + 1)).c_str());
    fontes[i] = obs_source_create("fmcam_fonte", ("OBS Cam Mobile " + std::to_string(i + 1)).c_str(), d, nullptr);
    obs_data_release(d);
  }
  checar(esperar([&] {
    for (auto* f : fontes)
      if (numero(estado(f), "quadros") < 30) return false;
    return true;
  }),
         "as 4 fontes puxando quadros do iphone-falso");
  for (int i = 0; i < 4; ++i) {
    std::string j = estado(fontes[i]);
    std::printf("    fonte %d: %s\n", i + 1, j.c_str());
    checar(contem(j, "\"estado\":\"conectado\"") && numero(j, "w") == 1920 && numero(j, "h") == 1080 && numero(j, "erros") == 0,
           "fonte " + std::to_string(i + 1) + " conectada 1920x1080 sem erro de decodificacao");
    checar(contem(j, "\"nome\":\"Camera " + std::to_string(i + 1) + "\"") && contem(j, "\"modelo\":\"iPhone " + std::to_string(11 + i) + "\""),
           "fonte " + std::to_string(i + 1) + " sabe nome e modelo do handshake");
  }

  // propriedades: lista com os 4 aparelhos pelo nome da camera e o modelo
  obs_properties_t* pr = obs_source_properties(fontes[0]);
  obs_property_t* lista = obs_properties_get(pr, "udid");
  std::vector<std::string> itens;
  for (size_t i = 0; lista && i < obs_property_list_item_count(lista); ++i) itens.push_back(obs_property_list_item_name(lista, i));
  for (auto& s : itens) std::printf("    item da lista: %s\n", s.c_str());
  int nomeados = 0;
  for (int i = 1; i <= 4; ++i)
    for (auto& s : itens)
      if (contem(s, "iPhone " + std::to_string(10 + i) + " \xC2\xB7 Cabo")) ++nomeados;
  checar(nomeados == 4, "lista de aparelhos mostra '<modelo comercial> \xC2\xB7 Cabo' dos 4");
  checar(!contem(itens.empty() ? "" : itens[0], "Primeiro iPhone"), "nao existe mais a opcao 'Primeiro iPhone no cabo'");
  checar(obs_properties_get(pr, "atualizar") && obs_properties_get(pr, "predefinicao") && obs_properties_get(pr, "lente") &&
             obs_properties_get(pr, "bitrate") && obs_properties_get(pr, "wifi") && obs_properties_get(pr, "wifi_ip"),
         "botao Atualizar lista, predefinicao, lente, bitrate e Wi-Fi manual");
  obs_properties_destroy(pr);

  // iPhone 3 sai do cabo, depois volta: as outras tres nao sentem
  long antes[4];
  for (int i = 0; i < 4; ++i) antes[i] = numero(estado(fontes[i]), "quadros");
  ips[2].matar();
  fake.desanexar(3);
  checar(esperar([&] { return contem(estado(fontes[2]), "sem_cabo") || contem(estado(fontes[2]), "sem_app"); }), "fonte 3 percebe a queda");
  std::this_thread::sleep_for(1500ms);
  for (int i : {0, 1, 3}) checar(numero(estado(fontes[i]), "quadros") > antes[i] + 20, "fonte " + std::to_string(i + 1) + " seguiu normal");
  long parado = numero(estado(fontes[2]), "quadros");
  ips[2] = fmtest::iphoneFalso(falso, amostra, "Camera 3", "iPhone 13", "uuid-3", portas[2]);
  fake.anexar({9, "UDID-3", "USB"}, "127.0.0.1", portas[2]);
  checar(esperar([&] { return numero(estado(fontes[2]), "quadros") > parado + 30; }, 15000), "fonte 3 volta sozinha quando o iPhone volta");

  // dois iPhones numa fonte so nao acontece: a quinta fonte no UDID-1 fica ocupada
  obs_data_t* d = obs_data_create();
  obs_data_set_string(d, "udid", "UDID-1");
  obs_source_t* dupla = obs_source_create("fmcam_fonte", "OBS Cam Mobile duplicada", d, nullptr);
  obs_data_release(d);
  checar(esperar([&] { return contem(estado(dupla), "ocupado"); }), "segunda fonte no mesmo iPhone fica ocupada (nao rouba)");
  checar(numero(estado(fontes[0]), "quadros") > 0 && contem(estado(fontes[0]), "conectado"), "a primeira continua conectada");

  // ---- troca de resolucao (1080p -> 720p) com itens de cena ----
  {
    // sem video nao ha canvas para as coordenadas relativas do OBS 33: o teste usa as absolutas (em pixels)
    obs_data_t* priv = obs_data_create();
    obs_data_set_bool(priv, "AbsoluteCoordinates", true);
    obs_apply_private_data(priv);
    obs_data_release(priv);
    obs_scene_t* cena = obs_scene_create("cena teste");
    obs_data_t* dz = obs_data_create();
    obs_data_set_string(dz, "udid", "UDID-Z");
    obs_source_t* fz = obs_source_create("fmcam_fonte", "OBS Cam Mobile Z", dz, nullptr);
    obs_data_release(dz);
    obs_sceneitem_t* cheio = obs_scene_add(cena, fz);  // padrao: pos 0,0, escala 1
    obs_sceneitem_t* pip = obs_scene_add(cena, fz);
    // sem video iniciado o libobs nao inicializa o item (pos NaN): poe os padroes de um item novo do OBS
    vec2 zero = {0, 0}, um = {1, 1};
    obs_sceneitem_set_pos(cheio, &zero);
    obs_sceneitem_set_scale(cheio, &um);
    obs_sceneitem_set_rot(cheio, 0);
    obs_sceneitem_set_alignment(cheio, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
    obs_sceneitem_set_bounds_type(cheio, OBS_BOUNDS_NONE);
    obs_sceneitem_crop sem = {0, 0, 0, 0};
    obs_sceneitem_set_crop(cheio, &sem);
    obs_sceneitem_set_crop(pip, &sem);
    obs_sceneitem_set_alignment(pip, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
    obs_sceneitem_set_bounds_type(pip, OBS_BOUNDS_NONE);
    vec2 ppos = {1400, 780}, pesc = {0.25f, 0.25f};
    obs_sceneitem_set_pos(pip, &ppos);
    obs_sceneitem_set_scale(pip, &pesc);
    uint16_t pz;
    auto ipZ = fmtest::iphoneFalso(falso, amostra, "Camera Z", "iPhone 15 Pro Max", "uuid-z", pz);
    fake.anexar({20, "UDID-Z", "USB"}, "127.0.0.1", pz);
    checar(esperar([&] { return numero(estado(fz), "w") == 1920 && numero(estado(fz), "quadros") > 5; }, 15000), "fonte Z em 1920x1080");
    // tally: o libobs so dispara "activate" no tick do video (sem video nao ha tick), entao o teste emite o mesmo
    // sinal que o libobs emite quando a cena vai para o Programa e confere a fiacao ate a sessao.
    checar(contem(estado(fz), "\"noar\":false"), "fora do Programa: noar=false");
    signal_handler_t* sh = obs_source_get_signal_handler(fz);
    calldata_t cds;
    calldata_init(&cds);
    calldata_set_ptr(&cds, "source", fz);
    signal_handler_signal(sh, "activate", &cds);
    checar(esperar([&] { return contem(estado(fz), "\"noar\":true"); }), "sinal activate liga o tally da sessao");
    signal_handler_signal(sh, "deactivate", &cds);
    checar(esperar([&] { return contem(estado(fz), "\"noar\":false"); }), "sinal deactivate desliga o tally");
    calldata_free(&cds);
    vec2 b;
    obs_sceneitem_get_bounds(cheio, &b);
    { vec2 pp0, ps0; obs_sceneitem_get_pos(cheio,&pp0); obs_sceneitem_get_scale(cheio,&ps0);
      std::printf("    item cheio: pos %.0f,%.0f escala %.2f bounds_tipo %d bounds %.0fx%.0f align %u\n", pp0.x, pp0.y, ps0.x,
                  int(obs_sceneitem_get_bounds_type(cheio)), b.x, b.y, obs_sceneitem_get_alignment(cheio)); }
    checar(obs_sceneitem_get_bounds_type(cheio) == OBS_BOUNDS_SCALE_INNER && b.x == 1920 && b.y == 1080,
           "item que enchia o canvas ganhou bounds 1920x1080 SCALE_INNER");
    // o dono troca a predefinicao: o iPhone passa a mandar 720p (mesma fonte, mesmo aparelho)
    ipZ.matar();
    fake.desanexar(20);
    checar(esperar([&] { return !contem(estado(fz), "\"estado\":\"conectado\""); }), "fonte Z percebe a troca");
    uint16_t pz2;
    auto ipZ2 = fmtest::iphoneFalso(falso, amostra720, "Camera Z", "iPhone 15 Pro Max", "uuid-z", pz2);
    fake.anexar({21, "UDID-Z", "USB"}, "127.0.0.1", pz2);
    checar(esperar([&] { return numero(estado(fz), "w") == 1280 && numero(estado(fz), "h") == 720 && contem(estado(fz), "conectado"); }, 15000),
           "fonte Z agora em 1280x720");
    vec2 pos, bb;
    obs_sceneitem_get_pos(cheio, &pos);
    obs_sceneitem_get_bounds(cheio, &bb);
    checar(obs_sceneitem_get_bounds_type(cheio) == OBS_BOUNDS_SCALE_INNER && bb.x == 1920 && bb.y == 1080 && pos.x == 960 && pos.y == 540,
           "item de tela cheia segue com bounds 1920x1080 centralizado (720p preenche, nao vai pro canto)");
    vec2 pp, ps;
    obs_sceneitem_get_pos(pip, &pp);
    obs_sceneitem_get_scale(pip, &ps);
    checar(obs_sceneitem_get_bounds_type(pip) == OBS_BOUNDS_NONE && pp.x == 1400 && pp.y == 780 && ps.x == 0.25f,
           "picture-in-picture redimensionado a mao nao foi tocado");
    obs_sceneitem_remove(cheio);
    obs_sceneitem_remove(pip);
    obs_scene_release(cena);
    obs_source_release(fz);
    ipZ2.matar();
  }


  // ---- troca de qualidade feita no iPhone no meio da conexao (1080p -> 720p, sem reconectar) ----
  {
    obs_scene_t* cena = obs_scene_create("cena troca no iphone");
    obs_data_t* dy = obs_data_create();
    obs_data_set_string(dy, "udid", "UDID-Y");
    obs_source_t* fy = obs_source_create("fmcam_fonte", "OBS Cam Mobile Y", dy, nullptr);
    obs_data_release(dy);
    obs_sceneitem_t* cheio = obs_scene_add(cena, fy);
    vec2 zero = {0, 0}, um = {1, 1};
    obs_sceneitem_set_pos(cheio, &zero);
    obs_sceneitem_set_scale(cheio, &um);
    obs_sceneitem_set_rot(cheio, 0);
    obs_sceneitem_set_alignment(cheio, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
    obs_sceneitem_set_bounds_type(cheio, OBS_BOUNDS_NONE);
    obs_sceneitem_crop sem = {0, 0, 0, 0};
    obs_sceneitem_set_crop(cheio, &sem);
    uint16_t py;
    // fonte nova = qualidade "do iPhone" (padrao): o iPhone falso troca sozinho 1080p30 -> 720p30 depois de 4 s
    auto ipY = fmtest::iphoneFalso(falso, amostra, "Camera Y", "iPhone 17 Pro", "uuid-y", py,
                                   {"--preset-proprio", "1080p30", "--trocar-preset-apos-s", "4", "--para", "720p30",
                                    "--arquivo-troca", amostra720});
    fake.anexar({30, "UDID-Y", "USB"}, "127.0.0.1", py);
    checar(esperar([&] { return numero(estado(fy), "w") == 1920 && numero(estado(fy), "quadros") > 5; }, 15000), "fonte Y comecou em 1920x1080");
    // a fonte nasceu antes do iPhone aparecer no cabo, entao ja tentou conectar: conta as tentativas daqui para a frente
    const long reconAntes = numero(estado(fy), "reconexoes");
    vec2 b0;
    obs_sceneitem_get_bounds(cheio, &b0);
    checar(obs_sceneitem_get_bounds_type(cheio) == OBS_BOUNDS_SCALE_INNER && b0.x == 1920 && b0.y == 1080, "item de tela cheia ganhou bounds no 1080p");
    checar(esperar([&] { return numero(estado(fy), "w") == 1280 && numero(estado(fy), "h") == 720; }, 15000),
           "iPhone trocou para 720p no meio da conexao: a fonte passou a 1280x720");
    std::string j = estado(fy);
    std::printf("    fonte Y depois da troca: %s\n", j.c_str());
    checar(numero(j, "reconexoes") == reconAntes && contem(j, "\"estado\":\"conectado\"") && numero(j, "erros") == 0,
           "a troca aconteceu na mesma conexao (reconexoes igual a antes da troca, sem erro de decodificacao)");
    vec2 pos, bb;
    obs_sceneitem_get_pos(cheio, &pos);
    obs_sceneitem_get_bounds(cheio, &bb);
    checar(obs_sceneitem_get_bounds_type(cheio) == OBS_BOUNDS_SCALE_INNER && bb.x == 1920 && bb.y == 1080 && pos.x == 960 && pos.y == 540,
           "item de tela cheia segue com bounds 1920x1080 centralizado depois da troca feita no iPhone");
    obs_sceneitem_remove(cheio);
    obs_scene_release(cena);
    obs_source_release(fy);
    ipY.matar();
  }

  // ---- fonte sem aparelho escolhido: fixa o primeiro iPhone que ligou, e a lista mostra esse mesmo iPhone ----
  {
    obs_source_release(fontes[0]);  // libera o UDID-1 (o primeiro da lista) para a fonte sem aparelho escolhido
    fontes[0] = nullptr;
    obs_data_t* dp = obs_data_create();  // sem "udid": primeiro iPhone no cabo
    obs_source_t* fp = obs_source_create("fmcam_fonte", "OBS Cam Mobile primeiro", dp, nullptr);
    obs_data_release(dp);
    checar(esperar([&] { return contem(estado(fp), "\"estado\":\"conectado\""); }, 20000), "fonte sem aparelho escolhido conectou ao primeiro iPhone");
    std::this_thread::sleep_for(500ms);
    obs_data_t* sp = obs_source_get_settings(fp);
    std::string udidSalvo = obs_data_get_string(sp, "udid");
    obs_data_release(sp);
    std::printf("    udid salvo na config depois de conectar: '%s'\n", udidSalvo.c_str());
    checar(udidSalvo == "UDID-1", "a config guardou o iPhone que ligou (UDID-1)");
    obs_data_t* sp2 = obs_source_get_settings(fp);
    obs_properties_t* pp = obs_source_properties(fp);
    obs_property_t* lp = obs_properties_get(pp, "udid");
    bool selecionadoNaLista = false;
    for (size_t i = 0; lp && i < obs_property_list_item_count(lp); ++i)
      if (std::string(obs_property_list_item_string(lp, i)) == obs_data_get_string(sp2, "udid")) selecionadoNaLista = true;
    obs_data_release(sp2);
    checar(selecionadoNaLista, "o iPhone selecionado esta na lista (a selecao mostrada bate com o que esta ligado)");
    obs_properties_destroy(pp);
    obs_source_release(fp);
  }

  // fechar tudo rapido
  auto t0 = std::chrono::steady_clock::now();
  obs_source_release(dupla);
  for (auto* f : fontes)
    if (f) obs_source_release(f);
  // o libobs destroi a fonte numa thread dele (destroy_defer): espera o destroy do modulo rodar em todas
  checar(esperar([&] { return g_removidas == 8; }, 8000), "as 8 fontes foram destruidas");
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  std::printf("    fontes liberadas e destruidas em %lld ms\n", static_cast<long long>(ms));
  checar(ms < 5000, "remover as fontes nao trava (<5 s)");
  for (auto& p : ips) p.matar();
  fake.parar();
  obs_shutdown();
  std::printf("headless: %s\n", falhas ? "FALHOU" : "OK");
  return falhas ? 1 : 0;
}
