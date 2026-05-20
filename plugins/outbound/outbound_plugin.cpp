// ============================================================
// UCOnline2 plugin -- Outbound (AppId 2681030)
//
// Reference example of a per-game plugin built against the v1
// plugin ABI (see include/uco_plugin.h).
//
// Game uses EOSSDK-Win64-Shipping.dll for "Show Multiplayer
// Code" auth. Steam ticket flows through EOS_Connect_Login;
// Epic's backend validates against the AppId configured for
// the EOS product, which doesn't match our spoofed Steam
// (480) -> "Failed(2): Ticket for other app".
//
// What this plugin installs:
//   - ISteamUser::GetAuthSessionTicket  (vtable[13]) -> synthesize
//     a ticket with ogAppId embedded.
//   - ISteamUser::BeginAuthSession      (vtable[15]) -> always OK.
//   - GetAuthSessionTicketResponse_t    callback     -> force OK.
//   - ValidateAuthTicketResponse_t      callback     -> force OK.
//   - EOSSDK exports EOS_Connect_Login / EOS_Auth_Login        ->
//     short-circuit to Success with fake LocalUserId.
//
// MinHook is statically linked into this DLL; it maintains its
// own hook state independent of the host's MinHook instance.
// ============================================================
#include <Windows.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

#define STEAM_API_EXPORTS
#include "../../include/sdk/steam_api.h"
#include "../../include/sdk/steamclientpublic.h"
#include "../../include/sdk/isteamuser.h"

#include "../../include/MinHook.h"
#include "../../include/uco_plugin.h"

#include "il2cpp_runtime.h"

// ============================================================
// Plugin-local state -- populated in UCO_PluginInit
// ============================================================
static UCO_LogFn g_Log               = nullptr;
static uint32_t  g_ForcedAppId       = 480;
static uint32_t  g_OriginalAppId     = 0;
static ISteamUser*  g_pSteamUser     = nullptr;
static uint32_t  g_TicketSerial      = 0;
static HANDLE    g_hEosWatcherThread = nullptr;
static volatile LONG g_bShutdown     = 0;

#define LOG(...) do { if (g_Log) g_Log(__VA_ARGS__); } while (0)

// Visible to il2cpp_runtime.cpp via extern "C" prototype.
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
// Synthetic Steam auth ticket builder
// ============================================================
static inline void TWU16(unsigned char*& p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p += 2;
}
static inline void TWU32(unsigned char*& p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
    p += 4;
}
static inline void TWU64(unsigned char*& p, uint64_t v)
{
    TWU32(p, (uint32_t)(v & 0xFFFFFFFFu));
    TWU32(p, (uint32_t)(v >> 32));
}

static uint32_t BuildSyntheticTicket(void* pTicket, int cbMaxTicket,
                                     uint64_t steamID, uint32_t appId)
{
    const uint32_t kGCTokenLen    = 20;
    const uint32_t kSessionHdrLen = 24;
    const uint32_t kAppTicketBody = 42;  // ver+sid+appid+ip*2+flags+gen+exp+lic#+dlc#+rsv
    const uint32_t kAppTicketSig  = 128;
    const uint32_t kAppTicketLen  = kAppTicketBody + kAppTicketSig;
    const uint32_t kTotal = 4 + kGCTokenLen + 4 + kSessionHdrLen
                          + 4 + kAppTicketLen;

    if (!pTicket || cbMaxTicket < (int)kTotal)
        return 0;

    unsigned char* p = (unsigned char*)pTicket;
    memset(p, 0, kTotal);

    uint32_t nowUnix = (uint32_t)time(nullptr);
    uint32_t nowMs   = GetTickCount();

    // GCToken
    TWU32(p, kGCTokenLen);
    TWU64(p, steamID ^ 0xA5A5A5A5A5A5A5A5ull);
    TWU64(p, steamID);
    TWU32(p, nowMs);

    // SessionHeader
    TWU32(p, kSessionHdrLen);
    TWU32(p, 1);
    TWU32(p, 2);
    TWU32(p, 0);
    TWU32(p, 0);
    TWU32(p, nowMs);
    TWU32(p, ++g_TicketSerial);

    // AppOwnershipTicket
    TWU32(p, kAppTicketLen);
    TWU32(p, 4);                  // version
    TWU64(p, steamID);
    TWU32(p, appId);              // <- ogAppId
    TWU32(p, 0);                  // ext ip
    TWU32(p, 0);                  // int ip
    TWU32(p, 0x4);                // flags
    TWU32(p, nowUnix);            // generated
    TWU32(p, nowUnix + 24*3600);  // expires
    TWU16(p, 0);                  // licenseCount
    TWU16(p, 0);                  // dlcCount
    TWU16(p, 0);                  // reserved
    p += kAppTicketSig;           // zero signature

    return (uint32_t)(p - (unsigned char*)pTicket);
}

