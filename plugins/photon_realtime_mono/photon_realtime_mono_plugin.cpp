// ============================================================
// UCOnline2 plugin -- Photon PUN (Mono runtime) redirect
//
// Mono-runtime counterpart of plugins/photon_realtime. Unity games
// built with the Mono scripting backend (not IL2CPP) ship the
// runtime as mono-2.0-bdwgc.dll. The C# game code is loaded
// from <Game>_Data/Managed/ as standard .NET assemblies.
//
// Confirmed targets:
//   - R.E.P.O.  (Steam AppId 3241660)
//
// Mechanism is identical to the IL2CPP variant:
//   1. Static edit of resources.assets via Set-PhotonAppId.ps1
//      rewrites the embedded AppIdRealtime GUID.
//   2. LoadBalancingPeer.OpAuthenticate hook is the wire-time
//      override -- we substitute the appId-string argument and
//      force the authValues.authType byte to a user-configured
//      value (Custom=0 by default, matching a permissive Custom
//      Auth URL set up on the Photon dashboard).
//
// Configuration (in union-crax.ini, next to the game exe):
//
//   [Realtime]
//   PhotonAppIdRealtime=<your photon pun app's GUID>
//   ForcedAuthType=0
//
// MinHook is statically linked.
// ============================================================
#include <Windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "../../include/MinHook.h"
#include "../../include/uco_plugin.h"

#include "mono_runtime.h"

// ============================================================
// Plugin-local state
// ============================================================
static UCO_LogFn g_Log              = nullptr;
static uint32_t  g_ForcedAppId      = 480;
static uint32_t  g_OriginalAppId    = 0;
static volatile LONG g_bShutdown    = 0;
static HANDLE    g_hWatcherThread   = nullptr;

#define LOG(...) do { if (g_Log) g_Log(__VA_ARGS__); } while (0)

// See the IL2CPP plugin (photon_realtime_plugin.cpp) for the full
// rationale. Two AppId slots: Realtime (main PUN cloud) and
// Voice (Photon Voice cloud, only used by games that ship voice
// chat -- R.E.P.O. for example).
static char        g_OurAppIdUtf8[64]      = {};
static MonoString* g_OurAppIdString        = nullptr;
static bool        g_AppIdPatchEnabled     = false;

static char        g_OurVoiceAppIdUtf8[64] = {};
static MonoString* g_OurVoiceAppIdString   = nullptr;
static bool        g_VoiceAppIdEnabled     = false;

// Peer classifier -- see IL2CPP plugin for design notes. First
// peer pThis observed = Realtime, second distinct pThis = Voice.
struct PeerSlot { void* pThis; int product; };
static PeerSlot         g_Peers[4]  = {};
static int              g_PeerCount = 0;
static CRITICAL_SECTION g_PeerCs;
static bool             g_PeerCsInit = false;

static int ClassifyPeer(void* pThis)
{
    if (!pThis) return 0;
    EnterCriticalSection(&g_PeerCs);
    int product = 0;
    bool found = false;
    for (int i = 0; i < g_PeerCount; ++i) {
        if (g_Peers[i].pThis == pThis) { product = g_Peers[i].product; found = true; break; }
    }
    if (!found) {
        product = (g_PeerCount == 0) ? 0 : 1;
        if (g_PeerCount < (int)(sizeof(g_Peers)/sizeof(g_Peers[0]))) {
            g_Peers[g_PeerCount++] = { pThis, product };
            LOG("[Realtime/Mono] new peer %p classified as %s", pThis,
                product == 0 ? "Realtime" : "Voice");
        }
    }
    LeaveCriticalSection(&g_PeerCs);
    return product;
}

// CustomAuthenticationType byte enum: Custom=0, Steam=1, ..., None=255
static unsigned int g_ForcedAuthType = 0;

// AuthenticationValues layout in Mono is the same as IL2CPP:
// MonoObject header (16 bytes) followed by C# fields. The first
// public field on Photon.Realtime.AuthenticationValues is
// `byte authType`, sitting at +0x10 after the header.
static const size_t kOffsetAuthValues_authType = 0x10;

