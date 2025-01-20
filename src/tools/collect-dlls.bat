@ECHO OFF

:: Host system directories
SET SYSDIR="%WINDIR%\System32"

:: Qiling rootfs directories
SET QL_WINDIR="Windows"
SET QL_SYSDIR="%QL_WINDIR%\System32"

MKDIR %QL_WINDIR%
MKDIR %QL_SYSDIR%

:: Collect 32-bit DLL files
CALL :collect_dll advapi32.dll
CALL :collect_dll bcrypt.dll
CALL :collect_dll cfgmgr32.dll
CALL :collect_dll ci.dll
CALL :collect_dll combase.dll
CALL :collect_dll comctl32.dll
CALL :collect_dll comdlg32.dll
CALL :collect_dll crypt32.dll
CALL :collect_dll cryptbase.dll
CALL :collect_dll gdi32.dll
CALL :collect_dll hal.dll
CALL :collect_dll iphlpapi.dll
CALL :collect_dll kdcom.dll
CALL :collect_dll kernel32.dll
CALL :collect_dll KernelBase.dll
CALL :collect_dll mpr.dll
CALL :collect_dll mscoree.dll
CALL :collect_dll msvcp_win.dll
CALL :collect_dll msvcp60.dll
CALL :collect_dll msvcr120_clr0400.dll, msvcr110.dll
CALL :collect_dll msvcrt.dll
CALL :collect_dll netapi32.dll
CALL :collect_dll ntdll.dll
CALL :collect_dll ole32.dll
CALL :collect_dll oleaut32.dll
CALL :collect_dll psapi.dll
CALL :collect_dll rpcrt4.dll
CALL :collect_dll sechost.dll
CALL :collect_dll setupapi.dll
CALL :collect_dll shell32.dll
CALL :collect_dll shlwapi.dll
CALL :collect_dll sspicli.dll
CALL :collect_dll ucrtbase.dll
CALL :collect_dll ucrtbased.dll
CALL :collect_dll urlmon.dll
CALL :collect_dll user32.dll
CALL :collect_dll userenv.dll
CALL :collect_dll uxtheme.dll
CALL :collect_dll vcruntime140.dll
CALL :collect_dll vcruntime140d.dll
CALL :collect_dll vcruntime140_1.dll
CALL :collect_dll vcruntime140_1d.dll
CALL :collect_dll version.dll
CALL :collect_dll win32u.dll
CALL :collect_dll winhttp.dll
CALL :collect_dll wininet.dll
CALL :collect_dll winmm.dll
CALL :collect_dll ws2_32.dll
CALL :collect_dll wsock32.dll

:: Collect extras
CALL :collect %SYSDIR64%, ntoskrnl.exe, %QL_SYSDIR32%

:: All done!
EXIT /B 0

:: Functions definitions
:normpath
SET %1=%~dpfn2
EXIT /B

:collect
CALL :normpath SRC, %~1\%~2
CALL :normpath DST, %~3\%~4

IF EXIST %SRC% (
	ECHO %SRC% -^> %DST%
	COPY /B /Y "%SRC%" "%DST%" >NUL
)
EXIT /B

:collect_dll
CALL :collect %SYSDIR%, %~1, %QL_SYSDIR%, %~2
EXIT /B
