// ============================================================
// ofm_watcher.dll - dynamic-analysis watcher for OFM's Phasmo fix
//
// Inject this DLL into a Phasmophobia.exe process that's running
// under OFM (OnlineFix) to learn what OFM's hook code (inside the
// VMProtected PhotonBridge.dll) does at runtime. We can't read
// PhotonBridge's code statically, but we can see every API call
// it makes by hooking those APIs ourselves.
//
// Primary signal: VirtualProtect + NtProtectVirtualMemory hooks
//   - Every patch OFM applies has to flip page permissions to
//     writable first. Filtering by "caller is inside
//     PhotonBridge.dll" reveals every memory address OFM writes
//     to. That tells us WHAT gets patched, even if we don't
//     know HOW the patch is generated.
//
// Secondary signal: IL2CPP introspection hooks
//   - il2cpp_class_get_methods, il2cpp_image_get_class etc.
//     If OFM walks the class table to find SteamAuth, we see
//     it iterating.
//
// Log file: C:\Users\Bertie\AppData\Local\Temp\ofm_watcher.log
//
// MinHook is statically linked.
// ============================================================
#include <Windows.h>
#include <winternl.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <intrin.h>

#include "../../include/MinHook.h"

static FILE* g_log = nullptr;
static CRITICAL_SECTION g_logLock;

// Module ranges (set in InitOnce)
static uintptr_t g_gaBase = 0, g_gaEnd = 0;
static uintptr_t g_pbBase = 0, g_pbEnd = 0;
static uintptr_t g_ofBase = 0, g_ofEnd = 0;  // OnlineFix64.dll

#define LOG(...) DoLog(__VA_ARGS__)

static void DoLog(const char* fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

static const char* WhereIs(uintptr_t addr, uintptr_t* off)
{
    if (addr >= g_pbBase && addr < g_pbEnd) { *off = addr - g_pbBase; return "PhotonBridge"; }
    if (addr >= g_ofBase && addr < g_ofEnd) { *off = addr - g_ofBase; return "OnlineFix64"; }
    if (addr >= g_gaBase && addr < g_gaEnd) { *off = addr - g_gaBase; return "GameAssembly"; }
    *off = addr;
    return "?";
}

// True if the return address belongs to a module we care about.
// We filter aggressively to avoid drowning in noise.
static bool InterestingCaller(void* ret)
{
    uintptr_t r = (uintptr_t)ret;
    return (r >= g_pbBase && r < g_pbEnd) ||
           (r >= g_ofBase && r < g_ofEnd);
}

// ============================================================
// VirtualProtect hook -- catches every page-permission change.
// OFM has to call this (or NtProtectVirtualMemory) before each
// inline patch to flip the target page writable.
// ============================================================
typedef BOOL (WINAPI *Fn_VirtualProtect)(LPVOID, SIZE_T, DWORD, PDWORD);
static Fn_VirtualProtect g_origVirtualProtect = nullptr;

static BOOL WINAPI Hooked_VirtualProtect(LPVOID addr, SIZE_T size, DWORD newProtect, PDWORD oldProtect)
{
    void* ret = _ReturnAddress();
    BOOL r = g_origVirtualProtect(addr, size, newProtect, oldProtect);
    if (InterestingCaller(ret))
    {
        uintptr_t retOff, addrOff;
        const char* retMod  = WhereIs((uintptr_t)ret,  &retOff);
        const char* addrMod = WhereIs((uintptr_t)addr, &addrOff);
        LOG("VirtualProtect    addr=%s+0x%zx (%p) size=0x%zx newProt=0x%x   <- %s+0x%zx",
            addrMod, addrOff, addr, size, newProtect, retMod, retOff);
    }
    return r;
}

// ============================================================
// NtProtectVirtualMemory hook -- syscall-level path.
// VMProtect-protected code often bypasses user-mode hooks by
// calling the Nt* syscall directly. Hook this too.
// ============================================================
typedef NTSTATUS (NTAPI *Fn_NtProtectVirtualMemory)(
    HANDLE ProcessHandle, PVOID* BaseAddress, PSIZE_T NumberOfBytesToProtect,
    ULONG NewAccessProtection, PULONG OldAccessProtection);
static Fn_NtProtectVirtualMemory g_origNtProtect = nullptr;

static NTSTATUS NTAPI Hooked_NtProtectVirtualMemory(
    HANDLE ProcessHandle, PVOID* BaseAddress, PSIZE_T NumberOfBytesToProtect,
    ULONG NewAccessProtection, PULONG OldAccessProtection)
{
    void* ret = _ReturnAddress();
    PVOID addrBefore = BaseAddress ? *BaseAddress : nullptr;
    SIZE_T sizeBefore = NumberOfBytesToProtect ? *NumberOfBytesToProtect : 0;
    NTSTATUS s = g_origNtProtect(ProcessHandle, BaseAddress, NumberOfBytesToProtect,
                                  NewAccessProtection, OldAccessProtection);
    if (InterestingCaller(ret))
    {
        uintptr_t retOff, addrOff;
        const char* retMod  = WhereIs((uintptr_t)ret, &retOff);
        const char* addrMod = WhereIs((uintptr_t)addrBefore, &addrOff);
        LOG("NtProtect         addr=%s+0x%zx (%p) size=0x%zx newProt=0x%x status=0x%lx  <- %s+0x%zx",
            addrMod, addrOff, addrBefore, sizeBefore, NewAccessProtection, s, retMod, retOff);
    }
    return s;
}

// ============================================================
// VirtualAlloc hook -- catches OFM's stub-page allocations.
// ============================================================
typedef LPVOID (WINAPI *Fn_VirtualAlloc)(LPVOID, SIZE_T, DWORD, DWORD);
static Fn_VirtualAlloc g_origVirtualAlloc = nullptr;

static LPVOID WINAPI Hooked_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD allocType, DWORD protect)
{
    void* ret = _ReturnAddress();
    LPVOID r = g_origVirtualAlloc(addr, size, allocType, protect);
    if (InterestingCaller(ret))
    {
        uintptr_t retOff;
        const char* retMod = WhereIs((uintptr_t)ret, &retOff);
        LOG("VirtualAlloc      hint=%p size=0x%zx type=0x%x prot=0x%x -> %p  <- %s+0x%zx",
            addr, size, allocType, protect, r, retMod, retOff);
    }
    return r;
}

