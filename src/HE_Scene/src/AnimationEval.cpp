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

} // namespace

void sampleClip(const AnimationClipAsset& clip, float t, std::vector<JointTRS>& localTRS)
{
    for (const auto& channel : clip.channels)
    {
        if (channel.jointIndex >= static_cast<uint32_t>(localTRS.size())) continue;
        if (channel.times.empty()) continue;

        const size_t k     = findBracket(channel.times, t);
        const float  alpha = bracketAlpha(channel.times, k, t);
        auto& trs = localTRS[channel.jointIndex];

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
}

void composeBoneMatrices(const SkeletalMeshAsset&      mesh,
                         const std::vector<JointTRS>& localTRS,
                         std::vector<glm::mat4>&       boneMatrices)
{
    const size_t jointCount = mesh.skeleton.size();
    std::vector<glm::mat4> worldMats(jointCount);
    for (size_t i = 0; i < jointCount; ++i)
    {
        const auto& trs   = (i < localTRS.size()) ? localTRS[i] : JointTRS{};
        const glm::mat4 T = glm::translate(glm::mat4(1.0f), trs.translation);
        const glm::mat4 R = glm::mat4_cast(trs.rotation);
        const glm::mat4 S = glm::scale(glm::mat4(1.0f), trs.scale);
        const glm::mat4 local = T * R * S;
        const int32_t parent = mesh.skeleton[i].parent;
        worldMats[i] = (parent < 0) ? local : worldMats[static_cast<size_t>(parent)] * local;
    }
    boneMatrices.resize(jointCount);
    for (size_t i = 0; i < jointCount; ++i)
    {
        glm::mat4 ibm;
        std::memcpy(&ibm, mesh.skeleton[i].inverseBindMatrix.data(), sizeof(glm::mat4));
        boneMatrices[i] = worldMats[i] * ibm;
    }
}

void blendTRS(const std::vector<JointTRS>& a,
              const std::vector<JointTRS>& b,
              float                        alpha,
              std::vector<JointTRS>&       out)
{
    const size_t count = std::min(a.size(), b.size());
    out.resize(count);
    for (size_t i = 0; i < count; ++i)
    {
        out[i].translation = glm::mix(a[i].translation, b[i].translation, alpha);
        out[i].rotation    = glm::slerp(a[i].rotation,  b[i].rotation,    alpha);
        out[i].scale       = glm::mix(a[i].scale,       b[i].scale,       alpha);
    }
}
