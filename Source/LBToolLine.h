/**
 * @file LBToolLine.h
 * @brief Line brush — two-click line placement.
 *
 * First click records the start point (no spawn yet). Second click walks
 * from start to end, calling the active sibling's spawn function once per
 * step. Spacing is configurable via the `Stride` slider in the brush
 * settings UI.
 *
 * Phase T1: ships in com.polyphase.editor.levelbuilder.tool.core. The
 * brush is engine-agnostic — every spawn goes through
 * `LevelBuilderCoreAPI::GetSpawnFnForActiveTool`, so the brush itself
 * never touches engine asset / node headers.
 *
 * Per the sibling ABI contract, LevelBuilderBrush derivees carry NO
 * data members (vtable layout must match across DLLs). All per-brush
 * state lives in file-scope statics in LBToolLine.cpp.
 */

#pragma once

#include "LevelBuilderInterfaces.h"

struct LevelBuilderCoreAPI;

class LBToolLine : public LevelBuilderBrush
{
public:
    const char* GetName() const override { return "Line"; }

    bool CanPlace(const LevelBuilderPlacementRequest& request) override;
    LevelBuilderPlacementResult Place(const LevelBuilderPlacementRequest& request) override;

    void DrawSettingsUI() override;

    static void Initialize(LevelBuilderCoreAPI* api);
    static void Shutdown(LevelBuilderCoreAPI* api);
};

// Viewport overlay trampoline. Tool.core's plugin entry registers this
// with EditorUIHooks::RegisterViewportOverlay; the impl in LBToolLine.cpp
// draws the start-point sphere, a line to the current hover hit, and
// stride markers along that line — but only when the Line brush is the
// active brush and a start point has been set.
extern "C" void LBToolLine_DrawViewportOverlayTrampoline(
    float x, float y, float w, float h, void* userData);