// ============================================================
// ISteamUser hooks
// ============================================================
typedef HAuthTicket (S_CALLTYPE *Fn_GetAuthSessionTicket)(
    void* pThis, void* pTicket, int cbMaxTicket,
    uint32* pcbTicket, const SteamNetworkingIdentity* pIdentity);
typedef EBeginAuthSessionResult (S_CALLTYPE *Fn_BeginAuthSession)(
    void* pThis, const void* pAuthTicket, int cbAuthTicket, CSteamID steamID);

static Fn_GetAuthSessionTicket g_pfnOrigGetAuthSessionTicket = nullptr;
static Fn_BeginAuthSession     g_pfnOrigBeginAuthSession     = nullptr;

static HAuthTicket S_CALLTYPE Hooked_GetAuthSessionTicket(
    void* pThis, void* pTicket, int cbMaxTicket,
    uint32* pcbTicket, const SteamNetworkingIdentity* pIdentity)
{
    HAuthTicket h = g_pfnOrigGetAuthSessionTicket(
        pThis, pTicket, cbMaxTicket, pcbTicket, pIdentity);

    if (g_OriginalAppId == 0 || g_OriginalAppId == g_ForcedAppId)
        return h;

    if (!pTicket || !pcbTicket || cbMaxTicket <= 0)
        return h;

    uint64_t steamID = 0;
    if (g_pSteamUser)
        steamID = g_pSteamUser->GetSteamID().ConvertToUint64();

    uint32_t written = BuildSyntheticTicket(pTicket, cbMaxTicket, steamID, g_OriginalAppId);
    if (written == 0)
        return h;

    *pcbTicket = written;
    LOG("[Outbound] GetAuthSessionTicket synthesized %u bytes AppId=%u handle=%u",
        written, g_OriginalAppId, h);
    return h;
}

static EBeginAuthSessionResult S_CALLTYPE Hooked_BeginAuthSession(
    void* pThis, const void* pAuthTicket, int cbAuthTicket, CSteamID steamID)
{
    if (g_OriginalAppId == 0 || g_OriginalAppId == g_ForcedAppId)
        return g_pfnOrigBeginAuthSession(pThis, pAuthTicket, cbAuthTicket, steamID);
    LOG("[Outbound] BeginAuthSession bypass: returning OK (sid=%llu)",
        (unsigned long long)steamID.ConvertToUint64());
    return k_EBeginAuthSessionResultOK;
}

// ============================================================
// Steam callback patchers
// k_iSteamUserCallbacks = 100
//   163 = GetAuthSessionTicketResponse_t
//   143 = ValidateAuthTicketResponse_t
// ============================================================
static void __cdecl PatchGetAuthSessionTicketResponse(uint8_t* buf, uint32_t cb)
{
    if (cb >= sizeof(GetAuthSessionTicketResponse_t))
    {
        auto* p = (GetAuthSessionTicketResponse_t*)buf;
        if (p->m_eResult != k_EResultOK)
        {
            LOG("[Outbound] Forcing GetAuthSessionTicketResponse %d->OK", (int)p->m_eResult);
            p->m_eResult = k_EResultOK;
        }
    }
}

static void __cdecl PatchValidateAuthTicketResponse(uint8_t* buf, uint32_t cb)
{
    if (cb >= sizeof(ValidateAuthTicketResponse_t))
    {
        auto* p = (ValidateAuthTicketResponse_t*)buf;
        if (p->m_eAuthSessionResponse != k_EAuthSessionResponseOK)
        {
            LOG("[Outbound] Forcing ValidateAuthTicketResponse %d->OK",
                (int)p->m_eAuthSessionResponse);
            p->m_eAuthSessionResponse = k_EAuthSessionResponseOK;
        }
    }
}

// ============================================================
// EOSSDK hooks -- declared inline since the SDK header is big
// ============================================================
typedef int32_t  EOS_EResult;
typedef void*    EOS_HConnect;
typedef void*    EOS_HAuth;
typedef void*    EOS_ProductUserId;
typedef void*    EOS_EpicAccountId;
typedef void*    EOS_ContinuanceToken;
#define EOS_EResult_Success 0

