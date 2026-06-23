#include "LBToolBoxFill.h"

#include "LBToolShared.h"
#include "LevelBuilderCoreAPI.h"
#include "LevelBuilderCoreLoader.h"

#if EDITOR
#include "imgui.h"
#include "Plugins/PolyphaseEngineAPI.h"
#endif

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    LBToolBoxFill sInstance;

    bool   sHasStart = false;
    LBVec3 sStart{0, 0, 0};
    float  sStride = 1.0f;

    // Hard cap on total cells per commit. Mirrors Line/Box's safety net
    // for the case where the user picks a 0.05 stride and clicks across
    // a 50 m area — without this we'd try to spawn ~1M nodes.
    constexpr int kMaxFillCells = 4096;

    // Preview grid markers cap so a small stride doesn't drown the viewport.
    constexpr int kMaxPreviewCells = 256;

    struct FillExtents
    {
        float minX, maxX, minZ, maxZ;
        float y;
        int   nx;        // cell count along X (>= 1 if width >= stride)
        int   nz;        // cell count along Z
        float stride;
    };

    FillExtents BuildExtents(const LBVec3& a, const LBVec3& b, float stride)
    {
        FillExtents e{};
        e.minX = a.x < b.x ? a.x : b.x;
        e.maxX = a.x > b.x ? a.x : b.x;
        e.minZ = a.z < b.z ? a.z : b.z;
        e.maxZ = a.z > b.z ? a.z : b.z;
        e.y    = a.y;
        e.stride = stride < 1e-4f ? 1e-4f : stride;

        const float w = e.maxX - e.minX;
        const float h = e.maxZ - e.minZ;
        e.nx = (int)(w / e.stride) + 1;   // +1 so a 1-stride-wide rect still emits 2 cells
        e.nz = (int)(h / e.stride) + 1;
        if (e.nx < 1) e.nx = 1;
        if (e.nz < 1) e.nz = 1;
        return e;
    }
}

bool LBToolBoxFill::CanPlace(const LevelBuilderPlacementRequest& /*request*/) { return true; }

LevelBuilderPlacementResult LBToolBoxFill::Place(const LevelBuilderPlacementRequest& request)
{
    LevelBuilderPlacementResult result{};
    result.success = 0;

    if (!sHasStart)
    {
        sStart    = request.position;
        sHasStart = true;
        static const char* kMsg = "BoxFill: first corner set; click again to commit";
        result.errorMessage = kMsg;
        return result;
    }

    LevelBuilderCoreAPI* api = LevelBuilderCoreLoader::Get();
    if (!api)
    {
        static const char* kErr = "BoxFill: core API unavailable";
        result.errorMessage = kErr;
        sHasStart = false;
        return result;
    }

    void* userData = nullptr;
    LevelBuilderCoreAPI::LBBrushSpawnFn spawn =
        LBToolShared::ResolveSpawn(api, &userData);
    if (!spawn)
    {
        static const char* kErr =
            "BoxFill: no spawn fn for active tool (sibling needs RegisterSpawnFn) "
            "or pre-v4 core";
        result.errorMessage = kErr;
        sHasStart = false;
        return result;
    }

    // Shift-axis-lock degenerates the fill to a strip. Consistent with Line/Box.
    const LBVec3 endRaw = LBToolShared::MaybeAxisConstrain(sStart, request.position);

    FillExtents e = BuildExtents(sStart, endRaw, sStride);

    // Cap by clamping nx*nz to kMaxFillCells. Shrink the row count first
    // (preferring full rows over partial ones) so the cap behavior is
    // predictable.
    bool capped = false;
    if ((long long)e.nx * (long long)e.nz > kMaxFillCells)
    {
        capped = true;
        // Solve nz <= maxCells / nx; clamp nz keeping nx as-is.
        e.nz = kMaxFillCells / (e.nx > 0 ? e.nx : 1);
        if (e.nz < 1) e.nz = 1;
    }

    void* lastSpawned = nullptr;
    int   placed = 0;
    int   totalPoints = 0;

    // v9 undo grouping. Editor-only.
#if EDITOR
    PolyphaseEngineAPI* engApi = (PolyphaseEngineAPI*)api->GetEngineAPI();
    if (engApi && engApi->EditorAction_BeginGroup) engApi->EditorAction_BeginGroup("BoxFill");
#endif

    for (int iz = 0; iz < e.nz; ++iz)
    {
        const float z = e.minZ + iz * e.stride;
        for (int ix = 0; ix < e.nx; ++ix)
        {
            const float x = e.minX + ix * e.stride;
            const LBVec3 p{x, e.y, z};
            ++totalPoints;
            void* n = spawn(nullptr, &p, &request.rotation, userData);
            if (n) { lastSpawned = n; ++placed; }
        }
    }

#if EDITOR
    if (engApi && engApi->EditorAction_EndGroup) engApi->EditorAction_EndGroup();
#endif

    sHasStart = false;

    if (placed > 0)
    {
        result.success     = 1;
        result.spawnedNode = lastSpawned;
        if (api->LogDebug)
        {
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "[LBToolBoxFill] committed fill: %d / %d cells (%dx%d, stride=%.3f%s)",
                          placed, totalPoints, e.nx, e.nz, e.stride,
                          capped ? ", CAPPED" : "");
            api->LogDebug(buf);
        }
    }
    else
    {
        static const char* kErr = "BoxFill: all spawn calls failed";
        result.errorMessage = kErr;
    }
    return result;
}

