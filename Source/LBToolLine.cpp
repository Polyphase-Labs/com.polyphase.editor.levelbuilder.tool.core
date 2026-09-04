#include "LBToolLine.h"

#include "LBToolShared.h"
#include "LevelBuilderCoreAPI.h"
#include "LevelBuilderCoreLoader.h"

#if EDITOR
#include "imgui.h"
#include "Plugins/PolyphaseEngineAPI.h"
#include "LBToolPicker.h"
#include "LBToolMaskImage.h"
#include "LBToolDistribution.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

    // Multi-target picker — random pick per step. Empty → falls back
    // to the active palette piece (legacy behavior).
    std::vector<std::string> sTargets;
    bool sOpenTargetsPicker = false;
    bool sJustOpened        = false;
    uint32_t sSeed          = 0xDEADu;

    // Optional mask along the line. Mask sampled at (t, 0.5) per step —
    // 1D usage of a 2D mask. Useful for "fence with gaps" pattern.
#if EDITOR
    LBToolMaskImage::MaskState sMask;
#endif
    bool  sUseMask       = false;
    float sMaskThreshold = 0.50f;
    bool  sMaskInvert    = false;
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
    LBToolDistribution::Rng rng = LBToolDistribution::SeedRng(sSeed);
    sSeed = (sSeed * 1103515245u + 12345u) | 1u;
    const bool useMask = sUseMask && LBToolMaskImage::EnsureLoaded(sMask);
    const int  mthresh255 = (int)(std::clamp(sMaskThreshold, 0.0f, 1.0f) * 255.0f);
#endif

    void* lastSpawned = nullptr;
    int   placed      = 0;
    const int count = walk.PieceCount();
    for (int i = 0; i < count; ++i)
    {
        const LBVec3 pos = walk.At(i);

#if EDITOR
        // Mask gate — sample at (t, 0.5) along the line. 1D usage of
        // the 2D mask; lets the user paint a "fence with gaps."
        if (useMask)
        {
            const float t = (count > 1) ? ((float)i / (float)(count - 1)) : 0.5f;
            const LBToolMaskImage::Pixel p = LBToolMaskImage::Sample(sMask, t, 0.5f);
            const int lum = ((int)p.r + (int)p.g + (int)p.b) / 3;
            const bool passes = sMaskInvert ? (lum < mthresh255) : (lum >= mthresh255);
            if (!passes) continue;
        }
        const char* asset = LBToolPicker::PickFromList(sTargets, nullptr, rng);
#else
        const char* asset = nullptr;
#endif
        void* n = spawn(asset, &pos, &request.rotation, userData);
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

    // ---- Targets (random pick per step) ----
    LevelBuilderCoreAPI* api = LevelBuilderCoreLoader::Get();
    std::vector<LBToolPicker::PieceChoice> pieces =
        LBToolPicker::CollectActiveKitPieces(api);
    const std::string projectRoot = LBToolPicker::CachedProjectRoot();
    const std::string kitFolder   = LBToolPicker::GetActiveKitFolder(api);

    ImGui::Spacing();
    ImGui::TextUnformatted("Targets (random pick per step)");
    if (sTargets.empty())
    {
        ImGui::TextDisabled("(empty → uses active palette piece)");
    }
    else
    {
        for (int i = 0; i < (int)sTargets.size(); ++i)
        {
            ImGui::PushID(i);
            const LBToolPicker::PieceChoice* pc =
                LBToolPicker::FindByAsset(pieces, sTargets[i]);
            LBToolPicker::DrawThumbButton(pc, projectRoot, kitFolder,
                                          32.0f, false, "line_tgt", "?");
            if (ImGui::IsItemHovered() && pc)
                ImGui::SetTooltip("%s", pc->display.c_str());
            ImGui::PopID();
            if (i + 1 < (int)sTargets.size()) ImGui::SameLine();
        }
    }
    if (ImGui::Button("Edit targets…##line_edit_targets", ImVec2(150, 0)))
    {
        sOpenTargetsPicker = true;
        sJustOpened        = true;
    }

    // ---- Optional mask ----
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Mask (1D — sample along line)##line_mask"))
    {
        ImGui::Checkbox("Use mask##line_use_mask", &sUseMask);
        if (sUseMask)
        {
            LBToolMaskImage::DrawMaskUI(sMask, "line");
            ImGui::SliderFloat("Threshold##line_mt", &sMaskThreshold, 0.0f, 1.0f, "%.2f");
            ImGui::Checkbox("Invert##line_mi", &sMaskInvert);
            ImGui::TextDisabled("Sampled at (t, 0.5) along the line — useful for fence-with-gaps.");
        }
    }

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

    // Targets modal.
    if (sOpenTargetsPicker)
    {
        ImGui::OpenPopup("Line: Targets##line_tgt_modal");
        sOpenTargetsPicker = false;
    }
    LBToolPicker::DrawPiecePickerModal("Line: Targets##line_tgt_modal",
                                       "Line — multi-select (random per step)",
                                       /*multiSelect=*/true, /*allowAny=*/false,
                                       sJustOpened, &sTargets);
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
