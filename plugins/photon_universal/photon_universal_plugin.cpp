// ============================================================
// UCOnline2 -- Photon Universal Plugin
//
// One DLL that handles everything UCOnline2 currently supports
// in the Photon space:
//
//   * Photon Realtime / PUN  (IL2CPP and Mono backends)
//   * Photon Fusion 2        (IL2CPP)
//   * Photon Voice           (paired with Realtime/PUN)
//
// At init we auto-detect:
//   * Unity backend (Mono vs IL2CPP) via runtime DLL presence
//   * Photon flavor (Realtime/PUN vs Fusion) via metadata/assembly scan
// and install only the relevant module hooks.
//
// INI config (union-crax.ini next to the game exe):
//
//   [Realtime]
//   PhotonAppIdRealtime=<your Realtime app GUID>
//   PhotonAppIdVoice=<your Voice app GUID>      ; optional
//   ForcedAuthType=0
//
//   [Fusion]
//   PhotonAppIdFusion=<your Fusion app GUID>
//   ForcedAuthType=0
//
// You only need to populate the section(s) matching the game's
// Photon flavor. Mega plugin reads both; the inactive one is
// just ignored.
//
// MinHook is statically linked.
// ============================================================
#include <Windows.h>
#ifdef UCO_PHASMO_EXPERIMENTAL
#include <DbgHelp.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "../../include/MinHook.h"
#include "../../include/uco_plugin.h"

#include "il2cpp_runtime.h"
#include "mono_runtime.h"

// ============================================================
// SHARED INFRA
// ============================================================
static UCO_LogFn g_Log              = nullptr;
static uint32_t  g_ForcedAppId      = 480;
static uint32_t  g_OriginalAppId    = 0;
static volatile LONG g_bShutdown    = 0;
static HANDLE    g_hWatcherThread   = nullptr;
#ifdef UCO_PHASMO_EXPERIMENTAL
static PVOID     g_hCrashCapture    = nullptr;
static volatile LONG g_CrashCaptureWritten = 0;
#endif

#define LOG(...) do { if (g_Log) g_Log(__VA_ARGS__); } while (0)

// MONO_Log and IL2CPP_Log are extern "C" hooks the runtime helpers
// call to surface diagnostics. Both forward to g_Log.
extern "C" void MONO_Log(const char* fmt, ...)
{
    if (!g_Log) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_Log("%s", buf);
}
extern "C" void IL2CPP_Log(const char* fmt, ...)
{
    if (!g_Log) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_Log("%s", buf);
}

#ifdef UCO_PHASMO_EXPERIMENTAL
// Temporary crash capture for IL2CPP startup failures that bypass the game's
// disabled crash sender and do not reach Windows Error Reporting.
static LONG CALLBACK CaptureUnhandledAccessViolation(PEXCEPTION_POINTERS info)
{
    if (!info || !info->ExceptionRecord ||
        info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        InterlockedCompareExchange(&g_CrashCaptureWritten, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    char tempPath[MAX_PATH] = {};
    char dumpPath[MAX_PATH] = {};
    if (GetTempPathA((DWORD)sizeof(tempPath), tempPath))
    {
        _snprintf_s(dumpPath, sizeof(dumpPath), _TRUNCATE,
            "%suco2_phasmo_av_%lu.dmp", tempPath, GetCurrentProcessId());
        HANDLE file = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION exceptionInfo = {};
            exceptionInfo.ThreadId = GetCurrentThreadId();
            exceptionInfo.ExceptionPointers = info;
            exceptionInfo.ClientPointers = FALSE;
            HMODULE dbghelp = LoadLibraryA("DbgHelp.dll");
            auto writeDump = dbghelp
                ? (decltype(&MiniDumpWriteDump))GetProcAddress(dbghelp,
                    "MiniDumpWriteDump") : nullptr;
            if (writeDump)
                writeDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                    (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithThreadInfo |
                                    MiniDumpWithUnloadedModules),
                    &exceptionInfo, nullptr, nullptr);
            if (dbghelp) FreeLibrary(dbghelp);
            CloseHandle(file);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// Resolve <game>\union-crax.ini path once.
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
// MODULE: Realtime / PUN (IL2CPP backend)
//
// Hooks LoadBalancingPeer.OpAuthenticate / OpAuthenticateOnce,
// PhotonPeer.SendOperation, and AuthenticationValues.set_AuthType.
// Per-peer classifier routes the first peer to Realtime AppId
// and the second distinct peer to Voice AppId.
// ============================================================
namespace ModRealtimeIL2CPP {

static char  g_AppIdUtf8[64]      = {};
static void* g_AppIdString        = nullptr;
static bool  g_AppIdPatchEnabled  = false;
static char  g_VoiceAppIdUtf8[64] = {};
static void* g_VoiceAppIdString   = nullptr;
static bool  g_VoiceAppIdEnabled  = false;
static unsigned int g_ForcedAuthType = 0;
static const size_t kOffsetAuthType = 0x10;

struct PeerSlot { void* pThis; int product; };
static PeerSlot g_Peers[4] = {};
static int      g_PeerCount = 0;
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
            LOG("[Realtime] new peer %p classified as %s", pThis,
                product == 0 ? "Realtime" : "Voice");
        }
    }
    LeaveCriticalSection(&g_PeerCs);
    return product;
}

static void EnsureStrings()
{
    if (g_AppIdPatchEnabled && !g_AppIdString) {
        g_AppIdString = IL2CPP_StringNew(g_AppIdUtf8);
    }
    if (g_VoiceAppIdEnabled && !g_VoiceAppIdString) {
        g_VoiceAppIdString = IL2CPP_StringNew(g_VoiceAppIdUtf8);
    }
}

static void* PickAppIdString(void* pThis, const char** outName)
{
    EnsureStrings();
    int product = ClassifyPeer(pThis);
    void* r = (product == 1) ? g_VoiceAppIdString : g_AppIdString;
    if (outName) *outName = (product == 1) ? "Voice" : "Realtime";
    return r;
}
static const char* PickAppIdUtf8(void* pThis, const char** outName)
{
    int product = ClassifyPeer(pThis);
    if (outName) *outName = (product == 1) ? "Voice" : "Realtime";
    if (product == 1) return g_VoiceAppIdEnabled ? g_VoiceAppIdUtf8 : nullptr;
    return g_AppIdPatchEnabled ? g_AppIdUtf8 : nullptr;
}

static void PatchAuthType(void* authValues, const char* sender)
{
    if (!authValues) return;
    unsigned char* p = (unsigned char*)authValues + kOffsetAuthType;
    unsigned char prev = *p;
    *p = (unsigned char)(g_ForcedAuthType & 0xFF);
    LOG("[Realtime] %s: authValues.authType %u -> %u", sender, prev, g_ForcedAuthType);
}

typedef void (__fastcall *Fn_SetAuthType)(void* pThis, unsigned int value);
static Fn_SetAuthType g_pfnOrigSetAuthType = nullptr;
static void __fastcall Hooked_SetAuthType(void* pThis, unsigned int v) {
    g_pfnOrigSetAuthType(pThis, g_ForcedAuthType);
}

typedef bool (__fastcall *Fn_OpAuth)(void*, void*, void*, void*, void*, bool);
typedef bool (__fastcall *Fn_OpAuthOnce)(void*, void*, void*, void*, void*, int, int);
static Fn_OpAuth     g_pfnOrigOpAuth     = nullptr;
static Fn_OpAuthOnce g_pfnOrigOpAuthOnce = nullptr;

static bool __fastcall Hooked_OpAuth(void* pThis, void* appId, void* ver, void* auth, void* region, bool lobby) {
    const char* name = "Realtime";
    void* replace = PickAppIdString(pThis, &name);
    if (replace && appId != replace) {
        LOG("[Realtime] OpAuth (%s peer): appId arg %p -> %p", name, appId, replace);
        appId = replace;
    }
    PatchAuthType(auth, "OpAuth");
    return g_pfnOrigOpAuth(pThis, appId, ver, auth, region, lobby);
}
static bool __fastcall Hooked_OpAuthOnce(void* pThis, void* appId, void* ver, void* auth, void* region, int enc, int proto) {
    const char* name = "Realtime";
    void* replace = PickAppIdString(pThis, &name);
    if (replace && appId != replace) {
        LOG("[Realtime] OpAuthOnce (%s peer): appId arg %p -> %p", name, appId, replace);
        appId = replace;
    }
    PatchAuthType(auth, "OpAuthOnce");
    return g_pfnOrigOpAuthOnce(pThis, appId, ver, auth, region, enc, proto);
}

typedef bool (__fastcall *Fn_SendOp)(void* pThis, uint8_t op, void* params, void* opts, void* a5, void* a6);
static Fn_SendOp g_pfnOrigSendOp = nullptr;
static bool __fastcall Hooked_SendOp(void* pThis, uint8_t op, void* params, void* opts, void* a5, void* a6) {
    bool isAuth = (op == 220 || op == 226 || op == 230 || op == 231);
    if (isAuth && params) {
        const char* name = "Realtime";
        const char* userId = PickAppIdUtf8(pThis, &name);
        if (userId && userId[0]) {
            if (IL2CPP_DictByteStringSetItem((Il2CppObject*)params, 224, userId))
                LOG("[Realtime] SendOp op=%u (%s peer): params[224] AppId -> %s", op, name, userId);
        }
        if (g_ForcedAuthType <= 255) {
            IL2CPP_DictByteByteSetItem((Il2CppObject*)params, 217, (uint8_t)(g_ForcedAuthType & 0xFF));
        }
    }
    return g_pfnOrigSendOp(pThis, op, params, opts, a5, a6);
}

static bool InstallHook(void* target, void* detour, void** original,
                        const char* label)
{
    if (!target) {
        LOG("[Realtime] %s method not found", label);
        return false;
    }

    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        LOG("[Realtime] %s hook create failed: %s", label,
            MH_StatusToString(status));
        return false;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        LOG("[Realtime] %s hook enable failed: %s", label,
            MH_StatusToString(status));
        MH_RemoveHook(target);
        return false;
    }

    LOG("[Realtime] %s hook @ %p", label, target);
    return true;
}

