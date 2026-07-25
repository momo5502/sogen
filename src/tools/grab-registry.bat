@echo off
REM Captures THIS machine's registry hives into a directory usable as a Sogen guest registry
REM (analyzer -r <dir>), then seeds the Steam-bridge keys into the user hive. Grabbing on the target
REM machine -- rather than reusing a dump from another box -- gives the guest this machine's real network
REM configuration (default gateway, interfaces), which games rely on for NAT traversal (UPnP / NAT-PMP).

NET SESSIONS > NUL 2>&1
IF %ERRORLEVEL% NEQ 0 (
	ECHO Error: This script requires administrative privileges.
	EXIT /B 1
)

REM --- Target registry dir: first argument, or "registry" by default (create-root.bat passes root\registry).
SET REGDIR=%~1
IF "%REGDIR%"=="" SET REGDIR=registry

REM --- Steam bridge seed. STEAM_PID MUST equal STEAM_FAKE_PROCESS_ID in src/windows-emulator/handles.hpp
REM --- (the guest opens this pid to confirm "Steam is running"). ACTIVE_USER is the Steam account id.
REM --- The steamclient shim DLLs live in C:\steam\ inside the guest (placed there by install-steam-shim.ps1).
SET STEAM_PID=0x8B0
SET STEAM_ACTIVE_USER=100691295
SET STEAM_DIR=C:\steam
SET STEAM_CLIENT_DLL=%STEAM_DIR%\steamclient.dll
SET STEAM_CLIENT_DLL64=%STEAM_DIR%\steamclient64.dll
SET STEAM_PATH=%STEAM_DIR%

IF NOT EXIST "%REGDIR%" MKDIR "%REGDIR%"

REG SAVE HKLM\SYSTEM "%REGDIR%\SYSTEM" /Y
REG SAVE HKLM\SECURITY "%REGDIR%\SECURITY" /Y
REG SAVE HKLM\SOFTWARE "%REGDIR%\SOFTWARE" /Y
REG SAVE HKLM\HARDWARE "%REGDIR%\HARDWARE" /Y
REG SAVE HKLM\SAM "%REGDIR%\SAM" /Y
COPY /B /Y C:\Users\Default\NTUSER.DAT "%REGDIR%\NTUSER.DAT"

REM --- Seed the guest's HKCU (\Software\Valve\Steam) so its steam_api finds our steamclient shim and the
REM --- synthetic Steam process. Load the copied hive, add the values, unload. A stale mount from a failed
REM --- previous run is unloaded first (error suppressed).
REG UNLOAD HKLM\SogenSeed >NUL 2>&1
REG LOAD HKLM\SogenSeed "%REGDIR%\NTUSER.DAT"
REG ADD HKLM\SogenSeed\Software\Valve\Steam\ActiveProcess /v SteamClientDll64 /t REG_SZ    /d "%STEAM_CLIENT_DLL64%" /f
REG ADD HKLM\SogenSeed\Software\Valve\Steam\ActiveProcess /v SteamClientDll   /t REG_SZ    /d "%STEAM_CLIENT_DLL%"   /f
REG ADD HKLM\SogenSeed\Software\Valve\Steam\ActiveProcess /v Universe         /t REG_DWORD /d 1                       /f
REG ADD HKLM\SogenSeed\Software\Valve\Steam\ActiveProcess /v pid              /t REG_DWORD /d %STEAM_PID%             /f
REG ADD HKLM\SogenSeed\Software\Valve\Steam\ActiveProcess /v ActiveUser       /t REG_DWORD /d %STEAM_ACTIVE_USER%    /f
REG ADD HKLM\SogenSeed\Software\Valve\Steam               /v SteamPath        /t REG_SZ    /d "%STEAM_PATH%"         /f
REG ADD HKLM\SogenSeed\Software\Valve\Steam               /v SteamExe         /t REG_SZ    /d "%STEAM_PATH%\steam.exe" /f
REG UNLOAD HKLM\SogenSeed

REM --- Audio endpoint seed. On a headless/RDP host the redirected playback/recording device lives under
REM --- MMDevices\Audio\RemoteRender / RemoteCapture, but mmdevapi (WASAPI) resolves the default endpoint by
REM --- opening Render\{id} / Capture\{id}, and the legacy DirectSound path ENUMERATES the subkeys of Render.
REM --- Copy the remote endpoints into the local folders (real, enumerable keys) and mark them active + local
REM --- (DeviceState=ACTIVE, Protocol=0) so they enumerate as the default device.
REG UNLOAD HKLM\SogenSwSeed >NUL 2>&1
REG LOAD HKLM\SogenSwSeed "%REGDIR%\SOFTWARE"
SET MMDEV=HKLM\SogenSwSeed\Microsoft\Windows\CurrentVersion\MMDevices\Audio
CALL :seed_audio Render RemoteRender
CALL :seed_audio Capture RemoteCapture
REG UNLOAD HKLM\SogenSwSeed

ECHO Done. Guest registry written to "%REGDIR%". Use it with: analyzer -r "%REGDIR%"
EXIT /B 0

REM --- %1 = local folder (Render/Capture), %2 = remote folder (RemoteRender/RemoteCapture). No-op if the
REM --- host has no remote endpoints (i.e. this isn't an RDP capture).
:seed_audio
REG QUERY "%MMDEV%\%~2" >NUL 2>&1
IF ERRORLEVEL 1 EXIT /B 0
REG COPY "%MMDEV%\%~2" "%MMDEV%\%~1" /s /f >NUL
FOR /F "delims=" %%K IN ('REG QUERY "%MMDEV%\%~1" 2^>NUL ^| FINDSTR /I /C:"\MMDevices\Audio\%~1\"') DO (
	REG QUERY "%%K" /v DeviceState >NUL 2>&1 || REG ADD "%%K" /v DeviceState /t REG_DWORD /d 1 /f >NUL
	REG ADD "%%K" /v Protocol /t REG_DWORD /d 0 /f >NUL
)
EXIT /B 0
