# Provisions the guest GPU runtime into an emulation root: drops the latest DXVK (D3D8-11) and
# vkd3d-proton (D3D12) D3D->Vulkan translation DLLs over the bundled DirectX ones, replaces
# opengl32.dll with Mesa's, and installs the GPU-bridge Vulkan shim as vulkan-1.dll. 64-bit files
# go to system32, 32-bit files to syswow64.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$WindowsDir, # <root>/filesys/c/windows
    [Parameter(Mandatory)][string]$Shim64,     # 64-bit vulkan-shim.dll
    [Parameter(Mandatory)][string]$Shim32      # 32-bit vulkan-shim.dll
)

$ErrorActionPreference = 'Stop'

$system32 = Join-Path $WindowsDir 'system32'
$syswow64 = Join-Path $WindowsDir 'syswow64'

# Overwrites the bundled DirectX DLLs with a translation layer's.
function Install-Dlls([string]$label, [string]$srcDir, [string]$dstDir) {
    if (-not (Test-Path $srcDir)) { throw "$label source directory not found: $srcDir" }
    Get-ChildItem -Path $srcDir -Filter '*.dll' | ForEach-Object {
        $dst = Join-Path $dstDir $_.Name
        Write-Host "${label}: $($_.Name) -> $dst"
        Copy-Item -Path $_.FullName -Destination $dst -Force
    }
}

# Auth only raises the api.github.com rate limit; it is never sent to the asset CDN below.
$apiHeaders = @{ 'User-Agent' = 'sogen-ci' }
if ($env:GITHUB_TOKEN) { $apiHeaders['Authorization'] = "Bearer $env:GITHUB_TOKEN" }

# Resolve the latest DXVK release tarball (skip the *-native Linux build).
$release = Invoke-RestMethod -Uri 'https://api.github.com/repos/doitsujin/dxvk/releases/latest' -Headers $apiHeaders
$asset = $release.assets |
    Where-Object { $_.name -like 'dxvk-*.tar.gz' -and $_.name -notlike '*native*' } |
    Select-Object -First 1
if (-not $asset) { throw "No DXVK release tarball found in $($release.tag_name)" }

Write-Host "Downloading DXVK $($release.tag_name): $($asset.name)"
$work = Join-Path ([System.IO.Path]::GetTempPath()) ('dxvk-' + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $work -Force | Out-Null
try {
    $tarball = Join-Path $work $asset.name
    # browser_download_url redirects to a public CDN; don't forward the API auth header to it.
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $tarball

    tar -xzf $tarball -C $work
    if ($LASTEXITCODE -ne 0) { throw "Failed to extract $tarball" }

    $dxvkRoot = Get-ChildItem -Path $work -Directory | Where-Object { $_.Name -like 'dxvk-*' } | Select-Object -First 1
    if (-not $dxvkRoot) { throw 'Unexpected DXVK archive layout' }

    Install-Dlls 'DXVK' (Join-Path $dxvkRoot.FullName 'x64') $system32
    Install-Dlls 'DXVK' (Join-Path $dxvkRoot.FullName 'x32') $syswow64

    # vkd3d-proton runs on top of DXVK's dxgi.dll. The bundled d3d12.dll needs a real D3D12 driver, which the guest does not have.
    $vkd3dRelease = Invoke-RestMethod -Uri 'https://api.github.com/repos/HansKristian-Work/vkd3d-proton/releases/latest' -Headers $apiHeaders
    $vkd3dAsset = $vkd3dRelease.assets | Where-Object { $_.name -like 'vkd3d-proton-*.tar.zst' } | Select-Object -First 1
    if (-not $vkd3dAsset) { throw "No vkd3d-proton release archive found in $($vkd3dRelease.tag_name)" }

    Write-Host "Downloading vkd3d-proton $($vkd3dRelease.tag_name): $($vkd3dAsset.name)"
    $vkd3dArchive = Join-Path $work $vkd3dAsset.name
    Invoke-WebRequest -Uri $vkd3dAsset.browser_download_url -OutFile $vkd3dArchive

    # The archive is a zstd-compressed tarball: unpack the tar first, then its contents.
    $vkd3dDir = Join-Path $work 'vkd3d'
    7z x -y "-o$vkd3dDir" $vkd3dArchive | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Failed to extract $vkd3dArchive" }
    $vkd3dTar = Get-ChildItem -Path $vkd3dDir -Filter '*.tar' | Select-Object -First 1
    if (-not $vkd3dTar) { throw 'Unexpected vkd3d-proton archive layout' }
    7z x -y "-o$vkd3dDir" $vkd3dTar.FullName | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Failed to extract $($vkd3dTar.FullName)" }

    $vkd3dRoot = Get-ChildItem -Path $vkd3dDir -Directory | Where-Object { $_.Name -like 'vkd3d-proton-*' } | Select-Object -First 1
    if (-not $vkd3dRoot) { throw 'Unexpected vkd3d-proton archive layout' }

    Install-Dlls 'vkd3d-proton' (Join-Path $vkd3dRoot.FullName 'x64') $system32
    Install-Dlls 'vkd3d-proton' (Join-Path $vkd3dRoot.FullName 'x86') $syswow64

    # Windows' opengl32 needs a vendor OpenGL driver, which the guest does not have. Mesa's picks the first
    # driver it can start, Zink on the Vulkan shim or llvmpipe on the CPU.
    $mesaRelease = Invoke-RestMethod -Uri 'https://api.github.com/repos/pal1000/mesa-dist-win/releases/latest' -Headers $apiHeaders
    $mesaAsset = $mesaRelease.assets | Where-Object { $_.name -like 'mesa3d-*-release-msvc.7z' } | Select-Object -First 1
    if (-not $mesaAsset) { throw "No Mesa MSVC release archive found in $($mesaRelease.tag_name)" }

    Write-Host "Downloading Mesa $($mesaRelease.tag_name): $($mesaAsset.name)"
    $mesaArchive = Join-Path $work $mesaAsset.name
    Invoke-WebRequest -Uri $mesaAsset.browser_download_url -OutFile $mesaArchive

    $mesaRoot = Join-Path $work 'mesa'
    7z x -y "-o$mesaRoot" $mesaArchive | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Failed to extract $mesaArchive" }

    foreach ($file in 'opengl32.dll', 'libgallium_wgl.dll') {
        Copy-Item -Path (Join-Path $mesaRoot "x64/$file") -Destination (Join-Path $system32 $file) -Force
        Copy-Item -Path (Join-Path $mesaRoot "x86/$file") -Destination (Join-Path $syswow64 $file) -Force
    }
    Write-Host 'Mesa OpenGL installed (system32 + syswow64)'

    # Install the GPU-bridge Vulkan shim as the system vulkan-1.dll DXVK loads.
    Copy-Item -Path $Shim64 -Destination (Join-Path $system32 'vulkan-1.dll') -Force
    Copy-Item -Path $Shim32 -Destination (Join-Path $syswow64 'vulkan-1.dll') -Force
    Write-Host 'Vulkan shim installed as vulkan-1.dll (system32 + syswow64)'
}
finally {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
