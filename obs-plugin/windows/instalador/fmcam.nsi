; Instalador OBS Cam Mobile Extension (Windows x64). Gerado por obs-plugin/scripts/montar-instalador-windows.sh
; Defines vindos do script: VERSAO, EXT_DIR (pasta fmcam-extension), MSI, ICONE, SAIDA
Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!include "WinVer.nsh"

!define NOME "OBS Cam Mobile Extension"
!define CHAVE_UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\FMCAMExtension"
!define OBS_VERSAO_ALVO "33.0.1"

Name "${NOME}"
OutFile "${SAIDA}"
RequestExecutionLevel admin
BrandingText "Leonardo Rejani"

VIProductVersion "${VERSAO}.0"
VIAddVersionKey /LANG=1046 "ProductName" "${NOME}"
VIAddVersionKey /LANG=1046 "CompanyName" "Leonardo Rejani"
VIAddVersionKey /LANG=1046 "LegalCopyright" "Leonardo Rejani"
VIAddVersionKey /LANG=1046 "FileDescription" "Instalador da ${NOME}"
VIAddVersionKey /LANG=1046 "FileVersion" "${VERSAO}"
VIAddVersionKey /LANG=1046 "ProductVersion" "${VERSAO}"

!define MUI_ICON "${ICONE}"
!define MUI_UNICON "${ICONE}"
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TITLE "${NOME}"
!define MUI_WELCOMEPAGE_TEXT "Este instalador prepara o PC da transmissão para usar o iPhone como câmera no OBS.$\r$\n$\r$\nEle instala o serviço USB da Apple (se ainda não existir) e a ${NOME}.$\r$\n$\r$\nClique em Instalar para começar."
!define MUI_FINISHPAGE_TITLE "Tudo certo"
!define MUI_FINISHPAGE_TEXT "Pronto. Abra o OBS, em Fontes clique em + e escolha OBS Cam Mobile Extension. Ligue o iPhone no cabo com o OBS Cam Mobile aberto e toque em Confiar."

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "PortugueseBR"

