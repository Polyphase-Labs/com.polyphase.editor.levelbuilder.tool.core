/**
 * @file LBToolMaskFill.h
 * @brief MaskFill brush — two-click rectangle stamped by a PNG mask.
 *
 * Same click flow as Box / BoxFill / NoiseFill. On commit the brush
 * walks the rectangle in cell-sized steps, samples a greyscale PNG
 * mask at each cell's normalized UV, and spawns a piece when the
 * pixel's luminance passes the configured threshold. Lets artists
 * author level layouts in Photoshop / Krita / any pixel editor and
 * stamp them into the scene — paint a town in B&W, drag a rectangle,
 * commit, get the town.
 *
 * Phase T7. Vtable-safe: no data members on the class. State lives in
 * file-scope statics in LBToolMaskFill.cpp.
 */

#pragma once

#include "LevelBuilderInterfaces.h"

struct LevelBuilderCoreAPI;

class LBToolMaskFill : public LevelBuilderBrush
{
public:
    const char* GetName() const override { return "MaskFill"; }

    bool CanPlace(const LevelBuilderPlacementRequest& request) override;
    LevelBuilderPlacementResult Place(const LevelBuilderPlacementRequest& request) override;

    void DrawSettingsUI() override;

    static void Initialize(LevelBuilderCoreAPI* api);
    static void Shutdown(LevelBuilderCoreAPI* api);
};

// Viewport overlay — preview the rectangle outline between Click 1 and
// Click 2. Same magenta as MaskFill's UI so the user can tell it apart
// from NoiseFill (cyan) and Box (white).
extern "C" void LBToolMaskFill_DrawViewportOverlayTrampoline(
    float x, float y, float w, float h, void* userData);