extern "C" void MONO_Log(const char* fmt, ...)
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
static bool InstallMonoHook(const char* image, const char* ns, const char* klass,
                            const char* method, int argc, void* hook, void** orig,
                            const char* logName)
{
    void* fn = MONO_FindMethodPtr(image, ns, klass, method, argc);
    if (!fn) { LOG("[Realtime/Mono] could not find %s", logName); return false; }
    if (MH_CreateHook(fn, hook, orig) != MH_OK || MH_EnableHook(fn) != MH_OK)
    {
        LOG("[Realtime/Mono] hook FAILED for %s at %p", logName, fn);
        return false;
    }
    LOG("[Realtime/Mono] %s hook installed at %p", logName, fn);
    return true;
}

// ============================================================
// Hook: LoadBalancingPeer.OpAuthenticate (the wire-time override)
//
// PUN signature (Mono, v4.x):
//   bool OpAuthenticate(string appId, string appVersion,
//                       AuthenticationValues authValues,
//                       string regionCode, bool getLobbyStatistics)
// ============================================================
typedef bool (__fastcall *Fn_OpAuthenticate)(
    void* pThis, void* appId, void* appVersion, void* authValues,
    void* regionCode, bool getLobbyStatistics);
static Fn_OpAuthenticate g_pfnOrigOpAuthenticate = nullptr;

typedef bool (__fastcall *Fn_OpAuthenticateOnce)(
    void* pThis, void* appId, void* appVersion, void* authValues,
    void* regionCode, int encryptionMode, int expectedProtocol);
static Fn_OpAuthenticateOnce g_pfnOrigOpAuthenticateOnce = nullptr;

static void EnsureAppIdStrings()
{
    if (g_AppIdPatchEnabled && !g_OurAppIdString) {
        g_OurAppIdString = MONO_StringNew(g_OurAppIdUtf8);
        if (g_OurAppIdString)
            LOG("[Realtime/Mono] built managed string for Realtime '%s' at %p",
                g_OurAppIdUtf8, g_OurAppIdString);
    }
    if (g_VoiceAppIdEnabled && !g_OurVoiceAppIdString) {
        g_OurVoiceAppIdString = MONO_StringNew(g_OurVoiceAppIdUtf8);
        if (g_OurVoiceAppIdString)
            LOG("[Realtime/Mono] built managed string for Voice '%s' at %p",
                g_OurVoiceAppIdUtf8, g_OurVoiceAppIdString);
    }
}

static void* PatchAppIdArg(void* pThis, void* appId, const char* sender)
{
    EnsureAppIdStrings();
    int product = ClassifyPeer(pThis);
    MonoString* replacement = nullptr;
    const char* productName = "Realtime";
    if (product == 1) {
        replacement = g_VoiceAppIdEnabled ? g_OurVoiceAppIdString : nullptr;
        productName = "Voice";
    } else {
        replacement = g_AppIdPatchEnabled ? g_OurAppIdString : nullptr;
    }
    if (!replacement) return appId;
    if (appId != replacement)
        LOG("[Realtime/Mono] %s (%s peer): appId arg %p -> %p", sender, productName,
            appId, replacement);
    return replacement;
}

static const char* PickAppIdUtf8ForPeer(void* pThis, const char** outProductName)
{
    int product = ClassifyPeer(pThis);
    if (product == 1) {
        if (outProductName) *outProductName = "Voice";
        return g_VoiceAppIdEnabled ? g_OurVoiceAppIdUtf8 : nullptr;
    }
    if (outProductName) *outProductName = "Realtime";
    return g_AppIdPatchEnabled ? g_OurAppIdUtf8 : nullptr;
}

static void PatchAuthValuesAuthType(void* authValues, const char* sender)
{
    if (!authValues) return;
    unsigned char* p = (unsigned char*)authValues + kOffsetAuthValues_authType;
    unsigned char prev = *p;
    *p = (unsigned char)(g_ForcedAuthType & 0xFF);
    LOG("[Realtime/Mono] %s: authValues.authType %u -> %u", sender, prev, g_ForcedAuthType);
}

static bool __fastcall Hooked_OpAuthenticate(
    void* pThis, void* appId, void* appVersion, void* authValues,
    void* regionCode, bool getLobbyStatistics)
{
    appId = PatchAppIdArg(pThis, appId, "OpAuthenticate");
    PatchAuthValuesAuthType(authValues, "OpAuthenticate");
    return g_pfnOrigOpAuthenticate(pThis, appId, appVersion, authValues,
                                    regionCode, getLobbyStatistics);
}