// ============================================================
// IL2CPP introspection hooks -- reveal what OFM looks up.
// All resolved at runtime from GameAssembly.dll.
// ============================================================
typedef struct Il2CppDomain Il2CppDomain;
typedef struct Il2CppImage Il2CppImage;
typedef struct Il2CppAssembly Il2CppAssembly;
typedef struct Il2CppClass Il2CppClass;
typedef struct MethodInfo MethodInfo;

typedef Il2CppAssembly* (*Fn_il2cpp_domain_assembly_open)(Il2CppDomain*, const char*);
typedef Il2CppClass* (*Fn_il2cpp_class_from_name)(const Il2CppImage*, const char*, const char*);
typedef const MethodInfo* (*Fn_il2cpp_class_get_method_from_name)(Il2CppClass*, const char*, int);
typedef const MethodInfo* (*Fn_il2cpp_class_get_methods)(Il2CppClass*, void**);
typedef const char* (*Fn_il2cpp_class_get_name)(Il2CppClass*);
typedef const char* (*Fn_il2cpp_class_get_namespace)(Il2CppClass*);
typedef const char* (*Fn_il2cpp_method_get_name)(const MethodInfo*);
typedef const Il2CppImage* (*Fn_il2cpp_assembly_get_image)(const Il2CppAssembly*);
typedef const char* (*Fn_il2cpp_image_get_name)(const Il2CppImage*);
typedef Il2CppClass* (*Fn_il2cpp_image_get_class)(const Il2CppImage*, size_t);
typedef size_t (*Fn_il2cpp_image_get_class_count)(const Il2CppImage*);

static Fn_il2cpp_domain_assembly_open       g_orig_domain_assembly_open       = nullptr;
static Fn_il2cpp_class_from_name             g_orig_class_from_name             = nullptr;
static Fn_il2cpp_class_get_method_from_name  g_orig_class_get_method_from_name  = nullptr;
static Fn_il2cpp_class_get_methods           g_orig_class_get_methods           = nullptr;
static Fn_il2cpp_class_get_name              g_orig_class_get_name              = nullptr;
static Fn_il2cpp_class_get_namespace         g_orig_class_get_namespace         = nullptr;
static Fn_il2cpp_method_get_name             g_orig_method_get_name             = nullptr;
static Fn_il2cpp_assembly_get_image          g_orig_assembly_get_image          = nullptr;
static Fn_il2cpp_image_get_name              g_orig_image_get_name              = nullptr;
static Fn_il2cpp_image_get_class             g_orig_image_get_class             = nullptr;
static Fn_il2cpp_image_get_class_count       g_orig_image_get_class_count       = nullptr;

