# Outbound plugin

Reference UCOnline2 plugin (see `include/uco_plugin.h`) demonstrating two patching strategies that this kind of EOS-gated game needs:

1. **Steam-side hooks**: synthesize a Steam ticket with `ogAppId` embedded, bypass `BeginAuthSession`, force auth callbacks to OK. Lives in `outbound_plugin.cpp`.
2. **EOS-side hooks**: intercept `EOS_Connect_Login` / `EOS_Auth_Login` and short-circuit them to success. Lives in `outbound_plugin.cpp`.
3. **IL2CPP runtime patching**: resolve specific C# methods in `GameAssembly.dll` by name and replace their compiled bodies. Reusable runtime resolver in `il2cpp_runtime.{h,cpp}`.

Layers 1 and 2 we have done already and they install cleanly. They are NOT what makes the multiplayer flow work for this game — confirmed by the fact that even with both layers active, "Failed(2): Ticket for other app" still appears. The validation that produces that error lives in Outbound's own compiled C# code, which is what layer 3 targets. OnlineFix's per-game `Custom.dll` does exactly the same thing (we confirmed by tracing its dynamic API resolution — it pulls in the `il2cpp_*` family of functions, not any Steam or EOS functions).

## Finding the method to hook

Custom.dll's exact byte-level patch into Outbound is unknown to us, but the IL2CPP API gives us a name-based lookup that is resilient across game updates. The workflow:

1. Download **Il2CppDumper** from <https://github.com/Perfare/Il2CppDumper>.
2. Run it against this install:
   ```
   Il2CppDumper.exe \
     "<game>\GameAssembly.dll" \
     "<game>\Outbound_Data\il2cpp_data\Metadata\global-metadata.dat" \
     dump_out
   ```
3. Open `dump_out/dump.cs`. This file contains every C# class and method in Outbound, with their original names.
4. Find the multiplayer / ticket-validation method. Likely keywords to grep:
   - `"Failed(0)"`, `"Failed({0})"` — the format string
   - `"Ticket"`, `"MultiplayerCode"`, `"JoinCode"`, `"SteamMatchmaking"`, `"AuthTicket"`
   - The class is probably named something like `SteamMultiplayerManager`, `LobbyConnection`, `MultiplayerAuth`.
5. Note its fully qualified path: assembly image (usually `Assembly-CSharp`), namespace, class name, method name, and arg count.

## Adding the hook

Once the target method is identified, edit `outbound_plugin.cpp` → `TryInstallIl2CppHooks()`:

```cpp
typedef bool (__fastcall *Fn_ValidateTicket)(void* pThis, void* ticket);
static Fn_ValidateTicket g_pfnOrigValidateTicket = nullptr;

static bool __fastcall Hooked_ValidateTicket(void* pThis, void* ticket)
{
    LOG("[Outbound] ValidateTicket bypass -> returning true");
    return true;  // or call original then mutate result
}

static void TryInstallIl2CppHooks()
{
    if (!IL2CPP_IsReady()) return;
    static bool done = false;
    if (done) return;

    void* target = IL2CPP_FindMethodPtr(
        "Assembly-CSharp", "Outbound.Multiplayer",
        "SteamMultiplayerManager", "ValidateTicket", 1);
    if (!target) return;

    MH_CreateHook(target, &Hooked_ValidateTicket,
                  (void**)&g_pfnOrigValidateTicket);
    MH_EnableHook(target);
    LOG("[Outbound] ValidateTicket hook installed at %p", target);
    done = true;
}
```

The signature (`__fastcall`, params, return type) must match what IL2CPP generated for that method. Il2CppDumper's output shows the C# signature; the native calling convention on Windows x64 is always Microsoft x64 (`__fastcall` in MSVC speak), but instance methods take `this` as the first argument.

## Build

```powershell
msbuild plugins\outbound\outbound_plugin.vcxproj `
  -p:Configuration=Release -p:Platform=x64 -m
```

Output: `plugins\outbound\relbuild\x64\outbound.dll`. Drop into `<game>\plugins\outbound.dll`.

## Recommended approach: Photon AppId override

Outbound's multiplayer auth is gated by **Photon Fusion custom auth**: the master server forwards the Steam ticket to Outbound's backend, which validates via Steam Web API and rejects tickets signed for AppId 480 (Spacewar) instead of 2681030. Suppressing the failure callback hides the UI error but the connection is still dead.

The clean workaround is to **redirect the game to a Photon Fusion app you control**, where custom auth is disabled (or trivially permissive). Then there is no rejection. Your players will see only other players on the same redirected app — which for a closed friend group is exactly what you want.

### Steps

1. Sign up at <https://dashboard.photonengine.com/> (free).
2. Create a new app, type **Fusion**.
3. Copy the AppId GUID it gives you.
4. Add it to your `union-crax.ini`:

   ```ini
   [Settings]
   AppId=480
   ogAppId=2681030
   PluginsFolder=plugins
   GetStubbedLol=false

   [Outbound]
   PhotonAppIdFusion=<your-guid-here>
   ```

5. On next launch, the plugin hooks `Fusion.Photon.Realtime.PhotonAppSettings.get_Global` and overwrites the embedded `FusionAppSettings.AppIdFusion` with your GUID. The log will show `[Outbound] AppId patch: FusionAppSettings.AppIdFusion replaced`.

If `PhotonAppIdFusion` isn't set in the ini, the AppId hook is not installed and the plugin behaves as before (auth still fails on Outbound's app).

## Limitations

- IL2CPP hooks are sensitive to the C# method's argument layout. If the game updates and changes the method signature (added/removed an argument), the hook must be updated.
- The method name is what we rely on; if the developer renames it, lookup fails and the plugin logs `method not found`.
- IL2CPP's MethodInfo layout has been mostly stable since Unity 2018+; this plugin only touches the first field (`methodPointer`), which has not moved.
