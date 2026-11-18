#include "rede.h"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace fmappcam {

namespace {

#ifdef _WIN32
using SocketNativo = SOCKET;
inline SocketNativo nat(Soquete s) { return static_cast<SocketNativo>(s); }
inline int ultimoErro() { return WSAGetLastError(); }
inline bool interrompido() { return false; }
#else
using SocketNativo = int;
inline SocketNativo nat(Soquete s) { return static_cast<SocketNativo>(s); }
inline int ultimoErro() { return errno; }
inline bool interrompido() { return errno == EINTR; }
#endif

std::string textoErro(const char* oque) {
  return std::string(oque) + " (codigo " + std::to_string(ultimoErro()) + ")";
}

void configurar(Soquete s) {
#ifdef SO_NOSIGPIPE
  int um = 1;
  setsockopt(nat(s), SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&um), sizeof um);
#else
  (void)s;
#endif
}

void definirBloqueante(Soquete s, bool bloqueante) {
#ifdef _WIN32
  u_long modo = bloqueante ? 0 : 1;
  ioctlsocket(nat(s), FIONBIO, &modo);
#else
  int f = fcntl(nat(s), F_GETFL, 0);
  fcntl(nat(s), F_SETFL, bloqueante ? (f & ~O_NONBLOCK) : (f | O_NONBLOCK));
#endif
}

thread_local const std::atomic<bool>* t_cancelar = nullptr;
constexpr int kFatiaCancelamentoMs = 10;

int esperarUmaVez(Soquete s, short evento, int timeoutMs) {
  for (;;) {
#ifdef _WIN32
    WSAPOLLFD p;
    p.fd = nat(s);
    p.events = evento;
    p.revents = 0;
    int r = WSAPoll(&p, 1, timeoutMs);
#else
    pollfd p;
    p.fd = nat(s);
    p.events = evento;
    p.revents = 0;
    int r = poll(&p, 1, timeoutMs);
#endif
    if (r < 0) {
      if (interrompido()) continue;
      return -1;
    }
    if (r == 0) return 0;
    return 1;  // pronto, ou erro/hangup (o recv/send seguinte conta)
  }
}

// Com cancelamento ligado, espera em fatias curtas e olha a flag entre elas.
int esperar(Soquete s, short evento, int timeoutMs) {
  if (!t_cancelar) return esperarUmaVez(s, evento, timeoutMs);
  uint64_t limite = agoraMonotonicoUs() + (timeoutMs < 0 ? ~0ull >> 1 : static_cast<uint64_t>(timeoutMs) * 1000);
  for (;;) {
    if (t_cancelar->load()) return -1;
    uint64_t agora = agoraMonotonicoUs();
    if (agora >= limite) return 0;
    uint64_t resta = (limite - agora + 999) / 1000;
    int fatia = resta < static_cast<uint64_t>(kFatiaCancelamentoMs) ? static_cast<int>(resta) : kFatiaCancelamentoMs;
    int r = esperarUmaVez(s, evento, fatia);
    if (r != 0) return r;
  }
}

}  // namespace

void definirCancelamentoDaThread(const std::atomic<bool>* flag) { t_cancelar = flag; }

bool iniciarRede() {
#ifdef _WIN32
  static bool ok = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  return ok;
#else
  return true;
#endif
}

void fecharSoquete(Soquete s) {
  if (s == kSoqueteInvalido) return;
#ifdef _WIN32
  closesocket(nat(s));
#else
  close(nat(s));
#endif
}

void desligarSoquete(Soquete s) {
  if (s == kSoqueteInvalido) return;
#ifdef _WIN32
  shutdown(nat(s), SD_BOTH);
#else
  shutdown(nat(s), SHUT_RDWR);
#endif
}

Soquete conectarTcp(const std::string& host, uint16_t porta, int timeoutMs, std::string& erro) {
  iniciarRede();
  addrinfo dicas;
  std::memset(&dicas, 0, sizeof dicas);
  dicas.ai_family = AF_UNSPEC;
  dicas.ai_socktype = SOCK_STREAM;
  addrinfo* lista = nullptr;
  int rc = getaddrinfo(host.c_str(), std::to_string(porta).c_str(), &dicas, &lista);
  if (rc != 0 || !lista) {
    erro = "nao resolveu " + host;
    return kSoqueteInvalido;
  }
  Soquete resultado = kSoqueteInvalido;
  for (addrinfo* a = lista; a; a = a->ai_next) {
    Soquete s = static_cast<Soquete>(socket(a->ai_family, a->ai_socktype, a->ai_protocol));
    if (s == kSoqueteInvalido || nat(s) == static_cast<SocketNativo>(-1)) continue;
    configurar(s);
    definirBloqueante(s, false);
    int c = connect(nat(s), a->ai_addr, static_cast<int>(a->ai_addrlen));
    bool ok = (c == 0);
    if (!ok) {
#ifdef _WIN32
      bool andando = (WSAGetLastError() == WSAEWOULDBLOCK);
#else
      bool andando = (errno == EINPROGRESS);
#endif
      if (andando && esperar(s, POLLOUT, timeoutMs) == 1) {
        int e = 0;
#ifdef _WIN32
        int tam = sizeof e;
#else
        socklen_t tam = sizeof e;
#endif
        getsockopt(nat(s), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&e), &tam);
        ok = (e == 0);
      }
    }
    if (ok) {
      definirBloqueante(s, true);
      desativarNagle(s);
      resultado = s;
      break;
    }
    fecharSoquete(s);
  }
  freeaddrinfo(lista);
  if (resultado == kSoqueteInvalido) erro = "nao conectou em " + host + ":" + std::to_string(porta);
  return resultado;
}

