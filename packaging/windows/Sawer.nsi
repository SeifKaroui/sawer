!ifndef APP_VERSION
  !error "APP_VERSION must be supplied by the release build"
!endif

!ifndef APP_EXE
  !error "APP_EXE must point to the tested Sawer executable"
!endif

!ifndef OUTPUT_DIR
  !error "OUTPUT_DIR must point to the release asset directory"
!endif

!define APP_NAME "Sawer"
!define APP_GUID "{49266FD7-A24C-40B7-9364-AB6615BF4510}"
!define APP_URL "https://github.com/SeifKaroui/sawer"
!define APP_EXE_NAME "Sawer.exe"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_GUID}"

Unicode true
RequestExecutionLevel user
ManifestDPIAware true
ManifestSupportedOS Win10
Name "${APP_NAME} ${APP_VERSION}"
OutFile "${OUTPUT_DIR}\Sawer-Setup.exe"
InstallDir "$LOCALAPPDATA\Programs\Sawer"
InstallDirRegKey HKCU "${UNINSTALL_KEY}" "InstallLocation"
SetCompressor /FINAL zlib
Icon "..\..\assets\windows\Sawer.ico"
UninstallIcon "..\..\assets\windows\Sawer.ico"
BrandingText "Sawer"
AutoCloseWindow true
ShowInstDetails nevershow
ShowUninstDetails nevershow
VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "Sawer"
VIAddVersionKey /LANG=1033 "ProductVersion" "${APP_VERSION}"
VIAddVersionKey /LANG=1033 "FileVersion" "${APP_VERSION}"
VIAddVersionKey /LANG=1033 "FileDescription" "Sawer Installer"
VIAddVersionKey /LANG=1033 "CompanyName" "Sawer"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright (C) 2026 Sawer"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "nsDialogs.nsh"
!include "WinMessages.nsh"
!include "x64.nsh"

!define MUI_ICON "..\..\assets\windows\Sawer.ico"
!define MUI_UNICON "..\..\assets\windows\Sawer.ico"
!define MUI_ABORTWARNING

Page custom ConfirmPage
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var ConfirmDialog

!macro CheckApplicationWritable
  ${If} ${FileExists} "$INSTDIR\${APP_EXE_NAME}"
    ClearErrors
    FileOpen $0 "$INSTDIR\${APP_EXE_NAME}" a
    ${If} ${Errors}
      MessageBox MB_OK|MB_ICONEXCLAMATION "Close Sawer before continuing and check that the installation folder is writable." /SD IDOK
      SetErrorLevel 2
      Quit
    ${EndIf}
    FileClose $0
  ${EndIf}
!macroend

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "Sawer requires 64-bit Windows 10 or Windows 11."
    Abort
  ${EndIf}
  SetShellVarContext current
FunctionEnd

Function ConfirmPage
  !insertmacro MUI_HEADER_TEXT "Install Sawer" "A fast, native, local-first whiteboard."
  nsDialogs::Create 1018
  Pop $ConfirmDialog
  ${If} $ConfirmDialog == error
    Abort
  ${EndIf}

  ${NSD_CreateLabel} 0 12u 100% 38u "Sawer will be installed for your Windows account. It needs no administrator access, account, or restart."
  Pop $0
  ${NSD_CreateLabel} 0 58u 100% 24u "Choose Install and Sawer will open when it is ready."
  Pop $0

  GetDlgItem $0 $HWNDPARENT 1
  SendMessage $0 ${WM_SETTEXT} 0 "STR:Install"
  nsDialogs::Show
FunctionEnd

