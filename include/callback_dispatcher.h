/**
 *  The heavy lifter of this and how multiplayer games work on Steam in general.
 *  This is the class that handles all of the callbacks, and also the call results.
 *  Matchmaking, lobby creation & handling, server list retrieval, all of that is
 *  done through here. Without the dispatcher, none of these things would work.
 *
 *  ~veeλnti<3 2026
 */

#pragma once

#include <map>

typedef bool (S_CALLTYPE* Fn_BGetCallback)(HSteamPipe hPipe, CallbackMsg_t* pMsg);
typedef void (S_CALLTYPE* Fn_FreeLastCallback)(HSteamPipe hPipe);
typedef bool (S_CALLTYPE* Fn_GetAPICallResult)(HSteamPipe hPipe, SteamAPICall_t hCall, void* pBuf, int cubBuf, int iExpected, bool* pbFailed);

// Scoped lock for CCallbackDispatcher::m_MapLock.
class CDispatcherLock
{
public:
	explicit CDispatcherLock(CRITICAL_SECTION* pcs) : m_pcs(pcs) { if (m_pcs) EnterCriticalSection(m_pcs); }
	~CDispatcherLock() { if (m_pcs) LeaveCriticalSection(m_pcs); }
private:
	CRITICAL_SECTION* m_pcs;
	CDispatcherLock(const CDispatcherLock&);
	CDispatcherLock& operator=(const CDispatcherLock&);
};

class CCallbackDispatcher
{
public:
	Fn_BGetCallback m_pfnBGetCallback;
	Fn_FreeLastCallback m_pfnFreeLastCallback;
	Fn_GetAPICallResult m_pfnGetAPICallResult;
	HSteamUser m_CurrentUser;
	int m_ManualCbId;
	int m_ManualCbSize;
	bool m_bProcessing;
	std::multimap<int, CCallbackBase*> m_CallbackMap;
	std::map<SteamAPICall_t, CCallbackBase*> m_CallResultMap;
	std::map<SteamAPICall_t, BYTE*> m_BufferMap;

	// Guards the three maps above.
	//
	// SteamAPI_RunCallbacks serializes DISPATCH against itself (via
	// g_CallbackLock), but SteamAPI_RegisterCallback / UnregisterCallback /
	// RegisterCallResult / UnregisterCallResult take NO lock. So a game thread
	// can mutate these maps while another thread is walking them mid-dispatch --
	// UE does exactly this, driving SteamAPI_RunCallbacks from
	// FOnlineAsyncTaskManagerSteam::OnlineTick() while the game thread registers
	// callbacks. The stale iterator then yields a dangling CCallbackBase*, and
	// the virtual call through it faults (seen as EXCEPTION_ACCESS_VIOLATION at
	// 0x8 / 0xffffffffffffffff in _guard_dispatch_icall under Forever Skies).
	//
	// Re-entrant by design: callbacks legitimately register/unregister others
	// from inside Run(), so this must be a recursive CRITICAL_SECTION.
	CRITICAL_SECTION m_MapLock;

	CCallbackDispatcher();
	~CCallbackDispatcher();

	void Shutdown();
	void DispatchFrame(HSteamPipe hPipe, bool bServer);
	void DispatchFrameSafe(HSteamPipe hPipe, bool bServer);
	void ExecuteCallResult(HSteamPipe hPipe, SteamAPICall_t hCall, CCallbackBase* pCb);
	void Add(CCallbackBase* pCb, int iCallback);
	void AddCallResult(CCallbackBase* pCb, SteamAPICall_t hCall);
	void Remove(CCallbackBase* pCb);
	void RemoveCallResult(CCallbackBase* pCb, SteamAPICall_t hCall);
};

CCallbackDispatcher* GetDispatcher();
