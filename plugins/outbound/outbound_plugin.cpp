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
typedef void (__fastcall *Fn_OnCustomAuthenticationResponse)(void* pThis, void* runner, void* data);

static Fn_OnCustomAuthenticationFailed   g_pfnOrigOnCustomAuthenticationFailed                = nullptr;
static Fn_OnCustomAuthenticationResponse g_pfnOrigNetworkDelegatesOnCustomAuthResponse        = nullptr;
static Fn_OnCustomAuthenticationResponse g_pfnOrigNetworkEventsOnCustomAuthResponse           = nullptr;

// NetworkManager.OnShutdown(NetworkRunner, ShutdownReason)
typedef void (__fastcall *Fn_NetworkManagerOnShutdown)(void* pThis, void* runner, int shutdownReason);
static Fn_NetworkManagerOnShutdown g_pfnOrigNetworkManagerOnShutdown = nullptr;

// PhotonAppSettings.get_Global() -- static getter, no args, returns PhotonAppSettings*
typedef void* (__fastcall *Fn_PhotonAppSettings_get_Global)();
static Fn_PhotonAppSettings_get_Global g_pfnOrigPhotonAppSettingsGetGlobal = nullptr;

// AuthenticationValues.set_AuthType(CustomAuthenticationType value)
// CustomAuthenticationType is byte-backed. None = 255.
typedef void (__fastcall *Fn_AuthValues_set_AuthType)(void* pThis, unsigned int value);
static Fn_AuthValues_set_AuthType g_pfnOrigAuthValuesSetAuthType = nullptr;

// AuthenticationValues.set_Token(object value)
// Originally the Steam ticket bytes. Force null so Photon
// can't interpret AuthType=255-with-token as "client wants
// custom auth".
typedef void (__fastcall *Fn_AuthValues_set_Token)(void* pThis, void* value);
static Fn_AuthValues_set_Token g_pfnOrigAuthValuesSetToken = nullptr;

// LoadBalancingClient.set_AppId(string) -- the connection-time
// AppId, copied from FusionAppSettings just before Connect.
// The PhotonAppSettings patch doesn't reach this because
// Fusion's RelayClient maintains its own FusionAppSettings copy
// in Config @ offset 0x1D8.
typedef void (__fastcall *Fn_LBC_set_AppId)(void* pThis, void* value);
static Fn_LBC_set_AppId g_pfnOrigLBCsetAppId = nullptr;

// FusionRelayClient.ConnectUsingSettings(AppSettings appSettings)
// This is the actual entry point Fusion uses to start a Photon
// connection. The AppSettings is passed by reference; if we
// rewrite its AppIdFusion field on the way in, the connection
// goes to our app. Most reliable hook target.
typedef bool (__fastcall *Fn_FRC_ConnectUsingSettings)(void* pThis, void* appSettings);
static Fn_FRC_ConnectUsingSettings g_pfnOrigFRCconnectUsingSettings = nullptr;

// Field offset of AppIdFusion on AppSettings base class (per dump
// line 1034518). FusionAppSettings inherits from AppSettings, so
// the same offset applies on any FusionAppSettings instance.
static const size_t kOffsetAppSettings_AppIdFusion = 0x18;

// NetworkRunner.StartGame(StartGameArgs args) -- THE entry point
// for Photon Fusion connections. StartGameArgs is a struct
// passed by hidden pointer on x64 ABI. Layout per dump:
//   StartGameArgs.AuthValues               @ 0x100 (managed ptr)
//   StartGameArgs.CustomPhotonAppSettings  @ 0x108 (managed ptr to FusionAppSettings)
// If the game sets CustomPhotonAppSettings, Photon uses THAT
// instead of PhotonAppSettings.Global -- which is why the
// PhotonAppSettings.Global patch never reaches the actual
// connection. We rewrite the CustomPhotonAppSettings'
// AppIdFusion field here.
typedef void* (__fastcall *Fn_NetworkRunner_StartGame)(void* pThis, void* args);
static Fn_NetworkRunner_StartGame g_pfnOrigNetworkRunnerStartGame = nullptr;

static const size_t kOffsetStartGameArgs_CustomPhotonAppSettings = 0x108;

