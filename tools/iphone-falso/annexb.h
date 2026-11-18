// Separa um fluxo H.264 Annex B em unidades de acesso (um quadro cada).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fmappcam {

struct UnidadeAcesso {
  bool chave = false;
  std::vector<uint8_t> dados;
};

struct FluxoH264 {
  std::vector<uint8_t> config;  // SPS+PPS em Annex B
  std::vector<UnidadeAcesso> quadros;
};

inline FluxoH264 separarAnnexB(const std::vector<uint8_t>& b) {
  struct Nal { size_t ini, fim; int tipo; bool primeiraFatia; };
  std::vector<Nal> nals;
  size_t i = 0, n = b.size();
  std::vector<size_t> inicios;  // posicao do 00 00 01 (o zero extra, se houver, fica no NAL anterior)
  while (i + 3 <= n) {
    if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
      inicios.push_back(i);
      i += 3;
    } else {
      ++i;
    }
  }
  for (size_t k = 0; k < inicios.size(); ++k) {
    size_t cab = inicios[k] + 3;
    size_t fim = k + 1 < inicios.size() ? inicios[k + 1] : n;
    // volta o 00 extra do start code de 4 bytes seguinte para ele
    if (k + 1 < inicios.size() && fim > cab && b[fim - 1] == 0) --fim;
    size_t ini = inicios[k];
    if (ini > 0 && b[ini - 1] == 0) --ini;  // start code de 4 bytes
    if (cab >= n) continue;
    Nal nal;
    nal.ini = ini;
    nal.fim = fim;
    nal.tipo = b[cab] & 0x1F;
    nal.primeiraFatia = (cab + 1 < n) && (b[cab + 1] & 0x80);  // first_mb_in_slice == 0
    nals.push_back(nal);
  }
  FluxoH264 f;
  UnidadeAcesso atual;
  bool temVcl = false;
  auto fechar = [&] {
    if (!atual.dados.empty() && temVcl) f.quadros.push_back(std::move(atual));
    atual = UnidadeAcesso();
    temVcl = false;
  };
  for (auto& nal : nals) {
    bool vcl = nal.tipo >= 1 && nal.tipo <= 5;
    if (nal.tipo == 7 || nal.tipo == 8) {
      // SPS/PPS do inicio vao para o config e nao para os quadros
      if (f.quadros.empty() && !temVcl)
        f.config.insert(f.config.end(), b.begin() + static_cast<long>(nal.ini), b.begin() + static_cast<long>(nal.fim));
      if (temVcl) fechar();
      continue;
    }
    bool abreNovo = (vcl && temVcl && nal.primeiraFatia) || (!vcl && temVcl && (nal.tipo == 6 || nal.tipo == 9));
    if (abreNovo) fechar();
    atual.dados.insert(atual.dados.end(), b.begin() + static_cast<long>(nal.ini), b.begin() + static_cast<long>(nal.fim));
    if (vcl) {
      temVcl = true;
      if (nal.tipo == 5) atual.chave = true;
    }
  }
  fechar();
  return f;
}

}  // namespace fmappcam
