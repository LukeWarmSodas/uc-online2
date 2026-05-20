#include "il2cpp_runtime.h"

#include <string.h>

// Logger from outbound_plugin.cpp (set during UCO_PluginInit).
extern "C" void IL2CPP_Log(const char* fmt, ...);

// ============================================================
// IL2CPP function pointers we resolve from GameAssembly.dll
// ============================================================
typedef Il2CppDomain*        (*Fn_il2cpp_domain_get)();
typedef const Il2CppAssembly** (*Fn_il2cpp_domain_get_assemblies)(Il2CppDomain*, size_t*);
typedef const Il2CppImage*   (*Fn_il2cpp_assembly_get_image)(const Il2CppAssembly*);
typedef const char*          (*Fn_il2cpp_image_get_name)(const Il2CppImage*);
typedef Il2CppClass*         (*Fn_il2cpp_class_from_name)(const Il2CppImage*, const char*, const char*);
typedef const MethodInfo*    (*Fn_il2cpp_class_get_method_from_name)(Il2CppClass*, const char*, int);
typedef void                 (*Fn_il2cpp_thread_attach)(Il2CppDomain*);

static HMODULE g_hGameAssembly = nullptr;
static bool    g_bReady        = false;

static Fn_il2cpp_domain_get                  g_domain_get                  = nullptr;
static Fn_il2cpp_domain_get_assemblies       g_domain_get_assemblies       = nullptr;
static Fn_il2cpp_assembly_get_image          g_assembly_get_image          = nullptr;
static Fn_il2cpp_image_get_name              g_image_get_name              = nullptr;
static Fn_il2cpp_class_from_name             g_class_from_name             = nullptr;
static Fn_il2cpp_class_get_method_from_name  g_class_get_method_from_name  = nullptr;
static Fn_il2cpp_thread_attach               g_thread_attach               = nullptr;

#define RESOLVE(name) \
    do { \
        g_##name = (Fn_il2cpp_##name)GetProcAddress(g_hGameAssembly, "il2cpp_" #name); \
        if (!g_##name) { \
            IL2CPP_Log("[il2cpp] GameAssembly missing il2cpp_" #name); \
            return false; \
        } \
    } while (0)

bool IL2CPP_TryInit(void)
{
    if (g_bReady) return true;

    g_hGameAssembly = GetModuleHandleA("GameAssembly.dll");
    if (!g_hGameAssembly) return false; // not loaded yet -- caller retries

    RESOLVE(domain_get);
    RESOLVE(domain_get_assemblies);
    RESOLVE(assembly_get_image);
    RESOLVE(image_get_name);
    RESOLVE(class_from_name);
    RESOLVE(class_get_method_from_name);
    RESOLVE(thread_attach);

    // Attach the current native thread to the IL2CPP domain so we
    // can call into managed code safely. Required before any
    // il2cpp_class_* call on most Unity versions.
    Il2CppDomain* dom = g_domain_get();
    if (dom)
        g_thread_attach(dom);

    g_bReady = true;
    IL2CPP_Log("[il2cpp] runtime resolved, domain=%p", dom);
    return true;
}

bool IL2CPP_IsReady(void) { return g_bReady; }

// Walks the loaded assemblies, finds one whose image name matches
// `imageName`, then looks up class by namespace+name.
Il2CppClass* IL2CPP_FindClass(const char* imageName,
                              const char* namespaceName,
                              const char* className)
{
    if (!g_bReady) return nullptr;
    Il2CppDomain* dom = g_domain_get();
    if (!dom) return nullptr;

    size_t count = 0;
    const Il2CppAssembly** asms = g_domain_get_assemblies(dom, &count);
    if (!asms || count == 0) return nullptr;

    for (size_t i = 0; i < count; ++i)
    {
        const Il2CppImage* img = g_assembly_get_image(asms[i]);
        if (!img) continue;
        const char* name = g_image_get_name(img);
        if (!name) continue;

        // Image names usually look like "Assembly-CSharp.dll". Match
        // both with and without the extension.
        bool match = false;
        if (strcmp(name, imageName) == 0)
        {
            match = true;
        }
        else
        {
            size_t inl = strlen(imageName);
            size_t nl = strlen(name);
            if (nl == inl + 4 &&
                strncmp(name, imageName, inl) == 0 &&
                strcmp(name + inl, ".dll") == 0)
            {
                match = true;
            }
        }
        if (!match) continue;

        Il2CppClass* k = g_class_from_name(img, namespaceName, className);
        if (k) return k;
    }
    return nullptr;
}

const MethodInfo* IL2CPP_FindMethod(Il2CppClass* klass,
                                    const char* methodName,
                                    int argCount)
{
    if (!g_bReady || !klass) return nullptr;
    return g_class_get_method_from_name(klass, methodName, argCount);
}

void* IL2CPP_FindMethodPtr(const char* imageName,
                           const char* namespaceName,
                           const char* className,
                           const char* methodName,
                           int argCount)
{
    Il2CppClass* k = IL2CPP_FindClass(imageName, namespaceName, className);
    if (!k)
    {
        IL2CPP_Log("[il2cpp] class not found: %s::%s.%s",
                   imageName, namespaceName ? namespaceName : "", className);
        return nullptr;
    }
    const MethodInfo* m = IL2CPP_FindMethod(k, methodName, argCount);
    if (!m)
    {
        IL2CPP_Log("[il2cpp] method not found: %s::%s.%s.%s(argc=%d)",
                   imageName, namespaceName ? namespaceName : "", className,
                   methodName, argCount);
        return nullptr;
    }
    return m->methodPointer;
}
