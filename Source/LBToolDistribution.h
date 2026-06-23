/**
 * @file LBToolDistribution.h
 * @brief Random-distribution helpers shared by the Paint (T6) and
 *        NoiseFill (T3) brushes.
 *
 * - 32-bit PCG-style RNG so brush output is deterministic per seed.
 * - Uniform-disc / uniform-rect sampling for stamping.
 * - 2D value-noise (no external deps; ~30 LOC) for organic scatter.
 * - Yaw jitter composed on top of a base orientation.
 *
 * No ABI exposure — these are tool.core-internal helpers.
 */

#pragma once

#include "LevelBuilderCoreAPI.h"

#include <cstdint>

namespace LBToolDistribution
{
    struct Rng { uint32_t state; };

    Rng    SeedRng(uint32_t seed);
    float  NextFloat01(Rng& r);                    // [0, 1)
    float  NextFloatRange(Rng& r, float a, float b);

    // Uniform random position inside a horizontal disc on the XZ plane.
    LBVec3 RandomInDisc(const LBVec3& center, float radius, Rng& r);

    // Uniform random position inside a horizontal rectangle on XZ.
    LBVec3 RandomInRect(const LBVec3& minXZ, const LBVec3& maxXZ, float y, Rng& r);

    // 2D value-noise in [0, 1]. `scale` controls feature size (larger →
    // coarser). `seed` lets the artist reroll without changing scale.
    float  Noise2D(float x, float z, float scale, uint32_t seed);

    // Compose a Y-axis yaw jitter on top of `base`. `jitterDeg` of 0
    // returns `base` unchanged.
    LBQuat JitteredYaw(const LBQuat& base, float jitterDeg, Rng& r);
}
