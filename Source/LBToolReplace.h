/**
 * @file LBToolReplace.h
 * @brief Replace brush — click a placed piece (or drag a radius)
 *        and swap it with the currently-armed palette piece.
 *
 * v1 ships single-click semantics. Holding a radius wider than the
 * typical piece footprint extends to a bulk-within-disc swap — handy
 * for "swap all my grey prototype walls in this area with the final
 * art" workflows. Default radius (0.1 m) effectively targets one
 * piece per click.
 *
 * Implementation uses the v7 enumerate-fn hook to find candidate
 * placed pieces around the click point. Each target's current world
 * transform is captured (from the engine, not the sibling's placed
 * registry — so gizmo-moved pieces replace at their CURRENT location)
 * BEFORE the visitor returns 1 to consume. After the enumerate call
 * returns, the brush re-spawns the new asset at each captured
 * transform via the active sibling's spawn fn.
 *
 * The whole replace is wrapped in EditorAction_BeginGroup("Replace") /
 * EndGroup so Ctrl+Z reverts every swap as one step.
 *
 * Phase T4. Vtable-safe: no data members on the class. State lives in
 * file-scope statics in LBToolReplace.cpp.
 */

#pragma once

#include "LevelBuilderInterfaces.h"

struct LevelBuilderCoreAPI;

class LBToolReplace : public LevelBuilderBrush
{
public:
    const char* GetName() const override { return "Replace"; }

    bool CanPlace(const LevelBuilderPlacementRequest& request) override;
    LevelBuilderPlacementResult Place(const LevelBuilderPlacementRequest& request) override;

    void DrawSettingsUI() override;

    // Replace operates on already-placed pieces, not on an armed
    // palette item — its source / targets are configured in the
    // brush's own UI. Without this override, core's click dispatcher
    // would refuse to deliver a click until the user armed a palette
    // piece (which would just be visual noise — Replace ignores it).
    bool NeedsArmedPreview() const override { return false; }

    static void Initialize(LevelBuilderCoreAPI* api);
    static void Shutdown(LevelBuilderCoreAPI* api);
};

// Viewport overlay — wire circle at the cursor showing the replace
// radius (cyan to distinguish from Paint's green/red).
extern "C" void LBToolReplace_DrawViewportOverlayTrampoline(
    float x, float y, float w, float h, void* userData);
