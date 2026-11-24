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

int main(int argc, char** argv) {
  // Argumento opcional: roda so os testes cujo nome contem o texto (ex.: ./teste_logica_fonte duas_fontes).
  const std::string filtro = argc > 1 ? argv[1] : "";
  int ruins = 0;
  size_t rodados = 0;
  for (auto& c : teste::casos()) {
    if (!filtro.empty() && c.nome.find(filtro) == std::string::npos) continue;
    ++rodados;
    int antes = teste::falhas();
    std::cout << "[ RODANDO ] " << c.nome << std::endl;
    c.fn();
    bool ok = teste::falhas() == antes;
    if (!ok) ++ruins;
    std::cout << (ok ? "[   OK    ] " : "[ FALHOU  ] ") << c.nome << std::endl;
  }
  std::cout << rodados - static_cast<size_t>(ruins) << "/" << rodados << " testes ok\n";
  return ruins ? 1 : 0;
}
