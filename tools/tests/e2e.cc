// Testes ponta a ponta: sobe iphone-falso (e usbmuxd-falso) como processos e roda o visor contra eles.
// Uso: teste_e2e <modo> <iphone-falso> <visor> <usbmuxd-falso> <amostra.h264> <pasta-de-trabalho>
// Modos: tcp, usb, desvio, perda, trava, multi, reconexao, lento. So POSIX.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "protocolo.h"
#include "rede.h"

using namespace fmappcam;

namespace {

int g_falhas = 0;
void checar(bool ok, const std::string& msg) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FALHOU", msg.c_str());
  if (!ok) ++g_falhas;
}

struct Proc {
  pid_t pid = -1;
  int saida = -1;  // leitura do stdout
  std::string buffer;

  bool lerMais() {
    char b[4096];
    ssize_t n = read(saida, b, sizeof b);
    if (n <= 0) return false;
    buffer.append(b, static_cast<size_t>(n));
    return true;
  }
  // le ate achar `marca`; devolve a linha
  bool esperarLinha(const std::string& marca, std::string& linha) {
    for (;;) {
      size_t p = buffer.find(marca);
      if (p != std::string::npos) {
        size_t f = buffer.find('\n', p);
        if (f != std::string::npos) {
          linha = buffer.substr(p, f - p);
          return true;
        }
      }
      if (!lerMais()) return false;
    }
  }
  std::string lerTudo() {
    while (lerMais()) {}
    return buffer;
  }
  void matar() {
    if (pid > 0) {
      kill(pid, SIGTERM);
      int st;
      waitpid(pid, &st, 0);
      pid = -1;
    }
    if (saida >= 0) close(saida);
    saida = -1;
  }
  int esperar() {
    int st = 0;
    waitpid(pid, &st, 0);
    pid = -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
  }
};

