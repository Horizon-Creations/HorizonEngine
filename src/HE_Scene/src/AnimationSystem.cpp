#include <HorizonScene/AnimationSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/AnimatorComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{

// Locate the lower bracket index k such that times[k] <= t < times[k+1].
// Returns the last valid index when t is past the final keyframe.
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

// Linear interpolation factor between bracket k and k+1.
static float bracketAlpha(const std::vector<float>& times, size_t k, float t)
{
    if (k + 1 >= times.size()) return 0.0f;
    const float span = times[k + 1] - times[k];
    if (span <= 0.0f) return 0.0f;
    return std::clamp((t - times[k]) / span, 0.0f, 1.0f);
}

} // namespace

void AnimationSystem::update(HorizonWorld& world, ContentManager& cm, float dt)
{
    auto& reg  = world.registry();
    auto  view = reg.view<AnimatorComponent, SkeletalMeshComponent>();

    for (auto [e, animator, smc] : view.each())
    {
        if (!animator.playing) continue;

        const AnimationClipAsset* clip = cm.getAnimationClip(animator.clipAssetId);
        if (!clip || clip->duration <= 0.0f) continue;

        // Advance playback time
        animator.playbackTime += dt * animator.playbackSpeed;
        if (animator.looping)
        {
            animator.playbackTime = std::fmod(animator.playbackTime, clip->duration);
            if (animator.playbackTime < 0.0f)
                animator.playbackTime += clip->duration;
        }
        else
        {
            if (animator.playbackTime >= clip->duration)
            {
                animator.playbackTime = clip->duration;
                animator.playing      = false;
            }
        }

        const float t = animator.playbackTime;

        // Need the skeleton to resolve parent chain and IBM
        const SkeletalMeshAsset* mesh = cm.getSkeletalMesh(smc.meshAssetId);
        if (!mesh || mesh->skeleton.empty()) continue;

        const size_t jointCount = mesh->skeleton.size();

        struct JointTRS
        {
            glm::vec3 translation = glm::vec3(0.0f);
            glm::quat rotation    = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // w,x,y,z identity
            glm::vec3 scale       = glm::vec3(1.0f);
        };
        std::vector<JointTRS> localTRS(jointCount);

        // Sample every channel and accumulate into per-joint TRS
        for (const auto& channel : clip->channels)
        {
            if (channel.jointIndex >= static_cast<uint32_t>(jointCount)) continue;
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
                // glTF stores quaternions as xyzw; glm::quat ctor is (w, x, y, z)
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

        // Forward kinematics: parent index is guaranteed < child index for
        // well-formed glTF skins (spec §5.22 — joints listed in hierarchy order).
        std::vector<glm::mat4> worldMats(jointCount);
        for (size_t i = 0; i < jointCount; ++i)
        {
            const auto& trs   = localTRS[i];
            const glm::mat4 T = glm::translate(glm::mat4(1.0f), trs.translation);
            const glm::mat4 R = glm::mat4_cast(trs.rotation);
            const glm::mat4 S = glm::scale(glm::mat4(1.0f), trs.scale);
            const glm::mat4 local = T * R * S;

            const int32_t parent = mesh->skeleton[i].parent;
            worldMats[i] = (parent < 0) ? local
                                        : worldMats[static_cast<size_t>(parent)] * local;
        }

        // Bone matrix = world joint matrix × inverse bind matrix
        smc.boneMatrices.resize(jointCount);
        for (size_t i = 0; i < jointCount; ++i)
        {
            glm::mat4 ibm;
            std::memcpy(&ibm, mesh->skeleton[i].inverseBindMatrix.data(), sizeof(glm::mat4));
            smc.boneMatrices[i] = worldMats[i] * ibm;
        }
        smc.dirty = true;
    }
}
