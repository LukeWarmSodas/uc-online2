// ============================================================
// UCOnline2 plugin -- EOS_custom
//
// Lets an EOS game authenticate against Epic's REAL backend using
// YOUR own free Epic app, via an anonymous EOS Connect Device ID
// login. Co-op then runs on Epic's own relay -- no LAN, no VPN and
// no EOS emulator.
//
// THE PROBLEM
//   These games log into EOS with a Steam session ticket. Epic
//   validates that ticket server-side against Steam, which always
//   fails for an emulated ticket. From the game's own UE log:
//     External credential 'STEAM_SESSION_TICKET' failed to
//     authenticate with EOS Connect:
//     EOS_Connect_ExternalTokenValidationFailed
//   With no EOS identity there is no ProductUserId, so the session
//   layer refuses to host:
//     HostingPlayerNum provided to CreateSession does not have
//     online identity.
//   No session is created, so nothing is advertised to Steam: the
//   "Join Game" button never appears and joiners find nothing.
//
// THE FIX -- two hooks on the genuine Epic EOSSDK-Win64-Shipping.dll
//   1. EOS_Platform_Create: rewrite ProductId / SandboxId /
//      DeploymentId / ClientId / ClientSecret to OUR Epic app so
//      every player lands in the same session pool.
//   2. EOS_Connect_Login: replace the doomed STEAM_SESSION_TICKET
//      credential (type 18) with DEVICEID_ACCESS_TOKEN (type 10).
//      Device ID login is anonymous -- there is no external platform
//      for Epic to validate -- so it succeeds and yields a real
//      ProductUserId. It also REQUIRES UserLoginInfo.DisplayName.
//   Device ID login needs a device id to exist first, so we call
//   EOS_Connect_CreateDeviceId once on the first attempt and let the
//   game's own login retry loop succeed on a later pass.
//
// The EOS SDK can load from a subdir (Forever Skies:
// ProjectZeppelin\Binaries\Win64\RedpointEOS\EOSSDK-Win64-Shipping.dll)
// and typically loads BEFORE UCO_PluginInit runs, so the watcher
// thread starts from DllMain, waits for the module, resolves the
// exports and installs the hooks. Until the host hands us its
// logger, a fallback writes straight to %TEMP%\uc_online2.log.
//
// The EOS struct layouts below are trimmed to the fields we touch and
// pinned to the EOS SDK 1.15.x ABI (Forever Skies ships 1.15.5). Only
// leading fields are used, which are stable across minor versions.
//
// MinHook statically linked (same as the other UCOnline2 plugins).
// ============================================================
#include <Windows.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../../include/MinHook.h"
#include "../../include/uco_plugin.h"

// ------------------------------------------------------------
// Minimal EOS types (SDK 1.15.x). Only what we need to LOG.
// ------------------------------------------------------------
typedef void* EOS_HPlatform;
typedef void* EOS_HConnect;
typedef int32_t EOS_EResult;

