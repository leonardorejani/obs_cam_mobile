// Sincronia de relogio: pongs fora de ordem, RTT enorme, relogio do iPhone pulando.
#include <cstdio>
#include "protocolo.h"
using namespace fmappcam;
int main() {
  const int64_t desvio = 5'000'000'000LL;  // iPhone 5000 s a frente do cliente
  auto pong = [&](SincroniaRelogio& s, uint64_t t0, uint64_t rtt) { s.registrar(t0, t0 + rtt, t0 + rtt / 2 + desvio); };
  { SincroniaRelogio s; for (int i = 0; i < 5; ++i) pong(s, 1000000 + i * 1000000, 2000);
    std::printf("normal: dif=%lld (esperado %lld) ida=%lld\n", (long long)s.diferenca(), (long long)desvio, (long long)s.idaEVolta()); }
  { // pong fora de ordem / eco de ping que ainda nao foi enviado: t0 no futuro => ida negativa vence o "menor RTT"
    SincroniaRelogio s; for (int i = 0; i < 5; ++i) pong(s, 1000000 + i * 1000000, 2000);
    s.registrar(/*t0*/ 9000000, /*t1*/ 6000000, /*ts*/ 6001000 + desvio);
    std::printf("eco com t0 > t1: ida=%lld dif=%lld (erro de %lld us na diferenca; atraso medido de quadro com 20ms reais = %lld us)\n",
                (long long)s.idaEVolta(), (long long)s.diferenca(), (long long)(s.diferenca() - desvio),
                (long long)s.atraso(7000000 + 20000, 7000000 + desvio)); }
  { // relogio do iPhone pula 1 s no meio: a amostra antiga de menor RTT segue mandando por ate 10 pongs
    SincroniaRelogio s; for (int i = 0; i < 3; ++i) pong(s, 1000000 + i * 1000000, i == 1 ? 1000 : 3000);
    for (int i = 3; i < 9; ++i) { uint64_t t0 = 1000000 + i * 1000000; s.registrar(t0, t0 + 3000, t0 + 1500 + desvio + 1000000); }
    std::printf("pulo de 1 s no relogio do iPhone: dif=%lld (certo seria %lld): atraso de quadro real=20ms medido como %lld us\n",
                (long long)s.diferenca(), (long long)(desvio + 1000000), (long long)s.atraso(20000000 + 20000, 20000000 + desvio + 1000000)); }
  { // RTT de 4 s no unico pong: erro de ate RTT/2 na diferenca se a ida e a volta forem assimetricas
    SincroniaRelogio s; uint64_t t0 = 1000000; s.registrar(t0, t0 + 4000000, t0 + 100 + desvio);  // iPhone respondeu quase na hora da ida; volta levou 4 s
    std::printf("RTT 4 s assimetrico: dif=%lld, erro=%lld us\n", (long long)s.diferenca(), (long long)(s.diferenca() - desvio)); }
}
