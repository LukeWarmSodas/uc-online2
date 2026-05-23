# photon_pun plugin

**Universal Photon PUN multiplayer redirect for Unity IL2CPP games.**

Sibling of [`photon_fusion`](../photon_fusion/) (for Fusion 2 games) and [`photon_pun_mono`](../photon_pun_mono/) (for Mono PUN games). Redirects the game from the developer's Photon Cloud app to one you control, then forces the wire-time auth type so Photon's master accepts the client without a publisher Steam key.

## Quick terminology

- **Photon Realtime SDK** is Photon's low-level networking library. On the [Photon dashboard](https://dashboard.photonengine.com/) you create apps of type **`Realtime`**.
- **PUN (Photon Unity Networking)** is a *higher-level* Unity wrapper *built on top of* Photon Realtime. A PUN-using game ships both `PhotonUnityNetworking.dll` (PUN) and `PhotonRealtime.dll` (Realtime).
- A PUN game's Photon dashboard app is a **Realtime-type** app. PUN was a separate dashboard product type historically but Photon merged it into Realtime years ago.
- If the game has voice chat (via `PhotonVoice.PUN.dll`), it needs a **second, separate** Photon app of type **`Voice`** with its own AppId GUID.

This plugin targets IL2CPP-compiled Unity games whose C# code is in `GameAssembly.dll` (no `Managed/` folder of .NET DLLs). For Mono-compiled games use [`photon_pun_mono`](../photon_pun_mono/).

## Confirmed working

| Game | Steam AppId | Notes |
|---|---|---|
| _(none yet)_ | | |

**Known hard:** Phasmophobia (Steam 739630) — Phasmo's `Assembly-CSharp.dll` is Beebyte-obfuscated and gates multiplayer behind a UGS-auth UI check that fires *before* the Photon wire layer. None of this plugin's hooks help because PUN's `OpAuthenticate` / `SendOperation` are never reached. Cracking it needs the [OsOmE1/Beebyte-Deobfuscator](https://github.com/OsOmE1/Beebyte-Deobfuscator) tool to recover original class names, then a game-specific patch to bypass that UI check.

## Quick start

1. Set up two free Photon apps at <https://dashboard.photonengine.com/>:
   - One of type **`Realtime`** (used for the main multiplayer connection — even though the game uses PUN, the dashboard app type is Realtime).
   - One of type **`Voice`** if the game has voice chat (most do).
2. On each app, **Manage → Authentication → Add Provider → Custom**. Paste the same permissive Cloudflare Worker URL on both. **Uncheck "Reject Clients on Authentication Failure"**. Save.
3. **Double-click `Setup.bat`** in this folder. It prompts for: game folder → Realtime GUID → Voice GUID → real Steam AppId. It will:
   - Patch every Photon AppId slot in the game's `resources.assets`.
   - Drop `photon_pun.dll` into `<game>\plugins\`.
   - Write a complete `union-crax.ini` at the game root.
4. Drop UCOnline2's `steam_api64.dll` into `<game>\<Game>_Data\Plugins\x86_64\` (back up the original first).
5. Launch the game.

If `Setup.bat` prints `GUID NOT FOUND`, the game isn't a Photon PUN target. The DLL won't help on its own — try the [`photon_fusion`](../photon_fusion/) plugin if you think the game uses Fusion 2 instead.

## How it differs from `photon_fusion`

| | `photon_fusion` | `photon_pun` |
|---|---|---|
| Target middleware | Fusion 2 | PUN (built on Realtime SDK) |
| Library DLLs in game | `Fusion.Realtime.dll` etc. | `PhotonUnityNetworking.dll` + `PhotonRealtime.dll` |
| Class namespace | `Fusion.Photon.Realtime` | `Photon.Realtime` (and `Photon.Pun`) |
| ScriptableObject | `PhotonAppSettings` (single GUID `AppIdFusion`) | `ServerSettings` (multiple GUIDs: `AppIdRealtime`, `AppIdChat`, `AppIdVoice`, `AppIdFusion`) |
| Photon dashboard app | type `Fusion` | type `Realtime` (+ `Voice` for voice-chat games) |
| ini section | `[Fusion]` | `[PUN]` |

## ini configuration

`union-crax.ini` next to the game exe:

```ini
[Settings]
AppId=480
ogAppId=<the game's real Steam AppId>
PluginsFolder=plugins
GetStubbedLol=false

