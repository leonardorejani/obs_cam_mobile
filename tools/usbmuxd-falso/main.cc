// usbmuxd-falso: soquete Unix que imita o usbmuxd para testes.
// --aparelho UDID=host:porta (repetivel, ids 1,2,3...), --rede UDID (aparelho Network, so para provar o filtro).
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "protocolo.h"
#include "usbmuxd_falso.h"

using namespace fmappcam;

static volatile std::sig_atomic_t g_sair = 0;

int main(int argc, char** argv) {
  std::string socket;
  uint16_t porta = kPortaPadrao;
  struct Ap { std::string udid, host; uint16_t porta; bool rede; };
  std::vector<Ap> aps;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (i + 1 >= argc) { std::fprintf(stderr, "faltou valor em %s\n", a.c_str()); return 2; }
    std::string v = argv[++i];
    if (a == "--socket") socket = v;
    else if (a == "--porta-esperada") porta = static_cast<uint16_t>(std::atoi(v.c_str()));
    else if (a == "--aparelho") {
      size_t ig = v.find('='), dp = v.rfind(':');
      if (ig == std::string::npos || dp == std::string::npos || dp < ig) { std::fputs("use UDID=host:porta\n", stderr); return 2; }
      aps.push_back({v.substr(0, ig), v.substr(ig + 1, dp - ig - 1), static_cast<uint16_t>(std::atoi(v.c_str() + dp + 1)), false});
    } else if (a == "--rede") aps.push_back({v, "127.0.0.1", 1, true});
    else { std::fprintf(stderr, "opcao desconhecida %s\n", a.c_str()); return 2; }
  }
  if (socket.empty()) { std::fputs("uso: usbmuxd-falso --socket CAMINHO --aparelho UDID=host:porta ...\n", stderr); return 2; }
  UsbmuxdFalso fake(socket, porta);
  int id = 1;
  for (auto& a : aps) fake.anexar({id++, a.udid, a.rede ? "Network" : "USB"}, a.host, a.porta);
  std::string erro;
  if (!fake.iniciar(erro)) { std::fprintf(stderr, "%s\n", erro.c_str()); return 1; }
  std::signal(SIGTERM, [](int) { g_sair = 1; });
  std::signal(SIGINT, [](int) { g_sair = 1; });
  std::printf("PRONTO %s\n", socket.c_str());
  std::fflush(stdout);
  while (!g_sair) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  fake.parar();
  return 0;
}
