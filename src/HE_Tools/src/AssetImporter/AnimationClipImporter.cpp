#include "AnimationClipImporter.h"
#include <cstdint>
#include "ImporterCommon.h"
#include "Diagnostics/Logger.h"
#include "cgltf.h"

#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace
{

static void logError(const std::string& msg)
{
    HE_LOG_ERROR(Tool, "%s", ("AnimationClipImporter: " + msg).c_str());
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

// glTF animation names are free-form text and end up as a file name — keep only
// characters that are safe on every target file system, so an artist-named clip
// ("Run / Fast") cannot produce an unwritable path or escape the output folder.
static std::string sanitizeFileStem(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name)
        out.push_back((std::isalnum(c) || c == '_' || c == '-') ? static_cast<char>(c) : '_');
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? std::string("clip") : out;
}

// The clip's asset name: the glTF's own animation name, or a deterministic
// fallback for unnamed animations.
static std::string clipName(const cgltf_animation& anim, cgltf_size index,
                            const std::filesystem::path& sourcePath)
{
    return anim.name ? std::string(anim.name)
                     : (sourcePath.stem().string() + "_anim" + std::to_string(index));
}

// The file stem one clip is written under: sanitised, prefixed with the glTF's
// own stem, and made unique against the stems already handed out for this file.
// importAndWrite() and outputPaths() must agree exactly, so both come here.
static std::string clipFileStem(const std::string& name, const std::string& prefix,
                                std::unordered_set<std::string>& usedStems)
{
    // import() already prefixes unnamed clips with the source stem; only add
    // the prefix when the glTF supplied its own name, so two characters that
    // both export a "Run" animation do not fight over one file.
    std::string stem = sanitizeFileStem(name);
    if (stem.rfind(prefix, 0) != 0)
        stem = prefix + stem;

    // Two animations may legitimately carry the same name inside one glTF.
    std::string unique = stem;
    for (int n = 2; !usedStems.insert(unique).second; ++n)
        unique = stem + "_" + std::to_string(n);
    return unique;
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
        clip->name = clipName(anim, ai, sourcePath);

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

AnimationClipImporter::WriteResult AnimationClipImporter::importAndWrite(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& contentRoot,
    const std::filesystem::path& relativeOutputDir)
{
    WriteResult result;

    const std::string  gltfStem = sanitizeFileStem(sourcePath.stem().string());
    const std::string  prefix   = gltfStem + "_";
    std::unordered_set<std::string> usedStems;

    for (auto& clip : import(sourcePath))
    {
        if (!clip) continue;

        const std::string unique = clipFileStem(clip->name, prefix, usedStems);

        clip->path = Importer::toAssetPath(relativeOutputDir / (unique + ".hasset"));
        if (Importer::writeAsset(*clip, contentRoot))
        {
            ++result.written;
            HE_LOG_INFO(Tool, "%s",
                ("AnimationClipImporter: " + sourcePath.filename().string() + " -> "
                 + clip->path + " (" + std::to_string(clip->channels.size())
                 + " channels, " + std::to_string(clip->duration) + "s)").c_str());
        }
        else
        {
            ++result.failed;
            logError("failed to write " + clip->path);
        }
    }
    return result;
}

std::vector<std::string> AnimationClipImporter::outputPaths(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& relativeOutputDir)
{
    std::vector<std::string> paths;

    cgltf_options options{};
    cgltf_data*   data = nullptr;
    // JSON only — animations_count and the animation names are filled in without
    // touching the (potentially large) .bin buffers. A file that does not parse
    // answers "no outputs"; the import itself then reports the real error.
    if (cgltf_parse_file(&options, sourcePath.string().c_str(), &data) != cgltf_result_success)
        return paths;

    const std::string  gltfStem = sanitizeFileStem(sourcePath.stem().string());
    const std::string  prefix   = gltfStem + "_";
    std::unordered_set<std::string> usedStems;

    paths.reserve(data->animations_count);
    for (cgltf_size ai = 0; ai < data->animations_count; ++ai)
    {
        const std::string unique =
            clipFileStem(clipName(data->animations[ai], ai, sourcePath), prefix, usedStems);
        paths.push_back(Importer::toAssetPath(relativeOutputDir / (unique + ".hasset")));
    }

    cgltf_free(data);
    return paths;
}
