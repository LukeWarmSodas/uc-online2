# photon_fusion plugin

**Universal Photon Fusion 2 multiplayer redirect for Unity IL2CPP games.**

A UCOnline2 plugin that gets multiplayer working for any Unity IL2CPP game built on Photon Fusion 2 whose matchmaking is gated behind Steam-backed custom authentication. It redirects the game from the developer's Photon Cloud app to one you control, then forces the wire-time auth type so Photon's master accepts the client without a publisher Steam key.

## Confirmed working

| Game | Steam AppId | Notes |
|---|---|---|
| Outbound | 2681030 | First game this was built and tested against |

If you get it working on another game, send a PR — the plugin should "just work" for any Photon Fusion 2 title.

## How it works (one paragraph)

Outbound (and any game using Photon Fusion 2) embeds the developer's Photon Cloud app GUID inside its Unity assets, plus references the same GUID in IL2CPP-compiled C# code. The plugin replaces both: a one-time edit of `<Game>_Data/*.assets` swaps the disk-side copy, and runtime IL2CPP hooks rewrite any cached or per-call copies before they hit the network. The hooks all target Photon Fusion library code (`PhotonAppSettings`, `AuthenticationValues`, `LoadBalancingPeer`) which is identical across every game using this middleware — that's why the same DLL works for multiple games.

## Setup walkthrough

### 1. Create your Photon Fusion 2 app

1. Sign up at <https://dashboard.photonengine.com/> (free, no card).
2. Click **Create a new app** → type **Fusion** → name it anything.
3. Copy the **AppId** GUID, e.g. `4ff936cd-afb9-486b-b8e3-6ab23d915af0`.

### 2. Create a permissive auth backend

The game sends an `AuthType=Steam` ticket on connect. Photon would try to validate it via Steam Web API — which fails because we don't have the developer's publisher key. The fix is to point Photon at a **Custom Authentication URL** you own that just always replies success.

Cloudflare Workers is the easiest way:

1. <https://workers.cloudflare.com> → sign up (free).
2. **Workers & Pages → Create application → Create Worker → Deploy**.
3. Click **Edit code** and paste:

   ```js
   export default {
     async fetch() {
       return new Response(JSON.stringify({
         ResultCode: 1,
         UserId: "anon-" + crypto.randomUUID(),
         Nickname: "Player"
       }), { headers: { "Content-Type": "application/json" } });
     }
   };
   ```

4. **Deploy** and copy the resulting URL (e.g. `https://my-fusion-auth.<handle>.workers.dev`).

### 3. Wire the Worker into Photon

In Photon dashboard for your app:

1. **Manage → Authentication**.
2. Remove any existing Steam provider.
3. **Add Provider → Custom**.
4. Paste the Worker URL into the **Authentication URL** field.
5. Leave the mandatory key/value pairs empty.
6. **Reject Clients on Authentication Failure: UNCHECKED**.
7. Save.

### 4. Static-edit the game's bundled AppId

Run the included PowerShell script from this folder:

```powershell
# Preview what it would change
.\Set-PhotonAppId.ps1 -GameDir 'C:\path\to\TheGame' -DryRun

# Apply
.\Set-PhotonAppId.ps1 -GameDir 'C:\path\to\TheGame' `
                       -NewAppId '4ff936cd-afb9-486b-b8e3-6ab23d915af0'
```

The script:
- Locates the `<Game>_Data` directory automatically.
- Scans every `.assets` file inside it for the `"PhotonAppSettings"` marker.
- Locates the 36-char ASCII GUID near it (length-prefixed at `0x24 00 00 00`).
- Backs up affected files (`.assets.bak`).
- Replaces the GUID in place.

To roll back: `.\Set-PhotonAppId.ps1 -GameDir '...' -Revert`.

If the script reports "No Photon Fusion GUID found", the game either doesn't use Photon Fusion 2 or its asset serialization layout differs from what the script expects.

### 5. Configure UCOnline2's ini

`union-crax.ini` next to the game exe:

```ini
[Settings]
AppId=480
ogAppId=<the game's real Steam AppId>
PluginsFolder=plugins
GetStubbedLol=false

