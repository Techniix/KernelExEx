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
  !include "drivers\chipset\kexchip_ids.nsh"

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
  LangString DESC_USING_KEXUNI ${LANG_ENGLISH} "Microsoft Layer for Unicode not found: using KernelExEx's own (kexuni.dll)"
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

  IfFileExists $SYSDIR\unicows.dll 0 lbl_download
  
  ;check version
  GetDllVersion $SYSDIR\unicows.dll $R0 $R1
  IntOp $R2 $R0 / 0x00010000
  IntOp $R3 $R0 & 0x0000FFFF
  IntOp $R4 $R1 / 0x00010000
  IntOp $R5 $R1 & 0x0000FFFF
  StrCmp "1.1.3790.0" "$R2.$R3.$R4.$R5" lbl_Skip
  
  lbl_download:
  ;Microsoft no longer offers MSLU for download. KernelExEx brings its own
  ;implementation (kexuni.dll), so MSLU is optional: install it only when
  ;unicows.exe (the Microsoft redistributable) is next to this installer.
  IfFileExists "$EXEDIR\unicows.exe" 0 lbl_kexuni
  DetailPrint "Using $EXEDIR\unicows.exe"
  CopyFiles /SILENT "$EXEDIR\unicows.exe" "$TEMP\unicows.exe"
  Goto lbl_continue
  lbl_kexuni:
  DetailPrint "$(DESC_USING_KEXUNI)"
  Goto lbl_Skip

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
  
  ;KernelExEx's Unicode layer, used when MSLU (unicows.dll) is absent
  GetTempFileName $0 "$INSTDIR"
  File /oname=$0 "net-out\kexuni.dll"
  StrCpy $R4 "$INSTDIR\kexuni.dll"
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

Section /o "Intel chipset support (ICH2 to ICH10, 845 to P45)" SECKEXCHIP
  ;INFs only: they name the chipset's devices and load Windows' own drivers
  SetOutPath "$INSTDIR\drivers\chipset"
  File "drivers\chipset\kexchip.inf"
  File "drivers\chipset\kexide.inf"
  File "drivers\chipset\kexuhci.inf"
  SetOutPath "$INSTDIR"
  ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
  StrCmp $0 "" 0 +2
    StrCpy $0 "$WINDIR\INF"
  ${WordFind} "$0" "$INSTDIR\drivers\chipset" "E+1{" $1
  IfErrors 0 kexchip_listed
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" "$0;$INSTDIR\drivers\chipset"
  kexchip_listed:
  Call KexChipRedetect
  WriteRegStr HKLM "Software\KernelEx\Components" "KexChip" "1"
SectionEnd

;Devices of ours that Windows found no driver for (Other devices) are
;forgotten, so Windows finds them again at the next start, with our INFs.
;Devices that work are left alone. (A device whose wizard was cancelled is
;not kept at all: Windows asks again at every start and finds our INF.)
Function KexChipRedetect
  Push $R0
  Push $R1
  Push $R2
  Push $R3
  Push $R5
  Push $R6
  Push $R7
  StrCpy $R0 0
  kcr_loop:
    EnumRegKey $R1 HKLM "Enum\PCI" $R0
    StrCmp $R1 "" kcr_done
    IntOp $R0 $R0 + 1
    StrCpy $R2 $R1 13
    StrCmp $R2 "VEN_8086&DEV_" 0 kcr_loop
    StrCpy $R3 $R1 4 13
    ClearErrors
    ${WordFind} "${KEXCHIP_IDS0}" ";$R3;" "E+1{" $R2
    IfErrors 0 kcr_ours
    ClearErrors
    ${WordFind} "${KEXCHIP_IDS1}" ";$R3;" "E+1{" $R2
    IfErrors kcr_loop kcr_ours
  kcr_ours:
    StrCpy $R6 0
    kcr_inst:
      EnumRegKey $R5 HKLM "Enum\PCI\$R1" $R6
      StrCmp $R5 "" kcr_loop
      ReadRegStr $R7 HKLM "Enum\PCI\$R1\$R5" "Class"
      ;no driver: no class yet, or "Unknown" (Other devices, "do not install")
      StrCmp $R7 "" +2
      StrCmp $R7 "Unknown" 0 kcr_keep
        DetailPrint "Will detect again: PCI\$R1"
        DeleteRegKey HKLM "Enum\PCI\$R1\$R5"
        Goto kcr_inst
      kcr_keep:
      IntOp $R6 $R6 + 1
      Goto kcr_inst
  kcr_done:
  Pop $R7
  Pop $R6
  Pop $R5
  Pop $R3
  Pop $R2
  Pop $R1
  Pop $R0
