# Outbound plugin

Reference UCOnline2 plugin that gets Outbound's "Show Multiplayer Code" feature working when you don't own the game on Steam. It's also a worked example of the general technique for any Unity game built on Photon Fusion 2 that gates multiplayer behind Steam-backed custom authentication.

The TL;DR is:

1. Stand up your own Photon Fusion Cloud app.
2. Redirect Outbound to use your app's GUID instead of the developer's.
3. Use a permissive auth backend (a free Cloudflare Worker) so Photon doesn't try to validate Steam tickets we can't sign properly.

You and any friend who follows the same setup with the **same Photon GUID** will be on the same Photon cluster and can host / join each other's sessions.

## Setup walkthrough

### 1. Create your Photon Fusion 2 app

1. Sign up at <https://dashboard.photonengine.com/> (free, no card).
2. Click **Create a new app** → type **Fusion** → name it anything (e.g. `outbound-friends`).
3. Copy the **AppId** GUID it gives you, e.g. `4ff936cd-afb9-486b-b8e3-6ab23d915af0`.

### 2. Create a permissive auth backend

The game sends an `AuthType=Steam` ticket on connect. Photon will try to validate it via Steam Web API — which fails because we don't have Outbound's publisher key. The easiest fix is to point Photon at a **Custom Authentication URL** you own that just always replies success.

A Cloudflare Worker is the easiest way:

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

4. **Deploy** and copy the resulting URL (e.g. `https://outbound-auth.<your-handle>.workers.dev`).

### 3. Wire the Worker into Photon

In Photon dashboard for your app:

1. **Manage → Authentication**.
2. Remove any existing Steam provider.
3. **Add Provider → Custom**.
4. Paste the Worker URL into the **Authentication URL** field.
5. Leave the mandatory key/value pairs empty.
6. **Reject Clients on Authentication Failure: UNCHECKED**.
7. Save.

### 4. Static-edit Outbound's bundled AppId

Run the included PowerShell script (from this `plugins/outbound/` folder):

```powershell
.\Set-OutboundPhotonAppId.ps1 `
  -GameDir 'C:\Users\YOU\Downloads\Outbound' `
  -NewAppId '4ff936cd-afb9-486b-b8e3-6ab23d915af0'
```

It backs up `Outbound_Data\resources.assets` to `resources.assets.bak`, then replaces the developer's embedded Photon AppId GUID (`cffbe809-5036-43d8-84a1-7bf16c924721`) with yours in place.

If Outbound updates and the GUID-replacement no longer applies, the script will tell you — at that point the stock GUID has changed and the script needs updating.

To roll back: `.\Set-OutboundPhotonAppId.ps1 -GameDir '...' -Revert`.

### 5. Configure UCOnline2's ini

`union-crax.ini` next to `Outbound.exe`:

```ini
[Settings]
AppId=480
ogAppId=2681030
PluginsFolder=plugins
GetStubbedLol=false

[Outbound]
PhotonAppIdFusion=4ff936cd-afb9-486b-b8e3-6ab23d915af0
ForcedAuthType=0
```

- `PhotonAppIdFusion` — same GUID as the static edit. The plugin uses this as defense-in-depth in case the static edit isn't applied.
- `ForcedAuthType=0` — `Custom` (matches the Custom Auth URL provider you set up in step 3). Use `255` for `None` if you want pure anonymous instead.

### 6. Build and drop in the plugin

```powershell
msbuild plugins\outbound\outbound_plugin.vcxproj `
  -p:Configuration=Release -p:Platform=x64 -m
```

Copy the result into the game:

```powershell
Copy-Item plugins\outbound\relbuild\x64\outbound.dll `
  C:\Users\YOU\Downloads\Outbound\plugins\ -Force
```

(Create that `plugins\` folder if it doesn't exist.)

### 7. Test

Run the game, click **Show Multiplayer Code**. The code should display within a few seconds. Photon's dashboard should show CCU going from 0 → 1 while you're in the session.

Tail the log for `[Outbound]` lines:

```powershell
Get-Content "$env:TEMP\uc_online2.log" -Wait -Tail 30 |
  Select-String '\[Outbound\]'
```

Useful entries:

- `PhotonAppIdFusion override set: …` — config picked up.
- `LoadBalancingPeer.OpAuthenticate hook installed at …` — primary hook in place.
- `OpAuthenticate: authValues.authType 1 -> 0` — wire-time override fired (Steam → Custom). **This is the line that proves it works.**

## How the plugin works

The plugin hooks the IL2CPP runtime to override two things at the latest possible moment:

1. **AppIdFusion** — patched both at file level (the script) and at runtime when `PhotonAppSettings.get_Global` is called.
2. **AuthValues.authType** — patched by hooking `LoadBalancingPeer.OpAuthenticate`. IL2CPP inlines the game's own `authValues.AuthType = Steam` assignments past the C# property setter, so we have to intercept the actual wire-send call and rewrite the byte directly on the AuthValues object before serialization.

For a different game on the same stack (Unity + Photon Fusion 2 + Steam custom auth), the same two hooks are likely all that's needed — only the GUID lookup in the static asset edit script changes.

## Limitations

- **Players need to share the same Photon AppId.** Anyone using a different Photon app — including the official Outbound app — won't be on the same cluster and won't see your sessions.
- **The Cloudflare Worker accepts every authentication request.** Don't reuse the same Photon AppId for any project that needs real auth.
- **Photon Fusion's free tier has CCU limits** (20 CCU at the time of writing). Plenty for friend groups, not for public servers.
- **Game updates may break the static edit.** If the developer changes the embedded AppId GUID, edit `Set-OutboundPhotonAppId.ps1` to point at the new stock GUID.
