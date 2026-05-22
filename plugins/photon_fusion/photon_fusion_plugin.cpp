// ============================================================
// UCOnline2 plugin -- Photon Fusion 2 universal redirect
//
// Generic plugin against the UCOnline2 v1 plugin ABI (see
// include/uco_plugin.h) that gets multiplayer working for
// any Unity IL2CPP game built on Photon Fusion 2 whose
// auth is gated behind Steam-backed custom authentication.
//
// Works by redirecting the game from the developer's Photon
// Cloud app to one the user controls, then forcing the
// wire-time AuthType so Photon's master accepts the client
// without a publisher Steam key.
//
// Confirmed working on:
//   - Outbound  (Steam AppId 2681030)
//
// To add support for another game: usually nothing here
// changes. You configure your Photon Cloud GUID in
// union-crax.ini and run Set-PhotonAppId.ps1 against the
// game's resources.assets to swap the embedded GUID. The
// hooks below all target Photon Fusion library code that's
// identical across games.
//
// Mechanism (kept minimal after extensive iteration):
//
//   1. **Static edit of <game>_Data/resources.assets** rewrites
//      the embedded PhotonAppSettings.AppIdFusion GUID to the
//      user's app. See Set-PhotonAppId.ps1.
//
//   2. **PhotonAppSettings.get_Global** hook (defense-in-depth)
//      patches the cached singleton's AppIdFusion in case the
//      static edit didn't take or wasn't applied.
//
//   3. **AuthenticationValues.set_AuthType** hook intercepts
//      every property-setter call and forces AuthType to a
//      user-configured value. NOTE: IL2CPP inlines many of the
//      game's own assignments past this hook, so it isn't
//      enough on its own.
//
//   4. **LoadBalancingPeer.OpAuthenticate / OpAuthenticateOnce**
//      hooks rewrite the AuthValues.authType byte at offset
//      0x10 right before the auth operation is serialized to
//      the wire. This is the working override that survives
//      IL2CPP inlining -- it operates on the live AuthValues
//      object Photon is about to send.
//
// Configuration is read from union-crax.ini next to the game
// exe:
//   [Fusion]
//   PhotonAppIdFusion=<your photon app's GUID>
//   ForcedAuthType=0   (Custom; pair with a permissive
//                       Custom Authentication URL on Photon)
//                  255 (None; pair with no providers + anon)
//
// MinHook is statically linked into this DLL.
// ============================================================
#include <Windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "../../include/MinHook.h"
#include "../../include/uco_plugin.h"

#include "il2cpp_runtime.h"

// ============================================================
// Plugin-local state -- populated in UCO_PluginInit
// ============================================================
static UCO_LogFn g_Log              = nullptr;
static uint32_t  g_ForcedAppId      = 480;
static uint32_t  g_OriginalAppId    = 0;
static volatile LONG g_bShutdown    = 0;
static HANDLE    g_hWatcherThread   = nullptr;

// Photon AppId override read from union-crax.ini
static char  g_OurFusionAppIdUtf8[64] = {};
static void* g_OurFusionAppIdString    = nullptr;  // Il2CppString*
static bool  g_AppIdPatchEnabled       = false;

// CustomAuthenticationType byte enum:
//   Custom = 0, Steam = 1, Facebook = 2, ..., None = 255.
// Read from [Outbound]ForcedAuthType in union-crax.ini.
static unsigned int g_ForcedAuthType = 0;

// Field offsets confirmed from il2cpp dump
static const size_t kOffsetPhotonAppSettings_AppSettings = 0x20;
static const size_t kOffsetAppSettings_AppIdFusion       = 0x18;
static const size_t kOffsetAuthValues_authType           = 0x10;

#define LOG(...) do { if (g_Log) g_Log(__VA_ARGS__); } while (0)

// IL2CPP_Log shim used by il2cpp_runtime.cpp.
extern "C" void IL2CPP_Log(const char* fmt, ...)
{
    if (!g_Log) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_Log("%s", buf);
}