Soquete conectarUnix(const std::string& caminho, std::string& erro) {
#ifdef _WIN32
  (void)caminho;
  erro = "soquete Unix indisponivel no Windows";
  return kSoqueteInvalido;
#else
  sockaddr_un end;
  std::memset(&end, 0, sizeof end);
  end.sun_family = AF_UNIX;
  if (caminho.size() >= sizeof end.sun_path) {
    erro = "caminho do soquete grande demais";
    return kSoqueteInvalido;
  }
  std::memcpy(end.sun_path, caminho.c_str(), caminho.size());
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0) {
    erro = textoErro("socket");
    return kSoqueteInvalido;
  }
  configurar(s);
  if (connect(s, reinterpret_cast<sockaddr*>(&end), sizeof end) != 0) {
    erro = textoErro(("nao conectou em " + caminho).c_str());
    close(s);
    return kSoqueteInvalido;
  }
  return s;
#endif
}

Soquete escutarTcp(uint16_t& porta, std::string& erro, const std::string& endereco) {
  iniciarRede();
  SocketNativo s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == static_cast<SocketNativo>(-1)) {
    erro = textoErro("socket");
    return kSoqueteInvalido;
  }
  Soquete r = static_cast<Soquete>(s);
  configurar(r);
  int um = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&um), sizeof um);
  sockaddr_in end;
  std::memset(&end, 0, sizeof end);
  end.sin_family = AF_INET;
  end.sin_port = htons(porta);
  if (inet_pton(AF_INET, endereco.c_str(), &end.sin_addr) != 1) {
    erro = "endereco invalido " + endereco;
    fecharSoquete(r);
    return kSoqueteInvalido;
  }
  if (bind(s, reinterpret_cast<sockaddr*>(&end), sizeof end) != 0 || listen(s, 8) != 0) {
    erro = textoErro("bind/listen");
    fecharSoquete(r);
    return kSoqueteInvalido;
  }
#ifdef _WIN32
  int tam = sizeof end;
#else
  socklen_t tam = sizeof end;
#endif
  getsockname(s, reinterpret_cast<sockaddr*>(&end), &tam);
  porta = ntohs(end.sin_port);
  return r;
}

Soquete escutarUnix(const std::string& caminho, std::string& erro) {
#ifdef _WIN32
  (void)caminho;
  erro = "soquete Unix indisponivel no Windows";
  return kSoqueteInvalido;
#else
  sockaddr_un end;
  std::memset(&end, 0, sizeof end);
  end.sun_family = AF_UNIX;
  if (caminho.size() >= sizeof end.sun_path) {
    erro = "caminho do soquete grande demais";
    return kSoqueteInvalido;
  }
  std::memcpy(end.sun_path, caminho.c_str(), caminho.size());
  unlink(caminho.c_str());
  int s = socket(AF_UNIX, SOCK_STREAM, 0);
  if (s < 0) {
    erro = textoErro("socket");
    return kSoqueteInvalido;
  }
  configurar(s);
  if (bind(s, reinterpret_cast<sockaddr*>(&end), sizeof end) != 0 || listen(s, 8) != 0) {
    erro = textoErro("bind/listen unix");
    close(s);
    return kSoqueteInvalido;
  }
  return s;
#endif
}

Soquete aceitar(Soquete escuta, int timeoutMs) {
  if (esperar(escuta, POLLIN, timeoutMs) != 1) return kSoqueteInvalido;
  SocketNativo c = accept(nat(escuta), nullptr, nullptr);
  if (c == static_cast<SocketNativo>(-1)) return kSoqueteInvalido;
  Soquete r = static_cast<Soquete>(c);
  configurar(r);
  desativarNagle(r);
  return r;
}

int esperarLeitura(Soquete s, int timeoutMs) { return esperar(s, POLLIN, timeoutMs); }

long lerAlgum(Soquete s, uint8_t* buf, size_t max) {
  for (;;) {
#ifdef _WIN32
    int n = recv(nat(s), reinterpret_cast<char*>(buf), static_cast<int>(max), 0);
#else
    ssize_t n = recv(nat(s), buf, max, 0);
#endif
    if (n < 0 && interrompido()) continue;
    return static_cast<long>(n);
  }
}

