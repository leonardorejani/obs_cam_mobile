#!/bin/sh
# Gera a amostra H.264 (Annex B) usada pelo iphone-falso. Sem janela, so linha de comando.
# Uso: gerar.sh [saida.h264] [segundos] [LARGURAxALTURA]
set -eu
SAIDA="${1:-build/amostra.h264}"
SEG="${2:-10}"
TAM="${3:-1920x1080}"
FFMPEG="${FFMPEG:-ffmpeg}"
mkdir -p "$(dirname "$SAIDA")"

FONTE="testsrc2=size=$TAM:rate=30"
# Contador de quadro e relogio desenhados, se o ffmpeg tiver drawtext
if "$FFMPEG" -hide_banner -filters 2>/dev/null | grep -q ' drawtext '; then
  FONTE="$FONTE,drawtext=text='%{n} %{pts\\:hms}':x=40:y=40:fontsize=72:fontcolor=white:box=1:boxcolor=black@0.6"
fi

"$FFMPEG" -hide_banner -loglevel error -y -f lavfi -i "$FONTE" -t "$SEG" \
  -c:v libx264 -preset veryfast -bf 0 -g 60 -keyint_min 60 -sc_threshold 0 -tune zerolatency \
  -pix_fmt yuv420p -profile:v high -b:v 10M -maxrate 12M -bufsize 4M \
  -f h264 "$SAIDA"
echo "amostra: $SAIDA"
