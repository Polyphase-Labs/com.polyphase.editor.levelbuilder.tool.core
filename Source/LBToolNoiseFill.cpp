#include "LBToolNoiseFill.h"

#include "LBToolDistribution.h"
#include "LBToolShared.h"
#include "LevelBuilderCoreAPI.h"
#include "LevelBuilderCoreLoader.h"

#if EDITOR
#include "imgui.h"
#include "Plugins/PolyphaseEngineAPI.h"
#include "LBToolPicker.h"
#include "LBToolMaskImage.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    LBToolNoiseFill sInstance;

    // ---- Per-brush state (file-scope statics; no class members) ----
    bool   sHasStart = false;
    LBVec3 sStart{0,0,0};

    // Settings (controllable from DrawSettingsUI).
    float    sPieceStride = 1.0f;       // cell size in meters
    float    sThreshold   = 0.5f;       // noise cutoff [0,1]; higher = sparser
    float    sNoiseScale  = 3.0f;       // noise feature size in meters
    float    sJitterPos   = 0.5f;       // [0,1] fraction of cell
    float    sJitterYaw   = 0.0f;       // ±degrees
    uint32_t sSeed        = 1u;

    // Multi-target + optional mask (combined with noise — both must pass).
    std::vector<std::string> sTargets;
    bool sOpenTargetsPicker = false;
    bool sJustOpened        = false;
#if EDITOR
    LBToolMaskImage::MaskState sMask;
#endif
    bool  sUseMask       = false;
    float sMaskThreshold = 0.50f;
    bool  sMaskInvert    = false;

    // Max cells per commit so a huge rectangle with tiny stride can't
    // hang the editor. Mirrors LBToolShared::kStrideWalkMaxSteps in
    // spirit — at the cap we draw a warning in the settings UI.
    constexpr int kMaxCells = 256 * 256;

    LevelBuilderCoreAPI* CoreAPI() { return LevelBuilderCoreLoader::Get(); }

    void RerollSeed()
    {
        // Bump deterministically so subsequent rerolls visit fresh seeds.
        sSeed = (sSeed * 1664525u + 1013904223u) | 1u;
    }

    // Compute the corner-aligned rect from two clicks.
    void RectFromCorners(const LBVec3& a, const LBVec3& b,
                         LBVec3& outMin, LBVec3& outMax, float& outY)
    {
        outMin.x = std::min(a.x, b.x);
        outMin.z = std::min(a.z, b.z);
        outMax.x = std::max(a.x, b.x);
        outMax.z = std::max(a.z, b.z);
        outY     = 0.5f * (a.y + b.y);    // average Y; cells use this height
        outMin.y = outY;
        outMax.y = outY;
    }
}

bool LBToolNoiseFill::CanPlace(const LevelBuilderPlacementRequest&)
{
    return true;
}