static bool TryInstall()
{
    if (!IL2CPP_IsReady()) return false;
    // Current PUN builds use PhotonRealtime.dll. The runtime helper still
    // falls back across all loaded images, which keeps older assembly naming
    // compatible, but asking for the real image first avoids needless misses.
    if (!IL2CPP_FindClass("PhotonRealtime", "Photon.Realtime", "LoadBalancingPeer")) {
        static bool loggedMissingPeer = false;
        if (!loggedMissingPeer) {
            LOG("[Realtime] Photon.Realtime.LoadBalancingPeer not available");
            loggedMissingPeer = true;
        }
        return false;
    }

    if (!g_PeerCsInit) { InitializeCriticalSection(&g_PeerCs); g_PeerCsInit = true; }

    int installed = 0;
    void* fn = IL2CPP_FindMethodPtr("PhotonRealtime", "Photon.Realtime",
        "AuthenticationValues", "set_AuthType", 1);
    if (InstallHook(fn, (void*)&Hooked_SetAuthType,
        (void**)&g_pfnOrigSetAuthType, "set_AuthType")) ++installed;

    fn = IL2CPP_FindMethodPtr("PhotonRealtime", "Photon.Realtime",
        "LoadBalancingPeer", "OpAuthenticate", -1);
    if (InstallHook(fn, (void*)&Hooked_OpAuth,
        (void**)&g_pfnOrigOpAuth, "OpAuthenticate")) ++installed;

    fn = IL2CPP_FindMethodPtr("PhotonRealtime", "Photon.Realtime",
        "LoadBalancingPeer", "OpAuthenticateOnce", -1);
    if (InstallHook(fn, (void*)&Hooked_OpAuthOnce,
        (void**)&g_pfnOrigOpAuthOnce, "OpAuthenticateOnce")) ++installed;

    fn = IL2CPP_FindMethodPtr("Photon3Unity3D", "ExitGames.Client.Photon", "PhotonPeer", "SendOperation", -1);
    if (!fn)
        fn = IL2CPP_FindMethodPtr("Photon3Unity3D", "ExitGames.Client.Photon", "PeerBase", "SendOperation", -1);
    if (InstallHook(fn, (void*)&Hooked_SendOp,
        (void**)&g_pfnOrigSendOp, "SendOperation")) ++installed;

    if (installed == 0) return false;
    LOG("[Realtime] IL2CPP module active (%d hooks)", installed);
    return true;
}

static void ReadIni(const char* ini)
{
    GetPrivateProfileStringA("Realtime", "PhotonAppIdRealtime", "", g_AppIdUtf8, sizeof(g_AppIdUtf8), ini);
    if (!g_AppIdUtf8[0]) GetPrivateProfileStringA("PUN", "PhotonAppIdRealtime", "", g_AppIdUtf8, sizeof(g_AppIdUtf8), ini);
    g_AppIdPatchEnabled = (g_AppIdUtf8[0] != 0);
    GetPrivateProfileStringA("Realtime", "PhotonAppIdVoice", "", g_VoiceAppIdUtf8, sizeof(g_VoiceAppIdUtf8), ini);
    if (!g_VoiceAppIdUtf8[0]) GetPrivateProfileStringA("PUN", "PhotonAppIdVoice", "", g_VoiceAppIdUtf8, sizeof(g_VoiceAppIdUtf8), ini);
    g_VoiceAppIdEnabled = (g_VoiceAppIdUtf8[0] != 0);
    char buf[8] = {};
    GetPrivateProfileStringA("Realtime", "ForcedAuthType", "", buf, sizeof(buf), ini);
    if (!buf[0]) GetPrivateProfileStringA("PUN", "ForcedAuthType", "0", buf, sizeof(buf), ini);
    g_ForcedAuthType = (unsigned int)strtoul(buf, nullptr, 10);
    if (g_AppIdPatchEnabled)
        LOG("[Realtime] IL2CPP: Realtime AppId=%s Voice=%s AuthType=%u",
            g_AppIdUtf8, g_VoiceAppIdUtf8[0] ? g_VoiceAppIdUtf8 : "(none)", g_ForcedAuthType);
}
} // namespace ModRealtimeIL2CPP


