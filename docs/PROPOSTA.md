# OBS Cam Mobile: proposta

Câmera do iPhone dentro do OBS, pelo cabo, com o menor atraso possível, sem pagar app de terceiros. Uso interno da FM Rocket (transmissões do FM Conecta).

## Problema

Hoje a transmissão usa o DroidCam: app no iPhone + plugin no OBS. Funciona bem pelo cabo, mas o app do celular é pago e caro para o uso que temos. E qualquer atualização do DroidCam (app ou plugin) pode quebrar a transmissão sem aviso.

## O que vamos construir

Três peças, todas nossas:

```
 iPhone                         cabo USB                    PC Windows
+----------------------+                         +-------------------------------+
| OBS Cam Mobile (app iOS)  |                         | Apple Devices (serviço USB)   |
|  câmera -> H.264     |  ====== usbmuxd ======> |            |                  |
|  servidor TCP :4848  |                         | OBS Cam Mobile (extensão do OBS)   |
|  tela "no ar"        | <--- comandos/tally --- |  decodifica -> fonte no OBS   |
+----------------------+                         +-------------------------------+
        ^ instalado e renovado pelo SideStore (Apple ID grátis)
```

### 1. App do iPhone (Swift)

- Captura com AVFoundation, câmera traseira (grande-angular, ultra ou tele).
- Codifica no chip do iPhone (VideoToolbox), H.264, sem B-frames, modo tempo real. Padrão 1080p 30 quadros, 10 Mbps. Opção de 60 quadros e HEVC depois.
- Abre um servidor TCP na porta 4848 (porta nossa, não a 4747 do DroidCam) e anuncia na rede local por Bonjour (`_fmcam._tcp`) para o modo Wi-Fi.
- Tela simples: estado da conexão, nome do aparelho, bateria, temperatura, e borda vermelha "NO AR" quando a cena está no ar no OBS.
- Tela sempre acesa enquanto o app está aberto. Carrega pelo próprio cabo.
- Ajustes travados pelo OBS: foco, exposição e balanço de branco (trava para a imagem não "respirar" durante a live).

### 2. Extensão do OBS (C/C++, Windows)

- Uma fonte nova no OBS: "OBS Cam Mobile".
- Lista os iPhones ligados no cabo (pelo serviço USB da Apple) e os que estão no Wi-Fi (Bonjour). Cabo sempre aparece primeiro.
- Recebe o vídeo, decodifica com o FFmpeg que já vem no OBS (decodificação por placa de vídeo quando houver) e entrega o quadro sem fila (modo sem buffer do OBS), que é o que tira atraso.
- Painel da fonte: aparelho, predefinição (1080p30, 1080p60, 720p60), lente, travar foco/exposição/branco, espelhar.
- Reconecta sozinha se o cabo sair ou o app fechar: a fonte mostra a última imagem com aviso e volta quando o iPhone voltar.
- Manda o "no ar" para o iPhone quando a cena que contém a fonte vai para o programa.
- Instalador próprio (.exe), sem Microsoft Store: copia a extensão para a pasta de plugins do OBS, confere se o serviço USB da Apple ("Apple Mobile Device Support") existe e, se faltar, baixa o instalador oficial do site da Apple e instala só esse serviço. A confirmar num Windows limpo.

### 3. Protocolo OBS Cam Mobile v1 (nosso)

Escrito do zero, sem copiar código do DroidCam. Tudo sobre uma conexão TCP.

1. OBS conecta e manda uma linha JSON: `{"v":1,"preset":"1080p30","lente":"wide","bitrate":10000000}`.
2. iPhone responde com uma linha JSON: `{"ok":true,"aparelho":"iPhone 13","w":1920,"h":1080,"fps":30}`.
3. Depois disso, só pacotes binários: `[tipo 1 byte][tempo 8 bytes][tamanho 4 bytes][dados]`.
   - tipo 1: configuração do H.264 (SPS/PPS)
   - tipo 2: quadro de vídeo
   - tipo 3: status do iPhone (bateria, temperatura, JSON)
   - tipo 4: comando do OBS (lente, travas, "no ar", JSON)
   - tipo 9: ping