static Il2CppAssembly* Hooked_domain_assembly_open(Il2CppDomain* dom, const char* name)
{
    void* ret = _ReturnAddress();
    Il2CppAssembly* r = g_orig_domain_assembly_open(dom, name);
    uintptr_t retOff;
    const char* retMod = WhereIs((uintptr_t)ret, &retOff);
    LOG("il2cpp_domain_assembly_open(name=%s) = %p  <- %s+0x%zx",
        name ? name : "(null)", r, retMod, retOff);
    return r;
}

static Il2CppClass* Hooked_class_from_name(const Il2CppImage* img, const char* ns, const char* name)
{
    void* ret = _ReturnAddress();
    Il2CppClass* r = g_orig_class_from_name(img, ns, name);
    if (InterestingCaller(ret))
    {
        uintptr_t retOff;
        const char* retMod = WhereIs((uintptr_t)ret, &retOff);
        const char* imgName = (img && g_orig_image_get_name) ? g_orig_image_get_name(img) : "?";
        LOG("class_from_name(img=%s, ns=%s, name=%s) = %p  <- %s+0x%zx",
            imgName, ns ? ns : "", name ? name : "(null)", r, retMod, retOff);
    }
    return r;
}

static const MethodInfo* Hooked_class_get_method_from_name(Il2CppClass* klass, const char* name, int argc)
{
    void* ret = _ReturnAddress();
    const MethodInfo* r = g_orig_class_get_method_from_name(klass, name, argc);
    if (InterestingCaller(ret))
    {
        uintptr_t retOff;
        const char* retMod = WhereIs((uintptr_t)ret, &retOff);
        const char* kname = (klass && g_orig_class_get_name) ? g_orig_class_get_name(klass) : "?";
        LOG("class_get_method_from_name(class=%s, name=%s, argc=%d) = %p  <- %s+0x%zx",
            kname, name ? name : "(null)", argc, r, retMod, retOff);
    }
    return r;
}

static const MethodInfo* Hooked_class_get_methods(Il2CppClass* klass, void** iter)
{
    void* ret = _ReturnAddress();
    const MethodInfo* r = g_orig_class_get_methods(klass, iter);
    if (InterestingCaller(ret) && r && g_orig_method_get_name)
    {
        const char* mname = g_orig_method_get_name(r);
        const char* kname = (klass && g_orig_class_get_name) ? g_orig_class_get_name(klass) : "?";
        uintptr_t retOff;
        const char* retMod = WhereIs((uintptr_t)ret, &retOff);
        LOG("class_get_methods(class=%s) -> %s  <- %s+0x%zx",
            kname, mname ? mname : "?", retMod, retOff);
    }
    return r;
}

static Il2CppClass* Hooked_image_get_class(const Il2CppImage* img, size_t idx)
{
    void* ret = _ReturnAddress();
    Il2CppClass* r = g_orig_image_get_class(img, idx);
    if (InterestingCaller(ret) && r && g_orig_class_get_name)
    {
        // Only log a few-out-of-many to avoid spam (one per 100 idxs + first 5)
        if (idx < 5 || (idx % 100) == 0)
        {
            const char* imgName = (img && g_orig_image_get_name) ? g_orig_image_get_name(img) : "?";
            const char* kname = g_orig_class_get_name(r);
            uintptr_t retOff;
            const char* retMod = WhereIs((uintptr_t)ret, &retOff);
            LOG("image_get_class(img=%s, idx=%zu) = %s  <- %s+0x%zx",
                imgName, idx, kname ? kname : "?", retMod, retOff);
        }
    }
    return r;
}

// ============================================================
// Initialization
// ============================================================
static bool ResolveModule(const char* dllName, uintptr_t* base, uintptr_t* end)
{
    HMODULE h = GetModuleHandleA(dllName);
    if (!h) return false;
    MODULEINFO mi;
    if (!GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi))) return false;
    *base = (uintptr_t)mi.lpBaseOfDll;
    *end  = *base + mi.SizeOfImage;
    return true;
}

