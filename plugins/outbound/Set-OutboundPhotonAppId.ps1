# Replace Outbound's bundled Photon Fusion AppId with one you control.
#
# Outbound's PhotonAppSettings asset embeds its AppIdFusion GUID
# directly inside Outbound_Data\resources.assets. Both Outbound's
# real GUID and any replacement GUID are 36 ASCII characters, so
# we can swap in place without touching the file size or any
# Unity bookkeeping bytes.
#
# Usage:
#   .\Set-OutboundPhotonAppId.ps1 -GameDir "C:\path\to\Outbound" `
#                                 -NewAppId "<your-photon-app-guid>"
#
# Pass -Revert to restore the .bak created on first run.

[CmdletBinding(DefaultParameterSetName="Apply")]
param(
    [Parameter(Mandatory=$true)]
    [string]$GameDir,

    [Parameter(Mandatory=$true, ParameterSetName="Apply")]
    [ValidatePattern('^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$')]
    [string]$NewAppId,

    [Parameter(ParameterSetName="Revert")]
    [switch]$Revert
)

$assets = Join-Path $GameDir 'Outbound_Data\resources.assets'
$backup = $assets + '.bak'

if (-not (Test-Path $assets)) {
    throw "resources.assets not found at: $assets"
}

if ($Revert) {
    if (-not (Test-Path $backup)) { throw "No backup file found at $backup" }
    Copy-Item $backup $assets -Force
    Write-Output "Restored resources.assets from backup."
    return
}

# Outbound's stock Photon Fusion AppId GUID. If a future patch
# changes it, this will fail to find -- adjust here.
$stock = 'cffbe809-5036-43d8-84a1-7bf16c924721'

if ($NewAppId.Length -ne $stock.Length) {
    throw "GUID length mismatch (expected 36 chars)"
}

if (-not (Test-Path $backup)) {
    Copy-Item $assets $backup -Force
    Write-Output "Backed up resources.assets -> resources.assets.bak"
}

$bytes = [System.IO.File]::ReadAllBytes($assets)
$oldB = [System.Text.Encoding]::ASCII.GetBytes($stock)
$newB = [System.Text.Encoding]::ASCII.GetBytes($NewAppId.ToLowerInvariant())

$found = $false
for ($i = 0; $i -le $bytes.Length - $oldB.Length; $i++) {
    $match = $true
    for ($j = 0; $j -lt $oldB.Length; $j++) {
        if ($bytes[$i + $j] -ne $oldB[$j]) { $match = $false; break }
    }
    if ($match) {
        for ($j = 0; $j -lt $newB.Length; $j++) { $bytes[$i + $j] = $newB[$j] }
        [System.IO.File]::WriteAllBytes($assets, $bytes)
        Write-Output ("Replaced AppIdFusion at offset 0x{0:X} ({0})." -f $i)
        Write-Output "  was: $stock"
        Write-Output "  now: $NewAppId"
        $found = $true
        break
    }
}

if (-not $found) {
    throw "Stock GUID '$stock' not found in resources.assets. Already patched, or game version changed."
}