FunctionEnd

Section /o "AC'97 audio (Intel ICH, nVidia nForce, AMD; any AC'97 codec)" SECKEXAC97
  ;a WDM audio driver: Windows finds it in this folder when it finds the audio device
  SetOutPath "$INSTDIR\drivers\audio"
  File "drivers\audio\kexac97.sys"
  File "drivers\audio\kexac97.inf"
  SetOutPath "$INSTDIR"
  ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
  StrCmp $0 "" 0 +2
    StrCpy $0 "$WINDIR\INF"
  ${WordFind} "$0" "$INSTDIR\drivers\audio" "E+1{" $1
  IfErrors 0 kexac97_listed
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" "$0;$INSTDIR\drivers\audio"
  kexac97_listed:
  WriteRegStr HKLM "Software\KernelEx\Components" "KexAc97" "1"
SectionEnd

Section /o "USB 2.0 (EHCI: Intel, VIA, NEC, nVidia, AMD, SiS, ALi and others)" SECKEXEHCI
  ;a WDM bus driver: Windows finds it in this folder when it finds the USB 2.0 controller
  SetOutPath "$INSTDIR\drivers\usb2"
  File "drivers\usb2\kexehci.sys"
  File "drivers\usb2\kexehci.inf"
  SetOutPath "$INSTDIR"
  ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
  StrCmp $0 "" 0 +2
    StrCpy $0 "$WINDIR\INF"
  ${WordFind} "$0" "$INSTDIR\drivers\usb2" "E+1{" $1
  IfErrors 0 kexehci_listed
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" "$0;$INSTDIR\drivers\usb2"
  kexehci_listed:
  Call KexEhciRedetect
  WriteRegStr HKLM "Software\KernelEx\Components" "KexEhci" "1"
SectionEnd

;USB 2.0 controllers Windows found no driver for are forgotten, so Windows
;finds them again at the next start, with our INF (as KexChipRedetect does).
Function KexEhciRedetect
  Push $R0
  Push $R1
  Push $R2
  Push $R5
  Push $R6
  Push $R7
  StrCpy $R0 0
  ker_loop:
    EnumRegKey $R1 HKLM "Enum\PCI" $R0
    StrCmp $R1 "" ker_done
    IntOp $R0 $R0 + 1
    StrCpy $R6 0
    ker_inst:
      EnumRegKey $R5 HKLM "Enum\PCI\$R1" $R6
      StrCmp $R5 "" ker_loop
      ReadRegStr $R2 HKLM "Enum\PCI\$R1\$R5" "CompatibleIDs"
      ClearErrors
      ${WordFind} "$R2" "CC_0C0320" "E+1{" $R7
      IfErrors ker_keep
      ReadRegStr $R7 HKLM "Enum\PCI\$R1\$R5" "Class"
      StrCmp $R7 "" +2
      StrCmp $R7 "Unknown" 0 ker_keep
        DetailPrint "Will detect again: PCI\$R1"
        DeleteRegKey HKLM "Enum\PCI\$R1\$R5"
        Goto ker_inst
      ker_keep:
      IntOp $R6 $R6 + 1
      Goto ker_inst
  ker_done:
  Pop $R7
  Pop $R6
  Pop $R5
  Pop $R2
  Pop $R1
  Pop $R0
