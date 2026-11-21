; Robz Play - Windows installer (NSIS 3, 64-bit Unicode).
;
; Built by scripts/robzplay-package.sh, which passes:
;   VERSION     0.2.2
;   SOURCE      the staged application folder
;   OUTFILE     the installer to write
;   UNLIST      a generated list of Delete / RMDir lines, one per installed file and folder
;   SIZEKB      the size of the staged folder in KiB
;   ICON        gui/robzplay.ico
;
; It installs for the current user only, so it needs no administrator rights and shows no
; UAC prompt. The settings of the application (HKCU\Software\RobzPlay\RobzPlay) are neither
; written nor removed here: an update or a reinstall keeps the registered consoles.

Unicode true
Target amd64-unicode
ManifestDPIAware true
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 64

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

; /UPDATE: started by Robz Play itself to install a newer version. Only the progress page is
; shown, the installer waits for Robz Play to close, and it starts the new version at the end.
Var UpdateMode

!define APP_NAME "Robz Play"
!define APP_EXE "RobzPlay.exe"
!define APP_URL "https://github.com/kutsalkiremitci/robz-play"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\RobzPlay"

Name "${APP_NAME}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\Robz Play"
InstallDirRegKey HKCU "${UNINST_KEY}" "InstallLocation"
BrandingText "${APP_NAME} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APP_NAME} Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "AGPL-3.0-only"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
; the sunset of the icon beside the welcome and finish pages, the icon in the corner of the rest
; (makensis works in the folder of this script, so the paths are relative to it)
!define MUI_WELCOMEFINISHPAGE_BITMAP "..\assets\robzplay\logo\kurulum-yan.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "..\assets\robzplay\logo\kurulum-ust.bmp"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "$(WelcomeText)"
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "$(RunNow)"
!define MUI_FINISHPAGE_LINK "${APP_URL}"
!define MUI_FINISHPAGE_LINK_LOCATION "${APP_URL}"

!define MUI_PAGE_CUSTOMFUNCTION_PRE SkipInUpdateMode
!insertmacro MUI_PAGE_WELCOME
!define MUI_PAGE_CUSTOMFUNCTION_PRE SkipInUpdateMode
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_PAGE_CUSTOMFUNCTION_PRE SkipInUpdateMode
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; The first language is the one used when Windows itself is in neither of them.
!insertmacro MUI_LANGUAGE "Turkish"
!insertmacro MUI_LANGUAGE "English"

LangString WelcomeText ${LANG_TURKISH} "Robz Play, PS5'ini ev ağın üzerinden bilgisayarına bağlar ve GTA VI gibi 30 FPS çalışan oyunları 60 FPS akıcılığında gösterir.$\r$\n$\r$\nKurulum yönetici izni istemez; yalnızca senin kullanıcı hesabına kurulur. Daha önce kurduysan ayarların ve kayıtlı konsolların korunur."
LangString WelcomeText ${LANG_ENGLISH} "Robz Play connects your PS5 to this PC over your home network and shows 30 FPS games such as GTA VI as smoothly as 60 FPS.$\r$\n$\r$\nSetup needs no administrator rights; it installs for your user account only. If you installed it before, your settings and registered consoles are kept."
LangString RunNow ${LANG_TURKISH} "Robz Play'i şimdi aç"
LangString RunNow ${LANG_ENGLISH} "Open Robz Play now"
LangString AppRunning ${LANG_TURKISH} "Robz Play şu anda açık. Kapat, sonra Yeniden Dene'ye bas."
LangString AppRunning ${LANG_ENGLISH} "Robz Play is running. Close it, then choose Retry."

Function .onInit
	StrCpy $UpdateMode 0
	${GetParameters} $R0
	ClearErrors
	${GetOptions} $R0 "/UPDATE" $R1
	${IfNot} ${Errors}
		StrCpy $UpdateMode 1
	${EndIf}
FunctionEnd

Function SkipInUpdateMode
	${If} $UpdateMode == 1
		Abort
	${EndIf}
FunctionEnd

; Stops until the installed executable can be written, i.e. until the application is closed.
; During an update Robz Play is closing itself, so it is given up to 15 seconds first.
!macro EnsureNotRunning
	${If} ${FileExists} "$INSTDIR\${APP_EXE}"
		StrCpy $R9 0
		${Do}
			ClearErrors
			FileOpen $0 "$INSTDIR\${APP_EXE}" a
			${IfNot} ${Errors}
				FileClose $0
				${ExitDo}
			${EndIf}
			${If} $UpdateMode == 1
			${AndIf} $R9 < 60
				IntOp $R9 $R9 + 1
				Sleep 250
				${Continue}
			${EndIf}
			MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "$(AppRunning)" /SD IDCANCEL IDRETRY +2
			Abort
		${Loop}
	${EndIf}
!macroend

Section "Robz Play" SecMain
	SectionIn RO
	!insertmacro EnsureNotRunning

	; An older version in this folder: remove exactly the files it installed, so nothing of
	; it is left behind that the new uninstaller would not know about.
	${If} ${FileExists} "$INSTDIR\Uninstall.exe"
		ExecWait '"$INSTDIR\Uninstall.exe" /S _?=$INSTDIR'
		Delete "$INSTDIR\Uninstall.exe"
	${EndIf}

	SetOutPath "$INSTDIR"
	File /r "${SOURCE}\*.*"
	WriteUninstaller "$INSTDIR\Uninstall.exe"

	CreateShortcut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
	CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
	; for support: the folder with the start-up log (robzplay-acilis.log) and the stream logs
	CreateDirectory "$APPDATA\RobzPlay\RobzPlay\log"
	CreateShortcut "$SMPROGRAMS\${APP_NAME} günlükleri.lnk" "$APPDATA\RobzPlay\RobzPlay\log"

	WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
	WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
	WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${APP_NAME}"
	WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
	WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
	WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
	WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
	WriteRegStr HKCU "${UNINST_KEY}" "URLInfoAbout" "${APP_URL}"
	WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" ${SIZEKB}
	WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
	WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1

	; Windows remembers the icon it has shown for a path. Installed over a version with another
	; icon (0.4.0 brought a new one), the shortcuts kept the old picture until the next sign-in.
	; The shell is told that these three changed, and to read its icons again.
	System::Call 'shell32::SHChangeNotify(i 0x00002000, i 0x0005, w "$INSTDIR\${APP_EXE}", p 0)'
	System::Call 'shell32::SHChangeNotify(i 0x00002000, i 0x0005, w "$DESKTOP\${APP_NAME}.lnk", p 0)'
	System::Call 'shell32::SHChangeNotify(i 0x00002000, i 0x0005, w "$SMPROGRAMS\${APP_NAME}.lnk", p 0)'
	${RefreshShellIcons}

	${If} $UpdateMode == 1
		SetAutoClose true
		Exec '"$INSTDIR\${APP_EXE}"'
	${EndIf}
SectionEnd

Section "Uninstall"
	!insertmacro EnsureNotRunning

	Delete "$SMPROGRAMS\${APP_NAME}.lnk"
	Delete "$SMPROGRAMS\${APP_NAME} günlükleri.lnk"
	Delete "$DESKTOP\${APP_NAME}.lnk"

	; Only what was installed is removed - never the folder as a whole, which may be one
	; the user picked and keeps other things in.
	!include "${UNLIST}"
	Delete "$INSTDIR\Uninstall.exe"
	RMDir "$INSTDIR"

	DeleteRegKey HKCU "${UNINST_KEY}"
SectionEnd