static bool __fastcall Hooked_OpAuthenticateOnce(
    void* pThis, void* appId, void* appVersion, void* authValues,
    void* regionCode, int encryptionMode, int expectedProtocol)
{
    appId = PatchAppIdArg(pThis, appId, "OpAuthenticateOnce");
    PatchAuthValuesAuthType(authValues, "OpAuthenticateOnce");
    return g_pfnOrigOpAuthenticateOnce(pThis, appId, appVersion, authValues,
                                        regionCode, encryptionMode, expectedProtocol);
}

// ============================================================
// Diagnostic hooks -- log-only, no behavior modification.
// Tell us which higher-level PUN methods REPO invokes when the
// user clicks an online button. Whichever fires reveals where
// the auth flow actually lives.
// ============================================================
typedef bool (__fastcall *Fn_Diag6)(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6);
static Fn_Diag6 g_pfnOrig_ConnectToMasterServer = nullptr;
static Fn_Diag6 g_pfnOrig_ConnectToNameServer  = nullptr;
static Fn_Diag6 g_pfnOrig_ConnectUsingSettings = nullptr;
static Fn_Diag6 g_pfnOrig_PhotonNetwork_ConnectUsingSettings = nullptr;
static Fn_Diag6 g_pfnOrig_AuthenticateAsync    = nullptr;

static bool __fastcall Hooked_ConnectToMasterServer(void* pThis, void* a2, void* a3, void* a4, void* a5, void* a6)
{
    LOG("[Realtime/Mono] DIAG: LoadBalancingClient.ConnectToMasterServer called");
    return g_pfnOrig_ConnectToMasterServer(pThis, a2, a3, a4, a5, a6);
}
static bool __fastcall Hooked_ConnectToNameServer(void* pThis, void* a2, void* a3, void* a4, void* a5, void* a6)
{
    LOG("[Realtime/Mono] DIAG: LoadBalancingClient.ConnectToNameServer called");
    return g_pfnOrig_ConnectToNameServer(pThis, a2, a3, a4, a5, a6);
}
static bool __fastcall Hooked_ConnectUsingSettings(void* pThis, void* a2, void* a3, void* a4, void* a5, void* a6)
{
    LOG("[Realtime/Mono] DIAG: LoadBalancingClient.ConnectUsingSettings called");
    return g_pfnOrig_ConnectUsingSettings(pThis, a2, a3, a4, a5, a6);
}
static bool __fastcall Hooked_PhotonNetwork_ConnectUsingSettings(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6)
{
    LOG("[Realtime/Mono] DIAG: PhotonNetwork.ConnectUsingSettings called");
    return g_pfnOrig_PhotonNetwork_ConnectUsingSettings(a1, a2, a3, a4, a5, a6);
}
static bool __fastcall Hooked_AuthenticateAsync(void* pThis, void* a2, void* a3, void* a4, void* a5, void* a6)
{
    LOG("[Realtime/Mono] DIAG: LoadBalancingClient.AuthenticateAsync called");
    return g_pfnOrig_AuthenticateAsync(pThis, a2, a3, a4, a5, a6);
}

// ============================================================
// Last-resort hook: PhotonPeer.SendOperation
//
// This is the lowest-level wire-send method, in
// Photon3Unity3D.dll. Every Photon wire packet (Authenticate,
// JoinLobby, JoinRoom, etc.) flows through here regardless of
// which LoadBalancingPeer subclass invoked it. If a subclass-
// override of OpAuthenticate routes around our class-level hook,
// SendOperation still catches it.
//
// Signature (Mono, Photon3 v4.x):
//   bool SendOperation(byte operationCode,
//                      Dictionary<byte, object> parameters,
//                      SendOptions sendOptions)
// or:
//   bool SendOperation(byte operationCode,
//                      Dictionary<byte, object> parameters,
//                      bool sendReliable, byte channelId, ...)
// ============================================================
typedef bool (__fastcall *Fn_SendOperation)(void* pThis, uint8_t opCode, void* params,
                                            void* sendOptions, void* a5, void* a6);