// ============================================================
// MODULE: Realtime / PUN (Mono backend)
//
// Same shape as the IL2CPP module, different runtime helpers.
// ============================================================
namespace ModRealtimeMono {

static char        g_AppIdUtf8[64]      = {};
static MonoString* g_AppIdString        = nullptr;
static bool        g_AppIdPatchEnabled  = false;
static char        g_VoiceAppIdUtf8[64] = {};
static MonoString* g_VoiceAppIdString   = nullptr;
static bool        g_VoiceAppIdEnabled  = false;
static unsigned int g_ForcedAuthType = 0;
static const size_t kOffsetAuthType = 0x10;

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

static void EnsureStrings()
{
    if (g_AppIdPatchEnabled && !g_AppIdString) g_AppIdString = MONO_StringNew(g_AppIdUtf8);
    if (g_VoiceAppIdEnabled && !g_VoiceAppIdString) g_VoiceAppIdString = MONO_StringNew(g_VoiceAppIdUtf8);
}
static MonoString* PickAppIdString(void* pThis, const char** outName)
{
    EnsureStrings();
    int product = ClassifyPeer(pThis);
    if (outName) *outName = (product == 1) ? "Voice" : "Realtime";
    if (product == 1) return g_VoiceAppIdEnabled ? g_VoiceAppIdString : nullptr;
    return g_AppIdPatchEnabled ? g_AppIdString : nullptr;
}
static const char* PickAppIdUtf8(void* pThis, const char** outName)
{
    int product = ClassifyPeer(pThis);
    if (outName) *outName = (product == 1) ? "Voice" : "Realtime";
    if (product == 1) return g_VoiceAppIdEnabled ? g_VoiceAppIdUtf8 : nullptr;
    return g_AppIdPatchEnabled ? g_AppIdUtf8 : nullptr;
}

static void PatchAuthType(void* authValues, const char* sender)
{
    if (!authValues) return;
    unsigned char* p = (unsigned char*)authValues + kOffsetAuthType;
    unsigned char prev = *p;
    *p = (unsigned char)(g_ForcedAuthType & 0xFF);
    LOG("[Realtime/Mono] %s: authValues.authType %u -> %u", sender, prev, g_ForcedAuthType);
}

typedef void (__fastcall *Fn_SetAuthType)(void* pThis, unsigned int value);
static Fn_SetAuthType g_pfnOrigSetAuthType = nullptr;
static void __fastcall Hooked_SetAuthType(void* pThis, unsigned int v) {
    g_pfnOrigSetAuthType(pThis, g_ForcedAuthType);
}

typedef bool (__fastcall *Fn_OpAuth)(void*, void*, void*, void*, void*, bool);
typedef bool (__fastcall *Fn_OpAuthOnce)(void*, void*, void*, void*, void*, int, int);
static Fn_OpAuth     g_pfnOrigOpAuth     = nullptr;
static Fn_OpAuthOnce g_pfnOrigOpAuthOnce = nullptr;
static bool __fastcall Hooked_OpAuth(void* pThis, void* appId, void* ver, void* auth, void* region, bool lobby) {
    const char* name = "Realtime";
    MonoString* replace = PickAppIdString(pThis, &name);
    if (replace && appId != replace) {
        LOG("[Realtime/Mono] OpAuth (%s peer): appId arg %p -> %p", name, appId, replace);
        appId = replace;
    }
    PatchAuthType(auth, "OpAuth");
    return g_pfnOrigOpAuth(pThis, appId, ver, auth, region, lobby);
}
static bool __fastcall Hooked_OpAuthOnce(void* pThis, void* appId, void* ver, void* auth, void* region, int enc, int proto) {
    const char* name = "Realtime";
    MonoString* replace = PickAppIdString(pThis, &name);
    if (replace && appId != replace) {
        LOG("[Realtime/Mono] OpAuthOnce (%s peer): appId arg %p -> %p", name, appId, replace);
        appId = replace;
    }
    PatchAuthType(auth, "OpAuthOnce");
    return g_pfnOrigOpAuthOnce(pThis, appId, ver, auth, region, enc, proto);
}

typedef bool (__fastcall *Fn_SendOp)(void* pThis, uint8_t op, void* params, void* opts, void* a5, void* a6);
static Fn_SendOp g_pfnOrigSendOp = nullptr;
static bool __fastcall Hooked_SendOp(void* pThis, uint8_t op, void* params, void* opts, void* a5, void* a6) {
    bool isAuth = (op == 220 || op == 226 || op == 230 || op == 231);
    if (isAuth && params) {
        const char* name = "Realtime";
        const char* userId = PickAppIdUtf8(pThis, &name);
        if (userId && userId[0]) {
            if (MONO_DictByteStringSetItem((MonoObject*)params, 224, userId))
                LOG("[Realtime/Mono] SendOp op=%u (%s peer): params[224] AppId -> %s", op, name, userId);
        }
        if (g_ForcedAuthType <= 255) {
            MONO_DictByteByteSetItem((MonoObject*)params, 217, (uint8_t)(g_ForcedAuthType & 0xFF));
        }
    }
    return g_pfnOrigSendOp(pThis, op, params, opts, a5, a6);
}

static bool TryInstall()
{
    if (!MONO_IsReady()) return false;
    if (!MONO_FindClass("Photon.Realtime", "Photon.Realtime", "LoadBalancingPeer")) return false;

    if (!g_PeerCsInit) { InitializeCriticalSection(&g_PeerCs); g_PeerCsInit = true; }

    void* fn;
    fn = MONO_FindMethodPtr("Photon.Realtime", "Photon.Realtime", "AuthenticationValues", "set_AuthType", 1);
    if (fn && MH_CreateHook(fn, (void*)&Hooked_SetAuthType, (void**)&g_pfnOrigSetAuthType) == MH_OK)
        { MH_EnableHook(fn); LOG("[Realtime/Mono] set_AuthType hook @ %p", fn); }
    fn = MONO_FindMethodPtr("Photon.Realtime", "Photon.Realtime", "LoadBalancingPeer", "OpAuthenticate", -1);
    if (fn && MH_CreateHook(fn, (void*)&Hooked_OpAuth, (void**)&g_pfnOrigOpAuth) == MH_OK)
        { MH_EnableHook(fn); LOG("[Realtime/Mono] OpAuthenticate hook @ %p", fn); }
    fn = MONO_FindMethodPtr("Photon.Realtime", "Photon.Realtime", "LoadBalancingPeer", "OpAuthenticateOnce", -1);
    if (fn && MH_CreateHook(fn, (void*)&Hooked_OpAuthOnce, (void**)&g_pfnOrigOpAuthOnce) == MH_OK)
        { MH_EnableHook(fn); LOG("[Realtime/Mono] OpAuthenticateOnce hook @ %p", fn); }
    fn = MONO_FindMethodPtr("Photon3Unity3D", "ExitGames.Client.Photon", "PhotonPeer", "SendOperation", -1);
    if (!fn) fn = MONO_FindMethodPtr("Photon3Unity3D", "ExitGames.Client.Photon", "PeerBase", "SendOperation", -1);
    if (fn && MH_CreateHook(fn, (void*)&Hooked_SendOp, (void**)&g_pfnOrigSendOp) == MH_OK)
        { MH_EnableHook(fn); LOG("[Realtime/Mono] SendOperation hook @ %p", fn); }
    LOG("[Realtime/Mono] module active");
    return true;
}

static void ReadIni(const char* ini)
{
    GetPrivateProfileStringA("Realtime", "PhotonAppIdRealtime", "", g_AppIdUtf8, sizeof(g_AppIdUtf8), ini);
    if (!g_AppIdUtf8[0]) GetPrivateProfileStringA("PUN", "PhotonAppIdRealtime", "", g_AppIdUtf8, sizeof(g_AppIdUtf8), ini);
    g_AppIdPatchEnabled = (g_AppIdUtf8[0] != 0);
    GetPrivateProfileStringA("Realtime", "PhotonAppIdVoice", "", g_VoiceAppIdUtf8, sizeof(g_VoiceAppIdUtf8), ini);
    if (!g_VoiceAppIdUtf8[0]) GetPrivateProfileStringA("PUN", "PhotonAppIdVoice", "", g_VoiceAppIdUtf8, sizeof(g_VoiceAppIdUtf8), ini);
    g_VoiceAppIdEnabled = (g_VoiceAppIdUtf8[0] != 0);
    char buf[8] = {};
    GetPrivateProfileStringA("Realtime", "ForcedAuthType", "", buf, sizeof(buf), ini);
    if (!buf[0]) GetPrivateProfileStringA("PUN", "ForcedAuthType", "0", buf, sizeof(buf), ini);
    g_ForcedAuthType = (unsigned int)strtoul(buf, nullptr, 10);
    if (g_AppIdPatchEnabled)
        LOG("[Realtime/Mono] Realtime AppId=%s Voice=%s AuthType=%u",
            g_AppIdUtf8, g_VoiceAppIdUtf8[0] ? g_VoiceAppIdUtf8 : "(none)", g_ForcedAuthType);
}
} // namespace ModRealtimeMono