FunctionEnd

SectionGroup "Network adapter drivers" SECNETDRV

Section /o "Intel PRO/1000 (OpenBSD em driver)" SECKEXEM
  ;kexem.sys, kexbsd.sys and the INF stay together in one folder added to
  ;DevicePath, so Windows offers the driver when it finds an Intel adapter.
  SetOutPath "$INSTDIR\drivers\kexem"
  File "drivers\kexnet\kexem.sys"
  File "drivers\kexnet\kexbsd.sys"
  File "drivers\kexnet\kexem.inf"
  SetOutPath "$INSTDIR"
  ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
  StrCmp $0 "" 0 +2
    StrCpy $0 "$WINDIR\INF"
  ${WordFind} "$0" "$INSTDIR\drivers\kexem" "E+1{" $1
  IfErrors 0 kexem_listed
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" "$0;$INSTDIR\drivers\kexem"
  kexem_listed:
  WriteRegStr HKLM "Software\KernelEx\Components" "KexEm" "1"
SectionEnd

Section /o "Realtek RTL8139/810x Fast Ethernet (onboard RTL8100C and others)" SECKEXRL
  ;kexrl.sys, kexbsd.sys and the INF stay together in one folder added to
  ;DevicePath, so Windows offers the driver when it finds a Realtek RTL8139 family adapter.
  SetOutPath "$INSTDIR\drivers\kexrl"
  File "drivers\kexnet\kexrl.sys"
  File "drivers\kexnet\kexbsd.sys"
  File "drivers\kexnet\kexrl.inf"
  SetOutPath "$INSTDIR"
  ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
  StrCmp $0 "" 0 +2
    StrCpy $0 "$WINDIR\INF"
  ${WordFind} "$0" "$INSTDIR\drivers\kexrl" "E+1{" $1
  IfErrors 0 kexrl_listed
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" "$0;$INSTDIR\drivers\kexrl"
  kexrl_listed:
  WriteRegStr HKLM "Software\KernelEx\Components" "KexRl" "1"
SectionEnd

Section /o "USB RNDIS adapters and phone tethering (OpenBSD urndis)" SECKEXRNDIS
  SetOutPath "$INSTDIR\drivers\kexrndis"
  File "drivers\kexnet\kexrndis.sys"
  File "drivers\kexnet\kexbsd.sys"
  File "drivers\kexnet\kexrndis.inf"
  SetOutPath "$INSTDIR"
  ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
  StrCmp $0 "" 0 +2
    StrCpy $0 "$WINDIR\INF"
  ${WordFind} "$0" "$INSTDIR\drivers\kexrndis" "E+1{" $1
  IfErrors 0 kexrndis_listed
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" "$0;$INSTDIR\drivers\kexrndis"
  kexrndis_listed:
  WriteRegStr HKLM "Software\KernelEx\Components" "KexRndis" "1"
SectionEnd

Section /o "USB CDC Ethernet adapters (KernelExEx driver)" SECKEXCDCE
  SetOutPath "$INSTDIR\drivers\kexcdce"
  File "drivers\kexnet\kexcdce.sys"
  File "drivers\kexnet\kexbsd.sys"
  File "drivers\kexnet\kexcdce.inf"
  SetOutPath "$INSTDIR"
  ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
  StrCmp $0 "" 0 +2
    StrCpy $0 "$WINDIR\INF"
  ${WordFind} "$0" "$INSTDIR\drivers\kexcdce" "E+1{" $1
  IfErrors 0 kexcdce_listed
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" "$0;$INSTDIR\drivers\kexcdce"
  kexcdce_listed:
  WriteRegStr HKLM "Software\KernelEx\Components" "KexCdce" "1"
SectionEnd

