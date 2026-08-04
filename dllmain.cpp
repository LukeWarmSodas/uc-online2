#include <Windows.h>
#include <Shlwapi.h>
#include <DbgHelp.h>
#include <new.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vector>

#define STEAM_API_EXPORTS

#include "include/sdk/steam_api.h"
#include "include/sdk/steamclientpublic.h"
#include "include/sdk/steam_gameserver.h"
#include "include/sdk/steamtypes.h"

S_API ISteamClient* g_pSteamClientGameServer = nullptr;

#include <vector>

#include "include/registfuncs.h"
#include "include/callback_dispatcher.h"
#include "include/uco_plugin.h"
#include "include/globals.h"
#include "include/uc_loader.h"
#include "include/dump_handler.h"
#include "include/MinHook.h"

#include "include/api/api_callbacks.h"
#include "include/api/api_client.h"
#include "include/api/api_interfaces.h"
#include "include/api/api_interfaces_v.h"
#include "include/api/api_gameserver.h"
#include "include/api/api_breakpad.h"
#include "include/api/api_shutdown.h"
#include "include/api/api_factory.h"
#include "include/api/api_flat.h"

// ============================================================
// Global variable definitions
// ============================================================

HMODULE g_ClientModule = nullptr;
HSteamPipe g_ClientPipe = 0;
HSteamUser g_ClientUser = 0;
ISteamClient* g_pSteamClient = nullptr;
ISteamClient* g_pSteamClientSafe = nullptr;
ISteamUtils* g_pUtilsForCallbacks = nullptr;
ISteamController* g_pControllerForCallbacks = nullptr;
ISteamInput* g_pInputForCallbacks = nullptr;
CSteamAPIContext g_ClientCtx;
bool g_bClientReady = false;
SRWLOCK g_CtxLock;

HMODULE g_ServerModule = nullptr;
HSteamPipe g_ServerPipe = 0;
HSteamUser g_ServerUser = 0;
ISteamClient* g_ServerClient = nullptr;
ISteamClient* g_pServerClient = nullptr;
ISteamClient* g_pSteamClientGameServer_Latest = nullptr;
ISteamGameServer* g_pGameServer = nullptr;
ISteamUtils* g_pServerUtils = nullptr;
CSteamGameServerAPIContext g_ServerCtx;
bool g_bServerReady = false;
EServerMode g_ServerMode = eServerModeInvalid;

bool g_bUsingBreakpad = false;
bool g_bFullDumps = false;
void* g_BreakpadCtx = nullptr;
PFNPreMinidumpCallback g_BreakpadCallback = nullptr;
char g_BreakpadVer[64] = { 0 };
char g_BreakpadTimestamp[64] = { 0 };
uint32 g_BreakpadAppID = 0;
uint64 g_BreakpadSteamID = 0;

// Default ON: route dispatch through CCallbackDispatcher::DispatchFrameSafe,
// whose try/catch (with /EHa) contains a faulting game callback instead of
// letting it kill the process. Games can still toggle this via
// SteamAPI_SetTryCatchCallbacks(). This is a safety net, not a substitute for
// the dispatcher's locking/lifetime fixes -- it just keeps a bad callback from
// being fatal (e.g. Forever Skies crashing from UE's online thread).
bool g_bTryCatch = true;
bool g_bVerboseLog = false;
int g_DispatchMode = 0;
char g_InstallPath[MAX_PATH] = { 0 };
bool g_bHaveInstallPath = false;
SRWLOCK g_CallbackLock;
uint32 g_ForcedAppId = 480;
uint32 g_OriginalAppId = 0;
// [Settings] EmulateTicket -- also gates auth-session-ticket emulation
// (see the ISteamUser hooks further down).
static bool g_bEmulateAuthTicket = false;

Fn_CreateInterface g_pfnCreateInterface = nullptr;
Fn_ReleaseThreadLocal g_pfnReleaseThreadLocal = nullptr;
Fn_IsKnownInterface g_pfnIsKnownInterface = nullptr;
Fn_NotifyMissing g_pfnNotifyMissing = nullptr;
Fn_BreakpadInit g_pfnBreakpadInit = nullptr;
Fn_BreakpadSetAppID g_pfnBreakpadSetAppID = nullptr;
Fn_BreakpadSetSteamID g_pfnBreakpadSetSteamID = nullptr;
Fn_BreakpadSetComment g_pfnBreakpadSetComment = nullptr;
Fn_BreakpadWriteDump g_pfnBreakpadWriteDump = nullptr;

uintp g_CtxCounter = 0;

// Forward declarations for SteamStub
static bool g_bSteamStubEnabled = false;
static void SteamStub_Init();

// ============================================================
// SteamInternal_ContextInit
// ============================================================

S_API void* S_CALLTYPE SteamInternal_ContextInit(void* pData)
{
	UCOLOG_HOT("[UCOnline2] SteamInternal_ContextInit");
	if (!pData) return nullptr;
	void** pArr = (void**)pData;
	uintp* pCounter = (uintp*)&pArr[1];
	char* pBase = (char*)pData;
	#if defined(_M_IX86)
		if (*pCounter == g_CtxCounter) return pBase + 8;
		AcquireSRWLockExclusive(&g_CtxLock);
		if (*pCounter != g_CtxCounter) { void(*pFn)(void*) = (void(*)(void*))pArr[0]; pFn(pBase + 8); *pCounter = g_CtxCounter; }
		ReleaseSRWLockExclusive(&g_CtxLock);
		return pBase + 8;
	#elif defined(_M_AMD64)
		if (*pCounter == g_CtxCounter) return pBase + 16;
		AcquireSRWLockExclusive(&g_CtxLock);
		if (*pCounter != g_CtxCounter) { void(*pFn)(void*) = (void(*)(void*))pArr[0]; pFn(pBase + 16); *pCounter = g_CtxCounter; }
		ReleaseSRWLockExclusive(&g_CtxLock);
		return pBase + 16;
	#endif
}

// ============================================================
// Logging
// ============================================================


static void UCOLogImpl(const char* fmt, va_list args)
{
	char msg[2048] = { 0 };
	_vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, args);

	char logPath[MAX_PATH] = { 0 };
	DWORD len = GetTempPathA(MAX_PATH, logPath);
	if (len == 0 || len > (MAX_PATH - 25)) return;

	if (!PathAppendA(logPath, "uc_online2.log")) return;

	FILE* f = nullptr;
	if (fopen_s(&f, logPath, "ab") != 0 || !f) return;

	SYSTEMTIME st = { 0 };
	GetLocalTime(&st);

	fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);

	size_t msgLen = strlen(msg);
	if (msgLen == 0 || msg[msgLen - 1] != '\n')
		fputs("\r\n", f);

	fclose(f);
}

void UCOLOG(const char* fmt, ...)
{
	if (!fmt) return;
	va_list args;
	va_start(args, fmt);
	UCOLogImpl(fmt, args);
	va_end(args);
}

void UCOColor(WORD color, const char* text)
{
	(void)color;
	if (text && text[0])
		UCOLOG("%s", text);
}

// ============================================================
// InitSteamClient // I seriously broke this later on in the
//				   // releases, I am SO SORRY ya'll!!
// ============================================================

