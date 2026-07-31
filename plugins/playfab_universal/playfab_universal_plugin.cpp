// ============================================================
// UCOnline2 -- playfab_universal Plugin
//
// Game-agnostic PlayFab redirect for Unity/Mono games running
// under UCOnline2. Points a game's PlayFab traffic at a title
// YOU control and logs in without Steam ownership validation.
//
// WHY THIS IS NEEDED
//   A game's own PlayFab title only accepts LoginWithSteam, which
//   makes PlayFab ask Steam "does this ticket own AppId X?". Under
//   UCOnline2 the ticket is minted for the spoofed AppId 480, so
//   that check fails and multiplayer never initialises. You cannot
//   fix this on the publisher's title -- but PlayFab titles are
//   free, so you point the game at your own and log in with an
//   anonymous CustomID instead. Same escape hatch as EOS_custom.
//
// MODULES (all optional, all driven from union-crax.ini)
//   1. TITLE REDIRECT (Mono)
//      PlayFab.PlayFabSettings.TitleId -> [PlayFab]TitleId.
//      Set at the source, so it is transport-agnostic.
//      (WinHTTP hooking does NOT reach this: UnityPlayer only
//      imports WinHttpGetIEProxyConfigForCurrentUser -- Unity's
//      UnityWebRequest has its own HTTP stack.)
//
//   2. LOGIN SWITCH (Mono, PlayFabUnityHttp.MakeApiCall)
//      Rewrites the outgoing request mid-flight:
//        URL   /Client/LoginWithSteam -> /Client/LoginWithCustomID
//        body  -> {"TitleId":"<ours>","CustomId":"<id>","CreateAccount":true}
//      LoginResult is identical either way, so the game's own
//      success handler runs unchanged and gets a real EntityToken.
//      This hook also acts as a safety net for module 1: any
//      *.playfabapi.com URL still pointing at another title has its
//      host rewritten here, even if set_TitleId landed late.
//
//   3. NATIVE ENDPOINT REDIRECT (WinHTTP)
//      PlayFab Party (PartyWin32.dll / PartyWin.dll) is native and
//      never sees the C# TitleId, so it POSTs to the ORIGINAL title
//      with OUR entity token -> "InvalidAPIEndpoint". Rewriting the
//      WinHttpConnect host fixes it; both hosts share the
//      *.playfabapi.com wildcard cert so TLS still validates.
//
//   4. OFFLINE-GATE UNLOCK (Mono, optional)
//      Force a static bool (e.g. Raft_Network.SignedIntoPlayfab)
//      true so the multiplayer UI is usable before login resolves.
//
// Mono-only. MinHook statically linked.
// ============================================================
#include <Windows.h>
#include <winhttp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#include "../../include/MinHook.h"
#include "../../include/uco_plugin.h"
#include "mono_runtime.h"

static UCO_LogFn      g_Log            = nullptr;
static volatile LONG  g_bShutdown      = 0;
static HANDLE         g_hWatcherThread = nullptr;
static ISteamUser*    g_pSteamUser     = nullptr;

// ---- config ------------------------------------------------
static char    g_TitleIdA[64]    = {};   // our PlayFab TitleId
static wchar_t g_TitleIdW[64]    = {};   // wide, for the WinHTTP host rewrite
static char    g_CustomIdA[128]  = {};   // literal CustomId; empty = use Steam ID
static char    g_LoginEndpoints[256] = {};  // comma-separated, e.g. "LoginWithSteam"
static bool    g_bNativeRedirect = true;    // module 3 on/off

static char    g_GateAssembly[64]  = {};
static char    g_GateNamespace[64] = {};
static char    g_GateClass[64]     = {};
static char    g_GateField[64]     = {};

#define LOG(...) do { if (g_Log) g_Log(__VA_ARGS__); } while (0)

extern "C" void MONO_Log(const char* fmt, ...)
{
    if (!g_Log) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_Log("%s", buf);
}