// LoadBalancingClient.OpAuthenticate(appId, appVersion, authValues,
//                                     regionCode, getLobbyStatistics)
// RVA 0xF04DC0. THIS is the actual wire-send call -- whatever
// authValues.authType field holds at the moment this fires is
// what reaches Photon's master server. Patching here bypasses
// IL2CPP inlining concerns.
//
// AuthenticationValues field layout (per dump):
//   authType field @ offset 0x10 (byte, but stored in an aligned slot)
typedef bool (__fastcall *Fn_OpAuthenticate)(
    void* pThis, void* appId, void* appVersion, void* authValues,
    void* regionCode, bool getLobbyStatistics);
static Fn_OpAuthenticate g_pfnOrigOpAuthenticate = nullptr;

typedef bool (__fastcall *Fn_OpAuthenticateOnce)(
    void* pThis, void* appId, void* appVersion, void* authValues,
    void* regionCode, int encryptionMode, int expectedProtocol);
static Fn_OpAuthenticateOnce g_pfnOrigOpAuthenticateOnce = nullptr;

static const size_t kOffsetAuthValues_authType = 0x10;

// Our replacement GUID for Fusion AppId, read from the ini.
// Stored as the managed System.String pointer (created lazily on
// first hook fire after IL2CPP runtime is ready).
static char  g_OurFusionAppIdUtf8[64] = {};
static void* g_OurFusionAppIdString    = nullptr;  // Il2CppString*
static bool  g_AppIdPatchEnabled       = false;

// Field offsets confirmed from il2cpp dump (TypeDefIndex 18602):
//   PhotonAppSettings.AppSettings @ 0x20
//   FusionAppSettings.AppIdRealtime @ 0x10
//   FusionAppSettings.AppIdFusion   @ 0x18
//   FusionAppSettings.AppIdChat     @ 0x20
//   FusionAppSettings.AppIdVoice    @ 0x28
static const size_t kOffsetPhotonAppSettings_AppSettings = 0x20;
static const size_t kOffsetFusionAppSettings_AppIdFusion = 0x18;

static void __fastcall Hooked_OnCustomAuthenticationFailed(void* pThis, void* debugMessage)
{
    LOG("[Outbound] Fusion.CloudServices.OnCustomAuthenticationFailed suppressed");
}

// Fan-out wrappers in Fusion.Runtime that distribute the auth
// response (success OR failure dictionary) to game-attached
// delegates / UnityEvents. Suppressing them prevents the game
// from ever seeing the failure dictionary and therefore from
// running its error-display code.
static void __fastcall Hooked_NetworkDelegatesOnCustomAuthResponse(void* pThis, void* runner, void* data)
{
    LOG("[Outbound] Fusion.NetworkDelegates.OnCustomAuthenticationResponse suppressed");
}

static void __fastcall Hooked_NetworkEventsOnCustomAuthResponse(void* pThis, void* runner, void* data)
{
    LOG("[Outbound] Fusion.NetworkEvents.OnCustomAuthenticationResponse suppressed");
}

// NetworkManager (Assembly-CSharp, global namespace) is the
// game's own INetworkRunnerCallbacks implementation. When
// Photon Fusion's master rejects custom auth, the runner
// shuts down with ShutdownReason.IncompatibleConfiguration
// (= 2) -- which matches the "Failed(2)" prefix in the UI.
// This is the method that actually formats and displays the
// error. Suppress it.
static void __fastcall Hooked_NetworkManagerOnShutdown(void* pThis, void* runner, int shutdownReason)
{
    LOG("[Outbound] NetworkManager.OnShutdown(reason=%d) suppressed", shutdownReason);
    // Do not call original: the original would display
    // "Failed(reason): '<debug message>'" to the user.
}

// On every call: get the real PhotonAppSettings, then patch
// its embedded FusionAppSettings.AppIdFusion field to our GUID.
// We allocate the Il2CppString lazily on the first call (we
// can't safely allocate before the il2cpp domain is ready, and
// even though IL2CPP_TryInit has succeeded by the time hooks
// fire, doing the allocation inside the hook keeps us on the
// thread that owns the domain attach).
// The game (via PlatformManager.GetAuthenticationSettings)
// builds AuthenticationValues with AuthType=Steam and the
// Steam ticket as Token. Photon routes by AuthType: with
// Steam, Photon calls Steam Web API using your dashboard's
// configured apiKeySecret -- which is bogus in our setup,
// so Steam returns 403 Forbidden and Photon forwards it.
//
// Force AuthType to None (255). Photon then treats the
// client as anonymous, skips provider validation entirely,
// and accepts the connection on apps that allow anonymous.
// Force-trigger flag for the PhotonAppSettings cached-singleton workaround.
static volatile LONG g_TriedForceGlobal = 0;

