// Estado do modulo: o unico RegistroAparelhos do processo (uma conexao Listen no usbmuxd para todas as fontes)
// e a lista das fontes vivas, para avisar o OBS quando um iPhone e plugado ou desplugado.
#pragma once
#include "aparelhos.h"

struct obs_source;
typedef struct obs_source obs_source_t;

namespace fmcam {

RegistroAparelhos* registroGlobal();  // nulo fora de obs_module_load..obs_module_unload

// Fontes vivas (para recarregar a lista de aparelhos nas propriedades).
void fonteViva(obs_source_t* s, bool viva);
void recarregarPropriedades();

void registrarFonte();  // fonte.cc

}  // namespace fmcam
