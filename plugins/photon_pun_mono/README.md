# photon_pun_mono plugin

**Universal Photon PUN multiplayer redirect for Unity games using the Mono scripting backend.**

Mono-runtime counterpart of [`photon_pun`](../photon_pun/) (which targets IL2CPP builds). Same idea, same hook strategy, different runtime APIs — uses `mono_*` exports from `MonoBleedingEdge/EmbedRuntime/mono-2.0-bdwgc.dll` instead of `il2cpp_*` from `GameAssembly.dll`.

## Quick terminology

- **Photon Realtime SDK** is the low-level networking library. On the [Photon dashboard](https://dashboard.photonengine.com/) you create apps of type **`Realtime`**.
- **PUN (Photon Unity Networking)** is a higher-level Unity wrapper *built on top of* Photon Realtime. A PUN game ships `PhotonUnityNetworking.dll` (PUN) + `PhotonRealtime.dll` (Realtime SDK).
- A PUN game's Photon dashboard app is a **Realtime-type** app — PUN was a separate dashboard product type historically but Photon merged it into Realtime years ago.
- If the game has voice chat (via `PhotonVoice.PUN.dll`), it needs a **second, separate** Photon app of type **`Voice`** with its own AppId GUID.

This plugin targets Mono-compiled Unity games (those with a `MonoBleedingEdge/` folder and `<Game>_Data/Managed/` containing plain .NET DLLs). For IL2CPP-compiled games use [`photon_pun`](../photon_pun/).

## Confirmed working

| Game | Steam AppId | Notes |
|---|---|---|
| **R.E.P.O.** | 3241660 | Requires **two** Photon apps — one Realtime + one Voice. Voice slot is mandatory because R.E.P.O. uses `PhotonVoice.PUN` for proximity chat and Photon's NameServer rejects the connection if the second AppId slot (Voice) holds the game dev's GUID. |

## Quick start

1. Create two free Photon apps at <https://dashboard.photonengine.com/>:
   - One of type **`Realtime`** for the main multiplayer connection.
   - One of type **`Voice`** for voice chat (most PUN-using games need this, including R.E.P.O.).
2. On each app: **Manage → Authentication → Add Provider → Custom**. Paste the same permissive Cloudflare Worker URL on both. **Uncheck "Reject Clients on Authentication Failure"**. Save.
3. **Double-click `Setup.bat`** in this folder. It prompts for: game folder → Realtime GUID → Voice GUID → real Steam AppId. It will:
   - Patch every Photon AppId slot in the game's `resources.assets` (one with your Realtime GUID, the next with your Voice GUID).
   - Drop `photon_pun_mono.dll` into `<game>\plugins\`.
   - Write a complete `union-crax.ini` at the game root.
4. Drop UCOnline2's `steam_api64.dll` into `<game>\<Game>_Data\Plugins\x86_64\` (back up the original first).
5. Launch the game.

If `Setup.bat` prints `GUID NOT FOUND`, the game isn't a Photon PUN target. The DLL won't help on its own.

## Why two Photon apps?

R.E.P.O.'s (and most PUN+Voice games') `resources.assets` embeds **two** Photon AppId GUIDs — one for Realtime, one for Voice. Photon validates each AppId server-side against its product type, so:

- If you put a Realtime AppId in the Voice slot → Photon rejects with `InvalidAuthentication`.
- If you leave the Voice slot pointing at the dev's GUID → Photon rejects same way.

You **must** create both apps and use both GUIDs. The script supports this via `-NewAppId` (Realtime, slot 0) and `-NewVoiceAppId` (Voice, slot 1).

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

- `PhotonAppIdRealtime` — your Photon Realtime app's AppId GUID. The plugin uses it as the runtime override in `OpAuthenticate` and the `SendOperation` params-dict rewrite.
- `ForcedAuthType=0` — `Custom` (matches the Custom Auth provider type on the dashboard). Use `255` for `None` if you want pure anonymous instead.

The Voice GUID doesn't currently have its own ini key — the asset-side patch handles it. The Voice connection itself doesn't go through any of our runtime hooks (voice runs on a separate connection path; the plugin's job is to make the Realtime connection succeed and let PUN handle voice handshake from there).

## Manual script usage

```powershell
# Preview discoveries (shows what's currently in resources.assets)
.\Set-PhotonAppId.ps1 -GameDir 'C:\path\to\TheGame' -DryRun

# Apply with BOTH GUIDs (recommended for PUN+Voice games like R.E.P.O.)
.\Set-PhotonAppId.ps1 -GameDir 'C:\path\to\TheGame' `
                      -NewAppId      '<realtime-app-guid>' `
                      -NewVoiceAppId '<voice-app-guid>'

# Apply with ONE GUID for all slots (works only if the game uses
# Realtime alone, no Voice / Chat / Fusion)
.\Set-PhotonAppId.ps1 -GameDir 'C:\path\to\TheGame' `
                      -NewAppId '<realtime-app-guid>'

# Roll back to backed-up .assets.bak files
.\Set-PhotonAppId.ps1 -GameDir 'C:\path\to\TheGame' -Revert
```

## Trying it on a new game

If the game folder has `MonoBleedingEdge/` AND `<Game>_Data/Managed/` with `PhotonUnityNetworking.dll` + `PhotonRealtime.dll`, it's a Mono PUN target — this plugin should fit:

```powershell
Get-ChildItem 'C:\path\to\TheGame\<Game>_Data\Managed' -Filter 'Photon*.dll'
```

Expected to see at minimum: `PhotonRealtime.dll`, `PhotonUnityNetworking.dll`. If you also see `PhotonVoice.dll` / `PhotonVoice.PUN.dll`, the game uses voice and you'll need a Photon Voice app too.

If the game has `il2cpp_data/Metadata/global-metadata.dat` instead of `Managed/*.dll`, it's IL2CPP — use [`photon_pun`](../photon_pun/) instead.

## Build

```powershell
msbuild plugins\photon_pun_mono\photon_pun_mono_plugin.vcxproj `
  -p:Configuration=Release -p:Platform=x64 -m

Copy-Item plugins\photon_pun_mono\relbuild\x64\photon_pun_mono.dll `
  C:\path\to\TheGame\plugins\ -Force
```

## What the plugin actually does at runtime

1. Resolves Mono runtime functions (`mono_class_from_name`, `mono_compile_method`, `mono_runtime_invoke`, `mono_value_box`, etc.) from `mono-2.0-bdwgc.dll`.
2. Walks Mono assemblies to find `Photon.Realtime.LoadBalancingPeer` and JIT-compiles its `OpAuthenticate` / `OpAuthenticateOnce` to get stable native pointers.
3. Hooks those + `ExitGames.Client.Photon.PhotonPeer.SendOperation` via MinHook.
4. At `OpAuthenticate` wire-send time: rewrites the `appId` string argument and forces `AuthenticationValues.authType = 0 (Custom)`.
5. At `SendOperation`: for every auth-bearing op (codes 220, 226, 230, 231), invokes `Dictionary<byte, object>.set_Item` via `mono_runtime_invoke` to rewrite `params[224]` (ApplicationId) and `params[217]` (ClientAuthenticationType). This is critical because PUN's `OpGetRegions` (op 220) sends an *empty string* for ApplicationId; without this rewrite, Photon NameServer rejects the very first wire packet with `InvalidAuthentication`.

## Limitations

- Same general caveats as `photon_pun`: free-tier Photon CCU limits; game updates can break the static asset edit; anti-cheat blocks this; Cloudflare Worker accepts every auth request (don't reuse for real projects).
- Mono-only — for IL2CPP games use [`photon_pun`](../photon_pun/).
- Requires creating **two** Photon apps (Realtime + Voice) for any game that uses PUN Voice. There's no way around this — Photon validates AppId type server-side.