typedef struct EOS_Connect_LoginCallbackInfo {
    EOS_EResult           ResultCode;
    void*                 ClientData;
    EOS_ProductUserId     LocalUserId;
    EOS_ContinuanceToken  ContinuanceToken;
} EOS_Connect_LoginCallbackInfo;

typedef void (__cdecl *EOS_Connect_OnLoginCallback)(
    const EOS_Connect_LoginCallbackInfo*);

typedef struct EOS_Connect_LoginOptions {
    int32_t      ApiVersion;
    const void*  Credentials;
    const void*  UserLoginInfo;
} EOS_Connect_LoginOptions;

typedef void (__cdecl *Fn_EOS_Connect_Login)(
    EOS_HConnect, const EOS_Connect_LoginOptions*, void*,
    EOS_Connect_OnLoginCallback);

typedef struct EOS_Auth_LoginCallbackInfo {
    EOS_EResult           ResultCode;
    void*                 ClientData;
    EOS_EpicAccountId     LocalUserId;
    EOS_ContinuanceToken  ContinuanceToken;
    int32_t               PreviousLoginStatus;
    void*                 PinGrantInfo;
    void*                 AccountFeatureRestrictedInfo;
    int32_t               SelectedAccountFeatureRestrictedInfo;
} EOS_Auth_LoginCallbackInfo;

typedef void (__cdecl *EOS_Auth_OnLoginCallback)(
    const EOS_Auth_LoginCallbackInfo*);

typedef struct EOS_Auth_LoginOptions {
    int32_t      ApiVersion;
    const void*  Credentials;
    int32_t      ScopeFlags;
    int32_t      LoginFlags;
} EOS_Auth_LoginOptions;

typedef void (__cdecl *Fn_EOS_Auth_Login)(
    EOS_HAuth, const EOS_Auth_LoginOptions*, void*,
    EOS_Auth_OnLoginCallback);

static Fn_EOS_Connect_Login g_pfnOrigEosConnectLogin = nullptr;
static Fn_EOS_Auth_Login    g_pfnOrigEosAuthLogin    = nullptr;
static uint64_t             g_FakePuidCounter        = 0x1000;
static uint64_t             g_FakeEaidCounter        = 0x2000;

// ---- EOS_Ecom_QueryOwnershipToken --------------------------
// This is the call that Outbound uses to verify Steam-ticket
// ownership against the EOS app's configured AppId. Confirmed
// via il2cpp dump:
//   EpicManager.GetOwnershipVerificationToken()
//     -> QueryOwnershipTokenCallbackInfo (Result @ 0x0)
// Epic's backend rejects because the Steam ticket is for
// AppId 480 not the real one. Short-circuit at the native EOS
// layer: fire the completion delegate immediately with
// EOS_Success and a non-empty OwnershipToken.

typedef struct EOS_Ecom_QueryOwnershipTokenCallbackInfo
{
    EOS_EResult       ResultCode;       // 0x00
    void*             ClientData;       // 0x08
    EOS_EpicAccountId LocalUserId;      // 0x10
    const char*       OwnershipToken;   // 0x18 (Utf8String / char*)
} EOS_Ecom_QueryOwnershipTokenCallbackInfo;

typedef void (__cdecl *EOS_Ecom_OnQueryOwnershipTokenCallback)(
    const EOS_Ecom_QueryOwnershipTokenCallbackInfo* Data);

typedef void (__cdecl *Fn_EOS_Ecom_QueryOwnershipToken)(
    void* /*EOS_HEcom*/ Handle,
    const void*         Options,
    void*               ClientData,
    EOS_Ecom_OnQueryOwnershipTokenCallback CompletionDelegate);

static Fn_EOS_Ecom_QueryOwnershipToken g_pfnOrigEosEcomQueryOwnership = nullptr;

// A throwaway non-empty ownership token. The C# side just needs
// a non-null Utf8String to treat the result as "have ownership"
// rather than empty.
static const char* kFakeOwnershipToken = "ucoco-synthetic-ownership-token";

