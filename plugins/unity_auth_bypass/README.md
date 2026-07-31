# unity_auth_bypass plugin

**Gets past Unity Gaming Services (UGS) sign-in in Unity IL2CPP games running under UCOnline2.**

Many modern Unity games gate multiplayer behind UGS authentication. The normal flow is:

```csharp
AuthenticationService.Instance.SignInWithSteamAsync(ticket)
```

which internally:

1. fetches a Steam auth ticket via `ISteamUser`,
2. POSTs `{ ticket, appId }` to Unity's UGS backend,
3. UGS asks the Steam Web API *"is this ticket entitled to AppId X?"*,
4. on success → `IsSignedIn = true`, `PlayerInfo` populated, game proceeds,
5. on failure → `SignInFailed`, and the game shows something like *"Failed to get Steam account information"* or *"Not signed into Unity Services"* and refuses to start multiplayer.

Because UCOnline2 spoofs **AppId 480 (Spacewar)**, step 3 always fails: the ticket is for 480, but UGS asks Steam about the game's real AppId.

## The fix

Hook `AuthenticationServiceInternal.SignInWithSteamAsync` and redirect every call to **`SignInAnonymouslyAsync`**. UGS still creates a real, valid player session — it just isn't tied to a Steam account. The game's `IsSignedIn` / `PlayerInfo` checks pass and multiplayer proceeds.

This is the same idea as the EOS Device ID swap in [`EOS_custom`](../EOS_custom/): replace a platform credential the backend will reject with an anonymous one it accepts.

The plugin covers all three overloads the SDK exposes:

```csharp
Task SignInWithSteamAsync(string ticket)
Task SignInWithSteamAsync(string ticket, SignInOptions options)
Task SignInWithSteamAsync(string ticket, string identity, SignInOptions options)
```

## Confirmed working

| Game | Steam AppId | Notes |
|---|---|---|
| **Phasmophobia** | 739630 | Used alongside [`photon_universal`](../photon_universal/) (Phasmo's multiplayer is Photon). Also needs the Phasmo-specific patches below. |

The UGS hook itself is game-agnostic — it targets `Unity.Services.Authentication`, not any game's code — so it should apply to other Unity games that sign into UGS with a Steam ticket.

## Phasmophobia-specific extras

Phasmophobia uses **Beebyte-style obfuscation**, which renames every class and method, so its own Steam gate can't be found by name. The plugin includes two extra, byte-signature-driven patches for it:

1. **Obfuscated `SteamAuth` entry point** — located by scanning `GameAssembly.dll`'s `.text` for a byte signature, then hooked to return null. (Same function OFM patches at RVA `0x42CA4A0` on that build.)
2. **`SteamAccountGate` NOP** — at RVA `0xEAF7CE` in Phasmo build 23249745. The bytes are **verified before patching**, so a different build is skipped rather than corrupted.

Both are safe on non-Phasmo games: if the signature doesn't match, the patch is skipped and logged.

> `photon_universal` bundles an equivalent Phasmo gate NOP, so for Phasmophobia you generally want both plugins present.

## Setup

1. Build (see below) and drop `unity_auth_bypass.dll` into `<game>\plugins\`.
2. Drop UCOnline2's `steam_api64.dll` into `<game>\<Game>_Data\Plugins\x86_64\` (back up the original).
3. `union-crax.ini` at the game root — no plugin-specific section needed:
   ```ini
   [Settings]
   AppId=480
   ogAppId=<the game's real Steam AppId>
   PluginsFolder=plugins
   GetStubbedLol=false
   ```
4. For a Photon game like Phasmophobia, add `photon_universal.dll` and its `[Realtime]` section too.

## Verify

```powershell
Get-Content "$env:TEMP\uc_online2.log" -Wait -Tail 40 | Select-String '\[Auth\]'
```

- `[Auth] SignInWithSteamAsync intercepted -> SignInAnonymouslyAsync` — **the line that proves the fix**.
- `[Auth] Phasmo SteamAuth signature matched at … / hook installed` — Phasmo gate found (Phasmo only).
- `[Auth] Phasmo SteamAuth signature NOT FOUND …` — expected on any non-Phasmo game; harmless.

If you never see the intercept line, the game either isn't using UGS Steam sign-in or hadn't reached it yet.

## Build

```powershell
msbuild plugins\unity_auth_bypass\unity_auth_bypass_plugin.vcxproj -p:Configuration=Release -p:Platform=x64 -m
```

Output: `plugins\unity_auth_bypass\relbuild\x64\unity_auth_bypass.dll`. IL2CPP only; MinHook statically linked.

## Limitations

- **IL2CPP only.** It resolves methods through the IL2CPP runtime (`GameAssembly.dll`); a Mono game needs a different approach.
- Anonymous UGS sign-in means **no Steam-linked identity** — anything the game ties to a Steam-backed UGS account (cross-device progression, friend lookups by Steam ID) won't behave normally.
- The Phasmo byte signature and RVA are pinned to build **23249745**. After a Phasmo update they'll likely stop matching; the plugin logs that and skips rather than misfiring, but the offsets then need re-locating.
- Doesn't help when the backend validates ownership **server-side** in a way anonymous sign-in can't satisfy — that's a different class of wall entirely.
