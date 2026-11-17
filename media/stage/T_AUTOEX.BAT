@echo off
rem KernelExEx: runs Windows 98 Setup from C:\WIN98, once (Windows is there after its first restart)
if exist C:\WINDOWS\WIN.COM goto end
C:\WIN98\SETUP.EXE C:\KEXINST\MSBATCH.INF /IS /IE
:end
