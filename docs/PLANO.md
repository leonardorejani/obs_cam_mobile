# OBS Cam Mobile: plano de implementação

Escrito em 18/11/2026. Parte da proposta (`PROPOSTA.md`) e das decisões do Leonardo (`../CLAUDE.md`). Nada daqui está codado ainda.

## Ideia central do plano

A parte difícil (protocolo, conexão pelo cabo, decodificação) fica numa biblioteca portátil em C++ que roda e se testa no Mac, sem OBS e sem iPhone. A extensão do OBS vira uma casca fina em cima dela. Assim quase tudo se prova antes de precisar do Windows.

Três decisões técnicas que sustentam isso:

1. **Cliente usbmuxd próprio, sem biblioteca de terceiro.** O serviço USB da Apple fala um protocolo simples de mensagens plist (listar aparelhos, conectar numa porta). No Mac ele atende em `/var/run/usbmuxd` (socket Unix, conferido nesta máquina). No Windows atende em `127.0.0.1:27015` (a confirmar no PC real). Escrever o cliente dá umas 300 linhas e tira a dependência do libusbmuxd.
2. **Vídeo em H.264 Annex B.** O app converte a saída do VideoToolbox (AVCC) para Annex B antes de mandar. O FFmpeg que já vem no OBS decodifica direto, sem tratamento especial.
3. **iPhone falso.** Um programa que fala o protocolo e manda um vídeo gravado. Com ele a biblioteca, o visualizador e a extensão se testam sem aparelho, inclusive em teste automático.

## Estrutura do repo

```
obs_cam_mobile/
  ios/                    app Swift (projeto Xcode)
    FMCam/                Captura, Codificador, Servidor, Protocolo, Tela
    FMCamTests/
  core/                   biblioteca C++17 portátil (CMake)
    protocolo.{h,cc}      handshake JSON + pacotes binários
    usbmux.{h,cc}         cliente usbmuxd (Mac e Windows)
    conexao.{h,cc}        cabo ou Wi-Fi, reconexão
    decodificador.{h,cc}  FFmpeg (avcodec), baixa latência
    tests/
  tools/
    iphone-falso/         servidor de teste que manda H.264 gravado
    visor/                recebe e mede (linha de comando, sem janela)
  obs-plugin/             casca do OBS (fonte, propriedades, painel)
  instalador/             Inno Setup (OBSCamMobile-Extension-Setup.exe)
  docs/                   PROPOSTA, PLANO, HANDOFF, PROTOCOLO, POP
  prototipo/              simulação HTML já entregue
```

## Fases

### M0: vídeo pelo cabo, medido (Mac + iPhone do Leonardo)

Objetivo: provar que o caminho inteiro funciona e medir o atraso de verdade. Se o número for ruim, descobrimos aqui, antes de escrever a extensão.

| # | Tarefa | Modelo |
|---|---|---|
| 0.1 | `docs/PROTOCOLO.md`: handshake, tipos de pacote, versão, erros | Opus |
| 0.2 | `core/protocolo` + testes (montar e desmontar pacote, pacote cortado no meio, versão desconhecida) | Sonnet |
| 0.3 | `core/usbmux`: listar aparelhos e conectar na porta 4848 pelo socket do Mac | Sonnet |
| 0.4 | `tools/iphone-falso`: fala o protocolo e manda um H.264 gravado em laço | Sonnet |
| 0.5 | `tools/visor`: conecta, decodifica, imprime quadros por segundo, quadros perdidos e atraso. Grava um PNG a cada N quadros como prova | Sonnet |
| 0.6 | App iOS mínimo: câmera traseira, VideoToolbox em tempo real sem B-frames, servidor TCP 4848 (Network.framework), tela de status, tela sempre acesa | Sonnet |
| 0.7 | Medição no aparelho real e registro em `docs/MEDICOES.md` | sessão |

Como medir o atraso:
- **Automático (cano):** cada quadro leva o relógio do iPhone. O visor acerta a diferença entre os relógios pelo ping do protocolo e calcula chegada menos captura. Mede codificação + cabo + decodificação.
- **Na vida real (vidro a vidro):** o Leonardo aponta o iPhone para um cronômetro na tela, e o visor grava o quadro com o próprio relógio carimbado. A diferença entre os dois números na imagem é o atraso total. Esse é o número que vale.

Pronto quando:
- `iphone-falso` + `visor` rodam em teste automático no Mac, sem aparelho.
- Com o iPhone do Leonardo no cabo: 10 minutos seguidos a 1080p 30 quadros, sem quadro perdido.
- Atraso vidro a vidro medido e anotado. Meta: até 100 ms. Acima de 150 ms, parar e investigar antes do M1.

Depende do Leonardo: iPhone ligado no Mac (instalação pelo Xcode com Apple ID grátis) e uns 15 minutos para a medição vidro a vidro.

### M1: extensão do OBS (Windows)