LevelBuilderPlacementResult LBToolNoiseFill::Place(const LevelBuilderPlacementRequest& request)
{
    LevelBuilderPlacementResult result{};
    result.success = 0;
    result.spawnedNode = nullptr;
    result.errorMessage = nullptr;

    // First click: stash corner 1.
    if (!sHasStart)
    {
        sStart = request.position;
        sHasStart = true;
        static const char* kMsg = "NoiseFill: corner set; click again to commit";
        result.errorMessage = kMsg;
        return result;
    }

    // Second click: commit.
    LevelBuilderCoreAPI* api = CoreAPI();
    if (!api)
    {
        static const char* kErr = "NoiseFill: core API unavailable";
        result.errorMessage = kErr;
        sHasStart = false;
        return result;
    }
    void* userData = nullptr;
    LevelBuilderCoreAPI::LBBrushSpawnFn spawn = LBToolShared::ResolveSpawn(api, &userData);
    if (!spawn)
    {
        static const char* kErr =
            "NoiseFill: no spawn fn registered for the active tool";
        result.errorMessage = kErr;
        sHasStart = false;
        return result;
    }

    LBVec3 rmin, rmax;
    float  y = 0.0f;
    RectFromCorners(sStart, request.position, rmin, rmax, y);

    const float stride = (sPieceStride > 0.05f) ? sPieceStride : 0.05f;
    const float thresh = std::clamp(sThreshold, 0.0f, 1.0f);
    const int   nx = (int)std::max(1.0f, std::ceil((rmax.x - rmin.x) / stride));
    const int   nz = (int)std::max(1.0f, std::ceil((rmax.z - rmin.z) / stride));
    const int   cells = nx * nz;
    if (cells > kMaxCells)
    {
        static const char* kErr =
            "NoiseFill: rectangle too large for current stride (cap hit)";
        result.errorMessage = kErr;
        sHasStart = false;
        return result;
    }

    LBToolDistribution::Rng rng = LBToolDistribution::SeedRng(sSeed);
    int spawned = 0;
    void* lastNode = nullptr;

    // v9 undo grouping: one Ctrl+Z reverts the whole scatter. Editor-only.
#if EDITOR
    PolyphaseEngineAPI* eng = (PolyphaseEngineAPI*)api->GetEngineAPI();
    if (eng && eng->EditorAction_BeginGroup) eng->EditorAction_BeginGroup("NoiseFill");
    const bool useMask = sUseMask && LBToolMaskImage::EnsureLoaded(sMask);
    const int  mthresh255 = (int)(std::clamp(sMaskThreshold, 0.0f, 1.0f) * 255.0f);
#endif

    for (int ix = 0; ix < nx; ++ix)
    {
        for (int iz = 0; iz < nz; ++iz)
        {
            const float cx = rmin.x + (ix + 0.5f) * stride;
            const float cz = rmin.z + (iz + 0.5f) * stride;
            const float n  = LBToolDistribution::Noise2D(cx, cz, sNoiseScale, sSeed);
            if (n < thresh) continue;

#if EDITOR
            // Combined gate: cell must pass BOTH the noise check above
            // AND the mask check (if mask is enabled). Lets the user
            // mix organic noise with hand-painted boundaries.
            if (useMask)
            {
                const float u = (nx > 1) ? ((float)ix / (float)(nx - 1)) : 0.5f;
                const float v = (nz > 1) ? ((float)iz / (float)(nz - 1)) : 0.5f;
                const LBToolMaskImage::Pixel mp = LBToolMaskImage::Sample(sMask, u, v);
                const int lum = ((int)mp.r + (int)mp.g + (int)mp.b) / 3;
                const bool passes = sMaskInvert ? (lum < mthresh255) : (lum >= mthresh255);
                if (!passes) continue;
            }
            const char* asset = LBToolPicker::PickFromList(sTargets, nullptr, rng);
#else
            const char* asset = nullptr;
#endif

            // Position jitter inside the cell.
            const float jx = sJitterPos * stride * 0.5f *
                             LBToolDistribution::NextFloatRange(rng, -1.0f, 1.0f);
            const float jz = sJitterPos * stride * 0.5f *
                             LBToolDistribution::NextFloatRange(rng, -1.0f, 1.0f);
            LBVec3 pos{ cx + jx, y, cz + jz };

            LBQuat rot = LBToolDistribution::JitteredYaw(
                LBQuat{0,0,0,1}, sJitterYaw, rng);

            void* node = spawn(asset, &pos, &rot, userData);
            if (node) { lastNode = node; ++spawned; }
        }
    }

#if EDITOR
    if (eng && eng->EditorAction_EndGroup) eng->EditorAction_EndGroup();
#endif

    sHasStart = false;
    result.success = (spawned > 0) ? 1 : 0;
    result.spawnedNode = lastNode;
    return result;
}