4. Versão no handshake: extensão e app recusam conversa com versão que não conhecem e mostram "atualize o app" ou "atualize a extensão", em vez de travar.

## Atraso esperado

Estimativa, a medir no protótipo: captura ~33 ms + codificação ~8 ms + cabo ~1 ms + decodificação ~8 ms + desenho no OBS ~16 ms = **~70 ms**, na casa do DroidCam pelo cabo. No Wi-Fi soma de 20 a 100 ms conforme a rede.

## Instalação

**PC Windows (uma vez):**
1. Rodar o `OBSCamMobile-Extension-Setup.exe` com o OBS fechado. Ele instala a extensão e, se faltar, o serviço USB da Apple (decisão de 18/11/2026: um instalador só, nada de Microsoft Store).

**Cada iPhone (uma vez):**
1. Ligar no PC, tocar em "Confiar neste computador".
2. Rodar o iloader no PC: instala o SideStore e o arquivo de pareamento no iPhone com um Apple ID grátis da empresa.
3. Ligar o "Modo de desenvolvedor" nos Ajustes do iPhone (pede reiniciar).
4. No SideStore, instalar o OBS Cam Mobile (arquivo .ipa nosso).
5. Abrir o app e permitir a câmera.

**Renovação:** a assinatura grátis vence em 7 dias. O SideStore renova com o LocalDevVPN ligado. Rotina sugerida: abrir o SideStore e tocar em "Renovar" um dia antes de cada transmissão.

## Riscos de terceiros que sobram (e o que fazemos)

| Risco | Pode quebrar | Defesa |
|---|---|---|
| Atualização do OBS muda a interface de plugins | a extensão | travar a versão do OBS no PC da transmissão, testar a versão nova antes de atualizar |
| Atualização do iOS quebra o SideStore | instalar e renovar (o app já instalado continua até vencer) | segurar a atualização do iOS nos iPhones de transmissão |
| Apple Devices/iTunes muda o serviço USB | conexão pelo cabo | modo Wi-Fi como reserva, mesma extensão |
| Apple ID grátis: limite de 3 apps por aparelho | instalar outros apps nossos | LiveContainer, ou conta paga se crescer |

Não depender do DroidCam é o ganho principal: o protocolo, o app e a extensão passam a ser nossos e só mudam quando a gente quiser.

## Licenças

- Código nosso do zero, sem trecho do DroidCam (o plugin dele é GPLv2, lemos só para entender o caminho USB).
- A extensão liga com o libobs (GPLv2). Para uso interno não há distribuição, então não há obrigação. Se um dia for distribuída para fora, a extensão precisa sair como GPL.
- libusbmuxd/libimobiledevice são LGPL: uso por biblioteca dinâmica, ok.

## Fases

| Fase | Entrega | Como provar |
|---|---|---|
| M0 | App manda H.264 pelo cabo, PC recebe e mostra num visualizador de teste | vídeo aparecendo no Windows, atraso medido com cronômetro na tela |
| M1 | Extensão do OBS com a fonte, lista de aparelhos e reconexão | fonte no OBS, tirar e pôr o cabo 10 vezes |
| M2 | Painel de ajustes, travas, "no ar", status de bateria/temperatura | teste de 2 horas seguidas no ar |
| M3 | Instalador da extensão + kit SideStore + guia de instalação | instalar do zero num PC e num iPhone limpos seguindo só o guia |
| M4 | Transmissão real do FM Conecta em paralelo com o DroidCam | comparar as duas lado a lado |

## O que precisamos para construir

- Mac com Xcode (já existe) para o app do iPhone.
- PC Windows com Visual Studio 2022 para compilar a extensão, ou um runner Windows no CI. Ponto em aberto.
- Um iPhone de teste e o Apple ID grátis que vai assinar o app.
