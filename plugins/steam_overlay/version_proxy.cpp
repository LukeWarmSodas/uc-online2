// ============================================================
// steam_overlay -- force the Steam overlay to attach before graphics startup.
//
// THIS IS NOT A UCONLINE2 PLUGIN. It is a renameable system-DLL proxy that the
// game loads at process start. patch.bat deploys it as version.dll for Unity or
// XINPUT1_3.dll for Unreal. It sits next to the real game EXE, not in plugins\.
//
// WHY IT EXISTS
// The Steam overlay (GameOverlayRenderer64.dll) has to install its DXGI/D3D
// present hook BEFORE the engine creates its swapchain. UCOnline2 loads the
// overlay from steam_api64's DllMain -- but many IL2CPP Unity games don't
// import steam_api64 statically; they P/Invoke it lazily on their first Steam
// call, which is AFTER UnityPlayer has already created the swapchain. By then
// the overlay is too late: it only manages to hook XInput, never the present,
// so Shift+Tab does nothing (confirmed in Steam's own gameoverlay_renderer.txt:
// XInput hooks only, and gameoverlayui64 never spawns).
//
// UnityPlayer.dll statically imports version.dll. Unreal shipping executables
// commonly import XINPUT1_3.dll. Either identity loads this shim before graphics
// initialization; all calls are then forwarded to the matching system DLL.
//
// Build (from plugins\steam_overlay): see steam_overlay.vcxproj ->
// overlay_proxy.dll. The patcher renames it to version.dll for Unity or
// XINPUT1_3.dll for Unreal games.
// ============================================================
#include <Windows.h>
#include <Xinput.h>
#include <stdio.h>
#include <stdarg.h>

static HMODULE g_Module        = nullptr;
static HMODULE g_SystemProxy   = nullptr;
static HMODULE g_Overlay       = nullptr;

static bool IsLogEnabled()
{
    char iniPath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, iniPath, MAX_PATH);
    char* slash = strrchr(iniPath, '\\');
    if (!slash) return false;
    strcpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - iniPath), "union-crax.ini");

    char value[16] = {};
    GetPrivateProfileStringA("Settings", "LogOverlay", "no",
        value, sizeof(value), iniPath);
    return _stricmp(value, "yes") == 0 ||
           _stricmp(value, "true") == 0 ||
           _stricmp(value, "on") == 0 ||
           strcmp(value, "1") == 0;
}

// ---- tiny log, written next to this DLL (i.e. the game folder) ----
static void Log(const char* fmt, ...)
{
    if (!IsLogEnabled()) return;

    char path[MAX_PATH] = {};
    if (g_Module)
    {
        GetModuleFileNameA(g_Module, path, MAX_PATH);
        char* slash = strrchr(path, '\\');
        if (slash) strcpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - path), "steam_overlay.log");
    }
    if (!path[0]) return;

    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") == 0 && f)
    {
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
}

// Read the spoofed AppId from steam_appid.txt next to the exe (default 480).
static void ReadAppId(char* out, DWORD cch)
{
    strcpy_s(out, cch, "480");
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (!slash) return;
    strcpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - exe), "steam_appid.txt");

    FILE* f = nullptr;
    if (fopen_s(&f, exe, "rb") == 0 && f)
    {
        char buf[32] = {};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        // Keep only the leading digits.
        DWORD w = 0;
        for (size_t i = 0; i < n && w + 1 < cch; ++i)
        {
            if (buf[i] < '0' || buf[i] > '9') break;
            out[w++] = buf[i];
        }
        out[w] = '\0';
        if (!out[0]) strcpy_s(out, cch, "480");
    }
}

// The overlay reads SteamGameId to know which app it's overlaying, and won't
// arm without it. Set the identity BEFORE loading the overlay, but never clobber
// a value the launcher already provided.
static void EnsureEnv(const char* name, const char* value)
{
    char cur[64] = {};
    if (GetEnvironmentVariableA(name, cur, sizeof(cur)) == 0)
        SetEnvironmentVariableA(name, value);
}