static const char* GetIniPath()
{
    static char path[MAX_PATH] = {};
    static bool computed = false;
    if (computed) return path[0] ? path : nullptr;
    computed = true;
    char exeDir[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    if (len == 0) return nullptr;
    for (int i = (int)len - 1; i >= 0; --i)
        if (exeDir[i] == '\\' || exeDir[i] == '/') { exeDir[i] = 0; break; }
    int n = _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\union-crax.ini", exeDir);
    if (n <= 0) { path[0] = 0; return nullptr; }
    return path;
}

typedef uint64_t (*Fn_GetSteamID)(ISteamUser*);
static uint64_t GetRealSteamID()
{
    if (!g_pSteamUser) return 0;
    HMODULE h = GetModuleHandleA("steam_api64.dll");
    if (!h) return 0;
    Fn_GetSteamID f = (Fn_GetSteamID)GetProcAddress(h, "SteamAPI_ISteamUser_GetSteamID");
    if (!f) return 0;
    return f(g_pSteamUser);
}

// The identity we present to our own PlayFab title. Stable per Steam
// account, so a player keeps the same PlayFab entity across sessions.
static const char* GetCustomId()
{
    static char id[128] = {};
    if (id[0]) return id;
    if (g_CustomIdA[0]) {
        strncpy_s(id, sizeof(id), g_CustomIdA, _TRUNCATE);
    } else {
        _snprintf_s(id, sizeof(id), _TRUNCATE, "%llu",
                    (unsigned long long)GetRealSteamID());
    }
    return id;
}

// ------------------------------------------------------------
// MODULE 1: PlayFab TitleId redirect (managed set_TitleId)
// ------------------------------------------------------------
typedef void (*Fn_setTitleId)(void* /*MonoString*/);
static volatile LONG g_TitleIdRedirected = 0;

static void TryRedirectTitleId()
{
    if (!g_TitleIdA[0]) return;
    if (InterlockedCompareExchange(&g_TitleIdRedirected, 0, 0)) return;
    void* fn = MONO_FindMethodPtr("PlayFab", "PlayFab", "PlayFabSettings", "set_TitleId", 1);
    if (!fn) return;
    MonoString* s = MONO_StringNew(g_TitleIdA);
    if (!s) return;
    ((Fn_setTitleId)fn)(s);
    InterlockedExchange(&g_TitleIdRedirected, 1);
    LOG("[PlayFab] PlayFabSettings.TitleId -> %s", g_TitleIdA);
}

// ------------------------------------------------------------
// MODULE 2: login switch + host safety net
// (PlayFabUnityHttp.MakeApiCall)
// ------------------------------------------------------------
typedef void(__fastcall* Fn_MakeApiCall)(void* pThis, void* reqContainer);
static Fn_MakeApiCall g_origMakeApiCall   = nullptr;
static int            g_offFullUrl        = -1;
static int            g_offPayload        = -1;
static int            g_offApiEndpoint    = -1;
static volatile LONG  g_LoginSwitchLogged = 0;
static volatile LONG  g_HostFixLogged     = 0;

// True if `url` names one of the platform logins we redirect. Kept to an
// explicit list so ordinary API calls are never touched.
static const char* MatchLoginEndpoint(const char* url)
{
    const char* list = g_LoginEndpoints[0] ? g_LoginEndpoints : "LoginWithSteam";
    char buf[256];
    strncpy_s(buf, sizeof(buf), list, _TRUNCATE);

    char* ctx = nullptr;
    for (char* tok = strtok_s(buf, ",; \t", &ctx); tok; tok = strtok_s(nullptr, ",; \t", &ctx)) {
        // The result points into `url` (the caller's buffer), not into `buf`,
        // so it stays valid after this function returns.
        const char* hit = strstr(url, tok);
        if (hit) return hit;
    }
    return nullptr;
}

// Rewrite the <title> in https://<title>.playfabapi.com/... to ours.
// Returns true if `out` was written.
static bool RewriteHost(const char* url, char* out, size_t outSize)
{
    if (!g_TitleIdA[0]) return false;
    const char* dom = strstr(url, ".playfabapi.com");
    if (!dom) return false;

    const char* schemeEnd = strstr(url, "://");
    const char* hostStart = schemeEnd ? schemeEnd + 3 : url;
    if (hostStart > dom) return false;

    size_t hostLen = (size_t)(dom - hostStart);
    if (hostLen == strlen(g_TitleIdA) &&
        _strnicmp(hostStart, g_TitleIdA, hostLen) == 0)
        return false;   // already ours

    _snprintf_s(out, outSize, _TRUNCATE, "%.*s%s%s",
                (int)(hostStart - url), url, g_TitleIdA, dom);
    return true;
}

static void __fastcall Hooked_MakeApiCall(void* pThis, void* reqContainer)
{
    if (reqContainer && g_offFullUrl >= 0 && g_TitleIdA[0]) {
        MonoString* urlObj = *(MonoString**)((uint8_t*)reqContainer + g_offFullUrl);
        char url[512] = {};
        if (urlObj && MONO_StringToUtf8(urlObj, url, sizeof(url))) {

            const char* hit = MatchLoginEndpoint(url);
            if (hit) {
                // -- platform login -> LoginWithCustomID --
                // Length of the matched endpoint name, so the tail is preserved.
                size_t pre    = (size_t)(hit - url);
                size_t nameLen = 0;
                while (hit[nameLen] && hit[nameLen] != '?' && hit[nameLen] != '/')
                    ++nameLen;

                char newUrl[512] = {};
                _snprintf_s(newUrl, sizeof(newUrl), _TRUNCATE, "%.*s%s%s",
                            (int)pre, url, "LoginWithCustomID", hit + nameLen);

                // Point it at our title too, in case module 1 lost the race.
                char fixedUrl[512] = {};
                const char* finalUrl = RewriteHost(newUrl, fixedUrl, sizeof(fixedUrl))
                                     ? fixedUrl : newUrl;

                MonoString* nu = MONO_StringNew(finalUrl);
                if (nu) *(void**)((uint8_t*)reqContainer + g_offFullUrl) = nu;

                if (g_offApiEndpoint >= 0) {
                    MonoString* ep = MONO_StringNew("/Client/LoginWithCustomID");
                    if (ep) *(void**)((uint8_t*)reqContainer + g_offApiEndpoint) = ep;
                }

                const char* cid = GetCustomId();
                char body[320] = {};
                int blen = _snprintf_s(body, sizeof(body), _TRUNCATE,
                    "{\"TitleId\":\"%s\",\"CustomId\":\"%s\",\"CreateAccount\":true}",
                    g_TitleIdA, cid);
                if (blen > 0 && g_offPayload >= 0) {
                    MonoObject* arr = MONO_NewByteArray(body, blen);
                    if (arr) *(void**)((uint8_t*)reqContainer + g_offPayload) = arr;
                }

                if (InterlockedExchange(&g_LoginSwitchLogged, 1) == 0)
                    LOG("[PlayFab] login switched -> LoginWithCustomID (CustomId=%s, body=%d bytes)",
                        cid, blen);
            } else {
                // -- safety net: any other call still aimed at the old title --
                char fixedUrl[512] = {};
                if (RewriteHost(url, fixedUrl, sizeof(fixedUrl))) {
                    MonoString* nu = MONO_StringNew(fixedUrl);
                    if (nu) *(void**)((uint8_t*)reqContainer + g_offFullUrl) = nu;
                    if (InterlockedExchange(&g_HostFixLogged, 1) == 0)
                        LOG("[PlayFab] late host rewrite -> %s (set_TitleId lost the race)",
                            fixedUrl);
                }
            }
        }
    }
    if (g_origMakeApiCall) g_origMakeApiCall(pThis, reqContainer);
}

// ------------------------------------------------------------
// MODULE 3: native Party endpoint redirect (WinHTTP)
// ------------------------------------------------------------
typedef HINTERNET (WINAPI* Fn_WinHttpConnect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
static Fn_WinHttpConnect g_origWinHttpConnect = nullptr;
static volatile LONG     g_WinHttpRedirLogged = 0;

static HINTERNET WINAPI Hooked_WinHttpConnect(HINTERNET hSession, LPCWSTR pswzServerName,
                                              INTERNET_PORT nServerPort, DWORD dwReserved)
{
    if (pswzServerName && g_TitleIdW[0] && wcsstr(pswzServerName, L".playfabapi.com")) {
        wchar_t newName[128] = {};
        _snwprintf_s(newName, _countof(newName), _TRUNCATE, L"%s.playfabapi.com", g_TitleIdW);
        if (_wcsicmp(pswzServerName, newName) != 0) {
            if (InterlockedExchange(&g_WinHttpRedirLogged, 1) == 0)
                LOG("[PlayFab] WinHttpConnect redirect %ls -> %ls", pswzServerName, newName);
            return g_origWinHttpConnect(hSession, newName, nServerPort, dwReserved);
        }
    }
    return g_origWinHttpConnect(hSession, pswzServerName, nServerPort, dwReserved);
}

// ------------------------------------------------------------
// MODULE 4: offline-gate unlock (optional static bool force)
// ------------------------------------------------------------
static MonoClass*    g_GateKlass     = nullptr;
static volatile LONG g_GateLogged    = 0;

static bool GateConfigured() { return g_GateClass[0] && g_GateField[0]; }

static void ApplyGate()
{
    if (!GateConfigured()) return;
    if (!g_GateKlass) {
        g_GateKlass = MONO_FindClass(g_GateAssembly[0] ? g_GateAssembly : nullptr,
                                     g_GateNamespace, g_GateClass);
        if (!g_GateKlass) return;
    }
    uint8_t trueVal = 1;
    if (MONO_SetStaticFieldByName(g_GateKlass, g_GateField, &trueVal)) {
        if (InterlockedExchange(&g_GateLogged, 1) == 0)
            LOG("[PlayFab] gate forced: %s.%s = true", g_GateClass, g_GateField);
    }
}

// ------------------------------------------------------------
// Install (idempotent per-hook; quiet retry until JIT-ready)
// ------------------------------------------------------------
static volatile LONG g_LoginHookDone   = 0;
static volatile LONG g_WinHttpHookDone = 0;

static bool TryInstallAll()
{
    if (!MONO_TryInit()) return false;

    TryRedirectTitleId();

    // -- login switch / host safety net --
    if (!InterlockedCompareExchange(&g_LoginHookDone, 0, 0)) {
        MonoClass* crc = MONO_FindClass("PlayFab", "PlayFab.Internal", "CallRequestContainer");
        void* mac = MONO_FindMethodPtr("PlayFab", "PlayFab.Internal", "PlayFabUnityHttp", "MakeApiCall", 1);
        if (crc && mac) {
            g_offFullUrl     = MONO_GetFieldOffset(crc, "FullUrl");
            g_offPayload     = MONO_GetFieldOffset(crc, "Payload");
            g_offApiEndpoint = MONO_GetFieldOffset(crc, "ApiEndpoint");
            if (g_offFullUrl >= 0 && g_offPayload >= 0 &&
                MH_CreateHook(mac, (void*)&Hooked_MakeApiCall, (void**)&g_origMakeApiCall) == MH_OK) {
                MH_EnableHook(mac);
                InterlockedExchange(&g_LoginHookDone, 1);
                LOG("[PlayFab] PlayFabUnityHttp.MakeApiCall hook @ %p (FullUrl off 0x%X, Payload off 0x%X)",
                    mac, (unsigned)g_offFullUrl, (unsigned)g_offPayload);
            }
        }
    }

    // -- native Party endpoint redirect --
    if (g_bNativeRedirect && g_TitleIdW[0] &&
        !InterlockedCompareExchange(&g_WinHttpHookDone, 0, 0)) {
        HMODULE hWin = GetModuleHandleW(L"winhttp.dll");
        if (hWin) {
            void* pc = (void*)GetProcAddress(hWin, "WinHttpConnect");
            if (pc && MH_CreateHook(pc, (void*)&Hooked_WinHttpConnect,
                                    (void**)&g_origWinHttpConnect) == MH_OK) {
                MH_EnableHook(pc);
                InterlockedExchange(&g_WinHttpHookDone, 1);
                LOG("[PlayFab] WinHttpConnect hook @ %p (redirect *.playfabapi.com -> %s.playfabapi.com)",
                    pc, g_TitleIdA);
            }
        }
    }

    ApplyGate();

    bool loginReady   = InterlockedCompareExchange(&g_LoginHookDone, 0, 0) != 0;
    bool nativeReady  = (!g_bNativeRedirect || !g_TitleIdW[0] ||
                         InterlockedCompareExchange(&g_WinHttpHookDone, 0, 0) != 0);
    bool titleReady   = (!g_TitleIdA[0] ||
                         InterlockedCompareExchange(&g_TitleIdRedirected, 0, 0) != 0);
    bool gateReady    = (!GateConfigured() || g_GateKlass != nullptr);

    return loginReady && nativeReady && titleReady && gateReady;
}

static DWORD WINAPI WatcherProc(LPVOID)
{
    bool installed = false;
    // Poll fast at first: the TitleId redirect has to land before the game's
    // first PlayFab call. (The MakeApiCall host rewrite covers us if it doesn't.)
    for (int i = 0; i < 1500 && InterlockedCompareExchange(&g_bShutdown, 0, 0) == 0; ++i) {
        if (TryInstallAll()) { installed = true; break; }
        Sleep(i < 100 ? 50 : 200);
    }

    if (!installed) {
        LOG("[PlayFab] watcher gave up (LoginHook=%ld TitleRedirect=%ld WinHttpHook=%ld)",
            InterlockedCompareExchange(&g_LoginHookDone, 0, 0),
            InterlockedCompareExchange(&g_TitleIdRedirected, 0, 0),
            InterlockedCompareExchange(&g_WinHttpHookDone, 0, 0));
        return 0;
    }

    // The gate is a static the game rewrites every frame, so it has to be
    // re-forced for as long as the process lives. Everything else is one-shot.
    while (GateConfigured() && InterlockedCompareExchange(&g_bShutdown, 0, 0) == 0) {
        ApplyGate();
        Sleep(200);
    }
    return 0;
}

extern "C" __declspec(dllexport) int __cdecl UCO_PluginInit(const UCO_PluginContext* ctx)
{
    if (!ctx) return 1;
    if (ctx->ApiVersion != UCO_PLUGIN_API_VERSION) return 2;
    g_Log        = ctx->Log;
    g_pSteamUser = ctx->pSteamUser;

    const char* ini = GetIniPath();
    if (ini) {
        // [PlayFab] is the documented section. Section/key lookup is
        // case-insensitive, so [Playfab] works too; [Raft] is read last
        // so existing raft_mp installs keep working unchanged.
        GetPrivateProfileStringA("PlayFab", "TitleId", "", g_TitleIdA, sizeof(g_TitleIdA), ini);
        if (!g_TitleIdA[0]) GetPrivateProfileStringA("PlayFab", "PlayFabTitleId", "", g_TitleIdA, sizeof(g_TitleIdA), ini);
        if (!g_TitleIdA[0]) GetPrivateProfileStringA("Raft",    "PlayFabTitleId", "", g_TitleIdA, sizeof(g_TitleIdA), ini);
        if (g_TitleIdA[0])
            MultiByteToWideChar(CP_ACP, 0, g_TitleIdA, -1, g_TitleIdW, _countof(g_TitleIdW));

        GetPrivateProfileStringA("PlayFab", "CustomId",       "",               g_CustomIdA,     sizeof(g_CustomIdA),     ini);
        GetPrivateProfileStringA("PlayFab", "LoginEndpoints", "LoginWithSteam", g_LoginEndpoints, sizeof(g_LoginEndpoints), ini);
        g_bNativeRedirect = GetPrivateProfileIntA("PlayFab", "RedirectNativeHttp", 1, ini) != 0;

        GetPrivateProfileStringA("PlayFab", "GateAssembly",  "", g_GateAssembly,  sizeof(g_GateAssembly),  ini);
        GetPrivateProfileStringA("PlayFab", "GateNamespace", "", g_GateNamespace, sizeof(g_GateNamespace), ini);
        GetPrivateProfileStringA("PlayFab", "GateClass",     "", g_GateClass,     sizeof(g_GateClass),     ini);
        GetPrivateProfileStringA("PlayFab", "GateField",     "", g_GateField,     sizeof(g_GateField),     ini);
    }

    LOG("[PlayFab] init: AppId=%u ogAppId=%u pSteamUser=%p TitleId=%s logins=%s nativeRedirect=%d gate=%s",
        ctx->ForcedAppId, ctx->OriginalAppId, (void*)g_pSteamUser,
        g_TitleIdA[0] ? g_TitleIdA : "(none - plugin idle)",
        g_LoginEndpoints, g_bNativeRedirect ? 1 : 0,
        GateConfigured() ? g_GateClass : "(none)");

    if (!g_TitleIdA[0]) {
        LOG("[PlayFab] no [PlayFab]TitleId in union-crax.ini -- nothing to do");
        return 0;
    }

    if (MH_Initialize() != MH_OK)
        LOG("[PlayFab] MH_Initialize non-OK (already inited?)");

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
    LOG("[PlayFab] shutdown");
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
