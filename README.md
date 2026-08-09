# uc-online2

Custom modified Steam API .dll for Steam games to spoof your game as Spacewar. Drop-in replacement for `steam_api.dll` / `steam_api64.dll`.

## Quick start — `patch.bat`

**Drag your game folder onto `patch.bat`.** It works out what the game needs and
does the whole setup, which for most games is everything you have to do.

It will:

- find the engine (Unity or Unreal) and the game's real executable
- find where `steam_api64.dll` actually lives and install ours **there**, backing
  up the original to `.bak` first
- write `union-crax.ini` next to the **running exe** — for Unreal that is not the
  game folder, and an ini in the wrong place is silently ignored
- detect **Photon**, **EOS**, **PlayFab** and **coherence**, copy the matching
  plugin, and prompt for whatever app IDs that backend needs

Everything third-party stays yours: it never invents a Photon GUID, an Epic app
or a coherence project. Press Enter at any prompt to skip it and a stub is
written for you to fill in later.

```
patch.bat "C:\Games\SomeGame"            full setup
patch.bat "C:\Games\SomeGame" /keyonly   coherence runtime key only, nothing else
```

### coherence games: the easy route

A coherence game has to point at a coherence project that has its schema
uploaded. Setting that up yourself means an account, the Unity editor and a
schema upload — so patch.bat offers a shortcut.

**Answer `SHARED` at the runtime-key prompt.** That points the game at a
community project which already has the schema uploaded and every region
enabled: no account, no Unity, nothing else to do.

It is a free tier, unmonitored, and shared with everyone else using it, so
**availability is not guaranteed** — it may be rate-limited or rotated without
notice, and everyone on it sees everyone else's lobbies. If co-op stops working,
suspect that first and set up your own project with
[`tools/coherence_schema`](tools/coherence_schema/README.md).

`/keyonly` changes which project a game points at and nothing else — handy for
switching between your own project and the shared one without disturbing a
working install. It does **not** deploy the emulator or the plugin, so for a
fresh install do a normal full run and answer `SHARED` there instead. Running it
twice is safe; the second run reports the key already matches.

**What it will not do:**

- Install into a 32-bit game. It refuses rather than writing an x64 DLL where it
  cannot load.
- Deploy `EOS_custom` until you supply an Epic app — an inert plugin only adds a
  variable while you are working out whether co-op runs over plain Steam.
- Upload a coherence schema. That needs the Unity editor; see
  [`tools/coherence_schema`](tools/coherence_schema/README.md).

If it reports **no secondary backend**, try the game with no plugin at all —
titles whose multiplayer is purely Steam lobbies and P2P work through
passthrough unmodified.

