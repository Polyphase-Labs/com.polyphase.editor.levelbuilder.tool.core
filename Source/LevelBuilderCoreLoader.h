/**
 * @file LevelBuilderCoreLoader.h
 * @brief Late-bound lookup of LevelBuilderCore_GetAPI from the core addon's
 *        already-loaded DLL.
 *
 * Modular dose NOT link against core. The lookup is intentionally
 * runtime-only so the core addon can be hot-reloaded without yanking the
 * modular addon down with it.
 *
 * Resolution strategy (Windows):
 *   1. GetModuleHandle("com.polyphase.editor.levelbuilder.core.dll")
 *      — the core addon is already loaded by the time modular's OnLoad runs
 *        (modular depends on core).
 *   2. GetProcAddress(hMod, "LevelBuilderCore_GetAPI").
 *
 * POSIX: same idea via dlsym(RTLD_DEFAULT, "LevelBuilderCore_GetAPI") —
 * scans every loaded shared library in the process for the symbol.
 */

#pragma once

#include "LevelBuilderCoreAPI.h"

namespace LevelBuilderCoreLoader
{
    // Resolves and caches the core API pointer. Returns nullptr if the
    // core addon isn't loaded yet, the symbol isn't exported, or the API
    // version doesn't match what this header declares.
    LevelBuilderCoreAPI* Get();

    // Forget the cached pointer. Call from OnUnload so the next reload
    // re-resolves against whatever core's currently in memory.
    void Reset();
}