[Fusion]
PhotonAppIdFusion=4ff936cd-afb9-486b-b8e3-6ab23d915af0
ForcedAuthType=0
```

- `PhotonAppIdFusion` — the same GUID you used in the static edit. The plugin uses this as defense-in-depth.
- `ForcedAuthType=0` — `Custom` (matches the Custom Auth URL provider you set up in step 3). Use `255` for `None` if you want pure anonymous instead.

### 6. Build and drop in the plugin

```powershell
msbuild plugins\photon_fusion\photon_fusion_plugin.vcxproj `
  -p:Configuration=Release -p:Platform=x64 -m
```

Copy the result into the game:

```powershell
Copy-Item plugins\photon_fusion\relbuild\x64\photon_fusion.dll `
  C:\path\to\TheGame\plugins\ -Force
```

(Create that `plugins\` folder if it doesn't exist.)

### 7. Test

Run the game and trigger multiplayer (hosting, "Show Multiplayer Code", whatever the game calls it). The code should appear within a few seconds. Photon's dashboard should show CCU going from 0 → 1.

Tail the log for `[Fusion]` lines:

```powershell
Get-Content "$env:TEMP\uc_online2.log" -Wait -Tail 30 |
  Select-String '\[Fusion\]'
```

Useful entries:

- `PhotonAppIdFusion override set: …` — config picked up.
- `LoadBalancingPeer.OpAuthenticate hook installed at …` — primary hook in place.
- `OpAuthenticate: authValues.authType 1 -> 0` — wire-time override fired. **This is the line that proves it works.**

## Trying it on a new game

If a Steam game has `Fusion.Realtime.dll` somewhere in its `<Game>_Data\` tree, it almost certainly uses Photon Fusion 2 and this plugin should work for it:

```powershell
Get-ChildItem -Recurse -Filter 'Fusion.Realtime.dll' 'C:\path\to\TheGame'
```

Test in order:

1. Run `Set-PhotonAppId.ps1 -DryRun` to confirm the script finds the embedded GUID.
2. Static-edit + drop the plugin + configure ini.
3. Launch; check log for the `OpAuthenticate: authType X -> Y` line.

If everything fires but you still see an error or it hangs:

- Check Photon dashboard CCU. If CCU goes up but the game errors, the issue is Photon-side config (provider mismatch, region disabled, etc.).
- If CCU stays at 0, the game has cached the AppId somewhere the script missed. Look for additional `*.assets` files with the developer's GUID.

PRs adding games to the "Confirmed working" table welcome. Just send a screenshot of the multiplayer code generating + the relevant log lines.

## How the plugin works (in detail)

Four hooks, all on Photon Fusion library code (so they don't change per game):

1. **`PhotonAppSettings.get_Global`** — rewrites the cached singleton's `AppIdFusion` on every call. Redundant with the static edit but keeps things working if the asset file wasn't patched.

2. **`AuthenticationValues.set_AuthType`** — intercepts the property setter and forces `g_ForcedAuthType`. Doesn't catch IL2CPP-inlined direct field writes, but catches Photon-internal AuthValues constructions and serves as a safe main-thread trampoline for triggering the `get_Global` hook.

3. **`LoadBalancingPeer.OpAuthenticate`** (and `OpAuthenticateOnce`) — **the primary working override.** This is the method Photon's master-server connection uses to send the authentication operation on the wire. We rewrite `authValues.authType` (offset 0x10) just before forwarding to the original. This catches the AuthType regardless of how the game set it earlier, including IL2CPP-inlined assignments.

For a different middleware (PUN, FishNet, Mirror, Unity Netcode), you'd need a different plugin — the technique is the same (find the wire-send method, patch the parameter byte) but the class/method names are different.

## Limitations

- **Players need to share the same Photon AppId.** Anyone using a different Photon app — including the official one — won't be on the same cluster.
- **The Cloudflare Worker accepts every authentication request.** Don't reuse the same Photon AppId for any project that needs real auth.
- **Photon Fusion's free tier has CCU limits** (20 CCU at the time of writing). Fine for friend groups, not for public servers.
- **Game updates can break the static edit.** If a developer changes the embedded AppId GUID, the script will need a re-run (it finds it dynamically, so usually just works again).
- **Anti-cheat blocks this.** Don't bother trying on EAC/BattlEye-protected games.
