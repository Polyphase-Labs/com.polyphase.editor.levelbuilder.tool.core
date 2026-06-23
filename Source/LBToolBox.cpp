#include "LBToolBox.h"

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
    LBToolBox sInstance;

    bool   sHasStart            = false;
    LBVec3 sStart{0, 0, 0};
    float  sStride              = 1.0f;
    // Default: each edge gets a 0/90/180/270° yaw added on top of the
    // user's R rotation so walls along +X edges face along +X, walls
    // along +Z edges face along +Z, etc. The user dials R to align
    // edge 0 (the +X edge) with their piece's authoring convention;
    // the other three edges auto-derive from that base.
    //
    // Toggle to false when the artist's pieces aren't directional
    // (symmetric props, columns, ...) so every edge uses the bare
    // user-R rotation.
    bool   sAutoRotatePerEdge   = true;

    // Build the four XZ-plane corners of the AABB from `a` and `b`,
    // anchored at `a.y` (the first click's height — pieces stay upright).
    // Returned in clockwise order starting at minX/minZ.
    void BuildBoxCorners(const LBVec3& a, const LBVec3& b, LBVec3 outCorners[4])
    {
        const float minX = a.x < b.x ? a.x : b.x;
        const float maxX = a.x > b.x ? a.x : b.x;
        const float minZ = a.z < b.z ? a.z : b.z;
        const float maxZ = a.z > b.z ? a.z : b.z;
        const float y    = a.y;

        outCorners[0] = LBVec3{minX, y, minZ};
        outCorners[1] = LBVec3{maxX, y, minZ};
        outCorners[2] = LBVec3{maxX, y, maxZ};
        outCorners[3] = LBVec3{minX, y, maxZ};
    }

    // Pure Y-axis (yaw) quaternion. Right-handed, Y-up: positive degrees
    // rotate counter-clockwise as viewed from above.
    LBQuat YawDegToQuat(float deg)
    {
        const float DEG2RAD = 3.14159265358979323846f / 180.0f;
        const float half = deg * DEG2RAD * 0.5f;
        return LBQuat{ 0.0f, std::sin(half), 0.0f, std::cos(half) };
    }

    // Hamilton quaternion multiplication. `a * b` reads as "apply b first,
    // then a" when rotating a vector via v' = q * v * conj(q).
    LBQuat QuatMul(const LBQuat& a, const LBQuat& b)
    {
        return LBQuat{
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
        };
    }

    // Per-edge yaw delta (degrees). Edge index runs 0/1/2/3 clockwise
    // matching BuildBoxCorners' output: +X, +Z, -X, -Z.
    float EdgeYawDelta(int edgeIdx)
    {
        return (float)((edgeIdx & 3) * 90);
    }

    // Shift the two endpoints of an edge inward by half-stride along the
    // edge direction so a piece's CENTER never lands on a corner. With
    // this inset:
    //   - Each piece sits fully inside the rectangle (no half-piece
    //     sticking out past minX / maxX / minZ / maxZ).
    //   - Adjacent edges' last/first pieces meet at the corner with
    //     their outer edges (not their centers) defining the AABB.
    //   - For edges shorter than `stride` the inset would invert; we
    //     leave those untouched and the walker emits a single piece
    //     at the start corner.
    void InsetEdgeByHalfStride(const LBVec3& a, const LBVec3& b, float stride,
                               LBVec3& outA, LBVec3& outB)
    {
        outA = a;
        outB = b;

        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float dz = b.z - a.z;
        const float len = std::sqrt(dx*dx + dy*dy + dz*dz);

        // Degenerate (zero-length edge) or short edge — caller still gets
        // the original endpoints and the walker handles it.
        if (len < stride) return;

        const float half = 0.5f * stride;
        const float ux = dx / len;
        const float uy = dy / len;
        const float uz = dz / len;
        outA = LBVec3{ a.x + half * ux, a.y + half * uy, a.z + half * uz };
        outB = LBVec3{ b.x - half * ux, b.y - half * uy, b.z - half * uz };
    }
}

bool LBToolBox::CanPlace(const LevelBuilderPlacementRequest& /*request*/) { return true; }

