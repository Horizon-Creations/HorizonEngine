#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// The pose a camera rig produces, and the curve a blend between two of them
// follows. Its own header because BOTH sides need it: CameraRigController
// solves into it, and CameraRigComponent stores one (the frozen source of a
// re-blend). Leaving it in the controller header would make the component
// include the controller that includes the component.
namespace HE {

// What a rig worked out for this frame — an intermediate VALUE, never state.
//
// The rig solves into one of these and only then writes the camera's transform,
// because lag, shake and blending all need the pose before it lands: a blend
// interpolates against the source rig's pose, and a shake is an additive offset
// that would accumulate into infinity if it were written and read back.
//
// ── The law ──────────────────────────────────────────────────────────────────
// The camera's TransformComponent is pure OUTPUT. The rig never reads it back.
// Everything the rig carries across frames lives in CameraRigComponent — so a
// gizmo drag, a script's setPosition or an undo snapshot cannot become rig
// state.
struct SolvedPose
{
    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };

    // The same rotation as the angles the rig actually holds, carried alongside
    // the quaternion rather than recovered from it. glm::eulerAngles picks an
    // equivalent-but-different triple (yaw −179° comes back as +181° with pitch
    // and roll flipped by 180°), and the camera's rotation is authored data that
    // the inspector shows — it has to survive as the numbers the rig set.
    // Blending slerps `rotation`; a rig that is not blending writes these.
    glm::vec3 eulerDegrees{ 0.0f };

    float     fovDegrees = 0.0f;     // the camera's base FOV plus its FOV kick
    bool      occluded   = false;    // the boom was shortened by geometry
    bool      valid      = false;    // the rig had a target and could be solved
};

// How a blend between two rigs is paced.
//
// SmoothStep is the default: a linear blend has a visible kink at both ends,
// where the camera starts and stops moving instantly.
enum class BlendCurve
{
    Linear,
    SmoothStep,
    EaseOut,
};

// Shape a normalised 0..1 progress with one of the curves above.
inline float applyBlendCurve(float t, BlendCurve curve)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    switch (curve)
    {
        case BlendCurve::Linear:     return t;
        case BlendCurve::EaseOut:    return 1.0f - (1.0f - t) * (1.0f - t);
        case BlendCurve::SmoothStep: break;
    }
    return t * t * (3.0f - 2.0f * t);
}

} // namespace HE