// ============================================================
// Hook installation helper
// ============================================================
static bool InstallIl2CppHook(const char* image, const char* ns, const char* klass,
                              const char* method, int argc, void* hook, void** orig,
                              const char* logName)
{
    void* fn = IL2CPP_FindMethodPtr(image, ns, klass, method, argc);
    if (!fn) { LOG("[Fusion] IL2CPP: could not find %s", logName); return false; }
    if (MH_CreateHook(fn, hook, orig) != MH_OK || MH_EnableHook(fn) != MH_OK)
    {
        LOG("[Fusion] hook FAILED for %s", logName);
        return false;
    }
    LOG("[Fusion] %s hook installed at %p", logName, fn);
    return true;
}

// ============================================================
// Hook 1: PhotonAppSettings.get_Global
//
// Returns the global singleton. On each call we rewrite the
// embedded AppIdFusion to point at the user's Photon app.
// Acts as defense-in-depth if resources.assets wasn't patched.
// ============================================================
typedef void* (__fastcall *Fn_PhotonAppSettings_get_Global)();
static Fn_PhotonAppSettings_get_Global g_pfnOrigPhotonAppSettingsGetGlobal = nullptr;

static void* __fastcall Hooked_PhotonAppSettings_get_Global()
{
    void* settings = g_pfnOrigPhotonAppSettingsGetGlobal();
    if (!settings || !g_AppIdPatchEnabled) return settings;

    if (!g_OurFusionAppIdString)
    {
        g_OurFusionAppIdString = IL2CPP_StringNew(g_OurFusionAppIdUtf8);
        if (!g_OurFusionAppIdString) return settings;
        LOG("[Fusion] built managed string for '%s' at %p",
            g_OurFusionAppIdUtf8, g_OurFusionAppIdString);
    }

    void** pAppSettingsField = (void**)((char*)settings + kOffsetPhotonAppSettings_AppSettings);
    void* appSettings = *pAppSettingsField;
    if (!appSettings) return settings;

    void** pAppIdFusion = (void**)((char*)appSettings + kOffsetAppSettings_AppIdFusion);
    if (*pAppIdFusion != g_OurFusionAppIdString)
    {
        void* oldStr = *pAppIdFusion;
        *pAppIdFusion = g_OurFusionAppIdString;
        LOG("[Fusion] PhotonAppSettings.AppIdFusion replaced (was %p)", oldStr);
    }
    return settings;
}

// ============================================================
// Hook 2: AuthenticationValues.set_AuthType
//
// Forces the property setter to write our chosen AuthType.
// Doesn't catch IL2CPP-inlined direct field writes, but
// catches everything that goes through the property API
// (Photon's internal AuthValues constructions, etc.) and
// also serves as a safe trampoline for triggering the
// PhotonAppSettings.get_Global hook on the main thread.
// ============================================================
typedef void (__fastcall *Fn_AuthValues_set_AuthType)(void* pThis, unsigned int value);
static Fn_AuthValues_set_AuthType g_pfnOrigAuthValuesSetAuthType = nullptr;

static volatile LONG g_TriedForceGlobal = 0;

static void __fastcall Hooked_AuthValues_set_AuthType(void* pThis, unsigned int value)
{
    if (g_AppIdPatchEnabled
        && InterlockedExchange(&g_TriedForceGlobal, 1) == 0
        && g_pfnOrigPhotonAppSettingsGetGlobal != nullptr)
    {
        LOG("[Fusion] forcing PhotonAppSettings.get_Global to apply AppId patch");
        Hooked_PhotonAppSettings_get_Global();
    }
    g_pfnOrigAuthValuesSetAuthType(pThis, g_ForcedAuthType);
}