static void HookOne(void* target, void* detour, void** trampoline, const char* name)
{
    if (!target) { LOG("[init] %s: target null, skip", name); return; }
    if (MH_CreateHook(target, detour, trampoline) != MH_OK)
    {
        LOG("[init] %s: MH_CreateHook failed (target=%p)", name, target);
        return;
    }
    if (MH_EnableHook(target) != MH_OK)
    {
        LOG("[init] %s: MH_EnableHook failed", name);
        return;
    }
    LOG("[init] %s hooked at %p", name, target);
}

static DWORD WINAPI InitThread(LPVOID)
{
    // Open log
    char logPath[MAX_PATH];
    GetTempPathA(sizeof(logPath), logPath);
    strcat_s(logPath, "ofm_watcher.log");
    g_log = fopen(logPath, "a");
    if (!g_log) return 1;

    InitializeCriticalSection(&g_logLock);
    LOG("======================================================");
    LOG("[init] ofm_watcher attached to PID %lu", GetCurrentProcessId());

    // Wait up to 30s for GameAssembly + PhotonBridge to load
    for (int i = 0; i < 300; ++i)
    {
        if (ResolveModule("GameAssembly.dll",  &g_gaBase, &g_gaEnd) &&
            ResolveModule("PhotonBridge.dll",  &g_pbBase, &g_pbEnd))
        {
            break;
        }
        Sleep(100);
    }
    ResolveModule("OnlineFix64.dll", &g_ofBase, &g_ofEnd);

    LOG("[init] GameAssembly.dll   = %p .. %p", (void*)g_gaBase, (void*)g_gaEnd);
    LOG("[init] PhotonBridge.dll   = %p .. %p", (void*)g_pbBase, (void*)g_pbEnd);
    LOG("[init] OnlineFix64.dll    = %p .. %p", (void*)g_ofBase, (void*)g_ofEnd);

    if (!g_pbBase)
    {
        LOG("[init] PhotonBridge.dll NOT loaded -- is OFM actually injected? Giving up.");
        return 1;
    }

    if (MH_Initialize() != MH_OK)
    {
        LOG("[init] MH_Initialize failed (already initialized by something else?)");
    }

    // Resolve & hook memory-protection APIs
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");

    void* vp = k32   ? GetProcAddress(k32, "VirtualProtect")           : nullptr;
    void* va = k32   ? GetProcAddress(k32, "VirtualAlloc")             : nullptr;
    void* np = ntdll ? GetProcAddress(ntdll, "NtProtectVirtualMemory") : nullptr;

    HookOne(vp, (void*)&Hooked_VirtualProtect, (void**)&g_origVirtualProtect, "VirtualProtect");
    HookOne(va, (void*)&Hooked_VirtualAlloc,   (void**)&g_origVirtualAlloc,   "VirtualAlloc");
    HookOne(np, (void*)&Hooked_NtProtectVirtualMemory, (void**)&g_origNtProtect, "NtProtectVirtualMemory");

    // Resolve & hook IL2CPP introspection exports from GameAssembly.dll
    HMODULE ga = GetModuleHandleA("GameAssembly.dll");
    if (ga)
    {
        #define HOOK_IL2(name) do { \
            void* p = GetProcAddress(ga, "il2cpp_" #name); \
            HookOne(p, (void*)&Hooked_##name, (void**)&g_orig_##name, "il2cpp_" #name); \
        } while (0)

        // Resolve helpers we use for context-string lookups (don't hook these)
        g_orig_class_get_name      = (Fn_il2cpp_class_get_name)     GetProcAddress(ga, "il2cpp_class_get_name");
        g_orig_class_get_namespace = (Fn_il2cpp_class_get_namespace)GetProcAddress(ga, "il2cpp_class_get_namespace");
        g_orig_method_get_name     = (Fn_il2cpp_method_get_name)    GetProcAddress(ga, "il2cpp_method_get_name");
        g_orig_assembly_get_image  = (Fn_il2cpp_assembly_get_image) GetProcAddress(ga, "il2cpp_assembly_get_image");
        g_orig_image_get_name      = (Fn_il2cpp_image_get_name)     GetProcAddress(ga, "il2cpp_image_get_name");
        g_orig_image_get_class_count = (Fn_il2cpp_image_get_class_count)GetProcAddress(ga, "il2cpp_image_get_class_count");

        HOOK_IL2(domain_assembly_open);
        HOOK_IL2(class_from_name);
        HOOK_IL2(class_get_method_from_name);
        HOOK_IL2(class_get_methods);
        HOOK_IL2(image_get_class);

        #undef HOOK_IL2
    }

    LOG("[init] watcher ready");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hMod);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
