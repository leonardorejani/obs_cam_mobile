// Sobe processos filhos (iphone-falso) para os testes. So POSIX.
#pragma once
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace fmtest {

struct Proc {
  pid_t pid = -1;
  int saida = -1;
  std::string buf;
  bool esperarLinha(const std::string& marca, std::string& linha) {
    for (;;) {
      size_t p = buf.find(marca);
      if (p != std::string::npos) {
        size_t f = buf.find('\n', p);
        if (f != std::string::npos) {
          linha = buf.substr(p, f - p);
          return true;
        }
      }
      char b[1024];
      ssize_t n = read(saida, b, sizeof b);
      if (n <= 0) return false;
      buf.append(b, static_cast<size_t>(n));
    }
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
};

inline Proc iniciar(const std::vector<std::string>& args) {
  int fd[2];
  if (pipe(fd) != 0) std::exit(99);
  Proc p;
  p.pid = fork();
  if (p.pid == 0) {
    dup2(fd[1], 1);
    close(fd[0]);
    close(fd[1]);
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

// iphone-falso com porta livre; devolve a porta.
inline Proc iphoneFalso(const std::string& bin, const std::string& amostra, const std::string& nome,
                        const std::string& modelo, const std::string& id, uint16_t& porta,
                        const std::vector<std::string>& extras = {}) {
  std::vector<std::string> args = {bin, "--arquivo", amostra, "--porta", "0", "--silencioso", "--nome", nome, "--aparelho",
                                   modelo, "--id", id};
  args.insert(args.end(), extras.begin(), extras.end());
  Proc p = iniciar(args);
  std::string l;
  if (!p.esperarLinha("ESCUTANDO ", l)) std::exit(98);
  porta = static_cast<uint16_t>(std::atoi(l.c_str() + 10));
  return p;
}

}  // namespace fmtest
