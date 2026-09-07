#include "AnimationEval.h"
#include <cstdint>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{

static size_t findBracket(const std::vector<float>& times, float t)
{
    size_t k = 0;
    for (size_t i = 0; i + 1 < times.size(); ++i)
    {
        if (times[i + 1] >= t) { k = i; return k; }
        k = i + 1;
    }
    return k;
}

static float bracketAlpha(const std::vector<float>& times, size_t k, float t)
{
    if (k + 1 >= times.size()) return 0.0f;
    const float span = times[k + 1] - times[k];
    if (span <= 0.0f) return 0.0f;
    return std::clamp((t - times[k]) / span, 0.0f, 1.0f);
}

// One channel, one time, into one joint's TRS. Pulled out of sampleClip so
// sampleJoint below evaluates exactly the same way — a second copy of the
// keyframe interpolation is a drift waiting to happen.
static void applyChannel(const AnimationChannel& channel, float t, JointTRS& trs)
{
    if (channel.times.empty()) return;

    const size_t k     = findBracket(channel.times, t);
    const float  alpha = bracketAlpha(channel.times, k, t);

    if (channel.path == AnimPathType::Translation)
    {
        glm::vec3 v0(channel.values[k * 3 + 0], channel.values[k * 3 + 1], channel.values[k * 3 + 2]);
        glm::vec3 v1 = (k + 1 < channel.times.size())
            ? glm::vec3(channel.values[(k + 1) * 3 + 0],
                        channel.values[(k + 1) * 3 + 1],
                        channel.values[(k + 1) * 3 + 2])
            : v0;
        trs.translation = glm::mix(v0, v1, alpha);
    }
    else if (channel.path == AnimPathType::Rotation)
    {
        // glTF stores xyzw; glm::quat ctor is (w, x, y, z)
        glm::quat q0(channel.values[k * 4 + 3],
                     channel.values[k * 4 + 0],
                     channel.values[k * 4 + 1],
                     channel.values[k * 4 + 2]);
        glm::quat q1 = (k + 1 < channel.times.size())
            ? glm::quat(channel.values[(k + 1) * 4 + 3],
                        channel.values[(k + 1) * 4 + 0],
                        channel.values[(k + 1) * 4 + 1],
                        channel.values[(k + 1) * 4 + 2])
            : q0;
        trs.rotation = glm::slerp(q0, q1, alpha);
    }
    else if (channel.path == AnimPathType::Scale)
    {
        glm::vec3 v0(channel.values[k * 3 + 0], channel.values[k * 3 + 1], channel.values[k * 3 + 2]);
        glm::vec3 v1 = (k + 1 < channel.times.size())
            ? glm::vec3(channel.values[(k + 1) * 3 + 0],
                        channel.values[(k + 1) * 3 + 1],
                        channel.values[(k + 1) * 3 + 2])
            : v0;
        trs.scale = glm::mix(v0, v1, alpha);
    }
}

} // namespace

void advancePlayback(float& playbackTime, bool& playing,
                     float playbackSpeed, bool looping, float duration, float dt)
{
    playbackTime += dt * playbackSpeed;
    if (looping)
    {
        playbackTime = std::fmod(playbackTime, duration);
        if (playbackTime < 0.0f)
            playbackTime += duration;
    }
    else
    {
        if (playbackTime >= duration)
        {
            playbackTime = duration;
            playing      = false;
        }
    }
}

JointTRS sampleJoint(const AnimationClipAsset& clip, uint32_t jointIndex, float t)
{
    JointTRS trs;
    for (const auto& channel : clip.channels)
        if (channel.jointIndex == jointIndex)
            applyChannel(channel, t, trs);
    return trs;
}

void sampleClip(const AnimationClipAsset& clip, float t, std::vector<JointTRS>& localTRS)
{
    for (const auto& channel : clip.channels)
    {
        if (channel.jointIndex >= static_cast<uint32_t>(localTRS.size())) continue;
        applyChannel(channel, t, localTRS[channel.jointIndex]);
    }
}

void composeBoneMatrices(const SkeletalMeshAsset&      mesh,
                         const std::vector<JointTRS>& localTRS,
                         std::vector<glm::mat4>&       boneMatrices)
{
    // The two halves, in a row. Not one loop any more, and deliberately not a
    // second implementation of either: a driver with IK runs composeModelMatrices,
    // solves in the model matrices it just got, and finishes with applyInverseBind
    // — and a driver WITHOUT IK has to end up at the same numbers, bit for bit,
    // or "no IK component changes nothing" would only be true to a tolerance.
    std::vector<glm::mat4> model;
    composeModelMatrices(mesh, localTRS, model);
    applyInverseBind(mesh, model, boneMatrices);
}

// blendTRS now lives in AnimationPose.cpp — public, because the layer stage, the
// editor preview and the tests all blend poses and none of them can reach an
// internal header.
