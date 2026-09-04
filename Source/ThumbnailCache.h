/**
 * @file ThumbnailCache.h
 * @brief Thin shim over the engine's shared editor image cache.
 *
 * The engine owns thumbnail decode + GPU upload behind
 * EditorUIHooks::EditorImage_Load (plugin API v9). This file used to carry its
 * own stb_image + Vulkan Image loader, copied from the engine's
 * AddonsWindow.cpp; that never actually linked, because class Image,
 * DestroyQueue, GetDestroyQueue and DeviceWaitIdle carry no POLYPHASE_API
 * annotation and so are absent from Polyphase.lib. The engine now exposes the
 * capability properly, and this is a forwarder.
 *
 * Lifetime: textures are owned by the EDITOR for the whole session. Nothing
 * here holds a GPU resource, so hot-reload is trivially safe — Clear() only
 * drops our pointer to the (engine-owned, permanently valid) hooks struct.
 */

#pragma once

#if EDITOR

#include "imgui.h"

#include <string>

struct EditorUIHooks;

namespace ThumbnailCache
{
    // Called once from the addon's RegisterEditorUI. Safe to call again on
    // hot-reload; the pointer is engine-owned and stable.
    void Bind(EditorUIHooks* hooks);

    // Returns an ImGui-renderable texture handle for the image at `absPath`,
    // or 0 if the file is missing / un-decodable, the engine predates plugin
    // API v9, or the backend isn't Vulkan. Same path returns the same handle
    // across frames; repeat calls are a hash lookup inside the engine, and
    // failures are negatively cached there.
    ImTextureID Get(const std::string& absPath);

    // Drop our reference to the hooks struct. Textures are NOT released — the
    // engine keeps them for the editor session on purpose (see the ownership
    // note in EditorUIHooks.h). Call during addon teardown.
    void Clear();
}

#endif // EDITOR
