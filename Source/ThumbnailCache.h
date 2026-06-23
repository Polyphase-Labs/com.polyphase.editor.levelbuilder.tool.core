/**
 * @file ThumbnailCache.h
 * @brief PNG-on-disk → ImGui texture handle cache for the modular addon's
 *        Phase-1 piece browser.
 *
 * Mirrors the pattern in the engine's AddonsWindow.cpp (PNG via stb_image,
 * uploaded into a Vulkan Image, registered with ImGui_ImplVulkan_AddTexture).
 * The engine doesn't yet expose this through the plugin API; until it does,
 * the addon owns its own loader.
 *
 * Lifetime: entries live until Clear() is called. Clear() must run *before*
 * the engine destroys the Vulkan device — i.e. from ModularPlacement::Shutdown,
 * not from a static destructor.
 */

#pragma once

#if EDITOR

#include "imgui.h"

#include <string>

namespace ThumbnailCache
{
    // Returns an ImGui-renderable texture handle for the PNG at `absPath`,
    // or 0 if the file is missing / un-decodable. Same path returns the
    // same handle across frames. A failure is cached so we don't retry the
    // open every frame.
    ImTextureID Get(const std::string& absPath);

    // DeviceWaitIdle, then release every cached descriptor + image. Call
    // exactly once during addon teardown before the engine tears Vulkan
    // down.
    void Clear();
}

#endif // EDITOR
