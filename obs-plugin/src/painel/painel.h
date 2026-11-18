// Painel (dock) "OBS Cam Mobile" dentro do OBS: lista os iPhones, estado, fps, bateria, temperatura e o botao
// "Adicionar a cena". Qt6 da mesma versao que o OBS carrega. Sem Q_OBJECT (sem moc): so lambdas.
#pragma once

namespace fmcam {
void iniciarPainel();  // quando o OBS termina de carregar, cria o dock
void pararPainel();
}  // namespace fmcam
