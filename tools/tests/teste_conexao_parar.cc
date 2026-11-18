// Conexao::parar() tem que voltar rapido em qualquer situacao (defeito achado em 18/11/2026: ~2,8 s com connect em IP
// mudo, ~4 s com usbmuxd mudo, e esperava o callback do usuario). Tudo em loopback, sem iPhone e sem rede de verdade.
// So POSIX. Saida: uma linha por caso com o tempo de parar().
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <vector>

#include "conexao.h"
#include "usbmux.h"

using namespace fmappcam;
using Rel = std::chrono::steady_clock;

namespace {

constexpr double kLimiteMs = 250;  // a meta e < 100 ms; folga para sanitizers e maquina carregada
int g_falhas = 0;

double ms(Rel::time_point a) { return std::chrono::duration<double, std::milli>(Rel::now() - a).count(); }
void esperarMs(int n) { std::this_thread::sleep_for(std::chrono::milliseconds(n)); }

void checar(bool ok, const std::string& msg) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FALHOU", msg.c_str());
  std::fflush(stdout);
  if (!ok) ++g_falhas;
}

double medirParar(Conexao& c, const std::string& caso) {
  auto t = Rel::now();
  c.parar();
  double d = ms(t);
  char b[160];
  std::snprintf(b, sizeof b, "%s: parar() levou %.0f ms (limite %.0f)", caso.c_str(), d, kLimiteMs);
  checar(d < kLimiteMs, b);
  return d;
}

// Porta TCP em que connect() fica pendurado: a fila de conexoes do listen() esta cheia e o SYN e descartado.
struct PortaMuda {
  int escuta = -1;
  std::vector<int> enchendo;
  uint16_t porta = 0;
  bool pronta = false;
  PortaMuda() {
    escuta = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(escuta, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(escuta, 0) != 0) return;
    socklen_t n = sizeof a;
    getsockname(escuta, reinterpret_cast<sockaddr*>(&a), &n);
    porta = ntohs(a.sin_port);
    int pendentes = 0;
    for (int i = 0; i < 2000 && pendentes < 3; ++i) {
      int s = socket(AF_INET, SOCK_STREAM, 0);
      fcntl(s, F_SETFL, O_NONBLOCK);
      connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
      pollfd p{s, POLLOUT, 0};
      if (poll(&p, 1, 100) == 0) ++pendentes;
      enchendo.push_back(s);
    }
    pronta = pendentes >= 1;
  }
  ~PortaMuda() {
    for (int s : enchendo) close(s);
    if (escuta >= 0) close(escuta);
  }
};

// iPhone minimo: aceita, le o handshake, responde ok e manda um status a cada 20 ms ate o cliente sair.
struct IphoneMinimo {
  Soquete escuta = kSoqueteInvalido;
  uint16_t porta = 0;
  bool silencioso;  // aceita e nunca responde o handshake
  std::atomic<bool> fim{false};
  std::thread th;
  std::vector<std::thread> filhos;
  explicit IphoneMinimo(bool mudo = false) : silencioso(mudo) {
    std::string erro;
    escuta = escutarTcp(porta, erro);
    th = std::thread([this] {
      while (!fim) {
        Soquete c = aceitar(escuta, 50);
        if (c == kSoqueteInvalido) continue;
        filhos.emplace_back([this, c] {
          std::string linha;
          if (lerLinha(c, kLinhaHandshakeMaxima, 3000, linha) == ResultadoIO::Ok && !silencioso) {
            RespostaHandshake r;
            r.ok = true;
            r.aparelho = "falso";
            r.w = 1920;
            r.h = 1080;
            r.fps = 30;
            std::string l = montarRespostaHandshake(r);
            enviarTudo(c, l.data(), l.size());
            uint64_t t = 0;
            while (!fim) {
              auto b = serializarPacote(Pacote{kTipoStatus, t += 20000, {'{', '}'}});
              if (!enviarTudo(c, b.data(), b.size())) break;
              uint8_t x[512];
              if (esperarLeitura(c, 20) == 1 && lerAlgum(c, x, sizeof x) <= 0) break;
            }
          } else {
            while (!fim && esperarLeitura(c, 50) >= 0) {
              uint8_t x[64];
              if (esperarLeitura(c, 0) == 1 && lerAlgum(c, x, sizeof x) <= 0) break;
            }
          }
          fecharSoquete(c);
        });
      }
    });
  }
  ~IphoneMinimo() {
    fim = true;
    th.join();
    for (auto& f : filhos) f.join();
    fecharSoquete(escuta);
  }
};

