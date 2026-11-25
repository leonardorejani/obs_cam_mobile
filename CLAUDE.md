# OBS Cam Mobile

Câmera do iPhone dentro do OBS pelo cabo, com o menor atraso possível, para as transmissões internas do FM Conecta. Substitui o DroidCam (app pago). Proposta completa em `docs/PROPOSTA.md`.

## Decisões do Leonardo (17/11/2026)

- PC do OBS é **Windows**.
- **Extensão do OBS nossa desde o início**, nada de plugin de terceiro: "não quero correr o risco de dar pau na transmissão por alguma atualização de terceiros".
- Protocolo nosso (não reaproveitar o do DroidCam), porta 4848.
- App do iPhone instalado sem conta paga: Apple ID grátis + SideStore (renovação a cada 7 dias). Ele quer reaproveitar o SideStore em outros projetos.
- Instalação no PC por um instalador `.exe` só, sem Microsoft Store (18/11/2026). O instalador cuida também do serviço USB da Apple.
- Nomes (18/11/2026): app do iPhone **OBS Cam Mobile**, extensão do OBS **OBS Cam Mobile Extension**. A pasta do repo é `obs_cam_mobile` (antes `fm_appcam`).
- **Vários iPhones no mesmo PC** (18/11/2026): a extensão lista todos os aparelhos ligados (ex.: 4), cada um com a sua configuração e a sua fonte no OBS, independentes.
- Transmissão (18/11/2026): PC Windows com **iPhone 17 Pro**. A extensão do Windows é tentada primeiro por compilação cruzada no Mac (clang-cl + SDK do Windows). Se não der, Visual Studio Build Tools no PC.
- Repositório **público com licença MIT** (25/11/2026). Nada de UDID, time da Apple, e-mail, nome de terceiro ou credencial em arquivo versionado. O instalador com o serviço USB da Apple continua só para uso interno, fora do git e sem release público. Não usar nome nem marca DroidCam.

## Estrutura (planejada)

- `ios/`: app Swift (AVFoundation + VideoToolbox + Network.framework).
- `core/`: biblioteca C++ portátil (protocolo, cliente usbmuxd próprio, decodificação). Testa no Mac sem OBS e sem iPhone.
- `tools/`: `iphone-falso` (servidor de teste) e `visor` (recebe e mede, sem janela).
- `obs-plugin/`: casca do OBS em cima do `core/` (CMake), alvo Windows.
- `instalador/`: Inno Setup do `OBSCamMobile-Extension-Setup.exe`.
- `docs/`: proposta, plano de implementação (`PLANO.md`), handoff (`HANDOFF.md`), protocolo, guia de instalação.
- `prototipo/`: simulação em HTML do fluxo de instalação e uso.

## Regras

- Código escrito do zero. O plugin do DroidCam (GPLv2) serve só de leitura para entender o caminho USB, nunca para copiar trecho.
- Teste de navegador sempre headless.
- Commit só com autorização explícita.
