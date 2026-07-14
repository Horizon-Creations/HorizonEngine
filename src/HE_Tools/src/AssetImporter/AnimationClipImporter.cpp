#include "AnimationClipImporter.h"
#include <cstdint>
#include "Diagnostics/Logger.h"
#include "cgltf.h"

#include <unordered_map>
#include <algorithm>
#include <cstring>

namespace
{

static void logError(const std::string& msg)
{
    Logger::Log(Logger::LogLevel::Error, ("AnimationClipImporter: " + msg).c_str());
}

// Build joint-node → skin index lookup (first skin only).
static std::unordered_map<const cgltf_node*, uint32_t>
buildJointMap(const cgltf_data& data)
{
    std::unordered_map<const cgltf_node*, uint32_t> m;
    if (data.skins_count == 0) return m;
    const cgltf_skin& skin = data.skins[0];
    for (cgltf_size i = 0; i < skin.joints_count; ++i)
        m[skin.joints[i]] = static_cast<uint32_t>(i);
    return m;
}

// Read all float values from an accessor into a std::vector<float>.
static std::vector<float> readFloats(const cgltf_accessor* acc)
{
    if (!acc) return {};
    const cgltf_size components =
        (acc->type == cgltf_type_vec4) ? 4 :
        (acc->type == cgltf_type_vec3) ? 3 :
        (acc->type == cgltf_type_vec2) ? 2 : 1;
    std::vector<float> out(acc->count * components);
    for (cgltf_size i = 0; i < acc->count; ++i)
        cgltf_accessor_read_float(acc, i, out.data() + i * components,
                                  static_cast<cgltf_size>(components));
    return out;
}

} // namespace

std::vector<std::unique_ptr<AnimationClipAsset>>
AnimationClipImporter::import(const std::filesystem::path& sourcePath)
{
    cgltf_options options{};
    cgltf_data*   data = nullptr;

    cgltf_result res = cgltf_parse_file(&options, sourcePath.string().c_str(), &data);
    if (res != cgltf_result_success)
    {
        logError(sourcePath.string() + ": parse failed");
        return {};
    }
    res = cgltf_load_buffers(&options, data, sourcePath.string().c_str());
    if (res != cgltf_result_success)
    {
        logError(sourcePath.string() + ": buffer load failed");
        cgltf_free(data);
        return {};
    }

    const auto jointMap = buildJointMap(*data);

    std::vector<std::unique_ptr<AnimationClipAsset>> clips;
    clips.reserve(data->animations_count);

    for (cgltf_size ai = 0; ai < data->animations_count; ++ai)
    {
        const cgltf_animation& anim = data->animations[ai];
        auto clip = std::make_unique<AnimationClipAsset>();
        clip->type = HE::AssetType::AnimationClip;
        clip->name = anim.name ? anim.name
                               : (sourcePath.stem().string() + "_anim" + std::to_string(ai));

        float maxTime = 0.0f;

        for (cgltf_size ci = 0; ci < anim.channels_count; ++ci)
        {
            const cgltf_animation_channel& ch  = anim.channels[ci];
            const cgltf_animation_sampler& smp = *ch.sampler;

            // Only support LINEAR interpolation for now; skip STEP/CUBICSPLINE.
            if (smp.interpolation != cgltf_interpolation_type_linear) continue;

            // Require the target node to be a known joint in the first skin.
            if (!ch.target_node) continue;
            auto jit = jointMap.find(ch.target_node);
            if (jit == jointMap.end()) continue;

            AnimPathType path;
            switch (ch.target_path)
            {
            case cgltf_animation_path_type_translation: path = AnimPathType::Translation; break;
            case cgltf_animation_path_type_rotation:    path = AnimPathType::Rotation;    break;
            case cgltf_animation_path_type_scale:       path = AnimPathType::Scale;       break;
            default: continue; // weights etc. not supported
            }

            AnimationChannel channel;
            channel.jointIndex = jit->second;
            channel.path       = path;
            channel.times      = readFloats(smp.input);
            channel.values     = readFloats(smp.output);

            if (!channel.times.empty())
                maxTime = std::max(maxTime, channel.times.back());

            clip->channels.push_back(std::move(channel));
        }

        clip->duration = maxTime;
        clips.push_back(std::move(clip));
    }

    cgltf_free(data);
    return clips;
}
