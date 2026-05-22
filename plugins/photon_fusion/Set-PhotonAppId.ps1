# Replace the Photon Fusion AppId embedded in a Unity IL2CPP
# game's assets with one you control.
#
# In a Unity build using Photon Fusion 2, the PhotonAppSettings
# ScriptableObject is serialized into one of the *.assets files
# under <Game>_Data/. The AppIdFusion GUID lives there as 36
# plain ASCII characters. Replacement in place preserves all
# Unity asset bookkeeping bytes, so no other file changes are
# needed.
#
# This script auto-discovers the stock GUID by:
#   1. Searching every .assets file in <Game>_Data/ for the
#      ASCII string "PhotonAppSettings".
#   2. Scanning the next few KB after the marker for a
#      length-prefixed 36-char GUID (uint32 length=0x24 then
#      36 hex/dash chars).
#   3. Replacing the GUID bytes in place with the user-supplied
#      one.
#
# Usage:
#   .\Set-PhotonAppId.ps1 -GameDir "C:\path\to\TheGame" `
#                          -NewAppId "<your-photon-fusion-guid>"
#
# Pass -DryRun to discover the GUID(s) and preview what would
# be replaced, without modifying any files.
#
# Pass -Revert to restore the .bak files this script creates
# on first run.

[CmdletBinding(DefaultParameterSetName="Apply")]
param(
    [Parameter(Mandatory=$true)]
    [string]$GameDir,

    [Parameter(Mandatory=$true, ParameterSetName="Apply")]
    [Parameter(Mandatory=$false, ParameterSetName="DryRun")]
    [ValidatePattern('^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$')]
    [string]$NewAppId,

    [Parameter(ParameterSetName="DryRun")]
    [switch]$DryRun,

    [Parameter(ParameterSetName="Revert")]
    [switch]$Revert
)

$dataDir = Get-ChildItem -Path $GameDir -Directory -Filter '*_Data' -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $dataDir) {
    throw "No <Game>_Data directory found under $GameDir"
}

if ($Revert) {
    $backups = Get-ChildItem -Path $dataDir.FullName -Filter '*.assets.bak' -ErrorAction SilentlyContinue
    if (-not $backups) { throw "No .assets.bak files found under $($dataDir.FullName)" }
    foreach ($b in $backups) {
        $orig = $b.FullName -replace '\.bak$',''
        Copy-Item $b.FullName $orig -Force
        Write-Output "Restored $($b.Name) -> $(Split-Path -Leaf $orig)"
    }
    return
}

$marker  = [System.Text.Encoding]::ASCII.GetBytes('PhotonAppSettings')
$lenByte = [byte[]](36,0,0,0)   # uint32 LE = 36 (= GUID length)

$assetFiles = Get-ChildItem -Path $dataDir.FullName -Filter '*.assets'
if (-not $assetFiles) { throw "No .assets files in $($dataDir.FullName)" }

$hits = @()
foreach ($f in $assetFiles) {
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)

    # Find each occurrence of "PhotonAppSettings"
    for ($i = 0; $i -le $bytes.Length - $marker.Length; $i++) {
        $match = $true
        for ($j = 0; $j -lt $marker.Length; $j++) {
            if ($bytes[$i + $j] -ne $marker[$j]) { $match = $false; break }
        }
        if (-not $match) { continue }

        # Scan forward up to 4KB for the length-prefix `24 00 00 00`
        # followed by 36 ASCII chars matching a GUID layout.
        $scanStart = $i + $marker.Length
        $scanEnd   = [Math]::Min($bytes.Length - 40, $scanStart + 4096)
        for ($k = $scanStart; $k -le $scanEnd; $k++) {
            if ($bytes[$k]   -ne $lenByte[0] -or
                $bytes[$k+1] -ne $lenByte[1] -or
                $bytes[$k+2] -ne $lenByte[2] -or
                $bytes[$k+3] -ne $lenByte[3]) { continue }
            $guidOff = $k + 4
            $guidStr = [System.Text.Encoding]::ASCII.GetString($bytes, $guidOff, 36)
            if ($guidStr -match '^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$') {
                $hits += [PSCustomObject]@{
                    File     = $f.FullName
                    Offset   = $guidOff
                    OldGuid  = $guidStr
                }
                break
            }
        }
    }
}

if ($hits.Count -eq 0) {
    throw "No Photon Fusion GUID found near 'PhotonAppSettings' markers in any .assets file. Game may not use Photon Fusion, or its serialization layout differs."
}

Write-Output "Discovered $($hits.Count) GUID occurrence(s):"
foreach ($h in $hits) {
    Write-Output ("  {0} @ 0x{1:X} -> {2}" -f (Split-Path -Leaf $h.File), $h.Offset, $h.OldGuid)
}

if ($DryRun) { Write-Output "(dry run; no files modified)"; return }

$newBytes = [System.Text.Encoding]::ASCII.GetBytes($NewAppId.ToLowerInvariant())
$touchedFiles = @{}

foreach ($h in $hits) {
    if (-not $touchedFiles.ContainsKey($h.File)) {
        $bak = $h.File + '.bak'
        if (-not (Test-Path $bak)) {
            Copy-Item $h.File $bak -Force
            Write-Output "Backed up $(Split-Path -Leaf $h.File) -> $(Split-Path -Leaf $bak)"
        }
        $touchedFiles[$h.File] = [System.IO.File]::ReadAllBytes($h.File)
    }
    $buf = $touchedFiles[$h.File]
    for ($j = 0; $j -lt 36; $j++) { $buf[$h.Offset + $j] = $newBytes[$j] }
}

foreach ($file in $touchedFiles.Keys) {
    [System.IO.File]::WriteAllBytes($file, $touchedFiles[$file])
    Write-Output "Wrote $(Split-Path -Leaf $file)"
}

Write-Output ""
Write-Output "Replaced $($hits.Count) GUID(s) with $NewAppId"
