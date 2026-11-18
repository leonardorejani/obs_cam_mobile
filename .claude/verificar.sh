#!/bin/sh
# Critério de pronto do OBS Cam Mobile.
# Padrão (rápido): JS do protótipo + compila o C++ + testes de unidade.
# COMPLETO=1: também os testes ponta a ponta (iphone-falso, usbmuxd-falso, 4 aparelhos)
# e os testes do app iOS no simulador (sem abrir janela).
set -e
cd "$(dirname "$0")/.."

T=$(mktemp -t fmcam).js
sed -n '/<script>/,/<\/script>/p' prototipo/index.html | sed '1d;$d' > "$T"
node --check "$T" && echo "prototipo: JS ok"
rm -f "$T"

B=build-verificar
cmake -S . -B "$B" -G Ninja >/dev/null
cmake --build "$B"
if [ "${COMPLETO:-0}" = "1" ]; then
  ctest --test-dir "$B" --output-on-failure
else
  ctest --test-dir "$B" --output-on-failure -E '^e2e'
fi

if [ "${COMPLETO:-0}" = "1" ]; then
  SIM=$(xcrun simctl list devices available | grep -m1 -o 'iPhone [0-9][0-9]* Pro ([0-9A-F-]*)' | sed 's/.*(\(.*\))/\1/')
  (cd ios && xcodegen generate >/dev/null && xcodebuild -project FMCam.xcodeproj -scheme FMCam \
    -destination "id=$SIM" -derivedDataPath build/verificar CODE_SIGNING_ALLOWED=NO test 2>&1 \
    | grep -E 'Executed [0-9]+ tests|TEST SUCCEEDED|TEST FAILED|error:' | tail -3)
fi
echo "verificar: ok"
