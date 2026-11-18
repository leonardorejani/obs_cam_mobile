# Protocolo OBS Cam Mobile v1

Escrito em 18/11/2026. Contrato entre o app do iPhone (servidor) e quem recebe o vídeo (extensão do OBS, visor, testes). Feito do zero, sem nada do protocolo do DroidCam.

## Transporte

- Uma conexão TCP. O iPhone é o servidor, na porta **4848**.
- Pelo cabo, o PC chega na porta 4848 do iPhone através do serviço USB da Apple (usbmuxd, ver "Cabo" no fim). No Wi-Fi, conecta direto no IP do iPhone.
- No Wi-Fi, o iPhone se anuncia por Bonjour como `_fmcam._tcp` (nome do serviço = nome da câmera, registro TXT com `id` e `v=1`). O app também mostra na tela o IP do Wi-Fi e a porta, para digitar à mão quando o Bonjour não passar pela rede.
- Vários iPhones no mesmo PC: cada um é uma conexão independente (pelo cabo, cada aparelho tem o seu `DeviceID` no usbmuxd). A extensão abre uma conexão por iPhone.
- Cada iPhone atende um cliente por vez. O segundo recebe erro `ocupado` e é desconectado.
- Todo número binário é **big-endian** (ordem de rede).

## 1. Handshake (texto)

O cliente conecta e manda **uma linha JSON** em UTF-8 terminada em `\n`, com no máximo 4096 bytes:

```json
{"v":1,"cliente":"OBS Cam Mobile Extension 1.0.0","preset":"1080p30","lente":"wide","bitrate":10000000}
```

| Campo | Obrigatório | Valores |
|---|---|---|
| `v` | sim | versão do protocolo que o cliente fala. Hoje `1` |
| `cliente` | não | nome e versão de quem conecta, para log |
| `preset` | não | `1080p30`, `1080p60`, `1080p120`, `2160p30`, `720p30`, `720p60`. **Ausente = usar a qualidade salva no iPhone** (o operador pode ter escolhido na tela do app). **Presente = vale o do cliente nesta conexão.** Se o aparelho não tiver o formato, o iPhone cai para o mais próximo abaixo e diz o que ficou em `w`, `h` e `fps` da resposta |
| `lente` | não | `wide`, `ultra`, `tele`. **Ausente = manter a lente que está escolhida no iPhone** (o operador pode ter trocado na tela do app). O cliente só manda quando alguém escolheu a lente de propósito |
| `bitrate` | não | bits por segundo. Faixa aceita: 1000000 a 50000000. **Ausente = usar o bitrate salvo no iPhone. Presente = vale o do cliente nesta conexão.** Se o cliente mandar só o `preset`, o bitrate é o padrão daquele preset |

O iPhone responde com **uma linha JSON** terminada em `\n`:

```json
{"ok":true,"v":1,"aparelho":"iPhone 13","nome":"iPhone Palco 1","id":"6F1C2D3E-0000-4000-8000-000000000001","w":1920,"h":1080,"fps":30,"codec":"h264"}
```

`nome` é o nome do aparelho (Ajustes > Geral > Sobre) e `id` é um identificador fixo do app naquele aparelho. Com vários iPhones no mesmo PC, é com eles que a extensão mostra qual é qual e lembra a configuração de cada um.

Se não aceitar, responde e fecha a conexão:

```json
{"ok":false,"erro":"versao","v_suportadas":[1]}
```

| `erro` | Quando | O que o cliente mostra |
|---|---|---|
| `versao` | `v` desconhecido | "Atualize o app do iPhone" se `v` do cliente for maior que todas as suportadas, senão "Atualize a extensão" |
| `ocupado` | já tem cliente conectado | "iPhone em uso por outro computador" |
| `handshake` | linha inválida, JSON quebrado, maior que 4096 bytes ou sem `\n` em 3 segundos | "Falha de conexão" |
| `camera` | câmera indisponível (permissão negada, outro app usando) | "Câmera do iPhone indisponível" |

Regras do texto (valem para as duas linhas do handshake e para o JSON dos tipos 3 e 4):

- UTF-8 válido, sem BOM. UTF-8 inválido ou BOM = `handshake`.
- Limite de 4096 bytes **sem contar** o `\n`.
- Campo desconhecido é ignorado pelos dois lados, **qualquer que seja o tipo do valor** (número, texto, lista, objeto aninhado). É isso que deixa versões novas acrescentarem campos.
- Chave repetida no mesmo objeto = `handshake` (não há "vence a primeira" nem "vence a última").
- `v` precisa ser número inteiro. Inteiro fora das versões suportadas (inclusive 0, negativo ou enorme) = `versao`. `v` ausente ou que não é inteiro = `handshake`.
- Valor inválido num campo opcional cai no padrão (o iPhone não recusa por isso).
- Na resposta `ocupado`, o iPhone lê a linha do cliente (até 3 s) antes de responder e fechar. Fechar sem ler gera RST em alguns sistemas (Windows) e o cliente perde a resposta.

## 2. Pacotes (binário)

Depois do handshake aceito, os dois lados só trocam pacotes:

```
+--------+----------------+----------------+-------------------+
| tipo   | tempo          | tamanho        | dados             |
| 1 byte | 8 bytes (u64)  | 4 bytes (u32)  | `tamanho` bytes   |
+--------+----------------+----------------+-------------------+
```

- Cabeçalho fixo de 13 bytes.
- `tempo` em **microssegundos**, no relógio de quem envia (definição por tipo abaixo).
- `tamanho` máximo: 8 MiB (8388608). Acima disso é erro de protocolo: quem recebe fecha a conexão.
- Tipo desconhecido: quem recebe pula os `tamanho` bytes e segue. É isso que deixa versões futuras acrescentarem tipos sem quebrar as antigas.

### Do iPhone para o cliente

| Tipo | Nome | `tempo` | `dados` |
|---|---|---|---|
| 1 | config | captura do quadro que vem a seguir | SPS e PPS do H.264 em Annex B (cada um com início `00 00 00 01`) |
| 2 | quadro | instante da captura do quadro | 1 byte de flags + o quadro H.264 em Annex B |
| 3 | status | instante do envio | JSON UTF-8 (abaixo) |
| 10 | pong | instante da resposta | 8 bytes: o `tempo` do ping que está sendo respondido |

Flags do quadro (tipo 2): bit 0 = quadro-chave (IDR). Os outros bits são zero na v1 e devem ser ignorados.

Regras do vídeo:
- H.264, sem B-frames (a ordem de envio é a ordem de exibição).
- O tipo 1 vem sempre antes do primeiro quadro, e de novo sempre que a configuração mudar (troca de lente ou de preset). O quadro seguinte a um tipo 1 é sempre quadro-chave.
- O primeiro quadro depois do handshake é quadro-chave.
- Quadro-chave pelo menos a cada 2 segundos, para o cliente se recuperar de perda sem pedir.

Status (tipo 3), a cada 2 segundos:

```json
{"bateria":0.82,"carregando":true,"temp":"nominal","fps":29.9,"kbps":9800}
```

`temp` é o estado térmico do iOS: `nominal`, `fair`, `serious`, `critical`.

Campos de câmera no mesmo status (todos opcionais, quem lê ignora o que não conhece):

| Campo | Tipo | Valores |
|---|---|---|
| `lente` | texto | `ultra`, `wide`, `tele` (a que está em uso) |
| `zoom` | número | fator de zoom atual (ex.: 1, 2, 5) |
| `foco` | texto | `auto`, `manual`, `travado` |
| `exposicao` | texto | `auto`, `manual`, `travada` |
| `branco` | texto | `auto`, `travado` |
| `bias` | número | compensação de exposição em EV |
| `iso` | número | ISO atual (inteiro) |
| `obturador` | texto | velocidade atual como fração, ex.: `1/60` |
| `noar` | bool | tally recebido do OBS |
| `w`, `h`, `fps_alvo` | número | formato em uso (reflete a troca de qualidade feita no iPhone) |
| `bitrate` | número | bitrate do codificador em uso, em bits por segundo (inteiro) |
| `qualidade_obs` | bool | `true` quando o cliente fixou `preset` ou `bitrate` nesta conexão (a qualidade em uso veio dele), `false` quando vale a qualidade salva no iPhone. Ausente em iPhone antigo, e o cliente trata como `false` |
| `preset_obs`, `bitrate_obs` | bool | qual dos dois o cliente fixou (`qualidade_obs` é um ou o outro). Ausentes = `false`. O cliente marca "(OBS)" só no item fixado |
| `assinatura_vence` | texto | data ISO 8601 em que a assinatura do app instalado vence (só aparece se o aparelho tem o perfil) |

Troca de qualidade feita no iPhone durante a conexão: quando o operador muda a qualidade na tela do app, o iPhone reconfigura o codificador **sem derrubar a conexão**. A ordem é: tipo 1 novo com o SPS e PPS do formato novo, o quadro seguinte é quadro-chave, e os campos `w`, `h`, `fps_alvo` e `bitrate` novos chegam no status do ciclo normal (a cada 2 segundos, não há status imediato na troca). O cliente reage ao tipo 1 novo (recria o decodificador e ajusta o que mostra ao tamanho do quadro) e ao status quando ele chegar. A trava é por campo: o que o cliente fixou no handshake (`preset_obs` ou `bitrate_obs` = `true`) o iPhone **não troca nessa conexão** e o painel do app mostra o valor do OBS desabilitado. O campo que o cliente não mandou continua livre no iPhone e pode mudar no meio da conexão.

### Do cliente para o iPhone

| Tipo | Nome | `tempo` | `dados` |
|---|---|---|---|
| 4 | comando | instante do envio | JSON UTF-8 (abaixo) |
| 9 | ping | relógio do cliente no envio | vazio |

Comandos da v1:

```json
{"cmd":"keyframe"}
{"cmd":"noar","valor":true}
```

`noar` é o tally: a extensão manda `true` quando a cena com aquela fonte vai para o Programa do OBS e `false` quando sai. O iPhone mostra uma borda vermelha "NO AR" e não mexe no vídeo.