// EOS_ELoginCredentialType / EOS_EExternalCredentialType values we
// might see (from eos_connect_types.h / eos_common.h, 1.15.x):
//   EOS_ECT_EPIC                       = 0
//   EOS_ECT_STEAM_APP_TICKET           = 1
//   EOS_ECT_PSN_ID_TOKEN               = 2
//   EOS_ECT_XBL_XSTS_TOKEN             = 3
//   EOS_ECT_DISCORD_ACCESS_TOKEN       = 4
//   EOS_ECT_GOG_SESSION_TICKET         = 5
//   EOS_ECT_NINTENDO_ID_TOKEN          = 6
//   EOS_ECT_NINTENDO_NSA_ID_TOKEN      = 7
//   EOS_ECT_UPLAY_ACCESS_TOKEN         = 8
//   EOS_ECT_OPENID_ACCESS_TOKEN        = 9
//   EOS_ECT_DEVICEID_ACCESS_TOKEN      = 10
//   EOS_ECT_APPLE_ID_TOKEN             = 11
//   EOS_ECT_GOOGLE_ID_TOKEN            = 12
//   EOS_ECT_OCULUS_USERID_NONCE        = 13
//   EOS_ECT_ITCHIO_JWT                 = 14
//   EOS_ECT_ITCHIO_KEY                 = 15
//   EOS_ECT_EPIC_ID_TOKEN              = 16
//   EOS_ECT_AMAZON_ACCESS_TOKEN        = 17
//   EOS_ECT_STEAM_SESSION_TICKET       = 18
static const char* ExternalCredTypeName(int t)
{
    switch (t) {
        case 0:  return "EPIC";
        case 1:  return "STEAM_APP_TICKET";
        case 2:  return "PSN_ID_TOKEN";
        case 3:  return "XBL_XSTS_TOKEN";
        case 4:  return "DISCORD_ACCESS_TOKEN";
        case 5:  return "GOG_SESSION_TICKET";
        case 6:  return "NINTENDO_ID_TOKEN";
        case 7:  return "NINTENDO_NSA_ID_TOKEN";
        case 8:  return "UPLAY_ACCESS_TOKEN";
        case 9:  return "OPENID_ACCESS_TOKEN";
        case 10: return "DEVICEID_ACCESS_TOKEN";
        case 11: return "APPLE_ID_TOKEN";
        case 12: return "GOOGLE_ID_TOKEN";
        case 13: return "OCULUS_USERID_NONCE";
        case 14: return "ITCHIO_JWT";
        case 15: return "ITCHIO_KEY";
        case 16: return "EPIC_ID_TOKEN";
        case 17: return "AMAZON_ACCESS_TOKEN";
        case 18: return "STEAM_SESSION_TICKET";
        default: return "UNKNOWN";
    }
}

// EOS_Connect_Credentials (eos_connect_types.h, ApiVersion 1)
//   int32 ApiVersion;
//   const char* Token;
//   EOS_EExternalCredentialType Type;
struct EOS_Connect_Credentials {
    int32_t     ApiVersion;
    const char* Token;
    int32_t     Type;
};

// EOS_Connect_UserLoginInfo (optional; used for DeviceID display name)
//   int32 ApiVersion; const char* DisplayName; (+ later versions add more)
struct EOS_Connect_UserLoginInfo {
    int32_t     ApiVersion;
    const char* DisplayName;
};

// EOS_Connect_LoginOptions
//   int32 ApiVersion;
//   const EOS_Connect_Credentials* Credentials;
//   const EOS_Connect_UserLoginInfo* UserLoginInfo;
struct EOS_Connect_LoginOptions {
    int32_t                            ApiVersion;
    const EOS_Connect_Credentials*     Credentials;
    const EOS_Connect_UserLoginInfo*   UserLoginInfo;
};

// EOS_Platform_Options -- we only read the leading id string fields.
// Layout (1.15.x, x64), offsets validated by ApiVersion sanity check:
//   int32 ApiVersion;            (+0)
//   [4 pad]                      (+4)
//   void* Reserved;              (+8)
//   const char* ProductId;       (+16)
//   const char* SandboxId;       (+24)
//   EOS_Platform_ClientCredentials ClientCredentials; { const char* ClientId; const char* ClientSecret; }  (+32, +40)
//   EOS_Bool bIsServer;          (+48)
//   const char* EncryptionKey;   (+56)
//   const char* OverrideCountryCode; (+64)
//   const char* OverrideLocaleCode;  (+72)
//   const char* DeploymentId;    (+80)
// We read these by offset defensively (only if ApiVersion looks sane).
struct EOS_Platform_ClientCredentials {
    const char* ClientId;
    const char* ClientSecret;
};

