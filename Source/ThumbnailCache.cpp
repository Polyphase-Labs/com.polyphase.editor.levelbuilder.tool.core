#include "ThumbnailCache.h"

#if EDITOR

#include "Plugins/EditorUIHooks.h"

namespace
{
    EditorUIHooks* sHooks = nullptr;
}

namespace ThumbnailCache
{
    void Bind(EditorUIHooks* hooks)
    {
        sHooks = hooks;
    }

    ImTextureID Get(const std::string& absPath)
    {
        if (absPath.empty())
            return (ImTextureID)0;

        // Null-check the field as well as the struct: EditorImage_Load is
        // absent on engines older than plugin API v9, where it reads back as
        // nullptr.
        if (sHooks == nullptr || sHooks->EditorImage_Load == nullptr)
            return (ImTextureID)0;

        return (ImTextureID)sHooks->EditorImage_Load(absPath.c_str());
    }

    void Clear()
    {
        // Nothing to release — the engine owns every texture for the editor
        // session. Just forget the hooks pointer so a stray post-unload Get()
        // returns 0 instead of calling through.
        sHooks = nullptr;
    }
}

#endif // EDITOR