void LBToolNoiseFill::DrawSettingsUI()
{
#if EDITOR
    ImGui::TextUnformatted("NoiseFill — scatter with 2D value-noise");
    ImGui::Separator();

    ImGui::SliderFloat("Piece stride (m)", &sPieceStride, 0.1f, 10.0f, "%.2f");
    ImGui::SliderFloat("Threshold",        &sThreshold,   0.0f, 1.0f,  "%.2f");
    ImGui::SliderFloat("Noise scale (m)",  &sNoiseScale,  0.25f, 32.0f, "%.2f");
    ImGui::SliderFloat("Position jitter",  &sJitterPos,   0.0f, 1.0f,  "%.2f");
    ImGui::SliderFloat("Yaw jitter (deg)", &sJitterYaw,   0.0f, 180.0f, "%.0f");

    int seedI = (int)sSeed;
    if (ImGui::InputInt("Seed", &seedI))
        sSeed = (uint32_t)(seedI <= 0 ? 1 : seedI);
    ImGui::SameLine();
    if (ImGui::Button("Reroll")) RerollSeed();

    // ---- Targets ----
    LevelBuilderCoreAPI* lbApi = CoreAPI();
    std::vector<LBToolPicker::PieceChoice> pieces =
        LBToolPicker::CollectActiveKitPieces(lbApi);
    const std::string projectRoot = LBToolPicker::CachedProjectRoot();
    const std::string kitFolder   = LBToolPicker::GetActiveKitFolder(lbApi);

    ImGui::Spacing();
    ImGui::TextUnformatted("Targets (random pick per cell)");
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
                                          32.0f, false, "nf_tgt", "?");
            if (ImGui::IsItemHovered() && pc)
                ImGui::SetTooltip("%s", pc->display.c_str());
            ImGui::PopID();
            if (i + 1 < (int)sTargets.size()) ImGui::SameLine();
        }
    }
    if (ImGui::Button("Edit targets…##nf_edit_targets", ImVec2(150, 0)))
    {
        sOpenTargetsPicker = true;
        sJustOpened        = true;
    }

    // ---- Optional mask (combines with noise — both must pass) ----
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Mask (combine with noise)##nf_mask"))
    {
        ImGui::Checkbox("Use mask##nf_use_mask", &sUseMask);
        if (sUseMask)
        {
            LBToolMaskImage::DrawMaskUI(sMask, "nf");
            ImGui::SliderFloat("Threshold##nf_mt", &sMaskThreshold, 0.0f, 1.0f, "%.2f");
            ImGui::Checkbox("Invert##nf_mi", &sMaskInvert);
            ImGui::TextDisabled("Cell must pass BOTH noise AND mask (AND-gate).");
        }
    }

    if (sOpenTargetsPicker)
    {
        ImGui::OpenPopup("NoiseFill: Targets##nf_tgt_modal");
        sOpenTargetsPicker = false;
    }
    LBToolPicker::DrawPiecePickerModal("NoiseFill: Targets##nf_tgt_modal",
                                       "NoiseFill — multi-select (random per cell)",
                                       /*multiSelect=*/true, /*allowAny=*/false,
                                       sJustOpened, &sTargets);

    ImGui::Spacing();
    if (sHasStart)
    {
        ImGui::TextColored(ImVec4(0.30f, 0.85f, 0.40f, 1.0f),
            "Click 2 to commit the rectangle. Start: (%.2f, %.2f, %.2f)",
            sStart.x, sStart.y, sStart.z);
        if (ImGui::Button("Cancel"))
            sHasStart = false;
    }
    else
    {
        ImGui::TextDisabled("Click in the viewport to set the first corner.");
    }
#endif
}

void LBToolNoiseFill::Initialize(LevelBuilderCoreAPI* api)
{
    if (!api) return;
    if (api->RegisterBrush) api->RegisterBrush("NoiseFill", &sInstance);
}

void LBToolNoiseFill::Shutdown(LevelBuilderCoreAPI* api)
{
    if (!api) return;
    if (api->UnregisterBrush) api->UnregisterBrush("NoiseFill");
    sHasStart = false;
}

// Viewport overlay — preview the rectangle outline + a cell-grid hint
// between Click 1 and Click 2.
extern "C" void LBToolNoiseFill_DrawViewportOverlayTrampoline(
    float /*vx*/, float /*vy*/, float /*vw*/, float /*vh*/, void* /*ud*/)
{
#if EDITOR
    LevelBuilderCoreAPI* api = CoreAPI();
    if (!api || !api->GetActiveBrushName || !api->GetEngineAPI) return;
    const char* active = api->GetActiveBrushName();
    if (!active || std::strcmp(active, "NoiseFill") != 0) return;
    if (!sHasStart) return;

    PolyphaseEngineAPI* eng = (PolyphaseEngineAPI*)api->GetEngineAPI();
    if (!eng || !eng->Gizmos_DrawLine || !eng->Gizmos_SetColor) return;

    LBVec3 hit;
    LBQuat hitRot;
    LBVec3 dummy;
    if (api->GetPreviewTransform) api->GetPreviewTransform(&hit, &hitRot, &dummy);

    LBVec3 rmin, rmax; float y = 0.0f;
    RectFromCorners(sStart, hit, rmin, rmax, y);

    eng->Gizmos_SetColor(0.20f, 0.85f, 1.0f, 0.85f);
    eng->Gizmos_DrawLine(rmin.x, y, rmin.z, rmax.x, y, rmin.z);
    eng->Gizmos_DrawLine(rmax.x, y, rmin.z, rmax.x, y, rmax.z);
    eng->Gizmos_DrawLine(rmax.x, y, rmax.z, rmin.x, y, rmax.z);
    eng->Gizmos_DrawLine(rmin.x, y, rmax.z, rmin.x, y, rmin.z);

    if (eng->Gizmos_ResetState) eng->Gizmos_ResetState();
#endif
}
