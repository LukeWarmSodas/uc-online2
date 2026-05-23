# ============================================================
# Detect-Photon.ps1
#
# Scans a Unity game folder and reports which UCOnline2 plugin
# the user should pick. Run this BEFORE running any of the
# photon_*\Setup.bat wizards so you don't waste time on the
# wrong one.
#
# It identifies:
#   - Unity scripting backend  : IL2CPP vs Mono
#   - Photon middleware flavor : Realtime / PUN  vs  Fusion 2
#   - Whether voice chat ships : PhotonVoice presence
#   - Whether Beebyte obfuscation is in use (hint to also pair
#     with unity_auth_bypass for SteamAuth-style gates)
#
# Output: one recommended plugin folder + ini sketch.
# ============================================================
param(
    [string]$GameDir = ""
)

$ErrorActionPreference = "Stop"

function Write-Status($label, $value, $color = "Gray") {
    Write-Host ("  {0,-22} " -f ($label + ":")) -NoNewline
    Write-Host $value -ForegroundColor $color
}

if (-not $GameDir) {
    Write-Host ""
    Write-Host "============================================================"
    Write-Host "  UCOnline2 -- Photon plugin detector"
    Write-Host "============================================================"
    Write-Host ""
    $GameDir = Read-Host "Full path to the game folder (the one with <game>.exe)"
}

$GameDir = $GameDir.Trim('"').Trim()
if (-not (Test-Path -LiteralPath $GameDir -PathType Container)) {
    Write-Host ""
    Write-Host "ERROR: that folder does not exist:" -ForegroundColor Red
    Write-Host "       $GameDir"
    exit 1
}

# ---- find the _Data folder
$dataDir = Get-ChildItem -LiteralPath $GameDir -Directory -Filter "*_Data" -ErrorAction SilentlyContinue |
           Select-Object -First 1
if (-not $dataDir) {
    Write-Host ""
    Write-Host "ERROR: no <Game>_Data folder found in:" -ForegroundColor Red
    Write-Host "       $GameDir"
    Write-Host "       Are you sure this is a Unity game?"
    exit 1
}

Write-Host ""
Write-Host "Inspecting:" -ForegroundColor White
Write-Status "data folder" $dataDir.Name
$gameName = $dataDir.Name -replace '_Data$',''
Write-Status "game name" $gameName

# Steam AppId hint (from steam_appid.txt if present)
$steamAppIdFile = Join-Path $GameDir "steam_appid.txt"
if (Test-Path -LiteralPath $steamAppIdFile) {
    $steamAppId = (Get-Content -LiteralPath $steamAppIdFile -ErrorAction SilentlyContinue | Select-Object -First 1).Trim()
    if ($steamAppId) { Write-Status "steam_appid.txt" $steamAppId Cyan }
}

# ---- detect scripting backend
$il2cppMeta = Join-Path $dataDir.FullName "il2cpp_data\Metadata\global-metadata.dat"
$managedDir = Join-Path $dataDir.FullName "Managed"
$isIL2CPP   = Test-Path -LiteralPath $il2cppMeta
$isMono     = Test-Path -LiteralPath $managedDir

$backend = ""
if ($isIL2CPP -and $isMono) {
    # IL2CPP build can ship a Managed folder for editor-only stuff but
    # the il2cpp_data is authoritative.
    $backend = "IL2CPP"
} elseif ($isIL2CPP) {
    $backend = "IL2CPP"
} elseif ($isMono) {
    $backend = "Mono"
}

if (-not $backend) {
    Write-Status "backend" "UNKNOWN (no il2cpp_data and no Managed/)" Red
    Write-Host ""
    Write-Host "Doesn't look like a standard Unity build. Aborting." -ForegroundColor Red
    exit 1
}
Write-Status "scripting backend" $backend Green

# ---- detect Photon flavor + voice
$photonFlavor = ""
$hasVoice     = $false
$beebyteHits  = 0

