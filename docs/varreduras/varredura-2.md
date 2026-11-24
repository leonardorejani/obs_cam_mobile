# Varredura 2 (24/11/2026)

Versão entregue: app **1.0.4 build 9** e extensão **1.0.4**. Só correções, nenhuma função nova. Aparelho: só o iPhone 15 Pro Max do Leonardo, no cabo, iOS 27.2. Computador: Mac com OBS 33.0.1, medido com o libobs sem janela (`medir_obs`). Nada foi aberto na tela do Mac. Provas em `medicoes/varredura2/`.

A caça foi feita sem ler o relatório 1. Ele só foi lido no fim, para não repetir item e para conferir as correções dele.

## Atraso no 15 Pro Max (cabo, iPhone em pé, canvas 1920x1080)

"Captura até OBS" = do instante da captura no iPhone até o quadro decodificado entregue ao OBS (p50 e p95). "Até canvas" soma o libobs desenhando o quadro. "Maior vazio" = maior intervalo entre dois quadros novos no canvas (congelamento). Medidas de 20 s depois de 3 s de aquecimento, app e extensão 1.0.3 (as correções desta varredura não mexem no caminho do vídeo andando, só na troca de formato).

| Formato | Captura até OBS p50 / p95 | Até canvas p50 | fps | Maior vazio | Arquivo |
|---|---|---|---|---|---|
| 720p30 | 45 / 54 ms | 49 ms | 30 | 78 ms | `antes-720p30.txt` |
| 1080p30 | 54 / 58 ms | 73 ms | 30 | 50 ms | `antes-1080p30.txt` |
| 1080p60 | 60 / 71 ms | 70 ms | 60 | 65 ms | `antes-1080p60.txt` |
| 2160p30 | 85 / 89 ms | 113 ms | 30 | 68 ms | `antes-2160p30.txt` |
| Partida direto em 4K (formato do iPhone, 4K salvo) | 84 / 88 ms | 89 ms | 30 | 71 ms | `antes-partida-4k.txt` |

Partida: primeiro quadro no canvas **103 a 151 ms** depois de criar a fonte com o iPhone já em 4K, e cerca de 1,1 s quando a câmera precisa trocar de formato (`antes-2160p30.txt`). Nenhuma queda, nenhuma reconexão em todas as rodadas.

Trocas 1080p e 4K com a transmissão aberta (6 trocas pelo controle remoto, 1080p30, 2160p30, 1080p30, 2160p30, 1080p60, 2160p30, uma a cada 6 s, 40 s de medida):

| | Câmera parada na troca (log do iPhone) | Maior vazio no canvas | Reconexões | Atraso depois de assentar |
|---|---|---|---|---|
| Antes (app 1.0.3) | 1,0 a 1,56 s | **1826 ms** | 0 | 4K 65 a 85 ms, 1080p 47 ms |
| Depois (app 1.0.4) | 0,55 a 0,73 s | **833 ms** | 0 | igual |

`antes-trocas.txt`/`.err`, `depois-trocas.txt`/`.err`, log do iPhone em `syslog-antes.txt` (linhas "trocando o formato" e "configurada").

Conclusão: o 4K anda a cerca de 85 ms do iPhone até o OBS, estável, sem crescer. A diferença para o 1080p é a codificação no iPhone (igual à varredura 1). O que travava o 4K era a troca de formato (bug 1).

## Bugs achados