[PUN]
PhotonAppIdRealtime=<your Realtime-type app's GUID>
ForcedAuthType=0
```

- `PhotonAppIdRealtime` — your Photon Realtime app's AppId GUID. Same value used in the static asset edit. The plugin uses it as defense-in-depth: rewrites the `appId` string argument at `OpAuthenticate` wire-send AND in the `SendOperation` params dict, so even if the asset-side edit missed something the runtime sends your GUID.
- `ForcedAuthType=0` — `Custom` (matches the Custom Auth provider on the dashboard). Use `255` for `None` if you want pure anonymous instead.

## Manual script usage

```powershell
# Preview discoveries
.\Set-PhotonAppId.ps1 -GameDir 'C:\path\to\TheGame' -DryRun

# Apply with separate Realtime + Voice GUIDs (recommended for any
# PUN game that ships PhotonVoice -- without a real Voice app
# GUID in slot 1, Photon NameServer rejects with InvalidAuthentication)
.\Set-PhotonAppId.ps1 -GameDir 'C:\path\to\TheGame' `
                      -NewAppId '<realtime-app-guid>' `
                      -NewVoiceAppId '<voice-app-guid>'

# Apply with one GUID across all slots (works only if the game
# uses Realtime alone, no Voice / Chat / Fusion)
.\Set-PhotonAppId.ps1 -GameDir 'C:\path\to\TheGame' `
                      -NewAppId '<realtime-app-guid>'

# Roll back
.\Set-PhotonAppId.ps1 -GameDir 'C:\path\to\TheGame' -Revert
```

The script anchors on the ASCII string `PhotonServerSettings` (Unity asset name of PUN's ServerSettings ScriptableObject) and patches **every** length-prefixed 36-char GUID following it. Multi-GB `.assets` files are handled via chunked native-speed byte search.

## Trying it on a new game

PUN doesn't ship as a separate native DLL in IL2CPP builds — managed code is baked into `GameAssembly.dll`. To detect PUN, scan the IL2CPP metadata for the canonical entry points:

```powershell
$meta = '<Game>_Data\il2cpp_data\Metadata\global-metadata.dat'
$s = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($meta))
foreach ($k in 'PhotonNetwork','LoadBalancingClient','LoadBalancingPeer','ServerSettings') {
    if ($s -match [regex]::Escape($k)) { "FOUND: $k" } else { "miss : $k" }
}
```

If `PhotonNetwork` AND `LoadBalancingClient` are both present (and `NetworkRunner` is not), it's PUN. If `NetworkRunner` is present, it's Fusion 2 — use [`photon_fusion`](../photon_fusion/).

If the game has no `il2cpp_data/` folder but has `<Game>_Data\Managed/` with .NET DLLs, it's Mono — use [`photon_pun_mono`](../photon_pun_mono/).

## Build

```powershell
msbuild plugins\photon_pun\photon_pun_plugin.vcxproj `
  -p:Configuration=Release -p:Platform=x64 -m

Copy-Item plugins\photon_pun\relbuild\x64\photon_pun.dll `
  C:\path\to\TheGame\plugins\ -Force
```

## What the plugin actually does at runtime

1. Hooks `Photon.Realtime.LoadBalancingPeer.OpAuthenticate` and `OpAuthenticateOnce` — rewrites the `appId` string argument and forces `AuthenticationValues.authType = 0 (Custom)`.
2. Hooks `ExitGames.Client.Photon.PhotonPeer.SendOperation` — for every auth-bearing op (codes 220, 226, 230, 231), reads/rewrites the params dict's entry `224` (`ApplicationId`) and `217` (`ClientAuthenticationType`). This is critical because PUN's `OpGetRegions` (op 220) sends an *empty string* for ApplicationId, which Photon NameServer rejects.
3. Hooks `Photon.Realtime.AuthenticationValues.set_AuthType` — diagnostics + defense-in-depth.

All hooks installed via MinHook against IL2CPP-resolved native method pointers.

## Limitations

- Players need to share the same Photon AppId.
- The Cloudflare Worker accepts every auth request — don't reuse it for real projects.
- Photon free tier has CCU limits (20 CCU).
- Game updates can break the static edit (re-run `Setup.bat`).
- Anti-cheat blocks this — don't bother with EAC/BattlEye-protected titles.
- This plugin only works on IL2CPP-compiled Unity games. For Mono games, use [`photon_pun_mono`](../photon_pun_mono/).
- For *deeply* obfuscated games whose own UI gates multiplayer on auth state (e.g. Phasmophobia with Beebyte), this plugin won't reach its hooks because the game bails before Photon is contacted.