void* InitSteamClient(HMODULE* phMod, bool bLocal, const char* iface)
{
	g_pUtilsForCallbacks = nullptr;
	g_pControllerForCallbacks = nullptr;
	g_pInputForCallbacks = nullptr;

	if (!phMod || !iface) return nullptr;

	*phMod = nullptr;

	char dllPath[MAX_PATH] = { 0 };
	const char* installDir = SteamAPI_GetSteamInstallPath();

	if (_stricmp(installDir, "UCOnline2_InvalidPath") == 0)
	{
		if (!bLocal) return nullptr;
	}
	else
	{
		#if defined(_M_IX86)
			_snprintf_s(dllPath, MAX_PATH, _TRUNCATE, "%s\\steamclient.dll", installDir);
		#elif defined(_M_AMD64)
			_snprintf_s(dllPath, MAX_PATH, _TRUNCATE, "%s\\steamclient64.dll", installDir);
		#endif
	}

	if (SteamAPI_IsSteamRunning())
	{
		*phMod = LoadLibraryExA(dllPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!*phMod)
			UCOColor(0, "[UCOnline2] Failed to load steamclient DLL");
	}
	else
	{
		UCOColor(0, "[UCOnline2] Steam is not running");
	}

	if (!*phMod)
	{
		if (!bLocal)
		{
			UCOColor(0, "[UCOnline2] No Steam instance and bLocal is false");
			return nullptr;
		}

		#if defined(_M_IX86)
			*phMod = LoadLibraryW(L"steamclient.dll");
		#elif defined(_M_AMD64)
			*phMod = LoadLibraryW(L"steamclient64.dll");
		#endif

		if (!*phMod)
		{
			UCOColor(0, "[UCOnline2] Cannot find steamclient DLL");
			return nullptr;
		}
	}

	g_pfnCreateInterface = (Fn_CreateInterface)GetProcAddress(*phMod, "CreateInterface");

	if (g_pfnCreateInterface)
	{
		g_pSteamClientSafe = (ISteamClient*)g_pfnCreateInterface("SteamClient023", nullptr);
		g_pfnReleaseThreadLocal = (Fn_ReleaseThreadLocal)GetProcAddress(*phMod, "Steam_ReleaseThreadLocalMemory");
		g_CtxCounter++;

		return g_pfnCreateInterface(iface, nullptr);
	}
	else
	{
		UCOColor(0, "[UCOnline2] CreateInterface not found in steamclient");
		FreeLibrary(*phMod);
		*phMod = nullptr;
	}

	return nullptr;
}


// ============================================================
// LoadGameOverlay
// ============================================================

static void LoadGameOverlay()
{
#if defined(_WIN32)
	#if defined(_M_IX86)
		HMODULE hOverlay = GetModuleHandleW(L"GameOverlayRenderer.dll");
	#elif defined(_M_AMD64)
		HMODULE hOverlay = GetModuleHandleW(L"GameOverlayRenderer64.dll");
	#endif

	if (g_ForcedAppId != 769 && !hOverlay)
	{
		const char* installPath = SteamAPI_GetSteamInstallPath();
		if (_stricmp(installPath, "UCOnline2_InvalidPath") != 0)
		{
			char overlayPath[MAX_PATH] = { 0 };
			#if defined(_M_IX86)
				_snprintf_s(overlayPath, MAX_PATH, _TRUNCATE, "%s\\GameOverlayRenderer.dll", installPath);
			#elif defined(_M_AMD64)
				_snprintf_s(overlayPath, MAX_PATH, _TRUNCATE, "%s\\GameOverlayRenderer64.dll", installPath);
			#endif
			HMODULE hLoaded = LoadLibraryExA(overlayPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			if (hLoaded)
			{
				UCOLOG("[UCOnline2] Loaded game overlay: %s", overlayPath);
			}
			else
			{
				UCOLOG("[UCOnline2] Failed to load game overlay: %s (error %lu)", overlayPath, GetLastError());
			}
		}
	}
#endif // _WIN32
}

// ============================================================
// DllMain
// ============================================================

// File-scope so the SteamAPI_Init path in api_client.h can call
// InitPlugins() on it, and DLL_PROCESS_DETACH can call
// ShutdownPlugins().
CDLLLoader s_PluginLoader;

BOOL WINAPI DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
	if (dwReason == DLL_PROCESS_ATTACH)
	{
		UCOLOG("[UCOnline2] DllMain -> DLL_PROCESS_ATTACH");

		s_PluginLoader.ReadConfig();
		g_ForcedAppId = s_PluginLoader.GetAppId();
		g_OriginalAppId = s_PluginLoader.GetOgAppId();

		SetAppIDEnv();
		WriteAppIDFile();

		// Load the game overlay early to ensure it can hook into graphics APIs
		LoadGameOverlay();

		char dllPath[MAX_PATH] = { 0 };
		DWORD len = GetModuleFileNameA(hModule, dllPath, sizeof(dllPath));

		if (len == 0)
			return FALSE;

		if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
			return FALSE;

		UCOLOG("[UCOnline2] DLL Path: %s", dllPath);

		// The actual likelihood of this being used or even working is low, as it wouldn't even work if it were named anything else.
		// However, I'm positive that compiling without this will cause it to scream at me and I no no like that. It make me maaaad.
		#if defined(_M_IX86)
			if (!StrStrIA(dllPath, "steam_api.dll"))
				UCOLOG("[UCOnline2] Warning: not named steam_api.dll");
		#elif defined(_M_AMD64)
			if (!StrStrIA(dllPath, "steam_api64.dll"))
				UCOLOG("[UCOnline2] Warning: not named steam_api64.dll");
		#endif

		UCOLOG("[UCOnline2] PID: %lu", GetCurrentProcessId());
		UCOLOG("[UCOnline2] Thread: %lu", GetCurrentThreadId());

		InitializeSRWLock(&g_CtxLock);
		InitializeSRWLock(&g_CallbackLock);

		// Read before anything chatty runs, so the hot-path traces honour it.
		g_bVerboseLog = s_PluginLoader.GetVerboseLog();
		if (g_bVerboseLog)
			UCOLOG("[UCOnline2] VerboseLog enabled -- per-frame callback traces will be logged");

		#ifdef _DEBUG
		g_pDumpHandler = new CDumpHandler();
		#endif

		s_PluginLoader.LoadPlugins();

		UCOLOG("[UCOnline2] %zu plugin(s) loaded", s_PluginLoader.LoadedCount());

		g_bSteamStubEnabled = s_PluginLoader.GetSteamStubEnabled();
		if (g_bSteamStubEnabled)
		{
			SteamStub_Init();
		}

		// DLC ownership: [DLC] UnlockAll + named entries, plus the legacy
		// [Settings] UnlockDLC list. Loaded before any game code can ask.
		UcoDlcStore::Load(s_PluginLoader.GetIniPath(), s_PluginLoader.GetOgAppId());
		UCOLOG("[UCOnline2] DLC store: UnlockAll=%d, %d entr%s",
			UcoDlcStore::UnlockAll() ? 1 : 0, UcoDlcStore::Count(),
			UcoDlcStore::Count() == 1 ? "y" : "ies");

		g_bEmulateAuthTicket = s_PluginLoader.GetEmulateTicketEnabled();
		if (s_PluginLoader.GetEmulateTicketEnabled()) {
			uint32 emulatedAppId = s_PluginLoader.GetOgAppId();
			if (emulatedAppId == 0) emulatedAppId = s_PluginLoader.GetAppId();
			CSteamUserStub::SetEmulatedApp(emulatedAppId);
		}
	}
	else if (dwReason == DLL_PROCESS_DETACH)
	{
		UCOLOG("[UCOnline2] DllMain -> DLL_PROCESS_DETACH");
		s_PluginLoader.ShutdownPlugins();
		if (g_bSteamStubEnabled)
		{
			MH_DisableHook(reinterpret_cast<LPVOID*>(GetTickCount));
			MH_Uninitialize();
		}
	}

	return TRUE;
}

// ============================================================
// CCallbackDispatcher
// ============================================================

static bool s_bDispatcherReady = false;

// ============================================================
// Callback patcher registry
//
// Plugins (see include/uco_plugin.h) can register a callback
// patcher for a specific iCallback. Patchers run -- in
// registration order -- on every matching callback before it
// is dispatched to the game's CCallback.
//
// The registry replaces the previous hard-coded auth callback
// patching. Per-game auth behavior now lives in a plugin.
// ============================================================
struct CallbackPatcherEntry
{
    int                    iCallback;
    UCO_CallbackPatcherFn  fn;
};
static std::vector<CallbackPatcherEntry> g_CallbackPatchers;
static SRWLOCK g_CallbackPatcherLock = SRWLOCK_INIT;

void UCO_RegisterCallbackPatcher(int iCallback, UCO_CallbackPatcherFn fn)
{
    if (!fn) return;
    AcquireSRWLockExclusive(&g_CallbackPatcherLock);
    g_CallbackPatchers.push_back({ iCallback, fn });
    ReleaseSRWLockExclusive(&g_CallbackPatcherLock);
    UCOLOG("[UCOnline2] Callback patcher registered for iCallback=%d", iCallback);
}

static void RunCallbackPatchers(int iCallback, uint8* pBuf, uint32 cbBuf)
{
    if (!pBuf || cbBuf == 0) return;
    AcquireSRWLockShared(&g_CallbackPatcherLock);
    // Iterate by index in case a patcher misbehaves and re-enters.
    size_t n = g_CallbackPatchers.size();
    for (size_t i = 0; i < n; i++)
    {
        const auto& e = g_CallbackPatchers[i];
        if (e.iCallback == iCallback)
            e.fn(pBuf, cbBuf);
    }
    ReleaseSRWLockShared(&g_CallbackPatcherLock);
}

CCallbackDispatcher::CCallbackDispatcher()
{
	UCOColor(FOREGROUND_BLUE | FOREGROUND_INTENSITY, "[UCOnline2] CCallbackDispatcher constructed\r\n");

	// Must exist before any thread can reach the maps.
	InitializeCriticalSection(&m_MapLock);

	m_pfnBGetCallback = nullptr;
	m_pfnFreeLastCallback = nullptr;
	m_pfnGetAPICallResult = nullptr;
	m_CurrentUser = 0;
	m_ManualCbId = 0;
	m_ManualCbSize = 0;
	m_bProcessing = false;
	m_CallbackMap.clear();
	m_CallResultMap.clear();
	m_BufferMap.clear();
	s_bDispatcherReady = true;
}

CCallbackDispatcher::~CCallbackDispatcher()
{
	UCOColor(FOREGROUND_BLUE | FOREGROUND_INTENSITY, "[UCOnline2] CCallbackDispatcher destroyed\r\n");

	s_bDispatcherReady = false;
	m_pfnBGetCallback = nullptr;
	m_pfnFreeLastCallback = nullptr;
	m_pfnGetAPICallResult = nullptr;
	m_CurrentUser = 0;
	m_ManualCbId = 0;
	m_ManualCbSize = 0;
	m_bProcessing = false;
	{
		CDispatcherLock lock(&m_MapLock);
		m_CallbackMap.clear();
		m_CallResultMap.clear();
		m_BufferMap.clear();
	}
	DeleteCriticalSection(&m_MapLock);
}

void CCallbackDispatcher::Shutdown()
{
	UCOColor(FOREGROUND_BLUE | FOREGROUND_INTENSITY, "[UCOnline2] CCallbackDispatcher shutdown\r\n");

	s_bDispatcherReady = false;
	m_pfnBGetCallback = nullptr;
	m_pfnFreeLastCallback = nullptr;
	m_pfnGetAPICallResult = nullptr;
	m_CurrentUser = 0;
	m_ManualCbId = 0;
	m_ManualCbSize = 0;
	m_bProcessing = false;
	{
		CDispatcherLock lock(&m_MapLock);
		m_CallbackMap.clear();
		m_CallResultMap.clear();
		m_BufferMap.clear();
	}
}

void CCallbackDispatcher::ExecuteCallResult(HSteamPipe hPipe, SteamAPICall_t hCall, CCallbackBase* pCb)
{
	if (!pCb)
		return;

	UCOLOG_HOT("[UCOnline2] ExecuteCallResult -> call=%lld cb=%d size=%d\r\n", hCall, pCb->GetICallback(), pCb->GetCallbackSizeBytes());

	BYTE* pBuffer = new BYTE[pCb->GetCallbackSizeBytes()]();
	bool bFailed = false;

	bool bResult = m_pfnGetAPICallResult(hPipe, hCall, pBuffer, pCb->GetCallbackSizeBytes(), pCb->GetICallback(), &bFailed);

	if (bResult && !bFailed)
	{
		size_t countBefore = m_CallbackMap.size();

		pCb->Run(pBuffer, bFailed, hCall);

		if (countBefore != m_CallbackMap.size())
		{
			UCOColor(FOREGROUND_BLUE | FOREGROUND_INTENSITY, "[UCOnline2] Callback map changed during CallResult execution\r\n");

			m_CallbackMap.erase(LobbyEnter_t::k_iCallback);
			pCb->Run(pBuffer);
		}

		m_BufferMap.insert(std::make_pair(hCall, pBuffer));
	}
	else
	{
		UCOColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[UCOnline2] GetAPICallResult failed\r\n");
		delete[] pBuffer;
	}

	// Retire the one-shot registration. The SDK makes the DISPATCHER own this:
	//     CCallResult<T,P>::Run(...) { m_hAPICall = k_uAPICallInvalid; // caller unregisters for us
	// (see include/sdk/steam_api_internal.h). Because Run() clears the handle,
	// the game's ~CCallResult() -> Cancel() sees an invalid handle and never
	// calls SteamAPI_UnregisterCallResult. If we don't drop the entry here it
	// lingers in m_CallResultMap and becomes a DANGLING pointer the moment the
	// game frees the object -- the next SteamAPICallCompleted_t match loop then
	// calls its virtuals (GetICallback / GetCallbackSizeBytes) on freed memory.
	{
		CDispatcherLock lock(&m_MapLock);
		m_CallResultMap.erase(hCall);
	}
}

// ------------------------------------------------------------
// Synthetic callbacks.
//
// Emulating an API call is only half the job: games that call an async Steam
// function register its response callback and block on it. Real Steam will
// never send a response for a call we answered ourselves, so we have to deliver
// it. This is the only path by which a callback we invented reaches the game.
//
// Delivery happens on whichever thread drives SteamAPI_RunCallbacks, under the
// same lock and the same matching rules as a real callback, so a game cannot
// tell the difference.
// ------------------------------------------------------------
void CCallbackDispatcher::PostCallback(int iCallback, const void* pvData, size_t cubData,
                                       HSteamUser user, bool bServer, unsigned delayMs)
{
	CDispatcherLock lock(&m_MapLock);
	UcoPendingCallback pending;
	pending.iCallback = iCallback;
	pending.user      = user;
	pending.bServer   = bServer;
	pending.dueTick   = GetTickCount64() + delayMs;
	if (pvData && cubData)
		pending.data.assign((const uint8_t*)pvData, (const uint8_t*)pvData + cubData);
	m_Synthetic.push_back(pending);
	UCOLOG("[UCOnline2] queued synthetic callback -> %d size=%zu delay=%ums",
		iCallback, cubData, delayMs);
}

// Find the one registered callback that should receive this id and run it.
// Extracted from DispatchFrame so real and synthetic delivery cannot drift
// apart. The caller must already hold m_MapLock.
bool CCallbackDispatcher::DispatchToTarget(int iCallback, void* pvData, uint32 cubData,
                                           HSteamUser user, bool bServer)
{
	CCallbackBase* pTarget = nullptr;
	bool bServerCb = false;

	for (auto it = m_CallbackMap.begin(); it != m_CallbackMap.end(); ++it)
	{
		CCallbackBase* pCb = it->second;
		if (!pCb)
			continue;

		if (it->first == iCallback && (pCb->m_nCallbackFlags & pCb->k_ECallbackFlagsRegistered))
		{
			if (user == g_ServerUser && (pCb->m_nCallbackFlags & pCb->k_ECallbackFlagsGameServer) && bServer)
			{
				pTarget = pCb; bServerCb = true; break;
			}
			else if (user == g_ClientUser && !(pCb->m_nCallbackFlags & pCb->k_ECallbackFlagsGameServer) && !bServer)
			{
				pTarget = pCb; bServerCb = false; break;
			}
		}
	}

	if (!pTarget)
		return false;

	(void)bServerCb;
	pTarget->Run(pvData);
	return true;
}

void CCallbackDispatcher::DrainSynthetic(bool bServer)
{
	// Snapshot the due entries under the lock, then dispatch them still holding
	// it -- same discipline as DispatchFrame, so a callback cannot be destroyed
	// mid-Run() by a concurrent unregister.
	CDispatcherLock lock(&m_MapLock);
	if (m_Synthetic.empty())
		return;

	const ULONGLONG now = GetTickCount64();
	for (size_t i = 0; i < m_Synthetic.size(); )
	{
		UcoPendingCallback& p = m_Synthetic[i];
		if (p.bServer != bServer || now < p.dueTick)
		{
			++i;
			continue;
		}

		UcoPendingCallback fire = p;                 // copy: the vector is edited below
		m_Synthetic.erase(m_Synthetic.begin() + (ptrdiff_t)i);

		const bool ran = DispatchToTarget(fire.iCallback, fire.data.empty() ? nullptr : fire.data.data(),
		                                  (uint32)fire.data.size(), fire.user, fire.bServer);
		UCOLOG("[UCOnline2] synthetic callback -> %d %s",
			fire.iCallback, ran ? "delivered" : "DROPPED (nothing registered for it)");
	}
}

void CCallbackDispatcher::DispatchFrame(HSteamPipe hPipe, bool bServer)
{
	// Before the early return below: Steam having nothing pending must not stop
	// our own queued callbacks from being delivered.
	DrainSynthetic(bServer);

	if (!m_pfnBGetCallback || !m_pfnFreeLastCallback || !m_pfnGetAPICallResult)
		return;

	CallbackMsg_t msg = { 0 };
	if (!m_pfnBGetCallback(hPipe, &msg))
		return;

	UCOLOG_HOT("[UCOnline2] Callback received -> %d\r\n", msg.m_iCallback);
	m_CurrentUser = msg.m_hSteamUser;

	// The lock is held for the whole dispatch, INCLUDING across Run().
	// That is deliberate and is what keeps the callback object alive: a game's
	// CCallback destructor calls SteamAPI_UnregisterCallback -> Remove(), which
	// needs this same lock, so an object cannot be destroyed while we are inside
	// its Run(). Releasing the lock first would reopen a use-after-free window.
	// The CRITICAL_SECTION is recursive, so a callback that registers or
	// unregisters others from inside Run() (same thread) still works.
	CDispatcherLock lock(&m_MapLock);

	if (msg.m_iCallback == SteamAPICallCompleted_t::k_iCallback && msg.m_cubParam == sizeof(SteamAPICallCompleted_t))
	{
		SteamAPICallCompleted_t* pCompleted = (SteamAPICallCompleted_t*)msg.m_pubParam;

		// Snapshot first: ExecuteCallResult re-enters and edits m_CallResultMap
		// (it retires the fired entry), which would invalidate a live iterator.
		std::vector<std::pair<SteamAPICall_t, CCallbackBase*> > matches;
		for (auto it = m_CallResultMap.begin(); it != m_CallResultMap.end(); ++it)
		{
			if (!it->second)
				continue;

			if (pCompleted->m_hAsyncCall == it->first &&
				pCompleted->m_iCallback == it->second->GetICallback() &&
				pCompleted->m_cubParam == (uint32)it->second->GetCallbackSizeBytes())
			{
				matches.push_back(*it);
			}
		}

		for (size_t i = 0; i < matches.size(); ++i)
			ExecuteCallResult(hPipe, matches[i].first, matches[i].second);
	}
	else
	{
		CCallbackBase* pTarget = nullptr;
		bool bServerCb = false;

		for (auto it = m_CallbackMap.begin(); it != m_CallbackMap.end(); ++it)
		{
			CCallbackBase* pCb = it->second;
			if (!pCb)
				continue;

			if (it->first == msg.m_iCallback && (pCb->m_nCallbackFlags & pCb->k_ECallbackFlagsRegistered))
			{
				if (msg.m_hSteamUser == g_ServerUser && (pCb->m_nCallbackFlags & pCb->k_ECallbackFlagsGameServer) && bServer)
				{
					pTarget = pCb;
					bServerCb = true;
					break;
				}
				else if (msg.m_hSteamUser == g_ClientUser && !(pCb->m_nCallbackFlags & pCb->k_ECallbackFlagsGameServer) && !bServer)
				{
					pTarget = pCb;
					bServerCb = false;
					break;
				}
			}
		}

		if (pTarget)
		{
			if (bServerCb)
			{
				UCOLOG_HOT("[UCOnline2] Server callback -> %d flags=%d\r\n", msg.m_iCallback, pTarget->m_nCallbackFlags);
				pTarget->Run(msg.m_pubParam);
			}
			else
			{
				UCOLOG_HOT("[UCOnline2] Client callback -> %d flags=%d\r\n", msg.m_iCallback, pTarget->m_nCallbackFlags);

				bool bSkip = false;

				if (msg.m_iCallback == HTML_NeedsPaint_t::k_iCallback && msg.m_cubParam == sizeof(HTML_NeedsPaint_t))
				{
					HTML_NeedsPaint_t* pPaint = (HTML_NeedsPaint_t*)msg.m_pubParam;
					if (pPaint->unWide == 1 || pPaint->unTall == 1)
						bSkip = true;
				}

				RunCallbackPatchers(msg.m_iCallback, msg.m_pubParam, msg.m_cubParam);

				if (!bSkip)
					pTarget->Run(msg.m_pubParam);
			}
		}
	}

	UCOLOG_HOT("[UCOnline2] Freeing callback -> %d\r\n", msg.m_iCallback);
	SecureZeroMemory(msg.m_pubParam, msg.m_cubParam);
	m_pfnFreeLastCallback(hPipe);
}

void CCallbackDispatcher::DispatchFrameSafe(HSteamPipe hPipe, bool bServer)
{
	try { DrainSynthetic(bServer); } catch (...) { }

	if (!m_pfnBGetCallback || !m_pfnFreeLastCallback || !m_pfnGetAPICallResult)
		return;

	CallbackMsg_t msg = { 0 };
	if (!m_pfnBGetCallback(hPipe, &msg))
		return;

	// The callback MUST be freed whether or not dispatch throws. If we skipped
	// the free on the exception path, BGetCallback would keep handing back the
	// same message forever -- turning one bad callback into an endless fault
	// loop (a hang instead of a crash).
	bool bDispatched = false;

	try
	{
		UCOLOG_HOT("[UCOnline2] Callback (safe) -> %d\r\n", msg.m_iCallback);
		m_CurrentUser = msg.m_hSteamUser;

		// Same locking/snapshot discipline as DispatchFrame -- see the comment
		// on m_MapLock. Held across Run() so a concurrent unregister+destroy
		// cannot free the callback out from under us.
		CDispatcherLock lock(&m_MapLock);

		if (msg.m_iCallback == SteamAPICallCompleted_t::k_iCallback && msg.m_cubParam == sizeof(SteamAPICallCompleted_t))
		{
			SteamAPICallCompleted_t* pCompleted = (SteamAPICallCompleted_t*)msg.m_pubParam;

			std::vector<std::pair<SteamAPICall_t, CCallbackBase*> > matches;
			for (auto it = m_CallResultMap.begin(); it != m_CallResultMap.end(); ++it)
			{
				if (!it->second)
					continue;

				if (pCompleted->m_hAsyncCall == it->first &&
					pCompleted->m_iCallback == it->second->GetICallback() &&
					pCompleted->m_cubParam == (uint32)it->second->GetCallbackSizeBytes())
				{
					matches.push_back(*it);
				}
			}

			for (size_t i = 0; i < matches.size(); ++i)
				ExecuteCallResult(hPipe, matches[i].first, matches[i].second);
		}
		else
		{
			CCallbackBase* pTarget = nullptr;
			bool bServerCb = false;

			for (auto it = m_CallbackMap.begin(); it != m_CallbackMap.end(); ++it)
			{
				CCallbackBase* pCb = it->second;
				if (!pCb)
					continue;

				if (it->first == msg.m_iCallback && (pCb->m_nCallbackFlags & pCb->k_ECallbackFlagsRegistered))
				{
					if (msg.m_hSteamUser == g_ServerUser && (pCb->m_nCallbackFlags & pCb->k_ECallbackFlagsGameServer) && bServer)
					{
						pTarget = pCb;
						bServerCb = true;
						break;
					}
					else if (msg.m_hSteamUser == g_ClientUser && !(pCb->m_nCallbackFlags & pCb->k_ECallbackFlagsGameServer) && !bServer)
					{
						pTarget = pCb;
						bServerCb = false;
						break;
					}
				}
			}

			if (pTarget)
			{
				if (bServerCb)
				{
					UCOLOG_HOT("[UCOnline2] Server callback (safe) -> %d flags=%d\r\n", msg.m_iCallback, pTarget->m_nCallbackFlags);
					pTarget->Run(msg.m_pubParam);
				}
				else
				{
					UCOLOG_HOT("[UCOnline2] Client callback (safe) -> %d flags=%d\r\n", msg.m_iCallback, pTarget->m_nCallbackFlags);
					RunCallbackPatchers(msg.m_iCallback, msg.m_pubParam, msg.m_cubParam);
					pTarget->Run(msg.m_pubParam);
				}
			}
		}

		bDispatched = true;
	}
	catch (...)
	{
		// Contained: a faulting game callback no longer kills the process.
		UCOColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[UCOnline2] Exception in callback dispatch -- callback skipped\r\n");
	}

	UCOLOG_HOT("[UCOnline2] Freeing callback (safe) -> %d (dispatched=%d)\r\n", msg.m_iCallback, (int)bDispatched);
	SecureZeroMemory(msg.m_pubParam, msg.m_cubParam);
	m_pfnFreeLastCallback(hPipe);
}

void CCallbackDispatcher::Add(CCallbackBase* pCb, int iCallback)
{
	if (!pCb)
	{
		UCOColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[UCOnline2] Add: null callback ptr\r\n");
		return;
	}

	if (pCb->GetCallbackSizeBytes() == 0)
		return;

	UCOLOG("[UCOnline2] Register callback -> %d size=%d flags=%d\r\n", iCallback, pCb->GetCallbackSizeBytes(), pCb->m_nCallbackFlags);

	pCb->m_nCallbackFlags |= pCb->k_ECallbackFlagsRegistered;
	pCb->m_iCallback = iCallback;

	CDispatcherLock lock(&m_MapLock);
	m_CallbackMap.insert(std::make_pair(iCallback, pCb));
}

void CCallbackDispatcher::AddCallResult(CCallbackBase* pCb, SteamAPICall_t hCall)
{
	if (!pCb || hCall == k_uAPICallInvalid)
	{
		UCOColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[UCOnline2] AddCallResult: invalid args\r\n");
		return;
	}

	if (pCb->GetICallback() == 0 || pCb->GetCallbackSizeBytes() == 0)
	{
		UCOColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[UCOnline2] AddCallResult: zero callback\r\n");
		return;
	}

	UCOLOG("[UCOnline2] Register call result -> %lld cb=%d size=%d\r\n", hCall, pCb->GetICallback(), pCb->GetCallbackSizeBytes());

	pCb->m_nCallbackFlags |= pCb->k_ECallbackFlagsRegistered;

	CDispatcherLock lock(&m_MapLock);
	auto existing = m_CallResultMap.find(hCall);
	if (existing == m_CallResultMap.end())
	{
		m_CallResultMap.insert(std::make_pair(hCall, pCb));
	}
	else
	{
		UCOColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[UCOnline2] AddCallResult: already registered\r\n");
		existing->second = pCb;
	}
}

void CCallbackDispatcher::Remove(CCallbackBase* pCb)
{
	if (!pCb)
	{
		UCOColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[UCOnline2] Remove: null callback ptr\r\n");
		return;
	}

	// Deliberately NO early-return on the registered flag. This runs from the
	// game's CCallback destructor, so bailing out would leave a pointer to an
	// object that is about to be freed sitting in the map -- a later dispatch
	// would then call Run()/virtuals on freed memory. The entry must go
	// regardless of the flag's state.
	CDispatcherLock lock(&m_MapLock);

	UCOLOG("[UCOnline2] Unregister callback -> %d flags=%d\r\n", pCb->GetICallback(), pCb->m_nCallbackFlags);
	pCb->m_nCallbackFlags &= ~pCb->k_ECallbackFlagsRegistered;

	// Erase EVERY entry for this callback (it can be registered under more than
	// one key); any straggler is a dangling pointer waiting to crash us.
	for (auto it = m_CallbackMap.begin(); it != m_CallbackMap.end(); )
	{
		if (it->second == pCb)
			it = m_CallbackMap.erase(it);
		else
			++it;
	}

	// It may also still be sitting in the call-result map, e.g. the game frees
	// a CCallResult whose API call never completed.
	for (auto it = m_CallResultMap.begin(); it != m_CallResultMap.end(); )
	{
		if (it->second == pCb)
			it = m_CallResultMap.erase(it);
		else
			++it;
	}
}

void CCallbackDispatcher::RemoveCallResult(CCallbackBase* pCb, SteamAPICall_t hCall)
{
	if (!pCb || hCall == k_uAPICallInvalid)
	{
		UCOColor(FOREGROUND_RED | FOREGROUND_INTENSITY, "[UCOnline2] RemoveCallResult: invalid args\r\n");
		return;
	}

	// As in Remove(): never early-return on the flag, or the map keeps a pointer
	// to an object the game is about to destroy.
	UCOLOG("[UCOnline2] Unregister call result -> %lld cb=%d flags=%d\r\n", hCall, pCb->GetICallback(), pCb->m_nCallbackFlags);
	pCb->m_nCallbackFlags &= ~pCb->k_ECallbackFlagsRegistered;

	CDispatcherLock lock(&m_MapLock);

	// This erase was missing entirely: the call-result entry was never removed,
	// so m_CallResultMap kept the pointer forever and it dangled as soon as the
	// game freed the CCallResult.
	m_CallResultMap.erase(hCall);

	// The same object may be registered under other handles too.
	for (auto it = m_CallResultMap.begin(); it != m_CallResultMap.end(); )
	{
		if (it->second == pCb)
			it = m_CallResultMap.erase(it);
		else
			++it;
	}

	auto bufIt = m_BufferMap.find(hCall);
	if (bufIt != m_BufferMap.end())
	{
		UCOColor(FOREGROUND_BLUE | FOREGROUND_INTENSITY, "[UCOnline2] Freeing call result buffer\r\n");
		delete[] bufIt->second;
		m_BufferMap.erase(bufIt);
	}
}

CCallbackDispatcher* GetDispatcher()
{
	static CCallbackDispatcher instance;
	return &instance;
}

// ============================================================
// CDumpHandler (_DEBUG only)
// ============================================================

#ifdef _DEBUG

#include <DbgHelp.h>

CDumpHandler::CDumpHandler()
{
	m_Comment.clear();
	m_hDbgHelp = nullptr;
	m_pfnWriteDump = nullptr;
	m_bReady = false;

	m_hDbgHelp = LoadLibraryExW(L"DbgHelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!m_hDbgHelp)
		return;

	FARPROC fp = GetProcAddress(m_hDbgHelp, "MiniDumpWriteDump");
	if (!fp)
	{
		FreeLibrary(m_hDbgHelp);
		m_hDbgHelp = nullptr;
		return;
	}

	m_pfnWriteDump = (Fn_MiniDumpWriteDump)fp;

	InitializeSRWLock(&m_Lock);
	m_bReady = true;
}

CDumpHandler::~CDumpHandler()
{
	m_pfnWriteDump = nullptr;
	m_Comment.clear();

	if (m_hDbgHelp)
	{
		FreeLibrary(m_hDbgHelp);
		m_hDbgHelp = nullptr;
	}

	m_bReady = false;
}

bool CDumpHandler::IsReady()
{
	return m_bReady;
}

void CDumpHandler::SetComment(const wchar_t* comment)
{
	m_Comment.assign(comment);
}

size_t CDumpHandler::GetCommentByteSize()
{
	if (m_Comment.empty()) return 0;
	return m_Comment.length() * sizeof(wchar_t);
}

const wchar_t* CDumpHandler::GetComment()
{
	return m_Comment.c_str();
}

void CDumpHandler::ClearComment()
{
	m_Comment.clear();
}

void CDumpHandler::WriteDump(DWORD exceptionCode, _EXCEPTION_POINTERS* pExceptionInfo)
{
	if (!m_hDbgHelp || !m_pfnWriteDump)
		return;

	HANDLE hProcess = GetCurrentProcess();
	DWORD pid = GetCurrentProcessId();

	AcquireSRWLockExclusive(&m_Lock);

	MINIDUMP_EXCEPTION_INFORMATION excInfo = { 0 };
	excInfo.ThreadId = GetCurrentThreadId();
	excInfo.ExceptionPointers = pExceptionInfo;
	excInfo.ClientPointers = FALSE;

	MINIDUMP_TYPE dumpType = (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithHandleData | MiniDumpWithUnloadedModules |
		MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo);

	if (GetCommentByteSize() != 0)
	{
		MINIDUMP_USER_STREAM streams[2] = { 0 };

		streams[0].Type = CommentStreamW;
		streams[0].BufferSize = (ULONG)GetCommentByteSize();
		streams[0].Buffer = (LPVOID)GetComment();

		MINIDUMP_EXCEPTION_STREAM excStream = { 0 };
		excStream.ExceptionRecord.ExceptionCode = exceptionCode;

		streams[1].Type = ExceptionStream;
		streams[1].BufferSize = sizeof(MINIDUMP_EXCEPTION_STREAM);
		streams[1].Buffer = &excStream;

		MINIDUMP_USER_STREAM_INFORMATION streamInfo = { 0 };
		streamInfo.UserStreamCount = 2;
		streamInfo.UserStreamArray = streams;

		m_pfnWriteDump(hProcess, pid, nullptr, dumpType, &excInfo, &streamInfo, nullptr);
	}
	else
	{
		MINIDUMP_USER_STREAM streams[1] = { 0 };

		MINIDUMP_EXCEPTION_STREAM excStream = { 0 };
		excStream.ExceptionRecord.ExceptionCode = exceptionCode;

		streams[0].Type = ExceptionStream;
		streams[0].BufferSize = sizeof(MINIDUMP_EXCEPTION_STREAM);
		streams[0].Buffer = &excStream;

		MINIDUMP_USER_STREAM_INFORMATION streamInfo = { 0 };
		streamInfo.UserStreamCount = 1;
		streamInfo.UserStreamArray = streams;

		m_pfnWriteDump(hProcess, pid, nullptr, dumpType, &excInfo, &streamInfo, nullptr);
	}

	ReleaseSRWLockExclusive(&m_Lock);
}
#endif

/**
 *  SteamStubbed, credits to DenuvoSanctuary for the original code for this.
 *  Originally written in Rust, rewritten here in C++ to integrate it.
 */
#include <intrin.h>
#include "include/MinHook.h"
#include <atomic>

static std::atomic<uint32_t> g_SteamStubCount{ 0 };
static constexpr uint32_t STEAM_STUB_MAX_COUNT = 1;
static constexpr uint8_t STEAM_STUB_SIGNATURE[] = { 0x44, 0x0F, 0xB6, 0xF8, 0x3C, 0x30, 0x0F, 0x84 };

typedef DWORD(WINAPI* GetTickCount_t)(void);
static GetTickCount_t g_OrigGetTickCount = nullptr;

static uint8_t* SteamStub_FindSignature(uint8_t* start, uint8_t* end, const uint8_t* sig, size_t sigLen)
{
	for (uint8_t* p = start; p < end - sigLen; ++p)
	{
		bool match = true;
		for (size_t i = 0; i < sigLen; ++i)
		{
			if (p[i] != sig[i])
			{
				match = false;
				break;
			}
		}
		if (match)
			return p;
	}
	return nullptr;
}

static DWORD WINAPI SteamStub_HookGetTickCount(void)
{
	uint8_t* returnAddr = reinterpret_cast<uint8_t*>(_ReturnAddress());

	uint8_t* start = returnAddr;
	uint8_t* end = start + 128;

	DWORD oldProtect = 0;
	if (!VirtualProtect(start, static_cast<SIZE_T>(end - start), PAGE_EXECUTE_READWRITE, &oldProtect))
	{
		return g_OrigGetTickCount();
	}

	uint8_t* found = SteamStub_FindSignature(start, end, STEAM_STUB_SIGNATURE, sizeof(STEAM_STUB_SIGNATURE));
	if (found)
	{
		found[7] = 0x85;

		uint32_t count = g_SteamStubCount.fetch_add(1, std::memory_order_seq_cst) + 1;
		if (count >= STEAM_STUB_MAX_COUNT)
		{
			MH_DisableHook(reinterpret_cast<LPVOID*>(GetTickCount));
		}
	}

	VirtualProtect(start, static_cast<SIZE_T>(end - start), oldProtect, &oldProtect);

	return g_OrigGetTickCount();
}

// ============================================================
// Generic Steam-side spoof hooks
//
// These are kept in core because they apply uniformly to any
// ogAppId-spoofed setup -- they don't carry per-game logic.
// Game-specific behaviors (auth ticket synthesis, EOS bypass,
// etc.) live in plugins; see include/uco_plugin.h and the
// reference Outbound plugin under plugins/outbound/.
// ============================================================

typedef uint32 (S_CALLTYPE *Fn_GetAppID)(void* pThis);
typedef bool   (S_CALLTYPE *Fn_BIsSubscribedApp)(void* pThis, AppId_t appID);
typedef bool   (S_CALLTYPE *Fn_BIsDlcInstalled)(void* pThis, AppId_t appID);
typedef uint32 (S_CALLTYPE *Fn_GetEarliestPurchase)(void* pThis, AppId_t appID);
typedef int    (S_CALLTYPE *Fn_GetDLCCount)(void* pThis);
typedef bool   (S_CALLTYPE *Fn_BGetDLCDataByIndex)(void* pThis, int iDLC, AppId_t* pAppID,
                                                   bool* pbAvailable, char* pchName, int cchName);

static Fn_GetAppID            g_pfnOriginalGetAppID           = nullptr;
static Fn_BIsSubscribedApp    g_pfnOriginalBIsSubscribedApp   = nullptr;
static Fn_BIsDlcInstalled     g_pfnOriginalBIsDlcInstalled    = nullptr;
static Fn_GetEarliestPurchase g_pfnOriginalGetEarliestPurchase = nullptr;
static Fn_GetDLCCount         g_pfnOriginalGetDLCCount        = nullptr;
static Fn_BGetDLCDataByIndex  g_pfnOriginalBGetDLCDataByIndex = nullptr;
static bool                g_bGetAppIDLoggedFirst        = false;
static bool                g_bSubscribedLoggedFirst      = false;
static bool                g_bDlcLoggedFirst             = false;
static bool                g_bDlcCountLoggedFirst        = false;

// Many games gate multiplayer behind "do you actually own this AppId?"
// via ISteamApps::BIsSubscribedApp(GetAppID()). Real Steam answers
// false because the user owns Spacewar (480), not the real AppId.
// Return true for the ogAppId.
static bool S_CALLTYPE Hooked_BIsSubscribedApp(void* pThis, AppId_t appID)
{
    bool original = g_pfnOriginalBIsSubscribedApp(pThis, appID);
    if (original) return true;

    // Real Steam answers for the SPOOFED AppId, so it says false both for the
    // game itself and for any DLC. Defer to the store for both.
    if (g_OriginalAppId != 0 && appID == g_OriginalAppId)
    {
        if (!g_bSubscribedLoggedFirst)
        {
            UCOLOG("[UCOnline2] BIsSubscribedApp(%u) hook returning true (Steam says false)", appID);
            g_bSubscribedLoggedFirst = true;
        }
        return true;
    }
    return UcoDlcStore::IsOwned((uint32)appID);
}

// The DLC surface below is the reason unlocking used to be unreliable: only
// BIsSubscribedApp was hooked, so a game asking "do I own <id>?" was answered,
// while a game ENUMERATING its DLC through GetDLCCount/BGetDLCDataByIndex got
// real Steam's answer for AppId 480 -- i.e. no DLC at all.
static bool S_CALLTYPE Hooked_BIsDlcInstalled(void* pThis, AppId_t appID)
{
    if (g_pfnOriginalBIsDlcInstalled && g_pfnOriginalBIsDlcInstalled(pThis, appID))
        return true;

    const bool ours = UcoDlcStore::IsInstalled((uint32)appID);
    if (ours && !g_bDlcLoggedFirst)
    {
        UCOLOG("[UCOnline2] BIsDlcInstalled(%u) hook returning true (Steam says false)", appID);
        g_bDlcLoggedFirst = true;
    }
    return ours;
}

// Some games treat a purchase time of 0 as "not owned", so answering the
// ownership question alone isn't enough. Backdated by four days.
static uint32 S_CALLTYPE Hooked_GetEarliestPurchaseUnixTime(void* pThis, AppId_t appID)
{
    uint32 original = g_pfnOriginalGetEarliestPurchase
                    ? g_pfnOriginalGetEarliestPurchase(pThis, appID) : 0;
    if (original) return original;
    if (!UcoDlcStore::IsOwned((uint32)appID)) return 0;
    return (uint32)(time(nullptr) - 4 * 24 * 60 * 60);
}

// Enumeration. UnlockAll can answer any ownership question without knowing the
// ids, but it cannot invent a LIST -- only DLC named in the ini can be reported
// here, which is why [DLC] entries matter for games that build a DLC menu.
static int S_CALLTYPE Hooked_GetDLCCount(void* pThis)
{
    const int original = g_pfnOriginalGetDLCCount ? g_pfnOriginalGetDLCCount(pThis) : 0;
    const int ours = UcoDlcStore::Count();
    if (ours <= 0) return original;

    if (!g_bDlcCountLoggedFirst)
    {
        UCOLOG("[UCOnline2] GetDLCCount hook returning %d configured DLC (Steam reports %d)",
            ours, original);
        g_bDlcCountLoggedFirst = true;
    }
    return ours;
}

static bool S_CALLTYPE Hooked_BGetDLCDataByIndex(void* pThis, int iDLC, AppId_t* pAppID,
                                                 bool* pbAvailable, char* pchName, int cchName)
{
    if (UcoDlcStore::Count() > 0)
        return UcoDlcStore::Get(iDLC, (uint32_t*)pAppID, pbAvailable, pchName, cchName);
    return g_pfnOriginalBGetDLCDataByIndex
         ? g_pfnOriginalBGetDLCDataByIndex(pThis, iDLC, pAppID, pbAvailable, pchName, cchName)
         : false;
}

// Make ISteamUtils::GetAppID() report ogAppId so the rest of the
// game stack agrees on the "real" AppId (matches the way OnlineFix
// exposes RealAppId via the same interface).
static uint32 S_CALLTYPE Hooked_GetAppID(void* pThis)
{
    uint32 original = g_pfnOriginalGetAppID(pThis);
    if (g_OriginalAppId == 0 || g_OriginalAppId == g_ForcedAppId)
        return original;

    if (!g_bGetAppIDLoggedFirst)
    {
        UCOLOG("[UCOnline2] GetAppID hook returning ogAppId=%u (Steam reports %u)",
            g_OriginalAppId, original);
        g_bGetAppIDLoggedFirst = true;
    }
    return g_OriginalAppId;
}


// ============================================================
// Auth session ticket emulation ([Settings] EmulateTicket).
//
// WHY
// Games that gate multiplayer on Steam identity call
// ISteamUser::GetAuthSessionTicket, send the blob to the host, and the host
// calls BeginAuthSession on it. Passed through, real Steam mints that ticket
// under the SPOOFED AppId (480), so it does not describe the game being played
// and the peer rejects it. In a log this shows up as the game registering
// GetAuthSessionTicketResponse_t (callback 163) over and over -- asking for a
// ticket, not getting a usable one, retrying.
//
// This is emulatable precisely BECAUSE the check is peer-side rather than
// Valve-side: both players run the same emulator, so one side mints the ticket
// and the other accepts it. Contrast a publisher backend that asks Steam to
// validate server-side -- that is unforgeable and no local emulation helps.
//
// The blob is shaped so a peer running this same code can sanity-check it: the
// first fields are a magic value, the ogAppId and our SteamID.
// ============================================================
typedef HAuthTicket (S_CALLTYPE *Fn_GetAuthSessionTicket)(void*, void*, int, uint32*, const SteamNetworkingIdentity*);
typedef EBeginAuthSessionResult (S_CALLTYPE *Fn_BeginAuthSession)(void*, const void*, int, CSteamID);
typedef void (S_CALLTYPE *Fn_EndAuthSession)(void*, CSteamID);
typedef void (S_CALLTYPE *Fn_CancelAuthTicket)(void*, HAuthTicket);

static Fn_GetAuthSessionTicket g_pfnOrigGetAuthSessionTicket = nullptr;
static Fn_BeginAuthSession     g_pfnOrigBeginAuthSession     = nullptr;
static Fn_EndAuthSession       g_pfnOrigEndAuthSession       = nullptr;
static Fn_CancelAuthTicket     g_pfnOrigCancelAuthTicket     = nullptr;
static volatile LONG           g_NextAuthTicket              = 0;

static HAuthTicket S_CALLTYPE Hooked_GetAuthSessionTicket(void* pThis, void* pTicket, int cbMaxTicket,
                                                          uint32* pcbTicket, const SteamNetworkingIdentity* pIdentity)
{
    if (!g_bEmulateAuthTicket || !pTicket || cbMaxTicket < 64)
        return g_pfnOrigGetAuthSessionTicket(pThis, pTicket, cbMaxTicket, pcbTicket, pIdentity);

    uint64 steamId = 0;
    if (g_ClientCtx.SteamUser())
        steamId = g_ClientCtx.SteamUser()->GetSteamID().ConvertToUint64();

    const uint32 appId = g_OriginalAppId ? g_OriginalAppId : g_ForcedAppId;
    uint8_t* p = (uint8_t*)pTicket;
    memset(p, 0, (size_t)cbMaxTicket);

    const uint32 magic = 0x554F4B54;                 /* 'UOKT' */
    memcpy(p +  0, &magic,   sizeof(magic));
    memcpy(p +  4, &appId,   sizeof(appId));
    memcpy(p +  8, &steamId, sizeof(steamId));
    const uint32 stamp = (uint32)time(nullptr);
    memcpy(p + 16, &stamp,   sizeof(stamp));

    const uint32 cb = 64;
    if (pcbTicket) *pcbTicket = cb;

    const HAuthTicket handle = (HAuthTicket)InterlockedIncrement(&g_NextAuthTicket);

    // The game waits on this; without it the request looks like it never
    // completed and the game retries forever.
    GetAuthSessionTicketResponse_t resp = {};
    resp.m_hAuthTicket = handle;
    resp.m_eResult     = k_EResultOK;
    if (GetDispatcher())
        GetDispatcher()->PostCallback(GetAuthSessionTicketResponse_t::k_iCallback,
            &resp, sizeof(resp), g_ClientUser, false, 10);

    UCOLOG("[UCOnline2] GetAuthSessionTicket emulated -> handle=%u appid=%u size=%u",
        handle, appId, cb);
    return handle;
}

static EBeginAuthSessionResult S_CALLTYPE Hooked_BeginAuthSession(void* pThis, const void* pAuthTicket,
                                                                  int cbAuthTicket, CSteamID steamID)
{
    if (!g_bEmulateAuthTicket)
        return g_pfnOrigBeginAuthSession(pThis, pAuthTicket, cbAuthTicket, steamID);

    // Accept, then answer the ValidateAuthTicketResponse_t the caller waits on;
    // returning OK alone leaves a host stuck on "authenticating".
    ValidateAuthTicketResponse_t resp = {};
    resp.m_SteamID              = steamID;
    resp.m_eAuthSessionResponse = k_EAuthSessionResponseOK;
    resp.m_OwnerSteamID         = steamID;
    if (GetDispatcher())
        GetDispatcher()->PostCallback(ValidateAuthTicketResponse_t::k_iCallback,
            &resp, sizeof(resp), g_ClientUser, false, 10);

    UCOLOG("[UCOnline2] BeginAuthSession emulated -> OK for %llu (%d byte ticket)",
        (unsigned long long)steamID.ConvertToUint64(), cbAuthTicket);
    return k_EBeginAuthSessionResultOK;
}

static void S_CALLTYPE Hooked_EndAuthSession(void* pThis, CSteamID steamID)
{
    if (!g_bEmulateAuthTicket) { g_pfnOrigEndAuthSession(pThis, steamID); return; }
    UCOLOG("[UCOnline2] EndAuthSession emulated for %llu",
        (unsigned long long)steamID.ConvertToUint64());
}

static void S_CALLTYPE Hooked_CancelAuthTicket(void* pThis, HAuthTicket hAuthTicket)
{
    if (!g_bEmulateAuthTicket) { g_pfnOrigCancelAuthTicket(pThis, hAuthTicket); return; }
    UCOLOG("[UCOnline2] CancelAuthTicket emulated for handle=%u", hAuthTicket);
}

void InstallSteamSpoofHooks()
{
    if (!g_bClientReady)
    {
        UCOLOG("[UCOnline2] Cannot install spoof hooks: client not ready");
        return;
    }

    // Only the AppId spoof needs ogAppId. DLC unlocking and ticket emulation do
    // not, and used to be skipped along with it by a single early return -- so
    // configuring [DLC] without an ogAppId silently did nothing at all.
    const bool bSpoofAppId = (g_OriginalAppId != 0 && g_OriginalAppId != g_ForcedAppId);
    if (!bSpoofAppId)
        UCOLOG("[UCOnline2] No usable ogAppId: AppId spoof skipped "
               "(DLC and ticket hooks are unaffected)");

    MH_Initialize();

    // ISteamUtils vtable: [9] = GetAppID.
    //   0:GetSecondsSinceAppActive  1:GetSecondsSinceComputerActive
    //   2:GetConnectedUniverse  3:GetServerRealTime  4:GetIPCountry
    //   5:GetImageSize  6:GetImageRGBA  7:GetCSERIPPort (private but
    //   present in vtable)  8:GetCurrentBatteryPower  9:GetAppID
    if (bSpoofAppId && g_ClientCtx.SteamUtils())
    {
        void** utilsVT = *reinterpret_cast<void***>(g_ClientCtx.SteamUtils());
        void* pGetAppIDFn = utilsVT[9];
        MH_STATUS s = MH_CreateHook(pGetAppIDFn, &Hooked_GetAppID,
            reinterpret_cast<void**>(&g_pfnOriginalGetAppID));
        if (s == MH_OK)
        {
            if (MH_EnableHook(pGetAppIDFn) == MH_OK)
                UCOLOG("[UCOnline2] GetAppID hook installed (will return %u)", g_OriginalAppId);
            else
                UCOLOG("[UCOnline2] MH_EnableHook failed for GetAppID");
        }
        else
        {
            UCOLOG("[UCOnline2] MH_CreateHook failed for GetAppID: %d", s);
        }
    }

    // ISteamApps vtable, in declaration order from include/sdk/isteamapps.h.
    // Hooking by index is safe here because WE pin the interface version:
    // api_client.h requests STEAMAPPS_INTERFACE_VERSION008 explicitly, so the
    // layout cannot shift under us. If that version string ever changes, these
    // indices must be re-derived from the matching header.
    //
    //   0:BIsSubscribed          1:BIsLowViolence     2:BIsCybercafe
    //   3:BIsVACBanned           4:GetCurrentGameLanguage
    //   5:GetAvailableGameLanguages                   6:BIsSubscribedApp
    //   7:BIsDlcInstalled        8:GetEarliestPurchaseUnixTime
    //   9:BIsSubscribedFromFreeWeekend               10:GetDLCCount
    //  11:BGetDLCDataByIndex    12:InstallDLC        13:UninstallDLC
    if ((bSpoofAppId || UcoDlcStore::Active()) && g_ClientCtx.SteamApps())
    {
        void** appsVT = *reinterpret_cast<void***>(g_ClientCtx.SteamApps());

        struct DlcHook { int index; void* detour; void** original; const char* name; };
        const DlcHook hooks[] = {
            { 6,  &Hooked_BIsSubscribedApp,          (void**)&g_pfnOriginalBIsSubscribedApp,    "BIsSubscribedApp" },
            { 7,  &Hooked_BIsDlcInstalled,           (void**)&g_pfnOriginalBIsDlcInstalled,     "BIsDlcInstalled" },
            { 8,  &Hooked_GetEarliestPurchaseUnixTime,(void**)&g_pfnOriginalGetEarliestPurchase, "GetEarliestPurchaseUnixTime" },
            { 10, &Hooked_GetDLCCount,               (void**)&g_pfnOriginalGetDLCCount,         "GetDLCCount" },
            { 11, &Hooked_BGetDLCDataByIndex,        (void**)&g_pfnOriginalBGetDLCDataByIndex,  "BGetDLCDataByIndex" },
        };

        int installed = 0;
        for (const DlcHook& h : hooks)
        {
            void* target = appsVT[h.index];
            MH_STATUS s = MH_CreateHook(target, h.detour, h.original);
            if (s == MH_OK && MH_EnableHook(target) == MH_OK)
                ++installed;
            else
                UCOLOG("[UCOnline2] failed to hook ISteamApps::%s (vtable[%d]): %d",
                    h.name, h.index, s);
        }

        UCOLOG("[UCOnline2] ISteamApps DLC hooks: %d/%d installed (UnlockAll=%d, %d configured DLC)",
            installed, (int)(sizeof(hooks) / sizeof(hooks[0])),
            UcoDlcStore::UnlockAll() ? 1 : 0, UcoDlcStore::Count());
    }

    // ISteamUser vtable, declaration order from include/sdk/isteamuser.h.
    // Safe to index because api_client.h pins SteamUser023.
    //   13:GetAuthSessionTicket  14:GetAuthTicketForWebApi
    //   15:BeginAuthSession      16:EndAuthSession      17:CancelAuthTicket
    if (g_bEmulateAuthTicket && g_ClientCtx.SteamUser())
    {
        void** userVT = *reinterpret_cast<void***>(g_ClientCtx.SteamUser());

        struct UserHook { int index; void* detour; void** original; const char* name; };
        const UserHook uhooks[] = {
            { 13, &Hooked_GetAuthSessionTicket, (void**)&g_pfnOrigGetAuthSessionTicket, "GetAuthSessionTicket" },
            { 15, &Hooked_BeginAuthSession,     (void**)&g_pfnOrigBeginAuthSession,     "BeginAuthSession" },
            { 16, &Hooked_EndAuthSession,       (void**)&g_pfnOrigEndAuthSession,       "EndAuthSession" },
            { 17, &Hooked_CancelAuthTicket,     (void**)&g_pfnOrigCancelAuthTicket,     "CancelAuthTicket" },
        };

        int ok = 0;
        for (const UserHook& h : uhooks)
        {
            void* target = userVT[h.index];
            MH_STATUS st = MH_CreateHook(target, h.detour, h.original);
            if (st == MH_OK && MH_EnableHook(target) == MH_OK)
                ++ok;
            else
                UCOLOG("[UCOnline2] failed to hook ISteamUser::%s (vtable[%d]): %d",
                    h.name, h.index, st);
        }
        UCOLOG("[UCOnline2] auth ticket emulation: %d/%d hooks installed",
            ok, (int)(sizeof(uhooks) / sizeof(uhooks[0])));
    }
}

static void SteamStub_Init()
{
	if (MH_Initialize() != MH_OK)
		return;

	void* pTarget = reinterpret_cast<void*>(GetTickCount);

	if (MH_CreateHook(pTarget, SteamStub_HookGetTickCount, reinterpret_cast<LPVOID*>(&g_OrigGetTickCount)) != MH_OK)
		return;

	if (MH_EnableHook(pTarget) != MH_OK)
		return;

	UCOLOG("[UCOnline2] SteamStub hook initialized");
}