| # | Onde | Sintoma | Causa raiz | Correção | Teste ou prova | Status |
|---|---|---|---|---|---|---|
| 1 | App, `Captura.swift` `configurarSessao` | Trocar a qualidade com o OBS conectado (iPhone, painel ou propriedades) congelava a imagem 1 a 1,6 s. O watchdog do iPhone derruba a conexão com 1,5 s sem quadro, então a troca podia virar queda | Toda troca de formato tirava e punha de volta a entrada e a saída da câmera, o que reinicia as câmeras do dispositivo virtual | Mesma câmera: só troca o formato ativo, mantendo entrada, saída e rotação. Câmera diferente (lente física, foco manual) continua pelo caminho completo | `Varredura2Tests.testMesmaCameraSoTrocaOFormato` e `testOutraCameraRefazEntradaESaida`. Aparelho: 1,0 a 1,56 s antes, 0,55 a 0,73 s depois. Canvas: 1826 ms antes, 833 ms depois | Corrigido |
| 2 | App, `Servidor.swift` `qualidadeMudou` | Sem o OBS conectado, mudar a qualidade no iPhone não mexia na câmera: o "Formato" do topo seguia no antigo e a próxima conexão tinha que reconfigurar (partida mais lenta) | O aviso de qualidade só ia para a sessão de uma conexão. Sem conexão era descartado | Sem conexão, o servidor manda o formato salvo para a câmera do preview (só em primeiro plano e sem cliente) | `QualidadeServidorTests.testQualidadeSemConexaoVaiParaACamera` e `testQualidadeComConexaoNaoMexeNoPreview`. Aparelho antes: depois de escolher 1080p o log seguiu "formato atual 2160p30" na conexão seguinte (`syslog-antes.txt` 11:23:22). Depois, app 1.0.4 no aparelho sem OBS: o "Formato" do topo foi para "1080p · 30" e voltou para "2160p · 30" junto com a qualidade (`depois-qualidade-sem-obs-1080.png` e `-2160.png`) | Corrigido |
| 3 | App, `Tela.swift` | Câmera sem permissão, sem câmera, pausada pelo iOS (ligação, outro app, temperatura) ou com erro: tela preta ou parada, com "Esperando" ou "Conectado" | O estado da câmera era calculado (`estadoCamera`) e nenhuma tela usava. A interrupção do iOS nem mudava o estado | Aviso fixo no meio da tela com o motivo. Interrupção com o app na frente vira estado "interrompida" e volta para "ligada" quando o iOS solta | `Varredura2Tests.testEstadoDaCameraApareceNaTela` | Corrigido (a interrupção real não foi provocada no aparelho) |
| 4 | App, painel Balanço de branco | Com o branco em Travado sem mexer na régua, o painel dizia "5500 K", mas a trava fica no valor de agora | Texto com valor fixo quando não há temperatura escolhida | Mostra "Atual" até o operador escolher uma temperatura | `Varredura2Tests.testBrancoTravadoSemTemperaturaNaoInventaNumero` | Corrigido |
| 5 | Extensão, `fonte.cc` propriedades | Com o iPhone em pé, o estado mostrava "(pedido 1080p30)" como se o iPhone entregasse menos que o pedido | Comparava a altura do quadro (1920 em pé) com a da predefinição (1080) | Compara o lado menor do quadro (`entregaMenorQuePedido`) | `predefinicao_fixada_com_iphone_em_pe_nao_diz_que_entrega_menos` | Corrigido |
| 6 | Extensão, `fonte.cc` `atualizar` | "Quando o iPhone cair" trocado com o iPhone já caído não fazia nada até a próxima queda. O guia diz "vale na hora" | A tela preta ou a imagem só eram desenhadas no instante da queda | Com o iPhone caído, a opção nova é desenhada na hora (`aplicarQuedaAgora`) | `quando_o_iphone_cair_trocado_com_o_iphone_ja_caido_vale_na_hora` | Corrigido |
| 7 | Extensão, propriedades e log | Com Tela preta ou Imagem, o estado dizia "segurando o último quadro" | Texto fixo | Diz "mostrando tela preta" ou "mostrando a imagem escolhida" (pt-BR e en-US) | `estado_com_iphone_caido_diz_o_que_a_fonte_mostra` | Corrigido |
| 8 | Extensão, `sessao.cc` `aoEstadoCon` | Remover a fonte (ou trocar a predefinição) bem no fim do handshake deixava o iPhone reservado: painel "em uso" e outra fonte no mesmo iPhone em "ocupado: outra fonte do OBS" até reiniciar o OBS | O "Conectado" que já tinha passado da checagem de `parando` reservava o iPhone depois que o `parar()` limpou as reservas | Depois de reservar, confere `parando` de novo e solta | `fonte_removida_durante_o_handshake_nao_deixa_o_iphone_reservado`: antes falha com `fontes=1` e a fonte nova nunca conecta (`teste-reserva-antes.txt`), depois passa (`teste-reserva-depois.txt`) | Corrigido |
| 9 | Extensão, painel, combo de bitrate | O painel oferecia 25 Mbps, que o app não tem (Ajustes > Qualidade do iPhone ficava sem nada marcado). Bitrate do iPhone fora da lista deixava o combo no item anterior, e o "Aplicar" de quem só trocou a resolução mandava esse bitrate junto | Lista própria do painel, diferente da do app e das propriedades. Valor desconhecido não entrava no combo | Mesma lista do app (6, 10, 15, 20, 30) e o bitrate real entra no combo quando não está nela (`opcoesDeBitrate`) | `painel_lista_o_bitrate_real_do_iphone_e_os_mesmos_do_app` | Corrigido |
| 10 | Docs, `PROTOCOLO.md` | Dizia que, com só o `preset` no handshake, o bitrate é o padrão do preset | O código usa o bitrate salvo no iPhone (cada campo sozinho), e é isso que a extensão espera | Texto corrigido | Leitura de `QualidadeEnvio.resolver` | Corrigido |

