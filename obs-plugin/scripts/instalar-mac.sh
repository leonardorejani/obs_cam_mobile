#!/bin/sh
# Instala o bundle no lugar onde o OBS procura plugins de usuario e assina de novo (ad-hoc) a copia.
set -e
ORIGEM="$1"
[ -d "$ORIGEM" ] || { echo "uso: instalar-mac.sh <fmcam-extension.plugin>"; exit 2; }
DEST="$HOME/Library/Application Support/obs-studio/plugins"
mkdir -p "$DEST"
rm -rf "$DEST/fmcam-extension.plugin"
ditto "$ORIGEM" "$DEST/fmcam-extension.plugin"
codesign --force --sign - "$DEST/fmcam-extension.plugin"
codesign --verify --verbose=2 "$DEST/fmcam-extension.plugin"
echo "instalado em $DEST/fmcam-extension.plugin"
