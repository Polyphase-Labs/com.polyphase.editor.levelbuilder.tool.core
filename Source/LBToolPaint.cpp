#include "LBToolPaint.h"

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
#include <cstring>
#include <vector>

namespace
{
    LBToolPaint sInstance;

    enum class Mode { Add = 0, Erase = 1, Both = 2 };

    // Settings (controllable from DrawSettingsUI).
    Mode      sMode        = Mode::Add;
    float     sRadius      = 1.5f;       // meters
    float     sDensity     = 8.0f;       // placements per m² per second (Add only)
    float     sJitterPos   = 1.0f;       // [0,1] fraction of radius
    float     sJitterYaw   = 0.0f;       // ±degrees
    float     sStampMs     = 80.0f;      // ms between stamps (Add minimum interval)
    uint32_t  sSeed        = 12345u;

    // Per-frame accumulator: when LMB is held in Add mode we accumulate
    // time since the last stamp and emit one whenever the accumulator
    // exceeds `sStampMs` (in milliseconds). Reset when the button releases.
    float     sStampAccum  = 0.0f;
    bool      sLmbWasDown  = false;
    bool      sRmbWasDown  = false;

    LevelBuilderCoreAPI* CoreAPI() { return LevelBuilderCoreLoader::Get(); }

    // ---- Erase mode visitor ----
    // v7 contract: returning 1 means "consume" — the sibling's
    // enumerate fn handles the destroy + its own registry cleanup in
    // the same transaction. We just count what's gone.
    int EraseVisit(void* node, void* visitUd)
    {
        if (!node) return 0;
        int* counter = (int*)visitUd;
        if (counter) (*counter)++;
        return 1;   // consume
    }

    // Erase any placed pieces within `radius` of `center` via the
    // active sibling's enumerate fn. The sibling is responsible for
    // calling DestroyNode and removing the node from its own
    // placed-piece registry — that prevents the use-after-free we hit
    // in v7-first-cut when 60Hz TickEditor re-destroyed dangling
    // pointers.
    int EraseAt(LevelBuilderCoreAPI* api, const LBVec3& center, float radius)
    {
        if (!api || !api->GetEnumerateFnForActiveTool) return 0;
        void* enumUd = nullptr;
        LevelBuilderCoreAPI::LBEnumeratePlacementsFn enumFn =
            api->GetEnumerateFnForActiveTool(&enumUd);
        if (!enumFn) return 0;

        int destroyed = 0;
        enumFn(&center, radius, &EraseVisit, &destroyed, enumUd);
        return destroyed;
    }

    // Drop a single stamp's worth of placements. Returns count spawned.
    int StampAdd(LevelBuilderCoreAPI* api, const LBVec3& center)
    {
        void* userData = nullptr;
        LevelBuilderCoreAPI::LBBrushSpawnFn spawn =
            LBToolShared::ResolveSpawn(api, &userData);
        if (!spawn) return 0;

        // Number of placements this stamp = density × stamp area × interval.
        // Stamp area = π r². Interval = sStampMs/1000.
        const float area = 3.14159265358979323846f * sRadius * sRadius;
        const float dt   = sStampMs * 0.001f;
        const float n    = sDensity * area * dt;
        int    nWhole    = (int)n;
        float  nFrac     = n - (float)nWhole;
        // Probabilistically include the fractional placement so density
        // converges to the configured value over time.
        LBToolDistribution::Rng rng = LBToolDistribution::SeedRng(sSeed);
        sSeed = (sSeed * 1103515245u + 12345u) | 1u;        // walk the seed
        if (LBToolDistribution::NextFloat01(rng) < nFrac) ++nWhole;

        int spawned = 0;
        const float jitterRadius = sRadius * std::clamp(sJitterPos, 0.0f, 1.0f);
        for (int i = 0; i < nWhole; ++i)
        {
            LBVec3 pos = LBToolDistribution::RandomInDisc(center, jitterRadius, rng);
            LBQuat rot = LBToolDistribution::JitteredYaw(LBQuat{0,0,0,1}, sJitterYaw, rng);
            if (spawn(nullptr, &pos, &rot, userData))
                ++spawned;
        }
        return spawned;
    }
}

bool LBToolPaint::CanPlace(const LevelBuilderPlacementRequest&)
{
    return true;
}

