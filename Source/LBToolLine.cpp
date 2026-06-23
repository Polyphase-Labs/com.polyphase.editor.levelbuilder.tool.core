#include "LBToolLine.h"

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
    // Singleton brush instance registered with core. Pure-virtual derived
    // class with NO data members — matches the ABI rule that lets core
    // safely call vtable entries across DLL boundaries.
    LBToolLine sInstance;

    // Per-brush state lives in file-scope statics (same convention modular
    // uses for SnapMode / SnapMaxDist). Resets automatically when the
    // addon hot-reloads.
    bool   sHasStart = false;
    LBVec3 sStart{0,0,0};
    float  sStride = 1.0f;
}

bool LBToolLine::CanPlace(const LevelBuilderPlacementRequest& /*request*/)
{
    // Always accept — first click captures the start, second commits;
    // either way the click is consumed.
    return true;
}

LevelBuilderPlacementResult LBToolLine::Place(const LevelBuilderPlacementRequest& request)
{
    LevelBuilderPlacementResult result{};
    result.success = 0;
    result.spawnedNode = nullptr;
    result.errorMessage = nullptr;

    // -------- First click: stash the start point and wait --------
    if (!sHasStart)
    {
        sStart    = request.position;
        sHasStart = true;
        static const char* kMsg = "Line: start point set; click again to commit";
        result.errorMessage = kMsg;
        return result;
    }

    // -------- Second click: commit the line --------
    LevelBuilderCoreAPI* api = LevelBuilderCoreLoader::Get();
    if (!api)
    {
        static const char* kErr = "Line: core API unavailable";
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
            "Line: no spawn fn registered for the active tool (sibling needs "
            "RegisterSpawnFn) or pre-v4 core";
        result.errorMessage = kErr;
        sHasStart = false;
        return result;
    }

    // Shift-lock to the nearest cardinal axis. Same helper the overlay
    // uses, so previews match commits.
    const LBVec3 end = LBToolShared::MaybeAxisConstrain(sStart, request.position);

    // Pre-compute the stride walk — world-unit stepping so kit pieces
    // whose width equals the stride sit perfectly edge-to-edge. (Naive
    // lerp-by-t distributes pieces evenly across `dist` instead, which
    // silently widens every gap by `leftover / count` — produces the
    // visible mortar-lines-between-walls bug.)
    const LBToolShared::StrideWalk walk =
        LBToolShared::BuildStrideWalk(sStart, end, sStride);

    // v9 undo grouping: every spawn the sibling fires below pushes its
    // own EditorAction. Wrap the whole commit in a single engine group
    // so one Ctrl+Z reverts the whole line. Editor-only — runtime
    // doesn't ship the action-manager surface.
#if EDITOR
    PolyphaseEngineAPI* eng = (PolyphaseEngineAPI*)api->GetEngineAPI();
    if (eng && eng->EditorAction_BeginGroup) eng->EditorAction_BeginGroup("Line");
#endif

    void* lastSpawned = nullptr;
    int   placed      = 0;
    for (int i = 0; i < walk.PieceCount(); ++i)
    {
        const LBVec3 pos = walk.At(i);
        // assetName=null tells the sibling to use the active palette piece.
        void* n = spawn(nullptr, &pos, &request.rotation, userData);
        if (n) { lastSpawned = n; ++placed; }
    }

#if EDITOR
    if (eng && eng->EditorAction_EndGroup) eng->EditorAction_EndGroup();
#endif

    // Reset for the next line, regardless of partial failures — better UX
    // than getting stuck in mid-line if one of the N spawns hits a snag.
    sHasStart = false;

    if (placed > 0)
    {
        result.success     = 1;
        result.spawnedNode = lastSpawned;
        if (api->LogDebug)
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "[LBToolLine] committed line: %d / %d points placed (dist=%.2f, stride=%.3f)",
                          placed, walk.PieceCount(), walk.totalDist, walk.stride);
            api->LogDebug(buf);
        }
    }
    else
    {
        static const char* kErr = "Line: all spawn calls failed";
        result.errorMessage = kErr;
    }
    return result;
}

