# redpoint_steam_lobby plugin

**Fixes remote co-op invites/joins failing on games built with Redpoint's EOS Online Framework running in Steam mode.**

Some Unreal games use [Redpoint's EOS Online Framework](https://docs.redpoint.games/) with the `RedpointSteam` online subsystem — their EOS session is mirrored onto a Steam lobby (a *synthetic session*). When you accept a Steam invite or click **Join Game**, the game resolves it by doing a Steam **lobby search** and mapping the found lobby back to an EOS session handle.

That search inherits Epic's Steam implementation, which calls:

```cpp
ISteamMatchmaking::AddRequestLobbyListDistanceFilter( k_ELobbyDistanceFilterDefault )
```

`Default` = "same region or nearby regions only." So when host and joiner are geographically distant, the host's lobby is **filtered out** of the results, the invite's session ID never resolves, and the join dies with:

```
LogEOS: Error: Received invite from synthetic session, but could not resolve
session ID to session handle. Make sure the session is still publicly advertised.
FSEOSManagerSubsystem::OnSessionUserInviteAccepted - Failed to join the game
```

(It sometimes surfaces instead as *"Failed to join during level transition"* — same resolution path, landing mid-session-rebuild.)

## The fix

Redpoint's [official fix](https://docs.redpoint.games/docs/support/troubleshooting/steaminvite) is an engine source patch (`k_ELobbyDistanceFilterDefault` → `k_ELobbyDistanceFilterWorldwide` in `OnlineSessionAsyncLobbySteam.cpp`). We can't rebuild a shipping game's engine — but UCOnline2 owns `steam_api64.dll`, so this plugin hooks the Steam call directly and rewrites the argument to **Worldwide**. Same effect, no engine rebuild. The host's lobby is then returned regardless of distance and the invite resolves.

## Confirmed working

| Game | Steam AppId | Notes |
|---|---|---|
| Forever Skies | 1641960 | `RedpointSteam` OSS. Remote co-op invites/joins failed with the synthetic-session resolve error until the distance filter was forced Worldwide. |

Should apply to **any** Redpoint-EOS-over-Steam title where two distant players can't resolve each other's invites.

## Setup

1. Build (see below) and drop `redpoint_steam_lobby.dll` into `<game>\plugins\`.
2. Drop UCOnline2's `steam_api64.dll` into the game's Steam-loading location (back up the original).
3. `union-crax.ini` at the game root — no plugin-specific section needed:
   ```ini
   [Settings]
   AppId=480
   ogAppId=<the game's real Steam AppId>
   PluginsFolder=plugins
   GetStubbedLol=false
   ```
4. Launch. **Both players need the plugin** (the fix is on the searching/joining side, but running it on both is harmless and covers whoever initiates).

## Verify

```powershell
Get-Content "$env:TEMP\uc_online2.log" -Wait -Tail 40 | Select-String '\[RedpointSteamLobby\]'
```

- `[RedpointSteamLobby] Hook installed on ISteamMatchmaking::AddRequestLobbyListDistanceFilter …` — hook is in place.
- `[RedpointSteamLobby] Rewriting lobby distance filter 1 -> 3 (Worldwide) …` — fired on the first lobby search. After this, a remote invite/join should resolve.

If you never see the "Rewriting" line, the game didn't do a lobby search (wrong OSS, or the invite path wasn't triggered).

## How it works

- At `UCO_PluginInit`, reads the resolved `AddRequestLobbyListDistanceFilter` pointer straight out of the live `ISteamMatchmaking` vtable (`ctx->pSteamMatchmaking`), slot **9**.
- MinHooks that function body (not the vtable memory — so it's safe against const/shared vtable pages and covers every `ISteamMatchmaking` instance).
- The hook ignores whatever distance the game passed and always forwards `k_ELobbyDistanceFilterWorldwide (3)` to the original.

## Build

```powershell
msbuild plugins\redpoint_steam_lobby\redpoint_steam_lobby_plugin.vcxproj -p:Configuration=Release -p:Platform=x64 -m
```

Output: `plugins\redpoint_steam_lobby\relbuild\x64\redpoint_steam_lobby.dll`. Single-source; MinHook statically linked.

## Limitations

- Only helps games whose invite/join goes through a **Steam lobby search** (Redpoint EOS-over-Steam and similar). It does nothing for games that don't call `AddRequestLobbyListDistanceFilter`.
- Worldwide search can add a few seconds of matchmaking latency vs. region-local (Valve's own note on the enum) — a non-issue for direct friend invites.
- The vtable index (9) is pinned to the current Steamworks `ISteamMatchmaking` layout. If a future SDK reorders those methods, update `kDistanceFilterVtableIndex` in the source.
