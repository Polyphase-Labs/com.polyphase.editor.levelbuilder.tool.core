/**
 * @file LBToolBoxFill.h
 * @brief BoxFill brush — two-click rectangular grid-fill placement.
 *
 * First click captures one corner; second click commits. Every cell in
 * the stride-stepped grid that lies inside the AABB gets a piece. The
 * rectangle lies on the XZ plane at the first click's Y so pieces stay
 * upright. Cap on total cells (`kMaxFillCells`) prevents a tiny stride
 * over a huge area from blowing up.
 */

#pragma once

#include "LevelBuilderInterfaces.h"

struct LevelBuilderCoreAPI;

class LBToolBoxFill : public LevelBuilderBrush
{
public:
    const char* GetName() const override { return "BoxFill"; }

    bool CanPlace(const LevelBuilderPlacementRequest& request) override;
    LevelBuilderPlacementResult Place(const LevelBuilderPlacementRequest& request) override;

    void DrawSettingsUI() override;

    static void Initialize(LevelBuilderCoreAPI* api);
    static void Shutdown(LevelBuilderCoreAPI* api);
};

// Viewport overlay trampoline — draws the start-corner sphere, a wire
// rectangle from start to the current hover hit, plus grid-cell markers
// (capped) so the user can see roughly where pieces will land.
extern "C" void LBToolBoxFill_DrawViewportOverlayTrampoline(
    float x, float y, float w, float h, void* userData);