if ($backend -eq "Mono") {
    $managedDlls = Get-ChildItem -LiteralPath $managedDir -Filter "*.dll" -File -ErrorAction SilentlyContinue
    $names = $managedDlls | ForEach-Object { $_.Name }
    $hasFusion = ($names | Where-Object { $_ -match '^Fusion(\.|$)' }) -ne $null
    $hasPun    = ($names -contains 'PhotonUnityNetworking.dll') -or
                 ($names -contains 'PhotonRealtime.dll')
    $hasVoice  = ($names -contains 'PhotonVoice.dll') -or
                 ($names -contains 'PhotonVoice.PUN.dll')
    if ($hasFusion)  { $photonFlavor = "Fusion" }
    elseif ($hasPun) { $photonFlavor = "Realtime" }
} else {
    # IL2CPP -- grep metadata for symbol names. Beebyte renames classes
    # but PUBLIC reflection types and external Photon dependency names
    # survive (the C# code calls into them by name through IL2CPP's
    # metadata, which retains those strings).
    try {
        $bytes = [IO.File]::ReadAllBytes($il2cppMeta)
    } catch {
        Write-Status "metadata" "FAILED TO READ" Red
        exit 1
    }
    $text = [Text.Encoding]::ASCII.GetString($bytes)

    $hasFusion = $text.Contains("NetworkRunner") -or
                 $text.Contains("Fusion.Photon.Realtime") -or
                 $text.Contains("PhotonAppSettings")
    $hasPun    = $text.Contains("PhotonUnityNetworking") -or
                 ($text.Contains("PhotonNetwork") -and $text.Contains("LoadBalancingClient"))
    $hasVoice  = $text.Contains("PhotonVoice") -or $text.Contains("VoiceConnection")

    # Beebyte signature: many ASCII references to Malayalam Unicode
    # ranges (U+0D00..U+0D7F) appear in metadata when classes were
    # renamed. We sniff by counting raw 3-byte UTF-8 sequences that
    # land in the Malayalam block (E0 B4 ?? / E0 B5 ??).
    for ($i = 0; $i -lt ($bytes.Length - 2); $i++) {
        if ($bytes[$i] -eq 0xE0 -and ($bytes[$i+1] -eq 0xB4 -or $bytes[$i+1] -eq 0xB5)) {
            $beebyteHits++
            if ($beebyteHits -ge 500) { break }  # enough to be sure
        }
    }

    if     ($hasFusion -and -not $hasPun) { $photonFlavor = "Fusion" }
    elseif ($hasPun -and -not $hasFusion) { $photonFlavor = "Realtime" }
    elseif ($hasPun -and $hasFusion)      { $photonFlavor = "Both" }
}

if ($photonFlavor) {
    Write-Status "Photon flavor" $photonFlavor Green
} else {
    Write-Status "Photon flavor" "none detected" Red
}
Write-Status "voice chat ships" $(if ($hasVoice) { "YES (needs a Voice app)" } else { "no" }) $(if ($hasVoice) { "Yellow" } else { "Gray" })

if ($backend -eq "IL2CPP") {
    Write-Status "Beebyte obfuscation" $(if ($beebyteHits -ge 50) { "YES ($beebyteHits+ Malayalam hits)" } else { "no" }) $(if ($beebyteHits -ge 50) { "Yellow" } else { "Gray" })
}

# ---- recommendation
Write-Host ""
Write-Host "============================================================"
Write-Host "  Recommendation"
Write-Host "============================================================"
Write-Host ""

if (-not $photonFlavor) {
    Write-Host "  This game doesn't appear to use Photon at all." -ForegroundColor Yellow
    Write-Host "  UCOnline2's Photon plugins won't help here."
    Write-Host ""
    Write-Host "  (UCOnline2 still works as a generic Steam emu; it"
    Write-Host "   only does Photon redirection if a Photon plugin is"
    Write-Host "   present.)"
    exit 0
}

if ($photonFlavor -eq "Both") {
    Write-Host "  Both PUN and Fusion class names found in the same" -ForegroundColor Yellow
    Write-Host "  build. This is unusual. Try Fusion first; if its"
    Write-Host "  hooks don't fire (check the log), fall back to"
    Write-Host "  Realtime."
    Write-Host ""
}

$plugin = ""
if ($photonFlavor -eq "Fusion") {
    $plugin = "photon_fusion"
} elseif ($photonFlavor -eq "Realtime" -or $photonFlavor -eq "Both") {
    $plugin = if ($backend -eq "Mono") { "photon_realtime_mono" } else { "photon_realtime" }
}

Write-Host "  -> Use plugins\$plugin\Setup.bat" -ForegroundColor Cyan
Write-Host ""

# Extras users need to know about
$extras = @()
if ($hasVoice) {
    $extras += "Voice chat ships -- create a second Photon app of TYPE 'Voice' on the dashboard. Setup.bat will prompt for its GUID."
}
if ($beebyteHits -ge 50) {
    $extras += "Beebyte obfuscation detected. If this game has a 'failed to get account info'-style gate before multiplayer, you'll also need plugins\unity_auth_bypass\unity_auth_bypass.dll. Phasmophobia is the known case; its gate is already wired in that plugin."
}

if ($extras.Count -gt 0) {
    Write-Host "  Also note:"
    foreach ($e in $extras) {
        Write-Host "    - $e" -ForegroundColor Yellow
    }
    Write-Host ""
}

# Sketch the ini section the recommended plugin will write
Write-Host "  union-crax.ini sketch:" -ForegroundColor White
Write-Host "    [Settings]"
Write-Host "    AppId=480"
if ($steamAppId) { Write-Host "    ogAppId=$steamAppId" } else { Write-Host "    ogAppId=<the game's real Steam AppId>" }
Write-Host "    PluginsFolder=plugins"
Write-Host "    GetStubbedLol=false"
Write-Host ""
if ($photonFlavor -eq "Fusion") {
    Write-Host "    [Fusion]"
    Write-Host "    PhotonAppIdFusion=<your-Fusion-app-GUID>"
} else {
    Write-Host "    [Realtime]"
    Write-Host "    PhotonAppIdRealtime=<your-Realtime-app-GUID>"
    if ($hasVoice) {
        Write-Host "    PhotonAppIdVoice=<your-Voice-app-GUID>"
    }
}
Write-Host "    ForcedAuthType=0"
Write-Host ""
