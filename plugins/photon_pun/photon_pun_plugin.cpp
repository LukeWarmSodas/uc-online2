// ============================================================
// UCOnline2 plugin -- Photon PUN universal redirect
//
// Sibling of plugins/photon_fusion. Same idea, different
// middleware: PUN (Photon Unity Networking) is the older
// Photon SDK that predates Fusion 2. PUN is the API of choice
// for many Unity multiplayer games (Phasmophobia, lots of
// social VR titles, etc.).
//
// Strategy:
//
//   1. **Static edit of <game>_Data/resources.assets** rewrites
//      the embedded ServerSettings.AppSettings.AppIdRealtime
//      GUID to the user's app. See Set-PhotonAppId.ps1.
//
//   2. **LoadBalancingPeer.OpAuthenticate / OpAuthenticateOnce**
//      hooks are the wire-time override. We rewrite:
//        - the `appId` string-pointer parameter (defense in
//          depth in case the static edit missed an AppId
//          stored elsewhere), and
//        - the `authValues.authType` byte at offset 0x10,
//          which forces Photon's master to use our chosen
//          custom-auth handshake regardless of what the game
//          assigned earlier.
//
//   3. **AuthenticationValues.set_AuthType** hook intercepts
//      every property-setter call and forces AuthType to the
//      configured value. IL2CPP inlines past this in most
//      games, so it's diagnostics + defense in depth -- the
//      OpAuthenticate hook is the one that actually wins.
//
// Configuration (next to the game exe, union-crax.ini):
//
//   [PUN]
//   PhotonAppIdRealtime=<your photon app's GUID>
//   ForcedAuthType=0    (Custom; pair with permissive Custom
//                        Authentication URL on Photon)
//                  255  (None; pair with no providers + anon)
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
static char  g_OurAppIdUtf8[64]    = {};
static void* g_OurAppIdString      = nullptr;  // Il2CppString*
static bool  g_AppIdPatchEnabled   = false;

// CustomAuthenticationType byte enum:
//   Custom = 0, Steam = 1, Facebook = 2, ..., None = 255.
static unsigned int g_ForcedAuthType = 0;

// Confirmed identical to Fusion: AuthenticationValues lives
// in Photon.Realtime and is a sealed class whose authType byte
// sits at +0x10. Same layout used in PUN since they share the
// realtime client.
static const size_t kOffsetAuthValues_authType = 0x10;

#define LOG(...) do { if (g_Log) g_Log(__VA_ARGS__); } while (0)

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
    if (!fn) { LOG("[PUN] IL2CPP: could not find %s", logName); return false; }
    if (MH_CreateHook(fn, hook, orig) != MH_OK || MH_EnableHook(fn) != MH_OK)
    {
        LOG("[PUN] hook FAILED for %s", logName);
        return false;
    }
    LOG("[PUN] %s hook installed at %p", logName, fn);
    return true;
}

// ============================================================
// Hook 1: AuthenticationValues.set_AuthType (defense in depth)
// ============================================================
typedef void (__fastcall *Fn_AuthValues_set_AuthType)(void* pThis, unsigned int value);
static Fn_AuthValues_set_AuthType g_pfnOrigAuthValuesSetAuthType = nullptr;

static void __fastcall Hooked_AuthValues_set_AuthType(void* pThis, unsigned int value)
{
    g_pfnOrigAuthValuesSetAuthType(pThis, g_ForcedAuthType);
}

// ============================================================
// Hook 2 (PRIMARY): LoadBalancingPeer.OpAuthenticate
//
// PUN's wire-send. Signature (Photon.Realtime, v4.x):
//   bool OpAuthenticate(string appId, string appVersion,
//                       AuthenticationValues authValues,
//                       string regionCode, bool getLobbyStatistics)
//
// We rewrite:
//   - appId               -> our managed Il2CppString
//   - authValues.authType -> g_ForcedAuthType byte
// ============================================================
typedef bool (__fastcall *Fn_OpAuthenticate)(
    void* pThis, void* appId, void* appVersion, void* authValues,
    void* regionCode, bool getLobbyStatistics);
static Fn_OpAuthenticate g_pfnOrigOpAuthenticate = nullptr;

typedef bool (__fastcall *Fn_OpAuthenticateOnce)(
    void* pThis, void* appId, void* appVersion, void* authValues,
    void* regionCode, int encryptionMode, int expectedProtocol);
