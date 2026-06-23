#include "LBToolNoiseFill.h"

#include "LBToolDistribution.h"
#include "LBToolShared.h"
#include "LevelBuilderCoreAPI.h"
#include "LevelBuilderCoreLoader.h"

#if EDITOR
#include "imgui.h"
#include "Plugins/PolyphaseEngineAPI.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

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
#endif

    for (int ix = 0; ix < nx; ++ix)
    {
        for (int iz = 0; iz < nz; ++iz)
        {
            const float cx = rmin.x + (ix + 0.5f) * stride;
            const float cz = rmin.z + (iz + 0.5f) * stride;
            const float n  = LBToolDistribution::Noise2D(cx, cz, sNoiseScale, sSeed);
            if (n < thresh) continue;

            // Position jitter inside the cell. sJitterPos of 0 → center;
            // 1.0 → may land anywhere in the cell.
            const float jx = sJitterPos * stride * 0.5f *
                             LBToolDistribution::NextFloatRange(rng, -1.0f, 1.0f);
            const float jz = sJitterPos * stride * 0.5f *
                             LBToolDistribution::NextFloatRange(rng, -1.0f, 1.0f);
            LBVec3 pos{ cx + jx, y, cz + jz };

            LBQuat rot = LBToolDistribution::JitteredYaw(
                LBQuat{0,0,0,1}, sJitterYaw, rng);

            void* node = spawn(nullptr, &pos, &rot, userData);
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
