// Carga do modulo SEM iniciar o OBS: dlopen do binario instalado, simbolos obrigatorios, versao da API,
// locale presente e dependencias so do OBS e do sistema (nada de /opt/homebrew no caminho de carga).
// Uso: teste_carga_modulo <fmcam-extension.plugin>
#include <dlfcn.h>
#include <obs-module.h>
#include <obs.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

static int falhas = 0;
static void checar(bool ok, const std::string& m) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FALHOU", m.c_str());
  if (!ok) ++falhas;
}
static bool existe(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  std::string bundle = argv[1];
  std::string bin = bundle + "/Contents/MacOS/fmcam-extension";
  checar(existe(bin), "binario em Contents/MacOS: " + bin);
  checar(existe(bundle + "/Contents/Info.plist"), "Info.plist");
  checar(existe(bundle + "/Contents/Resources/locale/pt-BR.ini") && existe(bundle + "/Contents/Resources/locale/en-US.ini"),
         "locale pt-BR e en-US em Resources/locale");
  {
    std::ifstream f(bundle + "/Contents/Resources/locale/pt-BR.ini");
    std::stringstream ss;
    ss << f.rdbuf();
    checar(ss.str().find("Atualizar lista") != std::string::npos && ss.str().find("Predefinição") != std::string::npos,
           "pt-BR com acento");
  }

  // dependencias: so @rpath do OBS e sistema
  std::string cmd = "otool -L '" + bin + "'";
  FILE* p = popen(cmd.c_str(), "r");
  char linha[1024];
  bool ruim = false;
  int deps = 0;
  while (p && fgets(linha, sizeof linha, p)) {
    std::string l = linha;
    if (l.find("compatibility version") == std::string::npos) continue;
    ++deps;
    bool ok = l.find("\t@rpath/") == 0 || l.find("\t/usr/lib/") == 0 || l.find("\t/System/Library/") == 0;
    if (!ok) {
      ruim = true;
      std::printf("    dependencia fora do permitido: %s", l.c_str());
    }
  }
  if (p) pclose(p);
  checar(deps > 0 && !ruim, "otool -L: so @rpath (OBS), /usr/lib e /System (" + std::to_string(deps) + " dependencias)");

  void* h = dlopen(bin.c_str(), RTLD_NOW | RTLD_LOCAL);
  checar(h != nullptr, std::string("dlopen") + (h ? "" : std::string(": ") + dlerror()));
  if (!h) return 1;
  const char* simbolos[] = {"obs_module_load", "obs_module_unload", "obs_module_ver", "obs_module_set_pointer",
                            "obs_module_name", "obs_module_description", "obs_module_set_locale", "obs_module_text"};
  for (auto s : simbolos) checar(dlsym(h, s) != nullptr, std::string("simbolo ") + s);
  auto ver = reinterpret_cast<uint32_t (*)()>(dlsym(h, "obs_module_ver"));
  if (ver) {
    uint32_t v = ver(), o = obs_get_version();
    std::printf("    obs_module_ver=%u.%u.%u  libobs=%u.%u.%u\n", v >> 24, (v >> 16) & 0xff, v & 0xffff, o >> 24,
                (o >> 16) & 0xff, o & 0xffff);
    checar((v >> 24) == (o >> 24), "versao maior da API igual a do libobs do OBS instalado");
  }
  auto nome = reinterpret_cast<const char* (*)()>(dlsym(h, "obs_module_name"));
  if (nome) checar(std::string(nome()) == "OBS Cam Mobile Extension", "nome do modulo");
  dlclose(h);
  std::printf("carga: %s\n", falhas ? "FALHOU" : "OK");
  return falhas ? 1 : 0;
}
