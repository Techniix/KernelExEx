# KernelExEx API inventory

Generated from the sources by `toolchain/gen_api_inventory.py`; do not edit by hand.
The same data is in [`api.json`](api.json). See [`PORTING.md`](PORTING.md) for how to use it.

Default configuration: `DCFG1 contents=std,kexbases,kexbasen,kextls,kexrt`.

Kinds:

* **new**: added (not in Windows 98 SE)
* **replaced**: replaced or extended (Windows 98 SE has a weaker version)
* **stub**: stub: exists so programs load, fails with ERROR_CALL_NOT_IMPLEMENTED or similar
* **mslu**: forwarded to the Microsoft Layer for Unicode (unicows.dll)

| DLL | added | replaced | stub | MSLU | provided by |
|---|---|---|---|---|---|
| ADVAPI32.DLL | 12 | 61 | 35 | 14 | kexbasen, kexbases, kextls |
| BCRYPT.DLL | 10 | 0 | 0 | 0 | bcrypt.dll |
| COMCTL32.DLL | 0 | 1 | 0 | 0 | kexbases |
| COMDLG32.DLL | 2 | 4 | 0 | 7 | kexbasen, kexbases |
| CRYPTUI.DLL | 1 | 0 | 0 | 0 | kexbasen |
| GDI32.DLL | 11 | 32 | 6 | 26 | kexbasen, kexbases |
| KERNEL32.DLL | 97 | 104 | 10 | 71 | kexbasen, kexbases, kexrt |
| KEXTLS.DLL | 21 | 0 | 0 | 0 | kextls.dll |
| MSIMG32.DLL | 5 | 0 | 0 | 0 | msimg32.dll |
| MSVCRT.DLL | 1 | 0 | 0 | 0 | kextls |
| OLE32.DLL | 1 | 0 | 0 | 0 | kexbasen |
| PDH.DLL | 71 | 0 | 0 | 0 | pdh.dll |
| PSAPI.DLL | 17 | 0 | 0 | 0 | psapi.dll |
| RPCRT4.DLL | 0 | 4 | 0 | 0 | kexbasen |
| SHELL32.DLL | 10 | 6 | 2 | 11 | kexbasen, kexbases |
| SHFOLDER.DLL | 2 | 0 | 0 | 0 | kexbasen |
| USER32.DLL | 22 | 83 | 2 | 55 | kexbasen, kexbases |
| USERENV.DLL | 16 | 0 | 0 | 0 | userenv.dll |
| UXTHEME.DLL | 48 | 0 | 0 | 0 | uxtheme.dll |
| VERSION.DLL | 0 | 6 | 0 | 0 | kexbases |
| WINHTTP.DLL | 19 | 0 | 0 | 0 | winhttp.dll |
| WINMM.DLL | 0 | 2 | 0 | 0 | kexbasen |
| WINSPOOL.DRV | 2 | 1 | 4 | 24 | kexbasen |
| WS2_32.DLL | 8 | 1 | 0 | 0 | kexbasen, kexrt |
| WTSAPI32.DLL | 20 | 0 | 0 | 0 | wtsapi32.dll |

## ADVAPI32.DLL

**new** (12): `CheckTokenMembership`, `CreateRestrictedToken`, `CreateWellKnownSid`, `LsaClose`, `LsaNtStatusToWinError`, `LsaOpenPolicy`, `LsaRetrievePrivateData`, `LsaStorePrivateData`, `RegDisablePredefinedCache`, `RegOpenCurrentUser`, `SystemFunction036`, `TraceMessage`

**replaced** (61): `AccessCheck`, `AddAccessAllowedAce`, `AdjustTokenPrivileges`, `AllocateAndInitializeSid`, `CopySid`, `DeleteAce`, `DuplicateToken`, `DuplicateTokenEx`, `EqualPrefixSid`, `EqualSid`, `FreeSid`, `GetAce`, `GetFileSecurityA`, `GetFileSecurityW`, `GetLengthSid`, `GetNamedSecurityInfoA`, `GetNamedSecurityInfoW`, `GetSecurityDescriptorControl`, `GetSecurityDescriptorDacl`, `GetSecurityDescriptorGroup`, `GetSecurityDescriptorLength`, `GetSecurityDescriptorOwner`, `GetSecurityDescriptorSacl`, `GetSidIdentifierAuthority`, `GetSidLengthRequired`, `GetSidSubAuthority`, `GetSidSubAuthorityCount`, `GetTokenInformation`, `ImpersonateSelf`, `InitializeAcl`, `InitializeSecurityDescriptor`, `InitializeSid`, `IsValidSecurityDescriptor`, `IsValidSid`, `LookupAccountSidA`, `LookupAccountSidW`, `LookupPrivilegeValueA`, `LookupPrivilegeValueW`, `MakeSelfRelativeSD`, `OpenProcessToken`, `OpenThreadToken`, `PrivilegeCheck`, `RegCreateKeyExW`, `RegCreateKeyW`, `RegDeleteKeyW`, `RegEnumValueW`, `RegGetKeySecurity`, `RegOpenKeyExW`, `RegOpenKeyW`, `RegQueryValueExW`, `RegSetValueExA`, `RegSetValueExW`, `RevertToSelf`, `SetFileSecurityA`, `SetFileSecurityW`, `SetKernelObjectSecurity`, `SetSecurityDescriptorDacl`, `SetSecurityDescriptorGroup`, `SetSecurityDescriptorOwner`, `SetSecurityDescriptorSacl`, `SetThreadToken`

