Unicode true
!include "LogicLib.nsh"
!include "${OWNED_FILES}"
Name "SoundFlow"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\SoundFlow"
InstallDirRegKey HKCU "Software\SoundFlow" "InstallDir"
RequestExecutionLevel user
SetCompressor zlib
LoadLanguageFile "${NSISDIR}\Contrib\Language files\SimpChinese.nlf"

!ifdef BUILD_UNINSTALLER
!uninstfinalize '${CAPTURE_CMD} "%1" "${UNINST_OUT}"'
Section
  WriteUninstaller "$EXEDIR\Uninstall.exe"
SectionEnd

UninstPage uninstConfirm
UninstPage instfiles

Function un.onInit
  ReadRegStr $1 HKCU "Software\SoundFlow" "InstallDir"
  ${If} $1 == ""
  ${OrIf} $1 != $INSTDIR
    Abort
  ${EndIf}
  !insertmacro SoundFlowVerifyOwnedDirs invalid
  Return
invalid:
  Abort
FunctionEnd

Section Uninstall
  !insertmacro SoundFlowVerifyOwnedDirs invalid
  Delete "$SMPROGRAMS\SoundFlow\SoundFlow.lnk"
  RMDir "$SMPROGRAMS\SoundFlow"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SoundFlow"
  DeleteRegKey HKCU "Software\SoundFlow"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "SoundFlow"
  !insertmacro SoundFlowDeleteOwnedFiles
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Return
invalid:
  Abort
SectionEnd
!else
Page directory
Page instfiles

Function .onVerifyInstDir
  !insertmacro SoundFlowVerifyOwnedDirs invalid
  ReadRegStr $1 HKCU "Software\SoundFlow" "InstallDir"
  ${If} $1 == $INSTDIR
  ${AndIf} ${FileExists} "$INSTDIR\SoundFlow.exe"
  ${AndIf} ${FileExists} "$INSTDIR\Uninstall.exe"
    Return
  ${EndIf}
  ClearErrors
  FindFirst $2 $3 "$INSTDIR\*"
  ${If} ${Errors}
    Return
  ${EndIf}
  ${Do}
    ${If} $3 != "."
    ${AndIf} $3 != ".."
      FindClose $2
      Goto invalid
    ${EndIf}
    ClearErrors
    FindNext $2 $3
  ${LoopUntil} ${Errors}
  FindClose $2
  Return
invalid:
  Abort
FunctionEnd

Section
  Call .onVerifyInstDir
  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*"
  File "${UNINSTALLER_EXE}"
  CreateDirectory "$SMPROGRAMS\SoundFlow"
  CreateShortCut "$SMPROGRAMS\SoundFlow\SoundFlow.lnk" "$INSTDIR\SoundFlow.exe"
  WriteRegStr HKCU "Software\SoundFlow" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SoundFlow" "DisplayName" "SoundFlow"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SoundFlow" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SoundFlow" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SoundFlow" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SoundFlow" "NoRepair" 1
SectionEnd
!endif
