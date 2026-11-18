#include "teste.h"

namespace teste {
std::vector<Caso>& casos() {
  static std::vector<Caso> c;
  return c;
}
int& falhas() {
  static int f = 0;
  return f;
}
void falhar(const char* arq, int linha, const std::string& msg) {
  ++falhas();
  std::cerr << "  FALHOU " << arq << ":" << linha << " " << msg << "\n";
}
}  // namespace teste

int main() {
  int ruins = 0;
  for (auto& c : teste::casos()) {
    int antes = teste::falhas();
    std::cout << "[ RODANDO ] " << c.nome << std::endl;
    c.fn();
    bool ok = teste::falhas() == antes;
    if (!ok) ++ruins;
    std::cout << (ok ? "[   OK    ] " : "[ FALHOU  ] ") << c.nome << std::endl;
  }
  std::cout << teste::casos().size() - static_cast<size_t>(ruins) << "/" << teste::casos().size() << " testes ok\n";
  return ruins ? 1 : 0;
}
