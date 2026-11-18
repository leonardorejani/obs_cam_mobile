#include "modelo_iphone.h"

#include <cctype>

namespace fmcam {

namespace {
struct Modelo {
  const char* id;
  const char* nome;
};
// Fonte: lista de identificadores de aparelhos Apple de Adam Wolf (gist adamawolf/3048717, "Apple_mobile_device_types"),
// conferida em 18/11/2026. Vai de iPhone12,1 (iPhone 11) ate iPhone18,x (linha iPhone 17, inclui o Air e o 17e).
// Identificador fora da tabela nao e adivinhado: cai em "iPhone (<codigo>)".
const Modelo kModelos[] = {
    {"iPhone12,1", "iPhone 11"},         {"iPhone12,3", "iPhone 11 Pro"},     {"iPhone12,5", "iPhone 11 Pro Max"},
    {"iPhone12,8", "iPhone SE (2020)"},
    {"iPhone13,1", "iPhone 12 mini"},    {"iPhone13,2", "iPhone 12"},         {"iPhone13,3", "iPhone 12 Pro"},
    {"iPhone13,4", "iPhone 12 Pro Max"},
    {"iPhone14,2", "iPhone 13 Pro"},     {"iPhone14,3", "iPhone 13 Pro Max"}, {"iPhone14,4", "iPhone 13 mini"},
    {"iPhone14,5", "iPhone 13"},         {"iPhone14,6", "iPhone SE (2022)"},
    {"iPhone14,7", "iPhone 14"},         {"iPhone14,8", "iPhone 14 Plus"},    {"iPhone15,2", "iPhone 14 Pro"},
    {"iPhone15,3", "iPhone 14 Pro Max"},
    {"iPhone15,4", "iPhone 15"},         {"iPhone15,5", "iPhone 15 Plus"},    {"iPhone16,1", "iPhone 15 Pro"},
    {"iPhone16,2", "iPhone 15 Pro Max"},
    {"iPhone17,1", "iPhone 16 Pro"},     {"iPhone17,2", "iPhone 16 Pro Max"}, {"iPhone17,3", "iPhone 16"},
    {"iPhone17,4", "iPhone 16 Plus"},    {"iPhone17,5", "iPhone 16e"},
    {"iPhone18,1", "iPhone 17 Pro"},     {"iPhone18,2", "iPhone 17 Pro Max"}, {"iPhone18,3", "iPhone 17"},
    {"iPhone18,4", "iPhone Air"},        {"iPhone18,5", "iPhone 17e"},
};
}  // namespace

std::string nomeComercialIphone(const std::string& modelo) {
  if (modelo.empty()) return "";
  for (auto& m : kModelos)
    if (modelo == m.id) return m.nome;
  if (modelo.find(',') == std::string::npos) return modelo;  // ja e um nome
  return "iPhone (" + modelo + ")";
}

std::map<std::string, std::string> rotulosIphones(const std::vector<IphoneParaRotulo>& iphones) {
  std::map<std::string, std::string> chaves;  // ordena pela chave
  for (auto& i : iphones) chaves[i.chave] = i.base.empty() ? "iPhone" : i.base;
  std::map<std::string, int> contagem;
  std::map<std::string, std::string> r;
  for (auto& p : chaves) {
    int n = ++contagem[p.second];
    r[p.first] = n == 1 ? p.second : p.second + " (" + std::to_string(n) + ")";
  }
  return r;
}

}  // namespace fmcam
