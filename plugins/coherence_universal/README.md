# coherence_universal

Gets past **coherence Cloud**'s platform login for games built on the
[coherence](https://coherence.io) Unity SDK.

## Confirmed working

| Game | Steam AppId | SDK | Notes |
|---|---|---|---|
| **Vampire Survivors** | 1794680 | coherence 1.6 (IL2CPP) | Lobby created on our own coherence project, 2026-08-09. Also needs the runtime-key asset patch below. |

## The wall

coherence authenticates with a platform credential. On Steam that is
`POST /login/steam` carrying a Steam auth session ticket, which coherence
validates **server-side** against the publisher's project config (their Steam
publisher key and real AppId). An emulated ticket minted for the spoofed AppId
is rejected outright:

```
path=/login/steam method=POST statusCode=400
errorCode=InvalidCredentials
-> "Online Services are not available at the moment."
```

Not a weak check — it is judging a credential we cannot forge, the same class of
wall as Photon's custom auth and EOS's `STEAM_SESSION_TICKET`.

## The lever

coherence exposes anonymous login as a first-class API, so we redirect to it:

```csharp
public static LoginOperation LoginWithSteam(string ticket, string identity, CancellationToken ct)
public static LoginOperation LoginAsGuest(CancellationToken ct)
```

The plugin hooks three layers, whichever the game happens to use:

- `Coherence.Cloud.AuthClient.LoginWithSteam` (low level)
- `Coherence.Cloud.CoherenceCloud.LoginWithSteam` (static facade)
- the game's own login entry point (for Vampire Survivors,
  `VampireSurvivors.CoherenceLoginModule.Login`)

**Hook the game's entry point, not the SDK's.** On Vampire Survivors both
coherence-level hooks installed cleanly and never fired — the game reaches login
by a path neither covers, and `dump.cs` shows signatures but not bodies, so the
callee cannot be read off statically. Replacing `CoherenceLoginModule.Login` and
driving the game's own `OnCompleteLogin` with a guest `LoginOperation` worked
first try, because whatever `Login` called internally no longer runs.

## Configuration — `[Coherence]` in `union-crax.ini`

| Key | Default | Meaning |
|---|---|---|
| `ForceGuestLogin` | `true` | Redirect the Steam login to guest login. |
| `ProjectId` | *(unset)* | Override `RuntimeSettings.projectID`. |
| `RuntimeKey` | *(unset)* | Override `RuntimeSettings.runtimeKey`. **See the warning below.** |
| `LocalMode` | `false` | Flip `localDevelopmentMode` and use a replication server on localhost — no Cloud, no account, no schema upload. |
| `LaunchReplicationServer` | `true` | With `LocalMode`, start the game's own `replication-server.exe`. |

## Two traps worth knowing

**Do not hook trivial getters in an IL2CPP build.** `get_RuntimeKey` and friends
are one-line accessors that load a string field; every identically-shaped getter
compiles to the same code and the linker folds them into ONE function. Hooking
that address hooks *every* string getter in the game — Unity's own parameter
names and asset paths came back as the runtime key, and the game hung during
addressable loading. Write the field instead (offsets from the il2cpp dump), and
sanity-check that the slot already holds a readable string before writing.

**The runtime key cannot be patched at runtime at all.** coherence captures it
during `CoherenceBridge` init, roughly a second *before* UCOnline2 loads plugins
(which happens at `SteamAPI_Init`, after Unity has booted). Patching the field,
and even patching the string's characters in place, both left requests going to
the publisher's project. It has to be patched in the game data:

```
VampireSurvivors_Data/globalgamemanagers.assets
```

Runtime keys are 32 hex characters, so it is a same-length byte replace — no
offsets shift. Back up the file first.

### Reproducing the patch

**1. Find the game's current runtime key.** The easiest way is to let the plugin
tell you: put *any* 32-character value in `[Coherence] RuntimeKey`, run the game
once, and read `%TEMP%\uc_online2.log`:

```
[Coherence] RuntimeKey patched in place (was "1b82a78f1d0d48519d6be40c39af5162")
```

The value in brackets is the publisher's key. (The in-place patch itself does
not survive — coherence has already copied the key by then — but it is a
reliable way to *read* it.)

