// Conexao::parar() sob estresse: latencia de parada e corridas (rodar com TSan).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include "conexao.h"
using namespace fmappcam;
using Rel = std::chrono::steady_clock;
static double ms(Rel::time_point a) { return std::chrono::duration<double, std::milli>(Rel::now() - a).count(); }
int main(int argc, char** argv) {
  std::string blackhole = argc > 1 ? argv[1] : "10.255.255.1";
  { // 1. parar() durante conectarTcp para IP que nao responde
    OpcoesConexao op; op.host = blackhole; Conexao c(op); c.iniciar(); std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto t = Rel::now(); c.parar(); std::printf("parar() durante connect a %s: %.0f ms\n", blackhole.c_str(), ms(t)); }
  { // 2. parar() logo apos iniciar(), 200 vezes (corrida abrir()/sock_)
    double max = 0;
    for (int i = 0; i < 200; ++i) { OpcoesConexao op; op.host = "127.0.0.1"; op.porta = 1; Conexao c(op); c.iniciar(); auto t = Rel::now(); c.parar(); max = std::max(max, ms(t)); }
    std::printf("200x iniciar/parar contra porta fechada: pior parar() = %.0f ms\n", max); }
  { // 3. enviarComando concorrente com reconexoes e parar
    OpcoesConexao op; op.porta = 1; op.atrasoReconexaoMs = 1; Conexao c(op); std::atomic<bool> fim{false};
    std::thread a([&] { while (!fim) c.enviarComando("{\"cmd\":\"keyframe\"}"); });
    std::thread b([&] { while (!fim) { (void)c.resposta(); (void)c.sincronizada(); int64_t x; c.atrasoUs(1, 2, x); } });
    c.iniciar(); std::this_thread::sleep_for(std::chrono::milliseconds(500)); c.parar(); fim = true; a.join(); b.join();
    std::printf("concorrencia enviarComando/resposta com reconexao rapida: ok, reconexoes=%d\n", c.reconexoes()); }
  { // 4. parar() com callback de pacote travado (cliente lento): quanto demora
    OpcoesConexao op; op.host = "127.0.0.1"; op.porta = argc > 2 ? uint16_t(atoi(argv[2])) : 1; Conexao c(op);
    std::atomic<int> n{0};
    c.definirCallbacks([&](const Pacote&, uint64_t) { n++; std::this_thread::sleep_for(std::chrono::milliseconds(1500)); }, nullptr);
    c.iniciar(); std::this_thread::sleep_for(std::chrono::milliseconds(1200)); auto t = Rel::now(); c.parar();
    std::printf("parar() com callback de 1.5 s em andamento: %.0f ms (pacotes=%d)\n", ms(t), n.load()); }
}