Section /o "Realtek USB Wi-Fi: TP-Link Archer T2U/T2U Plus, TL-WN722N v2/v3 and others (FreeBSD rtwn)" SECKEXRTWN
  SetOutPath "$INSTDIR\drivers\kexrtwn"
  File "drivers\kexnet\kexrtwn.sys"
  File "drivers\kexnet\kexbsd.sys"
  File "drivers\kexnet\kexrtwn.inf"
  ;Realtek firmware, redistributable in binary form (LICENSE.TXT)
  File "drivers\kexnet\rtwn-fw\RW88EU.FW"
  File "drivers\kexnet\rtwn-fw\RW88EE.FW"
  File "drivers\kexnet\rtwn-fw\RW88FU.FW"
  File "drivers\kexnet\rtwn-fw\RW92CE.FW"
  File "drivers\kexnet\rtwn-fw\RW92CEB.FW"
  File "drivers\kexnet\rtwn-fw\RW92CT.FW"
  File "drivers\kexnet\rtwn-fw\RW92CU.FW"
  File "drivers\kexnet\rtwn-fw\RW92EU.FW"
  File "drivers\kexnet\rtwn-fw\RW23BU.FW"
  File "drivers\kexnet\rtwn-fw\RW12AU.FW"
  File "drivers\kexnet\rtwn-fw\RW21AU.FW"
  File "drivers\kexnet\rtwn-fw\LICENSE.TXT"
  SetOutPath "$INSTDIR"
  ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
  StrCmp $0 "" 0 +2
    StrCpy $0 "$WINDIR\INF"
  ${WordFind} "$0" "$INSTDIR\drivers\kexrtwn" "E+1{" $1
  IfErrors 0 kexrtwn_listed
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" "$0;$INSTDIR\drivers\kexrtwn"
  kexrtwn_listed:
  WriteRegStr HKLM "Software\KernelEx\Components" "KexRtwn" "1"
SectionEnd

Section /o "Realtek USB Wi-Fi, older adapters FreeBSD rtwn does not list (OpenBSD urtwn)" SECKEXURTWN
  SetOutPath "$INSTDIR\drivers\kexurtwn"
  File "drivers\kexnet\kexurtwn.sys"
  File "drivers\kexnet\kexbsd.sys"
  File "drivers\kexnet\kexurtwn.inf"
  ;Realtek firmware, redistributable in binary form (LICENSE.TXT)
  File "drivers\kexnet\urtwn-fw\8188EU.FW"
  File "drivers\kexnet\urtwn-fw\8192CU.FW"
  File "drivers\kexnet\urtwn-fw\8192CT.FW"
  File "drivers\kexnet\urtwn-fw\8192EU.FW"
  File "/oname=FWLICENSE.TXT" "drivers\kexnet\urtwn-fw\LICENSE.TXT"
  SetOutPath "$INSTDIR"
  ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
  StrCmp $0 "" 0 +2
    StrCpy $0 "$WINDIR\INF"
  ${WordFind} "$0" "$INSTDIR\drivers\kexurtwn" "E+1{" $1
  IfErrors 0 kexurtwn_listed
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" "$0;$INSTDIR\drivers\kexurtwn"
  kexurtwn_listed:
  WriteRegStr HKLM "Software\KernelEx\Components" "KexUrtwn" "1"
SectionEnd

SectionGroupEnd