// ============================================================
// MODULE: Photon Fusion 2 (IL2CPP)
//
// Different ScriptableObject (PhotonAppSettings), different
// namespace (Fusion.Photon.Realtime). Single AppId slot:
// AppIdFusion. We override via the get_Global hook which
// returns the singleton with our AppId stamped in.
// ============================================================
namespace ModFusion {

static char  g_AppIdUtf8[64]      = {};
static void* g_AppIdString        = nullptr;
static bool  g_AppIdPatchEnabled  = false;
static unsigned int g_ForcedAuthType = 0;
static const size_t kOffsetAppSettings_AppIdFusion = 0x18;
static const size_t kOffsetAuthType                = 0x10;

typedef void* (__fastcall *Fn_GetGlobal)();
static Fn_GetGlobal g_pfnOrigGetGlobal = nullptr;
static void* __fastcall Hooked_GetGlobal()
{
    void* settings = g_pfnOrigGetGlobal();
    if (!settings || !g_AppIdPatchEnabled) return settings;
    if (!g_AppIdString) g_AppIdString = IL2CPP_StringNew(g_AppIdUtf8);
    if (!g_AppIdString) return settings;
    // settings is a wrapper; the actual AppSettings is at settings+0x10 conventionally.
    void** pAppSettings = (void**)((char*)settings + 0x10);
    void* appSettings = *pAppSettings;
    if (!appSettings) return settings;
    void** pAppId = (void**)((char*)appSettings + kOffsetAppSettings_AppIdFusion);
    if (*pAppId != g_AppIdString) {
        void* old = *pAppId;
        *pAppId = g_AppIdString;
        LOG("[Fusion] AppIdFusion patched in singleton (was %p)", old);
    }
    return settings;
}

typedef void (__fastcall *Fn_SetAuthType)(void* pThis, unsigned int value);
static Fn_SetAuthType g_pfnOrigSetAuthType = nullptr;
static void __fastcall Hooked_SetAuthType(void* pThis, unsigned int v) {
    g_pfnOrigSetAuthType(pThis, g_ForcedAuthType);
}

typedef bool (__fastcall *Fn_OpAuth)(void*, void*, void*, void*, void*, bool);
typedef bool (__fastcall *Fn_OpAuthOnce)(void*, void*, void*, void*, void*, int, int);
static Fn_OpAuth     g_pfnOrigOpAuth     = nullptr;
static Fn_OpAuthOnce g_pfnOrigOpAuthOnce = nullptr;

static void PatchAuthType(void* authValues, const char* sender) {
    if (!authValues) return;
    unsigned char* p = (unsigned char*)authValues + kOffsetAuthType;
    unsigned char prev = *p;
    *p = (unsigned char)(g_ForcedAuthType & 0xFF);
    LOG("[Fusion] %s: authType %u -> %u", sender, prev, g_ForcedAuthType);
}
static bool __fastcall Hooked_OpAuth(void* pThis, void* appId, void* ver, void* auth, void* region, bool lobby) {
    PatchAuthType(auth, "OpAuth");
    return g_pfnOrigOpAuth(pThis, appId, ver, auth, region, lobby);
}
static bool __fastcall Hooked_OpAuthOnce(void* pThis, void* appId, void* ver, void* auth, void* region, int enc, int proto) {
    PatchAuthType(auth, "OpAuthOnce");
    return g_pfnOrigOpAuthOnce(pThis, appId, ver, auth, region, enc, proto);
}

static bool TryInstall()
{
    if (!IL2CPP_IsReady()) return false;
    if (!IL2CPP_FindClass("Fusion.Realtime", "Fusion.Photon.Realtime", "PhotonAppSettings")) return false;

    void* fn;
    if (g_AppIdPatchEnabled) {
        fn = IL2CPP_FindMethodPtr("Fusion.Realtime", "Fusion.Photon.Realtime", "PhotonAppSettings", "get_Global", 0);
        if (fn && MH_CreateHook(fn, (void*)&Hooked_GetGlobal, (void**)&g_pfnOrigGetGlobal) == MH_OK)
            { MH_EnableHook(fn); LOG("[Fusion] PhotonAppSettings.get_Global hook @ %p", fn); }
    }
    fn = IL2CPP_FindMethodPtr("Fusion.Realtime", "Fusion.Photon.Realtime", "AuthenticationValues", "set_AuthType", 1);
    if (fn && MH_CreateHook(fn, (void*)&Hooked_SetAuthType, (void**)&g_pfnOrigSetAuthType) == MH_OK)
        { MH_EnableHook(fn); LOG("[Fusion] set_AuthType hook @ %p", fn); }
    fn = IL2CPP_FindMethodPtr("Fusion.Realtime", "Fusion.Photon.Realtime", "LoadBalancingPeer", "OpAuthenticate", -1);
    if (fn && MH_CreateHook(fn, (void*)&Hooked_OpAuth, (void**)&g_pfnOrigOpAuth) == MH_OK)
        { MH_EnableHook(fn); LOG("[Fusion] OpAuthenticate hook @ %p", fn); }
    fn = IL2CPP_FindMethodPtr("Fusion.Realtime", "Fusion.Photon.Realtime", "LoadBalancingPeer", "OpAuthenticateOnce", -1);
    if (fn && MH_CreateHook(fn, (void*)&Hooked_OpAuthOnce, (void**)&g_pfnOrigOpAuthOnce) == MH_OK)
        { MH_EnableHook(fn); LOG("[Fusion] OpAuthenticateOnce hook @ %p", fn); }
    LOG("[Fusion] module active");
    return true;
}

static void ReadIni(const char* ini)
{
    GetPrivateProfileStringA("Fusion", "PhotonAppIdFusion", "", g_AppIdUtf8, sizeof(g_AppIdUtf8), ini);
    g_AppIdPatchEnabled = (g_AppIdUtf8[0] != 0);
    char buf[8] = {};
    GetPrivateProfileStringA("Fusion", "ForcedAuthType", "0", buf, sizeof(buf), ini);
    g_ForcedAuthType = (unsigned int)strtoul(buf, nullptr, 10);
    if (g_AppIdPatchEnabled)
        LOG("[Fusion] AppId=%s AuthType=%u", g_AppIdUtf8, g_ForcedAuthType);
}
} // namespace ModFusion


