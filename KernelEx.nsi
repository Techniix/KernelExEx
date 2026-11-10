  ;ANSI installer: Unicode NSIS installers do not run on Windows 9x
  Unicode false

  !define _VERSION '4.7.0'
  !define _VERSION_CODE 0x04070000
  
  !ifndef _DEBUG
    !define FLAVOUR 'Release'
    !define VERSION '${_VERSION}'
  !else
    !define FLAVOUR 'Debug'
    !define VERSION '${_VERSION} Debug'
  !endif

;--------------------------------
;KernelEx net modules (clang/lld build): copy beside KernelEx and map the
;DLL name through KernelEx's KnownDLLs so every process finds it.

!macro NetModule name KEY
  GetTempFileName $0 "$INSTDIR"
  File /oname=$0 net-out\${name}.dll
  Delete "$INSTDIR\${name}.dll"
  Rename /REBOOTOK $0 "$INSTDIR\${name}.dll"
  WriteRegStr HKLM "Software\KernelEx\KnownDLLs" "${KEY}" "${KEY}.DLL"
!macroend

;--------------------------------
;Includes

  !include "MUI2.nsh"
  !include "UpdateDLL.nsh"
  !include "WinVer.nsh"
  !include "nsDialogs.nsh"
  !include "WordFunc.nsh"

;--------------------------------
;General

  SetCompressor /SOLID lzma
  
  ;Name and file
  Name "KernelEx"
  Caption "KernelExEx ${VERSION} Setup"
  OutFile "KernelEx-dev.exe"

  ;Default installation folder
  InstallDir "$WINDIR\KernelEx"

  ;Get installation folder from registry if available
  InstallDirRegKey HKLM "Software\KernelEx" "InstallDir"

;--------------------------------
;Variables

  Var ENABLEBUTTON
  Var WARNING_TEXT
  Var StartMenuFolder

;--------------------------------
;Interface Settings

  !define MUI_ABORTWARNING
;  !define MUI_FINISHPAGE_NOAUTOCLOSE
;  !define MUI_UNFINISHPAGE_NOAUTOCLOSE

;--------------------------------
;Pages

  !insertmacro MUI_PAGE_WELCOME
  !insertmacro MUI_PAGE_LICENSE "License.txt"

  ;Start Menu Folder Page Configuration
  !define MUI_STARTMENUPAGE_REGISTRY_ROOT "HKLM" 
  !define MUI_STARTMENUPAGE_REGISTRY_KEY "Software\KernelEx" 
  !define MUI_STARTMENUPAGE_REGISTRY_VALUENAME "SMDir"
  !insertmacro MUI_PAGE_STARTMENU Application $StartMenuFolder

  !insertmacro MUI_PAGE_COMPONENTS
  !insertmacro MUI_PAGE_INSTFILES
  Page custom PageDefConfig PageLeaveDefConfig
  !insertmacro MUI_PAGE_FINISH

  !insertmacro MUI_UNPAGE_WELCOME
  !insertmacro MUI_UNPAGE_CONFIRM
  !insertmacro MUI_UNPAGE_INSTFILES
  !insertmacro MUI_UNPAGE_FINISH

;--------------------------------
;Languages

  !insertmacro MUI_LANGUAGE "English"
  
  LangString DESC_SHORTPRODUCT ${LANG_ENGLISH} "Microsoft Layer for Unicode"
  LangString DESC_LONGPRODUCT ${LANG_ENGLISH} "Microsoft Layer for Unicode on Windows 95, 98, and Me Systems, 1.1.3790.0"
  LangString DESC_PRODUCT_DECISION ${LANG_ENGLISH} "$(DESC_SHORTPRODUCT) is required.$\nIt is strongly \
    advised that you install$\n$(DESC_SHORTPRODUCT) before continuing.$\nIf you choose to continue, \
    you will need to connect$\nto the internet before proceeding.$\nWould you like to continue with \
    the installation?"
  LangString SEC_PRODUCT ${LANG_ENGLISH} "$(DESC_SHORTPRODUCT) "
  LangString DESC_INSTALLING ${LANG_ENGLISH} "Installing"
  LangString DESC_DOWNLOADING1 ${LANG_ENGLISH} "Downloading"
  LangString DESC_DOWNLOADFAILED ${LANG_ENGLISH} "Could not download MSLU. Put unicows.exe (the Microsoft Layer for Unicode redistributable) next to this installer and run it again. Error:"
  LangString ERROR_PRODUCT_INVALID_PATH ${LANG_ENGLISH} "The $(DESC_SHORTPRODUCT) Installation$\n\
    was not found in the following location:$\n"
  LangString ERROR_PRODUCT_FATAL ${LANG_ENGLISH} "A fatal error occurred during the installation$\n\
    of the $(DESC_SHORTPRODUCT)."
  LangString DESC_SETTINGS_PRESERVE ${LANG_ENGLISH} "Do you want to preserve custom settings?"