// ---- EOS_Ecom_QueryOwnershipBySandboxIds --------------------
// First call in EpicManager.GetOwnershipVerificationToken's
// two-step chain. b__0 in the dump fires on its completion;
// if this fails the chain stops before QueryOwnershipToken
// ever runs (which is why our QueryOwnershipToken hook never
// got an intercept).
//
// Struct layout from il2cpp dump (TypeDefIndex 7146 / internal):
//   ResultCode               @ 0x00 (4 bytes)
//   ClientData               @ 0x08
//   LocalUserId              @ 0x10
//   SandboxIdItemOwnerships  @ 0x18 (pointer, can be null)
//   SandboxIdItemOwnershipsCount @ 0x20 (uint32)

typedef struct EOS_Ecom_QueryOwnershipBySandboxIdsCallbackInfo
{
    EOS_EResult       ResultCode;                        // 0x00
    void*             ClientData;                        // 0x08
    EOS_EpicAccountId LocalUserId;                       // 0x10
    const void*       SandboxIdItemOwnerships;           // 0x18
    uint32_t          SandboxIdItemOwnershipsCount;      // 0x20
    uint32_t          _pad;                              // 0x24 (align to 8)
} EOS_Ecom_QueryOwnershipBySandboxIdsCallbackInfo;

typedef void (__cdecl *EOS_Ecom_OnQueryOwnershipBySandboxIdsCallback)(
    const EOS_Ecom_QueryOwnershipBySandboxIdsCallbackInfo* Data);

typedef void (__cdecl *Fn_EOS_Ecom_QueryOwnershipBySandboxIds)(
    void* Handle, const void* Options, void* ClientData,
    EOS_Ecom_OnQueryOwnershipBySandboxIdsCallback CompletionDelegate);

static Fn_EOS_Ecom_QueryOwnershipBySandboxIds g_pfnOrigEosEcomQueryOwnershipBySandboxIds = nullptr;

static void __cdecl Hooked_EOS_Ecom_QueryOwnershipBySandboxIds(
    void* Handle, const void* Options, void* ClientData,
    EOS_Ecom_OnQueryOwnershipBySandboxIdsCallback CompletionDelegate)
{
    LOG("[Outbound] EOS_Ecom_QueryOwnershipBySandboxIds intercept");
    if (!CompletionDelegate) return;

    EOS_Ecom_QueryOwnershipBySandboxIdsCallbackInfo info = {};
    info.ResultCode                    = EOS_EResult_Success;
    info.ClientData                    = ClientData;
    info.LocalUserId                   = (EOS_EpicAccountId)(uintptr_t)(++g_FakeEaidCounter);
    info.SandboxIdItemOwnerships       = nullptr;
    info.SandboxIdItemOwnershipsCount  = 0;
    CompletionDelegate(&info);
    LOG("[Outbound] EOS_Ecom_QueryOwnershipBySandboxIds: fired Success (empty array)");
}

static void __cdecl Hooked_EOS_Ecom_QueryOwnershipToken(
    void* Handle, const void* Options, void* ClientData,
    EOS_Ecom_OnQueryOwnershipTokenCallback CompletionDelegate)
{
    LOG("[Outbound] EOS_Ecom_QueryOwnershipToken intercept");
    if (!CompletionDelegate) return;

    EOS_Ecom_QueryOwnershipTokenCallbackInfo info = {};
    info.ResultCode     = EOS_EResult_Success;
    info.ClientData     = ClientData;
    info.LocalUserId    = (EOS_EpicAccountId)(uintptr_t)(++g_FakeEaidCounter);
    info.OwnershipToken = kFakeOwnershipToken;
    CompletionDelegate(&info);
    LOG("[Outbound] EOS_Ecom_QueryOwnershipToken: fired Success with fake token");
}

static void __cdecl Hooked_EOS_Connect_Login(
    EOS_HConnect Handle, const EOS_Connect_LoginOptions* Options,
    void* ClientData, EOS_Connect_OnLoginCallback CompletionDelegate)
{
    LOG("[Outbound] EOS_Connect_Login intercept");
    if (!CompletionDelegate) return;
    EOS_Connect_LoginCallbackInfo info = {};
    info.ResultCode  = EOS_EResult_Success;
    info.ClientData  = ClientData;
    info.LocalUserId = (EOS_ProductUserId)(uintptr_t)(++g_FakePuidCounter);
    CompletionDelegate(&info);
}

