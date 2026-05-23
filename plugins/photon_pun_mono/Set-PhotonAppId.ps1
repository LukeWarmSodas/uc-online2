# Replace the Photon AppIds embedded in a Unity game's
# resources.assets with ones you control.
#
# Terminology: a PUN-using Unity game stores its AppIds inside a
# ServerSettings ScriptableObject (asset name "PhotonServerSettings").
# That ScriptableObject contains up to four AppId strings:
#   AppIdFusion, AppIdRealtime, AppIdChat, AppIdVoice
# Most games leave several empty. PUN games typically populate
# AppIdRealtime + (often) AppIdVoice. On the Photon dashboard,
# both correspond to apps of type "Realtime" and "Voice"
# respectively. PUN itself is a Unity wrapper built on the
# Photon Realtime SDK; the dashboard product type for PUN is
# "Realtime", not "PUN".
#
# Sibling of plugins/photon_fusion/Set-PhotonAppId.ps1, which
# anchors on the "PhotonAppSettings" marker (Fusion 2's
# ScriptableObject, single AppIdFusion slot).
#
# Usage:
#   # Recommended for PUN+Voice games (most modern PUN games):
#   .\Set-PhotonAppId.ps1 -GameDir "C:\path\to\TheGame" `
#                          -NewAppId      "<realtime-app-guid>" `
#                          -NewVoiceAppId "<voice-app-guid>"
#
#   # Realtime-only games (no voice chat):
#   .\Set-PhotonAppId.ps1 -GameDir "C:\path\to\TheGame" `
#                          -NewAppId "<realtime-app-guid>"
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

    # Optional second-slot GUID. PUN's PhotonServerSettings can
    # contain multiple AppIds (AppIdRealtime + AppIdChat / AppIdVoice).
    # If this is supplied, the SECOND length-prefixed GUID found
    # gets patched with it. If omitted, the second slot also gets
    # $NewAppId (legacy behavior; works only if the game tolerates
    # a same-type AppId in the Voice/Chat slot, which most games
    # don't -- Photon validates by product type).
    [Parameter(Mandatory=$false, ParameterSetName="Apply")]
    [Parameter(Mandatory=$false, ParameterSetName="DryRun")]
    [ValidatePattern('^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$')]
    [string]$NewVoiceAppId,

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

# Native-speed byte search via inline C#. Streams the file
# in 64 MiB chunks so even multi-GB .assets files (some games
# have these) don't blow up memory.
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Collections.Generic;

public class PhotonAppIdScanner
{
    static readonly Regex GuidRegex = new Regex(
        @"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$",
        RegexOptions.Compiled);

    public struct Hit { public long Offset; public string OldGuid; }

    public static List<Hit> Scan(string path, string markerStr, int scanAhead)
    {
        var hits = new List<Hit>();
        byte[] marker = Encoding.ASCII.GetBytes(markerStr);
        int chunkSize = 64 * 1024 * 1024;
        // Overlap must be large enough that any marker + the
        // following length-prefix + 36-char GUID fits in the
        // tail of one chunk OR the head of the next.
        int overlap = marker.Length + scanAhead + 40;

        using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20))
        {
            byte[] buf = new byte[chunkSize + overlap];
            long baseOff = 0;
            int carry = 0;
            while (true)
            {
                int n = fs.Read(buf, carry, chunkSize);
                if (n <= 0) break;
                int valid = carry + n;
                int searchEnd = (n == chunkSize) ? valid - overlap : valid;

                for (int i = 0; i <= searchEnd - marker.Length; i++)
                {
                    bool ok = true;
                    for (int j = 0; j < marker.Length; j++)
                        if (buf[i + j] != marker[j]) { ok = false; break; }
                    if (!ok) continue;

                    int kStart = i + marker.Length;
                    int kEnd = Math.Min(valid - 40, kStart + scanAhead);
                    for (int k = kStart; k <= kEnd; k++)
                    {
                        if (buf[k] != 36 || buf[k+1] != 0 ||
                            buf[k+2] != 0 || buf[k+3] != 0) continue;
                        int guidOff = k + 4;
                        string g = Encoding.ASCII.GetString(buf, guidOff, 36);
                        if (GuidRegex.IsMatch(g))
                        {
                            hits.Add(new Hit { Offset = baseOff + guidOff, OldGuid = g });
                            // continue scanning: PUN's PhotonServerSettings
                            // can contain multiple GUIDs (AppIdRealtime +
                            // AppIdChat + AppIdVoice + AppIdFusion). All of
                            // them are bundled in the same block and any
                            // unpatched one points back at the dev's app,
                            // causing connection rejection downstream.
                        }
                    }
                }

                if (n < chunkSize) break;
                // Carry the overlap window into the next read.
                int tail = valid - searchEnd;
                Buffer.BlockCopy(buf, searchEnd, buf, 0, tail);
                baseOff += searchEnd;
                carry = tail;
            }
        }
        return hits;
    }
}
'@ -Language CSharp