void LBToolLine::DrawSettingsUI()
{
#if EDITOR
    ImGui::TextUnformatted("Line brush");
    ImGui::Separator();

    ImGui::SliderFloat("Stride", &sStride, 0.1f, 10.0f, "%.2f");

    if (sHasStart)
    {
        ImGui::TextColored(ImVec4(0.20f, 1.00f, 0.30f, 0.85f),
                           "Click again to commit. Start: (%.2f, %.2f, %.2f)",
                           sStart.x, sStart.y, sStart.z);
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel"))
            sHasStart = false;
        ImGui::TextDisabled("Tip: hold Shift to lock the line to the nearest cardinal axis.");
    }
    else
    {
        ImGui::TextDisabled("Click in the viewport to set the start point.");
        ImGui::TextDisabled("Tip: hold Shift before the second click to lock to a cardinal axis.");
    }
#endif
}

void LBToolLine::Initialize(LevelBuilderCoreAPI* api)
{
    if (!api || !api->RegisterBrush) return;
    api->RegisterBrush("Line", &sInstance);
}

void LBToolLine::Shutdown(LevelBuilderCoreAPI* api)
{
    if (!api || !api->UnregisterBrush) return;
    api->UnregisterBrush("Line");
}

// -----------------------------------------------------------------------------
// Viewport overlay — only draws when the Line brush is the active brush
// AND a start point has been captured. Otherwise no-ops so it adds zero
// visual noise.
//
// Draws:
//   - a wire sphere at the captured start point
//   - a line from start → current hover hit
//   - small wire-sphere markers at each `sStride` step along that line
//     (capped so a tiny stride can't spam thousands of gizmos)
//
// The active hover hit is read from core's v2 context via
// Viewport_GetHoverHit — same source the placement preview uses.
// -----------------------------------------------------------------------------

namespace
{
#if EDITOR
    void DrawLinePreview_Impl(float /*vx*/, float /*vy*/, float /*vw*/, float /*vh*/, void* /*ud*/)
    {
        if (!sHasStart) return;

        LevelBuilderCoreAPI* api = LevelBuilderCoreLoader::Get();
        if (!api) return;

        // Only draw when the Line brush is the user's active brush. If
        // the user switched away mid-line, the overlay quietly stops.
        const char* activeBrush = api->GetActiveBrushName ? api->GetActiveBrushName() : "";
        if (!activeBrush || std::strcmp(activeBrush, "Line") != 0) return;

        PolyphaseEngineAPI* eng = (PolyphaseEngineAPI*)api->GetEngineAPI();
        if (!eng) return;
        if (!eng->Gizmos_DrawWireSphere || !eng->Gizmos_DrawLine || !eng->Gizmos_SetColor)
            return;

        // -------- Start-point sphere --------
        eng->Gizmos_SetColor(0.20f, 0.80f, 1.00f, 0.95f);   // cyan
        eng->Gizmos_DrawWireSphere(sStart.x, sStart.y, sStart.z, 0.15f);

        // -------- Hover hit (live end) --------
        if (!api->Viewport_GetHoverHit)
        {
            if (eng->Gizmos_ResetState) eng->Gizmos_ResetState();
            return;
        }
        LBVec3 hit{0,0,0};
        LBVec3 nrm{0,1,0};
        void*  node = nullptr;
        if (!api->Viewport_GetHoverHit(&hit, &nrm, &node))
        {
            if (eng->Gizmos_ResetState) eng->Gizmos_ResetState();
            return;
        }

        // Mirror the commit-time axis lock — preview must show exactly
        // where pieces will land.
        const bool shiftLocked = ImGui::GetIO().KeyShift;
        hit = LBToolShared::MaybeAxisConstrain(sStart, hit);

        // Cyan when free, amber when axis-locked.
        const float lr = shiftLocked ? 1.00f : 0.20f;
        const float lg = shiftLocked ? 0.85f : 0.80f;
        const float lb = shiftLocked ? 0.20f : 1.00f;

        // Line from start to hover.
        eng->Gizmos_SetColor(lr, lg, lb, 0.95f);
        eng->Gizmos_DrawLine(sStart.x, sStart.y, sStart.z, hit.x, hit.y, hit.z);

        // Stride markers along the line — shared helper handles cap +
        // truncation indicator + "last marker bigger" behavior.
        const LBToolShared::StrideWalk walk =
            LBToolShared::BuildStrideWalk(sStart, hit, sStride);
        LBToolShared::DrawStrideMarkers(eng, walk, lr, lg, lb, 0.55f);

        if (eng->Gizmos_ResetState) eng->Gizmos_ResetState();
    }
#endif // EDITOR
}

extern "C" void LBToolLine_DrawViewportOverlayTrampoline(
    float x, float y, float w, float h, void* userData)
{
#if EDITOR
    DrawLinePreview_Impl(x, y, w, h, userData);
#else
    (void)x; (void)y; (void)w; (void)h; (void)userData;
#endif
}