#ifdef UCO_PHASMO_EXPERIMENTAL
// ============================================================
// MODULE: Unity Services Auth (IL2CPP)
//
// Experimental, unreleased Phasmophobia work. The release
// photon_universal build compiles this section out.
// ============================================================
namespace ModUnityAuth {

typedef void* (__fastcall *Fn_Task)(void* pThis, void* a1, void* a2, void* a3);
static Fn_Task g_pfnSignInAnon          = nullptr;
static Fn_Task g_pfnOrigSignInAnon[5]   = {};
static Fn_Task g_pfnOrigExternalSignIn[5] = {};
static Fn_Task g_pfnOrigSignInSteam[5]  = {};
static Fn_Task g_pfnOrigLinkSteam[5]    = {};
static Fn_Task g_pfnOrigUpdatePlayer[5] = {};
static Fn_Task g_pfnOrigRefreshToken[5] = {};
static Fn_Task g_pfnOrigHandleSignIn[5] = {};
static bool g_SteamSignInInstalled = false;
typedef void* (__fastcall *Fn_SelfReport)(void* detection);
static Fn_SelfReport g_pfnOrigSelfReport = nullptr;
static bool g_SelfReportInstalled = false;
typedef void (__fastcall *Fn_AuthRequestCompleted)(void* pThis, void* tcs,
    int64_t responseCode, bool isNetworkError, bool isServerError,
    void* errorText, void* bodyText, void* headers, void* method);
static Fn_AuthRequestCompleted g_pfnOrigAuthRequestCompleted = nullptr;
static bool g_AuthRequestCompletedInstalled = false;

typedef void* (__fastcall *Fn_GetCompleted)();
static Fn_GetCompleted g_pfnTaskCompleted = nullptr;
typedef void (__fastcall *Fn_VoidOne)(void*, void*);
typedef void (__fastcall *Fn_VoidZero)(void*);
static Fn_VoidOne g_pfnOrigRemoteConfigAdd = nullptr;
static Fn_VoidZero g_pfnOrigValidateDependencies = nullptr;

static void __fastcall Hooked_RemoteConfigAdd(void* pThis, void*)
{
    LOG("[Auth] RemoteConfig FetchCompleted registration skipped");
}

static void __fastcall Hooked_ValidateDependencies(void* pThis)
{
    LOG("[Auth] CloudCode dependency validation skipped");
}

typedef void* (__fastcall *Fn_GetObject)(void* pThis);
typedef void  (__fastcall *Fn_SetString)(void* pThis, void* value);
typedef void  (__fastcall *Fn_SetState)(void* pThis, int value);
typedef void  (__fastcall *Fn_SetObject)(void* pThis, void* value);
typedef void  (__fastcall *Fn_PlayerInfoCtor)(void* pThis, void* playerId);

static Fn_GetObject      g_pfnGetPlayerIdComponent = nullptr;
static Fn_GetObject      g_pfnGetAccessTokenComponent = nullptr;
static Fn_GetObject      g_pfnGetSessionTokenComponent = nullptr;
static Fn_SetString      g_pfnSetPlayerId = nullptr;
static Fn_SetString      g_pfnSetAccessToken = nullptr;
static Fn_SetString      g_pfnSetSessionToken = nullptr;
static Fn_SetState       g_pfnSetState = nullptr;
static Fn_SetObject      g_pfnSetPlayerInfo = nullptr;
static Fn_PlayerInfoCtor g_pfnPlayerInfoCtor = nullptr;
static Il2CppClass*      g_pPlayerInfoClass = nullptr;

static bool CompleteLocalSignIn(void* pThis)
{
    if (!pThis || !g_pfnTaskCompleted || !g_pfnGetPlayerIdComponent ||
        !g_pfnGetAccessTokenComponent || !g_pfnGetSessionTokenComponent ||
        !g_pfnSetPlayerId || !g_pfnSetAccessToken || !g_pfnSetSessionToken ||
        !g_pfnSetState)
        return false;

    char playerId[64] = {};
    _snprintf_s(playerId, sizeof(playerId), _TRUNCATE, "uco2-%u-%lu",
        g_OriginalAppId ? g_OriginalAppId : g_ForcedAppId, GetCurrentProcessId());
    void* id = IL2CPP_StringNew(playerId);
    void* accessToken = IL2CPP_StringNew(
        "eyJhbGciOiJub25lIn0.eyJzdWIiOiJ1Y28yLWxvY2FsIiwiaWF0IjoxNzAwMDAwMDAwLCJleHAiOjQxMDAwMDAwMDB9.c2ln");
    if (!id || !accessToken) return false;

    void* playerIdComponent = g_pfnGetPlayerIdComponent(pThis);
    void* accessTokenComponent = g_pfnGetAccessTokenComponent(pThis);
    void* sessionTokenComponent = g_pfnGetSessionTokenComponent(pThis);
    if (!playerIdComponent || !accessTokenComponent || !sessionTokenComponent)
        return false;

    g_pfnSetPlayerId(playerIdComponent, id);
    g_pfnSetAccessToken(accessTokenComponent, accessToken);
    // Do not invent a server session token. Leaving this empty prevents UGS
    // from attempting a refresh with a value the service cannot validate.
    g_pfnSetSessionToken(sessionTokenComponent, nullptr);

    if (g_pPlayerInfoClass && g_pfnPlayerInfoCtor && g_pfnSetPlayerInfo)
    {
        Il2CppObject* info = IL2CPP_ObjectNew(g_pPlayerInfoClass);
        if (info)
        {
            g_pfnPlayerInfoCtor(info, id);
            g_pfnSetPlayerInfo(pThis, info);
        }
    }

    // AuthenticationState.Authorized is value 2 in UGS Authentication 3.x.
    g_pfnSetState(pThis, 2);
    LOG("[Auth] local Unity session authorized as %s", playerId);
    return true;
}

static void* __fastcall Hooked_LocalSignIn(void* pThis, void*, void*, void*)
{
    LOG("[Auth] Unity sign-in intercepted -> local authorized session");
    if (CompleteLocalSignIn(pThis)) return g_pfnTaskCompleted();
    LOG("[Auth] local sign-in setup incomplete");
    return nullptr;
}

static void* __fastcall Hooked_SignInWithSteam(void* pThis, void* a1, void* a2, void* a3) {
    LOG("[Auth] SignInWithSteam intercepted -> SignInAnonymouslyAsync");
    Il2CppObject* task = IL2CPP_InvokeObjectOne((Il2CppObject*)pThis,
        "SignInAnonymouslyAsync", nullptr);
    if (task) return task;

    LOG("[Auth] anonymous sign-in invocation failed; using original Steam sign-in");
    return g_pfnOrigSignInSteam[2]
        ? g_pfnOrigSignInSteam[2](pThis, a1, a2, a3) : nullptr;
}
static void* __fastcall Hooked_LinkSteam(void* pThis, void* a1, void* a2, void* a3) {
    LOG("[Auth] LinkWithSteam intercepted -> CompletedTask");
    if (g_pfnTaskCompleted) return g_pfnTaskCompleted();
    return nullptr; // Not installed unless CompletedTask resolved.
}
static void* __fastcall Hooked_UpdatePlayer(void* pThis, void* a1, void* a2, void* a3) {
    LOG("[Auth] UpdatePlayerName intercepted -> CompletedTask");
    if (g_pfnTaskCompleted) return g_pfnTaskCompleted();
    return nullptr;
}
static void* __fastcall Hooked_RefreshToken(void* pThis, void* a1, void* a2, void* a3) {
    LOG("[Auth] RefreshAccessToken intercepted -> CompletedTask");
    if (g_pfnTaskCompleted) return g_pfnTaskCompleted();
    return nullptr;
}
static void* __fastcall Hooked_HandleSignInRefresh(void* pThis, void* a1, void* a2, void* a3) {
    LOG("[Auth] HandleSignInRefreshRequest intercepted -> CompletedTask");
    if (g_pfnTaskCompleted) return g_pfnTaskCompleted();
    return nullptr;
}
static void* __fastcall Hooked_HandleSignInRequest(void* pThis, void* a1, void* a2, void* a3) {
    LOG("[Auth] HandleSignInRequest intercepted -> CompletedTask");
    if (g_pfnTaskCompleted) return g_pfnTaskCompleted();
    return nullptr;
}
// CloudEvents.SelfReport is static and takes only the detection string.
// Including a fake instance/extra arguments prevents the detour from matching
// the IL2CPP calling convention reliably on x64.
static void* __fastcall Hooked_SelfReport(void* detection) {
    LOG("[Auth] CloudEvents.SelfReport intercepted -> CompletedTask");
    Il2CppObject* task = IL2CPP_InvokeStaticZero("mscorlib",
        "System.Threading.Tasks", "Task", "get_CompletedTask");
    if (task)
    {
        LOG("[Auth] CloudEvents.SelfReport -> returning managed CompletedTask");
        return task;
    }
    return g_pfnOrigSelfReport ? g_pfnOrigSelfReport(detection) : nullptr;
}

static void __fastcall Hooked_AuthRequestCompleted(void* pThis, void* tcs,
    int64_t responseCode, bool isNetworkError, bool isServerError,
    void* errorText, void* bodyText, void* headers, void* method)
{
    if (responseCode == 401)
    {
        // The UGS client decodes idToken immediately after deserializing this
        // response. Keep the token structurally valid and include the claims
        // its AccessToken model reads instead of returning a placeholder JWT.
        static const char kLocalSignInResponse[] =
            "{\"idToken\":\"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
            "eyJhdWQiOlsiYjRjY2JlZTktODZiMy00MWUwLWE2NjctZmJiMjhmOWQ3YzYwIl0s"
            "ImlhdCI6MTcwNDA2NzIwMCwic3ViIjoidWNvMi1sb2NhbCIsImV4cCI6NDEwMjQ0NDgwMH0."
            "c2ln\",\"sessionToken\":\"uco2-local-session\","
            "\"user\":{\"id\":\"uco2-local\",\"createdAt\":\"2024-01-01T00:00:00Z\","
            "\"externalIds\":[],\"username\":\"uco2\"},\"lastNotificationDate\":\"\"}";
        void* localResponse = IL2CPP_StringNew(kLocalSignInResponse);
        if (localResponse && IL2CPP_InvokeVoidOne((Il2CppObject*)tcs,
            "SetResult", (Il2CppObject*)localResponse))
        {
            LOG("[Auth] WebRequest.RequestCompleted completed local sign-in task");
            return;
        }
        LOG("[Auth] failed to complete local sign-in task; using original 401 response");
        if (g_pfnOrigAuthRequestCompleted)
            g_pfnOrigAuthRequestCompleted(pThis, tcs, responseCode,
                isNetworkError, isServerError, errorText, bodyText, headers,
                method);
        return;
    }
    if (g_pfnOrigAuthRequestCompleted)
        g_pfnOrigAuthRequestCompleted(pThis, tcs, responseCode, isNetworkError,
            isServerError, errorText, bodyText, headers, method);
}

static bool TryInstallEarlySelfReportHook()
{
    if (g_SelfReportInstalled || !g_pfnTaskCompleted) return g_SelfReportInstalled;

    void* fn = IL2CPP_FindMethodPtr("Assembly-CSharp", "", "CloudEvents", "SelfReport", -1);
    if (!fn)
        fn = IL2CPP_FindMethodPtr("Assembly-CSharp-firstpass", "", "CloudEvents", "SelfReport", -1);
    if (!fn) return false;

    if (MH_CreateHook(fn, (void*)&Hooked_SelfReport,
        (void**)&g_pfnOrigSelfReport) == MH_OK &&
        MH_EnableHook(fn) == MH_OK)
    {
        g_SelfReportInstalled = true;
        LOG("[Auth] CloudEvents.SelfReport early hook @ %p", fn);
        return true;
    }
    return false;
}

static bool TryInstallEarlyAuthRequestHook()
{
    if (g_AuthRequestCompletedInstalled) return true;

    void* fn = IL2CPP_FindMethodPtr(nullptr, "Unity.Services.Authentication",
        "WebRequest", "RequestCompleted", 7);
    if (!fn) return false;

    if (MH_CreateHook(fn, (void*)&Hooked_AuthRequestCompleted,
        (void**)&g_pfnOrigAuthRequestCompleted) == MH_OK &&
        MH_EnableHook(fn) == MH_OK)
    {
        g_AuthRequestCompletedInstalled = true;
        LOG("[Auth] Authentication.WebRequest.RequestCompleted hook @ %p", fn);
        return true;
    }
    return false;
}

static int HookAuthOverloads(const char* methodName, void* detour, Fn_Task originals[5])
{
    void* hooked[5] = {};
    int hookedCount = 0;
    for (int argc = 0; argc <= 4; ++argc)
    {
        void* fn = IL2CPP_FindMethodPtr(nullptr, "Unity.Services.Authentication",
            "AuthenticationServiceInternal", methodName, argc);
        if (!fn) continue;

        bool duplicate = false;
        for (int i = 0; i < hookedCount; ++i)
            if (hooked[i] == fn) { duplicate = true; break; }
        if (duplicate) continue;

        if (MH_CreateHook(fn, detour, (void**)&originals[argc]) == MH_OK &&
            MH_EnableHook(fn) == MH_OK)
        {
            hooked[hookedCount++] = fn;
            LOG("[Auth] %s overload argc=%d hook @ %p", methodName, argc, fn);
        }
        else
        {
            LOG("[Auth] failed to hook %s overload argc=%d @ %p", methodName, argc, fn);
        }
    }
    return hookedCount;
}

static bool TryInstall()
{
    if (!IL2CPP_IsReady()) return false;

    if (!g_SelfReportInstalled)
    {
        void* selfReport = IL2CPP_FindMethodPtr("Assembly-CSharp", "",
            "CloudEvents", "SelfReport", 1);
        if (selfReport &&
            MH_CreateHook(selfReport, (void*)&Hooked_SelfReport,
                (void**)&g_pfnOrigSelfReport) == MH_OK &&
            MH_EnableHook(selfReport) == MH_OK)
        {
            g_SelfReportInstalled = true;
            LOG("[Auth] managed CloudEvents.SelfReport hook @ %p", selfReport);
        }
    }

    if (g_SteamSignInInstalled) return g_SelfReportInstalled;
    if (!IL2CPP_FindClass("Unity.Services.Authentication",
        "Unity.Services.Authentication", "AuthenticationServiceInternal"))
        return false;

    void* steamSignIn = IL2CPP_FindMethodPtr(nullptr,
        "Unity.Services.Authentication", "AuthenticationServiceInternal",
        "SignInWithSteamAsync", 2);
    if (!steamSignIn) return false;

    if (MH_CreateHook(steamSignIn, (void*)&Hooked_SignInWithSteam,
        (void**)&g_pfnOrigSignInSteam[2]) != MH_OK ||
        MH_EnableHook(steamSignIn) != MH_OK)
        return false;

    g_SteamSignInInstalled = true;
    LOG("[Auth] managed SignInWithSteamAsync -> anonymous sign-in hook @ %p",
        steamSignIn);
    return true;
}
} // namespace ModUnityAuth