bool enviarTudo(Soquete s, const void* dados, size_t n) {
  const char* p = static_cast<const char*>(dados);
  while (n > 0) {
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
#ifdef _WIN32
    int w = send(nat(s), p, static_cast<int>(n), flags);
#else
    ssize_t w = send(nat(s), p, n, flags);
#endif
    if (w < 0) {
      if (interrompido()) continue;
      return false;
    }
    if (w == 0) return false;
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

ResultadoIO lerExato(Soquete s, uint8_t* buf, size_t n, int timeoutMs) {
  const uint64_t limite = agoraMonotonicoUs() + static_cast<uint64_t>(timeoutMs) * 1000;
  size_t lido = 0;
  while (lido < n) {
    uint64_t agora = agoraMonotonicoUs();
    if (agora >= limite) return ResultadoIO::Timeout;
    int r = esperar(s, POLLIN, static_cast<int>((limite - agora + 999) / 1000));
    if (r < 0) return ResultadoIO::Erro;
    if (r == 0) return ResultadoIO::Timeout;
    long k = lerAlgum(s, buf + lido, n - lido);
    if (k == 0) return ResultadoIO::Fechado;
    if (k < 0) return ResultadoIO::Erro;
    lido += static_cast<size_t>(k);
  }
  return ResultadoIO::Ok;
}

ResultadoIO lerLinha(Soquete s, size_t maximo, int timeoutMs, std::string& linha) {
  linha.clear();
  const uint64_t limite = agoraMonotonicoUs() + static_cast<uint64_t>(timeoutMs) * 1000;
  // Um byte por vez de proposito: nao pode consumir nada alem do '\n'.
  for (;;) {
    uint8_t b;
    uint64_t agora = agoraMonotonicoUs();
    if (agora >= limite) return ResultadoIO::Timeout;
    int r = esperar(s, POLLIN, static_cast<int>((limite - agora + 999) / 1000));
    if (r < 0) return ResultadoIO::Erro;
    if (r == 0) return ResultadoIO::Timeout;
    long k = lerAlgum(s, &b, 1);
    if (k == 0) return ResultadoIO::Fechado;
    if (k < 0) return ResultadoIO::Erro;
    if (b == '\n') return ResultadoIO::Ok;
    if (linha.size() >= maximo) return ResultadoIO::Erro;  // `maximo` bytes e mais um que nao e o fim
    linha.push_back(static_cast<char>(b));
  }
}

void tornarNaoBloqueante(Soquete s) { definirBloqueante(s, false); }

bool parFechou(Soquete s) {
  if (esperarUmaVez(s, POLLIN, 0) != 1) return false;
  char b;
#ifdef _WIN32
  int n = recv(nat(s), &b, 1, MSG_PEEK);
  if (n < 0) return WSAGetLastError() != WSAEWOULDBLOCK;
#else
  ssize_t n = recv(nat(s), &b, 1, MSG_PEEK | MSG_DONTWAIT);
  if (n < 0) return errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
#endif
  return n == 0;
}

long enviarAlgum(Soquete s, const void* dados, size_t n) {
  for (;;) {
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
#ifdef _WIN32
    int w = send(nat(s), static_cast<const char*>(dados), static_cast<int>(n), flags);
    if (w < 0) return WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1;
#else
    ssize_t w = send(nat(s), dados, n, flags);
    if (w < 0) {
      if (errno == EINTR) continue;
      return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }
#endif
    return static_cast<long>(w);
  }
}

int esperarES(Soquete s, bool querGravar, int timeoutMs, bool& legivel, bool& gravavel) {
  legivel = gravavel = false;
  const short ev = static_cast<short>(POLLIN | (querGravar ? POLLOUT : 0));
  for (;;) {
#ifdef _WIN32
    WSAPOLLFD p;
    p.fd = nat(s);
    p.events = ev;
    p.revents = 0;
    int r = WSAPoll(&p, 1, timeoutMs);
#else
    pollfd p;
    p.fd = nat(s);
    p.events = ev;
    p.revents = 0;
    int r = poll(&p, 1, timeoutMs);
#endif
    if (r < 0) {
      if (interrompido()) continue;
      return -1;
    }
    if (r == 0) return 0;
    legivel = (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
    gravavel = (p.revents & POLLOUT) != 0;
    return 1;
  }
}

void ajustarTimeoutEnvio(Soquete s, int ms) {
#ifdef _WIN32
  DWORD v = static_cast<DWORD>(ms);
  setsockopt(nat(s), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&v), sizeof v);
#else
  timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  setsockopt(nat(s), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

void desativarNagle(Soquete s) {
  int um = 1;
  setsockopt(nat(s), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&um), sizeof um);
}

uint64_t agoraMonotonicoUs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace fmappcam
