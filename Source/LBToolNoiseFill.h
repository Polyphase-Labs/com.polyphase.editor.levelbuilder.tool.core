/**
 * @file LBToolNoiseFill.h
 * @brief NoiseFill brush — two-click rectangle, organic scatter.
 *
 * Same click flow as Box / BoxFill. On commit, the brush walks the
 * rectangle in cell-sized steps, samples 2D value noise at each cell,
 * and spawns a piece (via the active sibling's spawn fn) when the
 * noise sample is above `threshold`. Position is jittered within the
 * cell; rotation can be yaw-jittered.
 *
 * Phase T3. Vtable-safe: no data members on the class. State lives in
 * file-scope statics in LBToolNoiseFill.cpp.
 */

#pragma once

#include "LevelBuilderInterfaces.h"

struct LevelBuilderCoreAPI;

class LBToolNoiseFill : public LevelBuilderBrush
{
public:
    const char* GetName() const override { return "NoiseFill"; }

    bool CanPlace(const LevelBuilderPlacementRequest& request) override;
    LevelBuilderPlacementResult Place(const LevelBuilderPlacementRequest& request) override;

    void DrawSettingsUI() override;

    static void Initialize(LevelBuilderCoreAPI* api);
    static void Shutdown(LevelBuilderCoreAPI* api);
};

extern "C" void LBToolNoiseFill_DrawViewportOverlayTrampoline(
    float x, float y, float w, float h, void* userData);
