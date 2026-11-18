# OBS Cam Mobile Extension (plugin do OBS)

Fonte de vídeo do OBS que puxa a câmera do iPhone pelo cabo (ou pelo Wi-Fi). Cada iPhone é uma fonte separada, com a própria configuração. Por cima do `core/` do repo, sem nada de plugin de terceiro.

## Compilar no Mac

Precisa de: OBS 33.0.1 em `/Applications/OBS.app`, `cmake`, `ninja`, `ffmpeg` (Homebrew) e internet na primeira vez (baixa os headers do obs-studio 33.0.1 e do simde para `obs-plugin/deps/`).

```
cmake -S . -B build -G Ninja
cmake --build build
```

Os headers do FFmpeg vêm do Homebrew, mas o plugin roda com o FFmpeg que o OBS carrega (mesma versão maior, o plugin se recusa a abrir se for diferente). O painel (dock) precisa dos headers do Qt 6.11.1, a mesma versão do OBS. Para baixar: `python3 -m venv obs-plugin/deps/venv && obs-plugin/deps/venv/bin/pip install aqtinstall && obs-plugin/deps/venv/bin/aqt install-qt mac desktop 6.11.1 clang_64 --outputdir obs-plugin/deps/qt`. Sem isso o plugin compila sem o painel.

## Instalar

```
cmake --build build --target instalar-obs
```

Copia o bundle para `~/Library/Application Support/obs-studio/plugins/fmcam-extension.plugin` e assina de novo (ad-hoc). **Feche e abra o OBS de novo** depois de instalar. Quem estava aberto continua com a versão antiga.

## Testar

Sem janela (ctest): `ctest --test-dir build --output-on-failure`. Os testes `obs_*` usam iphone-falso e usbmuxd-falso, nunca o iPhone de verdade.

## Testar no OBS (passo a passo)

1. iPhone ligado no cabo, desbloqueado, com o app OBS Cam Mobile aberto.
2. Abrir o OBS. Em **Fontes**, clicar em **+** e escolher **OBS Cam Mobile Extension**.
3. Em **Aparelho**, escolher o iPhone. O nome da câmera e o modelo aparecem na lista. Clicar em **Atualizar lista** se ele não aparecer. Em **Predefinição** e **Bitrate** o padrão é "Definida/Definido no iPhone" (vale a qualidade escolhida na tela do app, e se o operador mudar no iPhone a fonte acompanha sem reconectar). Quem fixar no OBS (720p30, 720p60, 1080p30, 1080p60, 1080p120 e 2160p30, e 6000 a 30000 kbps) manda no iPhone nessa conexão. O estado mostra o que o iPhone realmente entregou, e se foi menos que o pedido mostra "(pedido ...)".
4. **Painel**: menu **Docks**, **OBS Cam Mobile**. Lista os iPhones com estado, fps, bateria, temperatura, lente, ISO, obturador, foco e exposição (quando o app mandar) e o botão **Adicionar à cena**, que já cria a fonte apontada para aquele iPhone.
5. **Vários iPhones**: cada um vira uma fonte. Dois iPhones ligados aparecem os dois na lista. Duas fontes no mesmo iPhone não funcionam, a segunda fica "ocupado".
6. **NO AR (tally)**: quando a cena com a fonte vai para o Programa, o iPhone mostra a borda vermelha. Em modo estúdio só o Programa conta, a Prévia não.
7. **Wi-Fi**: iPhone e PC na mesma rede, app aberto. O iPhone aparece na lista com "(Wi-Fi)" depois dos do cabo. Na primeira vez o macOS pergunta se o OBS pode usar a **Rede local**, e precisa permitir (Ajustes do Sistema, Privacidade e Segurança, Rede local, OBS). Se o iPhone já está no cabo, só o do cabo aparece. Reserva: marcar "Usar Wi-Fi em vez do cabo" e digitar o IP que o app mostra na tela.
8. Se um item antigo da fonte ficou pequeno no canto depois de trocar a predefinição, clicar no item e apertar **Cmd+F** (ajustar à tela). Itens novos e os que enchiam a tela já se ajustam sozinhos.
9. Tirar o cabo: a fonte segura o último quadro e volta sozinha quando o iPhone voltar.

## O que muda no Windows

- Alvo: MSVC (Visual Studio Build Tools). Não foi compilado nem testado no Windows ainda.
- Usa o `obs-deps` e o obs-studio da mesma versão do OBS instalado. Configurar com `-DFMCAM_OBS_SRC=<fonte do obs-studio> -DFMCAM_OBS_LIB_DIR=<pasta com obs.lib e obs-frontend-api.lib> -DFMCAM_FFMPEG_INCLUDE_DIR=<headers do FFmpeg do obs-deps> -DFMCAM_FFMPEG_LIB_DIR=<libs>`. Painel: `-DFMCAM_COM_PAINEL=ON -DFMCAM_QT_LIB_DIR=<Qt6 do obs-deps>`.
- Instalar em `C:\ProgramData\obs-studio\plugins\fmcam-extension\bin\64bit\fmcam-extension.dll` e `...\data\locale\*.ini` (alvo `install` do CMake monta essa estrutura).
- O serviço USB da Apple fala em `127.0.0.1:27015` (já é o padrão do core no Windows).
- Wi-Fi: usa `DnsServiceBrowse` e `DnsServiceResolve` do `windns.h` (Windows 10+), sem o Bonjour da Apple. O arquivo `src/logica/descoberta_wifi_win.cc` está escrito, mas ainda sem teste no Windows.

- Instalador de um clique (serviço USB da Apple + extensão): `obs-plugin/scripts/montar-instalador-windows.sh` gera `dist/OBSCamMobile-Extension-Setup.exe`. O MSI da Apple fica em `obs-plugin/windows/instalador/vendor/` (ignorado no git, nunca publicar). Instruções para quem instala: `dist/COMO-INSTALAR-NO-WINDOWS.txt`.

## Variáveis para diagnóstico e teste

`FMCAM_USBMUXD` (caminho do usbmuxd), `FMCAM_BONJOUR` (`off` desliga, `local` só anúncios da própria máquina), `FMCAM_CANVAS` (`1920x1080`, só para teste sem vídeo). Sem elas o plugin se comporta normalmente.

## Estrutura

`src/logica/` lógica sem libobs (estado, reconexão, escolha do aparelho, tally, Bonjour). `src/glue/` cola do libobs (fonte e módulo). `src/painel/` dock Qt. `tests/` testes.