**stub** (35): `ConvertSecurityDescriptorToStringSecurityDescriptorA`, `ConvertSecurityDescriptorToStringSecurityDescriptorW`, `ConvertSidToStringSidA`, `ConvertSidToStringSidW`, `ConvertStringSecurityDescriptorToSecurityDescriptorA`, `ConvertStringSecurityDescriptorToSecurityDescriptorW`, `ConvertStringSidToSidA`, `ConvertStringSidToSidW`, `CreateProcessWithLogonW`, `CryptAcquireContextW`, `CryptEnumProvidersW`, `CryptEnumProviderTypesW`, `CryptGetDefaultProviderW`, `CryptSetProviderExW`, `CryptSetProviderW`, `CryptSignHashW`, `CryptVerifySignatureW`, `DecryptFileA`, `DecryptFileW`, `EncryptFileA`, `EncryptFileW`, `EnumServicesStatusExA`, `EnumServicesStatusExW`, `FileEncryptionStatusA`, `FileEncryptionStatusW`, `InitiateSystemShutdownExA`, `InitiateSystemShutdownExW`, `IsWellKnownSid`, `NotifyBootConfigStatus`, `OpenSCManagerA`, `OpenSCManagerW`, `QueryServiceStatusEx`, `QueryWindows31FilesMigration`, `RegOverridePredefKey`, `SynchronizeWindows31FilesAndWindowsNTRegistry`

**mslu** (14): `GetUserNameW`, `IsTextUnicode`, `RegConnectRegistryW`, `RegDeleteValueW`, `RegEnumKeyExW`, `RegEnumKeyW`, `RegLoadKeyW`, `RegQueryInfoKeyW`, `RegQueryMultipleValuesW`, `RegQueryValueW`, `RegReplaceKeyW`, `RegSaveKeyW`, `RegSetValueW`, `RegUnLoadKeyW`


## BCRYPT.DLL

net/bcrypt: CNG subset: RNG, MD5, SHA-1, SHA-2, HMAC

**new** (10): `BCryptCloseAlgorithmProvider`, `BCryptCreateHash`, `BCryptDestroyHash`, `BCryptDuplicateHash`, `BCryptFinishHash`, `BCryptGenRandom`, `BCryptGetProperty`, `BCryptHash`, `BCryptHashData`, `BCryptOpenAlgorithmProvider`


## COMCTL32.DLL

**replaced** (1): `InitCommonControlsEx`


## COMDLG32.DLL

**new** (2): `PrintDlgExA`, `PrintDlgExW`

**replaced** (4): `GetOpenFileNameA`, `GetOpenFileNameW`, `GetSaveFileNameA`, `GetSaveFileNameW`

**mslu** (7): `ChooseColorW`, `ChooseFontW`, `FindTextW`, `GetFileTitleW`, `PageSetupDlgW`, `PrintDlgW`, `ReplaceTextW`


## CRYPTUI.DLL

**new** (1): `CryptUIDlgViewContext`


## GDI32.DLL

**new** (11): `AddFontResourceExA`, `AddFontResourceExW`, `GetCharABCWidthsI`, `GetCharWidthI`, `GetFontUnicodeRanges`, `GetGlyphIndicesA`, `GetGlyphIndicesW`, `GetTextExtentExPointI`, `GetTextExtentPointI`, `RemoveFontResourceExA`, `RemoveFontResourceExW`