// usbmuxd que fica mudo: modo 0 nao responde nada (nem ListDevices), modo 1 lista um aparelho mas nao responde Connect.
struct MuxMudo {
  std::string caminho;
  Soquete escuta = kSoqueteInvalido;
  int modo;
  std::atomic<bool> fim{false};
  std::thread th;
  std::vector<std::thread> filhos;
  MuxMudo(std::string c, int m) : caminho(std::move(c)), modo(m) {
    std::string erro;
    escuta = escutarUnix(caminho, erro);
    th = std::thread([this] {
      while (!fim) {
        Soquete c = aceitar(escuta, 50);
        if (c == kSoqueteInvalido) continue;
        filhos.emplace_back([this, c] {
          std::string xml, erro;
          uint32_t tag = 0;
          NoPlist p;
          std::string tipo;
          if (modo == 1 && lerMensagemUsbmux(c, xml, &tag, 2000, erro) && lerPlist(xml, p) &&
              p.buscarTexto("MessageType", tipo) && tipo == "ListDevices") {
            auto m = montarMensagemUsbmux(tag, montarPlistDeviceList({{1, "U1", "USB"}}));
            enviarTudo(c, m.data(), m.size());
          }  // qualquer outra mensagem (o Connect) fica sem resposta
          while (!fim) {  // le e ignora tudo ate o cliente sair
            uint8_t x[256];
            if (esperarLeitura(c, 50) == 1 && lerAlgum(c, x, sizeof x) <= 0) break;
          }
          fecharSoquete(c);
        });
      }
    });
  }
  ~MuxMudo() {
    fim = true;
    th.join();
    for (auto& f : filhos) f.join();
    fecharSoquete(escuta);
    unlink(caminho.c_str());
  }
};

}  // namespace

