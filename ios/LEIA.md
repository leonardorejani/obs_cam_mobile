# OBS Cam Mobile (app do iPhone)

App Swift, só frameworks da Apple (AVFoundation, VideoToolbox, Network). Fala o protocolo de `docs/PROTOCOLO.md` como servidor TCP na porta 4848.

## Gerar, compilar e testar (no Mac)

```
cd ios
xcodegen generate
xcodebuild -project FMCam.xcodeproj -scheme FMCam -destination 'platform=iOS Simulator,name=iPhone 17 Pro' CODE_SIGNING_ALLOWED=NO test
xcodebuild -project FMCam.xcodeproj -scheme FMCam -destination 'generic/platform=iOS' CODE_SIGNING_ALLOWED=NO build
```

O simulador não tem câmera: o teste prova o servidor, o protocolo e a resposta de erro `camera`. Vídeo de verdade só no aparelho.

## Instalar no iPhone (Leonardo, uma vez)

1. No iPhone: Ajustes > Privacidade e Segurança > Modo de Desenvolvedor > ligar (reinicia o aparelho).
2. Ligar o iPhone no Mac pelo cabo e tocar em "Confiar".
3. Abrir `ios/FMCam.xcodeproj` no Xcode. Em Xcode > Settings > Accounts, entrar com o Apple ID grátis.
4. Alvo FMCam > Signing & Capabilities: escolher o Team pessoal (o do Apple ID). Se der conflito de identificador, trocar o Bundle Identifier (ex.: `com.fmrocket.fmcam.leo`).
5. Escolher o iPhone como destino e rodar (Cmd+R).
6. No iPhone: Ajustes > Geral > VPN e Gerenciamento de Dispositivos > tocar no seu Apple ID > Confiar.
7. Abrir o OBS Cam Mobile e permitir a câmera.

Com Apple ID grátis o app expira em 7 dias. A renovação é só pelo SideStore (Meus Apps > Renovar).

## Nome da câmera

Como o iOS 16+ devolve o nome do aparelho genérico, o app tem o campo "Nome desta câmera" (ex.: "Palco 1"), salvo no aparelho. O padrão é "iPhone" + 4 primeiros caracteres do identificador do aparelho (ex.: "iPhone 6F1C"), então dois iPhones nunca ficam iguais. Esse nome aparece grande na tela e vai no campo `nome` do handshake (junto de `aparelho` = modelo e `id`). Trocar o nome vale a partir da próxima conexão.

## Tela e controles

O app abre em paisagem, com o preview da câmera em tela cheia (mostra exatamente o quadro que vai para o OBS, sem corte) e a câmera fica ligada sempre que o app está em primeiro plano, mesmo sem cliente. A codificação só roda com cliente conectado. Em segundo plano a câmera para.

- Toque na imagem: foca naquele ponto. Toque longo: trava foco e exposição (aparece "AE/AF travado"). Toque duplo: esconde ou mostra os controles (modo limpo). Pinça: zoom.
- Botões de lente (só as que o aparelho tem: 0,5x, 1x, 2x, tele). Trocar de lente com o OBS conectado não derruba a conexão.
- Botão de ajustes (canto superior direito): nome da câmera, grade de terços e marca de centro (só no preview, nunca no vídeo), foco Auto, Contínuo ou Manual, exposição Auto ou Manual (ISO e obturador), compensação de exposição, balanço de branco travado com temperatura em K, estabilização (desligada por padrão porque soma atraso).
- Os ajustes ficam salvos no aparelho. Na conexão o pedido de lente do OBS vale. Depois, o último que mudar (iPhone ou OBS) vale ao vivo.
- Tally: quando o OBS manda "no ar", a tela ganha borda vermelha e o selo NO AR.
- Wi-Fi: a tela mostra IP e porta (toque copia). O app também se anuncia por Bonjour (`_fmcam._tcp`) com o nome da câmera.

Os logs (`com.fmrocket.fmcam`, todas as mensagens começam com "fmcam") aparecem no `idevicesyslog | grep fmcam`.
