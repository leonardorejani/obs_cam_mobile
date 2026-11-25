# OBS Cam Mobile: handoff

Atualizado em 24/11/2026 (varredura 2, versão 1.0.4). Relatórios: `docs/varreduras/varredura-1.md` e `docs/varreduras/varredura-2.md`. Para quem pegar o projeto do zero (outra sessão ou o próprio Leonardo daqui a um mês).

## O que é

Câmera do iPhone dentro do OBS pelo cabo (ou Wi-Fi), com atraso baixo, para as transmissões internas do FM Conecta. Substitui o DroidCam. Três peças nossas: app **OBS Cam Mobile** (iPhone), **OBS Cam Mobile Extension** (OBS) e o protocolo (`docs/PROTOCOLO.md`).

## Estado hoje

Branch `feat/m0`, **nada commitado** (commit só com autorização). Apresentação local: `apresentacao/index.html` (servida em `http://127.0.0.1:8792/apresentacao/` quando o servidor está de pé: `python3 -m http.server 8792 --bind 127.0.0.1` na raiz).

| Peça | Estado |
|---|---|
| Protocolo v1 | Pronto. Regras de texto do handshake, fila, relógio, status e tally fechadas no doc |
| `core/` + `tools/` (C++) | Pronto. Cliente usbmuxd próprio, conexão por UDID, decodificação, iPhone falso, visor, usbmuxd falso |
| App OBS Cam Mobile | Rodando no iPhone do Leonardo (15 Pro Max, assinado com o Personal Team dele, vence 25/11 10:29). Preview em paisagem, linhas-guia, lentes, foco/exposição/branco manuais, estabilização (modo de menor atraso), tally, IP e Bonjour, nome da câmera, validade da assinatura na tela |
| OBS Cam Mobile Extension | Instalada no OBS 33.0.1 do Mac. Fonte por iPhone, lista por cabo e Wi-Fi, reconexão, painel OBS Cam Mobile, tally, reajuste de resolução, lente "Manter a do iPhone" |
| Windows | `.dll` x64 gerada no Mac por compilação cruzada (`obs-plugin/scripts/compilar-windows.sh`, clang-cl + xwin + libs do OBS 33.0.1 Windows), zip em `dist/OBSCamMobile-Extension-Windows.zip`. **Nunca rodou num Windows** |
| Tela do app (18/11 13:30) | Layout "E" aprovado no protótipo `prototipo/tela/index.html`: topo em duas cápsulas sem engrenagem, menu Câmera/Guias/Info/Ajustes com trilho, lentes em pílula, botão limpar abaixo delas, lado do menu configurável. Instalado no iPhone do Leonardo |
| Qualidade de envio | Definida no iPhone (Ajustes > Qualidade: resolução, quadros, bitrate) ou fixada no OBS ("Definida no iPhone" é o padrão da extensão). Troca no meio da conexão sem reconectar. Ver `docs/PROTOCOLO.md` |
| Instalador Windows | `dist/OBSCamMobile-Extension-Setup.exe` (NSIS, `obs-plugin/scripts/montar-instalador-windows.sh`): instala o Apple Mobile Device Support 20.0.1.47 (MSI da Apple, assinatura verificada, uso interno, fora do git) e a extensão. Nunca rodou num Windows |
| SideStore | Não instalado ainda. `iloader` em /Applications, `dist/OBSCamMobile.ipa` pronto. Falta o Leonardo: LocalDevVPN e Apple ID no iloader. Plano: renovação automática do SideStore ligada + automação no Atalhos ("Dias para vencer" do OBS Cam Mobile <= 2 roda "Refresh All Apps") |

## Versão 1.0.4 (24/11/2026, varredura 2)

- App 1.0.4 build 9 e extensão 1.0.4. Só correções, sem função nova. Detalhe em `docs/varreduras/varredura-2.md`.
- Troca de qualidade com a conexão aberta: o vídeo parava 1 a 1,6 s (encostando no watchdog de 1,5 s do iPhone). Agora a mesma câmera só troca o formato ativo: cerca de 0,6 s.
- Qualidade escolhida no iPhone sem o OBS conectado vai na hora para a câmera (antes o "Formato" do topo continuava no velho).
- Câmera sem permissão, pausada pelo iOS ou com erro aparece como aviso no meio da tela. Branco travado sem temperatura escolhida mostra "Atual" em vez de "5500 K".
- Extensão: "(pedido ...)" falso com o iPhone em pé, "Quando o iPhone cair" agora vale na hora com o iPhone já caído, o estado diz tela preta ou imagem quando é isso que aparece, reserva presa ao remover a fonte durante o handshake, combo de bitrate do painel com o valor real do iPhone e sem o 25 Mbps que o app não tem.