// Steam install path from the registry (HKCU first, HKLM fallback).
static bool GetSteamPath(char* out, DWORD cch)
{
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &k) == ERROR_SUCCESS)
    {
        DWORD type = 0, sz = cch;
        LONG r = RegQueryValueExA(k, "SteamPath", nullptr, &type, (BYTE*)out, &sz);
        RegCloseKey(k);
        if (r == ERROR_SUCCESS && type == REG_SZ)
        {
            for (char* p = out; *p; ++p) if (*p == '/') *p = '\\';   // Steam stores forward slashes
            return true;
        }
    }
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", 0, KEY_READ, &k) == ERROR_SUCCESS)
    {
        DWORD type = 0, sz = cch;
        LONG r = RegQueryValueExA(k, "InstallPath", nullptr, &type, (BYTE*)out, &sz);
        RegCloseKey(k);
        if (r == ERROR_SUCCESS && type == REG_SZ)
            return true;
    }
    return false;
}

// Read a boolean flag from union-crax.ini next to the exe.
static bool ReadIniBool(const char* section, const char* key, bool defVal)
{
    char iniPath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, iniPath, MAX_PATH);
    char* slash = strrchr(iniPath, '\\');
    if (!slash) return defVal;
    strcpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - iniPath), "union-crax.ini");

    char value[16] = {};
    GetPrivateProfileStringA(section, key, defVal ? "yes" : "no",
        value, sizeof(value), iniPath);
    return _stricmp(value, "yes") == 0 || _stricmp(value, "true") == 0 ||
           _stricmp(value, "on") == 0 || strcmp(value, "1") == 0;
}

// Read a string value from union-crax.ini next to the exe. Returns false (and
// leaves out empty) when the key is missing.
static bool ReadIniString(const char* section, const char* key, char* out, DWORD cch)
{
    out[0] = '\0';
    char iniPath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, iniPath, MAX_PATH);
    char* slash = strrchr(iniPath, '\\');
    if (!slash) return false;
    strcpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - iniPath), "union-crax.ini");
    return GetPrivateProfileStringA(section, key, "", out, cch, iniPath) > 0 && out[0];
}

// Locate the UCOnline2 steam_api64.dll to preload. Checks next to the exe
// (Unreal / generic) first, then the Unity layout <root>\*_Data\Plugins\x86_64.
static bool FindSteamApiDll(char* out, DWORD cch)
{
    char root[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, root, MAX_PATH);
    char* slash = strrchr(root, '\\');
    if (!slash) return false;
    *slash = '\0';   // root = game directory

    _snprintf_s(out, cch, _TRUNCATE, "%s\\steam_api64.dll", root);
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return true;

    char pattern[MAX_PATH] = {};
    _snprintf_s(pattern, MAX_PATH, _TRUNCATE, "%s\\*_Data", root);
    WIN32_FIND_DATAA fd = {};
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            _snprintf_s(out, cch, _TRUNCATE,
                "%s\\%s\\Plugins\\x86_64\\steam_api64.dll", root, fd.cFileName);
            if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) { FindClose(h); return true; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return false;
}