Steam still has to be running, at the same elevation as the game — see
[Administrator / elevation](#administrator--elevation).

## Usage (manual)

__**If using downloaded .dlls from [Releases](https://github.com/LukeWarmSodas/uc-online2/releases):**__
- 1. Extract the archive downloaded from the __**LATEST**__ release.
- 2. Copy the corresponding .dll to replace your original .dll.
   - 2a. Rename the original .dll before copying it to something else if you feel you must back it up, something like ``steam_api_o.dll`` as Goldberg Emu suggests or ``steam_api64.dll.old``. (It doesn't matter as long as it is just changed.)
- 3. Make sure Steam is running first. Then try running the game as you normally would from the .exe. If it has SteamStub, use Steamless to remove it or use the .dll made to bypass it for games that Steamless cannot unpack. (Dave the Diver is an example.)
   - 3a. If it throws an error related to auth failure, restart Steam and try again. If the error persists, contact me. I'll work with you to figure it out one way or another. 
   - 3b. If the game won't launch at all, see [Administrator / elevation](#administrator--elevation) — Steam and the game have to run at the same elevation. If the game needs admin, run Steam as admin too.

__**If using self built .dlls:**__
- 1. Run `build.bat` or open `uc_online2.vcxproj` in Visual Studio.
- 2. Copy the output .dll to your game folder:
   - **32-bit:** `build\x86\steam_api.dll`
   - **64-bit:** `build\x64\steam_api64.dll`
- 3. Replace your `steam_api(64).dll` with one from here. Back it up if necessary by renaming it to `steam_api(64)_o.dll`.

## Administrator / elevation

UCOnline2 is a **passthrough** — the real Steam client has to be running, and the game talks to it over Steam's IPC. That IPC is sensitive to Windows integrity levels, so **run Steam and the game at the same elevation.**

**What matters is that they match** — either is fine as long as both are the same:

| Steam | Game | Result |
|---|---|---|
| normal | normal | ✅ works — the usual setup |
| **admin** | **admin** | ✅ works — use this when the game needs admin |
| normal | admin | ✅ Most of the time works |
| admin | normal | ❌ Won't work since game can't hook into steam |

A mismatch typically looks like the game refusing to start at all, an init/auth failure, or the game behaving as though Steam isn't running.

**Some games genuinely require admin.** Those aren't broken — you just have to bring Steam up to match:

1. Fully exit Steam (tray icon → Exit — not just closing the window).
2. Right-click `steam.exe` → **Run as administrator**, and let it finish signing in.
3. Launch the game as admin as usual.

If a game *doesn't* need admin, leave both normal; it's the simpler setup and there's nothing to gain from elevating.

Two things that catch people out:

- Windows silently elevates a child process when its parent is elevated — launching the game from an elevated launcher, script, or terminal elevates the game too, even with every compatibility checkbox clear.
- Closing Steam's window only hides it to the tray. If you're switching Steam's elevation you have to actually **Exit** it first, or you'll just reattach to the still-running non-elevated instance.

## Configuration

Create `union-crax.ini` next to the game executable to change your AppId as needed. If this file is missing, AppId defaults to `480` and plugins are not loaded. `PluginsFolder` is relative to the game executable or wherever it's set in the .ini. Or should be. I haven't tested it yet. Check the `steam_appid.txt` file that gets created upon running the game to check if your set AppId was accepted. For games that have `480` patched in

Set `WarnOverlayDisabled=true` to log a startup warning when the Steam overlay
will not work. See [Steam overlay](#steam-overlay) below.
the game's code, try setting it to something else free that's multiplayer, like `440`
(Team Fortress 2). Shapes of Dreams did not work using `480`, but worked fine with `440`.
((THANK YOU to deityofsukana for helping figure that out for certain!!!))

```ini
[Settings]
AppId=480
ogAppId=220 # Half-life 2
PluginsFolder=plugins
GetStubbedLol=false
UnlockDLC=123,456,789 # Legacy DLC list; prefer the [DLC] section below
EmulateTicket=true  # Enable ticket emulation using ogAppId or AppId
```

## Unlock DLC

Games ask about DLC in two different ways, and both have to be answered:

- **"Do I own AppId 211?"** — `BIsSubscribedApp` / `BIsDlcInstalled`
- **"List my DLC"** — `GetDLCCount` + `BGetDLCDataByIndex`

The second kind is why unlocking used to look unreliable: a game that checks ids it
already knows worked, while a game that builds its DLC menu by *enumerating* saw
nothing, because real Steam answers for the spoofed AppId and reports no DLC.

Use the `[DLC]` section:

```ini
[DLC]
UnlockAll=1              ; answer "owned" for any DLC id the game asks about

; Named entries are what enumeration can report. Needed by games that build a
; DLC list from the API rather than checking ids they already know.
211=Half-Life 2: Deathmatch
212=Half-Life 2: Lost Coast
```

`UnlockAll=1` handles every ownership *question* without you knowing any ids, but it
cannot invent a *list* — only named entries appear in `GetDLCCount` /
`BGetDLCDataByIndex`. So if DLC still doesn't show up in-game, add the entries
explicitly; the ids are on SteamDB under the game's DLC tab.

The old comma-separated form still works and is merged in (those entries enumerate
as `DLC <id>`, since they carry no name):

```ini
[Settings]
UnlockDLC=211,212,213,218
```

To check what was picked up, look in `%TEMP%/uc_online2.log` for:

```
[UCOnline2] DLC store: UnlockAll=1, 2 entries
[UCOnline2] ISteamApps DLC hooks: 5/5 installed (UnlockAll=1, 2 configured DLC)
```

## Emulate Encrypted App Ticket

The `EmulateTicket` setting covers **both** kinds of Steam ticket a game may ask for:

- **Encrypted app ticket** — `GetEncryptedAppTicket` / `RequestEncryptedAppTicket`, plus `UserHasLicenseForApp` ownership checks, answered for `ogAppId` (or `AppId` if unset).
- **Auth session ticket** — `GetAuthSessionTicket`, plus the legacy `InitiateGameConnection` client/server handshake used by older Steam integrations. Previously these passed straight through to real Steam, which mints the ticket under the *spoofed* AppId, so the host rejected it. The tell in the log is the game registering callback `163` (`GetAuthSessionTicketResponse_t`) repeatedly: asking for a ticket, never getting a usable one, retrying.

`BeginAuthSession` / `EndAuthSession` / `CancelAuthTicket` are emulated to match, and the response callbacks (`GetAuthSessionTicketResponse_t`, `ValidateAuthTicketResponse_t`) are delivered — a game that *waits* on those would otherwise hang even with a valid-looking ticket.

This works because the check is **peer-side**: both players run the emulator, so one mints the ticket and the other accepts it. It does **not** help where a publisher's own server asks Steam to validate the ticket — that is server-side and unforgeable, no matter what the client does.

Look for this in `%TEMP%/uc_online2.log`:

```
[UCOnline2] auth ticket emulation: 6/6 hooks installed
[UCOnline2] GetAuthSessionTicket emulated -> handle=1 appid=2300320 size=64
[UCOnline2] BeginAuthSession emulated -> OK for 7656119... (64 byte ticket)
```

I really didn't know how to actually go about this, sorry - I used AI to try and finish what I had gotten through with it. I don't know how OFME utilizes it, but I can assume it actually is a ticket emulation system which is what I tried to make. But, I don't want it to _not_ work due to it being emulated, or too emulated, if this makes sense at all. I don't want to deviate too much and have this not work at all.
Speaking of which, I can't say for certain if this function would actually work or not, I will rely on the community to find out about that from testing for me. Sorry to throw that on y'all like this. ^^;

Example:
```ini
[Settings]
AppId=480
ogAppId=440  # Used for ticket emulation (falls back to AppId if not set)
EmulateTicket=true
```

## Steam overlay

**To get Shift+Tab and the Steam invite UI, add the game to Steam and launch it
from there** — Games > *Add a Non-Steam Game* > pick the game's `.exe`, then run
it from your library. Confirmed working (Plague Inc: Evolved).

The emulator behaves identically either way; it does not care who started the
process. But the overlay does.

### Why launching directly cannot work

The overlay is Valve's own `GameOverlayRenderer(64).dll`. UCOnline2 loads it into
the process and sets the environment it expects — but **Steam only arms the
overlay for a process it launched itself**. Started directly, Steam never spawns
`gameoverlayui64.exe` for the session and never authorises it, so the renderer
sits in the process with nothing to display and no input hook. That decision is
made inside the Steam client; no environment variable reaches it.

This is why `Shift+Tab` does nothing and invite dialogs come up blank when the
game is launched straight from its folder.

### What UCOnline2 does do

Once Steam *has* launched the game, Steam sets the overlay environment for the
**real** AppId. Since we then spoof the AppId, `SteamOverlayGameId` has to be
re-pointed at the spoofed one (see [ogAppId](#ogappid)) or the overlay attaches
to a game id the Steam client has no session for. UCOnline2 also fills in the
rest of what Steam normally exports — `SteamClientLaunch`, `SteamPath`,
`SteamAppUser` — before loading the renderer, since the overlay reads them once
on attach.

### Diagnosing it

The startup log states the outcome directly:

```
[UCOnline2] Loaded game overlay: C:\...\GameOverlayRenderer64.dll
[UCOnline2]   env: SteamGameId=480 SteamClientLaunch=1 SteamPath=... user=<account>
[UCOnline2] Overlay: renderer module loaded, launched-by-Steam=1 (IsOverlayEnabled=...)
```

`launched-by-Steam=0` means the overlay will not work, whatever else looks right —
add it as a Non-Steam Game. Note that `IsOverlayEnabled` reads `0` for the first
few seconds even on a healthy launch, because the overlay is still attaching; it
is reported for completeness and is not a verdict on its own.

## What this cannot fix

Some games are out of reach no matter what the emulator does, and it is worth
knowing the shape of that before spending an evening on one.

**Servers that verify a Steam app-ownership ticket.** The game asks Steam for an
auth session ticket and sends it to the publisher, whose server checks Valve's
**128-byte signature** on it. That signature cannot be produced without Valve's
private key, so no amount of client-side work passes the check. `EmulateTicket`
produces a structurally correct ticket, which is enough for a peer that runs this
same emulator, and never enough for a server that validates properly.

Confirmed on **Farming Simulator 25** by hooking `GetAuthSessionTicket` inside a
competing fix and reading the bytes it produces: its ticket is a **genuine,
Valve-signed ticket belonging to a different Steam account** — one that actually
owns the game — generated on a server weeks earlier and replayed by every user of
that fix. It works because the credential is real, not because the check is weak.

How to recognise it early:

- the game registers callback `163` (`GetAuthSessionTicketResponse_t`) and the
  connection fails a second or two later — a round trip, not a local error
- a structurally valid emulated ticket still gets rejected

**Related walls**, all server-side and all unforgeable: a publisher backend that
allow-lists its own application ID, an account service that mints its own session
token from a platform ticket, and kernel-level anti-cheat. If ownership is proven
to a machine you do not control, the answer is no.

**What still works:** everything validated *peer-side*. Games where multiplayer is
Steam lobbies plus P2P, or where the host itself validates the joining player, are
the normal case and are what this project is for.

## Plugin Loader / Injector

If `PluginsFolder` is set in the .ini file, all `.dll` files in that folder are loaded at startup in alphabetical order. Use prefixes to control load order:

```
plugins/
  01_first_plugin.dll
  02_second_plugin.dll
  03_another_one_(dj_khaled!!).dll
```

### Plugin ABI (v1)

Plugins that want to integrate with UCOnline2 (rather than just running DllMain side effects) can export two C functions defined in [`include/uco_plugin.h`](include/uco_plugin.h):

```c
__declspec(dllexport) int  UCO_PluginInit(const UCO_PluginContext* ctx);
__declspec(dllexport) void UCO_PluginShutdown(void);
```

- `UCO_PluginInit` is called once after `SteamAPI_Init` succeeds. The context exposes the resolved `ISteam*` interfaces, the configured AppId / ogAppId, a logger, and a callback-patcher registration function. Return 0 for success.
- `UCO_PluginShutdown` is called on detach (in reverse load order). Use it to disable MinHook hooks and clean up.

#### What lives in core vs in a plugin

Core (`steam_api64.dll`) only ships generic Steam-side spoofing:
- `ISteamUtils::GetAppID` and `ISteamApps::BIsSubscribedApp` are vtable-hooked to report `ogAppId` so games that ask "what AppId am I, and do I own it?" get a consistent answer.

Anything game-specific — ticket synthesis, `BeginAuthSession` bypass, EOSSDK hooks, custom networking, IL2CPP patches — belongs in a plugin.

## SteamStubbed

If `GetStubbedLol` is enabled in the .ini file, it will attempt to patch SteamStub on the fly. This is meant for games that Steamless cannot unpack, such as Dave the Diver. However, it can be used to keep from modifying the game files at all, or as little as possible. I'm not responsible for the code, it was used from DenuvoSanctuary's original Rust code, [which can be found here](https://github.com/denuvosanctuary/steamstubbed). I
rewrote it in C++ so I could try integrating it into this project and not need to inject it. I did not ask for permission to use it in any way, so if there are any issues with that, please contact me and I'll remove it or work something out. It's not much of a change anyways, and they're easy to find too.

If the function is disabled, or was never written in the first place, then it simply 
will just ignore the function entirely and continue as it wassn't implemented in the
first place.

## "ogAppId"

This is an attempt to allow the overlay to force use the right game assets even when you very clearly are supposedly running Spacewar. Setting the original AppId here just gets calculated to the 64-bit Game ID string it expects (which I just learned about too...) and is used for the `SteamOverlayGameId` environment variable which could easily be run as a launch arg, but requires you knowing the long string of numbers for your game, so this just makes it way easier to set up. `SteamGameId` is not touched at all by this, as it can cause problems. It uses the `AppId` for that, except it also gets converted to the expected 64-bit Game ID string.

## Building

__**Quick way (true Chad way - quick, simple, and easy):**__
- 1. Run `build.bat`.
- 2. ???
- 3. Profit.

__**With Visual Studio (the bum way - requires too much effort):**__
- 1. Open `uc_online2.vcxproj`.
- 2. Select Release | Win32 or Release | x64.
- 3. Build.

Requires Visual Studio 2022 with C / C++ Environment selected (v143 or higher toolset). If MSBuild is not found, `build.bat` will tell you where to get it. The `build.bat` script has a preset path that applies to my personal setup and if it doesn't match yours, you will need to modify it. Otherwise, you'll consistently error out even when you have everything installed. 

## Forking / Modifications 

Okay, so this part I did not cover as of publishing the source files, this will cover personal modifications and forks as well as modifications to this.

- Please feel free to fork this and modify it however you see fit, and if you feel it would benefit the original repo, make a PR and I'll look into it.
- You are allowed to modify this and distribute it to your liking, all I ask is that I'm made aware of this. Read the license for more info. 
   - I only want this so I can be certain that it's not being distributed as a method to spread malware. (I should make the license have something for this, or make it better...)
- Have any ideas but you're not sure how to implement it? Or don't know how to code? Contact me with your ideas. I'll work on it for you and communicate with you throughout the process. 

## Issues?

- No, this will not work with Denuvo protected games. If you think it can, modify it so that it can work like an activated game, but even then I cannot guarantee it will work. It will likely reject you and you will need to get re-activated as your token will be fucked permanently. So basically, __I say just don't even bother. It'll likely waste your time and the activators' time too.__
- As it is right now, DLC you don't own will likely not work - I'll try and add functionality for that in and if it works, then it'll likely work the same as Goldberg does.
- If you're trying this with a game that has the AppId hard coded in (like with Godot games) then you'll need to modify the game to set the AppId to what you need it to be. Though, you won't even need this at all if you do that lol. 
- You cannot join VAC protected servers or servers hosted using the real AppId in Garry's Mod or other Source games or any other games that have similar protections. (GoldSrc games seemingly do not apply, as CS1.6 let me join any servers.) Please do not message me asking why you can't join any servers in Garry's Mod. Instead, ask me how you can play with your friends if they have legitimate copies. :)
- Game won't launch, or behaves like Steam isn't running? Check elevation before anything else — a game running as admin while Steam isn't (or vice versa) will refuse to start. See [Administrator / elevation](#administrator--elevation).
- For any other unexpected or unaccounted for issues, please contact me. I have yet to test this with every game so I will rely on the community to do so. 
