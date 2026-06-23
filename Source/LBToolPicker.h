/**
 * @file LBToolPicker.h
 * @brief Shared piece-picker UI for tool.core brushes (Replace, MaskFill, …).
 *
 * Provides the modal grid-of-thumbnails picker that lets the user select
 * one piece (single-select) or many (multi-select) from the active kit.
 * Originally inlined in LBToolReplace; extracted so MaskFill (and future
 * brushes like Paint-with-target-list) can share it.
 *
 * Editor-only — entire surface gated #if EDITOR.
 */

#pragma once

#if EDITOR

#include "LevelBuilderCoreAPI.h"

#include "imgui.h"

#include <string>
#include <vector>

namespace LBToolPicker
{
    struct PieceChoice
    {
        std::string display;
        std::string asset;
        std::string category;
        std::string iconPath;
    };

    // Snapshot the active kit's pieces. Called per-frame from each
    // brush's settings UI; cheap unless the kit is huge.
    std::vector<PieceChoice> CollectActiveKitPieces(LevelBuilderCoreAPI* api);

    const PieceChoice* FindByAsset(const std::vector<PieceChoice>& pieces,
                                   const std::string& asset);

    // Project root + kit folder caching. Used by the icon resolver.
    const std::string& CachedProjectRoot();
    std::string        GetActiveKitFolder(LevelBuilderCoreAPI* api);

    // Resolve a piece's `iconPath` to an absolute file path. Tries kit-
    // folder-relative first (canonical for folder-mode kits), then
    // project-root-relative (legacy + loose-format kits), then absolute.
    std::string ResolveIconAbs(const std::string& iconPath,
                               const std::string& projectRoot,
                               const std::string& kitFolder);

    // Thumbnail texture for a piece via tool.core's ThumbnailCache.
    // Returns 0 if the iconPath is empty / un-decodable.
    ImTextureID FetchThumbnail(const PieceChoice& pc,
                               const std::string& projectRoot,
                               const std::string& kitFolder);

    // Square thumbnail button. Returns true if clicked. Selected state
    // is rendered with a bold cyan border + tint that's visible across
    // both image and text-fallback paths.
    bool DrawThumbButton(const PieceChoice* pc,
                         const std::string& projectRoot,
                         const std::string& kitFolder,
                         float size,
                         bool selected,
                         const char* idStr,
                         const char* placeholderLabel = "?");

    // Piece-picker modal. multiSelect=false → click a tile, modal
    // closes, `*out` becomes a one-element vector with the picked asset
    // (or empty if the special "<any>" entry was chosen + allowAny=true).
    // multiSelect=true → toggle tiles, Confirm/Cancel to commit/discard.
    //
    // `seedFromOut` is set true the frame BEFORE you call this (latch
    // pattern) so the modal knows to snapshot the current `*out` into
    // its internal working set. Consumed by the modal.
    bool DrawPiecePickerModal(const char* modalId,
                              const char* headerLabel,
                              bool multiSelect,
                              bool allowAny,
                              bool& seedFromOut,
                              std::vector<std::string>* out);
}

// Cheap random pick from a target list. Returns fallback when list is
// empty; deterministic per-call by passing the brush's RNG through.
// Lives outside the namespace because LBToolDistribution + std::vector
// don't need to be pulled into LBToolPicker.h.
#include "LBToolDistribution.h"
#include <vector>
#include <string>

namespace LBToolPicker
{
    inline const char* PickFromList(const std::vector<std::string>& list,
                                    const char* fallback,
                                    LBToolDistribution::Rng& rng)
    {
        if (list.empty()) return fallback;
        if (list.size() == 1) return list[0].c_str();
        const float u = LBToolDistribution::NextFloat01(rng);
        int pick = (int)(u * (float)list.size());
        if (pick < 0) pick = 0;
        if (pick >= (int)list.size()) pick = (int)list.size() - 1;
        return list[pick].c_str();
    }
}

#endif // EDITOR
