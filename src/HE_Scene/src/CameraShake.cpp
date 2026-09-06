#include "HorizonScene/CameraShake.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace HE {

namespace {

    // A 32-bit integer hash (lowbias32). Cheap, well mixed, and — the part that
    // matters here — identical on every platform, because it is integer
    // arithmetic all the way down. A float-based hash would give a different
    // shake on a different compiler.
    uint32_t hashU32(uint32_t x)
    {
        x ^= x >> 16;
        x *= 0x7feb352dU;
        x ^= x >> 15;
        x *= 0x846ca68bU;
        x ^= x >> 16;
        return x;
    }

    // 24 of those bits, mapped to [-1, 1).
    float hashToSigned(uint32_t x)
    {
        return static_cast<float>(hashU32(x) >> 8) * (1.0f / 8388608.0f) - 1.0f;
    }

} // namespace

float shakeNoise(uint32_t seed, float t)
{
    const float base = std::floor(t);
    const auto  cell = static_cast<int32_t>(base);
    const float frac = t - base;

    // Smoothstep between the two cell values rather than a straight lerp: the
    // linear version has a corner at every integer step, and a corner in a
    // camera offset is a visible tick.
    const float s = frac * frac * (3.0f - 2.0f * frac);

    const float a = hashToSigned(static_cast<uint32_t>(cell)     ^ seed);
    const float b = hashToSigned(static_cast<uint32_t>(cell + 1) ^ seed);
    return a + (b - a) * s;
}

float shakeEnvelope(const ShakeInstance& s)
{
    float env = 1.0f;
    if (s.blendIn > 0.0f)
        env = std::min(env, s.elapsed / s.blendIn);
    // An endless shake has no end to fade towards, so it only ever fades in.
    if (s.duration > 0.0f && s.blendOut > 0.0f)
        env = std::min(env, (s.duration - s.elapsed) / s.blendOut);
    return std::clamp(env, 0.0f, 1.0f);
}

float shakeRemainingEnergy(const ShakeInstance& s)
{
    if (s.id == 0) return 0.0f;
    const float amplitude =
        glm::length(s.posAmplitude) + glm::length(s.rotAmplitude);
    if (s.duration <= 0.0f) return std::numeric_limits<float>::infinity();
    return std::max(0.0f, s.duration - s.elapsed) * amplitude;
}

ShakeOffset evaluateShakes(ShakeInstance* shakes, std::size_t count, float dt)
{
    ShakeOffset out;
    if (!shakes) return out;

    const float step = std::max(0.0f, dt);
    for (std::size_t i = 0; i < count; ++i)
    {
        ShakeInstance& s = shakes[i];
        if (s.id == 0) continue;

        s.elapsed += step;

        // Freed BEFORE it is summed: once a shake is over its contribution has
        // to be exactly zero, not the last epsilon of its fade-out.
        if (s.duration > 0.0f && s.elapsed >= s.duration)
        {
            s = ShakeInstance{};
            continue;
        }

        const float env   = shakeEnvelope(s);
        const float phase = s.elapsed * s.frequency;

        // One noise stream per axis. The offsets keep the three axes from
        // moving in lockstep, which would turn a shake into a diagonal slide.
        out.position += s.posAmplitude * env
            * glm::vec3(shakeNoise(s.seed + 1u, phase),
                        shakeNoise(s.seed + 2u, phase),
                        shakeNoise(s.seed + 3u, phase));
        out.rotationDegrees += s.rotAmplitude * env
            * glm::vec3(shakeNoise(s.seed + 101u, phase),
                        shakeNoise(s.seed + 102u, phase),
                        shakeNoise(s.seed + 103u, phase));
    }
    return out;
}

float evaluateFovKick(FovKick& kick, float dt)
{
    if (!kick.active) return 0.0f;

    kick.elapsed += std::max(0.0f, dt);

    const float attack = std::max(0.0f, kick.attack);
    const float hold   = std::max(0.0f, kick.hold);
    const float decay  = std::max(0.0f, kick.decay);
    const float total  = attack + hold + decay;

    if (kick.elapsed >= total)
    {
        kick = FovKick{};
        return 0.0f;
    }

    float env;
    if (kick.elapsed < attack)             env = kick.elapsed / attack;
    else if (kick.elapsed < attack + hold) env = 1.0f;
    else                                   env = 1.0f - (kick.elapsed - attack - hold) / decay;

    return kick.amplitude * std::clamp(env, 0.0f, 1.0f);
}

} // namespace HE