LangString DESC_KEXCHIP ${LANG_ENGLISH} "Names every part of Intel chipsets newer than Windows 98 (ICH2 to ICH10: SMBus, LPC, USB, IDE/SATA, bridges) so Device Manager shows no unknown devices, and sets them up with Windows' own drivers."
LangString DESC_KEXEHCI ${LANG_ENGLISH} "USB 2.0 speed for the USB 2.0 ports of any EHCI controller (the Intel ICH4 to ICH10 and later chipsets, VIA, NEC and other add-in cards). Windows 98 runs them at USB 1.1 speed only. USB 1.1 mice, keyboards and printers stay on Windows' own drivers."
LangString DESC_KEXAC97 ${LANG_ENGLISH} "Sound for onboard AC'97 audio (Intel ICH to ICH7, nVidia nForce 1 to 4, AMD-8111) with any AC'97 codec, such as the Realtek ALC655 on the MSI 848P Neo2-V. Windows 98 has no driver for these."
LangString DESC_NETDRV ${LANG_ENGLISH} "Drivers for network adapters Windows 98 has none for, from the BSD systems."
LangString DESC_KEXRNDIS ${LANG_ENGLISH} "USB network adapters that speak RNDIS, including USB tethering from an Android phone, with OpenBSD's urndis driver."
LangString DESC_KEXCDCE ${LANG_ENGLISH} "USB Ethernet adapters that follow the CDC Ethernet standard (many USB-C and Realtek RTL8152/8153 adapters)."
LangString DESC_KEXRTWN ${LANG_ENGLISH} "Wi-Fi with WPA2 for Realtek USB adapters: RTL8188CU/EU/FU, RTL8192CU/EU, RTL8723BU, RTL8812AU and RTL8821AU, including TP-Link Archer T2U, T2U Plus and T2U Nano, TL-WN722N v2/v3 and TL-WN725N. FreeBSD's rtwn driver with OpenBSD's wireless stack. Set the network name and passphrase in the adapter's Advanced properties."
LangString DESC_KEXURTWN ${LANG_ENGLISH} "Wi-Fi with WPA2 for the Realtek USB adapters FreeBSD's rtwn does not list, with OpenBSD's urtwn driver and wireless stack."
LangString DESC_KEXRL ${LANG_ENGLISH} "Realtek RTL8139, RTL8139C/C+/D, RTL8100/8100C and RTL8101L 10/100 adapters, onboard or on a card (D-Link DFE-530TX+/538TX, SMC 1211TX and many others). Our own driver: Windows 98 has none for these."
LangString DESC_KEXEM ${LANG_ENGLISH} "Intel PRO/1000 gigabit adapters (82540 to I350, ICH/PCH onboard), with OpenBSD's em driver. Windows uses it when it finds one."

LangString DESC_GAMEPAD ${LANG_ENGLISH} "Support for modern game controllers."
LangString DESC_XINPUT ${LANG_ENGLISH} "XInput (xinput1_3.dll) for games that need it. Works with any controller Windows sees; layouts follow the SDL controller database."
LangString DESC_KEXPAD ${LANG_ENGLISH} "Driver for Xbox 360 and compatible USB pads (8BitDo, PowerA, PDP, Hori and others). Windows uses it when you plug one in."

Section /o "Keep the clock right (internet time)" SECTIMESYNC
  ;kexntp.exe: SNTP from several servers, median, sanity checks
  SetOutPath "$INSTDIR"
  File "net-out\kexntp.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "KernelExEx TimeSync" '"$INSTDIR\kexntp.exe"'
  WriteRegDWORD HKLM "Software\KernelEx\TimeSync" "Enabled" 1
  WriteRegStr HKLM "Software\KernelEx\Components" "TimeSync" "1"
SectionEnd

LangString DESC_TIMESYNC ${LANG_ENGLISH} "Sets the clock from internet time servers every 6 hours. Secure websites need a correct clock. Settings: HKLM\Software\KernelEx\TimeSync."

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
  !insertmacro MUI_DESCRIPTION_TEXT ${SECTIMESYNC} $(DESC_TIMESYNC)
  !insertmacro MUI_DESCRIPTION_TEXT ${SECGAMEPAD} $(DESC_GAMEPAD)
  !insertmacro MUI_DESCRIPTION_TEXT ${SECXINPUT} $(DESC_XINPUT)
  !insertmacro MUI_DESCRIPTION_TEXT ${SECKEXPAD} $(DESC_KEXPAD)
  !insertmacro MUI_DESCRIPTION_TEXT ${SECNETDRV} $(DESC_NETDRV)
  !insertmacro MUI_DESCRIPTION_TEXT ${SECKEXCHIP} $(DESC_KEXCHIP)
  !insertmacro MUI_DESCRIPTION_TEXT ${SECKEXAC97} $(DESC_KEXAC97)
  !insertmacro MUI_DESCRIPTION_TEXT ${SECKEXEHCI} $(DESC_KEXEHCI)
  !insertmacro MUI_DESCRIPTION_TEXT ${SECKEXEM} $(DESC_KEXEM)
  !insertmacro MUI_DESCRIPTION_TEXT ${SECKEXRL} $(DESC_KEXRL)
  !insertmacro MUI_DESCRIPTION_TEXT ${SECKEXRNDIS} $(DESC_KEXRNDIS)
  !insertmacro MUI_DESCRIPTION_TEXT ${SECKEXCDCE} $(DESC_KEXCDCE)
  !insertmacro MUI_DESCRIPTION_TEXT ${SECKEXRTWN} $(DESC_KEXRTWN)
  !insertmacro MUI_DESCRIPTION_TEXT ${SECKEXURTWN} $(DESC_KEXURTWN)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