// Forward declaration -- the set_AuthType hook invokes this to apply
// the AppId patch on the cached singleton.
static void* __fastcall Hooked_PhotonAppSettings_get_Global();

// CustomAuthenticationType values (byte enum):
//   Custom = 0, Steam = 1, Facebook = 2, ..., None = 255
// Read from union-crax.ini so users can pick what their Photon
// app expects without rebuilding. Default 0 (Custom) routes
// the auth through a Photon Custom Auth URL.
static unsigned int g_ForcedAuthType = 0;

static void __fastcall Hooked_AuthValues_set_AuthType(void* pThis, unsigned int value)
{
    if (g_AppIdPatchEnabled
        && InterlockedExchange(&g_TriedForceGlobal, 1) == 0
        && g_pfnOrigPhotonAppSettingsGetGlobal != nullptr)
    {
        LOG("[Outbound] forcing PhotonAppSettings.get_Global to apply AppId patch");
        Hooked_PhotonAppSettings_get_Global();
    }

    // We still hook the setter for completeness, but IL2CPP may
    // inline the game's own assignment past this hook. The real
    // override happens in Hooked_OpAuthenticate below.
    if (value != g_ForcedAuthType)
        LOG("[Outbound] AuthenticationValues.set_AuthType(%u) -> forced %u", value, g_ForcedAuthType);
    g_pfnOrigAuthValuesSetAuthType(pThis, g_ForcedAuthType);
}

static void __fastcall Hooked_AuthValues_set_Token(void* pThis, void* value)
{
    if (value)
        LOG("[Outbound] AuthenticationValues.set_Token(%p) -> forced null", value);
    g_pfnOrigAuthValuesSetToken(pThis, nullptr);
}

static void __fastcall Hooked_LBC_set_AppId(void* pThis, void* value)
{
    // If we already have our managed-string version of the GUID,
    // force the connection AppId to use it. Otherwise pass the
    // original through -- this can happen briefly before the
    // get_Global hook has fired to lazy-build the string.
    if (g_OurFusionAppIdString)
    {
        LOG("[Outbound] LoadBalancingClient.set_AppId(%p) -> overridden to our GUID (%p)",
            value, g_OurFusionAppIdString);
        g_pfnOrigLBCsetAppId(pThis, g_OurFusionAppIdString);
    }
    else
    {
        LOG("[Outbound] LoadBalancingClient.set_AppId(%p) -> passthrough (our string not ready yet)", value);
        g_pfnOrigLBCsetAppId(pThis, value);
    }
}