LevelBuilderPlacementResult LBToolPaint::Place(const LevelBuilderPlacementRequest& request)
{
    // Single-click stamp — fires one stamp on a plain click (separate
    // from the drag-driven TickEditor path).
    LevelBuilderPlacementResult result{};
    result.success = 0;
    result.spawnedNode = nullptr;
    result.errorMessage = nullptr;

    LevelBuilderCoreAPI* api = CoreAPI();
    if (!api)
    {
        static const char* kErr = "Paint: core API unavailable";
        result.errorMessage = kErr;
        return result;
    }

    if (sMode == Mode::Erase)
    {
        int n = EraseAt(api, request.position, sRadius);
        result.success = (n > 0) ? 1 : 0;
        return result;
    }

    int n = StampAdd(api, request.position);
    result.success = (n > 0) ? 1 : 0;
    return result;
}

void LBToolPaint::TickEditor(float deltaTime)
{
    LevelBuilderCoreAPI* api = CoreAPI();
    if (!api) return;

    // Hover hit must be valid to know WHERE to stamp/erase.
    LBVec3 hit, hitN;
    void*  hitNode = nullptr;
    if (!api->Viewport_GetHoverHit) return;
    if (!api->Viewport_GetHoverHit(&hit, &hitN, &hitNode)) return;

    const bool lmbDown = api->Viewport_IsLmbDown && api->Viewport_IsLmbDown();
    const bool rmbDown = api->Viewport_IsRmbDown && api->Viewport_IsRmbDown();

    // What the buttons MEAN given the current mode.
    const bool wantAdd =
        ((sMode == Mode::Add  || sMode == Mode::Both) && lmbDown);
    const bool wantErase =
        (sMode == Mode::Erase && lmbDown) ||
        (sMode == Mode::Both  && rmbDown);

    // ---- v9 undo grouping ----
    // Each press → release of a button is ONE stroke = one Ctrl+Z entry.
    // Open the group on the rising edge of whichever button is active;
    // close on the falling edge. Lmb / Rmb tracked separately because
    // in "Both" mode the user can press one without affecting the other.
    // Editor-only — runtime doesn't ship the action-manager surface.
#if EDITOR
    PolyphaseEngineAPI* eng = (PolyphaseEngineAPI*)api->GetEngineAPI();

    auto beginGroup = [&](const char* label) {
        if (eng && eng->EditorAction_BeginGroup) eng->EditorAction_BeginGroup(label);
    };
    auto endGroup = [&]() {
        if (eng && eng->EditorAction_EndGroup) eng->EditorAction_EndGroup();
    };

    // LMB rising edge — start an Add stroke (or Erase if mode=Erase).
    if (lmbDown && !sLmbWasDown)
        beginGroup(sMode == Mode::Erase ? "Paint Erase" : "Paint Stroke");
    // LMB falling edge — close the Add/Erase stroke, reset accum.
    if (!lmbDown && sLmbWasDown)
    {
        endGroup();
        sStampAccum = 0.0f;
    }
    // RMB rising edge — only meaningful in Both mode for Erase.
    if (rmbDown && !sRmbWasDown && sMode == Mode::Both)
        beginGroup("Paint Erase");
    if (!rmbDown && sRmbWasDown && sMode == Mode::Both)
        endGroup();
#else
    // Same accum reset on LMB release even without undo grouping.
    if (!lmbDown && sLmbWasDown) sStampAccum = 0.0f;
#endif
    sLmbWasDown = lmbDown;
    sRmbWasDown = rmbDown;

    // ---- Erase (RMB held in Add/Both; LMB held in Erase mode) ----
    if (wantErase)
    {
        EraseAt(api, hit, sRadius);
        // Erase per-frame; no rate-limit. Most placed-piece registries
        // are small enough that ~60Hz erase is fine.
        return;
    }

    // ---- Add ----
    if (!wantAdd) return;

    sStampAccum += deltaTime * 1000.0f;       // ms
    const float interval = (sStampMs > 1.0f) ? sStampMs : 1.0f;
    while (sStampAccum >= interval)
    {
        sStampAccum -= interval;
        StampAdd(api, hit);
    }
}