;--------------------------------
;Functions

Function PageDefConfig

  nsDialogs::Create /NOUNLOAD 1018
  Pop $0

  !insertmacro MUI_HEADER_TEXT "Default configuration" "Choose default configuration of KernelEx."
  nsDialogs::CreateItem /NOUNLOAD STATIC ${WS_VISIBLE}|${WS_CHILD}|${WS_CLIPSIBLINGS} 0 0 0 100% 40 "You can choose default KernelEx configuration here. This configuration will be used for all applications for which no other configuration is specified."
  Pop $R0
  
  nsDialogs::CreateItem /NOUNLOAD BUTTON ${BS_AUTORADIOBUTTON}|${BS_VCENTER}|${BS_MULTILINE}|${WS_VISIBLE}|${WS_CHILD}|${WS_CLIPSIBLINGS}|${WS_GROUP}|${WS_TABSTOP} 0 10 75 100% 30 "Enable KernelEx extensions for all applications (recommended)"
  Pop $ENABLEBUTTON
  ${NSD_OnClick} $ENABLEBUTTON ToggleWarning
  
  nsDialogs::CreateItem /NOUNLOAD BUTTON ${BS_AUTORADIOBUTTON}|${BS_TOP}|${BS_MULTILINE}|${WS_VISIBLE}|${WS_CHILD}|${WS_CLIPSIBLINGS} 0 10 105 100% 30 "Disable KernelEx extensions"
  Pop $R0
  ${NSD_OnClick} $R0 ToggleWarning
  
  nsDialogs::CreateItem /NOUNLOAD STATIC ${WS_CHILD}|${WS_CLIPSIBLINGS} 0 0 175 100% 40 "If you choose to disable KernelEx extensions, programs you run won't get extended API functions by default. You will have to enable KernelEx extensions individually for applications which require newer Windows versions. This choice is better if you're paranoid."
  Pop $WARNING_TEXT

  SendMessage $ENABLEBUTTON ${BM_SETCHECK} 1 0

  nsDialogs::Show

FunctionEnd

Function PageLeaveDefConfig

  SendMessage $ENABLEBUTTON ${BM_GETCHECK} 0 0 $R0
  ${If} $R0 == 1
	WriteRegDWORD HKLM "Software\KernelEx" "DisableExtensions" 0
  ${Else}
	WriteRegDWORD HKLM "Software\KernelEx" "DisableExtensions" 1
  ${EndIf}

FunctionEnd

Function ToggleWarning

	Pop $R0
	${If} $R0 == $ENABLEBUTTON
		ShowWindow $WARNING_TEXT ${SW_HIDE}
	${Else}
		ShowWindow $WARNING_TEXT ${SW_SHOW}
	${EndIf}

FunctionEnd

;--------------------------------
;Installer Section