// ============================================================
// MODULE: Phasmophobia SteamAuth gate NOP (IL2CPP, game-specific)
//
// NOPs the je at GameAssembly+0xEAF7CE that gates the "Failed to
// get Steam account information" error branch. Byte-verifies
// before patching so non-Phasmo or new-build games are skipped
// cleanly.
// ============================================================
namespace ModPhasmoGate {

// ---- Robust, build-INDEPENDENT primary: hook Phasmo's SteamAuth method ----
// The NOP below is keyed to one build's RVA and breaks on every Phasmo update.
// This instead SigScans the il2cpp section for the SteamAuth method prologue
// (wildcarded on the RVA-dependent bytes only) and hooks it to return null,
// short-circuiting the ticket fetch/POST and the "Failed to get Steam account
// information" failure. Beebyte renames symbols, not the compiler-emitted
// prologue, so this survives updates -- confirmed on builds 23249745 (RVA
// 0x42CA4A0) and 24434979 (RVA 0x43D9AC0). Ported from unity_auth_bypass.
struct PhSigByte { uint8_t v; bool wild; };
static const PhSigByte kSteamAuthSig[] = {
    {0x48,0},{0x89,0},{0x6C,0},{0x24,0},{0x18,0},          // mov [rsp+0x18], rbp
    {0x56,0},{0x57,0},{0x41,0},{0x56,0},                   // push rsi; rdi; r14
    {0x48,0},{0x83,0},{0xEC,0},{0x40,0},                   // sub rsp, 0x40
    {0x80,0},{0x3D,0},{0,1},{0,1},{0,1},{0,1},{0x00,0},    // cmp byte[rip+RVA], 0
    {0x49,0},{0x8B,0},{0xF9,0},{0x49,0},{0x8B,0},{0xE8,0}, // mov rdi,r9; rbp,r8
    {0x4C,0},{0x8B,0},{0xF2,0},{0x48,0},{0x8B,0},{0xF1,0}, // mov r14,rdx; rsi,rcx
    {0x75,0},{0x67,0},                                     // jne +0x67
};
static uint8_t* PhSigScan(uint8_t* base, size_t size, const PhSigByte* sig, size_t n)
{
    if (size < n) return nullptr;
    for (size_t i = 0; i <= size - n; ++i) {
        bool ok = true;
        for (size_t j = 0; j < n; ++j)
            if (!sig[j].wild && base[i + j] != sig[j].v) { ok = false; break; }
        if (ok) return base + i;
    }
    return nullptr;
}
static bool PhGetSection(HMODULE mod, const char* name, uint8_t** ob, size_t* os)
{
    if (!mod) return false;
    uint8_t* img = (uint8_t*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)img;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(img + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    char p[9] = {};
    strncpy_s(p, sizeof(p), name, 8);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (memcmp(sec[i].Name, p, 8) == 0) {
            *ob = img + sec[i].VirtualAddress;
            *os = sec[i].Misc.VirtualSize;
            return true;
        }
    return false;
}
typedef void (__fastcall *Fn_PhSteamAuth)(void*, void*, void*, void*);
static Fn_PhSteamAuth g_origPhSteamAuth = nullptr;
static volatile LONG  g_PhSteamAuthHooked = 0;
static void __fastcall Hooked_PhSteamAuth(void*, void*, void*, void*)
{
    LOG("[Phasmo] SteamAuth intercepted -> auth gate skipped");
}
static bool TryHookSteamAuth()
{
    if (InterlockedCompareExchange(&g_PhSteamAuthHooked, 0, 0)) return true;
    HMODULE ga = GetModuleHandleA("GameAssembly.dll");
    if (!ga) return false;
    uint8_t* sb = nullptr; size_t ss = 0;
    if (!PhGetSection(ga, "il2cpp", &sb, &ss)) return false;   // not IL2CPP
    uint8_t* hit = PhSigScan(sb, ss, kSteamAuthSig,
                             sizeof(kSteamAuthSig) / sizeof(kSteamAuthSig[0]));
    if (!hit) return false;   // not Phasmo, or prologue drifted
    if (MH_CreateHook(hit, (void*)&Hooked_PhSteamAuth, (void**)&g_origPhSteamAuth) != MH_OK ||
        MH_EnableHook(hit) != MH_OK)
        return false;
    InterlockedExchange(&g_PhSteamAuthHooked, 1);
    LOG("[Phasmo] SteamAuth hook installed at GameAssembly+0x%llx (build-independent SigScan)",
        (unsigned long long)(hit - (uint8_t*)ga));
    return true;
}

// ---- Fallback: NOP the known-build je (silent-skips on other/new builds) ----
static bool TryNopGate()
{
    HMODULE ga = GetModuleHandleA("GameAssembly.dll");
    if (!ga) return false;
    const uintptr_t kPatchRva = 0xEAF7CE;   // build 23249745 only
    uint8_t* site = (uint8_t*)ga + kPatchRva;
    const uint8_t kExpected[6] = { 0x0F, 0x84, 0xC3, 0x06, 0x00, 0x00 };
    if (memcmp(site, kExpected, 6) != 0) return false;   // drifted -> skip
    DWORD oldProt = 0;
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProt)) {
        LOG("[Phasmo] VirtualProtect failed GLE=%lu", GetLastError());
        return false;
    }
    for (int i = 0; i < 6; ++i) site[i] = 0x90;
    DWORD tmp = 0;
    VirtualProtect(site, 6, oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    LOG("[Phasmo] SteamAccountGate NOPed at GameAssembly+0x%llx (build-specific fallback)",
        (unsigned long long)kPatchRva);
    return true;
}

static bool TryInstall()
{
    if (!GetModuleHandleA("GameAssembly.dll")) return false;
    // Primary (build-independent) first; NOP is a same-build belt-and-suspenders.
    bool any = TryHookSteamAuth();
    if (TryNopGate()) any = true;
    return any;
}
} // namespace ModPhasmoGate
#endif // UCO_PHASMO_EXPERIMENTAL


