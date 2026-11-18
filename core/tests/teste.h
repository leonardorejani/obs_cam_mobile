// Mini framework de testes: TESTE(nome) { CHECAR(...); }, main em teste_main.cc.
#pragma once
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace teste {
struct Caso {
  std::string nome;
  std::function<void()> fn;
};
std::vector<Caso>& casos();
struct Registro {
  Registro(const char* n, std::function<void()> f) { casos().push_back({n, std::move(f)}); }
};
void falhar(const char* arq, int linha, const std::string& msg);
int& falhas();
}  // namespace teste

#define TESTE(nome)                                         \
  static void teste_##nome();                               \
  static teste::Registro registro_##nome(#nome, teste_##nome); \
  static void teste_##nome()

#define CHECAR(cond)                                                       \
  do {                                                                     \
    if (!(cond)) teste::falhar(__FILE__, __LINE__, "CHECAR(" #cond ")");   \
  } while (0)

#define CHECAR_IGUAIS(a, b)                                                          \
  do {                                                                               \
    auto va_ = (a);                                                                  \
    auto vb_ = (b);                                                                  \
    if (!(va_ == vb_)) {                                                             \
      std::ostringstream o_;                                                         \
      o_ << "CHECAR_IGUAIS(" #a ", " #b ") obtido=" << va_ << " esperado=" << vb_;   \
      teste::falhar(__FILE__, __LINE__, o_.str());                                   \
    }                                                                                \
  } while (0)
