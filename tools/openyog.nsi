; OpenYog — the Windows installer. tools/windows-cross-build.sh --installer
; builds it with MSYS2's makensis (under wine on Linux):
;
;   makensis /DVERSION=0.1.0 /DDEPLOY=<deploy dir> /DROOT=<source tree>
;            /DFILES=<uninstall list> /DOUTFILE=<setup .exe> tools/openyog.nsi
;
; DEPLOY is the folder the script fills (the exe, every DLL, the Qt
; plugins); FILES is an include it writes with a Delete per file and an RMDir
; per folder it installed, so uninstalling removes those and nothing else,
; whatever folder was chosen. A silent install is setup.exe /S [/D=folder].

Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"

!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenYog"

Name "OpenYog ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\OpenYog"
InstallDirRegKey HKLM "Software\OpenYog" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "OpenYog ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "OpenYog"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "OpenYog setup"
VIAddVersionKey "LegalCopyright" "GPL-3.0-or-later"

!define MUI_ICON "${ROOT}\qt\resources\openyog.ico"
!define MUI_UNICON "${ROOT}\qt\resources\openyog.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\openyog.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start OpenYog"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${ROOT}\GPL.txt"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_ICONSTOP "OpenYog needs 64-bit Windows."
        Abort
    ${EndIf}
    SetRegView 64
FunctionEnd

Function un.onInit
    SetRegView 64
FunctionEnd

Section "OpenYog" SecMain
    SectionIn RO
    SetOutPath "$INSTDIR"
    File /r "${DEPLOY}\*.*"
    WriteUninstaller "$INSTDIR\uninstall.exe"

    CreateDirectory "$SMPROGRAMS\OpenYog"
    CreateShortcut "$SMPROGRAMS\OpenYog\OpenYog.lnk" "$INSTDIR\openyog.exe"
    CreateShortcut "$SMPROGRAMS\OpenYog\Uninstall OpenYog.lnk" "$INSTDIR\uninstall.exe"

    WriteRegStr HKLM "Software\OpenYog" "InstallDir" "$INSTDIR"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "OpenYog"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "OpenYog contributors"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\openyog.exe"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr HKLM "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
    WriteRegStr HKLM "${UNINSTALL_KEY}" "URLInfoAbout" "https://github.com/AbdulOhab/openyog"
    WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD HKLM "${UNINSTALL_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "Desktop shortcut" SecDesktop
    CreateShortcut "$DESKTOP\OpenYog.lnk" "$INSTDIR\openyog.exe"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "OpenYog itself, with the libraries it needs."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "An OpenYog icon on the desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
    Delete "$DESKTOP\OpenYog.lnk"
    Delete "$SMPROGRAMS\OpenYog\OpenYog.lnk"
    Delete "$SMPROGRAMS\OpenYog\Uninstall OpenYog.lnk"
    RMDir "$SMPROGRAMS\OpenYog"
    ; the files the installer put there, and then the folders if empty
    !include "${FILES}"
    Delete "$INSTDIR\uninstall.exe"
    RMDir "$INSTDIR"
    DeleteRegKey HKLM "${UNINSTALL_KEY}"
    DeleteRegKey HKLM "Software\OpenYog"
SectionEnd
