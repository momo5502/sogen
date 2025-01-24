@ECHO OFF

NET SESSIONS > NUL 2>&1
IF %ERRORLEVEL% NEQ 0 (
	ECHO Error: This script requires administrative privileges.
	EXIT /B 1
)

SET SYSDIR="%WINDIR%\System32"

:: Qiling rootfs directories
SET EMU_ROOT=root
SET EMU_FILESYS=%EMU_ROOT%\filesys
SET EMU_WINDIR=%EMU_FILESYS%\c\windows
SET EMU_SYSDIR=%EMU_WINDIR%\system32
SET EMU_REGDIR=%EMU_ROOT%\registry

MKDIR %EMU_SYSDIR%
MKDIR %EMU_REGDIR%

REG SAVE HKLM\SYSTEM %EMU_REGDIR%\SYSTEM /Y
REG SAVE HKLM\SECURITY %EMU_REGDIR%\SECURITY /Y
REG SAVE HKLM\SOFTWARE %EMU_REGDIR%\SOFTWARE /Y
REG SAVE HKLM\HARDWARE %EMU_REGDIR%\HARDWARE /Y
REG SAVE HKLM\SAM %EMU_REGDIR%\SAM /Y
COPY /B /Y C:\Users\Default\NTUSER.DAT "%EMU_REGDIR%\NTUSER.DAT"

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
CALL :collect_dll kernelbase.dll
CALL :collect_dll mpr.dll
CALL :collect_dll mscoree.dll
CALL :collect_dll msvcp_win.dll
CALL :collect_dll msvcp60.dll
CALL :collect_dll msvcr120_clr0400.dll
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
CALL :collect_dll msvcp140.dll
CALL :collect_dll msvcp140d.dll
CALL :collect_dll d3d11.dll
CALL :collect_dll d3d9.dll
CALL :collect_dll d3d12.dll
CALL :collect_dll d3dcompiler_47.dll
CALL :collect_dll dxgi.dll
CALL :collect_dll dsound.dll
CALL :collect_dll dwmapi.dll
CALL :collect_dll hid.dll
CALL :collect_dll imm32.dll
CALL :collect_dll uiautomationcore.dll
CALL :collect_dll opengl32.dll
CALL :collect_dll normaliz.dll
CALL :collect_dll wintrust.dll
CALL :collect_dll wldap32.dll
CALL :collect_dll wtsapi32.dll
CALL :collect_dll x3daudio1_7.dll
CALL :collect_dll xapofx1_5.dll
CALL :collect_dll xinput1_3.dll
CALL :collect_dll xinput9_1_0.dll
CALL :collect_dll cryptsp.dll
CALL :collect_dll resampledmo.dll
CALL :collect_dll powrprof.dll
CALL :collect_dll winmmbase.dll
CALL :collect_dll gdi32full.dll
CALL :collect_dll glu32.dll
CALL :collect_dll msdmo.dll
CALL :collect_dll dxcore.dll
CALL :collect_dll mfplat.dll
CALL :collect_dll wer.dll
CALL :collect_dll dbghelp.dll
CALL :collect_dll mscms.dll
CALL :collect_dll ktmw32.dll
CALL :collect_dll shcore.dll
CALL :collect_dll diagnosticdatasettings.dll

CALL :collect_dll locale.nls

:: All done!
EXIT /B 0

:: Functions definitions
:normpath
SET %1=%~dpfn2
EXIT /B

:collect
CALL :normpath SRC, %~1\%~2
CALL :normpath DST, %~3\%~2

IF EXIST %SRC% (
	ECHO %SRC% -^> %DST%
	COPY /B /Y "%SRC%" "%DST%" >NUL
)
EXIT /B

:collect_dll
CALL :collect %SYSDIR%, %~1, %EMU_SYSDIR%
EXIT /B