// ============================================================
// ORCHESTRATOR
// ============================================================
static volatile LONG g_RealtimeIL2CPP_Done = 0;
static volatile LONG g_RealtimeMono_Done   = 0;
static volatile LONG g_Fusion_Done         = 0;
#ifdef UCO_PHASMO_EXPERIMENTAL
static volatile LONG g_UnityAuth_Done      = 0;
static volatile LONG g_PhasmoGate_Done     = 0;
#endif

static void RunDetectionPass()
{
#ifdef UCO_PHASMO_EXPERIMENTAL
    if (!InterlockedCompareExchange(&g_PhasmoGate_Done, 0, 0))
        if (ModPhasmoGate::TryInstall())     InterlockedExchange(&g_PhasmoGate_Done, 1);
#endif

    // IL2CPP-based modules
    if (IL2CPP_TryInit()) {
        if (!InterlockedCompareExchange(&g_RealtimeIL2CPP_Done, 0, 0))
            if (ModRealtimeIL2CPP::TryInstall()) InterlockedExchange(&g_RealtimeIL2CPP_Done, 1);
        if (!InterlockedCompareExchange(&g_Fusion_Done, 0, 0))
            if (ModFusion::TryInstall())         InterlockedExchange(&g_Fusion_Done, 1);
#ifdef UCO_PHASMO_EXPERIMENTAL
        if (!InterlockedCompareExchange(&g_UnityAuth_Done, 0, 0))
            if (ModUnityAuth::TryInstall())      InterlockedExchange(&g_UnityAuth_Done, 1);
#endif
    }
    // Mono-based module
    if (MONO_TryInit()) {
        if (!InterlockedCompareExchange(&g_RealtimeMono_Done, 0, 0))
            if (ModRealtimeMono::TryInstall()) InterlockedExchange(&g_RealtimeMono_Done, 1);
    }
}