Var PrecisaReiniciar

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "A ${NOME} só funciona no Windows de 64 bits." /SD IDOK
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_OK|MB_ICONSTOP "A ${NOME} precisa do Windows 10 ou 11." /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
  ; ProgramData (contexto "all"), caminho fixo, sem escolha de pasta
  StrCpy $INSTDIR "$APPDATA\obs-studio\plugins\fmcam-extension"
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Section "Instalar"
  SetRegView 64
  SetShellVarContext all
  StrCpy $PrecisaReiniciar "0"

  ; 1. OBS aberto: pede para fechar, nunca mata o processo
  verifica_obs:
  nsExec::ExecToStack 'cmd /c tasklist /FI "IMAGENAME eq obs64.exe" /NH | find /I "obs64.exe"'
  Pop $0
  Pop $1
  ${If} $0 == "0"
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "O OBS está aberto. Feche o OBS e clique em Tentar de novo." /SD IDCANCEL IDRETRY verifica_obs
    DetailPrint "Instalação cancelada: o OBS continua aberto."
    Abort "Instalação cancelada porque o OBS está aberto."
  ${EndIf}

  ; 2. Serviço USB da Apple
  DetailPrint "Verificando o serviço USB da Apple..."
  ClearErrors
  ReadRegStr $0 HKLM "SYSTEM\CurrentControlSet\Services\Apple Mobile Device Service" "ImagePath"
  ${If} ${Errors}
  ${OrIf} $0 == ""
    DetailPrint "Instalando o serviço USB da Apple (pode levar um minuto)..."
    SetOutPath "$PLUGINSDIR"
    File "/oname=$PLUGINSDIR\AppleMobileDeviceSupport64.msi" "${MSI}"
    ExecWait 'msiexec /i "$PLUGINSDIR\AppleMobileDeviceSupport64.msi" /qn /norestart' $0
    DetailPrint "msiexec terminou com o código $0"
    ${If} $0 == 0
      DetailPrint "Serviço USB da Apple instalado."
    ${ElseIf} $0 == 3010
      DetailPrint "Serviço USB da Apple instalado. O Windows pede reinício."
      StrCpy $PrecisaReiniciar "1"
    ${Else}
      MessageBox MB_OK|MB_ICONSTOP "Não foi possível instalar o serviço USB da Apple. O instalador da Apple devolveu o código $0. Nada da extensão foi instalado." /SD IDOK
      Abort "Falha no serviço USB da Apple (código $0)."
    ${EndIf}
  ${Else}
    DetailPrint "O serviço USB da Apple já está instalado. Pulando."
  ${EndIf}

  ; 3. Extensão
  DetailPrint "Instalando a ${NOME}..."
  RMDir /r "$INSTDIR"
  SetOutPath "$INSTDIR"
  File /r "${EXT_DIR}\*.*"

  ; 4. Desinstalador e entrada em Aplicativos instalados
  WriteUninstaller "$INSTDIR\desinstalar.exe"
  WriteRegStr HKLM "${CHAVE_UNINST}" "DisplayName" "${NOME}"
  WriteRegStr HKLM "${CHAVE_UNINST}" "DisplayVersion" "${VERSAO}"
  WriteRegStr HKLM "${CHAVE_UNINST}" "Publisher" "Leonardo Rejani"
  WriteRegStr HKLM "${CHAVE_UNINST}" "DisplayIcon" "$INSTDIR\desinstalar.exe"
  WriteRegStr HKLM "${CHAVE_UNINST}" "UninstallString" '"$INSTDIR\desinstalar.exe"'
  WriteRegStr HKLM "${CHAVE_UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKLM "${CHAVE_UNINST}" "NoModify" 1
  WriteRegDWORD HKLM "${CHAVE_UNINST}" "NoRepair" 1

  ; 5. Detecção do OBS (avisos, nunca bloqueiam)
  StrCpy $2 ""
  ReadRegStr $3 HKLM "SOFTWARE\OBS Studio" ""
  ${If} $3 != ""
  ${AndIf} ${FileExists} "$3\bin\64bit\obs64.exe"
    StrCpy $2 "$3\bin\64bit\obs64.exe"
  ${EndIf}
  ${If} $2 == ""
  ${AndIf} ${FileExists} "$PROGRAMFILES64\obs-studio\bin\64bit\obs64.exe"
    StrCpy $2 "$PROGRAMFILES64\obs-studio\bin\64bit\obs64.exe"
  ${EndIf}
  ${If} $2 == ""
    DetailPrint "OBS Studio não encontrado."
    MessageBox MB_OK|MB_ICONINFORMATION "Não encontrei o OBS Studio neste PC. A extensão precisa do OBS Studio ${OBS_VERSAO_ALVO} 64 bits. Instale o OBS e abra depois." /SD IDOK
  ${Else}
    GetDLLVersion "$2" $0 $1
    IntOp $4 $0 >> 16
    IntOp $5 $0 & 0xFFFF
    DetailPrint "OBS encontrado, versão $4.$5"
    ${If} $4 != 33
    ${OrIf} $5 != 0
      MessageBox MB_OK|MB_ICONEXCLAMATION "O OBS deste PC é a versão $4.$5. A extensão foi feita para o OBS ${OBS_VERSAO_ALVO} e pode não carregar." /SD IDOK
    ${EndIf}
  ${EndIf}

  ${If} $PrecisaReiniciar == "1"
    MessageBox MB_OK|MB_ICONINFORMATION "O serviço da Apple pede para reiniciar o Windows. Reinicie o PC antes de usar o iPhone pelo cabo." /SD IDOK
  ${EndIf}
SectionEnd

Section "Uninstall"
  SetRegView 64
  SetShellVarContext all
  nsExec::ExecToStack 'cmd /c tasklist /FI "IMAGENAME eq obs64.exe" /NH | find /I "obs64.exe"'
  Pop $0
  Pop $1
  ${If} $0 == "0"
    MessageBox MB_OK|MB_ICONEXCLAMATION "O OBS está aberto. Feche o OBS e rode o desinstalador de novo." /SD IDOK
    Abort
  ${EndIf}
  ; remove só a pasta da extensão e a entrada. O serviço da Apple fica.
  RMDir /r "$APPDATA\obs-studio\plugins\fmcam-extension"
  DeleteRegKey HKLM "${CHAVE_UNINST}"
SectionEnd
