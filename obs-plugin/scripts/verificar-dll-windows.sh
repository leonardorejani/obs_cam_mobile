#!/bin/sh
# Confere sem Windows: formato PE32+ x86-64, imports e exports da DLL.
set -eu
DLL="$1"; R="${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}/llvm-readobj"
echo "--- cabecalho"; "$R" --file-headers "$DLL" | grep -E "Format|Arch:|AddressSize|Machine"; "$R" --file-headers "$DLL" | grep -E "Magic|Subsystem|MajorOperatingSystemVersion|Characteristics" | head -4; echo "(Magic 0x20B = PE32+)"
echo "--- imports (DLLs)"; "$R" --coff-imports "$DLL" | awk '/^Import \{/{f=1} f&&/Name:/&&/\.(dll|DLL)/{print $2}' | sort -u
echo "--- exports"; "$R" --coff-exports "$DLL" | awk '/Name:/{print $2}' | sort
echo "--- caminhos do Mac na DLL (deve ficar vazio)"; strings "$DLL" | grep -E "/Users/|/opt/homebrew|/Applications" || echo "(nenhum)"
