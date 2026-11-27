#pragma once
// Camera motion blur (IRenderer::MotionBlurSettings): the numbers every backend's velocity pass takes.
#include <glm/glm.hpp>
#include <algorithm>

namespace HE
{
// prevViewProj * inverse(viewProj): carries a pixel's ndc position into the previous frame's clip space. Computed in double: the
// matrices hold the camera's translation (tens of km from the origin in a tile world) and a 250 km far plane, and a float inverse
// there leaves a still camera with more than the half pixel of motion the blur pass ignores. In double a still camera is identity.
inline glm::mat4 motionBlurReproject(const glm::mat4& prevViewProj, const glm::mat4& viewProj)
{
    return glm::mat4(glm::dmat4(prevViewProj) * glm::inverse(glm::dmat4(viewProj)));
}

// The pass's params: x shutter fraction, y max blur in pixels (the setting is at 720p and scales with the height), zw target size.
inline glm::vec4 motionBlurParams(float intensity, float maxBlur, int w, int h)
{
    return glm::vec4(std::max(intensity, 0.0f), std::max(maxBlur, 0.0f) * (static_cast<float>(h) / 720.0f),
                     static_cast<float>(w), static_cast<float>(h));
}
} // namespace HE
