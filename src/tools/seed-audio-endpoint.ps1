# Seeds an active playback endpoint into a loaded guest SOFTWARE hive. Hosts without a local playback device
# (CI runners, RDP sessions) have none, so DirectSound fails with DSERR_NODRIVER and WASAPI finds no default device.
# Administrators may not create subkeys under MMDevices\Audio\Render; only keys opened with
# REG_OPTION_BACKUP_RESTORE while SeRestorePrivilege is enabled bypass that ACL.

param([Parameter(Mandatory = $true)][string]$Hive)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace Sogen -Name Registry -MemberDefinition @'
[DllImport("ntdll.dll")]
public static extern int RtlAdjustPrivilege(int privilege, bool enable, bool currentThread, out bool previous);

[DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
public static extern int RegCreateKeyEx(UIntPtr key, string subKey, int reserved, string cls, int options, int access, IntPtr security, out UIntPtr result, out int disposition);

[DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
public static extern int RegSetValueEx(UIntPtr key, string name, int reserved, int type, byte[] data, int size);

[DllImport("advapi32.dll")]
public static extern int RegCloseKey(UIntPtr key);
'@

$SE_RESTORE_PRIVILEGE = 18
$REG_OPTION_BACKUP_RESTORE = 4
$KEY_ALL_ACCESS = 0xF003F
$HKEY_LOCAL_MACHINE = [UIntPtr][uint32]2147483650
$REG_SZ = 1
$REG_DWORD = 4

function Set-Key([string]$Path, [hashtable]$Values) {
    $key = [UIntPtr]::Zero
    $disposition = 0
    $status = [Sogen.Registry]::RegCreateKeyEx($HKEY_LOCAL_MACHINE, $Path, 0, $null, $REG_OPTION_BACKUP_RESTORE, $KEY_ALL_ACCESS, [IntPtr]::Zero, [ref]$key, [ref]$disposition)
    if ($status -ne 0) { throw "Creating HKLM\$Path failed: $status" }

    foreach ($name in $Values.Keys) {
        $value = $Values[$name]
        if ($value -is [string]) {
            $data = [Text.Encoding]::Unicode.GetBytes($value + [char]0)
            $type = $REG_SZ
        }
        else {
            $data = [BitConverter]::GetBytes([uint32]$value)
            $type = $REG_DWORD
        }
        $status = [Sogen.Registry]::RegSetValueEx($key, $name, 0, $type, $data, $data.Length)
        if ($status -ne 0) { throw "Setting $name failed: $status" }
    }

    [void][Sogen.Registry]::RegCloseKey($key)
}

$previous = $false
$status = [Sogen.Registry]::RtlAdjustPrivilege($SE_RESTORE_PRIVILEGE, $true, $false, [ref]$previous)
if ($status -ne 0) { throw "Enabling SeRestorePrivilege failed: $status" }

$endpointId = '{5e9d8c1a-7b3f-4c2e-9a61-0d4b8f2e6c37}'
$endpoint = "$Hive\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$endpointId"
Set-Key $endpoint @{ 'DeviceState' = 1 }
# PKEY_Device_FriendlyName, PKEY_DeviceInterface_FriendlyName and the endpoint's device interface path. Without the
# path, mmdevapi keeps re-reading the endpoint, and some games (MW3) never get past audio initialization.
Set-Key "$endpoint\Properties" @{
    '{a45c254e-df1c-4efd-8020-67d146a850e0},2' = 'Speakers'
    '{b3f8fa53-0004-438e-9003-51a46e139bfc},6' = 'Sogen Audio'
    '{9c119480-ddc2-4954-a150-5bd240d454ad},1' = "\\?\SWD#MMDEVAPI#{0.0.0.00000000}.$endpointId#{e6327cad-dcec-4949-ae8a-991e976a79d2}"
}
