@echo off
rem KernelExEx install, on the setup RAM disk (C:). GPLv2.
set PATH=C:\
cls
echo.
echo   KernelExEx: Windows 98 install
echo   ==============================
echo.
rem a Windows already on a disk (e.g. Setup stopped with a blue screen): show the end of
rem its start-up log, which names the driver it stopped at
set L=
for %%d in (D E F G H I J K L) do if exist %%d:\WINDOWS\WIN.COM if exist %%d:\BOOTLOG.TXT set L=%%d
if "%L%"=="" goto nolog
echo   Windows on drive %L%: the end of its last start-up log (BOOTLOG.TXT).
echo   If Windows stopped with an error, take a photo of this for a bug report.
echo.
KEXLOG %L%:\BOOTLOG.TXT
pause
if exist %L%:\WINDOWS\SYSTEM\VMM32.KEX KEXUNDO %L%: /ASK
if errorlevel 2 goto end
echo.
:nolog
rem Windows 98 already copied onto a disk: nothing more to do here
set T=
for %%d in (D E F G H I J K L) do if exist %%d:\KEXINST\READY.TXT set T=%%d
if not "%T%"=="" goto ready
rem a disk prepared on the last start: copy Windows 98 onto it
for %%d in (D E F G H I J K L) do if exist %%d:\KEXTARGT.MRK set T=%%d
if not "%T%"=="" goto copy
echo   This puts Windows 98 on a hard disk of this computer. The disk you
echo   choose is erased completely.
echo.
KEXPREP
if errorlevel 2 goto restart
echo.
echo   Nothing was changed. Take out the USB stick or disc and restart to
echo   leave, or type KEXPREP to try again.
goto end
:restart
echo.
echo   The computer restarts now and comes back here to copy Windows 98 on.
echo   Leave the USB stick or disc in.
pause
KEXPREP REBOOT
goto end
:copy
echo   Copying Windows 98 onto drive %T%: (a few minutes)...
EXTRACT /A /Y /L C:\ C:\WIN98\BASE4.CAB SYS.COM > NUL
EXTRACT /A /Y /L C:\ C:\WIN98\BASE4.CAB ATTRIB.EXE > NUL
if not exist C:\SYS.COM goto fail
SYS %T%: > NUL
if errorlevel 1 goto fail
rem no logo and no Windows start until Setup writes its own MSDOS.SYS
ATTRIB -S -H -R %T%:\MSDOS.SYS
copy C:\KEXINST\T_MSDOS.SYS %T%:\MSDOS.SYS > NUL
ATTRIB +S +H +R %T%:\MSDOS.SYS
rem this edition's console and keyboard drivers (Korean: BILING.SYS, MSHBIOS.COM)
if exist C:\KEXINST\LANG\*.* copy C:\KEXINST\LANG\*.* %T%:\ > NUL
md %T%:\WIN98
copy C:\WIN98\*.* %T%:\WIN98 > NUL
if errorlevel 1 goto fail
md %T%:\KEXINST
copy C:\KEXINST\*.* %T%:\KEXINST > NUL
copy C:\HIMEM.SYS %T%:\ > NUL
copy C:\KEXMEM.SYS %T%:\ > NUL
copy C:\KEXINST\T_CONFIG.SYS %T%:\CONFIG.SYS > NUL
copy C:\KEXINST\T_AUTOEX.BAT %T%:\AUTOEXEC.BAT > NUL
echo ready > %T%:\KEXINST\READY.TXT
del %T%:\KEXTARGT.MRK
:ready
echo.
echo   Windows 98 is ready to install from the hard disk.
echo.
echo   Take out the USB stick or disc now, then press a key: the computer
echo   restarts from the hard disk and Windows 98 Setup begins. Have your
echo   Windows 98 product key ready.
pause
KEXPREP REBOOT
goto end
:fail
echo.
echo   Copying failed. Restart to try again (the disk is not erased again).
:end