static void __cdecl Hooked_EOS_Auth_Login(
    EOS_HAuth Handle, const EOS_Auth_LoginOptions* Options,
    void* ClientData, EOS_Auth_OnLoginCallback CompletionDelegate)
{
    LOG("[Outbound] EOS_Auth_Login intercept");
    if (!CompletionDelegate) return;
    EOS_Auth_LoginCallbackInfo info = {};
    info.ResultCode  = EOS_EResult_Success;
    info.ClientData  = ClientData;
    info.LocalUserId = (EOS_EpicAccountId)(uintptr_t)(++g_FakeEaidCounter);
    CompletionDelegate(&info);
}

static bool InstallEosHook(HMODULE hMod, const char* name, void* hook, void** orig)
{
    FARPROC target = GetProcAddress(hMod, name);
    if (!target) { LOG("[Outbound] EOSSDK has no %s", name); return false; }
    if (MH_CreateHook((LPVOID)target, hook, orig) != MH_OK) return false;
    if (MH_EnableHook((LPVOID)target) != MH_OK) return false;
    LOG("[Outbound] %s hook installed at %p", name, target);
    return true;
}

static bool TryInstallEosHooks()
{
    HMODULE hEos = GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll");
    if (!hEos) return false;
    static bool installed = false;
    if (installed) return true;
    bool any = false;
    any |= InstallEosHook(hEos, "EOS_Connect_Login",
                          (void*)&Hooked_EOS_Connect_Login,
                          (void**)&g_pfnOrigEosConnectLogin);
    any |= InstallEosHook(hEos, "EOS_Auth_Login",
                          (void*)&Hooked_EOS_Auth_Login,
                          (void**)&g_pfnOrigEosAuthLogin);
    any |= InstallEosHook(hEos, "EOS_Ecom_QueryOwnershipToken",
                          (void*)&Hooked_EOS_Ecom_QueryOwnershipToken,
                          (void**)&g_pfnOrigEosEcomQueryOwnership);
    any |= InstallEosHook(hEos, "EOS_Ecom_QueryOwnershipBySandboxIds",
                          (void*)&Hooked_EOS_Ecom_QueryOwnershipBySandboxIds,
                          (void**)&g_pfnOrigEosEcomQueryOwnershipBySandboxIds);
    installed = any;
    return any;
}

// ---- Fusion.CloudServices.OnCustomAuthenticationFailed ----
//
// Photon Fusion's CloudServices is the IConnectionCallbacks
// implementation for the Photon master server. When Photon's
// custom auth callback URL (Outbound's backend) rejects our
// Steam ticket -- because the ticket is signed for AppId 480
// instead of the real AppId 2681030 -- the master returns
// CustomAuthenticationResult.Failed and Photon invokes
//   CloudServices.OnCustomAuthenticationFailed(string debugMessage)
// (Image 13: Fusion.Runtime.dll, RVA 0xF20B40 in the dump).
// That method displays the "Failed(2): 'Ticket for other app'"
// error to the user.
//
// Suppressing the call hides the UI error. The connection
// state may still be wedged on the Photon side; if that
// surfaces a new error we'll add a follow-up.
typedef void (__fastcall *Fn_OnCustomAuthenticationFailed)(void* pThis, void* debugMessage);
static Fn_OnCustomAuthenticationFailed g_pfnOrigOnCustomAuthenticationFailed = nullptr;

static void __fastcall Hooked_OnCustomAuthenticationFailed(void* pThis, void* debugMessage)
{
    LOG("[Outbound] Fusion.CloudServices.OnCustomAuthenticationFailed suppressed");
    // Intentionally do not call original: do not display the error UI.
}

static void TryInstallIl2CppHooks()
{
    if (!IL2CPP_IsReady()) return;
    static bool attempted = false;
    if (attempted) return;
    attempted = true;

    void* fn = IL2CPP_FindMethodPtr(
        "Fusion.Runtime", "Fusion", "CloudServices",
        "OnCustomAuthenticationFailed", 1);
    if (!fn)
    {
        LOG("[Outbound] IL2CPP: could not find Fusion.CloudServices.OnCustomAuthenticationFailed");
        return;
    }

    MH_STATUS s = MH_CreateHook(fn, &Hooked_OnCustomAuthenticationFailed,
                                (void**)&g_pfnOrigOnCustomAuthenticationFailed);
    if (s != MH_OK)
    {
        LOG("[Outbound] MH_CreateHook failed for OnCustomAuthenticationFailed: %d", s);
        return;
    }
    if (MH_EnableHook(fn) != MH_OK)
    {
        LOG("[Outbound] MH_EnableHook failed for OnCustomAuthenticationFailed");
        return;
    }
    LOG("[Outbound] Fusion.CloudServices.OnCustomAuthenticationFailed hook installed at %p", fn);
}