Section "-MSLU" SECPRODUCT

  !define URL_PRODUCT "http://download.microsoft.com/download/b/7/5/b75eace3-00e2-4aa0-9a6f-0b6882c71642/unicows.exe"

  IfFileExists $SYSDIR\unicows.dll 0 lbl_download
  
  ;check version
  GetDllVersion $SYSDIR\unicows.dll $R0 $R1
  IntOp $R2 $R0 / 0x00010000
  IntOp $R3 $R0 & 0x0000FFFF
  IntOp $R4 $R1 / 0x00010000
  IntOp $R5 $R1 & 0x0000FFFF
  StrCmp "1.1.3790.0" "$R2.$R3.$R4.$R5" lbl_Skip
  
  lbl_download:
  ;Prefer the MSLU redistributable (unicows.exe) placed next to the installer:
  ;the old download link is gone and NSISdl cannot fetch over HTTPS.
  IfFileExists "$EXEDIR\unicows.exe" 0 lbl_net
  DetailPrint "Using $EXEDIR\unicows.exe"
  CopyFiles /SILENT "$EXEDIR\unicows.exe" "$TEMP\unicows.exe"
  Goto lbl_continue
  lbl_net:
  DetailPrint "$(DESC_DOWNLOADING1) $(DESC_SHORTPRODUCT)..."
  MessageBox MB_ICONEXCLAMATION|MB_YESNO|MB_DEFBUTTON2 "$(DESC_PRODUCT_DECISION)" /SD IDNO \
    IDYES +2 IDNO 0
  Abort
  ; Download
  AddSize 900
  nsisdl::download /TIMEOUT=30000 "${URL_PRODUCT}" "$TEMP\unicows.exe"
  Pop $0
  StrCmp "$0" "success" lbl_continue
    DetailPrint "$(DESC_DOWNLOADFAILED) $0"
    Abort
 
  lbl_continue:
  DetailPrint "$(DESC_INSTALLING) $(DESC_SHORTPRODUCT)..."
  Banner::show /NOUNLOAD "$(DESC_INSTALLING) $(DESC_SHORTPRODUCT)..."
  CreateDirectory "$INSTDIR\MSLU"
  ExecWait '"$TEMP\unicows.exe" /t:$INSTDIR\MSLU' $0
  Banner::destroy
  Delete "$TEMP\unicows.exe"
   
  ; obtain any error code and inform the user ($0)
  ; If nsExec is unable to execute the process,
  ; it will return "error"
  ; If the process timed out it will return "timeout"
  ; else it will return the return code from the executed process.
  StrCmp $0 "" lbl_Error
  StrCmp $0 "0" lbl_NoError
  ; all others are fatal
  DetailPrint "$(ERROR_PRODUCT_FATAL)[$0]"
  Goto lbl_commonError
 
  lbl_Error:
  DetailPrint "$(ERROR_PRODUCT_INVALID_PATH)"
  Goto lbl_commonError
  
  lbl_commonError:
  RMDir /r "$INSTDIR"
  Abort
 
  lbl_NoError:
  CopyFiles /SILENT "$INSTDIR\MSLU\unicows.dll" "$INSTDIR\mslu.tmp"
  Rename /REBOOTOK "$INSTDIR\mslu.tmp" "$SYSDIR\unicows.dll"

  lbl_Skip:

SectionEnd

