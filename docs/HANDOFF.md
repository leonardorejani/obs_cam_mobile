# OBS Cam Mobile: handoff

Atualizado em 18/11/2026, 13:40. Para quem pegar o projeto do zero (outra sessão ou o próprio Leonardo daqui a um mês).

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

- Atraso vidro a vidro, 4K e 120 fps no aparelho, 2 h no ar (calor), vários iPhones reais, Windows.
- Uma queda rara dentro do FFmpeg só sob ThreadSanitizer (ASan limpo). Investigar se repetir.

## Esperando o Leonardo

1. Testar o instalador do Windows (`dist/OBSCamMobile-Extension-Setup.exe`) no PC da transmissão (OBS 33.0.1 + iTunes do site da Apple). O iPhone 17 Pro precisa do OBS Cam Mobile instalado (pelo SideStore, com o Apple ID que for assinar).
2. Qual Apple ID grátis da empresa assina o app nos iPhones da transmissão.
3. ~~Quais modelos de iPhone~~ Respondido 18/11: iPhone 17 Pro num PC Windows.
4. Autorização de commit.

## Armadilhas

- macOS lista o iPhone duas vezes no usbmuxd (USB e Network, Wi-Fi sync). Medido 18/11 16:27: segundos depois de ligar o cabo, com a entrada Network ativa, o macOS apaga a entrada USB. A extensão segue pela entrada Network e volta ao cabo quando a USB reaparece (~1 s de quadro parado na troca).
- OBS Cam Mobile no iPhone do Leonardo está instalado pelo SideStore (bundle com.fmrocket.fmcam.3SUC2FNCD5). Atualizar copiando dist/OBSCamMobile.ipa para Documents do SideStore (devicectl copy to, appDataContainer) e instalar pelo + em My Apps. Instalar pelo Xcode cria um 4o app e bate no limite de 3 da conta grátis.
- Troca de lente igual à câmera nativa: botões mudam o zoom do dispositivo virtual (rampa de 0,25 s) e o iOS escolhe a câmera física que foca (5x de perto usa a wide cortada, de longe a tele). Medido 18/11 18:23 no 15 Pro Max. Nunca forçar a câmera física: a tele não foca de perto.
- Foco por cena: só reaplica quando houve ponto tocado (reaplicar a cada evento fazia a tele caçar foco).

- Estabilização ligada no iPhone soma 1 a 2 quadros de atraso (medido 18/11: 85 ms com ela, ~45 ms sem).
- Teste de Bonjour do plugin enxerga iPhones reais na rede: os testes filtram pelos próprios ids.

- Não testar com aparelho de outra pessoa. Só o iPhone do Leonardo.
- Nada abre janela na tela dele. Testes iOS no simulador derrubando o processo abrem o aviso "encerrou inesperadamente" no Mac: teste nunca pode crashar.
- O iPhone atende um cliente por vez: com o OBS conectado, o visor recebe "ocupado".
- Câmera virtual do OBS no Mac precisa de aprovação em Ajustes > Geral > Itens de Início e Extensões > Extensões de Câmera (já aprovada nesta máquina) e saída em "Programa".
- O plugin do DroidCam é GPLv2: ler pode, copiar trecho não.
