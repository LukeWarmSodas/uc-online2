// ============================================================
// UCOnline2 plugin -- Redpoint EOS-over-Steam lobby distance fix
//
// THE BUG (documented by Redpoint themselves):
//   https://docs.redpoint.games/docs/support/troubleshooting/steaminvite
//
//   Games built on Redpoint's EOS Online Framework running in
//   Steam mode (OSS "RedpointSteam") carry their EOS session as
//   a *synthetic session* mirrored onto a Steam lobby. When you
//   accept an invite / click "Join Game", the plugin resolves
//   the invite by doing a Steam lobby search and mapping the
//   found lobby back to an EOS session handle.
//
//   That lobby search inherits Epic's Steam implementation,
//   which calls:
//
//     ISteamMatchmaking::AddRequestLobbyListDistanceFilter(
//         k_ELobbyDistanceFilterDefault )
//
//   Default = "same region or nearby regions only". So when the
//   host and joiner are geographically distant, the host's lobby
//   is filtered OUT of the search results, the invite's session
//   ID never resolves to a handle, and the join dies with:
//
//     LogEOS: Error: Received invite from synthetic session, but
//     could not resolve session ID to session handle. Make sure
//     the session is still publicly advertised.
//     FSEOSManagerSubsystem::OnSessionUserInviteAccepted -
//     Failed to join the game
//
//   (Sometimes it instead surfaces as "Failed to join during
//   level transition" if the join lands mid-session-rebuild --
//   same underlying resolution path.)
//
// THE FIX (same one Redpoint / OFM apply):
//   Force the distance filter to WORLDWIDE so the host's lobby is
//   returned regardless of distance. Redpoint's official fix is an
//   engine source patch (k_ELobbyDistanceFilterDefault ->
//   k_ELobbyDistanceFilterWorldwide in OnlineSessionAsyncLobbySteam.cpp).
//   We can't patch a shipping game's engine, but UCOnline2 already
//   owns steam_api64.dll, so we hook the Steam call directly and
//   rewrite the argument. Same effect, no engine rebuild.
//
// Confirmed needed for:
//   - Forever Skies (RealAppId 1641960, RedpointSteam OSS, remote co-op)
//   ...and should apply to any Redpoint-EOS-over-Steam title where
//   two distant players can't resolve each other's invites.
//
// MinHook is statically linked into this DLL (same as the other
// UCOnline2 plugins). We do NOT touch the vtable memory -- MinHook
// patches the resolved function body, so it's safe against const/
// shared vtable pages and covers every ISteamMatchmaking instance.
// ============================================================
#include <Windows.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

#include "../../include/MinHook.h"
#include "../../include/uco_plugin.h"

// ------------------------------------------------------------
// Constants pulled from the Steam SDK (kept local so this plugin
// stays SDK-independent, per the UCO plugin ABI note).
//
// enum ELobbyDistanceFilter (isteammatchmaking.h):
//   k_ELobbyDistanceFilterClose     = 0
//   k_ELobbyDistanceFilterDefault   = 1
//   k_ELobbyDistanceFilterFar       = 2
//   k_ELobbyDistanceFilterWorldwide = 3   <-- we force this
// ------------------------------------------------------------
static const int k_ELobbyDistanceFilterWorldwide = 3;

// Vtable slot of ISteamMatchmaking::AddRequestLobbyListDistanceFilter.
// Method order in isteammatchmaking.h (0-based, STEAM_CALL_RESULT
// macros are annotations, NOT vtable entries):
//   0 GetFavoriteGameCount
//   1 GetFavoriteGame
//   2 AddFavoriteGame
//   3 RemoveFavoriteGame
//   4 RequestLobbyList
//   5 AddRequestLobbyListStringFilter
//   6 AddRequestLobbyListNumericalFilter
//   7 AddRequestLobbyListNearValueFilter
//   8 AddRequestLobbyListFilterSlotsAvailable
//   9 AddRequestLobbyListDistanceFilter   <-- target
// If a future SDK bump reorders these, update this index.
static const int kDistanceFilterVtableIndex = 9;

