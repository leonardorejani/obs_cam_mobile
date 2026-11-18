# Guia de uma página: OBS Cam Mobile

Para quem opera a transmissão. Não precisa de conhecimento técnico: é só seguir na ordem.

## Antes de começar (uma vez por máquina)

**No PC (Windows):**

1. OBS Studio **33.0.1**, 64 bits. Outra versão pode não carregar a extensão.
2. Rode o `OBSCamMobile-Extension-Setup.exe` com o OBS fechado. Ele instala a extensão e o serviço USB da Apple.
   - Copie o instalador por pendrive, pasta da rede ou pela pasta do Google Drive no computador. Assim o Windows não mostra aviso.
   - Se aparecer "O Windows protegeu o computador": **Mais informações > Executar assim mesmo**.

**No iPhone:**

1. Apps instalados: **OBS Cam Mobile**, **SideStore** e **LocalDevVPN**.
2. O **LocalDevVPN** fica conectado e o iPhone no Wi-Fi. É assim que o app renova a assinatura sozinho.

## Toda transmissão

1. Ligue o iPhone no **cabo** do PC e abra o **OBS Cam Mobile**. Deixe o app aberto e na frente.
2. Abra o OBS. A fonte **OBS Cam Mobile Extension** já mostra a imagem.
3. Fonte nova: em Fontes, **+** > **OBS Cam Mobile Extension** > escolha **iPhone ... · Cabo**.
4. No iPhone aparece **NO AR** em vermelho quando a cena dele está no ar.

## No iPhone

- **Lentes:** 0,5 / 1 / 2 / 5 na pílula do lado.
- **Menu:** Câmera (estabilização, travas, branco, foco, EV), Guias (grade, cruz, nível), Info (o que aparece no topo), Ajustes (nome, qualidade, Wi-Fi, validade, gestos, lado do menu).
- **Limpar a tela:** botão do olho riscado ou toque duplo. A imagem enviada ao OBS nunca leva as guias.
- **Estabilização:** deixe **desligada** na transmissão. Ligada, ela atrasa a imagem.
- **Qualidade:** Ajustes > Qualidade. Se o OBS fixou o formato ou o bitrate, aquela linha aparece travada.

## Se der problema

| O que aparece | O que fazer |
|---|---|
| "O app não está aberto no iPhone" | Abra o OBS Cam Mobile e deixe ele na frente |
| "iPhone fora do cabo" | Confira o cabo e toque em **Confiar** no iPhone se ele perguntar |
| "Conectado pelo Wi-Fi (cabo indisponível)" | Está funcionando pelo Wi-Fi. Volta ao cabo sozinho |
| "iPhone ocupado" | Outra fonte ou outro computador está usando esse iPhone |
| Imagem parada | Ela volta sozinha em segundos. Se não voltar, tire e ponha o cabo |

## Assinatura (validade do app)

- A pílula de dias no topo do app mostra quanto falta. Amarela com 2 dias ou menos, vermelha quando vence.
- O SideStore renova sozinho com Wi-Fi e LocalDevVPN ligados.
- Para renovar na hora: SideStore > **My Apps** > **Refresh All**.