// Runs on its own thread so it isn't holding the loader lock. It still lands
// well before graphics init: version.dll is loaded as a UnityPlayer import,
// long before the exe's entry point, so this fires while the engine is still
// starting up.
static DWORD WINAPI LoaderThread(void*)
{
    // [VersionProxy] LoadDLLsEarly -- preload UCOnline2's steam_api64.dll here,
    // BEFORE the game runs. Unity P/Invokes steam_api64 lazily on its first
    // Steam call, which for an EOS/PlayFab game can land AFTER the game has
    // already created its backend platform -- too late for a plugin (e.g.
    // EOS_custom) to redirect it. version.dll loads before graphics init, so
    // preloading steam_api64 here arms those plugin hooks in time.
    if (ReadIniBool("VersionProxy", "LoadDLLsEarly", false))
    {
        if (GetModuleHandleA("steam_api64.dll"))
        {
            Log("[steam_overlay] LoadDLLsEarly: steam_api64.dll already loaded");
        }
        else
        {
            char dll[MAX_PATH] = {};
            if (FindSteamApiDll(dll, sizeof(dll)))
            {
                HMODULE h = LoadLibraryExA(dll, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                Log("[steam_overlay] LoadDLLsEarly: %s (%s)",
                    h ? "loaded steam_api64.dll EARLY" : "FAILED to load steam_api64.dll", dll);
            }
            else
            {
                Log("[steam_overlay] LoadDLLsEarly: steam_api64.dll not found");
            }
        }
    }

    // The overlay renderer is separate from the preload above. Skip it when the
    // overlay is disabled -- e.g. D3D12 titles where hooking the swapchain can
    // destabilise renderer bring-up.
    if (!ReadIniBool("Settings", "LoadOverlay", true))
    {
        Log("[steam_overlay] LoadOverlay disabled -- overlay not loaded");
        return 0;
    }

    if (g_Overlay || GetModuleHandleA("GameOverlayRenderer64.dll"))
    {
        Log("[steam_overlay] overlay already present -- nothing to do");
        return 0;
    }

    char appId[32] = {};
    ReadAppId(appId, sizeof(appId));   // spoofed id (480) -- the overlay target

    // [VersionProxy] SdrSafe -- for a game using Steam Datagram Relay, the
    // process's Steam context must be the REAL AppId: relay authorisation is
    // keyed off it, and preloading steamclient/overlay as spacewar here binds
    // the process to 480 before UCO2's SDR path runs, so the relay refuses to
    // route. Mirror UCO2's own split (SetAppIDEnv): put ogAppId on the context
    // vars and steer the overlay at the spoofed id via SteamOverlayGameId.
    char ogAppId[32] = {};
    if (ReadIniBool("VersionProxy", "SdrSafe", false) &&
        ReadIniString("Settings", "ogAppId", ogAppId, sizeof(ogAppId)))
    {
        SetEnvironmentVariableA("SteamAppId", ogAppId);
        SetEnvironmentVariableA("SteamGameId", ogAppId);
        EnsureEnv("SteamOverlayGameId", appId);
        Log("[steam_overlay] SdrSafe: Steam context AppId=%s, overlay id=%s", ogAppId, appId);
    }
    else
    {
        EnsureEnv("SteamAppId", appId);
        EnsureEnv("SteamGameId", appId);
    }
    EnsureEnv("SteamClientLaunch", "1");

    char steam[MAX_PATH] = {};
    if (!GetSteamPath(steam, sizeof(steam)))
    {
        Log("[steam_overlay] could not resolve Steam path from the registry");
        return 0;
    }

    char path[MAX_PATH] = {};
    // Preload steamclient64 so the overlay's dependencies resolve.
    if (!GetModuleHandleA("steamclient64.dll"))
    {
        _snprintf_s(path, MAX_PATH, _TRUNCATE, "%s\\steamclient64.dll", steam);
        LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }

    _snprintf_s(path, MAX_PATH, _TRUNCATE, "%s\\GameOverlayRenderer64.dll", steam);
    g_Overlay = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    Log("[steam_overlay] appid=%s  %s  (%s)",
        appId, g_Overlay ? "loaded GameOverlayRenderer64 EARLY" : "FAILED to load overlay", path);
    return 0;
}

// ---- system DLL passthrough ----
//
// Windows resolves the imported function names before DllMain, so the binary
// exports the union of version.dll and XINPUT1_3.dll entry points. At call time
// we load the real system DLL matching the filename the patcher gave us.
static void InitProxy()
{
    if (g_SystemProxy) return;

    char modulePath[MAX_PATH] = {};
    GetModuleFileNameA(g_Module, modulePath, MAX_PATH);
    const char* filename = strrchr(modulePath, '\\');
    filename = filename ? filename + 1 : modulePath;

    const char* systemName = nullptr;
    if (_stricmp(filename, "version.dll") == 0)
        systemName = "version.dll";
    else if (_stricmp(filename, "xinput1_3.dll") == 0)
        systemName = "xinput1_3.dll";
    else
    {
        Log("[steam_overlay] unsupported proxy filename: %s", filename);
        return;
    }

    char systemPath[MAX_PATH] = {};
    GetSystemDirectoryA(systemPath, MAX_PATH);
    strcat_s(systemPath, MAX_PATH, "\\");
    strcat_s(systemPath, MAX_PATH, systemName);
    g_SystemProxy = LoadLibraryA(systemPath);
    if (!g_SystemProxy)
        Log("[steam_overlay] failed to load system proxy target: %s (error %lu)",
            systemPath, GetLastError());
}

template <typename T>
static T Resolve(const char* name)
{
    InitProxy();
    return g_SystemProxy ? reinterpret_cast<T>(GetProcAddress(g_SystemProxy, name)) : nullptr;
}

template <typename T>
static T ResolveOrdinal(WORD ordinal)
{
    InitProxy();
    return g_SystemProxy
        ? reinterpret_cast<T>(GetProcAddress(g_SystemProxy, MAKEINTRESOURCEA(ordinal)))
        : nullptr;
}

extern "C" DWORD WINAPI Proxy_GetFileVersionInfoSizeA(LPCSTR file, LPDWORD handle)
{ using Fn = DWORD (WINAPI*)(LPCSTR, LPDWORD); auto f=Resolve<Fn>("GetFileVersionInfoSizeA"); return f ? f(file,handle) : 0; }
extern "C" DWORD WINAPI Proxy_GetFileVersionInfoSizeW(LPCWSTR file, LPDWORD handle)
{ using Fn = DWORD (WINAPI*)(LPCWSTR, LPDWORD); auto f=Resolve<Fn>("GetFileVersionInfoSizeW"); return f ? f(file,handle) : 0; }
extern "C" DWORD WINAPI Proxy_GetFileVersionInfoSizeExA(DWORD flags, LPCSTR file, LPDWORD handle)
{ using Fn = DWORD (WINAPI*)(DWORD,LPCSTR,LPDWORD); auto f=Resolve<Fn>("GetFileVersionInfoSizeExA"); return f ? f(flags,file,handle) : 0; }
extern "C" DWORD WINAPI Proxy_GetFileVersionInfoSizeExW(DWORD flags, LPCWSTR file, LPDWORD handle)
{ using Fn = DWORD (WINAPI*)(DWORD,LPCWSTR,LPDWORD); auto f=Resolve<Fn>("GetFileVersionInfoSizeExW"); return f ? f(flags,file,handle) : 0; }
extern "C" BOOL WINAPI Proxy_GetFileVersionInfoA(LPCSTR file, DWORD handle, DWORD len, LPVOID data)
{ using Fn = BOOL (WINAPI*)(LPCSTR,DWORD,DWORD,LPVOID); auto f=Resolve<Fn>("GetFileVersionInfoA"); return f ? f(file,handle,len,data) : FALSE; }
extern "C" BOOL WINAPI Proxy_GetFileVersionInfoW(LPCWSTR file, DWORD handle, DWORD len, LPVOID data)
{ using Fn = BOOL (WINAPI*)(LPCWSTR,DWORD,DWORD,LPVOID); auto f=Resolve<Fn>("GetFileVersionInfoW"); return f ? f(file,handle,len,data) : FALSE; }
extern "C" BOOL WINAPI Proxy_GetFileVersionInfoExA(DWORD flags, LPCSTR file, DWORD handle, DWORD len, LPVOID data)
{ using Fn = BOOL (WINAPI*)(DWORD,LPCSTR,DWORD,DWORD,LPVOID); auto f=Resolve<Fn>("GetFileVersionInfoExA"); return f ? f(flags,file,handle,len,data) : FALSE; }
extern "C" BOOL WINAPI Proxy_GetFileVersionInfoExW(DWORD flags, LPCWSTR file, DWORD handle, DWORD len, LPVOID data)
{ using Fn = BOOL (WINAPI*)(DWORD,LPCWSTR,DWORD,DWORD,LPVOID); auto f=Resolve<Fn>("GetFileVersionInfoExW"); return f ? f(flags,file,handle,len,data) : FALSE; }
extern "C" BOOL WINAPI Proxy_GetFileVersionInfoByHandle(HANDLE file, LPVOID data, DWORD len)
{ using Fn = BOOL (WINAPI*)(HANDLE,LPVOID,DWORD); auto f=Resolve<Fn>("GetFileVersionInfoByHandle"); return f ? f(file,data,len) : FALSE; }
extern "C" BOOL WINAPI Proxy_VerQueryValueA(const LPVOID block, LPCSTR sub, LPVOID* value, PUINT len)
{ using Fn = BOOL (WINAPI*)(const LPVOID,LPCSTR,LPVOID*,PUINT); auto f=Resolve<Fn>("VerQueryValueA"); return f ? f(block,sub,value,len) : FALSE; }
extern "C" BOOL WINAPI Proxy_VerQueryValueW(const LPVOID block, LPCWSTR sub, LPVOID* value, PUINT len)
{ using Fn = BOOL (WINAPI*)(const LPVOID,LPCWSTR,LPVOID*,PUINT); auto f=Resolve<Fn>("VerQueryValueW"); return f ? f(block,sub,value,len) : FALSE; }

// ---- XINPUT1_3.dll passthrough ----
extern "C" BOOL WINAPI Proxy_XInputDllMain(HINSTANCE, DWORD, LPVOID)
{ return TRUE; }
extern "C" void WINAPI Proxy_XInputEnable(BOOL enable)
{ using Fn = void (WINAPI*)(BOOL); auto f=Resolve<Fn>("XInputEnable"); if (f) f(enable); }
extern "C" DWORD WINAPI Proxy_XInputGetBatteryInformation(DWORD user, BYTE devType, XINPUT_BATTERY_INFORMATION* info)
{ using Fn = DWORD (WINAPI*)(DWORD,BYTE,XINPUT_BATTERY_INFORMATION*); auto f=Resolve<Fn>("XInputGetBatteryInformation"); return f ? f(user,devType,info) : ERROR_DEVICE_NOT_CONNECTED; }
extern "C" DWORD WINAPI Proxy_XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps)
{ using Fn = DWORD (WINAPI*)(DWORD,DWORD,XINPUT_CAPABILITIES*); auto f=Resolve<Fn>("XInputGetCapabilities"); return f ? f(user,flags,caps) : ERROR_DEVICE_NOT_CONNECTED; }
extern "C" DWORD WINAPI Proxy_XInputGetDSoundAudioDeviceGuids(DWORD user, GUID* render, GUID* capture)
{ using Fn = DWORD (WINAPI*)(DWORD,GUID*,GUID*); auto f=Resolve<Fn>("XInputGetDSoundAudioDeviceGuids"); return f ? f(user,render,capture) : ERROR_DEVICE_NOT_CONNECTED; }
extern "C" DWORD WINAPI Proxy_XInputGetKeystroke(DWORD user, DWORD reserved, PXINPUT_KEYSTROKE key)
{ using Fn = DWORD (WINAPI*)(DWORD,DWORD,PXINPUT_KEYSTROKE); auto f=Resolve<Fn>("XInputGetKeystroke"); return f ? f(user,reserved,key) : ERROR_DEVICE_NOT_CONNECTED; }
extern "C" DWORD WINAPI Proxy_XInputGetState(DWORD user, XINPUT_STATE* state)
{ using Fn = DWORD (WINAPI*)(DWORD,XINPUT_STATE*); auto f=Resolve<Fn>("XInputGetState"); return f ? f(user,state) : ERROR_DEVICE_NOT_CONNECTED; }
extern "C" DWORD WINAPI Proxy_XInputSetState(DWORD user, XINPUT_VIBRATION* vibration)
{ using Fn = DWORD (WINAPI*)(DWORD,XINPUT_VIBRATION*); auto f=Resolve<Fn>("XInputSetState"); return f ? f(user,vibration) : ERROR_DEVICE_NOT_CONNECTED; }