Pede um quadro-chave imediato (o cliente usa quando o decodificador perdeu a referência). Comando desconhecido, e campo desconhecido num comando conhecido, são ignorados. Lente, travas e "no ar" entram no M2 como comandos novos neste mesmo tipo.

## 3. Relógio e atraso

O `tempo` dos tipos 1, 2, 3 e 10 vem do **relógio de host do iPhone** (`CMClockGetHostTimeClock`, o mesmo que carimba a captura da câmera), convertido para microssegundos. O `tempo` do ping é o relógio monotônico do cliente, em microssegundos.

O cliente manda um ping por segundo. Para cada pong:

```
t0 = tempo do ping (relógio do cliente, ecoado nos dados do pong)
t1 = relógio do cliente quando o pong chegou
ts = tempo do pong (relógio do iPhone)
ida_e_volta = t1 - t0
diferenca   = ts - (t0 + t1) / 2      (iPhone menos cliente)
```

O cliente fica com a `diferenca` do ping de menor `ida_e_volta` nos últimos 10 (o mais confiável). Pong com eco no futuro (`t0 > t1`), ida e volta negativa ou acima de 2 s é descartado. Toda conta é feita sem estouro de inteiro (o `tempo` vem de outro aparelho e pode ser qualquer valor). Com ela, o atraso de cano de cada quadro é:

```
atraso = chegada_no_cliente - (tempo_do_quadro - diferenca)
```

Isso mede captura até chegada (codificação + transporte). A decodificação o cliente soma por conta própria. O atraso vidro a vidro (o que vale) se mede com cronômetro, ver `PLANO.md`.

## 4. Vida da conexão

- O iPhone fecha a conexão se não receber nada do cliente por **5 segundos** (o ping de 1 segundo mantém viva). Assim um cliente que sumiu sem fechar libera a câmera para o próximo.
- O cliente considera a conexão morta se não receber nada por **3 segundos** e reconecta (refaz o handshake).
- Erro de protocolo (tamanho acima do máximo, handshake inválido): quem detectou fecha a conexão. Nada de tentar ressincronizar no meio do fluxo.
- O iPhone para de codificar quando não há cliente (economiza bateria e calor).
- Cliente lento (fila de envio do iPhone cheia): o iPhone nunca acumula vídeo velho. Limite da fila = 1 segundo de vídeo no bitrate pedido (no mínimo 1 MiB). Acima dele, descarta os quadros que não são chave até o próximo quadro-chave. Se nem assim couber, descarta tudo o que está na fila antes do quadro-chave mais novo (que sempre é enviado inteiro). Pedido de quadro-chave ao codificador por esse motivo: no máximo um por segundo. Teto de memória da fila: 8 MiB, acima disso a conexão é fechada.
- Durante a partida da câmera, o que o cliente mandar fica guardado até 64 KiB. Passou disso, a conexão é fechada.

## 5. Cabo (usbmuxd)

Nota para quem implementa o cliente. Não faz parte do que trafega na porta 4848.

- Mac: socket Unix `/var/run/usbmuxd`. Windows: TCP `127.0.0.1:27015` (a confirmar no M1).
- Mensagem: cabeçalho de 16 bytes **little-endian** (`tamanho total`, `versao=1`, `tipo=8` plist, `tag`) + plist XML.
- `MessageType=ListDevices`: resposta traz `DeviceList`, cada item com `DeviceID` e `Properties.ConnectionType` (`USB` ou `Network`). Usar só `USB`.
- O mesmo iPhone pode aparecer duas vezes, com `ConnectionType` `USB` e `Network` (Wi-Fi sync ligado), e o macOS às vezes apaga a entrada `USB` com o cabo ligado. O cliente prefere a `USB`. Sem ela, usa o `DeviceID` da entrada `Network` do mesmo `SerialNumber` (o `Connect` funciona igual, o usbmuxd encaminha pela rede) e volta ao cabo quando a entrada `USB` reaparece. Vale também para o Apple Mobile Device Service do Windows.
- `MessageType=Listen`: o socket passa a receber eventos `Attached` e `Detached` (com `DeviceID`) quando um iPhone é plugado ou desplugado. É assim que a lista de aparelhos se atualiza sozinha.
- `MessageType=Connect` com `DeviceID` e `PortNumber` = porta 4848 **em ordem de rede dentro de um inteiro** (`htons(4848)` = 61458, ou 0xF012, numa máquina little-endian). Resposta `Result` 0 = conectado: a partir daí o mesmo socket vira o túnel TCP para a porta 4848 do iPhone, e o protocolo acima começa.
- Sempre preencher `ClientVersionString` e `ProgName` (`fmcam`).

## Versões

| Versão | Data | O que mudou |
|---|---|---|
| 1 | 18/11/2026 | Primeira versão |
| 1 | 18/11/2026 | Sem mudar o número da versão (só campos opcionais): `preset` e `bitrate` ausentes no handshake = qualidade salva no iPhone. Status ganha `bitrate` e `qualidade_obs`. Troca de qualidade pelo iPhone no meio da conexão |