// ------------------------------------------------------------
// Config: our own Epic app, from union-crax.ini [EOS].
// These replace whatever the game was built with, so both players
// end up on OUR deployment instead of the publisher's.
// ------------------------------------------------------------
struct EosConfig
{
    char ProductId[64];
    char SandboxId[64];
    char DeploymentId[64];
    char ClientId[128];
    char ClientSecret[256];
    char DisplayName[64];   // required by Device ID login
    bool bValid;
};
static EosConfig g_Cfg = {};

static void LoadEosConfig()
{
    char ini[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(nullptr, ini, MAX_PATH);
    if (!len) return;
    for (int i = (int)len - 1; i >= 0; --i)
        if (ini[i] == '\\' || ini[i] == '/') { ini[i] = 0; break; }
    strncat_s(ini, MAX_PATH, "\\union-crax.ini", _TRUNCATE);

    GetPrivateProfileStringA("EOS", "ProductId",    "", g_Cfg.ProductId,    sizeof(g_Cfg.ProductId),    ini);
    GetPrivateProfileStringA("EOS", "SandboxId",    "", g_Cfg.SandboxId,    sizeof(g_Cfg.SandboxId),    ini);
    GetPrivateProfileStringA("EOS", "DeploymentId", "", g_Cfg.DeploymentId, sizeof(g_Cfg.DeploymentId), ini);
    GetPrivateProfileStringA("EOS", "ClientId",     "", g_Cfg.ClientId,     sizeof(g_Cfg.ClientId),     ini);
    GetPrivateProfileStringA("EOS", "ClientSecret", "", g_Cfg.ClientSecret, sizeof(g_Cfg.ClientSecret), ini);
    GetPrivateProfileStringA("EOS", "DisplayName",  "Player", g_Cfg.DisplayName, sizeof(g_Cfg.DisplayName), ini);

    g_Cfg.bValid = g_Cfg.ProductId[0] && g_Cfg.SandboxId[0] && g_Cfg.DeploymentId[0] &&
                   g_Cfg.ClientId[0] && g_Cfg.ClientSecret[0];
}

// ------------------------------------------------------------
// Plugin state
// ------------------------------------------------------------
static UCO_LogFn g_Log = nullptr;

// Fallback logger: the EOS calls we care about happen during game
// startup, BEFORE the host hands us ctx->Log. Until then we append
// to %TEMP%\uc_online2.log ourselves so nothing is lost.
static void FallbackLog(const char* fmt, va_list ap)
{
    char path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, path)) return;
    strncat_s(path, MAX_PATH, "uc_online2.log", _TRUNCATE);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || !f) return;

    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    vfprintf(f, fmt, ap);
    fputs("\n", f);
    fclose(f);
}

static void EosLog(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    if (g_Log) {
        char buf[1024];
        _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
        g_Log("%s", buf);
    } else {
        FallbackLog(fmt, ap);
    }
    va_end(ap);
}

#define LOG(...) EosLog(__VA_ARGS__)

static HANDLE        g_hWatcher   = nullptr;
static volatile LONG g_bShutdown  = 0;

typedef EOS_HPlatform (EOS_CALL_PLACEHOLDER)(void);
typedef EOS_HPlatform (__cdecl* Fn_EOS_Platform_Create)(const void* Options);
typedef void          (__cdecl* Fn_EOS_Connect_Login)(EOS_HConnect Handle, const void* Options,
                                                      void* ClientData, void* CompletionDelegate);

static Fn_EOS_Platform_Create g_orig_Platform_Create = nullptr;
static Fn_EOS_Connect_Login   g_orig_Connect_Login   = nullptr;

static const char* SafeStr(const char* s) { return s ? s : "(null)"; }

