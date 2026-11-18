#!/bin/bash
# Gera dist/OBSCamMobile-Extension-Setup.exe (instalador NSIS de um clique) sem Windows.
# Precisa de: brew install makensis, python3 com Pillow, e:
#  - dist/windows/fmcam-extension/ (rode obs-plugin/scripts/compilar-windows.sh antes)
#  - obs-plugin/windows/instalador/vendor/AppleMobileDeviceSupport64.msi (software da Apple, ignorado no git, nunca publicar)
# Uso: obs-plugin/scripts/montar-instalador-windows.sh
set -euo pipefail
AQUI="$(cd "$(dirname "$0")" && pwd)"
RAIZ="$(cd "$AQUI/../.." && pwd)"
INST="$RAIZ/obs-plugin/windows/instalador"
EXT="$RAIZ/dist/windows/fmcam-extension"
MSI="$INST/vendor/AppleMobileDeviceSupport64.msi"
SAIDA="$RAIZ/dist/OBSCamMobile-Extension-Setup.exe"
VERSAO="${VERSAO:-1.0.0}"
ICO="$INST/fmcam.ico"

command -v makensis >/dev/null || { echo "falta makensis: brew install makensis"; exit 1; }
[ -f "$EXT/bin/64bit/fmcam-extension.dll" ] || { echo "falta $EXT (rode compilar-windows.sh)"; exit 1; }
[ -f "$MSI" ] || { echo "falta $MSI (veja o topo deste script)"; exit 1; }

python3 - "$RAIZ/marca/icone-pb-256.png" "$ICO" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGBA")
im.save(sys.argv[2], format="ICO", sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
PY

mkdir -p "$RAIZ/dist"
makensis -V2 -WX -DVERSAO="$VERSAO" -DEXT_DIR="$EXT" -DMSI="$MSI" -DICONE="$ICO" -DSAIDA="$SAIDA" "$INST/fmcam.nsi"
echo "== pronto: $SAIDA"
shasum -a 256 "$SAIDA"
ls -l "$SAIDA"
