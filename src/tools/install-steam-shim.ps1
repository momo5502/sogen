#!/usr/bin/env pwsh
# Places the built guest steamclient shim DLLs into the emulator root at C:\steam\ -- the location the
# seeded guest registry points the game's steam_api at (see grab-registry.bat). Mirrors the way
# install-gpu-runtime.ps1 drops the Vulkan shim into the root. Missing shims are skipped with a warning,
# so this is a harmless no-op when the Steam bridge was not built (e.g. no libclang in the environment).
param(
    [Parameter(Mandatory)][string]$SteamDir, # <root>/filesys/c/steam
    [string]$Shim64,                          # 64-bit steamclient64.dll (steam-shim OUTPUT_NAME on x64)
    [string]$Shim32                           # 32-bit steamclient.dll   (steam-shim OUTPUT_NAME on x86)
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Path $SteamDir -Force | Out-Null

function Install-Shim([string]$Src, [string]$Name) {
    if ($Src -and (Test-Path $Src)) {
        Copy-Item -Path $Src -Destination (Join-Path $SteamDir $Name) -Force
        Write-Host "Installed $Name -> $SteamDir"
    }
    else {
        Write-Warning "steamclient shim '$Name' not found ($Src); skipping (Steam bridge not built?)."
    }
}

Install-Shim $Shim64 'steamclient64.dll'
Install-Shim $Shim32 'steamclient.dll'