// ------------------------------------------------------------
// Hooked EOS_Platform_Create -- log the product/sandbox/deployment/client
// the game initializes with, then call through unchanged.
// ------------------------------------------------------------
static EOS_HPlatform __cdecl Hooked_Platform_Create(const void* Options)
{
    if (Options) {
        uint8_t* p = (uint8_t*)Options;   // writable: we redirect the ids in place
        int32_t apiver = *(const int32_t*)(p + 0);

        const char** ppProductId    = (const char**)(p + 16);
        const char** ppSandboxId    = (const char**)(p + 24);
        EOS_Platform_ClientCredentials* cc = (EOS_Platform_ClientCredentials*)(p + 32);
        const char** ppDeploymentId = (const char**)(p + 80);

        LOG("[EOSAuth] EOS_Platform_Create (ApiVersion=%d) game values:", apiver);
        LOG("[EOSAuth]   ProductId=%s SandboxId=%s DeploymentId=%s",
            SafeStr(*ppProductId), SafeStr(*ppSandboxId), SafeStr(*ppDeploymentId));
        LOG("[EOSAuth]   ClientId=%s", SafeStr(cc ? cc->ClientId : nullptr));

        if (g_Cfg.bValid) {
            // Point the platform at OUR Epic app. Both players must use the
            // same ids or they end up in different session pools.
            *ppProductId    = g_Cfg.ProductId;
            *ppSandboxId    = g_Cfg.SandboxId;
            *ppDeploymentId = g_Cfg.DeploymentId;
            if (cc) {
                cc->ClientId     = g_Cfg.ClientId;
                cc->ClientSecret = g_Cfg.ClientSecret;
            }
            LOG("[EOSAuth]   -> REDIRECTED to Product=%s Sandbox=%s Deployment=%s Client=%s",
                g_Cfg.ProductId, g_Cfg.SandboxId, g_Cfg.DeploymentId, g_Cfg.ClientId);
        } else {
            LOG("[EOSAuth]   -> NOT redirected: [EOS] section in union-crax.ini is incomplete");
        }
    } else {
        LOG("[EOSAuth] EOS_Platform_Create: Options=null");
    }
    return g_orig_Platform_Create ? g_orig_Platform_Create(Options) : nullptr;
}

// ------------------------------------------------------------
// Hooked EOS_Connect_Login -- the important one. Log the credential
// TYPE + whether a token is present, then call through unchanged.
// ------------------------------------------------------------
static const int EOS_ECT_DEVICEID_ACCESS_TOKEN = 10;

// EOS_Connect_CreateDeviceIdOptions (eos_connect_types.h, ApiVersion 1)
struct EOS_Connect_CreateDeviceIdOptions
{
    int32_t     ApiVersion;
    const char* DeviceModel;
};

typedef void (__cdecl* Fn_EOS_Connect_CreateDeviceId)(EOS_HConnect Handle, const void* Options,
                                                     void* ClientData, void* CompletionDelegate);
static Fn_EOS_Connect_CreateDeviceId g_pfn_CreateDeviceId = nullptr;
static volatile LONG g_bDeviceIdRequested = 0;

// EOS requires a non-null completion delegate; nothing to do with the result.
// EOS_Success and EOS_DuplicateNotAllowed both mean "a device id now exists".
static void __cdecl OnCreateDeviceIdCb(const void* /*Data*/)
{
    LOG("[EOSAuth] CreateDeviceId callback fired (device id should now exist).");
}

// Device ID login needs a device id to already exist, otherwise it returns
// EOS_NotFound. Rather than build an async chain, we kick off CreateDeviceId on
// the first login attempt and lean on the game's own retry loop -- the UE log
// shows it re-attempts Connect_Login every ~4s, and by then the id exists.
static void EnsureDeviceId(EOS_HConnect Handle)
{
    if (!g_pfn_CreateDeviceId || !Handle) return;
    if (InterlockedExchange(&g_bDeviceIdRequested, 1) != 0) return;   // once only

    EOS_Connect_CreateDeviceIdOptions opts = {};
    opts.ApiVersion  = 1;
    opts.DeviceModel = "PC Windows 64-bit";

    LOG("[EOSAuth] Requesting EOS_Connect_CreateDeviceId (needed before Device ID login)...");
    g_pfn_CreateDeviceId(Handle, &opts, nullptr, (void*)&OnCreateDeviceIdCb);
}

