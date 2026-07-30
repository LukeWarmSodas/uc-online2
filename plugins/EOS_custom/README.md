# EOS_custom plugin

**Gets multiplayer working in Epic Online Services (EOS) games running under UCOnline2, by pointing the game at a free Epic app *you* own and logging in anonymously.**

Co-op then runs over **Epic's own relay** — no LAN, no VPN, and no EOS emulator.

## The problem this solves

EOS games on Steam typically log into EOS Connect using a **Steam session ticket**. Epic validates that ticket *server-side* against Steam, which always fails when the ticket comes from an emulator. The game's own log shows it plainly:

```
LogOnlineIdentity: STEAM: Obtained steam authticket
LogEOS: Error: External credential 'STEAM_SESSION_TICKET' failed to authenticate
        with EOS Connect: EOS_Connect_ExternalTokenValidationFailed
```

With no EOS identity there is no `ProductUserId`, so the session layer refuses to host:

```
LogEOS: Error: HostingPlayerNum provided to CreateSession does not have online identity.
```

No session is created, so nothing is advertised to Steam. The symptoms look unrelated to auth, which makes this easy to misdiagnose:

- Steam's **"Join Game"** button never appears on your friends list
- the joiner sees no lobby / can't join, or fails during map transition
- the multiplayer menu may still open and even list servers, because that part is local

## The fix

Two hooks on the **genuine Epic** `EOSSDK-Win64-Shipping.dll`:

1. **`EOS_Platform_Create`** — rewrites `ProductId`, `SandboxId`, `DeploymentId`, `ClientId` and `ClientSecret` to **your** Epic app, so every player lands in the same session pool.
2. **`EOS_Connect_Login`** — replaces the doomed `STEAM_SESSION_TICKET` credential (type 18) with **`DEVICEID_ACCESS_TOKEN`** (type 10). Device ID login is anonymous: there's no external platform for Epic to validate, so it succeeds and yields a real `ProductUserId`.

Device ID login requires a device id to already exist, so the plugin calls `EOS_Connect_CreateDeviceId` once on the first attempt and lets the game's own login retry loop (these games re-attempt every few seconds) succeed on a later pass.

## Confirmed working

| Game | Steam AppId | Notes |
|---|---|---|
| **Forever Skies** | 1641960 | Redpoint EOS Online Framework (`RedpointSteam` OSS). Uses EOS **Sessions**. Remote co-op over Epic's relay. |
| **Palworld** | 895620 | Dual-stack Steam + EOS with its own integration, built on EOS **Lobbies**. Verified on build `0.7.3.90464`. |

These two use EOS very differently on top of the same login — Redpoint Sessions vs. raw Lobbies — and both are fixed by the same two hooks, because the hooks are on the **EOS SDK itself**, not on any game code. Expect this to work for most EOS titles that log in with a platform (Steam) credential.

Both games happen to ship a **byte-identical** Epic SDK build (1.15.5), which is also why the struct offsets below are safe for them. A game shipping a much newer SDK is the main thing worth re-verifying.

## Setup

### 1. Create a free Epic app

At <https://dev.epicgames.com/portal/> → create a Product. From **Product Settings** collect:

- **ProductId**, **SandboxId**, **DeploymentId**
- **ClientId** and **ClientSecret** (Clients → add a client; the secret is shown once)

Make sure the client's **Client Policy** permits what the game uses — Connect, plus P2P / Lobbies / Sessions.

> **Do NOT add an Identity Provider.** Every provider in that list (Steam, Epic, …) exists to validate a *real* platform account and would need that platform's Web API key. Device ID login deliberately bypasses all of it.

### 2. Restore the genuine Epic EOS SDK

The plugin hooks the **real** Epic SDK. If you previously dropped in an EOS *emulator* (e.g. Nemirtingas), put the game's original `EOSSDK-Win64-Shipping.dll` back — hooking an emulator does nothing, because it never contacts Epic.

Note the SDK may live in a subfolder, e.g. Forever Skies:
`ProjectZeppelin\Binaries\Win64\RedpointEOS\EOSSDK-Win64-Shipping.dll`

### 3. Deploy

- Build (see below) and drop `EOS_custom.dll` into `<game>\plugins\`
- Drop UCOnline2's `steam_api64.dll` into the game's Steam-loading location (back up the original)

### 4. Configure `union-crax.ini`

Next to the game's shipping exe:

```ini
[Settings]
AppId=480
ogAppId=<the game's real Steam AppId>
PluginsFolder=plugins
GetStubbedLol=false

[EOS]
ProductId=<your ProductId>
SandboxId=<your SandboxId>
DeploymentId=<your DeploymentId>
ClientId=<your ClientId>
ClientSecret=<your ClientSecret>
DisplayName=YourName
```

**Every player uses the same five IDs and a different `DisplayName`.** Same IDs = same session pool; the display name seeds a distinct identity.

### 5. Launch

- The **real Steam client must be running and signed in** (UCOnline2 is a passthrough emulator).
- Some games need to be **run as administrator** — Forever Skies does.

## Verify

```powershell
Get-Content "$env:TEMP\uc_online2.log" -Wait -Tail 30 | Select-String '\[EOSAuth\]'
```

- `[EOSAuth] EOS_Platform_Create ... -> REDIRECTED to Product=… Sandbox=… Deployment=…` — platform pointed at your app
- `[EOSAuth] EOS_Connect_Login: ... Credentials.Type=18 (STEAM_SESSION_TICKET) ... -> REWROTE to DEVICEID_ACCESS_TOKEN` — **this is the line that proves the fix**
- `[EOSAuth] Requesting EOS_Connect_CreateDeviceId ...` — first-run device id creation

The real confirmation is in the **game's own log**, which is far more informative than `uc_online2.log` for anything session-related:

```
%LOCALAPPDATA%\<GameName>\Saved\Logs\<GameName>.log
```

`EOS_Connect_ExternalTokenValidationFailed` and `does not have online identity` should both be **gone**.

## Build

```powershell
msbuild plugins\EOS_custom\EOS_custom_plugin.vcxproj -p:Configuration=Release -p:Platform=x64 -m
```

Output: `plugins\EOS_custom\relbuild\x64\EOS_custom.dll`. Single-source; MinHook statically linked.

## Notes and limitations

- **Your Epic app is doing the hosting.** Free-tier limits apply, and you can only play with people using your IDs — you won't see legitimate players (that's deliberate: it keeps you off the publisher's backend).
- Anonymous Device ID identities are per-machine. Deleting the local device id means a new `ProductUserId`.
- Struct offsets are pinned to the **EOS SDK 1.15.x** x64 ABI (`ProductId` +16, `SandboxId` +24, `ClientCredentials` +32, `DeploymentId` +80). A future SDK could reorder these; re-verify if a game ships something much newer.
- If a game's own code *also* validates the platform identity (not just EOS), it may need extra handling.
- The hooks install from `DllMain` because the EOS SDK usually loads before `UCO_PluginInit`; a fallback logger writes to `%TEMP%\uc_online2.log` until the host's logger is available.
