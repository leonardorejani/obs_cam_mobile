#include <chrono>
#include <cstdio>
#include <thread>
#include "conexao.h"
using namespace fmappcam;
using Rel = std::chrono::steady_clock;
static double ms(Rel::time_point a) { return std::chrono::duration<double, std::milli>(Rel::now() - a).count(); }
int main(int argc, char** argv) {
  std::string modo = argv[1];
  OpcoesConexao op;
  if (modo == "usb") { op.usb = true; op.usbmuxCaminho = argv[2]; op.udid = "U1"; }
  else { op.host = "127.0.0.1"; op.porta = uint16_t(atoi(argv[2])); }
  Conexao c(op);
  if (modo == "callback") c.definirCallbacks([&](const Pacote&, uint64_t) { std::this_thread::sleep_for(std::chrono::milliseconds(3000)); }, nullptr);
  c.iniciar(); std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  auto t = Rel::now(); c.parar(); std::printf("modo %s: parar() levou %.0f ms\n", modo.c_str(), ms(t));
}