**replaced** (32): `AddFontResourceW`, `CreateDIBSection`, `CreateFontIndirectA`, `DeleteObject`, `EnumFontFamiliesExA`, `EnumFontFamiliesExW`, `EnumFontFamiliesW`, `EnumFontsW`, `ExtCreatePen`, `ExtTextOutA`, `ExtTextOutW`, `GetCharABCWidthsW`, `GetCharWidth32A`, `GetCharWidth32W`, `GetGlyphOutlineA`, `GetGlyphOutlineW`, `GetObjectType`, `GetObjectW`, `GetRandomRgn`, `GetTextExtentPoint32A`, `GetTextExtentPoint32W`, `GetTextMetricsA`, `GetWorldTransform`, `MaskBlt`, `ModifyWorldTransform`, `PolyTextOutA`, `PolyTextOutW`, `RemoveFontResourceW`, `SelectObject`, `SetGraphicsMode`, `SetMapMode`, `SetWorldTransform`

**stub** (6): `AddFontMemResourceEx`, `GetDCBrushColor`, `GetDCPenColor`, `RemoveFontMemResourceEx`, `SetDCBrushColor`, `SetDCPenColor`

**mslu** (26): `CopyEnhMetaFileW`, `CopyMetaFileW`, `CreateColorSpaceW`, `CreateDCW`, `CreateEnhMetaFileW`, `CreateFontIndirectW`, `CreateFontW`, `CreateICW`, `CreateMetaFileW`, `CreateScalableFontResourceW`, `EnumICMProfilesW`, `GetCharacterPlacementW`, `GetEnhMetaFileDescriptionW`, `GetEnhMetaFileW`, `GetICMProfileW`, `GetKerningPairsW`, `GetLogColorSpaceW`, `GetMetaFileW`, `GetOutlineTextMetricsW`, `GetTextExtentExPointW`, `GetTextFaceW`, `GetTextMetricsW`, `ResetDCW`, `SetICMProfileW`, `StartDocW`, `UpdateICMRegKeyW`


## KERNEL32.DLL

**new** (97): `AcquireSRWLockExclusive`, `AcquireSRWLockShared`, `AssignProcessToJobObject`, `CancelTimerQueueTimer`, `ChangeTimerQueueTimer`, `CreateEventExA`, `CreateEventExW`, `CreateFiberEx`, `CreateJobObjectA`, `CreateJobObjectW`, `CreateMutexExA`, `CreateMutexExW`, `CreateSemaphoreExA`, `CreateSemaphoreExW`, `CreateTimerQueue`, `CreateTimerQueueTimer`, `DecodePointer`, `DecodeSystemPointer`, `DelayLoadFailureHook`, `DeleteTimerQueue`, `DeleteTimerQueueEx`, `DeleteTimerQueueTimer`, `EncodePointer`, `EncodeSystemPointer`, `EnumUILanguagesA`, `EnumUILanguagesW`, `FlsAlloc`, `FlsFree`, `FlsGetValue`, `FlsSetValue`, `FlushProcessWriteBuffers`, `GetConsoleWindow`, `GetCurrentProcessorNumber`, `GetFileSizeEx`, `GetModuleHandleExA`, `GetModuleHandleExW`, `GetNativeSystemInfo`, `GetProcessId`, `GetSystemDefaultUILanguage`, `GetSystemTimePreciseAsFileTime`, `GetSystemWindowsDirectoryA`, `GetSystemWindowsDirectoryW`, `GetTickCount64`, `GetUserDefaultUILanguage`, `GetVolumePathNameA`, `GetVolumePathNameW`, `GlobalMemoryStatusEx`, `InitializeConditionVariable`, `InitializeCriticalSectionEx`, `InitializeSRWLock`, `InitOnceBeginInitialize`, `InitOnceComplete`, `InitOnceExecuteOnce`, `InitOnceInitialize`, `InterlockedCompareExchange64`, `IsProcessInJob`, `IsWow64Process`, `KEXVersion`, `Module32FirstW`, `Module32NextW`, `MoveFileWithProgressA`, `MoveFileWithProgressW`, `OpenJobObjectA`, `OpenJobObjectW`, `OpenThread`, `Process32FirstW`, `Process32NextW`, `ProcessIdToSessionId`, `QueryInformationJobObject`, `QueryUnbiasedInterruptTime`, `QueueUserWorkItem`, `RegisterWaitForSingleObject`, `RegisterWaitForSingleObjectEx`, `ReleaseSRWLockExclusive`, `ReleaseSRWLockShared`, `RestoreLastError`, `RtlCaptureContext`, `SetDllDirectoryA`, `SetDllDirectoryW`, `SetFilePointerEx`, `SetInformationJobObject`, `SetThreadUILanguage`, `SetTimerQueueTimer`, `SleepConditionVariableCS`, `SleepConditionVariableSRW`, `TerminateJobObject`, `TryAcquireSRWLockExclusive`, `TryAcquireSRWLockShared`, `TzSpecificLocalTimeToSystemTime`, `UnregisterWait`, `UnregisterWaitEx`, `VerifyVersionInfoA`, `VerifyVersionInfoW`, `VerSetConditionMask`, `WakeAllConditionVariable`, `WakeConditionVariable`, `WTSGetActiveConsoleSessionId`