// Undocumented XInput 1.3 ordinal exports used by some games and controller
// libraries. Keep their original ordinals in the module definition file.
extern "C" DWORD WINAPI Proxy_XInputGetStateEx(DWORD user, XINPUT_STATE* state)
{ using Fn = DWORD (WINAPI*)(DWORD,XINPUT_STATE*); auto f=ResolveOrdinal<Fn>(100); return f ? f(user,state) : ERROR_DEVICE_NOT_CONNECTED; }
extern "C" DWORD WINAPI Proxy_XInputWaitForGuideButton(DWORD user, DWORD flags, void* eventInfo)
{ using Fn = DWORD (WINAPI*)(DWORD,DWORD,void*); auto f=ResolveOrdinal<Fn>(101); return f ? f(user,flags,eventInfo) : ERROR_DEVICE_NOT_CONNECTED; }
extern "C" DWORD WINAPI Proxy_XInputCancelGuideButtonWait(DWORD user)
{ using Fn = DWORD (WINAPI*)(DWORD); auto f=ResolveOrdinal<Fn>(102); return f ? f(user) : ERROR_DEVICE_NOT_CONNECTED; }
extern "C" DWORD WINAPI Proxy_XInputPowerOffController(DWORD user)
{ using Fn = DWORD (WINAPI*)(DWORD); auto f=ResolveOrdinal<Fn>(103); return f ? f(user) : ERROR_DEVICE_NOT_CONNECTED; }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_Module = module;
        DisableThreadLibraryCalls(module);
        HANDLE t = CreateThread(nullptr, 0, LoaderThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
