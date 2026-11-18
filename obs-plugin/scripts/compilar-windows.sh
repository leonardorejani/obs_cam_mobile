#!/bin/bash
# Compila a OBS Cam Mobile Extension para Windows x64 a partir do Mac (clang-cl + SDK/CRT do xwin), sem Windows.
# Saida: dist/OBSCamMobile-Extension-Windows.zip (+ dist/windows/fmcam-extension/ pronta para copiar).
# Tudo baixado vai para obs-plugin/deps/win (ignorado no git). Repetir o script reaproveita o que ja existe.
# Uso: obs-plugin/scripts/compilar-windows.sh
set -euo pipefail

# ---- UNICO lugar da versao do OBS alvo ----
OBS_VERSION="${OBS_VERSION:-33.0.1}"
# tag do obs-deps usada por essa versao do OBS (CMakePresets.json do obs-studio, bloco "dependencies")
OBS_DEPS_TAG="${OBS_DEPS_TAG:-2026-10-23}"
# CRT: MultiThreaded = estatico (/MT, nao pede VC++ Redistributable), MultiThreadedDLL = /MD
FMCAM_CRT="${FMCAM_CRT:-MultiThreaded}"
COM_PAINEL="${COM_PAINEL:-ON}"

AQUI="$(cd "$(dirname "$0")" && pwd)"
PLUGIN="$(cd "$AQUI/.." && pwd)"
RAIZ="$(cd "$PLUGIN/.." && pwd)"
W="$PLUGIN/deps/win"
LLVM="${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}"
BUILD="$RAIZ/build-win"
DIST="$RAIZ/dist"
export FMCAM_XWIN="$W/xwin"

LLD="${LLD_BIN:-/opt/homebrew/opt/lld/bin}"
export PATH="$LLD:$LLVM:$PATH"
for f in "$LLVM/clang-cl" "$LLVM/llvm-dlltool" "$LLD/lld-link" "$LLVM/llvm-readobj"; do
  [ -x "$f" ] || { echo "falta $f: brew install llvm lld"; exit 1; }
done
command -v xwin >/dev/null || { echo "falta xwin: brew install xwin"; exit 1; }
command -v cmake >/dev/null && command -v ninja >/dev/null || { echo "falta cmake/ninja"; exit 1; }
mkdir -p "$W" "$DIST"
baixar() { # repo tag arquivo destino
  [ -f "$4/$3" ] || gh release download "$2" -R "$1" -p "$3" -D "$4"
}

echo "== 1. SDK do Windows + CRT do MSVC (xwin; licenca da Microsoft aceita para uso interno de build)"
if [ ! -d "$W/xwin/crt/include" ]; then
  xwin --accept-license --arch x86_64 --cache-dir "$W/xwin-cache" splat --output "$W/xwin"
fi

echo "== 2. OBS $OBS_VERSION para Windows (DLLs) e headers"
[ -f "$PLUGIN/deps/obs-studio/libobs/obs.h" ] || git clone --depth 1 --branch "$OBS_VERSION" https://github.com/obsproject/obs-studio.git "$PLUGIN/deps/obs-studio"
baixar obsproject/obs-studio "$OBS_VERSION" "OBS-Studio-$OBS_VERSION-Windows-x64.zip" "$W"
[ -f "$W/obs-release/bin/64bit/obs.dll" ] || { mkdir -p "$W/obs-release"; unzip -q -o "$W/OBS-Studio-$OBS_VERSION-Windows-x64.zip" 'bin/64bit/*' -d "$W/obs-release"; }

echo "== 3. libs de importacao de obs.dll e obs-frontend-api.dll (geradas das exportacoes das DLLs)"
mkdir -p "$W/obs-lib"
for d in obs obs-frontend-api; do
  { echo "LIBRARY $d.dll"; echo EXPORTS
    "$LLVM/llvm-readobj" --coff-exports "$W/obs-release/bin/64bit/$d.dll" | awk '/^ *Name: /{print $2}'
  } > "$W/obs-lib/$d.def"
  "$LLVM/llvm-dlltool" -m i386:x86-64 -d "$W/obs-lib/$d.def" -l "$W/obs-lib/$d.lib"
done

echo "== 4. FFmpeg (headers + .lib) e Qt6 do obs-deps $OBS_DEPS_TAG"
baixar obsproject/obs-deps "$OBS_DEPS_TAG" "windows-deps-$OBS_DEPS_TAG-x64.zip" "$W"
[ -f "$W/obs-deps/lib/avcodec.lib" ] || { mkdir -p "$W/obs-deps"; unzip -q -o "$W/windows-deps-$OBS_DEPS_TAG-x64.zip" -d "$W/obs-deps"; }
if [ "$COM_PAINEL" = ON ]; then
  baixar obsproject/obs-deps "$OBS_DEPS_TAG" "windows-deps-qt6-$OBS_DEPS_TAG-x64.zip" "$W"
  [ -f "$W/qt6/lib/Qt6Widgets.lib" ] || { mkdir -p "$W/qt6"; unzip -q -o "$W/windows-deps-qt6-$OBS_DEPS_TAG-x64.zip" -d "$W/qt6"; }