**replaced** (104): `AddAtomW`, `CompareStringW`, `CopyFileExA`, `CopyFileExW`, `CopyFileW`, `CreateDirectoryExW`, `CreateDirectoryW`, `CreateFileA`, `CreateFileW`, `CreateIoCompletionPort`, `CreateThread`, `DefineDosDeviceW`, `DeleteCriticalSection`, `DeleteFileW`, `EnterCriticalSection`, `ExitProcess`, `FindAtomW`, `FindFirstChangeNotificationW`, `FindFirstFileExA`, `FindFirstFileExW`, `FindFirstFileW`, `FindNextFileW`, `FindResourceExW`, `FindResourceW`, `FoldStringA`, `FoldStringW`, `FreeEnvironmentStringsW`, `GetAtomNameW`, `GetCurrentDirectoryW`, `GetDefaultCommConfigW`, `GetDiskFreeSpaceA`, `GetDiskFreeSpaceExA`, `GetDiskFreeSpaceExW`, `GetDiskFreeSpaceW`, `GetEnvironmentStringsW`, `GetFileAttributesExW`, `GetFileAttributesW`, `GetFullPathNameW`, `GetLocaleInfoW`, `GetLongPathNameW`, `GetModuleFileNameW`, `GetModuleHandleW`, `GetQueuedCompletionStatus`, `GetShortPathNameW`, `GetStartupInfoW`, `GetStringTypeExW`, `GetStringTypeW`, `GetSystemDirectoryW`, `GetTempFileNameA`, `GetTempFileNameW`, `GetTempPathW`, `GetVersion`, `GetVersionExA`, `GetVersionExW`, `GetWindowsDirectoryW`, `GlobalAddAtomW`, `GlobalFindAtomW`, `GlobalGetAtomNameW`, `GlobalLock`, `GlobalUnlock`, `HeapAlloc`, `HeapCreate`, `HeapDestroy`, `HeapFree`, `HeapLock`, `HeapReAlloc`, `HeapSize`, `HeapUnlock`, `InitializeCriticalSectionAndSpinCount`, `IsProcessorFeaturePresent`, `LCMapStringW`, `LoadLibraryExW`, `LoadLibraryW`, `LockFileEx`, `lstrcmpiW`, `lstrcmpW`, `lstrcpynW`, `MapViewOfFile`, `MapViewOfFileEx`, `MoveFileExA`, `MoveFileExW`, `MoveFileW`, `MultiByteToWideChar`, `OutputDebugStringW`, `PostQueuedCompletionStatus`, `ReadConsoleInputW`, `ReadFile`, `RemoveDirectoryW`, `SearchPathW`, `SetConsoleTitleW`, `SetCurrentDirectoryW`, `SetFileAttributesW`, `SystemTimeToTzSpecificLocalTime`, `TlsAlloc`, `TlsFree`, `TlsGetValue`, `TlsSetValue`, `TryEnterCriticalSection`, `UnlockFileEx`, `VerLanguageNameW`, `VirtualAllocEx`, `VirtualFreeEx`, `WideCharToMultiByte`, `WriteFile`

**stub** (10): `CreateHardLinkA`, `CreateHardLinkW`, `GetComputerNameExA`, `GetComputerNameExW`, `GetProcessIoCounters`, `HeapSetInformation`, `IsValidLanguageGroup`, `ReplaceFileA`, `ReplaceFileW`, `RtlCaptureStackBackTrace`