Section "-Install"

  SetDetailsView show
  
  SetOutPath "$INSTDIR"
  
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\RunServicesOnce" "KexNeedsReboot" ""

  ;Revert KERNEL32.DLL file patch in case of upgrade
  IfFileExists "$WINDIR\SYSBCKUP\KERNEL32.DLL" 0 +7
    GetTempFileName $0 "$SYSDIR"
    Delete $0
    Rename /REBOOTOK "$WINDIR\SYSBCKUP\KERNEL32.DLL" $0
    Rename /REBOOTOK $0 "$SYSDIR\kernel32.dll"
    Delete "$INSTDIR\kernel32.bak"
    Goto Revert_Done

  IfFileExists "$INSTDIR\kernel32.bak" 0 +6
    GetTempFileName $0 "$SYSDIR"
    Delete $0
    Rename /REBOOTOK "$INSTDIR\kernel32.bak" $0
    Rename /REBOOTOK $0 "$SYSDIR\kernel32.dll"
    Goto Revert_Done
  
  Revert_Done:
  
  Delete /REBOOTOK "$INSTDIR\setupkex.exe"

  ;Files to install
  
  ;UpdateDLL_Func params:
  ;$R4 - target; $R5 - tempdir; $R6 - register?; $R7 - source
  GetTempFileName $0 "$INSTDIR"
  File /oname=$0 "Core\${FLAVOUR}\KernelEx.dll"
  StrCpy $R4 "$INSTDIR\KernelEx.dll"
  StrCpy $R6 "0"
  StrCpy $R7 $0
  Call UpgradeDLL_Func
  
  GetTempFileName $0 "$INSTDIR"
  File /oname=$0 "apilibs\kexbases\${FLAVOUR}\kexbases.dll"
  StrCpy $R4 "$INSTDIR\kexbases.dll"
  StrCpy $R6 "0"
  StrCpy $R7 $0
  Call UpgradeDLL_Func
  
  GetTempFileName $0 "$INSTDIR"
  File /oname=$0 "apilibs\kexbasen\${FLAVOUR}\kexbasen.dll"
  StrCpy $R4 "$INSTDIR\kexbasen.dll"
  StrCpy $R6 "0"
  StrCpy $R7 $0
  Call UpgradeDLL_Func
  
  GetTempFileName $0 "$INSTDIR"
  File /oname=$0 "sheet\${FLAVOUR}\sheet.dll"
  StrCpy $R4 "$INSTDIR\sheet.dll"
  StrCpy $R6 "1"
  StrCpy $R7 $0
  Call UpgradeDLL_Func
  
  GetTempFileName $0 "$INSTDIR"
  File /oname=$0 "kexCOM\${FLAVOUR}\kexCOM.dll"
  StrCpy $R4 "$INSTDIR\kexCOM.dll"
  StrCpy $R6 "1"
  StrCpy $R7 $0
  Call UpgradeDLL_Func

  SetOverwrite on
  
  File apilibs\core.ini
  File apilibs\settings.reg
  File license.txt
  File "Release Notes.txt"
  
  GetTempFileName $0 "$INSTDIR"
  File /oname=$0 "vxd\${FLAVOUR}\VKrnlEx.vxd"
  Delete "$INSTDIR\VKrnlEx.vxd"
  Rename /REBOOTOK $0 "$INSTDIR\VKrnlEx.vxd"
  
  GetTempFileName $0 "$INSTDIR"
  File /oname=$0 auxiliary\msimg32.dll
  Delete "$INSTDIR\msimg32.dll"
  Rename /REBOOTOK $0  "$INSTDIR\msimg32.dll"
  WriteRegStr HKLM "Software\KernelEx\KnownDLLs" \
    "MSIMG32" "MSIMG32.DLL"
  
  GetTempFileName $0 "$INSTDIR"
  File /oname=$0 auxiliary\pdh.dll
  Delete "$INSTDIR\pdh.dll"
  Rename /REBOOTOK $0  "$INSTDIR\pdh.dll"
  WriteRegStr HKLM "Software\KernelEx\KnownDLLs" \
    "PDH" "PDH.DLL"
  
  GetTempFileName $0 "$INSTDIR"
  File /oname=$0 auxiliary\psapi.dll
  Delete "$INSTDIR\psapi.dll"
  Rename /REBOOTOK $0  "$INSTDIR\psapi.dll"
  WriteRegStr HKLM "Software\KernelEx\KnownDLLs" \
    "PSAPI" "PSAPI.DLL"
  
  GetTempFileName $0 "$INSTDIR"
  File /oname=$0 auxiliary\uxtheme.dll
  Delete "$INSTDIR\uxtheme.dll"
  Rename /REBOOTOK $0  "$INSTDIR\uxtheme.dll"
  WriteRegStr HKLM "Software\KernelEx\KnownDLLs" \
    "UXTHEME" "UXTHEME.DLL"
  
  GetTempFileName $0 "$INSTDIR"
  File /oname=$0 auxiliary\wtsapi32.dll
  Delete "$INSTDIR\wtsapi32.dll"
  Rename /REBOOTOK $0  "$INSTDIR\wtsapi32.dll"
  WriteRegStr HKLM "Software\KernelEx\KnownDLLs" \
    "WTSAPI32" "WTSAPI32.DLL"
  
  GetTempFileName $0 "$INSTDIR"
  File /oname=$0 auxiliary\userenv.dll
  Delete "$INSTDIR\userenv.dll"
  Rename /REBOOTOK $0  "$INSTDIR\userenv.dll"
  WriteRegStr HKLM "Software\KernelEx\KnownDLLs" \
    "USERENV" "USERENV.DLL"
  
  ;KernelEx net: crypto + TLS (kextls), CNG subset (bcrypt), WinHTTP (winhttp)
  ;built by net/tools/build_win9x.py (staged in net-out by toolchain/build_installer.py)
  !insertmacro NetModule kextls KEXTLS
  !insertmacro NetModule bcrypt BCRYPT
  !insertmacro NetModule winhttp WINHTTP
  ;kexrt is an API library only (loaded by the resolver, not by name)
  File /oname=kexrt.dll net-out\kexrt.dll
  CreateDirectory "$INSTDIR\certs"
  CreateDirectory "$INSTDIR\certs\local"
  File /oname=certs\roots.pem net\certs\roots.pem
  
  SetOverwrite lastused
  
  ExecWait '"$WINDIR\regedit.exe" /s "$INSTDIR\settings.reg"'
  Delete "$INSTDIR\settings.reg"
  
  CreateDirectory $WINDIR\AppPatch\Custom
  File /oname=$WINDIR\AppPatch\Custom\KernelEx.sdb "util\sdbcreate\sdbdb\KernelEx.sdb"  
  File "util\sdbcreate\sdbdb\kexsdb.i.reg"
  ExecWait '"$WINDIR\regedit.exe" /s "$INSTDIR\kexsdb.i.reg"'
  Delete "$INSTDIR\kexsdb.i.reg"

  ;Store installation folder
  WriteRegStr HKLM "Software\KernelEx" "InstallDir" $INSTDIR
  
  ;Store run key
  WriteRegStr HKLM "System\CurrentControlSet\Services\VxD\VKRNLEX" "StaticVxD" "$INSTDIR\VKrnlEx.vxd"
  WriteRegStr HKLM "System\CurrentControlSet\Control\MPRServices\KernelEx" "DLLName" "$INSTDIR\KernelEx.dll"
  WriteRegStr HKLM "System\CurrentControlSet\Control\MPRServices\KernelEx" "EntryPoint" "_MprStart@4"
  WriteRegDWORD HKLM "System\CurrentControlSet\Control\MPRServices\KernelEx" "StackSize" 0x1000
  
  ;Store uninstaller key
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\KernelEx" "DisplayName" "KernelEx ${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\KernelEx" "UninstallString" '"$INSTDIR\uninstall.exe"'
  
  ;Write verifier
  SetOverWrite on
  File verify\${FLAVOUR}\verify.exe
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Run" \
    "KexVerify" "$INSTDIR\verify.exe"
  SetOverwrite lastused
  
  ;Create uninstaller
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ;Create shortcuts
  !insertmacro MUI_STARTMENU_WRITE_BEGIN Application
    CreateDirectory "$SMPROGRAMS\$StartMenuFolder"
    CreateShortCut "$SMPROGRAMS\$StartMenuFolder\Release Notes.lnk" "$INSTDIR\Release Notes.txt"
    CreateShortCut "$SMPROGRAMS\$StartMenuFolder\Verify Installation.lnk" "$INSTDIR\verify.exe"
    CreateShortCut "$SMPROGRAMS\$StartMenuFolder\KernelEx Home Page.lnk" "http://kernelex.sourceforge.net/"
    CreateShortCut "$SMPROGRAMS\$StartMenuFolder\KernelEx Wiki.lnk" "http://kernelex.sourceforge.net/wiki/"
    CreateShortCut "$SMPROGRAMS\$StartMenuFolder\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  !insertmacro MUI_STARTMENU_WRITE_END

  SetRebootFlag true

SectionEnd

;--------------------------------
;Optional: game controllers

!define DEVPATH_KEY "Software\Microsoft\Windows\CurrentVersion\Setup"

SectionGroup "Game controllers" SECGAMEPAD

Section "XInput for modern games" SECXINPUT
  ;xinput1_3.dll on top of the joystick API, with SDL-style mappings
  SetOutPath "$SYSDIR"
  File "net-out\xinput1_3.dll"
  ;one DLL answers to every XInput name games link against
  File "/oname=xinput1_1.dll" "net-out\xinput1_3.dll"
  File "/oname=xinput1_2.dll" "net-out\xinput1_3.dll"
  File "/oname=xinput1_4.dll" "net-out\xinput1_3.dll"
  File "/oname=xinput9_1_0.dll" "net-out\xinput1_3.dll"
  WriteRegStr HKLM "Software\KernelEx\Components" "XInput" "1"
  SetOutPath "$INSTDIR\gamepad"
  File "input\gamepad\gamecontrollerdb.txt"
  SetOutPath "$INSTDIR"
SectionEnd

Section "Xbox 360-protocol USB controller driver" SECKEXPAD
  ;kexpad.sys and its INF stay together in one folder that is added to
  ;DevicePath, so Windows finds them when a controller is plugged in.
  SetOutPath "$INSTDIR\drivers\kexpad"
  File "input\kexpad\kexpad.sys"
  File "input\kexpad\kexpad.inf"
  SetOutPath "$INSTDIR"
  ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
  StrCmp $0 "" 0 +2
    StrCpy $0 "$WINDIR\INF"
  ${WordFind} "$0" "$INSTDIR\drivers\kexpad" "E+1{" $1
  IfErrors 0 kexpad_listed
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" "$0;$INSTDIR\drivers\kexpad"
  kexpad_listed:
  WriteRegStr HKLM "Software\KernelEx\Components" "KexPad" "1"
SectionEnd

SectionGroupEnd

LangString DESC_GAMEPAD ${LANG_ENGLISH} "Support for modern game controllers."
LangString DESC_XINPUT ${LANG_ENGLISH} "XInput (xinput1_3.dll) for games that need it. Works with any controller Windows sees; layouts follow the SDL controller database."
LangString DESC_KEXPAD ${LANG_ENGLISH} "Driver for Xbox 360 and compatible USB pads (8BitDo, PowerA, PDP, Hori and others). Windows uses it when you plug one in."

;--------------------------------
;Optional: Protoweb proxy (off unless the user ticks it)
;Sends plain HTTP and FTP through Protoweb's archive proxy. HTTPS is left
;direct, so the TLS stack still reaches the real web. The previous settings
;are saved and put back on uninstall.

!define PW_PROXY "http=wayback.protoweb.org:7851;ftp=wayback.protoweb.org:7851"

Section /o "Use the Protoweb proxy for HTTP and FTP" SECPROTOWEB
  ReadRegStr $0 HKLM "Software\KernelEx\ProtowebBackup" "Saved"
  StrCmp $0 "1" pw_saved
    ReadRegDWORD $1 HKCU "Software\Microsoft\Windows\CurrentVersion\Internet Settings" "ProxyEnable"
    ReadRegStr $2 HKCU "Software\Microsoft\Windows\CurrentVersion\Internet Settings" "ProxyServer"
    ReadRegStr $3 HKCU "Software\Microsoft\Windows\CurrentVersion\Internet Settings" "ProxyOverride"
    WriteRegDWORD HKLM "Software\KernelEx\ProtowebBackup" "ProxyEnable" $1
    WriteRegStr HKLM "Software\KernelEx\ProtowebBackup" "ProxyServer" $2
    WriteRegStr HKLM "Software\KernelEx\ProtowebBackup" "ProxyOverride" $3
    WriteRegStr HKLM "Software\KernelEx\ProtowebBackup" "Saved" "1"
  pw_saved:
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Internet Settings" "ProxyEnable" 1
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Internet Settings" "ProxyServer" "${PW_PROXY}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Internet Settings" "ProxyOverride" "<local>"
SectionEnd

LangString DESC_PROTOWEB ${LANG_ENGLISH} "Browse restored 1990s websites. Plain HTTP and FTP go through wayback.protoweb.org:7851; HTTPS stays direct. Your old proxy settings are restored on uninstall."
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SECPROTOWEB} $(DESC_PROTOWEB)
  !insertmacro MUI_DESCRIPTION_TEXT ${SECGAMEPAD} $(DESC_GAMEPAD)
  !insertmacro MUI_DESCRIPTION_TEXT ${SECXINPUT} $(DESC_XINPUT)
  !insertmacro MUI_DESCRIPTION_TEXT ${SECKEXPAD} $(DESC_KEXPAD)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