static Fn_SendOperation g_pfnOrig_SendOperation = nullptr;

static bool __fastcall Hooked_SendOperation(void* pThis, uint8_t opCode, void* params,
                                             void* sendOptions, void* a5, void* a6)
{
    // Photon opcodes vary slightly between SDK versions. Empirically
    // observed REPO using 220 as its first wire op when connecting --
    // we treat 220/230/231 as authentication-bearing and rewrite the
    // ClientAuthenticationType param (key 217) to Custom (0).
    const char* opName = "?";
    bool isAuth = false;
    switch (opCode) {
        case 220: opName = "GetRegions/Auth"; isAuth = true;  break;
        case 222: opName = "ChangeGroups";                    break;
        case 226: opName = "GetRegions";                      break;
        case 227: opName = "JoinLobby";                       break;
        case 228: opName = "LeaveLobby";                      break;
        case 229: opName = "CreateGame";                      break;
        case 230: opName = "Authenticate";     isAuth = true;  break;
        case 231: opName = "AuthenticateOnce"; isAuth = true;  break;
        case 248: opName = "JoinRoom";                        break;
        case 252: opName = "SetProperties";                   break;
        case 253: opName = "RaiseEvent";                      break;
        case 254: opName = "Leave";                           break;
    }
    LOG("[Realtime/Mono] SendOperation opCode=%u (%s) params=%p", opCode, opName, params);

    if (isAuth && params)
    {
        // Inspect params[224] = ApplicationId (the AppId GUID string).
        // This tells us whether REPO's runtime AppId matches what we
        // patched into resources.assets, or whether it's hard-coded
        // somewhere else in Assembly-CSharp.dll overriding our patch.
        char buf[256];
        MonoObject* curAppId = MONO_DictByteGetItem((MonoObject*)params, 224);
        MONO_DescribeObject(curAppId, buf, sizeof(buf));
        LOG("[Realtime/Mono] SendOperation: BEFORE  params[224] (ApplicationId) = %s", buf);

        MonoObject* curRegion = MONO_DictByteGetItem((MonoObject*)params, 210);
        MONO_DescribeObject(curRegion, buf, sizeof(buf));
        LOG("[Realtime/Mono] SendOperation: BEFORE  params[210] (Region)        = %s", buf);

        MonoObject* curAppVer = MONO_DictByteGetItem((MonoObject*)params, 220);
        MONO_DescribeObject(curAppVer, buf, sizeof(buf));
        LOG("[Realtime/Mono] SendOperation: BEFORE  params[220] (AppVersion)    = %s", buf);

        // Force ClientAuthenticationType = 0 (Custom). Same as before.
        if (g_ForcedAuthType <= 255)
        {
            if (MONO_DictByteByteSetItem((MonoObject*)params, 217,
                                          (uint8_t)(g_ForcedAuthType & 0xFF)))
            {
                LOG("[Realtime/Mono] SendOperation: rewrote params[217] AuthType -> %u",
                    g_ForcedAuthType);
            }
        }

        // Rewrite ApplicationId to whichever user GUID matches this
        // peer's classified product (Realtime or Voice). If the
        // relevant slot isn't configured, params[224] is left alone.
        const char* productName = "Realtime";
        const char* userAppId   = PickAppIdUtf8ForPeer(pThis, &productName);
        if (userAppId && userAppId[0])
        {
            if (MONO_DictByteStringSetItem((MonoObject*)params, 224, userAppId))
            {
                LOG("[Realtime/Mono] SendOperation (%s peer): rewrote params[224] AppId -> %s",
                    productName, userAppId);
            }
            else
            {
                LOG("[Realtime/Mono] SendOperation (%s peer): FAILED to rewrite params[224]",
                    productName);
            }
        }
    }

    return g_pfnOrig_SendOperation(pThis, opCode, params, sendOptions, a5, a6);
}

