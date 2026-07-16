@ECHO OFF

NET SESSIONS > NUL 2>&1
IF %ERRORLEVEL% NEQ 0 (
	ECHO Error: This script requires administrative privileges.
	EXIT /B 1
)

SET SYSDIR="%WINDIR%\System32"
SET SYSDIR_WOW64="%WINDIR%\SysWOW64"

SET EMU_ROOT=root
SET EMU_FILESYS=%EMU_ROOT%\filesys
SET EMU_WINDIR=%EMU_FILESYS%\c\windows
SET EMU_SYSDIR=%EMU_WINDIR%\system32
SET EMU_SYSDIR_WOW64=%EMU_WINDIR%\syswow64
SET EMU_CURSORDIR=%EMU_WINDIR%\cursors
SET EMU_REGDIR=%EMU_ROOT%\registry
SET EMU_STEAMDIR=%EMU_FILESYS%\c\steam

MKDIR %EMU_SYSDIR%
MKDIR %EMU_SYSDIR_WOW64%
MKDIR %EMU_CURSORDIR%
MKDIR %EMU_REGDIR%
MKDIR %EMU_STEAMDIR%

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0create-profile-dirs.ps1" "%EMU_FILESYS%"

REM Capture this machine's registry hives and seed the Steam-bridge keys. Shared with grab-registry.bat
REM (which writes SYSTEM/SECURITY/SOFTWARE/HARDWARE/SAM + NTUSER.DAT into the given dir) to avoid duplication.
CALL "%~dp0grab-registry.bat" "%EMU_REGDIR%"

