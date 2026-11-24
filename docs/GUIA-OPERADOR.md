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

## No OBS (extensão 1.0.4)

**Propriedades da fonte** (clique duas vezes na fonte):

- **Status ao vivo**, logo abaixo do estado: canal (Cabo ou Wi-Fi), formato, atraso do iPhone até o OBS em ms, quadros por segundo, bitrate, bateria e temperatura. Atualiza sozinho a cada 2 s enquanto o iPhone manda vídeo.
- **Modo de recepção:** **Menor atraso** (padrão, use no cabo) ou **Mais estável** (guarda uma reserva de cerca de 120 ms para o vídeo não engasgar num Wi-Fi ruim). Trocar reconecta a fonte (1 a 2 s com o último quadro).
- **Quando o iPhone cair:** **Manter o último quadro** (padrão), **Tela preta** ou **Imagem** (escolha o arquivo logo abaixo). Vale na hora, sem reconectar, inclusive com o iPhone já caído. Quando o iPhone volta, o vídeo volta sozinho.
- **Decodificação:** **Automática** (padrão: hardware acima de 1080p), **Hardware** ou **Software**. Use Hardware se o 4K atrasar num PC fraco. Trocar reconecta a fonte.
- **Encaixar na tela sozinho:** ligado, o item da cena fica no mesmo lugar e do mesmo tamanho quando o iPhone gira ou a resolução muda, e o vídeo novo cabe dentro dele. Nasce ligado em fonte nova. Fonte criada antes da 1.0.3 fica desligada até você marcar. Não mexe em item com recorte ou espelhado, e item que já tem "caixa delimitadora" (bounds) o OBS já mantém sozinho.

**Painel OBS Cam Mobile** (menu Docks), por iPhone:

- **Lente** (0,5 / 1 / 2 / 5), **EV** (menos e mais, de 0,3 em 0,3), **Trava AE**, **Trava AF**, **Trava BB** e **Qualidade** (resolução, quadros e bitrate, com **Aplicar**). Cada botão faz o mesmo que o toque na tela do iPhone, e o iPhone mostra na hora. O que a fonte do OBS fixou (predefinição ou bitrate nas propriedades) aparece travado no painel.
- iPhone com app antigo mostra "Atualize o app do iPhone para controlar pelo OBS".
- **Testar:** mede 3 s e diz se o cabo está ok. Critérios: pelo Wi-Fi ou pela entrada de rede = ressalva. Atraso (p95) acima de 150 ms = ressalva, acima de 300 ms = atraso alto. Ida e volta acima de 20 ms no cabo = ressalva. Menos de 90% dos quadros pedidos = ressalva. Quadros pulados pelo computador = ressalva.

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