// ------------------------------------------------------------
// Plugin-local state
// ------------------------------------------------------------
static UCO_LogFn g_Log = nullptr;
#define LOG(...) do { if (g_Log) g_Log(__VA_ARGS__); } while (0)

// x64 has a single calling convention; `this` arrives in RCX, so a
// plain free function with a leading self pointer matches the
// member function's ABI exactly.
typedef void (*AddDistFilter_t)(void* self, int eLobbyDistanceFilter);
static AddDistFilter_t g_orig_AddDistanceFilter = nullptr;
static void*           g_target                 = nullptr;
static LONG            g_bLoggedOnce            = 0;

static void Hooked_AddRequestLobbyListDistanceFilter(void* self, int eLobbyDistanceFilter)
{
    // Force every lobby search to search worldwide, no matter what
    // the game asked for. This is the entire fix.
    if (InterlockedCompareExchange(&g_bLoggedOnce, 1, 0) == 0)
    {
        LOG("[RedpointSteamLobby] Rewriting lobby distance filter %d -> %d (Worldwide). "
            "Remote invites/joins will now resolve.",
            eLobbyDistanceFilter, k_ELobbyDistanceFilterWorldwide);
    }

    if (g_orig_AddDistanceFilter)
        g_orig_AddDistanceFilter(self, k_ELobbyDistanceFilterWorldwide);
}

// ------------------------------------------------------------
// UCO plugin entry points
// ------------------------------------------------------------
extern "C" __declspec(dllexport) int UCO_PluginInit(const UCO_PluginContext* ctx)
{
    if (!ctx || ctx->ApiVersion != UCO_PLUGIN_API_VERSION)
        return 1; // host too new/old for this plugin

    g_Log = ctx->Log;

    if (!ctx->pSteamMatchmaking)
    {
        LOG("[RedpointSteamLobby] ISteamMatchmaking is null; nothing to hook. "
            "(Is this a matchmaking/lobby game?)");
        return 0; // non-fatal
    }

    // Read the resolved function pointer straight out of the live vtable.
    void** vtable = *reinterpret_cast<void***>(ctx->pSteamMatchmaking);
    if (!vtable)
    {
        LOG("[RedpointSteamLobby] Null vtable on ISteamMatchmaking; aborting.");
        return 0;
    }
    g_target = vtable[kDistanceFilterVtableIndex];
    if (!g_target)
    {
        LOG("[RedpointSteamLobby] Null target at vtable[%d]; aborting.",
            kDistanceFilterVtableIndex);
        return 0;
    }

    // MinHook may already be initialized by the core or an earlier
    // plugin; ALREADY_INITIALIZED is fine to ignore.
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG("[RedpointSteamLobby] MH_Initialize failed: %d", s);
        return 0;
    }

    s = MH_CreateHook(g_target,
                      reinterpret_cast<void*>(&Hooked_AddRequestLobbyListDistanceFilter),
                      reinterpret_cast<void**>(&g_orig_AddDistanceFilter));
    if (s != MH_OK)
    {
        LOG("[RedpointSteamLobby] MH_CreateHook failed: %d", s);
        return 0;
    }

    s = MH_EnableHook(g_target);
    if (s != MH_OK)
    {
        LOG("[RedpointSteamLobby] MH_EnableHook failed: %d", s);
        MH_RemoveHook(g_target);
        g_target = nullptr;
        return 0;
    }

    LOG("[RedpointSteamLobby] Hook installed on ISteamMatchmaking::"
        "AddRequestLobbyListDistanceFilter (vtable[%d] @ %p).",
        kDistanceFilterVtableIndex, g_target);
    return 0;
}

extern "C" __declspec(dllexport) void UCO_PluginShutdown(void)
{
    if (g_target)
    {
        MH_DisableHook(g_target);
        MH_RemoveHook(g_target);
        g_target = nullptr;
        LOG("[RedpointSteamLobby] Hook removed.");
    }
    g_orig_AddDistanceFilter = nullptr;
}

BOOL WINAPI DllMain(HMODULE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls((HMODULE)0);
    return TRUE;
}
