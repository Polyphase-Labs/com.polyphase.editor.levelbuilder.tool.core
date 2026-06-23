#include "LevelBuilderCoreLoader.h"

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace
{
    LevelBuilderCoreAPI* sCached = nullptr;
    bool                 sResolveFailed = false;

    // Cheap "is the core DLL still loaded?" check used to invalidate the
    // cached API pointer when the user hot-reloads core out from under us.
    bool CoreModuleStillLoaded()
    {
#ifdef _WIN32
        HMODULE h = GetModuleHandleA("com.polyphase.editor.levelbuilder.core.dll");
        if (!h) h = GetModuleHandleA("com.polyphase.editor.levelbuilder.core");
        return h != nullptr;
#else
        // dlsym(RTLD_DEFAULT, ...) returns null if no loaded .so exports
        // the symbol — same signal as missing core.
        return dlsym(RTLD_DEFAULT, "LevelBuilderCore_GetAPI") != nullptr;
#endif
    }

    LevelBuilderCoreAPI* Resolve()
    {
        if (sCached)
        {
            // Defensive: if core was unloaded while we still held a pointer,
            // drop it before returning so callers don't dereference a stale
            // function-pointer table.
            if (!CoreModuleStillLoaded()) { sCached = nullptr; return nullptr; }
            return sCached;
        }
        if (sResolveFailed) return nullptr;

        LevelBuilderCore_GetAPIFn fn = nullptr;

#ifdef _WIN32
        // The plugin loader's DLL naming convention is
        // <package-id>.dll (per the manifest's `binaryName`).
        HMODULE h = GetModuleHandleA("com.polyphase.editor.levelbuilder.core.dll");
        if (!h)
        {
            // Some loaders strip the suffix or extension. Try a couple of
            // common variants before giving up.
            h = GetModuleHandleA("com.polyphase.editor.levelbuilder.core");
        }
        if (!h)
        {
            sResolveFailed = true;
            return nullptr;
        }
        fn = (LevelBuilderCore_GetAPIFn)GetProcAddress(h, "LevelBuilderCore_GetAPI");
#else
        // RTLD_DEFAULT scans every loaded .so for the symbol.
        fn = (LevelBuilderCore_GetAPIFn)dlsym(RTLD_DEFAULT, "LevelBuilderCore_GetAPI");
#endif
        if (!fn)
        {
            sResolveFailed = true;
            return nullptr;
        }

        LevelBuilderCoreAPI* api = fn();
        if (!api || api->apiVersion != LEVEL_BUILDER_CORE_API_VERSION)
        {
            sResolveFailed = true;
            return nullptr;
        }
        sCached = api;
        return sCached;
    }
}

namespace LevelBuilderCoreLoader
{
    LevelBuilderCoreAPI* Get()      { return Resolve(); }
    void                 Reset()    { sCached = nullptr; sResolveFailed = false; }
}