void LBToolBoxFill::DrawSettingsUI()
{
#if EDITOR
    ImGui::TextUnformatted("BoxFill brush (grid fill)");
    ImGui::Separator();

    ImGui::SliderFloat("Stride", &sStride, 0.1f, 10.0f, "%.2f");

    if (sHasStart)
    {
        ImGui::TextColored(ImVec4(0.20f, 1.00f, 0.30f, 0.85f),
                           "Click again to commit. Corner 1: (%.2f, %.2f, %.2f)",
                           sStart.x, sStart.y, sStart.z);
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel"))
            sHasStart = false;
        ImGui::TextDisabled("Tip: Shift collapses the fill to a single axis strip.");
    }
    else
    {
        ImGui::TextDisabled("Click in the viewport to set the first corner.");
        ImGui::TextDisabled("Every cell inside the rectangle gets a piece (capped at %d).", kMaxFillCells);
    }
#endif
}

void LBToolBoxFill::Initialize(LevelBuilderCoreAPI* api)
{
    if (!api || !api->RegisterBrush) return;
    api->RegisterBrush("BoxFill", &sInstance);
}

void LBToolBoxFill::Shutdown(LevelBuilderCoreAPI* api)
{
    if (!api || !api->UnregisterBrush) return;
    api->UnregisterBrush("BoxFill");
}

// -----------------------------------------------------------------------------
// Viewport overlay — start-corner sphere + wire rectangle + grid-cell
// markers (capped) so the user sees roughly where pieces will land.
// -----------------------------------------------------------------------------

namespace
{
#if EDITOR
    void DrawBoxFillPreview_Impl(float, float, float, float, void*)
    {
        if (!sHasStart) return;

        LevelBuilderCoreAPI* api = LevelBuilderCoreLoader::Get();
        if (!api) return;

        const char* activeBrush = api->GetActiveBrushName ? api->GetActiveBrushName() : "";
        if (!activeBrush || std::strcmp(activeBrush, "BoxFill") != 0) return;

        PolyphaseEngineAPI* eng = (PolyphaseEngineAPI*)api->GetEngineAPI();
        if (!eng) return;
        if (!eng->Gizmos_DrawWireSphere || !eng->Gizmos_DrawLine || !eng->Gizmos_SetColor)
            return;

        if (!api->Viewport_GetHoverHit) return;
        LBVec3 hit{0,0,0}, nrm{0,1,0};
        void*  node = nullptr;
        if (!api->Viewport_GetHoverHit(&hit, &nrm, &node))
        {
            eng->Gizmos_SetColor(0.20f, 0.80f, 1.00f, 0.95f);
            eng->Gizmos_DrawWireSphere(sStart.x, sStart.y, sStart.z, 0.15f);
            if (eng->Gizmos_ResetState) eng->Gizmos_ResetState();
            return;
        }

        const bool shiftLocked = ImGui::GetIO().KeyShift;
        hit = LBToolShared::MaybeAxisConstrain(sStart, hit);

        FillExtents e = BuildExtents(sStart, hit, sStride);

        // Edge color: cyan; amber when axis-locked (matches Line/Box).
        const float lr = shiftLocked ? 1.00f : 0.20f;
        const float lg = shiftLocked ? 0.85f : 0.80f;
        const float lb = shiftLocked ? 0.20f : 1.00f;

        // Start-corner sphere.
        eng->Gizmos_SetColor(lr, lg, lb, 0.95f);
        eng->Gizmos_DrawWireSphere(sStart.x, sStart.y, sStart.z, 0.15f);

        // Outline rectangle (4 lines).
        const LBVec3 c0{e.minX, e.y, e.minZ};
        const LBVec3 c1{e.maxX, e.y, e.minZ};
        const LBVec3 c2{e.maxX, e.y, e.maxZ};
        const LBVec3 c3{e.minX, e.y, e.maxZ};
        eng->Gizmos_DrawLine(c0.x, c0.y, c0.z, c1.x, c1.y, c1.z);
        eng->Gizmos_DrawLine(c1.x, c1.y, c1.z, c2.x, c2.y, c2.z);
        eng->Gizmos_DrawLine(c2.x, c2.y, c2.z, c3.x, c3.y, c3.z);
        eng->Gizmos_DrawLine(c3.x, c3.y, c3.z, c0.x, c0.y, c0.z);

        // Cell markers — visit row-major, cap at kMaxPreviewCells, mark
        // the truncation with a brighter amber sphere at the last drawn
        // cell so the user knows there will be more pieces than shown.
        const int totalCells = e.nx * e.nz;
        bool truncated = totalCells > kMaxPreviewCells;
        const int drawCells = truncated ? kMaxPreviewCells : totalCells;

        eng->Gizmos_SetColor(lr, lg, lb, 0.55f);
        int drawn = 0;
        LBVec3 lastDrawn{0,0,0};
        for (int iz = 0; iz < e.nz && drawn < drawCells; ++iz)
        {
            for (int ix = 0; ix < e.nx && drawn < drawCells; ++ix)
            {
                const float x = e.minX + ix * e.stride;
                const float z = e.minZ + iz * e.stride;
                eng->Gizmos_DrawWireSphere(x, e.y, z, 0.06f);
                lastDrawn = LBVec3{x, e.y, z};
                ++drawn;
            }
        }
        if (truncated && drawn > 0)
        {
            eng->Gizmos_SetColor(1.0f, 0.65f, 0.10f, 0.95f);
            eng->Gizmos_DrawWireSphere(lastDrawn.x, lastDrawn.y, lastDrawn.z, 0.13f);
        }

        if (eng->Gizmos_ResetState) eng->Gizmos_ResetState();
    }
#endif
}

extern "C" void LBToolBoxFill_DrawViewportOverlayTrampoline(
    float x, float y, float w, float h, void* ud)
{
#if EDITOR
    DrawBoxFillPreview_Impl(x, y, w, h, ud);
#else
    (void)x; (void)y; (void)w; (void)h; (void)ud;
#endif
}
