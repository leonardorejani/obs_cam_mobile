#!/bin/sh
# Prepara um iPhone novo pelo cabo: confiança, modo de desenvolvedor e o OBS Cam Mobile.
# O que só a pessoa faz no iPhone (Confiar, senha, ativar o modo depois de reiniciar, LocalDevVPN,
# login no iloader e no SideStore) o script espera e avisa.
# No iloader, instalar "LiveContainer + SideStore (Nightly)": o Stable 3.8.0 errava o endereço do iPhone na VPN
# (túnel para 10.7.1.2, medido em 23/11/2026). No SideStore: On-Device Anisette desligado, Anisette Server =
# mac-ani.jkcoxson.com. Login com 503: desde 20/11/2026 a Apple recusa quem se identifica como "macOS;". Em Settings >
# User Customizations > Anisette Client Configuration: Fetch, Save as Overrides e Client Info =
# <MacBookPro13,3> <Mac OS X;26.6;25F84> <com.apple.AuthKit/1 (com.apple.akd/1.0)>
# (funcionou em 24/11/2026, SideStore/SideStore#1745). Nunca tocar em Reset to Defaults.
# Uso:
#   sh tools/preparar-iphone.sh esperar      espera um iPhone que ainda não está na lista de conhecidos
#   sh tools/preparar-iphone.sh devmode UDID confia e liga o modo de desenvolvedor
#   sh tools/preparar-iphone.sh ipa UDID     copia o OBSCamMobile.ipa para o SideStore do aparelho
set -eu
cd "$(dirname "$0")/.."
CONHECIDOS="${CONHECIDOS:-00008130-00056554110A001C}"
IPA="dist/OBSCamMobile.ipa"

udids() { idevice_id -l 2>/dev/null | sort -u; }

case "${1:-}" in
esperar)
  echo "esperando um iPhone novo no cabo (conhecidos: $CONHECIDOS)"
  while :; do
    for u in $(udids); do
      echo "$CONHECIDOS" | grep -q "$u" && continue
      echo "iPhone novo: $u"
      ideviceinfo -u "$u" -k ProductType 2>/dev/null || true
      exit 0
    done
    sleep 3
  done ;;
devmode)
  u="$2"
  echo "confiança: toque em Confiar no iPhone e digite a senha"
  until idevicepair -u "$u" pair >/dev/null 2>&1; do sleep 3; done
  echo "pareado"
  devmodectl single "$u" || true
  echo "se o iPhone reiniciar, toque em Ativar no aviso do modo de desenvolvedor"
  until devmodectl list 2>/dev/null | grep "$u" | grep -q enabled; do sleep 5; done
  echo "modo de desenvolvedor ligado" ;;
ipa)
  u="$2"
  cd_id=$(xcrun devicectl list devices 2>/dev/null | awk -v n="$(ideviceinfo -u "$u" -k DeviceName 2>/dev/null)" '$0 ~ n {for(i=1;i<=NF;i++) if ($i ~ /^[0-9A-F-]{36}$/) print $i}' | head -1)
  b=$(xcrun devicectl device info apps --device "$cd_id" --include-all-apps 2>/dev/null | grep -o -E "com\.(kdt\.livecontainer|SideStore\.SideStore)\.[A-Z0-9]+" | head -1)
  [ -n "$b" ] || { echo "SideStore ainda não está no iPhone"; exit 1; }
  xcrun devicectl device copy to --device "$cd_id" --source "$IPA" --destination Documents/OBSCamMobile.ipa \
    --domain-type appDataContainer --domain-identifier "$b" | grep -E "error|Last modification" | head -2
  echo "ipa copiado para $b" ;;
*) sed -n 2,14p "$0"; exit 1 ;;
esac