;--------------------------------
;Uninstaller Section

Section "Uninstall"

  SetDetailsView show

  ;Game controllers
  ReadRegStr $0 HKLM "Software\KernelEx\Components" "XInput"
  StrCmp $0 "1" 0 +7
    Delete /REBOOTOK "$SYSDIR\xinput1_3.dll"
    Delete /REBOOTOK "$SYSDIR\xinput1_1.dll"
    Delete /REBOOTOK "$SYSDIR\xinput1_2.dll"
    Delete /REBOOTOK "$SYSDIR\xinput1_4.dll"
    Delete /REBOOTOK "$SYSDIR\xinput9_1_0.dll"
    Delete "$INSTDIR\gamepad\gamecontrollerdb.txt"
  RMDir "$INSTDIR\gamepad"
  ReadRegStr $0 HKLM "Software\KernelEx\Components" "KexPad"
  StrCmp $0 "1" 0 un_kexpad_done
    ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
    ${un.WordReplace} "$0" ";$INSTDIR\drivers\kexpad" "" "+" $1
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" $1
    Delete /REBOOTOK "$INSTDIR\drivers\kexpad\kexpad.sys"
    Delete "$INSTDIR\drivers\kexpad\kexpad.inf"
    RMDir "$INSTDIR\drivers\kexpad"
    RMDir "$INSTDIR\drivers"
  un_kexpad_done:
  DeleteRegKey HKLM "Software\KernelEx\Components"

  ;Put back the proxy settings saved before Protoweb was turned on
  ReadRegStr $0 HKLM "Software\KernelEx\ProtowebBackup" "Saved"
  StrCmp $0 "1" 0 pw_none
    ReadRegStr $2 HKCU "Software\Microsoft\Windows\CurrentVersion\Internet Settings" "ProxyServer"
    StrCmp $2 "${PW_PROXY}" 0 pw_drop
      ReadRegDWORD $1 HKLM "Software\KernelEx\ProtowebBackup" "ProxyEnable"
      ReadRegStr $2 HKLM "Software\KernelEx\ProtowebBackup" "ProxyServer"
      ReadRegStr $3 HKLM "Software\KernelEx\ProtowebBackup" "ProxyOverride"
      WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Internet Settings" "ProxyEnable" $1
      WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Internet Settings" "ProxyServer" $2
      WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Internet Settings" "ProxyOverride" $3
    pw_drop:
    DeleteRegKey HKLM "Software\KernelEx\ProtowebBackup"
  pw_none:

  ;Files to uninstall
  IfFileExists "$WINDIR\SYSBCKUP\KERNEL32.DLL" 0 +5
    GetTempFileName $0 "$SYSDIR"
    Delete $0
    CopyFiles /SILENT "$WINDIR\SYSBCKUP\KERNEL32.DLL" $0
    Rename /REBOOTOK $0 "$SYSDIR\kernel32.dll"

  Delete /REBOOTOK "$INSTDIR\KernelEx.dll"
  Delete /REBOOTOK "$INSTDIR\kexbases.dll"
  Delete /REBOOTOK "$INSTDIR\kexbasen.dll"
  Delete "$INSTDIR\core.ini"
  UnRegDLL "$INSTDIR\sheet.dll"
  Delete /REBOOTOK "$INSTDIR\sheet.dll"
  UnRegDLL "$INSTDIR\kexCOM.dll"
  Delete /REBOOTOK "$INSTDIR\kexCOM.dll"
  Delete "$INSTDIR\license.txt"
  Delete "$INSTDIR\Release Notes.txt"
  
  Delete /REBOOTOK "$INSTDIR\VKrnlEx.vxd"
  
  Delete /REBOOTOK "$INSTDIR\msimg32.dll"
  DeleteRegValue HKLM "Software\KernelEx\KnownDLLs" "MSIMG32"
  Delete /REBOOTOK "$INSTDIR\pdh.dll"
  DeleteRegValue HKLM "Software\KernelEx\KnownDLLs" "PDH"
  Delete /REBOOTOK "$INSTDIR\psapi.dll"
  DeleteRegValue HKLM "Software\KernelEx\KnownDLLs" "PSAPI"
  Delete /REBOOTOK "$INSTDIR\uxtheme.dll"
  DeleteRegValue HKLM "Software\KernelEx\KnownDLLs" "UXTHEME"
  Delete /REBOOTOK "$INSTDIR\wtsapi32.dll"
  DeleteRegValue HKLM "Software\KernelEx\KnownDLLs" "WTSAPI32"
  Delete /REBOOTOK "$INSTDIR\userenv.dll"
  DeleteRegValue HKLM "Software\KernelEx\KnownDLLs" "USERENV"
  Delete /REBOOTOK "$INSTDIR\kextls.dll"
  DeleteRegValue HKLM "Software\KernelEx\KnownDLLs" "KEXTLS"
  Delete /REBOOTOK "$INSTDIR\bcrypt.dll"
  DeleteRegValue HKLM "Software\KernelEx\KnownDLLs" "BCRYPT"
  Delete /REBOOTOK "$INSTDIR\winhttp.dll"
  DeleteRegValue HKLM "Software\KernelEx\KnownDLLs" "WINHTTP"
  Delete /REBOOTOK "$INSTDIR\kexrt.dll"
  Delete "$INSTDIR\certs\roots.pem"
  RMDir "$INSTDIR\certs\local"
  RMDir "$INSTDIR\certs"
  
  File "util\sdbcreate\sdbdb\kexsdb.u.reg"
  ExecWait '"$WINDIR\regedit.exe" /s "$INSTDIR\kexsdb.u.reg"'
  Delete "$INSTDIR\kexsdb.u.reg"
  Delete /REBOOTOK "$WINDIR\AppPatch\Custom\KernelEx.sdb"
  RMDir "$WINDIR\AppPatch\Custom"
  
  Delete "$INSTDIR\verify.exe"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "KexVerify"

  Delete "$INSTDIR\Uninstall.exe"

  RMDir /r "$INSTDIR\MSLU"
  WriteINIStr $WINDIR\wininit.ini Rename DIRNUL $INSTDIR

  ;remove Start Menu shortcuts
  !insertmacro MUI_STARTMENU_GETFOLDER Application $StartMenuFolder
  RMDir /r "$SMPROGRAMS\$StartMenuFolder"

  MessageBox MB_ICONQUESTION|MB_YESNO|MB_DEFBUTTON2 "$(DESC_SETTINGS_PRESERVE)" IDYES +2 IDNO 0
    DeleteRegKey HKLM "Software\KernelEx"
  
  DeleteRegKey HKLM "System\CurrentControlSet\Services\VxD\VKRNLEX"
  DeleteRegKey HKLM "System\CurrentControlSet\Control\MPRServices\KernelEx"
  DeleteRegKey /ifempty HKLM "System\CurrentControlSet\Control\MPRServices"
  
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\KernelEx"
  
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\RunServicesOnce" "KexNeedsReboot" ""
  SetRebootFlag true