**mslu** (71): `BuildCommDCBAndTimeoutsW`, `BuildCommDCBW`, `CallNamedPipeW`, `CommConfigDialogW`, `CreateEventW`, `CreateFileMappingW`, `CreateMailslotW`, `CreateMutexW`, `CreateProcessW`, `CreateSemaphoreW`, `CreateWaitableTimerW`, `EnumCalendarInfoExW`, `EnumCalendarInfoW`, `EnumDateFormatsExW`, `EnumDateFormatsW`, `EnumSystemCodePagesW`, `EnumSystemLocalesW`, `EnumTimeFormatsW`, `ExpandEnvironmentStringsW`, `FatalAppExitW`, `FillConsoleOutputCharacterW`, `FormatMessageW`, `GetCalendarInfoW`, `GetComputerNameW`, `GetConsoleTitleW`, `GetCPInfoExW`, `GetCurrencyFormatW`, `GetDateFormatW`, `GetDriveTypeW`, `GetEnvironmentVariableW`, `GetLogicalDriveStringsW`, `GetNamedPipeHandleStateW`, `GetNumberFormatW`, `GetPrivateProfileIntW`, `GetPrivateProfileSectionNamesW`, `GetPrivateProfileSectionW`, `GetPrivateProfileStringW`, `GetPrivateProfileStructW`, `GetProfileIntW`, `GetProfileSectionW`, `GetProfileStringW`, `GetTimeFormatW`, `GetVolumeInformationW`, `IsBadStringPtrW`, `OpenEventW`, `OpenFileMappingW`, `OpenMutexW`, `OpenSemaphoreW`, `OpenWaitableTimerW`, `PeekConsoleInputW`, `QueryDosDeviceW`, `ReadConsoleOutputCharacterW`, `ReadConsoleOutputW`, `ReadConsoleW`, `ScrollConsoleScreenBufferW`, `SetCalendarInfoW`, `SetComputerNameW`, `SetDefaultCommConfigW`, `SetEnvironmentVariableW`, `SetLocaleInfoW`, `SetVolumeLabelW`, `WaitNamedPipeW`, `WriteConsoleInputW`, `WriteConsoleOutputCharacterW`, `WriteConsoleOutputW`, `WriteConsoleW`, `WritePrivateProfileSectionW`, `WritePrivateProfileStringW`, `WritePrivateProfileStructW`, `WriteProfileSectionW`, `WriteProfileStringW`


## KEXTLS.DLL

net/kextls: direct TLS client, digest and random API (include/kextls.h)

**new** (21): `kextls_abi_version`, `kextls_client_new`, `kextls_close_notify`, `kextls_digest_block`, `kextls_digest_dup`, `kextls_digest_final`, `kextls_digest_free`, `kextls_digest_new`, `kextls_digest_size`, `kextls_digest_update`, `kextls_free`, `kextls_get_info`, `kextls_handshake`, `kextls_last_entropy_report`, `kextls_pending`, `kextls_random`, `kextls_read`, `kextls_strerror`, `kextls_trust_store_count`, `kextls_trust_store_load_pem`, `kextls_write`


## MSIMG32.DLL

auxiliary/msimg32

**new** (5): `AlphaBlend`, `DllInitialize`, `GradientFill`, `TransparentBlt`, `vSetDdrawflag`


## MSVCRT.DLL

**new** (1): `rand_s`


## OLE32.DLL

**new** (1): `CoWaitForMultipleHandles`


## PDH.DLL

auxiliary/pdh (stubs)

**new** (71): `PdhAddCounterA`, `PdhAddCounterW`, `PdhBrowseCountersA`, `PdhBrowseCountersW`, `PdhCalculateCounterFromRawValue`, `PdhCloseLog`, `PdhCloseQuery`, `PdhCollectQueryData`, `PdhCollectQueryDataEx`, `PdhComputeCounterStatistics`, `PdhConnectMachineA`, `PdhConnectMachineW`, `PdhDecodeWmiPathA`, `PdhDecodeWmiPathW`, `PdhEncodeWmiPathA`, `PdhEncodeWmiPathW`, `PdhEnumMachinesA`, `PdhEnumMachinesW`, `PdhEnumObjectItemsA`, `PdhEnumObjectItemsW`, `PdhEnumObjectsA`, `PdhEnumObjectsW`, `PdhExpandCounterPathA`, `PdhExpandCounterPathW`, `PdhFormatFromRawValue`, `PdhGetCounterInfoA`, `PdhGetCounterInfoW`, `PdhGetCounterTimeBase`, `PdhGetDataSourceTimeRangeA`, `PdhGetDataSourceTimeRangeW`, `PdhGetDefaultPerfCounterA`, `PdhGetDefaultPerfCounterW`, `PdhGetDefaultPerfObjectA`, `PdhGetDefaultPerfObjectW`, `PdhGetDllVersion`, `PdhGetFormattedCounterArrayA`, `PdhGetFormattedCounterArrayW`, `PdhGetFormattedCounterValue`, `PdhGetLogFileSize`, `PdhGetRawCounterArrayA`, `PdhGetRawCounterArrayW`, `PdhGetRawCounterValue`, `PdhIsRealTimeQuery`, `PdhLogServiceCommandA`, `PdhLogServiceCommandW`, `PdhLogServiceControlA`, `PdhLogServiceControlW`, `PdhLookupPerfIndexByNameA`, `PdhLookupPerfIndexByNameW`, `PdhLookupPerfNameByIndexA`, `PdhLookupPerfNameByIndexW`, `PdhMakeCounterPathA`, `PdhMakeCounterPathW`, `PdhOpenLogA`, `PdhOpenLogW`, `PdhOpenQueryA`, `PdhOpenQueryW`, `PdhParseCounterPathA`, `PdhParseCounterPathW`, `PdhParseInstanceNameA`, `PdhParseInstanceNameW`, `PdhReadRawLogRecord`, `PdhRemoveCounter`, `PdhSelectDataSourceA`, `PdhSelectDataSourceW`, `PdhSetCounterScaleFactor`, `PdhSetQueryTimeRange`, `PdhUpdateLogA`, `PdhUpdateLogW`, `PdhValidatePathA`, `PdhValidatePathW`