// ============================================================
// Hook 3 (THE PRIMARY OVERRIDE): LoadBalancingPeer.OpAuthenticate
//
// This is the wire-send call. The AuthValues argument is the
// object Photon is about to serialize -- we write the authType
// byte at offset 0x10 here, bypassing any IL2CPP inlining that
// happened at C# assignment sites. Whatever value we set
// becomes the AuthType Photon's master sees.
// ============================================================
typedef bool (__fastcall *Fn_OpAuthenticate)(
    void* pThis, void* appId, void* appVersion, void* authValues,
    void* regionCode, bool getLobbyStatistics);
static Fn_OpAuthenticate g_pfnOrigOpAuthenticate = nullptr;

typedef bool (__fastcall *Fn_OpAuthenticateOnce)(
    void* pThis, void* appId, void* appVersion, void* authValues,
    void* regionCode, int encryptionMode, int expectedProtocol);
static Fn_OpAuthenticateOnce g_pfnOrigOpAuthenticateOnce = nullptr;

static void PatchAuthValuesAuthType(void* authValues, const char* sender)
{
    if (!authValues) return;
    unsigned char* p = (unsigned char*)authValues + kOffsetAuthValues_authType;
    unsigned char prev = *p;
    *p = (unsigned char)(g_ForcedAuthType & 0xFF);
    LOG("[Fusion] %s: authValues.authType %u -> %u", sender, prev, g_ForcedAuthType);
}

static bool __fastcall Hooked_OpAuthenticate(
    void* pThis, void* appId, void* appVersion, void* authValues,
    void* regionCode, bool getLobbyStatistics)
{
    PatchAuthValuesAuthType(authValues, "OpAuthenticate");
    return g_pfnOrigOpAuthenticate(pThis, appId, appVersion, authValues,
                                    regionCode, getLobbyStatistics);
}

static bool __fastcall Hooked_OpAuthenticateOnce(
    void* pThis, void* appId, void* appVersion, void* authValues,
    void* regionCode, int encryptionMode, int expectedProtocol)
{
    PatchAuthValuesAuthType(authValues, "OpAuthenticateOnce");
    return g_pfnOrigOpAuthenticateOnce(pThis, appId, appVersion, authValues,
                                        regionCode, encryptionMode, expectedProtocol);
}

// ============================================================
// IL2CPP hook installer -- runs once GameAssembly.dll loads
// ============================================================
static void TryInstallIl2CppHooks()
{
    if (!IL2CPP_IsReady()) return;
    static bool attempted = false;
    if (attempted) return;
    attempted = true;

    // PhotonAppSettings.get_Global -- defense-in-depth for AppId.
    if (g_AppIdPatchEnabled)
    {
        InstallIl2CppHook(
            "Fusion.Realtime", "Fusion.Photon.Realtime", "PhotonAppSettings",
            "get_Global", 0,
            (void*)&Hooked_PhotonAppSettings_get_Global,
            (void**)&g_pfnOrigPhotonAppSettingsGetGlobal,
            "PhotonAppSettings.get_Global");
    }

    // AuthenticationValues.set_AuthType -- property setter override.
    InstallIl2CppHook(
        "Fusion.Realtime", "Fusion.Photon.Realtime", "AuthenticationValues",
        "set_AuthType", 1,
        (void*)&Hooked_AuthValues_set_AuthType,
        (void**)&g_pfnOrigAuthValuesSetAuthType,
        "AuthenticationValues.set_AuthType");

    // LoadBalancingPeer.OpAuthenticate -- THE wire-time override.
    // argCount = -1 to skip the parameter-count filter; some
    // IL2CPP versions count optional params differently.
    InstallIl2CppHook(
        "Fusion.Realtime", "Fusion.Photon.Realtime", "LoadBalancingPeer",
        "OpAuthenticate", -1,
        (void*)&Hooked_OpAuthenticate,
        (void**)&g_pfnOrigOpAuthenticate,
        "LoadBalancingPeer.OpAuthenticate");

    InstallIl2CppHook(
        "Fusion.Realtime", "Fusion.Photon.Realtime", "LoadBalancingPeer",
        "OpAuthenticateOnce", -1,
        (void*)&Hooked_OpAuthenticateOnce,
        (void**)&g_pfnOrigOpAuthenticateOnce,
        "LoadBalancingPeer.OpAuthenticateOnce");
}

