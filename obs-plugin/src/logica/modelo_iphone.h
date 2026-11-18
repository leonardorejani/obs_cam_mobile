// Nome comercial do iPhone a partir do identificador de modelo que o app manda no handshake ("iPhone16,2").
// Sem libobs, sem estado: testavel sozinho.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace fmcam {

// "iPhone16,2" -> "iPhone 15 Pro Max". Identificador que nao esta na tabela -> "iPhone (iPhone99,9)" (nunca inventa).
// Texto que ja e um nome (sem virgula, ex.: "iPhone 13" vindo de um iphone-falso) passa como esta. Vazio -> "".
std::string nomeComercialIphone(const std::string& modelo);

// Rotulos estaveis quando ha dois iPhones do mesmo modelo ao mesmo tempo: ordenados pela chave (UDID), o primeiro fica
// com o nome puro e os seguintes ganham " (2)", " (3)". Chave -> rotulo. `base` vazio vira "iPhone".
struct IphoneParaRotulo {
  std::string chave;  // UDID (ou "wifi:<id>" quando so se conhece o Wi-Fi)
  std::string base;   // nomeComercialIphone(...)
};
std::map<std::string, std::string> rotulosIphones(const std::vector<IphoneParaRotulo>& iphones);

}  // namespace fmcam
