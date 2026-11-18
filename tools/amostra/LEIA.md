# Amostra H.264 dos testes

Os testes ponta a ponta usam `amostra.h264` (1080p30, 10 s), que o git ignora (`*.h264`). Ela nasce sozinha no build:

    cmake -S . -B build-verificar -G Ninja && cmake --build build-verificar

O alvo `amostra` roda `tools/amostra/gerar.sh` e grava `build-verificar/amostra.h264`. Precisa de `ffmpeg` com libx264 (`brew install ffmpeg`). Para gerar na mão:

    sh tools/amostra/gerar.sh build-verificar/amostra.h264 10 1920x1080