static Fn_OpAuthenticateOnce g_pfnOrigOpAuthenticateOnce = nullptr;

// Make sure we have a managed Il2CppString of our AppId GUID
// ready to substitute. Called lazily on the main thread.
static void EnsureOurAppIdString()
{
    if (g_OurAppIdString || !g_AppIdPatchEnabled) return;
    g_OurAppIdString = IL2CPP_StringNew(g_OurAppIdUtf8);
    if (g_OurAppIdString)
        LOG("[PUN] built managed string for '%s' at %p",
            g_OurAppIdUtf8, g_OurAppIdString);
}

static void* PatchAppIdArg(void* appId, const char* sender)
{
    if (!g_AppIdPatchEnabled) return appId;
    EnsureOurAppIdString();
    if (!g_OurAppIdString) return appId;
    if (appId != g_OurAppIdString)
        LOG("[PUN] %s: appId arg %p -> %p", sender, appId, g_OurAppIdString);
    return g_OurAppIdString;
}

static void PatchAuthValuesAuthType(void* authValues, const char* sender)
{
    if (!authValues) return;
    unsigned char* p = (unsigned char*)authValues + kOffsetAuthValues_authType;
    unsigned char prev = *p;
    *p = (unsigned char)(g_ForcedAuthType & 0xFF);
    LOG("[PUN] %s: authValues.authType %u -> %u", sender, prev, g_ForcedAuthType);
}

static bool __fastcall Hooked_OpAuthenticate(
    void* pThis, void* appId, void* appVersion, void* authValues,
    void* regionCode, bool getLobbyStatistics)
{
    appId = PatchAppIdArg(appId, "OpAuthenticate");
    PatchAuthValuesAuthType(authValues, "OpAuthenticate");
    return g_pfnOrigOpAuthenticate(pThis, appId, appVersion, authValues,
                                    regionCode, getLobbyStatistics);
}

static bool __fastcall Hooked_OpAuthenticateOnce(
    void* pThis, void* appId, void* appVersion, void* authValues,
    void* regionCode, int encryptionMode, int expectedProtocol)
{
    appId = PatchAppIdArg(appId, "OpAuthenticateOnce");
    PatchAuthValuesAuthType(authValues, "OpAuthenticateOnce");
    return g_pfnOrigOpAuthenticateOnce(pThis, appId, appVersion, authValues,
                                        regionCode, encryptionMode, expectedProtocol);
}

// ============================================================
// PhotonPeer.SendOperation hook -- the lowest-level wire send.
//
// PUN's OpGetRegions (opCode 220) sends an EMPTY string for
// ApplicationId (params[224]). Photon's NameServer rejects with
// InvalidAuthentication when the AppId is missing/wrong. Asset-
// side patching only affects values PUN reads at static init,
// not what OpGetRegions sends.
//
// We rewrite params[224] (ApplicationId) and params[217]
// (ClientAuthenticationType) on every auth-bearing op. Same
// fix that unblocked R.E.P.O. on the Mono side, here for
// IL2CPP games like Phasmophobia.
// ============================================================
typedef bool (__fastcall *Fn_SendOperation)(void* pThis, uint8_t opCode, void* params,
                                            void* sendOptions, void* a5, void* a6);
static Fn_SendOperation g_pfnOrigSendOperation = nullptr;

