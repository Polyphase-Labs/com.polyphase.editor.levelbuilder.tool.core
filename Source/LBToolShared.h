/**
 * @file LBToolShared.h
 * @brief Shared helpers across tool.core's brushes (Line, Box, BoxFill, ...).
 *
 * Three thin layers, all stateless:
 *   1. Geometry — Shift-axis-lock helper, stride-walking math.
 *   2. Spawn resolution — one-line wrapper around
 *      `LevelBuilderCoreAPI::GetSpawnFnForActiveTool` that returns the
 *      active sibling's spawn fn + its userData.
 *   3. Preview overlay helpers — `Gizmos_*` wrappers that draw stride
 *      markers along a segment with sensible truncation caps so a tiny
 *      stride over a huge distance can't drown the viewport in gizmos.
 *
 * The brushes themselves carry NO data members (vtable layout must match
 * across DLLs). All per-brush state stays in file-scope statics inside
 * each brush's .cpp.
 */

#pragma once

#include "LevelBuilderCoreAPI.h"

struct PolyphaseEngineAPI;

namespace LBToolShared
{
    // -------- Geometry ---------------------------------------------------

    /**
     * @brief When Shift is held, snap `end` to whichever cardinal axis
     *        (relative to `start`) has the largest delta. No-op when
     *        Shift isn't held or when we're not in an editor build.
     *
     * Used identically by Line, Box's second-click footprint, and the
     * preview overlays — the same constraint at commit AND at preview
     * so the user sees what they'll get.
     */
    LBVec3 MaybeAxisConstrain(const LBVec3& start, const LBVec3& end);

    /**
     * @brief Pre-computed stride walk from `start` to `end`.
     *
     * `numSteps` is the number of stride-sized segments that fit between
     * start and end inclusive; the walk emits `numSteps + 1` points (i=0
     * is the start, i=numSteps is the last point at exactly numSteps*stride
     * from the start, which may stop short of `end` when the distance
     * isn't an integer multiple of stride).
     *
     * Capped at `kStrideWalkMaxSteps` to prevent a tiny stride over a
     * huge distance from blowing up.
     */
    struct StrideWalk
    {
        LBVec3 start{0,0,0};
        float  dirX = 0.0f, dirY = 0.0f, dirZ = 0.0f;   // unit direction
        float  stride   = 1.0f;
        int    numSteps = 0;                            // pieces = numSteps + 1
        float  totalDist = 0.0f;

        // Point at step i, i in [0, numSteps].
        LBVec3 At(int i) const
        {
            const float t = (float)i * stride;
            return LBVec3{ start.x + dirX * t,
                           start.y + dirY * t,
                           start.z + dirZ * t };
        }

        int   PieceCount() const { return numSteps + 1; }
        bool  HasMovement() const { return totalDist >= 1e-4f; }
    };

    constexpr int kStrideWalkMaxSteps = 1024;

    StrideWalk BuildStrideWalk(const LBVec3& start, const LBVec3& end,
                               float stride,
                               int   maxSteps = kStrideWalkMaxSteps);

    // -------- Spawn resolution -------------------------------------------

    /**
     * @brief Resolve the active tool's spawn function (v4 ABI).
     *
     * Returns nullptr if the core is too old, no spawn fn is registered
     * for the active tool, or no sibling is currently active. Each caller
     * builds its own error string for the result.errorMessage path.
     */
    LevelBuilderCoreAPI::LBBrushSpawnFn ResolveSpawn(
        LevelBuilderCoreAPI* api, void** outUserData);

    // -------- Preview overlay helpers (no-op outside EDITOR) -------------

#if EDITOR
    constexpr int kPreviewMarkersMax = 64;

    /**
     * @brief Cap a marker count so a runaway stride doesn't draw N
     *        thousand gizmos.  `outTruncated` is set true when the cap
     *        was hit; the caller can draw an amber "more pieces will
     *        spawn than you see" indicator at the last drawn marker.
     */
    int ClampPreviewMarkers(int wanted, bool* outTruncated);

    /**
     * @brief Draw a wire sphere at every stride step along `walk`,
     *        with the last marker drawn slightly larger so the user can
     *        see where the segment really ends (often short of the
     *        cursor). Respects `kPreviewMarkersMax`. No-op if engine
     *        doesn't expose `Gizmos_DrawWireSphere` / `Gizmos_SetColor`.
     *
     * Skips i=0 (the start) since callers usually draw the start point
     * with a distinct color.
     */
    void DrawStrideMarkers(PolyphaseEngineAPI* eng,
                           const StrideWalk&   walk,
                           float r, float g, float b, float a);
#endif // EDITOR
}