SectionEnd

;--------------------------------
; Detect obsolete pre-4.0 KUP or KEX
Function DetectOldKex

  System::Call "kernel32::KUPVersion(m .r0)"
  StrCmp $0 "" 0 +4
    System::Call "kernel32::KEXVersion(m .r0)"
    StrCmp $0 "" 0 +2
      Return
  StrCpy $1 $0 1 0
  IntCmp $1 4 +3 0 +3
    MessageBox MB_ICONSTOP|MB_OK "Setup has detected previous version of KernelEx ($0) installed on this computer. Please uninstall it before continuing."
    Abort

FunctionEnd

;--------------------------------
; We can't predict future... downgrading from higher version is forbidden
Function DetectDowngrade

  System::Call "kernelex::kexGetKEXVersion() i.r0 ? u"
  StrCmp $0 "" 0 +2
    Return
  IntCmp $0 ${_VERSION_CODE} +3 +3 0
    MessageBox MB_ICONSTOP|MB_OK "Can't downgrade. Please uninstall currently installed version of KernelEx before continuing."
    Abort

FunctionEnd

;--------------------------------
;.onInit

Function .onInit

  ${If} ${IsWin98}
  ${OrIf} ${IsWinME}
  Goto lbl_Ok
  ${EndIf}
  
  MessageBox MB_ICONSTOP|MB_OK "This program requires Windows 98 or Windows Millennium."
  Abort
  
  lbl_Ok:

  ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\RunServicesOnce" "KexNeedsReboot"
  IfErrors +3
    MessageBox MB_ICONSTOP|MB_OK "Detected unfinished previous installation.$\n\
      You have to restart the system in order to complete it before you can proceed further."
    Abort

  Call DetectOldKex
  Call DetectDowngrade

FunctionEnd

Function un.onInit

  ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\RunServicesOnce" "KexNeedsReboot"
  IfErrors +3
    MessageBox MB_ICONSTOP|MB_OK "Detected unfinished previous installation.$\n\
      You have to restart the system in order to complete it before you can proceed further."
    Abort
  
FunctionEnd
