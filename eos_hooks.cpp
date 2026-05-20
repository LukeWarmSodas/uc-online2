#include "include/eos_hooks.h"
#include "include/MinHook.h"

#include <Windows.h>

extern void UCOLOG(const char* fmt, ...);

// Resolved on first install attempt.
static HMODULE                g_hEosSdk                       = nullptr;
static Fn_EOS_Connect_Login   g_pfnOriginalEosConnectLogin    = nullptr;
static Fn_EOS_Auth_Login      g_pfnOriginalEosAuthLogin       = nullptr;
static bool                   g_bEosHooksInstalled            = false;
static uint64_t               g_FakeProductUserCounter        = 0x1000;
static uint64_t               g_FakeEpicAccountCounter        = 0x2000;

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

// Fake EOS_Auth_Login -- intercept and report success with a synthetic
// EOS_EpicAccountId. Games that gate multiplayer on Epic auth (passing
// the Steam ticket via EOS_LCT_ExternalAuth) hit "Failed(2): Ticket for
// other app" here because Epic's backend validates the ticket against
// the AppId configured for the EOS product.
static void __cdecl Hooked_EOS_Auth_Login(
    EOS_HAuth                       Handle,
    const EOS_Auth_LoginOptions*    Options,
    void*                           ClientData,
    EOS_Auth_OnLoginCallback        CompletionDelegate)
{
    int credType = (Options && Options->Credentials) ? Options->Credentials->Type : -1;
    UCOLOG("[UCOnline2] EOS_Auth_Login intercept: credType=%d clientData=%p",
        credType, ClientData);

    if (!CompletionDelegate)
    {
        UCOLOG("[UCOnline2] EOS_Auth_Login: no completion delegate");
        return;
    }

    EOS_Auth_LoginCallbackInfo info = {};
    info.ResultCode    = EOS_EResult_Success;
    info.ClientData    = ClientData;
    info.LocalUserId   = (EOS_EpicAccountId)(uintptr_t)(++g_FakeEpicAccountCounter);
    CompletionDelegate(&info);
    UCOLOG("[UCOnline2] EOS_Auth_Login: fired Success with fake EAID=0x%llx",
        (unsigned long long)(uintptr_t)info.LocalUserId);
}

// One-time dump of every EOS_* export so we know exactly which symbols
// the loaded EOSSDK exposes. Walks the DLL export directory directly.
static void DumpEosExports(HMODULE hMod)
{
    if (!hMod) return;
    BYTE* base = (BYTE*)hMod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    IMAGE_DATA_DIRECTORY& exp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!exp.VirtualAddress || !exp.Size) return;
    IMAGE_EXPORT_DIRECTORY* dir = (IMAGE_EXPORT_DIRECTORY*)(base + exp.VirtualAddress);
    DWORD* names = (DWORD*)(base + dir->AddressOfNames);

    UCOLOG("[UCOnline2] EOSSDK exports %u named symbols. Listing EOS_*_Login* / EOS_Auth_* / EOS_Connect_* / EOS_Lobby_Create*:",
        dir->NumberOfNames);
    for (DWORD i = 0; i < dir->NumberOfNames; ++i)
    {
        const char* n = (const char*)(base + names[i]);
        if (!n) continue;
        // Filter: only the auth-relevant ones to keep the log skim-able.
        bool keep =
            (strstr(n, "_Login")        != nullptr) ||
            (strstr(n, "EOS_Auth_")     == n)       ||
            (strstr(n, "EOS_Connect_")  == n)       ||
            (strstr(n, "EOS_Lobby_Create") == n)    ||
            (strstr(n, "EOS_Sessions_Create") == n);
        if (keep)
            UCOLOG("[UCOnline2]   export: %s", n);
    }
}

static bool InstallHook(const char* name, void* hook, void** original)
{
    FARPROC target = GetProcAddress(g_hEosSdk, name);
    if (!target)
    {
        UCOLOG("[UCOnline2] EOSSDK does not export %s -- skipping", name);
        return false;
    }
    MH_STATUS s = MH_CreateHook((LPVOID)target, hook, original);
    if (s != MH_OK)
    {
        UCOLOG("[UCOnline2] MH_CreateHook failed for %s: %d", name, s);
        return false;
    }
    s = MH_EnableHook((LPVOID)target);
    if (s != MH_OK)
    {
        UCOLOG("[UCOnline2] MH_EnableHook failed for %s: %d", name, s);
        return false;
    }
    UCOLOG("[UCOnline2] %s hook installed at %p", name, target);
    return true;
}

bool InstallEosHooks()
{
    if (g_bEosHooksInstalled)
        return true;

    g_hEosSdk = GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll");
    if (!g_hEosSdk)
        return false;

    MH_Initialize();

    DumpEosExports(g_hEosSdk);

    bool any = false;
    any |= InstallHook("EOS_Connect_Login",
                       (void*)&Hooked_EOS_Connect_Login,
                       (void**)&g_pfnOriginalEosConnectLogin);
    any |= InstallHook("EOS_Auth_Login",
                       (void*)&Hooked_EOS_Auth_Login,
                       (void**)&g_pfnOriginalEosAuthLogin);

    g_bEosHooksInstalled = any;
    return any;
}
