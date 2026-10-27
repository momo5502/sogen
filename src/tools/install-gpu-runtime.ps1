# Provisions the guest GPU runtime into an emulation root: drops pinned DXVK
# D3D->Vulkan translation DLLs over the bundled DirectX ones, replaces opengl32.dll with
# Mesa's, and installs the GPU-bridge Vulkan shim as vulkan-1.dll. 64-bit files go to
# system32, 32-bit files to syswow64.

[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$WindowsDir, # <root>/filesys/c/windows
    [Parameter(Mandatory)][string]$Shim64,     # 64-bit vulkan-shim.dll
    [Parameter(Mandatory)][string]$Shim32      # 32-bit vulkan-shim.dll
)

$ErrorActionPreference = 'Stop'

$system32 = Join-Path $WindowsDir 'system32'
$syswow64 = Join-Path $WindowsDir 'syswow64'

# DXVK ships its D3D DLLs under x64/ (64-bit) and x32/ (32-bit); overwrite the bundled DirectX ones.
function Install-Dxvk([string]$srcDir, [string]$dstDir) {
    if (-not (Test-Path $srcDir)) { throw "DXVK source directory not found: $srcDir" }
    Get-ChildItem -Path $srcDir -Filter '*.dll' | ForEach-Object {
        $dst = Join-Path $dstDir $_.Name
        Write-Host "DXVK: $($_.Name) -> $dst"
        Copy-Item -Path $_.FullName -Destination $dst -Force
    }
}

# Pinned so that rebuilding the emulation root reproduces the same guest GPU runtime.
$dxvk = @{
    Url    = 'https://github.com/doitsujin/dxvk/releases/download/v3.1.1/dxvk-3.1.1.tar.gz'
    Sha256 = '40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608'
}
$mesa = @{
    Url    = 'https://github.com/pal1000/mesa-dist-win/releases/download/26.2.4/mesa3d-26.2.4-release-msvc.7z'
    Sha256 = '351fc8c8b695878ffb3eaa044b3ead08672a48b1a045e3c3e3975811df0f6695'
}

function Get-PinnedArchive([hashtable]$archive, [string]$dir) {
    $path = Join-Path $dir ([System.IO.Path]::GetFileName($archive.Url))
    Write-Host "Downloading $($archive.Url)"
    Invoke-WebRequest -Uri $archive.Url -OutFile $path
    $hash = (Get-FileHash -Path $path -Algorithm SHA256).Hash
    if ($hash -ne $archive.Sha256) { throw "SHA-256 mismatch for $path`: expected $($archive.Sha256), got $hash" }
    return $path
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('dxvk-' + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $work -Force | Out-Null
try {
    $tarball = Get-PinnedArchive $dxvk $work
    tar -xzf $tarball -C $work
    if ($LASTEXITCODE -ne 0) { throw "Failed to extract $tarball" }

    $dxvkRoot = Get-ChildItem -Path $work -Directory | Where-Object { $_.Name -like 'dxvk-*' } | Select-Object -First 1
    if (-not $dxvkRoot) { throw 'Unexpected DXVK archive layout' }

    Install-Dxvk (Join-Path $dxvkRoot.FullName 'x64') $system32
    Install-Dxvk (Join-Path $dxvkRoot.FullName 'x32') $syswow64

    # Windows' opengl32 needs a vendor OpenGL driver, which the guest does not have. Mesa's picks the first
    # driver it can start, Zink on the Vulkan shim or llvmpipe on the CPU.
    $mesaArchive = Get-PinnedArchive $mesa $work
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