$assetFiles = Get-ChildItem -Path $dataDir.FullName -Filter '*.assets'
if (-not $assetFiles) { throw "No .assets files in $($dataDir.FullName)" }

$hits = @()
foreach ($f in $assetFiles) {
    # Anchor on the asset-name string Unity writes immediately
    # before the ScriptableObject's data block. For PUN that's
    # "PhotonServerSettings"; the scanner returns ALL length-
    # prefixed GUIDs in the ~4KB after the marker -- in PUN that
    # means AppIdRealtime + any of AppIdChat/AppIdVoice/AppIdFusion
    # that the game set. We patch every one to the user's GUID
    # because any unpatched one points back at the dev's app and
    # causes Photon NameServer to reject connection attempts
    # (observed in R.E.P.O. with AppIdVoice still pointing at the
    # dev's voice app -> InvalidAuthentication on the Online click).
    foreach ($h in [PhotonAppIdScanner]::Scan($f.FullName, 'PhotonServerSettings', 4096)) {
        $hits += [PSCustomObject]@{
            File    = $f.FullName
            Offset  = $h.Offset
            OldGuid = $h.OldGuid
        }
    }
}

if ($hits.Count -eq 0) {
    Write-Host ""
    Write-Host "ERROR: GUID not found - plugin will not work." -ForegroundColor Red
    Write-Host "  No Photon PUN AppIdRealtime GUID was found inside any" -ForegroundColor Red
    Write-Host "  .assets file under $($dataDir.FullName)." -ForegroundColor Red
    Write-Host "  The game probably doesn't use Photon PUN, or it stores" -ForegroundColor Red
    Write-Host "  its config in a non-standard layout. If the game uses" -ForegroundColor Red
    Write-Host "  Photon Fusion 2 instead, try the photon_fusion plugin." -ForegroundColor Red
    exit 2
}

Write-Output "Discovered $($hits.Count) GUID occurrence(s):"
foreach ($h in $hits) {
    Write-Output ("  {0} @ 0x{1:X} -> {2}" -f (Split-Path -Leaf $h.File), $h.Offset, $h.OldGuid)
}

if ($DryRun) { Write-Output "(dry run; no files modified)"; return }

# If a Voice GUID was supplied, use it for the SECOND slot (index 1)
# and any further slots fall back to the realtime GUID. PUN field
# order in PhotonServerSettings is typically:
#   [0] AppIdRealtime
#   [1] AppIdChat or AppIdVoice (whichever is set)
#   [2] the other one if set
# Photon validates each AppId server-side against its product type,
# so each slot must hold an AppId from a matching app on the
# Photon dashboard.
$mainBytes  = [System.Text.Encoding]::ASCII.GetBytes($NewAppId.ToLowerInvariant())
$voiceBytes = $null
if ($NewVoiceAppId) {
    $voiceBytes = [System.Text.Encoding]::ASCII.GetBytes($NewVoiceAppId.ToLowerInvariant())
}
$touchedFiles = @{}

$slotIdx = 0
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

    # Pick which GUID to write into this slot
    $writeBytes = $mainBytes
    $writeLabel = $NewAppId
    if ($slotIdx -eq 1 -and $voiceBytes) {
        $writeBytes = $voiceBytes
        $writeLabel = $NewVoiceAppId
    }
    for ($j = 0; $j -lt 36; $j++) { $buf[$h.Offset + $j] = $writeBytes[$j] }
    Write-Output ("  Slot {0} @ 0x{1:X}: {2} -> {3}" -f $slotIdx, $h.Offset, $h.OldGuid, $writeLabel)
    $slotIdx++
}

foreach ($file in $touchedFiles.Keys) {
    [System.IO.File]::WriteAllBytes($file, $touchedFiles[$file])
    Write-Output "Wrote $(Split-Path -Leaf $file)"
}

Write-Output ""
if ($voiceBytes) {
    Write-Output "Patched $($hits.Count) GUID slot(s): main=$NewAppId, voice=$NewVoiceAppId"
} else {
    Write-Output "Patched $($hits.Count) GUID slot(s) with $NewAppId"
    if ($hits.Count -gt 1) {
        Write-Host ""
        Write-Host "  NOTE: $($hits.Count) slots patched with the same AppId. If this is" -ForegroundColor Yellow
        Write-Host "  a PUN+Voice game and you only created a Realtime app on Photon," -ForegroundColor Yellow
        Write-Host "  the Voice slot will fail server-side validation. Re-run with" -ForegroundColor Yellow
        Write-Host "  -NewVoiceAppId <voice-app-guid> after creating a Photon Voice app." -ForegroundColor Yellow
    }
}