## PSAPI.DLL

auxiliary/psapi

**new** (17): `EmptyWorkingSet`, `EnumPageFilesA`, `EnumPageFilesW`, `EnumProcesses`, `EnumProcessModules`, `GetModuleBaseNameA`, `GetModuleBaseNameW`, `GetModuleFileNameExA`, `GetModuleFileNameExW`, `GetProcessFlags`, `GetProcessId`, `GetProcessImageFileNameA`, `GetProcessImageFileNameW`, `GetProcessMemoryInfo`, `InitializeProcessForWsWatch`, `QueryWorkingSet`, `QueryWorkingSetEx`


## RPCRT4.DLL

**replaced** (4): `NdrDllGetClassObject`, `RpcStringFreeW`, `UuidFromStringW`, `UuidToStringW`


## SHELL32.DLL

**new** (10): `IsUserAnAdmin`, `SHBindToParent`, `SHCreateDirectoryExA`, `SHCreateDirectoryExW`, `SHGetDiskFreeSpaceExA`, `SHGetDiskFreeSpaceExW`, `SHGetFolderLocation`, `SHGetFolderPathA`, `SHGetFolderPathW`, `SHParseDisplayName`

**replaced** (6): `CommandLineToArgvW`, `DragQueryFileW`, `SHGetDiskFreeSpaceA`, `SHGetSpecialFolderLocation`, `SHGetSpecialFolderPathA`, `SHGetSpecialFolderPathW`

**stub** (2): `SHCreateShellItem`, `SHOpenFolderAndSelectItems`

**mslu** (11): `ExtractIconExW`, `ExtractIconW`, `FindExecutableW`, `SHBrowseForFolderW`, `Shell_NotifyIconW`, `ShellAboutW`, `ShellExecuteExW`, `ShellExecuteW`, `SHFileOperationW`, `SHGetFileInfoW`, `SHGetNewLinkInfoW`


## SHFOLDER.DLL

**new** (2): `SHGetFolderPathA`, `SHGetFolderPathW`


## USER32.DLL

**new** (22): `AllowSetForegroundWindow`, `CreateWindowW`, `DefRawInputProc`, `DeregisterShellHookWindow`, `DisableProcessWindowsGhosting`, `GetAltTabInfoA`, `GetLastInputInfo`, `GetMouseMovePointsEx`, `GetRawInputBuffer`, `GetRawInputData`, `GetRawInputDeviceInfoA`, `GetRawInputDeviceInfoW`, `GetRawInputDeviceList`, `GetRegisteredRawInputDevices`, `IsHungAppWindow`, `LockSetForegroundWindow`, `LockWorkStation`, `RealGetWindowClassA`, `RealGetWindowClassW`, `RegisterRawInputDevices`, `RegisterShellHookWindow`, `UpdateLayeredWindow`