| # | Tarefa | Modelo |
|---|---|---|
| 1.1 | `core/usbmux` no Windows (TCP 127.0.0.1:27015) e `core` compilando com MSVC | Sonnet |
| 1.2 | `obs-plugin`: fonte de vídeo assíncrona "OBS Cam Mobile", modo sem fila, decodificação por placa de vídeo com queda para CPU | Sonnet |
| 1.3 | Propriedades: lista de todos os aparelhos ligados (cabo primeiro, pelo nome de cada iPhone), predefinição e lente por aparelho. Lista se atualiza ao plugar e desplugar | Sonnet |
| 1.3b | Vários iPhones ao mesmo tempo: uma fonte "OBS Cam Mobile Extension" por iPhone, configuração lembrada pelo id do aparelho, 4 iPhones no ar juntos | Sonnet |
| 1.4 | Reconexão: segura o último quadro com aviso e volta sozinha | Sonnet |
| 1.5 | Bonjour para o modo Wi-Fi de reserva | Sonnet |
| 1.6 | Tentar quebrar: cabo saindo 10 vezes, app fechado no meio, dois iPhones, OBS abrindo sem iPhone | Sonnet (esforço alto) |

Pronto quando: fonte no OBS do PC da transmissão mostrando o iPhone pelo cabo, 4 iPhones ligados aparecendo e no ar ao mesmo tempo, atraso vidro a vidro medido no OBS, e os testes de quebra sem travar o OBS.

Bloqueio: precisa de um Windows para compilar (ver "Em aberto").

### M2: controles e resistência

- Painel dentro do OBS: lente (0.5x, 1x, 2x), travar foco, travar exposição, travar branco.
- "NO AR": a extensão avisa o iPhone quando a fonte está no programa. Borda vermelha no app.
- Status do iPhone no OBS: bateria, temperatura, quadros por segundo.
- App reage a calor: aviso no OBS quando o iPhone esquenta, antes de o iOS derrubar a câmera.
- Aviso de assinatura no app ("vence em 2 dias").

Pronto quando: 2 horas seguidas no ar, carregando pelo cabo, sem queda e sem o atraso crescer.

### M3: instalação

- `.ipa` sem assinatura gerado por script (`xcodebuild archive` com assinatura desligada e empacotamento), para o SideStore assinar.
- `OBSCamMobile-Extension-Setup.exe` (Inno Setup): copia a extensão, confere o OBS, instala o serviço USB da Apple se faltar (baixando do site da Apple).
- `docs/POP.md`: o passo a passo final, com print de cada tela.

Pronto quando: instalação do zero num Windows limpo e num iPhone limpo, feita por outra pessoa seguindo só o POP.

### M4: transmissão real

Uma transmissão do FM Conecta com o OBS Cam Mobile e o DroidCam lado a lado, cada um num iPhone. Se o OBS Cam Mobile segurar a transmissão inteira, o DroidCam sai.

## Ordem e paralelismo

M0 é sequencial até 0.2. Depois, 0.3, 0.4 e 0.6 andam em paralelo (um agente cada). 0.5 depende de 0.3 e 0.4. M1 só começa com o M0 medido e com o Windows resolvido. M2 e M3 podem se sobrepor.

## Riscos, do maior para o menor

| Risco | Onde aparece | O que fazemos |
|---|---|---|
| Atraso real maior que o estimado (70 ms é conta de papel) | M0 | Medir antes de tudo. Ajustes possíveis: 60 quadros, bitrate, decodificação por placa |
| Sem Windows para compilar a extensão | M1 | Decidir antes do M1 (ver "Em aberto") |
| Serviço USB da Apple no Windows diferente do esperado | M1 | Conferir no PC real logo no começo do M1. Reserva: modo Wi-Fi |
| SideStore não renovar sozinho | em paralelo | Teste de 2 a 3 dias do passo a passo já entregue |
| iPhone esquentando em 2 horas de 1080p | M2 | Medir no M2. Reserva: 720p ou bitrate menor |
| Atualização do OBS quebrar a extensão | depois | Travar a versão do OBS no PC da transmissão |

## Em aberto (decisão do Leonardo)

1. **Onde compilar a extensão do Windows.** Opções: (a) instalar o Visual Studio Build Tools no PC da transmissão ou em outro PC da empresa, recomendado. (b) Runner Windows no GitHub Actions, hoje travado por cobrança. A VPS pessoal não entra: é só para projeto pessoal.
2. **Qual Apple ID grátis** assina o app nos iPhones (da empresa, não o pessoal).
3. **Quais modelos de iPhone** vão para a transmissão. Define as lentes disponíveis e o teto de qualidade.
4. **Áudio do iPhone:** fora do plano (o áudio vem da mesa). Confirmar.

## Regras do trabalho

- Nenhum trecho do plugin do DroidCam. Ele é GPLv2 e serviu só para entender que o caminho é o usbmuxd.
- Teste automático a cada peça. Erro achado vira teste que falha antes e passa depois.
- Nada abre janela na tela do Leonardo. Visor e testes rodam em linha de comando. O que só se prova com janela (OBS, medição vidro a vidro) vai no "como testar" para ele fazer.
- Commit só com autorização. Repo sem fluxo declarado: branch antes, nunca direto na main.
- Ferramentas a instalar no Mac quando o M0 começar: `cmake` e `ninja` (Homebrew). Não estão instalados hoje.
