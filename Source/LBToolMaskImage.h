/**
 * @file LBToolMaskImage.h
 * @brief Shared mask-image state + decoder + sampler for the MaskFill
 *        (rectangle-fill) and Paint (drag-stamp) brushes.
 *
 * Provides:
 *  - MaskState — path + decoded RGBA buffer + dimensions
 *  - EnsureLoaded — decodes the PNG once via stb_image; caches; re-loads
 *    only when path changes
 *  - Sample — returns RGBA at normalized [0,1] UV with edge-clamp.
 *    Y is flipped so image "up" maps to world +Z (top-down map auth).
 *  - DrawMaskUI — Path / Browse / Reload / 96×96 thumbnail row
 *
 * Editor-only — the stb_image decoder + ImGui controls don't ship to
 * runtime builds.
 */

#pragma once

#if EDITOR

#include <cstdint>
#include <string>
#include <vector>

struct LevelBuilderCoreAPI;

namespace LBToolMaskImage
{
    struct Pixel { uint8_t r, g, b, a; };

    struct MaskState
    {
        // Path the user pasted / picked. ImGui InputText buffer-style.
        char        pathBuf[512] = {0};
        // The path the cached buffer below was decoded from. Differs
        // from pathBuf only when the user just edited the input — used
        // to know when to re-decode.
        std::string loadedPath;
        int         w = 0;
        int         h = 0;
        std::vector<uint8_t> rgba;            // 4 bytes per pixel
        std::string lastStatus;
    };

    // Decode (or re-decode if path changed). Returns true if the mask
    // is loaded and usable after the call. Empty path → clears + false.
    bool EnsureLoaded(MaskState& s);

    // Force re-decode regardless of cache state. Useful for "Reload" UI.
    void ForceReload(MaskState& s);

    // Sample at normalized [0,1] UV. Returns {0,0,0,0} when unloaded.
    // Y is flipped image-side so image up = world +Z.
    Pixel Sample(const MaskState& s, float u, float v);

    // Whole UI row: text input + Browse (gated on ShowOpenFileDialog
    // availability) + Reload + 96×96 thumbnail preview + dimensions.
    // `idPrefix` namespaces the ImGui ids so two brushes can have one
    // each without collision. Returns true if the path changed.
    bool DrawMaskUI(MaskState& s, const char* idPrefix);
}

#endif // EDITOR