**replaced** (83): `AppendMenuW`, `CallMsgFilterW`, `CallWindowProcA`, `CallWindowProcW`, `ChangeMenuW`, `CharLowerBuffW`, `CharLowerW`, `CharNextExW`, `CharNextW`, `CharPrevExW`, `CharPrevW`, `CharToOemBuffW`, `CharToOemW`, `CharUpperBuffW`, `CharUpperW`, `CopyAcceleratorTableW`, `CreateAcceleratorTableW`, `CreateDialogIndirectParamW`, `CreateDialogParamW`, `CreateMDIWindowW`, `CreateWindowExW`, `DefDlgProcW`, `DefFrameProcW`, `DefMDIChildProcW`, `DefWindowProcW`, `DialogBoxIndirectParamW`, `DialogBoxParamW`, `DispatchMessageW`, `DrawCaptionTempW`, `EnableWindow`, `GetAncestor`, `GetClassInfoExW`, `GetClassInfoW`, `GetClassLongW`, `GetDlgItemTextW`, `GetMessageA`, `GetMessageW`, `GetNextDlgTabItem`, `GetWindowLongA`, `GetWindowLongW`, `GetWindowTextLengthW`, `GetWindowTextW`, `InsertMenuW`, `IsCharAlphaNumericW`, `IsCharAlphaW`, `IsCharLowerW`, `IsCharUpperW`, `IsDialogMessageW`, `IsWindowUnicode`, `LoadMenuA`, `LoadMenuIndirectW`, `LoadMenuW`, `LoadStringW`, `MapVirtualKeyA`, `MapVirtualKeyExA`, `ModifyMenuW`, `OemToCharBuffW`, `OemToCharW`, `PeekMessageA`, `PeekMessageW`, `PostMessageW`, `PostThreadMessageW`, `RegisterClassExW`, `RegisterClassW`, `SendDlgItemMessageW`, `SendMessageA`, `SendMessageCallbackW`, `SendMessageTimeoutW`, `SendMessageW`, `SendNotifyMessageW`, `SetClassLongW`, `SetDlgItemTextW`, `SetParent`, `SetWindowLongA`, `SetWindowLongW`, `SetWindowsHookExW`, `SetWindowsHookW`, `SetWindowTextW`, `SystemParametersInfoA`, `ToUnicode`, `ToUnicodeEx`, `TranslateAcceleratorW`, `UnregisterClassW`

**stub** (2): `PrintWindow`, `SetLayeredWindowAttributes`

**mslu** (55): `BroadcastSystemMessageW`, `ChangeDisplaySettingsExW`, `ChangeDisplaySettingsW`, `DdeCreateStringHandleW`, `DdeInitializeW`, `DdeQueryStringW`, `DlgDirListComboBoxW`, `DlgDirListW`, `DlgDirSelectComboBoxExW`, `DlgDirSelectExW`, `DrawStateW`, `DrawTextExW`, `DrawTextW`, `EnumDisplayDevicesW`, `EnumDisplaySettingsExW`, `EnumDisplaySettingsW`, `EnumPropsExW`, `EnumPropsW`, `FindWindowExW`, `FindWindowW`, `GetAltTabInfoW`, `GetClassNameW`, `GetClipboardFormatNameW`, `GetKeyboardLayoutNameW`, `GetKeyNameTextW`, `GetMenuItemInfoW`, `GetMenuStringW`, `GetMonitorInfoW`, `GetPropW`, `GetTabbedTextExtentW`, `GrayStringW`, `InsertMenuItemW`, `LoadAcceleratorsW`, `LoadBitmapW`, `LoadCursorFromFileW`, `LoadCursorW`, `LoadIconW`, `LoadImageW`, `LoadKeyboardLayoutW`, `MapVirtualKeyExW`, `MapVirtualKeyW`, `MessageBoxIndirectW`, `RegisterClipboardFormatW`, `RegisterDeviceNotificationW`, `RegisterWindowMessageW`, `RemovePropW`, `SetMenuItemInfoW`, `SetPropW`, `SystemParametersInfoW`, `TabbedTextOutW`, `VkKeyScanExW`, `VkKeyScanW`, `WinHelpW`, `wsprintfW`, `wvsprintfW`


## USERENV.DLL

auxiliary/userenv

**new** (16): `CreateEnvironmentBlock`, `DestroyEnvironmentBlock`, `ExpandEnvironmentStringsForUserA`, `ExpandEnvironmentStringsForUserW`, `GetAllUsersProfileDirectoryA`, `GetAllUsersProfileDirectoryW`, `GetProfilesDirectoryA`, `GetProfilesDirectoryW`, `GetProfileType`, `GetUserProfileDirectoryA`, `GetUserProfileDirectoryW`, `LoadUserProfileA`, `LoadUserProfileW`, `RegisterGPNotification`, `UnloadUserProfile`, `UnregisterGPNotification`


## UXTHEME.DLL

auxiliary/uxtheme (no themes; functions report themes off)