### Achados não corrigidos

| Onde | O que | Por que não |
|---|---|---|
| Extensão, propriedades | O status ao vivo recarrega as propriedades a cada 2 s com valor novo. No OBS isso reconstrói os campos, então um menu aberto pode fechar sozinho (o relatório 1 já citou o campo de IP) | Só se prova com a janela do OBS aberta (proibido aqui). Tirar o recarregamento mataria o status ao vivo. Fica para o Leonardo conferir (item 6 do "Onde verificar") |
| Extensão Windows, `descoberta_wifi_win.cc` | iPhone que sai do Wi-Fi sem avisar fica na lista Wi-Fi até reiniciar o OBS (a remoção só acontece com TTL 0) | Leitura de código, sem Windows para provar. Mesmo ponto em aberto da varredura 1 |
| Extensão Windows, `rede.cc` | Em Windows anterior ao 10 versão 2004, `WSAPoll` não avisa falha de connect: com o serviço da Apple parado cada tentativa espera 3 s e a mensagem não diz que o serviço parou | Só em Windows antigo. Sem Windows para provar |
| Extensão, `sessao.cc` `ligarWifiPorId` | Janela mínima em que a conexão velha do Wi-Fi por id pode chamar `con` nulo ao trocar de IP | Não reproduzido. Janela de microssegundos |
| Extensão, auto-encaixe | Item da fonte dentro de um grupo do OBS não recebe o ajuste automático | Comportamento antigo (a regra nunca desceu em grupos). Decisão do Leonardo |
| Painel, botão "Adicionar à cena" | Desabilitado para iPhone que só aparece pelo Wi-Fi | Parece decisão de produto (o painel cria fonte de cabo) |
| App, corrida rara | Tocar na imagem e, no mesmo instante, ligar a Trava AF pode deixar o botão apagado com o foco travado | Não reproduzido. Janela de milissegundos |

## Conferência da varredura 1

| # V1 | Correção | Conferido | Como |
|---|---|---|---|
| 1 | Câmera abre no formato salvo | ok | Partida em 4K com 4K salvo: primeiro quadro em 103 a 151 ms, "câmera pronta em 29 ms" (`antes-partida-4k.txt`, `syslog-antes.txt`). Incompleta num caso: depois de trocar a qualidade sem conexão a câmera ficava no formato velho. Corrigido aqui (bug 2) |
| 2 | Segunda chamada de primeiro plano não mexe na câmera | ok | Log ao abrir: "primeiro plano repetido, a câmera já está ligada" |
| 3 | Borda do NO AR atrás dos controles em pé | não reconferido no aparelho | Fotos da V1 seguem valendo, o código não mudou |
| 4, 5, 6 | Qualidade alvo e troca em andamento | ok | Testes da V1 verdes. Trocas reais 1080p e 4K no aparelho sem queda (6 de 6) |
| 7 | Religada da câmera a cada 30 s | ok (regra) | Teste da V1 verde. Erro real de câmera não provocado |
| 8, 9, 10 | Nome salvo, marca de travado, formato do painel | ok | Testes da V1 verdes |
| 11 | Handshake aceita os 6 presets | ok | Teste verde, e o `medir_obs` com 2160p30 fixado chega a 3840x2160 |
| 12, 13 | Windows: Wi-Fi e threads soltas | só compilação | Compila cruzado sem erro. Sem Windows para rodar |
| 14 | Versão do cliente | ok | Agora 1.0.4, conferido em `opcoes_novas_normalizam...` |
| Achado "interrupção da câmera" | Não corrigido na V1 | agora corrigido | Bug 3 |
| Item "Quando o iPhone cair ... Vale sem reconectar" | Função nova da V1 | incompleta | Só valia na próxima queda. Bug 6 |

