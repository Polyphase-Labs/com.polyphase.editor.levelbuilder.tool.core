/**
 * @file LBToolBox.h
 * @brief Box brush — two-click rectangular perimeter placement.
 *
 * First click captures one corner of the rectangle; second click commits.
 * The rectangle lies on the XZ plane at the first click's Y so pieces
 * stay upright. Four edges are walked stride-stepped via
 * `LBToolShared::BuildStrideWalk`; corner pieces are placed exactly once
 * (each edge skips its end corner to avoid the next edge's start).
 *
 * Per the sibling ABI contract, LevelBuilderBrush derivees carry NO data
 * members (vtable layout must match across DLLs). All per-brush state
 * lives in file-scope statics in LBToolBox.cpp.
 */

#pragma once

#include "LevelBuilderInterfaces.h"

struct LevelBuilderCoreAPI;

class LBToolBox : public LevelBuilderBrush
{
public:
    const char* GetName() const override { return "Box"; }

    bool CanPlace(const LevelBuilderPlacementRequest& request) override;
    LevelBuilderPlacementResult Place(const LevelBuilderPlacementRequest& request) override;

    void DrawSettingsUI() override;

    static void Initialize(LevelBuilderCoreAPI* api);
    static void Shutdown(LevelBuilderCoreAPI* api);
};

// Viewport overlay trampoline — draws the start-corner sphere plus a
// wire rectangle from start to the current hover hit when the Box
// brush is the active brush AND a start has been captured.
extern "C" void LBToolBox_DrawViewportOverlayTrampoline(
    float x, float y, float w, float h, void* userData);
