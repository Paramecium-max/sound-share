Unicode true
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

Section Uninstall
  Delete "$SMPROGRAMS\SoundFlow\SoundFlow.lnk"
  RMDir "$SMPROGRAMS\SoundFlow"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SoundFlow"
  DeleteRegKey HKCU "Software\SoundFlow"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "SoundFlow"
  Delete "$INSTDIR\SoundFlow.exe"
  Delete "$INSTDIR\Uninstall.exe"
  Delete "$INSTDIR\chrome_100_percent.pak"
  Delete "$INSTDIR\chrome_200_percent.pak"
  Delete "$INSTDIR\resources.pak"
  Delete "$INSTDIR\d3dcompiler_47.dll"
  Delete "$INSTDIR\ffmpeg.dll"
  Delete "$INSTDIR\libEGL.dll"
  Delete "$INSTDIR\libGLESv2.dll"
  Delete "$INSTDIR\vk_swiftshader.dll"
  Delete "$INSTDIR\vulkan-1.dll"
  Delete "$INSTDIR\snapshot_blob.bin"
  Delete "$INSTDIR\v8_context_snapshot.bin"
  Delete "$INSTDIR\icudtl.dat"
  Delete "$INSTDIR\vk_swiftshader_icd.json"
  Delete "$INSTDIR\version"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\LICENSES.chromium.html"
  RMDir /r "$INSTDIR\locales"
  RMDir /r "$INSTDIR\resources"
  RMDir "$INSTDIR"
SectionEnd
!else
Page directory
Page instfiles

Section
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