## Limite desta rodada

No fim da rodada o Mac deixou de ver o iPhone no USB (nem `idevice_id -l` nem o barramento USB o listavam, o `devicectl` seguiu falando pela rede). O app 1.0.4 foi instalado e conferido assim, mas a medida final de atraso com o app 1.0.4 já instalado não rodou. As trocas 1080p e 4K com a correção do bug 1 foram medidas no cabo com o mesmo código (build de teste antes de subir a versão).

## Testes

- `COMPLETO=1 sh .claude/verificar.sh` verde (`medicoes/varredura2/verificar-completo-final.txt`): ctest **14 de 14** (core 35 casos de protocolo e 14 de usbmux, lógica da fonte **42** casos, eram 37, headless do OBS) e iOS **254** testes, 0 falhas (eram 248).
- Novos: `ios/FMCamTests/Varredura2Tests.swift` (4), 2 em `QualidadeServidorTests`, e 5 em `obs-plugin/tests/teste_logica_fonte.cc` (seção "Varredura 2").
- `obs-plugin/tests/medir_obs.cc` ganhou: instante do primeiro quadro, maior vazio entre quadros e `FMCAM_COMANDOS_JUNTO=1` (comandos durante a medida, para medir as trocas).

## Artefatos

- `dist/OBSCamMobile.ipa`: Release sem assinatura, `Payload/FMCam.app`, bundle `com.fmrocket.fmcam`, 1.0.4 (9).
- `dist/OBSCamMobile-Extension-Setup.exe` e `dist/OBSCamMobile-Extension-Windows.zip`: extensão 1.0.4 compilada no Mac.
- Extensão do Mac reinstalada em `~/Library/Application Support/obs-studio/plugins/fmcam-extension.plugin`.

## Onde verificar

Feche e abra o OBS uma vez para carregar a extensão 1.0.4. iPhone no cabo, app 1.0.4 aberto.

1. **Troca de qualidade sem travar (bug 1):** com a fonte no ar, no iPhone Ajustes > Qualidade, alterne 1080p e 4K algumas vezes. A imagem no OBS para no máximo meio segundo e não cai. Antes ficava 1 a 2 s parada.
2. **Qualidade sem OBS (bug 2):** feche o OBS. No iPhone ligue "Formato" em Info. Em Ajustes > Qualidade troque de 4K para 1080p: o "Formato" do topo muda na hora para "1080p · 30".
3. **Aviso da câmera (bug 3):** com o app aberto, receba uma ligação ou abra a câmera por um atalho que pause a do app: aparece o aviso "O iPhone pausou a câmera..." no meio da tela, e some quando a câmera volta.
4. **Branco travado (bug 4):** Câmera > BB > Travado. A régua mostra "Atual". Mexa na régua: passa a mostrar a temperatura (ex.: "4300 K").
5. **Em pé com predefinição fixa (bug 5):** nas propriedades da fonte fixe 1080p30 e gire o iPhone em pé. O estado mostra "Entrega 1080x1920 @ 30 fps" sem "(pedido 1080p30)".
6. **Quando o iPhone cair (bugs 6 e 7):** com Manter o último quadro, feche o app no iPhone. Com o iPhone caído, abra as propriedades e escolha Tela preta: a fonte fica preta na hora e o estado diz "mostrando tela preta". Aproveite e veja se algum menu aberto das propriedades fecha sozinho com o vídeo ligado (achado não corrigido).
7. **Remover fonte (bug 8):** remova a fonte e crie outra para o mesmo iPhone várias vezes seguidas, logo depois de abrir o app. A fonte nova nunca fica em "ocupado: outra fonte do OBS".
8. **Bitrate no painel (bug 9):** Docks > OBS Cam Mobile. O combo de bitrate tem 6, 10, 15, 20 e 30 Mbps (sem 25) e mostra o mesmo bitrate que o iPhone em Ajustes > Qualidade.
9. **Windows:** instalar o `OBSCamMobile-Extension-Setup.exe` 1.0.4 com o OBS fechado e repetir 1, 5, 6 e 8 no PC da transmissão.