;--------------------------------
;Uninstaller Section

Section "Uninstall"

  SetDetailsView show

  ;Time sync
  ReadRegStr $0 HKLM "Software\KernelEx\Components" "TimeSync"
  StrCmp $0 "1" 0 +4
    DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "KernelExEx TimeSync"
    DeleteRegKey HKLM "Software\KernelEx\TimeSync"
    Delete /REBOOTOK "$INSTDIR\kexntp.exe"

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
  ReadRegStr $0 HKLM "Software\KernelEx\Components" "KexAc97"
  StrCmp $0 "1" 0 un_kexac97_done
    ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
    ${un.WordReplace} "$0" ";$INSTDIR\drivers\audio" "" "+" $1
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" $1
    Delete /REBOOTOK "$INSTDIR\drivers\audio\kexac97.sys"
    Delete "$INSTDIR\drivers\audio\kexac97.inf"
    RMDir "$INSTDIR\drivers\audio"
    RMDir "$INSTDIR\drivers"
  un_kexac97_done:
  ReadRegStr $0 HKLM "Software\KernelEx\Components" "KexEhci"
  StrCmp $0 "1" 0 un_kexehci_done
    ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
    ${un.WordReplace} "$0" ";$INSTDIR\drivers\usb2" "" "+" $1
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" $1
    Delete /REBOOTOK "$INSTDIR\drivers\usb2\kexehci.sys"
    Delete "$INSTDIR\drivers\usb2\kexehci.inf"
    RMDir "$INSTDIR\drivers\usb2"
    RMDir "$INSTDIR\drivers"
  un_kexehci_done:
  ReadRegStr $0 HKLM "Software\KernelEx\Components" "KexChip"
  StrCmp $0 "1" 0 un_kexchip_done
    ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
    ${un.WordReplace} "$0" ";$INSTDIR\drivers\chipset" "" "+" $1
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" $1
    Delete "$INSTDIR\drivers\chipset\kexchip.inf"
    Delete "$INSTDIR\drivers\chipset\kexide.inf"
    Delete "$INSTDIR\drivers\chipset\kexuhci.inf"
    RMDir "$INSTDIR\drivers\chipset"
    RMDir "$INSTDIR\drivers"
  un_kexchip_done:
  ReadRegStr $0 HKLM "Software\KernelEx\Components" "KexEm"
  StrCmp $0 "1" 0 un_kexem_done
    ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
    ${un.WordReplace} "$0" ";$INSTDIR\drivers\kexem" "" "+" $1
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" $1
    Delete /REBOOTOK "$INSTDIR\drivers\kexem\kexem.sys"
    Delete /REBOOTOK "$INSTDIR\drivers\kexem\kexbsd.sys"
    Delete "$INSTDIR\drivers\kexem\kexem.inf"
    RMDir "$INSTDIR\drivers\kexem"
    RMDir "$INSTDIR\drivers"
  un_kexem_done:
  ReadRegStr $0 HKLM "Software\KernelEx\Components" "KexRl"
  StrCmp $0 "1" 0 un_kexrl_done
    ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
    ${un.WordReplace} "$0" ";$INSTDIR\drivers\kexrl" "" "+" $1
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" $1
    Delete /REBOOTOK "$INSTDIR\drivers\kexrl\kexrl.sys"
    Delete /REBOOTOK "$INSTDIR\drivers\kexrl\kexbsd.sys"
    Delete "$INSTDIR\drivers\kexrl\kexrl.inf"
    RMDir "$INSTDIR\drivers\kexrl"
    RMDir "$INSTDIR\drivers"
  un_kexrl_done:
  ReadRegStr $0 HKLM "Software\KernelEx\Components" "KexRndis"
  StrCmp $0 "1" 0 un_kexrndis_done
    ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
    ${un.WordReplace} "$0" ";$INSTDIR\drivers\kexrndis" "" "+" $1
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" $1
    Delete /REBOOTOK "$INSTDIR\drivers\kexrndis\kexrndis.sys"
    Delete /REBOOTOK "$INSTDIR\drivers\kexrndis\kexbsd.sys"
    Delete "$INSTDIR\drivers\kexrndis\kexrndis.inf"
    RMDir "$INSTDIR\drivers\kexrndis"
    RMDir "$INSTDIR\drivers"
  un_kexrndis_done:
  ReadRegStr $0 HKLM "Software\KernelEx\Components" "KexCdce"
  StrCmp $0 "1" 0 un_kexcdce_done
    ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
    ${un.WordReplace} "$0" ";$INSTDIR\drivers\kexcdce" "" "+" $1
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" $1
    Delete /REBOOTOK "$INSTDIR\drivers\kexcdce\kexcdce.sys"
    Delete /REBOOTOK "$INSTDIR\drivers\kexcdce\kexbsd.sys"
    Delete "$INSTDIR\drivers\kexcdce\kexcdce.inf"
    RMDir "$INSTDIR\drivers\kexcdce"
    RMDir "$INSTDIR\drivers"
  un_kexcdce_done:
  ReadRegStr $0 HKLM "Software\KernelEx\Components" "KexRtwn"
  StrCmp $0 "1" 0 un_kexrtwn_done
    ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
    ${un.WordReplace} "$0" ";$INSTDIR\drivers\kexrtwn" "" "+" $1
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" $1
    Delete /REBOOTOK "$INSTDIR\drivers\kexrtwn\kexrtwn.sys"
    Delete /REBOOTOK "$INSTDIR\drivers\kexrtwn\kexbsd.sys"
    Delete "$INSTDIR\drivers\kexrtwn\*.FW"
    Delete "$INSTDIR\drivers\kexrtwn\LICENSE.TXT"
    Delete "$INSTDIR\drivers\kexrtwn\kexrtwn.inf"
    RMDir "$INSTDIR\drivers\kexrtwn"
    RMDir "$INSTDIR\drivers"
  un_kexrtwn_done:
  ReadRegStr $0 HKLM "Software\KernelEx\Components" "KexUrtwn"
  StrCmp $0 "1" 0 un_kexurtwn_done
    ReadRegStr $0 HKLM "${DEVPATH_KEY}" "DevicePath"
    ${un.WordReplace} "$0" ";$INSTDIR\drivers\kexurtwn" "" "+" $1
    WriteRegStr HKLM "${DEVPATH_KEY}" "DevicePath" $1
    Delete /REBOOTOK "$INSTDIR\drivers\kexurtwn\kexurtwn.sys"
    Delete /REBOOTOK "$INSTDIR\drivers\kexurtwn\kexbsd.sys"
    Delete "$INSTDIR\drivers\kexurtwn\*.FW"
    Delete "$INSTDIR\drivers\kexurtwn\FWLICENSE.TXT"
    Delete "$INSTDIR\drivers\kexurtwn\kexurtwn.inf"
    RMDir "$INSTDIR\drivers\kexurtwn"
    RMDir "$INSTDIR\drivers"
  un_kexurtwn_done:
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
  Delete /REBOOTOK "$INSTDIR\kexuni.dll"
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