fi
# o major do avcodec do OBS tem que bater com o das .lib/headers usados
AVC_OBS="$(cd "$W/obs-release/bin/64bit" && ls avcodec-*.dll)"; AVC_DEPS="$(cd "$W/obs-deps/bin" && ls avcodec-*.dll)"
[ "$AVC_OBS" = "$AVC_DEPS" ] || { echo "avcodec do OBS ($AVC_OBS) difere do obs-deps ($AVC_DEPS): ajuste OBS_DEPS_TAG"; exit 1; }
echo "avcodec: $AVC_OBS"

echo "== 5. configurar e compilar"
rm -rf "$BUILD"
cmake -S "$PLUGIN/windows" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PLUGIN/windows/clang-cl-win64.cmake" -DFMCAM_LLVM_BIN="$LLVM" -DFMCAM_LLD_BIN="$LLD" -DFMCAM_XWIN="$W/xwin" \
  -DFMCAM_OBS_SRC="$PLUGIN/deps/obs-studio" -DFMCAM_SIMDE_INC="$PLUGIN/deps/simde" \
  -DFMCAM_OBS_LIB_DIR="$W/obs-lib" -DFMCAM_DEPS_DIR="$W/obs-deps" -DFMCAM_QT_DIR="$W/qt6" \
  -DFMCAM_COM_PAINEL="$COM_PAINEL" -DFMCAM_CRT="$FMCAM_CRT"
cmake --build "$BUILD"

echo "== 6. montar pasta e zip"
rm -rf "$DIST/windows"; mkdir -p "$DIST/windows"
cmake --install "$BUILD" --prefix "$DIST/windows"
# Heredoc entre aspas: barra invertida no fim da linha nao pode juntar linhas (caminho do Windows).
cat > "$DIST/windows/LEIA-WINDOWS.txt" <<'TXT'
OBS Cam Mobile Extension para OBS (Windows x64)
Compilada para o OBS Studio @OBS_VERSION@. Outra versão pode não carregar.

ANTES DE INSTALAR
1. OBS Studio @OBS_VERSION@, 64 bits.
2. Serviço USB da Apple (só para usar pelo cabo). Uma destas opções:
   a) Só o serviço, sem o iTunes: baixe o instalador do iTunes 64 bits no site da
      Apple (apple.com/br/itunes) sem rodar. Abra com o 7-Zip, tire de dentro o
      AppleMobileDeviceSupport64.msi e instale só ele.
   b) App "Dispositivos Apple" da Microsoft Store.
   Para conferir: abra "Serviços" no Windows e veja "Apple Mobile Device Service"
   em execução.
   Pelo Wi-Fi não precisa de nada disso: o iPhone aparece sozinho na rede.
3. iPhone com o app OBS Cam Mobile aberto e na frente, ligado no cabo.
   Na primeira vez, toque em "Confiar" no iPhone.

INSTALAR
1. Feche o OBS.
2. Copie a pasta "fmcam-extension" (a que está dentro deste zip) para a pasta
   C:\ProgramData\obs-studio\plugins
3. Deve ficar assim:
   C:\ProgramData\obs-studio\plugins\fmcam-extension\bin\64bit\fmcam-extension.dll
   C:\ProgramData\obs-studio\plugins\fmcam-extension\data\locale\pt-BR.ini
4. Abra o OBS.

USAR
- Em Fontes, clique em + e escolha "OBS Cam Mobile Extension". Escolha o iPhone na lista.
- Painel com todos os iPhones: menu Docks > OBS Cam Mobile.
- Deixe a lente em "Manter a do iPhone" para trocar a lente pelo próprio iPhone.

SE NÃO APARECER
No OBS: Ajuda > Arquivos de log > Ver log atual. Procure as linhas com [fmcam].
- Nenhuma linha [fmcam]: a pasta está no lugar errado ou o OBS não é o @OBS_VERSION@.
- [fmcam] sem aparelho: confira o serviço da Apple (passo 2), o "Confiar" no iPhone
  e se o OBS Cam Mobile está aberto na frente.
TXT
sed -i '' "s/@OBS_VERSION@/$OBS_VERSION/g" "$DIST/windows/LEIA-WINDOWS.txt"
rm -f "$DIST/OBSCamMobile-Extension-Windows.zip"
(cd "$DIST/windows" && zip -q -r "$DIST/OBSCamMobile-Extension-Windows.zip" fmcam-extension LEIA-WINDOWS.txt)

echo "== 7. verificacao estatica da DLL"
sh "$AQUI/verificar-dll-windows.sh" "$DIST/windows/fmcam-extension/bin/64bit/fmcam-extension.dll"
echo "pronto: $DIST/OBSCamMobile-Extension-Windows.zip"