CALL :collect advapi32.dll
CALL :collect bcrypt.dll
CALL :collect bcryptprimitives.dll
CALL :collect cabinet.dll
CALL :collect cfgmgr32.dll
CALL :collect ci.dll
CALL :collect clbcatq.dll
CALL :collect coloradapterclient.dll
CALL :collect combase.dll
CALL :collect comctl32.dll
CALL :collect comdlg32.dll
CALL :collect coremessaging.dll
CALL :collect crypt32.dll
CALL :collect cryptbase.dll
CALL :collect cryptsp.dll
CALL :collect d3d10.dll
CALL :collect d3d10core.dll
CALL :collect d3d10warp.dll
CALL :collect d3d11.dll
CALL :collect d3d12.dll
CALL :collect d3d9.dll
CALL :collect d3dcompiler_43.dll
CALL :collect d3dcompiler_47.dll
CALL :collect dbghelp.dll
CALL :collect dbgcore.dll
CALL :collect dciman32.dll
CALL :collect ddraw.dll
CALL :collect devobj.dll
CALL :collect diagnosticdatasettings.dll
CALL :collect dinput8.dll
CALL :collect dnsapi.dll
CALL :collect dsound.dll
CALL :collect dwmapi.dll
CALL :collect dxcore.dll
CALL :collect dxgi.dll
CALL :collect dxva2.dll
CALL :collect fwpuclnt.dll
CALL :collect gdi32.dll
CALL :collect gdi32full.dll
CALL :collect gdiplus.dll
CALL :collect glu32.dll
CALL :collect hal.dll
CALL :collect hid.dll
CALL :collect imm32.dll
CALL :collect imagehlp.dll
CALL :collect inputhost.dll
CALL :collect iphlpapi.dll
CALL :collect kdcom.dll
CALL :collect kernel.appcore.dll
CALL :collect kernel32.dll
CALL :collect kernelbase.dll
CALL :collect ktmw32.dll
CALL :collect mfplat.dll
CALL :collect mfreadwrite.dll
CALL :collect mmdevapi.dll
CALL :collect mobilenetworking.dll
CALL :collect mpr.dll
CALL :collect msacm32.dll
CALL :collect msasn1.dll
CALL :collect mscms.dll
CALL :collect mscoree.dll
CALL :collect msdmo.dll
CALL :collect msvcp140.dll
CALL :collect msvcp140_atomic_wait.dll
CALL :collect msvcp140d.dll
CALL :collect msvcp60.dll
CALL :collect msvcp_win.dll
CALL :collect msvcr120_clr0400.dll
CALL :collect msvcrt.dll
CALL :collect mswsock.dll
CALL :collect napinsp.dll
CALL :collect ncrypt.dll
CALL :collect netapi32.dll
CALL :collect netmsg.dll
CALL :collect netutils.dll
CALL :collect nlsbres.dll
CALL :collect normaliz.dll
CALL :collect nsi.dll
CALL :collect ntasn1.dll
CALL :collect ntdll.dll
CALL :collect ole32.dll
CALL :collect oleaut32.dll
CALL :collect opengl32.dll
CALL :collect pdh.dll
CALL :collect powrprof.dll
CALL :collect profapi.dll
CALL :collect propsys.dll
CALL :collect psapi.dll
CALL :collect rasadhlp.dll
CALL :collect resampledmo.dll
CALL :collect rpcrt4.dll
CALL :collect rstrtmgr.dll
CALL :collect rsaenh.dll
CALL :collect sechost.dll
CALL :collect setupapi.dll
CALL :collect shcore.dll
CALL :collect shell32.dll
CALL :collect shlwapi.dll
CALL :collect slwga.dll
CALL :collect sppc.dll
CALL :collect srvcli.dll
CALL :collect sspicli.dll
CALL :collect ucrtbase.dll
CALL :collect ucrtbased.dll
CALL :collect uiautomationcore.dll
CALL :collect umpdc.dll
CALL :collect urlmon.dll
CALL :collect user32.dll
CALL :collect userenv.dll
CALL :collect usp10.dll
CALL :collect uxtheme.dll
CALL :collect vcruntime140.dll
CALL :collect vcruntime140_1.dll
CALL :collect vcruntime140_1d.dll
CALL :collect vcruntime140d.dll
CALL :collect version.dll
CALL :collect wer.dll
CALL :collect win32u.dll
CALL :collect windows.internal.graphics.display.displaycolormanagement.dll
CALL :collect windows.storage.dll
CALL :collect windowscodecs.dll
CALL :collect winhttp.dll
CALL :collect wininet.dll
CALL :collect winmm.dll
CALL :collect winmmbase.dll
CALL :collect winnlsres.dll
CALL :collect wintrust.dll
CALL :collect wintypes.dll
CALL :collect wlanapi.dll
CALL :collect wldap32.dll
CALL :collect wow64.dll
CALL :collect wow64base.dll
CALL :collect wow64con.dll
CALL :collect wow64cpu.dll
CALL :collect wow64win.dll
CALL :collect ws2_32.dll
CALL :collect wshbth.dll
CALL :collect wsock32.dll
CALL :collect wtsapi32.dll
CALL :collect x3daudio1_7.dll
CALL :collect xapofx1_5.dll
CALL :collect xinput1_3.dll
CALL :collect xinput1_4.dll
CALL :collect xinput9_1_0.dll

CALL :collect locale.nls
CALL :collect c_1252.nls
CALL :collect c_437.nls
CALL :collect c_850.nls

CALL :collect wdmaud.drv

CALL :collect_file "%WINDIR%\Cursors", aero_arrow.cur, %EMU_CURSORDIR%

EXIT /B 0

:normpath
SET %1=%~dpfn2
EXIT /B

:collect_file
CALL :normpath SRC, %~1\%~2
CALL :normpath DST, %~3\%~2

IF EXIST %SRC% (
	ECHO %SRC% -^> %DST%
	COPY /B /Y "%SRC%" "%DST%" >NUL
)
EXIT /B

:collect
CALL :collect_file %SYSDIR%, %~1, %EMU_SYSDIR%
CALL :collect_file %SYSDIR_WOW64%, %~1, %EMU_SYSDIR_WOW64%
EXIT /B