LevelBuilderPlacementResult LBToolBox::Place(const LevelBuilderPlacementRequest& request)
{
    LevelBuilderPlacementResult result{};
    result.success = 0;

    // -------- First click: stash the corner and wait --------
    if (!sHasStart)
    {
        sStart    = request.position;
        sHasStart = true;
        static const char* kMsg = "Box: first corner set; click again to commit";
        result.errorMessage = kMsg;
        return result;
    }

    // -------- Second click: commit the perimeter --------
    LevelBuilderCoreAPI* api = LevelBuilderCoreLoader::Get();
    if (!api)
    {
        static const char* kErr = "Box: core API unavailable";
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
            "Box: no spawn fn for active tool (sibling needs RegisterSpawnFn) "
            "or pre-v4 core";
        result.errorMessage = kErr;
        sHasStart = false;
        return result;
    }

    // Shift-axis-lock pulls the second corner onto a cardinal — produces a
    // narrow strip instead of a rectangle. Same constraint the overlay
    // previews so visuals match commits.
    const LBVec3 endShifted = LBToolShared::MaybeAxisConstrain(sStart, request.position);

    float stride = sStride;
    if (stride < 1e-4f) stride = 1e-4f;

    // Snap the second corner so the rect's per-axis dimensions are exact
    // stride multiples. Eliminates the "perimeter length isn't a multiple
    // of stride" leftover that produced a small gap at one corner per
    // edge. We snap dx/dz independently so a click that's nearly on a
    // cardinal still produces a sane rect. Y is anchored to the first
    // click so pieces stay upright (matches BuildBoxCorners).
    const LBVec3 endSnapped{
        sStart.x + std::round((endShifted.x - sStart.x) / stride) * stride,
        sStart.y,
        sStart.z + std::round((endShifted.z - sStart.z) / stride) * stride
    };

    LBVec3 corners[4];
    BuildBoxCorners(sStart, endSnapped, corners);

    // Walk each edge stride-stepped. Skip the END point per edge so the
    // next edge's start (= this edge's end) only fires once — every
    // corner gets exactly one piece.
    void* lastSpawned = nullptr;
    int   placed = 0;
    int   totalPoints = 0;

    // v9 undo grouping: one Ctrl+Z reverts the whole box. Editor-only.
#if EDITOR
    PolyphaseEngineAPI* eng = (PolyphaseEngineAPI*)api->GetEngineAPI();
    if (eng && eng->EditorAction_BeginGroup) eng->EditorAction_BeginGroup("Box");
#endif

    for (int e = 0; e < 4; ++e)
    {
        const LBVec3& a = corners[e];
        const LBVec3& b = corners[(e + 1) % 4];

        // Inset both endpoints by half-stride along the edge so piece
        // centers tile cleanly INSIDE the rectangle. With the inset:
        //   - Outer edges of the leftmost/rightmost pieces sit exactly
        //     on minX / maxX (same for Z).
        //   - Adjacent edges meet at the corner via their outer extents,
        //     not via overlapping centers.
        LBVec3 a2, b2;
        InsetEdgeByHalfStride(a, b, stride, a2, b2);

        LBToolShared::StrideWalk walk = LBToolShared::BuildStrideWalk(a2, b2, stride);

        // Per-edge rotation: compose `edgeDelta * userRotation` so the
        // user's R-yaw (encoded in request.rotation) is the BASE and the
        // edge index adds 0/90/180/270 on top. Disabled => every edge
        // uses the bare user rotation, matching pre-T2.1 behavior.
        LBQuat edgeRot = request.rotation;
        if (sAutoRotatePerEdge)
            edgeRot = QuatMul(YawDegToQuat(EdgeYawDelta(e)), request.rotation);

        // Walker emits PieceCount() pieces along the inset segment —
        // last piece sits at the inset end (b2), not at the original
        // corner b. The next edge picks up at a2_next = b + halfStride
        // along the next edge's direction, which is the perimeter
        // tile-edge meeting point.
        const int emit = walk.PieceCount();
        for (int i = 0; i < emit; ++i)
        {
            const LBVec3 p = walk.At(i);
            ++totalPoints;
            void* n = spawn(nullptr, &p, &edgeRot, userData);
            if (n) { lastSpawned = n; ++placed; }
        }
    }

#if EDITOR
    if (eng && eng->EditorAction_EndGroup) eng->EditorAction_EndGroup();
#endif

    sHasStart = false;

    if (placed > 0)
    {
        result.success     = 1;
        result.spawnedNode = lastSpawned;
        if (api->LogDebug)
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "[LBToolBox] committed perimeter: %d / %d pieces (stride=%.3f)",
                          placed, totalPoints, stride);
            api->LogDebug(buf);
        }
    }
    else
    {
        static const char* kErr = "Box: all spawn calls failed";
        result.errorMessage = kErr;
    }
    return result;
}

void LBToolBox::DrawSettingsUI()
{
#if EDITOR
    ImGui::TextUnformatted("Box brush (perimeter)");
    ImGui::Separator();

    ImGui::SliderFloat("Stride", &sStride, 0.1f, 10.0f, "%.2f");

    ImGui::Checkbox("Auto-rotate per edge", &sAutoRotatePerEdge);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Each edge gets a +0/+90/+180/+270 deg yaw added on top of\n"
            "the active tool's R rotation.\n\n"
            "Dial R so edge 0 (the +X edge) matches your piece's\n"
            "authoring direction — the other three edges follow.\n\n"
            "Disable for symmetric pieces (columns, props) so every\n"
            "edge uses the bare R rotation.");
    }

    if (sHasStart)
    {
        ImGui::TextColored(ImVec4(0.20f, 1.00f, 0.30f, 0.85f),
                           "Click again to commit. Corner 1: (%.2f, %.2f, %.2f)",
                           sStart.x, sStart.y, sStart.z);
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel"))
            sHasStart = false;
        ImGui::TextDisabled("Tip: hold Shift to lock the rectangle to a single axis.");
    }
    else
    {
        ImGui::TextDisabled("Click in the viewport to set the first corner.");
        ImGui::TextDisabled("Pieces are placed along the four edges of the rectangle.");
        ImGui::TextDisabled("The second corner snaps so width and depth are stride-multiples.");
    }