## Versão 1.0.3 (24/11/2026, varredura 1)

- App 1.0.3 build 8 instalado no 15 Pro Max (bundle do SideStore). Extensão 1.0.3 no OBS do Mac e no instalador do Windows.
- Novidades da extensão: status ao vivo nas propriedades, Modo de recepção (Menor atraso / Mais estável), Quando o iPhone cair (último quadro / tela preta / imagem), Decodificação (automática / hardware / software), Encaixar na tela sozinho, painel com controle remoto (lente, EV, travas AE/AF/BB, qualidade) e botão Testar. Protocolo ganhou os comandos e campos do controle remoto (ver `PROTOCOLO.md`).
- 4K medido no OBS de verdade (libobs sem janela, `medir_obs`): 2160p30 a cerca de 81 ms da captura até o quadro entregue ao OBS e 90 a 105 ms até o canvas desenhado. 1080p30: 54 ms e cerca de 60 ms.
- `obs-plugin/tests/medir_obs.cc`: ferramenta de medição no aparelho (não roda no ctest).

## Números medidos (iPhone real, cabo, 1080p30)

- Atraso captura até chegada no Mac: p50 ~43 a 49 ms, p95 ~51 ms. Decodificação ~9 ms. Vidro a vidro: não medido.
- 0 quadro perdido dentro de cada conexão.
- Reconexão do OBS quando o app reinicia: ~4 s, segurando o último quadro.
- **Critério do M0 cumprido:** 10 min 18 s no OBS do Leonardo (18/11, 11:50:12 a 12:00:30), 1080p60, 0 queda, 61 amostras do iPhone entre 59 e 60 fps, 0 quadro descartado (log do iPhone em `medicoes/obs-sessao/syslog.txt`, local e fora do git).

## Bugs reais achados no aparelho e corrigidos

1. Câmera configurada na fila do servidor: um travamento da câmera deixava o app mudo até 1 min. Câmera em fila própria + limite de partida + watchdog de 1,5 s.
2. Permissão de câmera pedida na primeira conexão: agora ao abrir o app.
3. Câmera desligava em `.inactive` (teclado, sheet, Central de Controle): quedas a cada 15 a 60 s. Agora só desliga em `.background`.
4. Estabilização `.auto` escolhia modo cinematográfico (atraso enorme). Agora `.standard`/`.previewOptimized`, sem trocar de lente.
5. Extensão mandava `lente:"wide"` em toda reconexão e sobrescrevia a lente do iPhone. Agora ausente = manter.
6. 720p ficava no canto do canvas do OBS. Agora a fonte se reajusta.
7. Visor dizia "0 perdidos" com tempo sem vídeo: agora conta `segundos_sem_video` e falha.
8. 8 defeitos da verificação adversarial (fuzz, sanitizers), todos com teste antes/depois. Provas em `tests/adversarial/`.

## Verificação

- `sh .claude/verificar.sh` (rápido) ou `COMPLETO=1 sh .claude/verificar.sh` (C++ 14 testes + iOS no simulador). Última rodada completa: 14/14 e iOS verde.
- Medir no iPhone real (sem o OBS conectado, o iPhone atende um cliente por vez): `build-final/tools/visor --usb --udid <UDID> --segundos 600 --png-dir medicoes/x --png-cada 1800`.
- Log do app no iPhone: `idevicesyslog -u <UDID> -m fmcam` (toda linha do app começa com "fmcam").
- Instalar o app no iPhone pelo cabo: ver `ios/LEIA.md` (xcodebuild com `DEVELOPMENT_TEAM`, `devicectl device install app`).

## Ainda não provado