static DWORD WINAPI WatcherProc(LPVOID)
{
    bool eosDone = false;
    bool il2Done = false;
    for (int i = 0; i < 600 && InterlockedCompareExchange(&g_bShutdown, 0, 0) == 0; ++i)
    {
        if (!eosDone) eosDone = TryInstallEosHooks();
        if (!il2Done && IL2CPP_TryInit())
        {
            il2Done = true;
            TryInstallIl2CppHooks();
        }
        if (eosDone && il2Done) return 0;
        Sleep(200);
    }
    if (!InterlockedCompareExchange(&g_bShutdown, 0, 0))
    {
        if (!eosDone) LOG("[Outbound] EOSSDK never loaded -- giving up on EOS hooks");
        if (!il2Done) LOG("[Outbound] GameAssembly.dll never resolved -- giving up on IL2CPP hooks");
    }
    return 0;
}

// ============================================================
// ISteamUser vtable hook installer
//   [13] = GetAuthSessionTicket
//   [15] = BeginAuthSession
// ============================================================
static void InstallSteamUserHooks()
{
    if (!g_pSteamUser) return;
    void** vt = *reinterpret_cast<void***>(g_pSteamUser);
    void* pGet   = vt[13];
    void* pBegin = vt[15];

    if (MH_CreateHook(pGet, &Hooked_GetAuthSessionTicket,
                      (void**)&g_pfnOrigGetAuthSessionTicket) == MH_OK
        && MH_EnableHook(pGet) == MH_OK)
        LOG("[Outbound] GetAuthSessionTicket hook installed");
    else
        LOG("[Outbound] GetAuthSessionTicket hook FAILED");

    if (MH_CreateHook(pBegin, &Hooked_BeginAuthSession,
                      (void**)&g_pfnOrigBeginAuthSession) == MH_OK
        && MH_EnableHook(pBegin) == MH_OK)
        LOG("[Outbound] BeginAuthSession hook installed");
    else
        LOG("[Outbound] BeginAuthSession hook FAILED");
}

// ============================================================
// Plugin ABI entry points
// ============================================================
extern "C" __declspec(dllexport) int __cdecl UCO_PluginInit(const UCO_PluginContext* ctx)
{
    if (!ctx) return 1;
    if (ctx->ApiVersion != UCO_PLUGIN_API_VERSION) return 2;

    g_Log           = ctx->Log;
    g_ForcedAppId   = ctx->ForcedAppId;
    g_OriginalAppId = ctx->OriginalAppId;
    g_pSteamUser    = ctx->pSteamUser;

    LOG("[Outbound] plugin v1 init: AppId=%u ogAppId=%u",
        g_ForcedAppId, g_OriginalAppId);

    if (g_OriginalAppId == 0 || g_OriginalAppId == g_ForcedAppId)
    {
        LOG("[Outbound] no ogAppId set -- plugin idle");
        return 0;
    }

    if (MH_Initialize() != MH_OK)
    {
        LOG("[Outbound] MH_Initialize failed");
        return 3;
    }

    // Steam-side hooks (need ISteamUser, which is ready now).
    InstallSteamUserHooks();

    // Callback patchers (run as soon as the host receives them).
    if (ctx->RegisterCallbackPatcher)
    {
        ctx->RegisterCallbackPatcher(163, &PatchGetAuthSessionTicketResponse);
        ctx->RegisterCallbackPatcher(143, &PatchValidateAuthTicketResponse);
    }

    // EOSSDK and GameAssembly.dll both load later during Unity
    // plugin init -- spin a watcher (max ~120s) that installs each
    // hook set the moment its target module appears.
    g_hEosWatcherThread = CreateThread(nullptr, 0, WatcherProc,
                                        nullptr, 0, nullptr);
    return 0;
}

extern "C" __declspec(dllexport) void __cdecl UCO_PluginShutdown(void)
{
    InterlockedExchange(&g_bShutdown, 1);
    if (g_hEosWatcherThread)
    {
        WaitForSingleObject(g_hEosWatcherThread, 1000);
        CloseHandle(g_hEosWatcherThread);
        g_hEosWatcherThread = nullptr;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    LOG("[Outbound] plugin shutdown");
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
