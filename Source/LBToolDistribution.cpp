#include "LBToolDistribution.h"

#include <cmath>
#include <cstdint>

namespace LBToolDistribution
{
    // ---- RNG (xorshift32 — fast, decent quality for jitter use) ----

    Rng SeedRng(uint32_t seed)
    {
        // Avoid the all-zero degenerate state.
        return Rng{ seed ? seed : 0x12345678u };
    }

    static inline uint32_t NextU32(Rng& r)
    {
        uint32_t x = r.state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        r.state = x;
        return x;
    }

    float NextFloat01(Rng& r)
    {
        // 24-bit mantissa → uniform [0, 1).
        return (NextU32(r) >> 8) * (1.0f / 16777216.0f);
    }

    float NextFloatRange(Rng& r, float a, float b)
    {
        return a + (b - a) * NextFloat01(r);
    }

    // ---- Sampling ----

    LBVec3 RandomInDisc(const LBVec3& center, float radius, Rng& r)
    {
        // sqrt() on the radius makes the distribution uniform in area
        // instead of biased toward the center.
        const float t   = NextFloat01(r);
        const float u   = NextFloat01(r);
        const float ang = 6.2831853071795864769f * t;
        const float rd  = radius * std::sqrt(u);
        return LBVec3{
            center.x + rd * std::cos(ang),
            center.y,
            center.z + rd * std::sin(ang),
        };
    }

    LBVec3 RandomInRect(const LBVec3& minXZ, const LBVec3& maxXZ, float y, Rng& r)
    {
        return LBVec3{
            NextFloatRange(r, minXZ.x, maxXZ.x),
            y,
            NextFloatRange(r, minXZ.z, maxXZ.z),
        };
    }

    // ---- 2D value-noise (no external deps) ----
    //
    // Hash each integer grid corner, bilerp between four corners using a
    // cubic smoothstep. ~30 LOC, plenty good enough for organic scatter
    // weighting. If you need higher-quality Perlin / simplex later, drop
    // in stb_perlin.h.

    static inline uint32_t HashCoord(int32_t x, int32_t z, uint32_t seed)
    {
        uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u + seed * 2147483647u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return h ^ (h >> 16);
    }

    static inline float HashFloat(int32_t x, int32_t z, uint32_t seed)
    {
        return (HashCoord(x, z, seed) >> 8) * (1.0f / 16777216.0f);
    }

    static inline float Smoothstep(float t)
    {
        return t * t * (3.0f - 2.0f * t);
    }

    float Noise2D(float x, float z, float scale, uint32_t seed)
    {
        if (scale <= 0.0f) scale = 1.0f;
        x /= scale;
        z /= scale;

        const int32_t xi = (int32_t)std::floor(x);
        const int32_t zi = (int32_t)std::floor(z);
        const float   tx = Smoothstep(x - (float)xi);
        const float   tz = Smoothstep(z - (float)zi);

        const float a = HashFloat(xi,     zi,     seed);
        const float b = HashFloat(xi + 1, zi,     seed);
        const float c = HashFloat(xi,     zi + 1, seed);
        const float d = HashFloat(xi + 1, zi + 1, seed);

        const float ab = a + (b - a) * tx;
        const float cd = c + (d - c) * tx;
        return ab + (cd - ab) * tz;
    }

    // ---- Yaw jitter ----

    LBQuat JitteredYaw(const LBQuat& base, float jitterDeg, Rng& r)
    {
        if (jitterDeg <= 0.0f) return base;
        const float yaw = NextFloatRange(r, -jitterDeg, jitterDeg);
        const float DEG2RAD = 3.14159265358979323846f / 180.0f;
        const float h = yaw * DEG2RAD * 0.5f;
        const float s = std::sin(h);
        const float c = std::cos(h);
        // Compose: out = base * yawQuat (apply jitter in local space).
        const LBQuat yq{ 0.0f, s, 0.0f, c };
        LBQuat out;
        out.w = base.w*yq.w - base.x*yq.x - base.y*yq.y - base.z*yq.z;
        out.x = base.w*yq.x + base.x*yq.w + base.y*yq.z - base.z*yq.y;
        out.y = base.w*yq.y - base.x*yq.z + base.y*yq.w + base.z*yq.x;
        out.z = base.w*yq.z + base.x*yq.y - base.y*yq.x + base.z*yq.w;
        return out;
    }
}