Proc iniciar(const std::vector<std::string>& args, bool silenciarErro) {
  int fd[2];
  if (pipe(fd) != 0) std::exit(99);
  Proc p;
  p.pid = fork();
  if (p.pid == 0) {
    dup2(fd[1], 1);
    close(fd[0]);
    close(fd[1]);
    if (silenciarErro) {
      int dn = open("/dev/null", O_WRONLY);
      dup2(dn, 2);
    }
    std::vector<char*> av;
    for (auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    execv(av[0], av.data());
    _exit(127);
  }
  close(fd[1]);
  p.saida = fd[0];
  return p;
}

Proc iphoneFalso(const std::string& bin, const std::string& amostra, std::vector<std::string> extra, uint16_t& porta) {
  std::vector<std::string> a = {bin, "--arquivo", amostra, "--porta", "0", "--silencioso"};
  a.insert(a.end(), extra.begin(), extra.end());
  Proc p = iniciar(a, false);
  std::string l;
  if (!p.esperarLinha("ESCUTANDO ", l)) {
    std::puts("iphone-falso nao subiu");
    std::exit(98);
  }
  porta = static_cast<uint16_t>(std::atoi(l.c_str() + 10));
  return p;
}

struct Resumo {
  ObjetoJson j;
  double n(const char* k) const {
    auto it = j.find(k);
    return it == j.end() ? -1e18 : it->second.n;
  }
  std::string s(const char* k) const {
    auto it = j.find(k);
    return it == j.end() ? "" : it->second.s;
  }
};

std::vector<Resumo> resumos(const std::string& saida) {
  std::vector<Resumo> v;
  size_t i = 0;
  while ((i = saida.find("RESUMO ", i)) != std::string::npos) {
    size_t f = saida.find('\n', i);
    Resumo r;
    if (lerObjetoJson(saida.substr(i + 7, f - i - 7), r.j)) v.push_back(r);
    i = f == std::string::npos ? saida.size() : f;
  }
  return v;
}

bool existe(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && st.st_size > 100;
}

std::string f1(double d) {
  char b[32];
  std::snprintf(b, sizeof b, "%.2f", d);
  return b;
}

void conferirSaudavel(const Resumo& r, const std::string& rot) {
  checar(r.n("perdidos") == 0, rot + " perdidos=0 (" + f1(r.n("perdidos")) + ")");
  checar(r.n("erros_decodificacao") == 0, rot + " erros_decodificacao=0");
  checar(r.n("fps") >= 28 && r.n("fps") <= 32, rot + " fps entre 28 e 32 (" + f1(r.n("fps")) + ")");
  checar(r.n("amostras_atraso") > 100, rot + " atraso medido em varios quadros (" + f1(r.n("amostras_atraso")) + ")");
  checar(r.n("atraso_ms_p95") < 50 && r.n("atraso_ms_p95") > -5, rot + " atraso p95 < 50 ms (" + f1(r.n("atraso_ms_p95")) + ")");
  checar(r.n("largura") == 1920 && r.n("altura") == 1080, rot + " 1920x1080 vindo do handshake");
}

// Cliente cru do iphone-falso: faz o handshake e devolve o soquete e a linha de resposta.
Soquete clienteCru(uint16_t porta, const PedidoHandshake& ped, std::string& resposta, int rcvbuf = 0) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (rcvbuf > 0) setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(porta);
  if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    close(s);
    return kSoqueteInvalido;
  }
  std::string h = montarPedidoHandshake(ped);
  enviarTudo(s, h.data(), h.size());
  if (lerLinha(s, kLinhaHandshakeMaxima, 3000, resposta) != ResultadoIO::Ok) resposta.clear();
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 7) {
    std::puts("uso: teste_e2e modo iphone-falso visor usbmuxd-falso amostra pasta");
    return 2;
  }
  const std::string modo = argv[1], falso = argv[2], visor = argv[3], umux = argv[4], amostra = argv[5], pasta = argv[6];
  mkdir(pasta.c_str(), 0755);
  const std::string base = pasta + "/" + modo;
  mkdir(base.c_str(), 0755);
  const std::string sock = "/tmp/fmcam_e2e_" + modo + "_" + std::to_string(getpid()) + ".sock";
  const std::string seg = "8";
  signal(SIGPIPE, SIG_IGN);

  if (modo == "tcp" || modo == "desvio" || modo == "perda" || modo == "trava") {
    uint16_t porta;
    std::vector<std::string> ex;
    if (modo == "desvio") ex = {"--desvio-relogio-us", "123456789012"};  // iPhone 34 h na frente
    if (modo == "perda") ex = {"--perder-quadro", "100"};
    // caso real do iPhone em 18/11/2026: conexao aberta, quadros parados. Sem contar isso o visor dizia perdidos=0
    if (modo == "trava") ex = {"--travar-apos-s", "2", "--travar-por-s", "4"};
    Proc f = iphoneFalso(falso, amostra, ex, porta);
    const std::string png = base + "/png";
    Proc v = iniciar({visor, "--tcp", "127.0.0.1:" + std::to_string(porta), "--segundos", seg, "--png-dir", png,
                      "--png-cada", "60"}, true);
    std::string out = v.lerTudo();
    int rc = v.esperar();
    f.matar();
    std::fputs(out.c_str(), stdout);
    auto rs = resumos(out);
    checar(rs.size() == 1, "um RESUMO");
    if (rs.size() == 1) {
      const Resumo& r = rs[0];
      if (modo == "trava") {
        checar(rc == 1, "visor sai com codigo 1 quando o video para (rc=" + std::to_string(rc) + ")");
        checar(r.n("segundos_sem_video") >= 2, "tempo sem video contado (" + f1(r.n("segundos_sem_video")) + " s)");
        checar(r.n("reconexoes") >= 1, "reconectou depois da trava");
        checar(r.n("quadros") > 60, "voltou a receber video depois da trava");
      } else if (modo == "perda") {
        checar(rc == 1, "visor sai com codigo 1 ao detectar perda (rc=" + std::to_string(rc) + ")");
        checar(r.n("perdidos") >= 1, "perda detectada (" + f1(r.n("perdidos")) + ")");
        checar(r.n("quadros") > 150, "recuperou e seguiu decodificando (" + f1(r.n("quadros")) + " quadros)");
      } else {
        checar(rc == 0, "visor sai com codigo 0 (rc=" + std::to_string(rc) + ")");
        conferirSaudavel(r, modo);
        checar(r.n("pngs") >= 1 && existe(png + "/quadro_000001.png"), "PNG gravado em " + png);
        checar(r.n("reconexoes") == 0, "sem reconexao");
      }
    }
  } else if (modo == "usb") {
    uint16_t porta;
    Proc f = iphoneFalso(falso, amostra, {"--nome", "Camera Teste", "--id", "uuid-teste-1"}, porta);
    Proc u = iniciar({umux, "--socket", sock, "--aparelho", "UDID-A=127.0.0.1:" + std::to_string(porta), "--rede", "UDID-REDE"}, true);
    std::string l;
    u.esperarLinha("PRONTO", l);
    // 1) listar
    Proc lst = iniciar({visor, "--usb", "--usbmux-socket", sock}, true);
    std::string so = lst.lerTudo();
    lst.esperar();
    std::fputs(so.c_str(), stdout);
    checar(so.find("UDID-A") != std::string::npos && so.find("UDID-REDE") == std::string::npos && so.find("1 aparelho") != std::string::npos,
           "--usb lista so o aparelho USB");
    // 2) transmitir pelo cabo
    const std::string png = base + "/png";
    Proc v = iniciar({visor, "--usb", "--udid", "UDID-A", "--usbmux-socket", sock, "--segundos", seg, "--png-dir", png,
                      "--png-cada", "60"}, true);
    std::string out = v.lerTudo();
    int rc = v.esperar();
    f.matar();
    u.matar();
    std::fputs(out.c_str(), stdout);
    auto rs = resumos(out);
    checar(rs.size() == 1, "um RESUMO");
    if (rs.size() == 1) {
      checar(rc == 0, "visor sai com codigo 0 (rc=" + std::to_string(rc) + ")");
      conferirSaudavel(rs[0], "usb");
      checar(rs[0].s("nome") == "Camera Teste" && rs[0].s("id") == "uuid-teste-1", "nome e id do handshake chegaram");
      checar(rs[0].n("pngs") >= 1, "PNG gravado");
    }
  } else if (modo == "multi") {
    const int N = 4;
    uint16_t portas[N];
    Proc fs[N];
    std::vector<std::string> args = {umux, "--socket", sock};
    for (int i = 0; i < N; ++i) {
      fs[i] = iphoneFalso(falso, amostra, {"--nome", "Camera " + std::to_string(i + 1), "--id", "uuid-" + std::to_string(i + 1),
                                           "--desvio-relogio-us", std::to_string(1000000000LL * (i + 1))}, portas[i]);
      args.push_back("--aparelho");
      args.push_back("UDID-" + std::to_string(i + 1) + "=127.0.0.1:" + std::to_string(portas[i]));
    }
    args.push_back("--rede");
    args.push_back("UDID-REDE");
    Proc u = iniciar(args, true);
    std::string l;
    u.esperarLinha("PRONTO", l);
    const std::string png = base + "/png";
    Proc v = iniciar({visor, "--usb", "--todos", "--usbmux-socket", sock, "--segundos", "12", "--png-dir", png,
                      "--png-cada", "90", "--permitir-perdas", "--permitir-quedas"}, true);
    // derruba o aparelho 3 no meio, os outros nao podem sentir
    std::thread derruba([&] {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      fs[2].matar();
    });
    std::string out = v.lerTudo();
    int rc = v.esperar();
    derruba.join();
    for (int i = 0; i < N; ++i) fs[i].matar();
    u.matar();
    std::fputs(out.c_str(), stdout);
    auto rs = resumos(out);
    checar(rs.size() == 4, "4 RESUMOs, um por aparelho (" + std::to_string(rs.size()) + ")");
    double quadrosVivos = 0;
    for (auto& r : rs) {
      std::string rot = r.s("aparelho");
      if (rot == "UDID-3") continue;
      checar(r.s("nome") == "Camera " + rot.substr(5), rot + " nome do handshake: " + r.s("nome"));
      conferirSaudavel(r, rot);
      checar(r.n("reconexoes") == 0, rot + " nao reconectou");
      quadrosVivos = r.n("quadros");
    }
    for (auto& r : rs)
      if (r.s("aparelho") == "UDID-3") {
        checar(r.n("quadros") > 60 && r.n("quadros") < quadrosVivos - 100, "derrubado parou no meio (" + f1(r.n("quadros")) + " quadros)");
        checar(r.n("reconexoes") >= 1, "derrubado tentou reconectar");
        checar(r.n("perdidos") == 0 && r.n("erros_decodificacao") == 0, "queda nao virou perda nem erro");
      }
    checar(rc == 0, "visor sai com codigo 0 (rc=" + std::to_string(rc) + ")");
    checar(existe(png + "/UDID-1/quadro_000001.png") && existe(png + "/UDID-4/quadro_000001.png"), "PNG por aparelho em subpasta");
  } else if (modo == "reconexao") {
    // Defeito de 18/11/2026: reconectar logo depois de fechar tomava "ocupado" porque o iphone-falso so percebia o
    // fechamento no proximo poll. Com a maquina ocupada a janela aparece. Meta: 0 ocupado em 500 reconexoes.
    uint16_t porta;
    Proc f = iphoneFalso(falso, amostra, {}, porta);
    std::atomic<bool> gira{true};
    std::vector<std::thread> carga;
    const unsigned nCarga = 6 * std::max(2u, std::thread::hardware_concurrency());
    for (unsigned i = 0; i < nCarga; ++i)
      carga.emplace_back([&] {
        volatile uint64_t x = 0;
        while (gira) x = x + 1;
      });
    int ok = 0, ocupado = 0, outro = 0;
    for (int i = 0; i < 500; ++i) {
      std::string resp;
      Soquete c = clienteCru(porta, PedidoHandshake{}, resp);
      if (c == kSoqueteInvalido) { ++outro; continue; }
      if (resp.find("\"ok\":true") != std::string::npos) ++ok;
      else if (resp.find("ocupado") != std::string::npos) ++ocupado;
      else ++outro;
      fecharSoquete(c);
    }
    gira = false;
    for (auto& t : carga) t.join();
    checar(ocupado == 0 && outro == 0 && ok == 500,
           "500 reconexoes imediatas: ok=" + std::to_string(ok) + " ocupado=" + std::to_string(ocupado) + " outro=" + std::to_string(outro));
    // dono vivo continua valendo: o segundo recebe ocupado e o primeiro nao e incomodado
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::string ra, rb;
    Soquete a = clienteCru(porta, PedidoHandshake{}, ra);
    checar(ra.find("\"ok\":true") != std::string::npos, "primeiro cliente entra");
    auto t = std::chrono::steady_clock::now();
    Soquete b = clienteCru(porta, PedidoHandshake{}, rb);
    double ate = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    checar(rb.find("ocupado") != std::string::npos, "segundo cliente recebe ocupado");
    checar(ate < 1500, "ocupado chega rapido (" + f1(ate) + " ms)");
    fecharSoquete(b);
    auto ping = serializarPacote(criarPing(12345));
    enviarTudo(a, ping.data(), ping.size());
    ParserPacotes ps;
    bool pong = false;
    uint8_t buf[65536];
    for (int i = 0; i < 50 && !pong; ++i) {
      if (esperarLeitura(a, 100) != 1) continue;
      long n = lerAlgum(a, buf, sizeof buf);
      if (n <= 0) break;
      ps.alimentar(buf, static_cast<size_t>(n));
      Pacote p;
      while (ps.proximo(p)) pong = pong || (p.tipo == kTipoPong && p.dados.size() == 8 && lerU64BE(p.dados.data()) == 12345);
    }
    checar(pong, "primeiro cliente segue vivo e recebe o pong");
    fecharSoquete(a);
    f.matar();
  } else if (modo == "lento") {
    // Cliente que para de ler com a conexao aberta. Politica da secao 4 do protocolo: o iPhone nao derruba esse cliente
    // (so 5 s sem receber DADOS dele derrubam), nao acumula video velho e volta num quadro-chave.
    uint16_t porta;
    Proc f = iphoneFalso(falso, amostra, {}, porta);
    PedidoHandshake ped;
    ped.bitrate = 1000000;  // fila de 1 MiB
    std::string resp;
    Soquete c = clienteCru(porta, ped, resp, 4096);
    checar(resp.find("\"ok\":true") != std::string::npos, "handshake aceito");
    bool vivo = true;
    for (int i = 0; i < 9 && vivo; ++i) {  // 9 s sem ler nada, so ping
      auto ping = serializarPacote(criarPing(1000 + static_cast<uint64_t>(i)));
      vivo = enviarTudo(c, ping.data(), ping.size());
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    checar(vivo, "cliente que nao le segue conectado por 9 s");
    ParserPacotes ps;
    uint64_t bytes = 0, quadros = 0, chaves = 0, pongs = 0;
    bool fechado = false;
    uint8_t buf[65536];
    auto ini = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - ini < std::chrono::seconds(3)) {
      if (esperarLeitura(c, 50) != 1) continue;
      long n = lerAlgum(c, buf, sizeof buf);
      if (n <= 0) { fechado = true; break; }
      bytes += static_cast<uint64_t>(n);
      ps.alimentar(buf, static_cast<size_t>(n));
      Pacote p;
      while (ps.proximo(p)) {
        if (p.tipo == kTipoQuadro) {
          ++quadros;
          if (!p.dados.empty() && (p.dados[0] & 1)) ++chaves;
        } else if (p.tipo == kTipoPong) {
          ++pongs;
        }
      }
    }
    checar(!fechado && !ps.erro(), "conexao aberta e fluxo integro ao voltar a ler");
    checar(pongs >= 1, "pong dos pings chegou (" + std::to_string(pongs) + ")");
    checar(chaves >= 1, "voltou com quadro-chave (" + std::to_string(chaves) + ")");
    // 12 s de video sao ~360 quadros. Com descarte chegam bem menos (fila de 1 MiB + o tempo real de 3 s)
    checar(quadros >= 30 && quadros < 200, "video velho descartado: " + std::to_string(quadros) + " quadros de ~360 gerados");
    std::printf("  bytes lidos ao voltar: %.2f MiB\n", bytes / 1048576.0);
    checar(bytes < 7u * 1048576u, "fila limitada (" + f1(bytes / 1048576.0) + " MiB lidos)");
    fecharSoquete(c);
    f.matar();
  } else {
    std::puts("modo desconhecido");
    return 2;
  }
  std::printf("%s: %s\n", modo.c_str(), g_falhas ? "FALHOU" : "OK");
  return g_falhas ? 1 : 0;
}