**2. Find which file holds it.** For a Unity game it is normally
`<Game>_Data/globalgamemanagers.assets`, but check rather than assume:

```bash
cd "<game>/<Game>_Data"
grep -lc "1b82a78f1d0d48519d6be40c39af5162" *.assets globalgamemanagers 2>/dev/null
```

**3. Replace it.** Same length in, same length out — verify both before writing:

```python
import io, os, shutil

f   = "globalgamemanagers.assets"
old = b"1b82a78f1d0d48519d6be40c39af5162"   # the publisher's key
new = b"fce1ea692a854b50b9f945ef6aa17758"   # yours, from the coherence dashboard
assert len(old) == len(new), "lengths must match or every following offset shifts"

data = io.open(f, "rb").read()
assert data.count(old) == 1, "expected exactly one occurrence"

if not os.path.exists(f + ".uco2.bak"):
    shutil.copyfile(f, f + ".uco2.bak")

io.open(f, "wb").write(data.replace(old, new))
```

The two asserts are the whole safety story: equal lengths means no offset in the
asset file moves, and exactly one occurrence means you are not overwriting
something that merely looks similar. Restore from `.uco2.bak` to undo.

**Redo this after any game update** — a patched `globalgamemanagers.assets` is
replaced wholesale when the game is updated.

## Shared project (no setup)

If you would rather not stand up your own project, a shared one is available for
**Vampire Survivors** — the schema is already uploaded and all regions are
enabled, so it works out of the box:

```
runtime key: fce1ea692a854b50b9f945ef6aa17758
```

Patch that into `VampireSurvivors_Data/globalgamemanagers.assets` in place of the
32-character key already there (see below) — or just answer `SHARED` when
`patch.bat` asks for a runtime key, and it does this for you.

Schemas currently uploaded to it:

| Build | Schema |
|---|---|
| 1.15.114 | `34701e1e7101dd9c6d0b5379d57936799ce0dc1c` |
| 23591499 | `64649e4d63da323108ba010763b14bed03a075ae` |

A coherence project holds several schemas at once, so one key covers multiple
game builds. A build whose schema is not listed will authenticate and then fail
with `SchemaNotFound` — the runtime key and the schema are separate things, and
a game update changes the schema while leaving the key alone.

**Availability is not guaranteed.** It runs on a free coherence tier, it is not
monitored, and it may be rate-limited, rotated or taken down at any time without
notice — if co-op stops working, assume that is why and set up your own project
using the steps below. Everyone sharing it also shares its quota, and everyone on
it can see everyone else's lobbies.

Only the *runtime key* is published here. It is a client-side identifier that
ships inside every coherence game by design, in the same way a Photon AppId
does. A project's **portal/service tokens are a different thing entirely** and
must never be put in a config, a README or a release — they grant management
access to the project.

## Using your own coherence project

1. Create a project; note its **runtime key**.
2. Patch that key into the game data (above).
3. Upload the game's schema to your project. The Hub uploads
   `Toolkit.schema + Gathered.schema + activeSchemas + extraSchemas`, hashed as
   `sha1(string.Join("\n", contents))`. Since a shipped game's `combined.schema`
   already *contains* the toolkit components as an exact prefix, split it: write
   everything after the toolkit prefix — **minus the single separator newline,
   which `string.Join` supplies** — as `Assets/coherence/Gathered.schema`, then
   upload without baking. Verify the dashboard shows the id the client asks for.
4. Enable **all regions** on the project, or lobby creation fails with
   `LobbyRegionNotFound`.

Note `activeSchemas` comes from `ProjectSettings.instance.activeSchemas`, *not*
`RuntimeSettings.schemas` — editing the latter has no effect on the upload.

## Local mode

`LocalMode=true` skips coherence Cloud entirely: it flips
`RuntimeSettings.localDevelopmentMode` and starts the replication server the game
already ships, pointed at the game's own schema. No project, no key, no upload,
no auth. Untested at the time of writing, but it needs none of the Cloud setup
above and the pieces are all present in a shipped build:

```
StreamingAssets/replication-server.exe   rooms --schema combined.schema
StreamingAssets/combined.schema
```

Setting `RuntimeSettings.localHost` to a remote address would extend this to
internet play against a self-hosted server.