void LBToolPaint::DrawSettingsUI()
{
#if EDITOR
    ImGui::TextUnformatted("Paint — drag-LMB to stamp, drag-RMB to erase");
    ImGui::Separator();

    static const char* kModeLabels[] = { "Add (LMB)", "Erase (LMB)", "Both (LMB+RMB)" };
    int mi = (int)sMode;
    if (ImGui::Combo("Mode", &mi, kModeLabels, IM_ARRAYSIZE(kModeLabels)))
        sMode = (Mode)mi;

    ImGui::SliderFloat("Radius (m)",          &sRadius,    0.1f, 10.0f, "%.2f");
    ImGui::SliderFloat("Density (per m²/s)",  &sDensity,   0.1f, 64.0f, "%.2f");
    ImGui::SliderFloat("Position jitter",     &sJitterPos, 0.0f, 1.0f,  "%.2f");
    ImGui::SliderFloat("Yaw jitter (deg)",    &sJitterYaw, 0.0f, 180.0f, "%.0f");
    ImGui::SliderFloat("Stamp interval (ms)", &sStampMs,   16.0f, 500.0f, "%.0f");

    int seedI = (int)sSeed;
    if (ImGui::InputInt("Seed", &seedI))
        sSeed = (uint32_t)(seedI <= 0 ? 1 : seedI);

    ImGui::Spacing();
    ImGui::TextDisabled("Mode determines what each mouse button does:");
    ImGui::BulletText("Add — LMB stamps, RMB does nothing");
    ImGui::BulletText("Erase — LMB erases, RMB does nothing");
    ImGui::BulletText("Both — LMB stamps, RMB erases");
#endif
}

void LBToolPaint::Initialize(LevelBuilderCoreAPI* api)
{
    if (!api) return;
    if (api->RegisterBrush) api->RegisterBrush("Paint", &sInstance);
}

void LBToolPaint::Shutdown(LevelBuilderCoreAPI* api)
{
    if (!api) return;
    if (api->UnregisterBrush) api->UnregisterBrush("Paint");
    sStampAccum = 0.0f;
    sLmbWasDown = false;
    sRmbWasDown = false;
}

// Viewport overlay — wire circle at the cursor showing the radius.
extern "C" void LBToolPaint_DrawViewportOverlayTrampoline(
    float /*vx*/, float /*vy*/, float /*vw*/, float /*vh*/, void* /*ud*/)
{
#if EDITOR
    LevelBuilderCoreAPI* api = CoreAPI();
    if (!api || !api->GetActiveBrushName || !api->GetEngineAPI) return;
    const char* active = api->GetActiveBrushName();
    if (!active || std::strcmp(active, "Paint") != 0) return;

    PolyphaseEngineAPI* eng = (PolyphaseEngineAPI*)api->GetEngineAPI();
    if (!eng || !eng->Gizmos_DrawLine || !eng->Gizmos_SetColor) return;

    LBVec3 hit, hitN; void* hitNode = nullptr;
    if (!api->Viewport_GetHoverHit) return;
    if (!api->Viewport_GetHoverHit(&hit, &hitN, &hitNode)) return;

    // Color: green when adding, red when erasing.
    const bool eraseMode = (sMode == Mode::Erase) ||
                           (sMode == Mode::Both &&
                            api->Viewport_IsRmbDown && api->Viewport_IsRmbDown());
    if (eraseMode) eng->Gizmos_SetColor(1.0f, 0.30f, 0.30f, 0.9f);
    else           eng->Gizmos_SetColor(0.30f, 1.0f, 0.30f, 0.9f);

    // Approximate a circle on the XZ plane with 32 segments.
    const int kSegs = 32;
    const float TWO_PI = 6.2831853071795864769f;
    LBVec3 prev{ hit.x + sRadius, hit.y, hit.z };
    for (int i = 1; i <= kSegs; ++i)
    {
        const float t = (float)i / (float)kSegs * TWO_PI;
        LBVec3 next{ hit.x + sRadius * std::cos(t),
                     hit.y,
                     hit.z + sRadius * std::sin(t) };
        eng->Gizmos_DrawLine(prev.x, prev.y, prev.z, next.x, next.y, next.z);
        prev = next;
    }
    if (eng->Gizmos_ResetState) eng->Gizmos_ResetState();
#endif
}