// Storage for the rewritten options. The SDK copies what it needs during the
// call, but keep these alive for the call's duration regardless.
static EOS_Connect_Credentials   g_DevCreds    = {};
static EOS_Connect_UserLoginInfo g_DevLoginInf = {};

static void __cdecl Hooked_Connect_Login(EOS_HConnect Handle, const void* Options,
                                        void* ClientData, void* CompletionDelegate)
{
    if (Options) {
        EOS_Connect_LoginOptions* o = (EOS_Connect_LoginOptions*)Options;   // writable
        int t = o->Credentials ? o->Credentials->Type : -1;

        LOG("[EOSAuth] EOS_Connect_Login: OptionsApiVersion=%d Credentials.Type=%d (%s) Token=%s",
            o->ApiVersion, t, ExternalCredTypeName(t),
            (o->Credentials && o->Credentials->Token) ? "(present)" : "(null)");

        // An external platform credential (e.g. STEAM_SESSION_TICKET) is
        // validated server-side by Epic against that platform. An EMULATED
        // Steam ticket always fails there -- the game log shows
        //   External credential 'STEAM_SESSION_TICKET' failed to authenticate
        //   with EOS Connect: EOS_Connect_ExternalTokenValidationFailed
        // which leaves the player with no ProductUserId, so Redpoint then
        // refuses CreateSession ("does not have online identity") and no
        // session/lobby is ever advertised.
        //
        // Device ID login is anonymous: no external platform, no identity
        // provider, nothing for Epic to validate against Steam. It yields a
        // real ProductUserId on our own deployment, which is all the game
        // needs to create and advertise a session.
        if (g_Cfg.bValid && t != EOS_ECT_DEVICEID_ACCESS_TOKEN) {
            EnsureDeviceId(Handle);

            g_DevCreds.ApiVersion = o->Credentials ? o->Credentials->ApiVersion : 1;
            g_DevCreds.Token      = nullptr;                        // must be null for Device ID
            g_DevCreds.Type       = EOS_ECT_DEVICEID_ACCESS_TOKEN;

            // Device ID login REQUIRES UserLoginInfo with a DisplayName.
            g_DevLoginInf.ApiVersion  = o->UserLoginInfo ? o->UserLoginInfo->ApiVersion : 1;
            g_DevLoginInf.DisplayName = g_Cfg.DisplayName;

            o->Credentials   = &g_DevCreds;
            o->UserLoginInfo = &g_DevLoginInf;

            LOG("[EOSAuth]   -> REWROTE to DEVICEID_ACCESS_TOKEN (DisplayName=%s)", g_Cfg.DisplayName);
        }
    } else {
        LOG("[EOSAuth] EOS_Connect_Login: Options=null");
    }
    if (g_orig_Connect_Login)
        g_orig_Connect_Login(Handle, Options, ClientData, CompletionDelegate);
}