#endif
}

void LBToolBox::Initialize(LevelBuilderCoreAPI* api)
{
    if (!api || !api->RegisterBrush) return;
    api->RegisterBrush("Box", &sInstance);
}

void LBToolBox::Shutdown(LevelBuilderCoreAPI* api)
{
    if (!api || !api->UnregisterBrush) return;
    api->UnregisterBrush("Box");
}

// -----------------------------------------------------------------------------
// Viewport overlay — start-corner sphere + wire rectangle from start to the
// current hover hit, plus stride markers along the four edges so the user
// can see exactly where pieces will land.
// -----------------------------------------------------------------------------

namespace
{
#if EDITOR
    void DrawBoxPreview_Impl(float, float, float, float, void*)
    {
        if (!sHasStart) return;

        LevelBuilderCoreAPI* api = LevelBuilderCoreLoader::Get();
        if (!api) return;

        const char* activeBrush = api->GetActiveBrushName ? api->GetActiveBrushName() : "";
        if (!activeBrush || std::strcmp(activeBrush, "Box") != 0) return;

        PolyphaseEngineAPI* eng = (PolyphaseEngineAPI*)api->GetEngineAPI();
        if (!eng) return;
        if (!eng->Gizmos_DrawWireSphere || !eng->Gizmos_DrawLine || !eng->Gizmos_SetColor)
            return;

        // Resolve hover.
        if (!api->Viewport_GetHoverHit) return;
        LBVec3 hit{0,0,0}, nrm{0,1,0};
        void*  node = nullptr;
        if (!api->Viewport_GetHoverHit(&hit, &nrm, &node))
        {
            // Still draw the corner sphere so the user sees the start.
            eng->Gizmos_SetColor(0.20f, 0.80f, 1.00f, 0.95f);
            eng->Gizmos_DrawWireSphere(sStart.x, sStart.y, sStart.z, 0.15f);
            if (eng->Gizmos_ResetState) eng->Gizmos_ResetState();
            return;
        }

        const bool shiftLocked = ImGui::GetIO().KeyShift;
        hit = LBToolShared::MaybeAxisConstrain(sStart, hit);

        // Mirror the commit-time stride-snap so the preview rectangle
        // sits where pieces will actually land.
        float strideForPreview = sStride;
        if (strideForPreview < 1e-4f) strideForPreview = 1e-4f;
        const LBVec3 hitSnapped{
            sStart.x + std::round((hit.x - sStart.x) / strideForPreview) * strideForPreview,
            sStart.y,
            sStart.z + std::round((hit.z - sStart.z) / strideForPreview) * strideForPreview
        };

        LBVec3 corners[4];
        BuildBoxCorners(sStart, hitSnapped, corners);

        // Edge color: cyan; amber when axis-locked (matches Line's palette).
        const float lr = shiftLocked ? 1.00f : 0.20f;
        const float lg = shiftLocked ? 0.85f : 0.80f;
        const float lb = shiftLocked ? 0.20f : 1.00f;

        // Start-corner sphere — bright so it's findable.
        eng->Gizmos_SetColor(lr, lg, lb, 0.95f);
        eng->Gizmos_DrawWireSphere(sStart.x, sStart.y, sStart.z, 0.15f);

        // 4 edges. The wire LINE stays on the AABB corners (it's the
        // user's actual footprint outline); marker spheres mirror the
        // commit-time half-stride inset so the user sees exactly where
        // pieces will land.
        for (int e = 0; e < 4; ++e)
        {
            const LBVec3& a = corners[e];
            const LBVec3& b = corners[(e + 1) % 4];
            eng->Gizmos_SetColor(lr, lg, lb, 0.95f);
            eng->Gizmos_DrawLine(a.x, a.y, a.z, b.x, b.y, b.z);

            LBVec3 a2, b2;
            InsetEdgeByHalfStride(a, b, sStride, a2, b2);
            LBToolShared::StrideWalk walk = LBToolShared::BuildStrideWalk(a2, b2, sStride);

            // DrawStrideMarkers skips i=0 (designed for Line where the
            // start sphere is the bright corner). Box's inset walk has a
            // real piece at i=0, so draw an explicit marker there before
            // delegating the rest to the shared helper.
            eng->Gizmos_SetColor(lr, lg, lb, 0.55f);
            const LBVec3 first = walk.At(0);
            eng->Gizmos_DrawWireSphere(first.x, first.y, first.z, 0.06f);

            LBToolShared::DrawStrideMarkers(eng, walk, lr, lg, lb, 0.55f);
        }

        if (eng->Gizmos_ResetState) eng->Gizmos_ResetState();
    }
#endif
}

extern "C" void LBToolBox_DrawViewportOverlayTrampoline(
    float x, float y, float w, float h, void* ud)
{
#if EDITOR
    DrawBoxPreview_Impl(x, y, w, h, ud);
#else
    (void)x; (void)y; (void)w; (void)h; (void)ud;
#endif
}