// ============================================================
// Byte-prologue verifier -- log the first 16 bytes at each
// hook target so we can confirm the hooked address is actual
// executable code (function prologue), not a struct or stub.
// ============================================================
static void LogBytesAt(void* addr, const char* label)
{
    if (!addr) return;
    uint8_t* p = (uint8_t*)addr;
    char buf[80] = {};
    int off = 0;
    for (int i = 0; i < 16 && off < (int)sizeof(buf) - 4; ++i)
    {
        off += _snprintf_s(buf + off, sizeof(buf) - off, _TRUNCATE, "%02X ", p[i]);
    }
    LOG("[Realtime/Mono] BYTES @ %s (%p): %s", label, addr, buf);
}

// ============================================================
// Hook installer + watcher
// ============================================================
static void TryInstallMonoHooks()
{
    if (!MONO_IsReady()) return;
    static bool attempted = false;
    if (attempted) return;
    attempted = true;

    InstallMonoHook(
        "PhotonRealtime", "Photon.Realtime", "LoadBalancingPeer",
        "OpAuthenticate", -1,
        (void*)&Hooked_OpAuthenticate,
        (void**)&g_pfnOrigOpAuthenticate,
        "LoadBalancingPeer.OpAuthenticate");

    InstallMonoHook(
        "PhotonRealtime", "Photon.Realtime", "LoadBalancingPeer",
        "OpAuthenticateOnce", -1,
        (void*)&Hooked_OpAuthenticateOnce,
        (void**)&g_pfnOrigOpAuthenticateOnce,
        "LoadBalancingPeer.OpAuthenticateOnce");

    // ----- Diagnostic hooks -----
    InstallMonoHook(
        "PhotonRealtime", "Photon.Realtime", "LoadBalancingClient",
        "ConnectToMasterServer", -1,
        (void*)&Hooked_ConnectToMasterServer,
        (void**)&g_pfnOrig_ConnectToMasterServer,
        "[DIAG] LoadBalancingClient.ConnectToMasterServer");
    InstallMonoHook(
        "PhotonRealtime", "Photon.Realtime", "LoadBalancingClient",
        "ConnectToNameServer", -1,
        (void*)&Hooked_ConnectToNameServer,
        (void**)&g_pfnOrig_ConnectToNameServer,
        "[DIAG] LoadBalancingClient.ConnectToNameServer");
    InstallMonoHook(
        "PhotonRealtime", "Photon.Realtime", "LoadBalancingClient",
        "ConnectUsingSettings", -1,
        (void*)&Hooked_ConnectUsingSettings,
        (void**)&g_pfnOrig_ConnectUsingSettings,
        "[DIAG] LoadBalancingClient.ConnectUsingSettings");
    InstallMonoHook(
        "PhotonUnityNetworking", "Photon.Pun", "PhotonNetwork",
        "ConnectUsingSettings", -1,
        (void*)&Hooked_PhotonNetwork_ConnectUsingSettings,
        (void**)&g_pfnOrig_PhotonNetwork_ConnectUsingSettings,
        "[DIAG] PhotonNetwork.ConnectUsingSettings");
    InstallMonoHook(
        "PhotonRealtime", "Photon.Realtime", "LoadBalancingClient",
        "AuthenticateAsync", -1,
        (void*)&Hooked_AuthenticateAsync,
        (void**)&g_pfnOrig_AuthenticateAsync,
        "[DIAG] LoadBalancingClient.AuthenticateAsync");

    // Lowest-level wire send. Photon3Unity3D.dll contains
    // PhotonPeer; some versions put SendOperation on PeerBase.
    if (!InstallMonoHook(
        "Photon3Unity3D", "ExitGames.Client.Photon", "PhotonPeer",
        "SendOperation", -1,
        (void*)&Hooked_SendOperation,
        (void**)&g_pfnOrig_SendOperation,
        "PhotonPeer.SendOperation"))
    {
        // Fallback to PeerBase
        InstallMonoHook(
            "Photon3Unity3D", "ExitGames.Client.Photon", "PeerBase",
            "SendOperation", -1,
            (void*)&Hooked_SendOperation,
            (void**)&g_pfnOrig_SendOperation,
            "PeerBase.SendOperation");
    }

    // Verify hook target bytes look like function prologues.
    // Mono JIT prologues typically start with 0x55 (push rbp)
    // or 0x48 (REX prefix for x64 instrs like mov rax, ...).
    LogBytesAt((void*)g_pfnOrigOpAuthenticate,                "OpAuthenticate");
    LogBytesAt((void*)g_pfnOrigOpAuthenticateOnce,            "OpAuthenticateOnce");
    LogBytesAt((void*)g_pfnOrig_ConnectToNameServer,          "ConnectToNameServer");
    LogBytesAt((void*)g_pfnOrig_PhotonNetwork_ConnectUsingSettings, "PhotonNetwork.ConnectUsingSettings");
    LogBytesAt((void*)g_pfnOrig_SendOperation,                "SendOperation");
}