// ------------------------------------------------------------
// Watcher: wait for the EOS SDK module, resolve exports, hook them.
// ------------------------------------------------------------
static bool InstallHooks()
{
    HMODULE hEos = GetModuleHandleA("EOSSDK-Win64-Shipping.dll");
    if (!hEos) return false; // not loaded yet

    void* pPlatformCreate = (void*)GetProcAddress(hEos, "EOS_Platform_Create");
    void* pConnectLogin   = (void*)GetProcAddress(hEos, "EOS_Connect_Login");

    // Called (not hooked) to create the device id before Device ID login.
    g_pfn_CreateDeviceId = (Fn_EOS_Connect_CreateDeviceId)GetProcAddress(hEos, "EOS_Connect_CreateDeviceId");
    if (!g_pfn_CreateDeviceId)
        LOG("[EOSAuth] WARNING: EOS_Connect_CreateDeviceId not exported; Device ID login will fail with EOS_NotFound");
    if (!pPlatformCreate || !pConnectLogin) {
        LOG("[EOSAuth] EOS module loaded but exports missing "
            "(Platform_Create=%p Connect_Login=%p). Is this the real Epic SDK?",
            pPlatformCreate, pConnectLogin);
        return true; // stop retrying; wrong DLL
    }

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        LOG("[EOSAuth] MH_Initialize failed: %d", s);
        return true;
    }

    if (MH_CreateHook(pPlatformCreate, (void*)&Hooked_Platform_Create,
                      (void**)&g_orig_Platform_Create) == MH_OK)
        MH_EnableHook(pPlatformCreate);
    else
        LOG("[EOSAuth] hook EOS_Platform_Create failed");

    if (MH_CreateHook(pConnectLogin, (void*)&Hooked_Connect_Login,
                      (void**)&g_orig_Connect_Login) == MH_OK)
        MH_EnableHook(pConnectLogin);
    else
        LOG("[EOSAuth] hook EOS_Connect_Login failed");

    LOG("[EOSAuth] DIAGNOSTIC hooks installed (Platform_Create @ %p, Connect_Login @ %p). "
        "Launch + reach multiplayer, then send the log.",
        pPlatformCreate, pConnectLogin);
    return true;
}

static DWORD WINAPI WatcherProc(LPVOID)
{
    // Poll up to ~3 minutes for the EOS SDK to load (subdir load can be late).
    for (int i = 0; i < 900 && InterlockedCompareExchange(&g_bShutdown, 0, 0) == 0; ++i) {
        if (InstallHooks()) return 0;
        Sleep(200);
    }
    LOG("[EOSAuth] EOSSDK-Win64-Shipping.dll never loaded; nothing hooked.");
    return 0;
}

// ------------------------------------------------------------
// UCO plugin entry points
//
// TIMING NOTE: the game creates its EOS platform and does its
// initial Connect login during STARTUP -- often before the host
// calls UCO_PluginInit (which waits for SteamAPI_Init). If we only
// started watching here we'd miss both calls entirely. So the
// watcher is started from DllMain (earliest possible moment, when
// UCOnline2's steam_api64.dll is loaded) and UCO_PluginInit just
// upgrades us to the host's logger once it's available.
// ------------------------------------------------------------
extern "C" __declspec(dllexport) int UCO_PluginInit(const UCO_PluginContext* ctx)
{
    if (!ctx || ctx->ApiVersion != UCO_PLUGIN_API_VERSION) return 1;
    g_Log = ctx->Log;
    LOG("[EOSAuth] plugin init (host logger attached).");
    return 0;
}

extern "C" __declspec(dllexport) void UCO_PluginShutdown(void)
{
    InterlockedExchange(&g_bShutdown, 1);
    if (g_hWatcher) {
        WaitForSingleObject(g_hWatcher, 1000);
        CloseHandle(g_hWatcher);
        g_hWatcher = nullptr;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    LOG("[EOSAuth] diagnostic plugin shutdown.");
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        // Start watching for the EOS SDK IMMEDIATELY -- the game's
        // EOS_Platform_Create / first EOS_Connect_Login happen during
        // startup, before UCO_PluginInit would ever run.
        LoadEosConfig();
        LOG("[EOSAuth] DllMain -- config %s (Product=%s Client=%s DisplayName=%s); watching for EOS SDK.",
            g_Cfg.bValid ? "OK" : "INCOMPLETE (see [EOS] in union-crax.ini)",
            g_Cfg.ProductId[0] ? g_Cfg.ProductId : "(unset)",
            g_Cfg.ClientId[0]  ? g_Cfg.ClientId  : "(unset)",
            g_Cfg.DisplayName);
        g_hWatcher = CreateThread(nullptr, 0, WatcherProc, nullptr, 0, nullptr);
    }
    return TRUE;
}