- Atraso vidro a vidro, 120 fps no aparelho, 2 h no ar (calor), vários iPhones reais, Windows. O 4K foi medido até o canvas do libobs (varredura 1).
- Uma queda rara dentro do FFmpeg só sob ThreadSanitizer (ASan limpo). Investigar se repetir.

## Esperando o Leonardo

1. Testar o instalador do Windows (`dist/OBSCamMobile-Extension-Setup.exe`) no PC da transmissão (OBS 33.0.1 + iTunes do site da Apple). O iPhone 17 Pro precisa do OBS Cam Mobile instalado (pelo SideStore, com o Apple ID que for assinar).
2. Qual Apple ID grátis da empresa assina o app nos iPhones da transmissão.
3. ~~Quais modelos de iPhone~~ Respondido 18/11: iPhone 17 Pro num PC Windows.
4. Autorização de commit.

## Armadilhas

- macOS lista o iPhone duas vezes no usbmuxd (USB e Network, Wi-Fi sync). Medido 18/11 16:27: segundos depois de ligar o cabo, com a entrada Network ativa, o macOS apaga a entrada USB. A extensão segue pela entrada Network e volta ao cabo quando a USB reaparece (~1 s de quadro parado na troca).
- OBS Cam Mobile no iPhone do Leonardo está instalado pelo SideStore (bundle com.fmrocket.fmcam.<time>). Atualizar copiando dist/OBSCamMobile.ipa para Documents do SideStore (devicectl copy to, appDataContainer) e instalar pelo + em My Apps. Instalar pelo Xcode cria um 4o app e bate no limite de 3 da conta grátis.
- Troca de lente igual à câmera nativa: botões mudam o zoom do dispositivo virtual (rampa de 0,25 s) e o iOS escolhe a câmera física que foca (5x de perto usa a wide cortada, de longe a tele). Medido 18/11 18:23 no 15 Pro Max. Nunca forçar a câmera física: a tele não foca de perto.
- Foco por cena: só reaplica quando houve ponto tocado (reaplicar a cada evento fazia a tele caçar foco).

- Estabilização ligada no iPhone soma 1 a 2 quadros de atraso (medido 18/11: 85 ms com ela, ~45 ms sem).
- Teste de Bonjour do plugin enxerga iPhones reais na rede: os testes filtram pelos próprios ids.
- O libobs adia o `obs_source_update` de fonte de vídeo para o próximo tick de vídeo: em teste sem vídeo a opção tem que nascer nas configurações da criação.
- Ferramenta que inicia o vídeo do libobs (OpenGL) e cai gera relatório de falha do macOS: soltar cena e fonte e chamar `obs_wait_for_destroy_queue()` antes do `obs_shutdown`.
- `idevicescreenshot` não funciona no iOS 27 sem a imagem de desenvolvedor: foto da tela pelo app com `FMCAM_DEBUG=1` e `fmcam.d.snap`, depois `devicectl device copy from` (Documents/snap-N.png).

- Não testar com aparelho de outra pessoa. Só o iPhone do Leonardo.
- Nada abre janela na tela dele. Testes iOS no simulador derrubando o processo abrem o aviso "encerrou inesperadamente" no Mac: teste nunca pode crashar.
- O iPhone atende um cliente por vez: com o OBS conectado, o visor recebe "ocupado".
- Câmera virtual do OBS no Mac precisa de aprovação em Ajustes > Geral > Itens de Início e Extensões > Extensões de Câmera (já aprovada nesta máquina) e saída em "Programa".
- O plugin do DroidCam é GPLv2: ler pode, copiar trecho não.

## 1.0.5 (24/11/2026)

- Ao girar, a janela vira na hora e `dimCamera` só muda depois do debounce e da troca do codificador. Layout de controle nunca usa `dimCamera` para saber o lado: controles seguem a janela, guias e borda do NO AR seguem `anguloFisico` (`MedidasTela.proporcao`). Teste: `testGirarNaoDesmontaATelaEnquantoOVideoNaoTroca`.
- Em pé, os botões do trilho têm largura igual (`MedidasTela.larguraItemEmPe`). A paisagem não mudou.
- Instalar pelo cabo com o perfil do SideStore deixa o SideStore com a versão antiga guardada. Depois, instalar o mesmo .ipa pelo + dele.