int main() {
  signal(SIGPIPE, SIG_IGN);
  const std::string base = "/tmp/fmcam_parar_" + std::to_string(getpid());

  {  // 1. connect pendurado
    PortaMuda mud;
    if (!mud.pronta) {
      std::puts("  [pulado] nao consegui deixar um connect pendurado neste sistema");
    } else {
      OpcoesConexao op;
      op.host = "127.0.0.1";
      op.porta = mud.porta;
      Conexao c(op);
      c.iniciar();
      esperarMs(300);  // ja esta dentro do connect (timeout de 3 s)
      medirParar(c, "connect pendurado");
    }
  }
  {  // 2. usbmuxd que nao responde ListDevices
    MuxMudo mux(base + "_a.sock", 0);
    OpcoesConexao op;
    op.usb = true;
    op.usbmuxCaminho = mux.caminho;
    op.udid = "U1";
    Conexao c(op);
    c.iniciar();
    esperarMs(300);
    medirParar(c, "usbmuxd mudo no ListDevices");
  }
  {  // 3. usbmuxd que lista mas nao responde Connect
    MuxMudo mux(base + "_b.sock", 1);
    OpcoesConexao op;
    op.usb = true;
    op.usbmuxCaminho = mux.caminho;
    op.udid = "U1";
    Conexao c(op);
    c.iniciar();
    esperarMs(300);
    medirParar(c, "usbmuxd mudo no Connect");
  }
  {  // 4. iPhone que aceita e nao responde o handshake
    IphoneMinimo f(/*mudo=*/true);
    OpcoesConexao op;
    op.porta = f.porta;
    Conexao c(op);
    c.iniciar();
    esperarMs(300);
    medirParar(c, "handshake sem resposta");
  }
  {  // 5. conectado e recebendo
    IphoneMinimo f;
    OpcoesConexao op;
    op.porta = f.porta;
    Conexao c(op);
    std::atomic<int> pacotes{0};
    c.definirCallbacks([&](const Pacote&, uint64_t) { ++pacotes; }, nullptr);
    c.iniciar();
    esperarMs(400);
    checar(c.conectada() && pacotes > 3, "conectou e recebeu pacotes antes de parar");
    medirParar(c, "conectado");
    checar(!c.conectada(), "conectada() falso depois de parar");
  }
  {  // 6. callback lento em andamento: parar() nao espera, e nada novo comeca depois dele
    struct Estado {
      std::atomic<int> comecou{0};
      std::atomic<int> estados{0};
      std::atomic<int64_t> ultimoInicioUs{0};
      std::atomic<int64_t> estadoDepoisUs{0};
    };
    auto st = std::make_shared<Estado>();  // o callback so usa isto: sobrevive a Conexao e ao escopo
    IphoneMinimo f;
    auto agoraUs = [] { return std::chrono::duration_cast<std::chrono::microseconds>(Rel::now().time_since_epoch()).count(); };
    int64_t retornoUs;
    {
      OpcoesConexao op;
      op.porta = f.porta;
      Conexao c(op);
      c.definirCallbacks(
          [st, agoraUs](const Pacote&, uint64_t) {
            st->ultimoInicioUs = agoraUs();
            ++st->comecou;
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
          },
          [st, agoraUs](EstadoConexao, const std::string&) {
            ++st->estados;
            st->estadoDepoisUs = agoraUs();
          });
      c.iniciar();
      esperarMs(700);  // callback de pacote ja esta dormindo
      checar(st->comecou == 1, "callback de pacote em andamento");
      medirParar(c, "callback lento em andamento");
      retornoUs = agoraUs();
    }  // destrutor com o callback ainda rodando
    esperarMs(2000);
    checar(st->comecou == 1, "nenhum callback de pacote novo depois de parar()");
    checar(st->estadoDepoisUs <= retornoUs, "nenhuma notificacao de estado depois de parar() ter retornado");
  }
  {  // 7. iniciar/parar em sequencia
    double pior = 0;
    for (int i = 0; i < 100; ++i) {
      OpcoesConexao op;
      op.porta = 1;
      Conexao c(op);
      c.iniciar();
      auto t = Rel::now();
      c.parar();
      pior = std::max(pior, ms(t));
    }
    char b[96];
    std::snprintf(b, sizeof b, "100x iniciar/parar: pior parar() = %.0f ms", pior);
    checar(pior < kLimiteMs, b);
  }
  {  // 8. parar() de duas threads, com comandos e consultas concorrentes
    IphoneMinimo f;
    OpcoesConexao op;
    op.porta = f.porta;
    Conexao c(op);
    std::atomic<bool> fim{false};
    std::thread a([&] { while (!fim) c.enviarComando("{\"cmd\":\"keyframe\"}"); });
    std::thread b([&] {
      while (!fim) {
        (void)c.resposta();
        (void)c.sincronizada();
        (void)c.udidAtual();
        int64_t x;
        c.atrasoUs(1, 2, x);
      }
    });
    c.iniciar();
    esperarMs(400);
    std::thread p1([&] { c.parar(); });
    std::thread p2([&] { c.parar(); });
    p1.join();
    p2.join();
    fim = true;
    a.join();
    b.join();
    checar(!c.conectada(), "dois parar() simultaneos terminam");
    c.iniciar();  // pode reiniciar depois de parar
    esperarMs(300);
    checar(c.conectada(), "reiniciou depois de parar()");
    medirParar(c, "segunda rodada");
  }

  std::printf("conexao_parar: %s\n", g_falhas ? "FALHOU" : "OK");
  return g_falhas ? 1 : 0;
}