**new** (48): `CloseThemeData`, `DrawThemeBackground`, `DrawThemeBackgroundEx`, `DrawThemeEdge`, `DrawThemeIcon`, `DrawThemeParentBackground`, `DrawThemeText`, `EnableThemeDialogTexture`, `EnableTheming`, `GetCurrentThemeName`, `GetThemeAppProperties`, `GetThemeBackgroundContentRect`, `GetThemeBackgroundExtent`, `GetThemeBackgroundRegion`, `GetThemeBool`, `GetThemeColor`, `GetThemeDocumentationProperty`, `GetThemeEnumValue`, `GetThemeFilename`, `GetThemeFont`, `GetThemeInt`, `GetThemeIntList`, `GetThemeMargins`, `GetThemeMetric`, `GetThemePartSize`, `GetThemePosition`, `GetThemePropertyOrigin`, `GetThemeRect`, `GetThemeString`, `GetThemeSysBool`, `GetThemeSysColor`, `GetThemeSysColorBrush`, `GetThemeSysFont`, `GetThemeSysInt`, `GetThemeSysSize`, `GetThemeSysString`, `GetThemeTextExtent`, `GetThemeTextMetrics`, `GetWindowTheme`, `HitTestThemeBackground`, `IsAppThemed`, `IsThemeActive`, `IsThemeBackgroundPartiallyTransparent`, `IsThemeDialogTextureEnabled`, `IsThemePartDefined`, `OpenThemeData`, `SetThemeAppProperties`, `SetWindowTheme`


## VERSION.DLL

**replaced** (6): `GetFileVersionInfoSizeW`, `GetFileVersionInfoW`, `VerFindFileW`, `VerInstallFileW`, `VerLanguageNameW`, `VerQueryValueW`


## WINHTTP.DLL

net/winhttp: synchronous WinHTTP with TLS 1.2/1.3 (kextls)

**new** (19): `WinHttpAddRequestHeaders`, `WinHttpCheckPlatform`, `WinHttpCloseHandle`, `WinHttpConnect`, `WinHttpCrackUrl`, `WinHttpGetDefaultProxyConfiguration`, `WinHttpGetIEProxyConfigForCurrentUser`, `WinHttpOpen`, `WinHttpOpenRequest`, `WinHttpQueryDataAvailable`, `WinHttpQueryHeaders`, `WinHttpQueryOption`, `WinHttpReadData`, `WinHttpReceiveResponse`, `WinHttpSendRequest`, `WinHttpSetOption`, `WinHttpSetStatusCallback`, `WinHttpSetTimeouts`, `WinHttpWriteData`


## WINMM.DLL

**replaced** (2): `waveInGetDevCapsW`, `waveOutGetDevCapsW`


## WINSPOOL.DRV

**new** (2): `GetDefaultPrinterA`, `GetDefaultPrinterW`

**replaced** (1): `EnumPrintersW`

**stub** (4): `GetPrinterDataExA`, `GetPrinterDataExW`, `SetPrinterDataExA`, `SetPrinterDataExW`

**mslu** (24): `AddJobW`, `AddMonitorW`, `AddPortW`, `AddPrinterDriverW`, `AddPrinterW`, `AddPrintProcessorW`, `AddPrintProvidorW`, `AdvancedDocumentPropertiesW`, `ConfigurePortW`, `DeleteMonitorW`, `DeletePortW`, `DeletePrinterDriverW`, `DeletePrintProcessorW`, `DeletePrintProvidorW`, `DeviceCapabilitiesW`, `DocumentPropertiesW`, `GetPrinterDriverDirectoryW`, `GetPrintProcessorDirectoryW`, `OpenPrinterW`, `ResetPrinterW`, `SetJobW`, `SetPrinterDataW`, `SetPrinterW`, `StartDocPrinterW`


## WS2_32.DLL

**new** (8): `freeaddrinfo`, `getaddrinfo`, `getnameinfo`, `inet_ntop`, `inet_pton`, `InetNtopW`, `InetPtonW`, `WSAPoll`

**replaced** (1): `select`


## WTSAPI32.DLL

auxiliary/wtsapi32

**new** (20): `WTSCloseServer`, `WTSDisconnectSession`, `WTSEnumerateProcessesA`, `WTSEnumerateProcessesW`, `WTSEnumerateServersA`, `WTSEnumerateServersW`, `WTSEnumerateSessionsA`, `WTSEnumerateSessionsW`, `WTSFreeMemory`, `WTSOpenServerA`, `WTSOpenServerW`, `WTSQuerySessionInformationA`, `WTSQuerySessionInformationW`, `WTSQueryUserConfigA`, `WTSQueryUserConfigW`, `WTSQueryUserToken`, `WTSRegisterSessionNotification`, `WTSTerminateProcess`, `WTSUnRegisterSessionNotification`, `WTSWaitSystemEvent`