static DWORD WINAPI WatcherProc(LPVOID)
{
    for (int i = 0; i < 600 && InterlockedCompareExchange(&g_bShutdown, 0, 0) == 0; ++i)
    {
        if (IL2CPP_TryInit())
        {
            TryInstallIl2CppHooks();
            return 0;
        }
        Sleep(200);
    }
    if (!InterlockedCompareExchange(&g_bShutdown, 0, 0))
        LOG("[Fusion] GameAssembly.dll never resolved -- giving up on IL2CPP hooks");
    return 0;
}

// ============================================================
// Ini reading
// ============================================================
static const char* GetIniPath()
{
    static char path[MAX_PATH] = {};
    static bool computed = false;
    if (computed) return path[0] ? path : nullptr;
    computed = true;

    char exeDir[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    if (len == 0) return nullptr;
    for (int i = (int)len - 1; i >= 0; --i) {
        if (exeDir[i] == '\\' || exeDir[i] == '/') { exeDir[i] = 0; break; }
    }
    int n = _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\union-crax.ini", exeDir);
    if (n <= 0) { path[0] = 0; return nullptr; }
    return path;
}

// ============================================================
// Plugin ABI entry points
// ============================================================
extern "C" __declspec(dllexport) int __cdecl UCO_PluginInit(const UCO_PluginContext* ctx)
{
    if (!ctx) return 1;
    if (ctx->ApiVersion != UCO_PLUGIN_API_VERSION) return 2;

    g_Log            = ctx->Log;
    g_ForcedAppId    = ctx->ForcedAppId;
    g_OriginalAppId  = ctx->OriginalAppId;

    LOG("[Fusion] plugin v1 init: AppId=%u ogAppId=%u",
        g_ForcedAppId, g_OriginalAppId);

    // Read [Fusion] PhotonAppIdFusion from union-crax.ini.
    const char* ini = GetIniPath();
    if (ini)
    {
        GetPrivateProfileStringA("Fusion", "PhotonAppIdFusion", "",
                                 g_OurFusionAppIdUtf8,
                                 sizeof(g_OurFusionAppIdUtf8), ini);
        if (g_OurFusionAppIdUtf8[0])
        {
            g_AppIdPatchEnabled = true;
            LOG("[Fusion] PhotonAppIdFusion override set: %s", g_OurFusionAppIdUtf8);
        }
        else
        {
            LOG("[Fusion] no [Fusion]PhotonAppIdFusion in ini -- AppId patch disabled");
        }

        char authTypeBuf[8] = {};
        GetPrivateProfileStringA("Fusion", "ForcedAuthType", "0",
                                 authTypeBuf, sizeof(authTypeBuf), ini);
        unsigned int parsed = (unsigned int)strtoul(authTypeBuf, nullptr, 10);
        if (parsed <= 255) g_ForcedAuthType = parsed;
        LOG("[Fusion] forced AuthType = %u", g_ForcedAuthType);
    }

    if (MH_Initialize() != MH_OK)
    {
        LOG("[Fusion] MH_Initialize failed");
        return 3;
    }

    // GameAssembly.dll loads later during Unity plugin init.
    // Spin a watcher that installs IL2CPP hooks once it appears.
    g_hWatcherThread = CreateThread(nullptr, 0, WatcherProc, nullptr, 0, nullptr);
    return 0;
}

extern "C" __declspec(dllexport) void __cdecl UCO_PluginShutdown(void)
{
    InterlockedExchange(&g_bShutdown, 1);
    if (g_hWatcherThread)
    {
        WaitForSingleObject(g_hWatcherThread, 1000);
        CloseHandle(g_hWatcherThread);
        g_hWatcherThread = nullptr;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    LOG("[Fusion] plugin shutdown");
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