Section "Sawer" SEC_SAWER
  SetShellVarContext current
  !insertmacro CheckApplicationWritable
  SetOutPath "$INSTDIR"
  SetOverwrite on
  ClearErrors
  File /oname=${APP_EXE_NAME} "${APP_EXE}"
  ${If} ${Errors}
    MessageBox MB_OK|MB_ICONSTOP "Sawer could not be installed. Check that the installation folder is writable." /SD IDOK
    SetErrorLevel 3
    Quit
  ${EndIf}
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateShortCut "$SMPROGRAMS\Sawer.lnk" "$INSTDIR\${APP_EXE_NAME}" "" "$INSTDIR\${APP_EXE_NAME}" 0
  CreateShortCut "$DESKTOP\Sawer.lnk" "$INSTDIR\${APP_EXE_NAME}" "" "$INSTDIR\${APP_EXE_NAME}" 0

  WriteRegStr HKCU "Software\Classes\Sawer.Board" "" "Sawer board"
  WriteRegStr HKCU "Software\Classes\Sawer.Board" "FriendlyTypeName" "Sawer board"
  WriteRegStr HKCU "Software\Classes\Sawer.Board\DefaultIcon" "" '"$INSTDIR\${APP_EXE_NAME}",0'
  WriteRegStr HKCU "Software\Classes\Sawer.Board\shell\open\command" "" '"$INSTDIR\${APP_EXE_NAME}" "%1"'

  ReadRegStr $0 HKCU "Software\Classes\.sawer" ""
  ${If} $0 == ""
    WriteRegStr HKCU "Software\Classes\.sawer" "" "Sawer.Board"
  ${EndIf}
  WriteRegStr HKCU "Software\Classes\.sawer\OpenWithProgids" "Sawer.Board" ""

  WriteRegStr HKCU "Software\Classes\Applications\Sawer.exe\SupportedTypes" ".sawer" ""
  WriteRegStr HKCU "Software\Classes\Applications\Sawer.exe\shell\open\command" "" '"$INSTDIR\${APP_EXE_NAME}" "%1"'

  WriteRegStr HKCU "Software\Sawer\Capabilities" "ApplicationName" "Sawer"
  WriteRegStr HKCU "Software\Sawer\Capabilities" "ApplicationDescription" "A fast, native, local-first whiteboard."
  WriteRegStr HKCU "Software\Sawer\Capabilities" "ApplicationIcon" '"$INSTDIR\${APP_EXE_NAME}",0'
  WriteRegStr HKCU "Software\Sawer\Capabilities\FileAssociations" ".sawer" "Sawer.Board"
  WriteRegStr HKCU "Software\RegisteredApplications" "Sawer" "Software\Sawer\Capabilities"

  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "Sawer"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "Sawer"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" '"$INSTDIR\${APP_EXE_NAME}",0'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "URLInfoAbout" "${APP_URL}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "URLUpdateInfo" "${APP_URL}/releases"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1

  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

  IfSilent installation_complete
  Exec '"$INSTDIR\${APP_EXE_NAME}"'

installation_complete:
SectionEnd

Section "Uninstall"
  SetShellVarContext current
  !insertmacro CheckApplicationWritable

  Delete "$DESKTOP\Sawer.lnk"
  Delete "$SMPROGRAMS\Sawer.lnk"
  Delete "$INSTDIR\${APP_EXE_NAME}"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  DeleteRegKey HKCU "Software\Classes\Sawer.Board"
  DeleteRegKey HKCU "Software\Classes\Applications\Sawer.exe"
  DeleteRegValue HKCU "Software\Classes\.sawer\OpenWithProgids" "Sawer.Board"
  DeleteRegKey /ifempty HKCU "Software\Classes\.sawer\OpenWithProgids"

  ReadRegStr $0 HKCU "Software\Classes\.sawer" ""
  ${If} $0 == "Sawer.Board"
    DeleteRegValue HKCU "Software\Classes\.sawer" ""
  ${EndIf}
  DeleteRegKey /ifempty HKCU "Software\Classes\.sawer"

  DeleteRegValue HKCU "Software\RegisteredApplications" "Sawer"
  DeleteRegKey HKCU "Software\Sawer\Capabilities"
  DeleteRegKey /ifempty HKCU "Software\Sawer"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"

  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd
