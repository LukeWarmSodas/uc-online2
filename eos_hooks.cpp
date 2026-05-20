#include "include/eos_hooks.h"
#include "include/MinHook.h"

#include <Windows.h>

extern void UCOLOG(const char* fmt, ...);

// Resolved on first install attempt.
static HMODULE                g_hEosSdk                       = nullptr;
static Fn_EOS_Connect_Login   g_pfnOriginalEosConnectLogin    = nullptr;
static bool                   g_bEosHooksInstalled            = false;
static uint64_t               g_FakeProductUserCounter        = 0x1000;

// Fires the game's completion delegate with EOS_EResult_Success.
// The LocalUserId is a fake pointer-sized identifier -- EOS treats
// EOS_ProductUserId as an opaque handle so any non-null value is
// accepted by downstream calls that just store it.
static void __cdecl Hooked_EOS_Connect_Login(
    EOS_HConnect                       Handle,
    const EOS_Connect_LoginOptions*    Options,
    void*                              ClientData,
    EOS_Connect_OnLoginCallback        CompletionDelegate)
{
    int credType = (Options && Options->Credentials) ? Options->Credentials->Type : -1;
    UCOLOG("[UCOnline2] EOS_Connect_Login intercept: credType=%d clientData=%p",
        credType, ClientData);

    if (!CompletionDelegate)
    {
        UCOLOG("[UCOnline2] EOS_Connect_Login: no completion delegate -- nothing to fake");
        return;
    }

    EOS_Connect_LoginCallbackInfo info = {};
    info.ResultCode       = EOS_EResult_Success;
    info.ClientData       = ClientData;
    info.LocalUserId      = (EOS_ProductUserId)(uintptr_t)(++g_FakeProductUserCounter);
    info.ContinuanceToken = nullptr;

    CompletionDelegate(&info);
    UCOLOG("[UCOnline2] EOS_Connect_Login: fired Success with fake PUID=0x%llx",
        (unsigned long long)(uintptr_t)info.LocalUserId);
}

bool InstallEosHooks()
{
    if (g_bEosHooksInstalled)
        return true;

    g_hEosSdk = GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll");
    if (!g_hEosSdk)
    {
        // Not loaded yet -- caller can retry later.
        return false;
    }

    FARPROC pConnectLogin = GetProcAddress(g_hEosSdk, "EOS_Connect_Login");
    if (!pConnectLogin)
    {
        UCOLOG("[UCOnline2] EOSSDK loaded but EOS_Connect_Login not exported");
        return false;
    }

    // MH_Initialize may already have been called -- duplicate call is harmless.
    MH_Initialize();

    MH_STATUS s = MH_CreateHook(
        (LPVOID)pConnectLogin,
        (LPVOID)&Hooked_EOS_Connect_Login,
        reinterpret_cast<LPVOID*>(&g_pfnOriginalEosConnectLogin));
    if (s != MH_OK)
    {
        UCOLOG("[UCOnline2] MH_CreateHook failed for EOS_Connect_Login: %d", s);
        return false;
    }

    s = MH_EnableHook((LPVOID)pConnectLogin);
    if (s != MH_OK)
    {
        UCOLOG("[UCOnline2] MH_EnableHook failed for EOS_Connect_Login: %d", s);
        return false;
    }

    UCOLOG("[UCOnline2] EOS_Connect_Login hook installed at %p", pConnectLogin);
    g_bEosHooksInstalled = true;
    return true;
}