static bool __fastcall Hooked_SendOperation(void* pThis, uint8_t opCode, void* params,
                                             void* sendOptions, void* a5, void* a6)
{
    const char* opName = "?";
    bool isAuth = false;
    switch (opCode) {
        case 220: opName = "GetRegions/Auth"; isAuth = true; break;
        case 226: opName = "GetRegions";      isAuth = true; break;
        case 230: opName = "Authenticate";    isAuth = true; break;
        case 231: opName = "AuthenticateOnce";isAuth = true; break;
        case 248: opName = "JoinRoom";        break;
        case 252: opName = "SetProperties";   break;
        case 253: opName = "RaiseEvent";      break;
        case 254: opName = "Leave";           break;
    }
    LOG("[PUN] SendOperation opCode=%u (%s) params=%p", opCode, opName, params);

    if (isAuth && params)
    {
        char buf[256];
        Il2CppObject* cur = IL2CPP_DictByteGetItem((Il2CppObject*)params, 224);
        IL2CPP_DescribeObject(cur, buf, sizeof(buf));
        LOG("[PUN] SendOperation: BEFORE params[224] (AppId) = %s", buf);

        if (g_AppIdPatchEnabled && g_OurAppIdUtf8[0])
        {
            if (IL2CPP_DictByteStringSetItem((Il2CppObject*)params, 224, g_OurAppIdUtf8))
                LOG("[PUN] SendOperation: rewrote params[224] AppId -> %s",
                    g_OurAppIdUtf8);
        }
        if (g_ForcedAuthType <= 255)
        {
            if (IL2CPP_DictByteByteSetItem((Il2CppObject*)params, 217,
                                            (uint8_t)(g_ForcedAuthType & 0xFF)))
                LOG("[PUN] SendOperation: rewrote params[217] AuthType -> %u",
                    g_ForcedAuthType);
        }
    }

    return g_pfnOrigSendOperation(pThis, opCode, params, sendOptions, a5, a6);
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

    // AuthenticationValues.set_AuthType -- diagnostics + defense in depth.
    InstallIl2CppHook(
        "Photon.Realtime", "Photon.Realtime", "AuthenticationValues",
        "set_AuthType", 1,
        (void*)&Hooked_AuthValues_set_AuthType,
        (void**)&g_pfnOrigAuthValuesSetAuthType,
        "AuthenticationValues.set_AuthType");

    // LoadBalancingPeer.OpAuthenticate -- THE wire-time override.
    InstallIl2CppHook(
        "Photon.Realtime", "Photon.Realtime", "LoadBalancingPeer",
        "OpAuthenticate", -1,
        (void*)&Hooked_OpAuthenticate,
        (void**)&g_pfnOrigOpAuthenticate,
        "LoadBalancingPeer.OpAuthenticate");

    InstallIl2CppHook(
        "Photon.Realtime", "Photon.Realtime", "LoadBalancingPeer",
        "OpAuthenticateOnce", -1,
        (void*)&Hooked_OpAuthenticateOnce,
        (void**)&g_pfnOrigOpAuthenticateOnce,
        "LoadBalancingPeer.OpAuthenticateOnce");

    // PhotonPeer.SendOperation -- the wire-time fix that unblocked
    // R.E.P.O. for the Mono variant. Same root cause on IL2CPP:
    // OpGetRegions sends an empty AppId in params[224] and Photon
    // NameServer rejects with InvalidAuthentication. We rewrite
    // it here mid-wire.
    if (!InstallIl2CppHook(
        "Photon3Unity3D", "ExitGames.Client.Photon", "PhotonPeer",
        "SendOperation", -1,
        (void*)&Hooked_SendOperation,
        (void**)&g_pfnOrigSendOperation,
        "PhotonPeer.SendOperation"))
    {
        InstallIl2CppHook(
            "Photon3Unity3D", "ExitGames.Client.Photon", "PeerBase",
            "SendOperation", -1,
            (void*)&Hooked_SendOperation,
            (void**)&g_pfnOrigSendOperation,
            "PeerBase.SendOperation");
    }
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
        LOG("[PUN] GameAssembly.dll never resolved -- giving up on IL2CPP hooks");
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

    LOG("[PUN] plugin v1 init: AppId=%u ogAppId=%u",
        g_ForcedAppId, g_OriginalAppId);

    const char* ini = GetIniPath();
    if (ini)
    {
        GetPrivateProfileStringA("PUN", "PhotonAppIdRealtime", "",
                                 g_OurAppIdUtf8,
                                 sizeof(g_OurAppIdUtf8), ini);
        if (g_OurAppIdUtf8[0])
        {
            g_AppIdPatchEnabled = true;
            LOG("[PUN] PhotonAppIdRealtime override set: %s", g_OurAppIdUtf8);
        }
        else
        {
            LOG("[PUN] no [PUN]PhotonAppIdRealtime in ini -- AppId patch disabled");
        }

        char authTypeBuf[8] = {};
        GetPrivateProfileStringA("PUN", "ForcedAuthType", "0",
                                 authTypeBuf, sizeof(authTypeBuf), ini);
        unsigned int parsed = (unsigned int)strtoul(authTypeBuf, nullptr, 10);
        if (parsed <= 255) g_ForcedAuthType = parsed;
        LOG("[PUN] forced AuthType = %u", g_ForcedAuthType);
    }

    if (MH_Initialize() != MH_OK)
    {
        LOG("[PUN] MH_Initialize failed");
        return 3;
    }

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
    LOG("[PUN] plugin shutdown");
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
