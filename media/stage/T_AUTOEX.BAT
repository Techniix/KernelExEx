@echo off
rem KernelExEx: runs Windows 98 Setup from C:\WIN98, once (Windows is there after its first restart)
if exist C:\WINDOWS\WIN.COM goto end
rem /p i: no Windows ACPI driver (it cannot cope with the ACPI tables of PCs from
rem about 2001 on and can crash at Setup's first restart); KEXACPI reads batteries
C:\WIN98\SETUP.EXE C:\KEXINST\MSBATCH.INF /IS /IE /p i
:end