// Override the wire-time auth type. We write the byte at
// offset 0x10 on the AuthValues object directly, so IL2CPP-
// inlined assignments earlier can't undo us.
static void PatchAuthValuesAuthType(void* authValues, const char* sender)
{
    if (!authValues) return;
    unsigned char* pAuthTypeByte = (unsigned char*)authValues + kOffsetAuthValues_authType;
    unsigned char prev = *pAuthTypeByte;
    *pAuthTypeByte = (unsigned char)(g_ForcedAuthType & 0xFF);
    LOG("[Outbound] %s: authValues.authType %u -> %u", sender, prev, g_ForcedAuthType);
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

// THE entry point. NetworkRunner.StartGame receives the
// game's StartGameArgs which can carry CustomPhotonAppSettings
// -- a per-call AppSettings override that wins over the
// global PhotonAppSettings singleton. We rewrite its
// AppIdFusion field in place so the connection actually
// targets our Photon app.
static void* __fastcall Hooked_NetworkRunner_StartGame(void* pThis, void* args)
{
    if (args && g_OurFusionAppIdString)
    {
        void** pCustom = (void**)((char*)args + kOffsetStartGameArgs_CustomPhotonAppSettings);
        void* custom = *pCustom;
        if (custom)
        {
            void** pAppIdFusion = (void**)((char*)custom + kOffsetAppSettings_AppIdFusion);
            void* oldStr = *pAppIdFusion;
            *pAppIdFusion = g_OurFusionAppIdString;
            LOG("[Outbound] StartGame: rewrote CustomPhotonAppSettings.AppIdFusion (was %p) -> %p",
                oldStr, g_OurFusionAppIdString);
        }
        else
        {
            LOG("[Outbound] StartGame: CustomPhotonAppSettings is null (game falls back to global -- already patched)");
        }
    }
    return g_pfnOrigNetworkRunnerStartGame(pThis, args);
}

// The big one: intercept the actual Fusion connection call.
// Mutates the passed AppSettings object's AppIdFusion field
// in-place before forwarding to the original implementation.
static bool __fastcall Hooked_FRC_ConnectUsingSettings(void* pThis, void* appSettings)
{
    if (appSettings && g_OurFusionAppIdString)
    {
        void** pAppIdFusion = (void**)((char*)appSettings + kOffsetAppSettings_AppIdFusion);
        void* oldStr = *pAppIdFusion;
        *pAppIdFusion = g_OurFusionAppIdString;
        LOG("[Outbound] ConnectUsingSettings: rewrote AppIdFusion (was %p) -> %p",
            oldStr, g_OurFusionAppIdString);
    }
    else
    {
        LOG("[Outbound] ConnectUsingSettings called (settings=%p, ourStr=%p) -- no patch applied",
            appSettings, g_OurFusionAppIdString);
    }
    return g_pfnOrigFRCconnectUsingSettings(pThis, appSettings);
}

static void* __fastcall Hooked_PhotonAppSettings_get_Global()
{
    void* settings = g_pfnOrigPhotonAppSettingsGetGlobal();
    if (!settings || !g_AppIdPatchEnabled) return settings;

    if (!g_OurFusionAppIdString)
    {
        g_OurFusionAppIdString = IL2CPP_StringNew(g_OurFusionAppIdUtf8);
        if (!g_OurFusionAppIdString)
        {
            LOG("[Outbound] AppId patch: IL2CPP_StringNew failed");
            return settings;
        }
        LOG("[Outbound] AppId patch: built managed string for '%s' at %p",
            g_OurFusionAppIdUtf8, g_OurFusionAppIdString);
    }

    // settings -> PhotonAppSettings instance
    void** pAppSettingsField = (void**)((char*)settings + kOffsetPhotonAppSettings_AppSettings);
    void* appSettings = *pAppSettingsField;
    if (!appSettings) return settings;

    void** pAppIdFusion = (void**)((char*)appSettings + kOffsetFusionAppSettings_AppIdFusion);
    void* oldStr = *pAppIdFusion;
    if (oldStr != g_OurFusionAppIdString)
    {
        *pAppIdFusion = g_OurFusionAppIdString;
        LOG("[Outbound] AppId patch: FusionAppSettings.AppIdFusion replaced (was %p)", oldStr);
    }
    return settings;
}

// Resolve %EXEDIR%\union-crax.ini once, cache result.
static const char* GetIniPath()
{
    static char path[MAX_PATH] = {};
    static bool computed = false;
    if (computed) return path[0] ? path : nullptr;
    computed = true;

    char exeDir[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    if (len == 0) return nullptr;
    // Strip filename
    for (int i = (int)len - 1; i >= 0; --i) {
        if (exeDir[i] == '\\' || exeDir[i] == '/') { exeDir[i] = 0; break; }
    }
    int n = _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\union-crax.ini", exeDir);
    if (n <= 0) { path[0] = 0; return nullptr; }
    return path;
}

static bool InstallIl2CppHook(const char* image, const char* ns, const char* klass,
                              const char* method, int argc, void* hook, void** orig,
                              const char* logName)
{
    void* fn = IL2CPP_FindMethodPtr(image, ns, klass, method, argc);
    if (!fn)
    {
        LOG("[Outbound] IL2CPP: could not find %s", logName);
        return false;
    }
    if (MH_CreateHook(fn, hook, orig) != MH_OK ||
        MH_EnableHook(fn) != MH_OK)
    {
        LOG("[Outbound] hook FAILED for %s", logName);
        return false;
    }
    LOG("[Outbound] %s hook installed at %p", logName, fn);
    return true;
}

static void TryInstallIl2CppHooks()
{
    if (!IL2CPP_IsReady()) return;
    static bool attempted = false;
    if (attempted) return;
    attempted = true;

    // Photon Realtime internal failure callback (the one we got firing already).
    InstallIl2CppHook(
        "Fusion.Runtime", "Fusion", "CloudServices",
        "OnCustomAuthenticationFailed", 1,
        (void*)&Hooked_OnCustomAuthenticationFailed,
        (void**)&g_pfnOrigOnCustomAuthenticationFailed,
        "Fusion.CloudServices.OnCustomAuthenticationFailed");

    // Fusion fan-outs to game-attached delegates / UnityEvents.
    // These are explicit interface implementations: in IL2CPP
    // metadata they appear with the full prefixed name, not the
    // bare method name. Try both forms.
    if (!InstallIl2CppHook(
            "Fusion.Runtime", "Fusion", "NetworkDelegates",
            "Fusion.INetworkRunnerCallbacks.OnCustomAuthenticationResponse", 2,
            (void*)&Hooked_NetworkDelegatesOnCustomAuthResponse,
            (void**)&g_pfnOrigNetworkDelegatesOnCustomAuthResponse,
            "Fusion.NetworkDelegates.OnCustomAuthenticationResponse"))
    {
        // Fall back to short name in case the metadata stores it that way.
        InstallIl2CppHook(
            "Fusion.Runtime", "Fusion", "NetworkDelegates",
            "OnCustomAuthenticationResponse", 2,
            (void*)&Hooked_NetworkDelegatesOnCustomAuthResponse,
            (void**)&g_pfnOrigNetworkDelegatesOnCustomAuthResponse,
            "Fusion.NetworkDelegates.OnCustomAuthenticationResponse (short)");
    }

    if (!InstallIl2CppHook(
            "Fusion.Runtime", "Fusion", "NetworkEvents",
            "Fusion.INetworkRunnerCallbacks.OnCustomAuthenticationResponse", 2,
            (void*)&Hooked_NetworkEventsOnCustomAuthResponse,
            (void**)&g_pfnOrigNetworkEventsOnCustomAuthResponse,
            "Fusion.NetworkEvents.OnCustomAuthenticationResponse"))
    {
        InstallIl2CppHook(
            "Fusion.Runtime", "Fusion", "NetworkEvents",
            "OnCustomAuthenticationResponse", 2,
            (void*)&Hooked_NetworkEventsOnCustomAuthResponse,
            (void**)&g_pfnOrigNetworkEventsOnCustomAuthResponse,
            "Fusion.NetworkEvents.OnCustomAuthenticationResponse (short)");
    }

    // The big one: NetworkManager.OnShutdown in Assembly-CSharp.
    // Photon Fusion's auth rejection converts to a runner
    // shutdown with ShutdownReason.IncompatibleConfiguration (=2),
    // which is the "Failed(2)" prefix in the UI. This is where
    // the game formats and displays the error.
    InstallIl2CppHook(
        "Assembly-CSharp", "", "NetworkManager",
        "OnShutdown", 2,
        (void*)&Hooked_NetworkManagerOnShutdown,
        (void**)&g_pfnOrigNetworkManagerOnShutdown,
        "NetworkManager.OnShutdown");

    // PhotonAppId patch path (the actual working approach):
    // hook PhotonAppSettings.get_Global so every consumer
    // sees the patched AppIdFusion -- redirecting the game
    // to a Photon Fusion app the user controls, where no
    // backend custom-auth check rejects them.
    if (g_AppIdPatchEnabled)
    {
        InstallIl2CppHook(
            "Fusion.Realtime", "Fusion.Photon.Realtime", "PhotonAppSettings",
            "get_Global", 0,
            (void*)&Hooked_PhotonAppSettings_get_Global,
            (void**)&g_pfnOrigPhotonAppSettingsGetGlobal,
            "PhotonAppSettings.get_Global");

        // When redirecting to a user-controlled Photon app, the
        // user almost certainly doesn't have Outbound's Steam
        // Publisher API key (it's a developer secret). Forcing
        // AuthType=None makes Photon skip Steam Web API
        // validation entirely so anonymous-allowed apps accept
        // the connection.
        InstallIl2CppHook(
            "Fusion.Realtime", "Fusion.Photon.Realtime", "AuthenticationValues",
            "set_AuthType", 1,
            (void*)&Hooked_AuthValues_set_AuthType,
            (void**)&g_pfnOrigAuthValuesSetAuthType,
            "AuthenticationValues.set_AuthType");

        // NOTE: set_Token hook is intentionally DISABLED. Nulling
        // the Token while AuthType=None puts AuthenticationValues
        // in a state Photon Fusion's internal preconditions don't
        // accept; the game gets stuck in a retry loop before any
        // network attempt is made (observed Photon allocating
        // tens of thousands of AuthValues per second with no
        // StartGame / GetAuthSessionTicket activity).
        //
        // With only AuthType forced to None, Photon's anonymous
        // path accepts the connection if the app's Anonymous
        // Authentication is enabled on the dashboard.
        // InstallIl2CppHook(
        //     "Fusion.Realtime", "Fusion.Photon.Realtime", "AuthenticationValues",
        //     "set_Token", 1,
        //     (void*)&Hooked_AuthValues_set_Token,
        //     (void**)&g_pfnOrigAuthValuesSetToken,
        //     "AuthenticationValues.set_Token");

        // Critical: catch the AppId at the connection layer.
        // Fusion.Photon.Realtime.LoadBalancingClient.set_AppId is
        // called immediately before connecting -- this is where
        // the AppId really matters.
        InstallIl2CppHook(
            "Fusion.Realtime", "Fusion.Photon.Realtime", "LoadBalancingClient",
            "set_AppId", 1,
            (void*)&Hooked_LBC_set_AppId,
            (void**)&g_pfnOrigLBCsetAppId,
            "LoadBalancingClient.set_AppId");

        // The most reliable target: FusionRelayClient.ConnectUsingSettings.
        // Takes the AppSettings object that drives the actual Photon
        // connection. We mutate AppIdFusion in-place before calling
        // original.
        InstallIl2CppHook(
            "Fusion.Realtime", "Fusion.Photon.Realtime", "FusionRelayClient",
            "ConnectUsingSettings", 1,
            (void*)&Hooked_FRC_ConnectUsingSettings,
            (void**)&g_pfnOrigFRCconnectUsingSettings,
            "FusionRelayClient.ConnectUsingSettings");

        // The actual entry point Outbound uses. Patches the
        // per-call CustomPhotonAppSettings override carried in
        // StartGameArgs.
        InstallIl2CppHook(
            "Fusion.Runtime", "Fusion", "NetworkRunner",
            "StartGame", 1,
            (void*)&Hooked_NetworkRunner_StartGame,
            (void**)&g_pfnOrigNetworkRunnerStartGame,
            "NetworkRunner.StartGame");

        // THE actual wire-time override. Photon's OpAuthenticate
        // is what sends the auth op to the master server; we
        // rewrite authValues.authType byte just before it goes
        // out. Catches any AuthType inlining the C# compiler
        // did at game-side assignment sites.
        InstallIl2CppHook(
            "Fusion.Realtime", "Fusion.Photon.Realtime", "LoadBalancingClient",
            "OpAuthenticate", 5,
            (void*)&Hooked_OpAuthenticate,
            (void**)&g_pfnOrigOpAuthenticate,
            "LoadBalancingClient.OpAuthenticate");

        InstallIl2CppHook(
            "Fusion.Realtime", "Fusion.Photon.Realtime", "LoadBalancingClient",
            "OpAuthenticateOnce", 6,
            (void*)&Hooked_OpAuthenticateOnce,
            (void**)&g_pfnOrigOpAuthenticateOnce,
            "LoadBalancingClient.OpAuthenticateOnce");
    }
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

    // Read [Outbound] PhotonAppIdFusion from union-crax.ini.
    // If set (typically a 36-char GUID from the user's own
    // Photon Fusion Cloud app), we patch the game to use it
    // -- bypassing Outbound's backend-validated custom auth.
    const char* ini = GetIniPath();
    if (ini)
    {
        GetPrivateProfileStringA("Outbound", "PhotonAppIdFusion", "",
                                 g_OurFusionAppIdUtf8,
                                 sizeof(g_OurFusionAppIdUtf8), ini);
        if (g_OurFusionAppIdUtf8[0])
        {
            g_AppIdPatchEnabled = true;
            LOG("[Outbound] PhotonAppIdFusion override set: %s", g_OurFusionAppIdUtf8);
        }
        else
        {
            LOG("[Outbound] no [Outbound]PhotonAppIdFusion in ini -- AppId patch disabled");
        }

        // Optional AuthType override; defaults to 0 (Custom).
        // Set to 255 for None (anonymous), 1 for Steam, etc.
        char authTypeBuf[8] = {};
        GetPrivateProfileStringA("Outbound", "ForcedAuthType", "0",
                                 authTypeBuf, sizeof(authTypeBuf), ini);
        unsigned int parsed = (unsigned int)strtoul(authTypeBuf, nullptr, 10);
        if (parsed <= 255) g_ForcedAuthType = parsed;
        LOG("[Outbound] forced AuthType = %u", g_ForcedAuthType);
    }

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