static DWORD WINAPI WatcherProc(LPVOID)
{
    for (int i = 0; i < 600 && InterlockedCompareExchange(&g_bShutdown, 0, 0) == 0; ++i)
    {
        if (MONO_TryInit())
        {
            // Mono assemblies aren't necessarily all loaded the
            // instant the runtime is up. Give the game a moment
            // to load PhotonRealtime.dll before scanning.
            Sleep(500);
            TryInstallMonoHooks();
            return 0;
        }
        Sleep(200);
    }
    if (!InterlockedCompareExchange(&g_bShutdown, 0, 0))
        LOG("[Realtime/Mono] mono-2.0-bdwgc.dll never resolved -- giving up");
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

    LOG("[Realtime/Mono] plugin v1 init: AppId=%u ogAppId=%u",
        g_ForcedAppId, g_OriginalAppId);

    if (!g_PeerCsInit) { InitializeCriticalSection(&g_PeerCs); g_PeerCsInit = true; }

    const char* ini = GetIniPath();
    if (ini)
    {
        // New ini section is [Realtime]; falls back to legacy [PUN]
        // for back-compat with inis written by older Setup.bat runs.
        GetPrivateProfileStringA("Realtime", "PhotonAppIdRealtime", "",
                                 g_OurAppIdUtf8,
                                 sizeof(g_OurAppIdUtf8), ini);
        if (!g_OurAppIdUtf8[0])
        {
            GetPrivateProfileStringA("PUN", "PhotonAppIdRealtime", "",
                                     g_OurAppIdUtf8,
                                     sizeof(g_OurAppIdUtf8), ini);
        }
        if (g_OurAppIdUtf8[0])
        {
            g_AppIdPatchEnabled = true;
            LOG("[Realtime/Mono] PhotonAppIdRealtime override set: %s", g_OurAppIdUtf8);
        }
        else
        {
            LOG("[Realtime/Mono] no PhotonAppIdRealtime in ini (section [Realtime] or [PUN]) -- Realtime AppId patch disabled");
        }

        GetPrivateProfileStringA("Realtime", "PhotonAppIdVoice", "",
                                 g_OurVoiceAppIdUtf8,
                                 sizeof(g_OurVoiceAppIdUtf8), ini);
        if (!g_OurVoiceAppIdUtf8[0])
        {
            GetPrivateProfileStringA("PUN", "PhotonAppIdVoice", "",
                                     g_OurVoiceAppIdUtf8,
                                     sizeof(g_OurVoiceAppIdUtf8), ini);
        }
        if (g_OurVoiceAppIdUtf8[0])
        {
            g_VoiceAppIdEnabled = true;
            LOG("[Realtime/Mono] PhotonAppIdVoice override set: %s", g_OurVoiceAppIdUtf8);
        }
        else
        {
            LOG("[Realtime/Mono] no PhotonAppIdVoice in ini -- Voice peer passthrough");
        }

        char authTypeBuf[8] = {};
        GetPrivateProfileStringA("Realtime", "ForcedAuthType", "",
                                 authTypeBuf, sizeof(authTypeBuf), ini);
        if (!authTypeBuf[0])
            GetPrivateProfileStringA("PUN", "ForcedAuthType", "0",
                                     authTypeBuf, sizeof(authTypeBuf), ini);
        unsigned int parsed = (unsigned int)strtoul(authTypeBuf, nullptr, 10);
        if (parsed <= 255) g_ForcedAuthType = parsed;
        LOG("[Realtime/Mono] forced AuthType = %u", g_ForcedAuthType);
    }

    if (MH_Initialize() != MH_OK)
    {
        LOG("[Realtime/Mono] MH_Initialize non-OK (already inited?)");
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
    LOG("[Realtime/Mono] plugin shutdown");
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
