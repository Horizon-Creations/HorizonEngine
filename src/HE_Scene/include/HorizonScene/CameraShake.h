#pragma once
#include <glm/glm.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

// ── Camera shake and FOV kick ────────────────────────────────────────────────
// The maths, and only the maths. The STATE lives in CameraRigComponent, but
// nothing here knows about a rig, an entity or a world — so the same functions
// can later shake a fly camera or an editor camera without moving house.
//
// Everything is a pure function of (parameters, elapsed): the same shake at the
// same elapsed gives the same offset, on every machine and in every run. That is
// what makes it testable, and it is why there is no random number generator
// anywhere in here.
namespace HE {

// How many shakes one rig can run at once. Fixed, so the component stays
// copyable and allocation-free; a ninth shake replaces the one with the least
// energy left in it.
inline constexpr std::size_t kMaxCameraShakes = 8;

struct ShakeInstance
{
    uint32_t  id = 0;                    // handle for stopping; 0 = free slot

    glm::vec3 posAmplitude{ 0.05f };     // metres, in CAMERA space
    // Degrees, as (pitch, yaw, roll). Roll is in here on purpose: it is the
    // component that makes a hit read as a hit, and the only axis the rig does
    // not otherwise use — rotation.z is 0 on every normal frame.
    glm::vec3 rotAmplitude{ 0.5f };

    float     frequency = 12.0f;         // Hz
    float     duration  = 0.4f;          // s; <= 0 runs until stopped
    float     blendIn   = 0.05f;         // s of fade in
    float     blendOut  = 0.15f;         // s of fade out (ignored if endless)
    float     elapsed   = 0.0f;
    uint32_t  seed      = 0;             // 0 = derive one from the handle
};

// The sum of every live shake on a rig, for one frame.
struct ShakeOffset
{
    glm::vec3 position{ 0.0f };          // metres, CAMERA space
    glm::vec3 rotationDegrees{ 0.0f };   // pitch, yaw, roll
};

// Value noise in [-1, 1]: hash at the integer steps, smoothstep between them.
//
// Not a sine. A sine reads as mechanical wobble with an audible period; an
// explosion does not have one. This costs two hashes and needs no state beyond
// `t`, which is what keeps a shake reproducible.
float shakeNoise(uint32_t seed, float t);

// The fade-in/fade-out envelope of one shake at its current `elapsed`, 0..1.
float shakeEnvelope(const ShakeInstance& s);

// How much is left in a shake — used to decide which one a ninth replaces.
// An endless shake never loses, because nothing else can outlast it.
float shakeRemainingEnergy(const ShakeInstance& s);

// Advance every live shake by `dt` and sum what they contribute.
//
// A shake whose duration has run out frees its slot HERE, before the sum, so a
// finished shake contributes exactly 0.0f and not an epsilon of tail.
ShakeOffset evaluateShakes(ShakeInstance* shakes, std::size_t count, float dt);

inline ShakeOffset evaluateShakes(std::array<ShakeInstance, kMaxCameraShakes>& shakes,
                                  float dt)
{
    return evaluateShakes(shakes.data(), shakes.size(), dt);
}

// ── FOV kick ─────────────────────────────────────────────────────────────────
// The same envelope as a shake without the noise: one attack, one hold, one
// decay. One slot per rig — several kicks at once add up to nonsense in
// practice, so a new one replaces the running one.
struct FovKick
{
    float amplitude = 0.0f;   // degrees; negative zooms IN
    float attack    = 0.05f;  // s
    float hold      = 0.0f;   // s
    float decay     = 0.2f;   // s
    float elapsed   = 0.0f;
    bool  active    = false;
};

// Advance the kick by `dt` and return its current offset in degrees.
// A finished kick frees its slot and returns exactly 0.0f — the camera's
// fovOffset is a difference against fovDegrees, and an epsilon left standing
// there would be a permanently slightly-wrong FOV.
float evaluateFovKick(FovKick& kick, float dt);

} // namespace HE
