# Provisions the guest GPU runtime into an emulation root: drops the latest DXVK
# D3D->Vulkan translation DLLs over the bundled DirectX ones and installs the GPU-bridge
# Vulkan shim as vulkan-1.dll. 64-bit files go to system32, 32-bit files to syswow64.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$WindowsDir, # <root>/filesys/c/windows
    [Parameter(Mandatory)][string]$Shim64,     # 64-bit vulkan-shim.dll
    [Parameter(Mandatory)][string]$Shim32      # 32-bit vulkan-shim.dll
)

$ErrorActionPreference = 'Stop'

$system32 = Join-Path $WindowsDir 'system32'
$syswow64 = Join-Path $WindowsDir 'syswow64'

$headers = @{ 'User-Agent' = 'sogen-ci' }
if ($env:GITHUB_TOKEN) { $headers['Authorization'] = "Bearer $env:GITHUB_TOKEN" }

# Resolve the latest DXVK release tarball (skip the *-native Linux build).
$release = Invoke-RestMethod -Uri 'https://api.github.com/repos/doitsujin/dxvk/releases/latest' -Headers $headers
$asset = $release.assets |
    Where-Object { $_.name -like 'dxvk-*.tar.gz' -and $_.name -notlike '*native*' } |
    Select-Object -First 1
if (-not $asset) { throw "No DXVK release tarball found in $($release.tag_name)" }

Write-Host "Downloading DXVK $($release.tag_name): $($asset.name)"
$work = Join-Path ([System.IO.Path]::GetTempPath()) ('dxvk-' + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $work -Force | Out-Null
$tarball = Join-Path $work $asset.name
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $tarball -Headers $headers

tar -xzf $tarball -C $work
if ($LASTEXITCODE -ne 0) { throw "Failed to extract $tarball" }

$dxvkRoot = Get-ChildItem -Path $work -Directory | Where-Object { $_.Name -like 'dxvk-*' } | Select-Object -First 1
if (-not $dxvkRoot) { throw 'Unexpected DXVK archive layout' }

# DXVK ships its D3D DLLs under x64/ (64-bit) and x32/ (32-bit); overwrite the bundled DirectX ones.
function Install-Dxvk([string]$srcDir, [string]$dstDir) {
    Get-ChildItem -Path $srcDir -Filter '*.dll' | ForEach-Object {
        $dst = Join-Path $dstDir $_.Name
        Write-Host "DXVK: $($_.Name) -> $dst"
        Copy-Item -Path $_.FullName -Destination $dst -Force
    }
}
Install-Dxvk (Join-Path $dxvkRoot.FullName 'x64') $system32
Install-Dxvk (Join-Path $dxvkRoot.FullName 'x32') $syswow64

# Install the GPU-bridge Vulkan shim as the system vulkan-1.dll DXVK loads.
Copy-Item -Path $Shim64 -Destination (Join-Path $system32 'vulkan-1.dll') -Force
Copy-Item -Path $Shim32 -Destination (Join-Path $syswow64 'vulkan-1.dll') -Force
Write-Host 'Vulkan shim installed as vulkan-1.dll (system32 + syswow64)'
