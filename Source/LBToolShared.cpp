#include "LBToolShared.h"

#if EDITOR
#include "imgui.h"
#include "Plugins/PolyphaseEngineAPI.h"
#endif

#include <cmath>

namespace LBToolShared
{
    // ---------- Geometry ----------

    LBVec3 MaybeAxisConstrain(const LBVec3& start, const LBVec3& end)
    {
#if EDITOR
        if (!ImGui::GetIO().KeyShift) return end;

        const float dx = end.x - start.x;
        const float dy = end.y - start.y;
        const float dz = end.z - start.z;
        const float ax = std::fabs(dx);
        const float ay = std::fabs(dy);
        const float az = std::fabs(dz);

        LBVec3 out = start;
        if (ax >= ay && ax >= az)      out.x = end.x;   // lock to X
        else if (ay >= ax && ay >= az) out.y = end.y;   // lock to Y
        else                           out.z = end.z;   // lock to Z
        return out;
#else
        (void)start;
        return end;
#endif
    }

    StrideWalk BuildStrideWalk(const LBVec3& start, const LBVec3& end,
                               float stride, int maxSteps)
    {
        StrideWalk w;
        w.start  = start;
        w.stride = stride < 1e-4f ? 1e-4f : stride;

        const float dx = end.x - start.x;
        const float dy = end.y - start.y;
        const float dz = end.z - start.z;
        w.totalDist = std::sqrt(dx*dx + dy*dy + dz*dz);

        if (w.totalDist >= 1e-4f)
        {
            w.dirX = dx / w.totalDist;
            w.dirY = dy / w.totalDist;
            w.dirZ = dz / w.totalDist;
            w.numSteps = (int)(w.totalDist / w.stride);
            if (maxSteps > 0 && w.numSteps > maxSteps)
                w.numSteps = maxSteps;
        }
        // else: numSteps stays 0 → PieceCount() == 1, a single piece at
        // `start`. The caller still gets feedback for a near-zero click.
        return w;
    }

    // ---------- Spawn resolution ----------

    LevelBuilderCoreAPI::LBBrushSpawnFn ResolveSpawn(
        LevelBuilderCoreAPI* api, void** outUserData)
    {
        if (outUserData) *outUserData = nullptr;
        if (!api || !api->GetSpawnFnForActiveTool) return nullptr;
        return api->GetSpawnFnForActiveTool(outUserData);
    }

    // ---------- Preview overlay ----------

#if EDITOR
    int ClampPreviewMarkers(int wanted, bool* outTruncated)
    {
        if (outTruncated) *outTruncated = false;
        if (wanted <= kPreviewMarkersMax) return wanted;
        if (outTruncated) *outTruncated = true;
        return kPreviewMarkersMax;
    }

    void DrawStrideMarkers(PolyphaseEngineAPI* eng,
                           const StrideWalk&   walk,
                           float r, float g, float b, float a)
    {
        if (!eng || !eng->Gizmos_DrawWireSphere || !eng->Gizmos_SetColor) return;
        if (walk.numSteps <= 0) return;

        bool truncated = false;
        const int markers = ClampPreviewMarkers(walk.numSteps, &truncated);

        eng->Gizmos_SetColor(r, g, b, a);
        for (int i = 1; i <= markers; ++i)
        {
            const LBVec3 p = walk.At(i);
            const float radius = (i == walk.numSteps) ? 0.13f : 0.06f;
            eng->Gizmos_DrawWireSphere(p.x, p.y, p.z, radius);
        }
        // Amber "you're getting more pieces than shown" indicator at the
        // last drawn marker.
        if (truncated)
        {
            const LBVec3 p = walk.At(markers);
            eng->Gizmos_SetColor(1.0f, 0.65f, 0.10f, 0.95f);
            eng->Gizmos_DrawWireSphere(p.x, p.y, p.z, 0.13f);
        }
    }
#endif // EDITOR
}
