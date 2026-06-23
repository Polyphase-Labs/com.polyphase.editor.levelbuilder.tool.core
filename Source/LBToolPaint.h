/**
 * @file LBToolPaint.h
 * @brief Paint brush — continuous drag stamp + erase.
 *
 * Driven entirely by `TickEditor` (v7 ABI). Holding LMB while the cursor
 * is over the viewport stamps pieces inside a configurable radius at a
 * configurable rate. Holding RMB erases pieces in the same radius via
 * the sibling's `EnumeratePlacementsInRadius` hook.
 *
 * Phase T6. Vtable-safe: no data members on the class. State lives in
 * file-scope statics in LBToolPaint.cpp.
 */

#pragma once

#include "LevelBuilderInterfaces.h"

struct LevelBuilderCoreAPI;

class LBToolPaint : public LevelBuilderBrush
{
public:
    const char* GetName() const override { return "Paint"; }

    bool CanPlace(const LevelBuilderPlacementRequest& request) override;
    LevelBuilderPlacementResult Place(const LevelBuilderPlacementRequest& request) override;

    void DrawSettingsUI() override;
    void TickEditor(float deltaTime) override;

    static void Initialize(LevelBuilderCoreAPI* api);
    static void Shutdown(LevelBuilderCoreAPI* api);
};

extern "C" void LBToolPaint_DrawViewportOverlayTrampoline(
    float x, float y, float w, float h, void* userData);
