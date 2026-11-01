# Create directory structure under filesys root for each user in ProfileList.
# Usage: .\create-profile-dirs.ps1 -FilesysRoot "root\filesys"
# ProfileImagePath is read, env vars expanded, path lowercased, then dirs created.

param(
    [Parameter(Mandatory = $true)]
    [string] $FilesysRoot
)

function New-FilesysDirectories([string] $WindowsPath, [string[]] $SubDirectories) {
    $exp = [Environment]::ExpandEnvironmentVariables($WindowsPath)
    $low = $exp.ToLower()
    $rel = $low -replace '^([a-z]):', '$1'
    $target = Join-Path $FilesysRoot $rel
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    Write-Host 'Created:' $target
    $SubDirectories | ForEach-Object {
        $sub = Join-Path $target $_
        New-Item -ItemType Directory -Path $sub -Force | Out-Null
        Write-Host 'Created:' $sub
    }
}

$profileListPath = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList'
Get-ChildItem $profileListPath -ErrorAction SilentlyContinue | ForEach-Object {
    $p = (Get-ItemProperty -Path $_.PSPath -Name ProfileImagePath -ErrorAction SilentlyContinue).ProfileImagePath
    if ($p) {
        # The folders every profile gets from the default profile. The shell only resolves a known folder that
        # exists, so a missing one makes SHGetFolderPath fail and games fall back to saving somewhere else.
        New-FilesysDirectories $p @('appdata\local', 'appdata\roaming', 'appdata\locallow',
                                    'appdata\roaming\microsoft\windows\start menu\programs\startup', 'desktop', 'documents',
                                    'downloads', 'favorites', 'links', 'music', 'pictures', 'saved games', 'videos')
    }
}

# The shared folders are values of ProfileList rather than profiles. Installers resolve the all-users desktop and Start
# menu through them to place their shortcuts.
$sharedFolders = Get-ItemProperty -Path $profileListPath -ErrorAction SilentlyContinue
if ($sharedFolders.Public) {
    New-FilesysDirectories $sharedFolders.Public @('desktop', 'documents', 'downloads', 'music', 'pictures', 'videos')
}
if ($sharedFolders.ProgramData) {
    New-FilesysDirectories $sharedFolders.ProgramData @('microsoft\windows\start menu\programs\startup')
}