static DWORD WINAPI WatcherProc(LPVOID)
{
    // Poll up to ~2 minutes for runtime + Photon classes to be loaded.
    for (int i = 0; i < 600 && InterlockedCompareExchange(&g_bShutdown, 0, 0) == 0; ++i) {
        RunDetectionPass();
        // If everything that wanted to activate has activated, we can stop.
        // But we don't know what "should" activate, so just keep polling
        // until shutdown or timeout. Cheap.
        Sleep(200);
    }
    return 0;
}

extern "C" __declspec(dllexport) int __cdecl UCO_PluginInit(const UCO_PluginContext* ctx)
{
    if (!ctx) return 1;
    if (ctx->ApiVersion != UCO_PLUGIN_API_VERSION) return 2;
    g_Log           = ctx->Log;
    g_ForcedAppId   = ctx->ForcedAppId;
    g_OriginalAppId = ctx->OriginalAppId;

#ifdef UCO_PHASMO_EXPERIMENTAL
    if (!g_hCrashCapture)
        g_hCrashCapture = AddVectoredExceptionHandler(1,
            &CaptureUnhandledAccessViolation);
#endif

#ifdef UCO_PHASMO_EXPERIMENTAL
    LOG("[PhasmoExperimental] plugin init: AppId=%u ogAppId=%u",
        g_ForcedAppId, g_OriginalAppId);
#else
    LOG("[Universal] photon_universal plugin init: AppId=%u ogAppId=%u",
        g_ForcedAppId, g_OriginalAppId);
#endif

    const char* ini = GetIniPath();
    if (ini) {
        ModRealtimeIL2CPP::ReadIni(ini);
        ModRealtimeMono::ReadIni(ini);
        ModFusion::ReadIni(ini);
    } else {
        LOG("[Universal] no union-crax.ini found");
    }

    if (MH_Initialize() != MH_OK)
        LOG("[Universal] MH_Initialize non-OK (already inited?)");

    g_hWatcherThread = CreateThread(nullptr, 0, WatcherProc, nullptr, 0, nullptr);
    return 0;
}

extern "C" __declspec(dllexport) void __cdecl UCO_PluginShutdown(void)
{
    InterlockedExchange(&g_bShutdown, 1);
    if (g_hWatcherThread) {
        WaitForSingleObject(g_hWatcherThread, 1000);
        CloseHandle(g_hWatcherThread);
        g_hWatcherThread = nullptr;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
#ifdef UCO_PHASMO_EXPERIMENTAL
    if (g_hCrashCapture) {
        RemoveVectoredExceptionHandler(g_hCrashCapture);
        g_hCrashCapture = nullptr;
    }
#endif
    LOG("[Universal] plugin shutdown");
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
