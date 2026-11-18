// Camada fina de soquetes (POSIX e Winsock) usada pelo core e pelas ferramentas.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace fmappcam {

using Soquete = std::intptr_t;
constexpr Soquete kSoqueteInvalido = -1;

enum class ResultadoIO { Ok, Timeout, Fechado, Erro };

bool iniciarRede();  // WSAStartup no Windows, noop no resto
void fecharSoquete(Soquete s);
void desligarSoquete(Soquete s);  // shutdown nos dois sentidos, acorda quem esta lendo

Soquete conectarTcp(const std::string& host, uint16_t porta, int timeoutMs, std::string& erro);
Soquete conectarUnix(const std::string& caminho, std::string& erro);  // so POSIX
Soquete escutarTcp(uint16_t& porta, std::string& erro, const std::string& endereco = "127.0.0.1");
Soquete escutarUnix(const std::string& caminho, std::string& erro);  // so POSIX
Soquete aceitar(Soquete escuta, int timeoutMs);  // invalido se estourou o tempo

// Cancelamento cooperativo (so da thread que chama): enquanto `*flag` for false tudo funciona normal. Quando vira true,
// toda espera (connect, esperarLeitura, lerExato, lerLinha, aceitar) desta thread acorda em ate ~10 ms e falha
// como erro. nullptr desliga. O ponteiro precisa viver mais que a thread ou ser desligado antes.
void definirCancelamentoDaThread(const std::atomic<bool>* flag);

int esperarLeitura(Soquete s, int timeoutMs);  // 1 pronto, 0 timeout, -1 erro
long lerAlgum(Soquete s, uint8_t* buf, size_t max);  // >0 bytes, 0 fechou, -1 erro
bool enviarTudo(Soquete s, const void* dados, size_t n);
ResultadoIO lerExato(Soquete s, uint8_t* buf, size_t n, int timeoutMs);
// Le ate '\n' (sem incluir). `maximo` conta so os bytes da linha, sem o '\n': uma linha de exatamente `maximo`
// bytes e aceita, `maximo`+1 bytes sem achar o fim e Erro.
ResultadoIO lerLinha(Soquete s, size_t maximo, int timeoutMs, std::string& linha);

// Envio sem bloqueio (para quem tem fila propria): tornarNaoBloqueante, depois enviarAlgum (>= 0 bytes enviados,
// 0 se o buffer do sistema esta cheio, -1 erro) e esperarES para saber quando ler ou gravar de novo.
// parFechou: o outro lado ja fechou ou resetou (le sem consumir nada). false se esta vivo, mesmo sem dados.
bool parFechou(Soquete s);
void tornarNaoBloqueante(Soquete s);
long enviarAlgum(Soquete s, const void* dados, size_t n);
// 1 se algo aconteceu (legivel e/ou gravavel; erro e fechamento contam como legivel), 0 timeout, -1 erro.
// So pede gravavel se `querGravar`.
int esperarES(Soquete s, bool querGravar, int timeoutMs, bool& legivel, bool& gravavel);

void ajustarTimeoutEnvio(Soquete s, int ms);
void desativarNagle(Soquete s);

uint64_t agoraMonotonicoUs();

}  // namespace fmappcam
