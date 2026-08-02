#include "SkeletalMeshImporter.h"
#include <algorithm>
#include <cstdint>
#include "ImporterCommon.h"
#include "Diagnostics/Logger.h"

// cgltf may already be defined via MeshImporter.cpp in the same link unit —
// include the header only here (implementation defined exactly once elsewhere).
#include "cgltf.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <unordered_map>

namespace
{

void logError(const std::string& msg)
{
    HE_LOG_ERROR(Tool, "%s", ("SkeletalMeshImporter: " + msg).c_str());
}

// Build the joint-index lookup: cgltf_node* → index in skin.joints[]
static std::unordered_map<const cgltf_node*, int32_t>
buildJointMap(const cgltf_skin& skin)
{
    std::unordered_map<const cgltf_node*, int32_t> m;
    for (cgltf_size i = 0; i < skin.joints_count; ++i)
        m[skin.joints[i]] = static_cast<int32_t>(i);
    return m;
}

// Find the parent index of joint[i]: the joint whose cgltf_node contains
// joints[i] as a direct child.
static int32_t findParent(const cgltf_skin& skin, cgltf_size jointIdx,
                           const std::unordered_map<const cgltf_node*, int32_t>& map)
{
    const cgltf_node* target = skin.joints[jointIdx];
    for (cgltf_size j = 0; j < skin.joints_count; ++j)
    {
        if (j == jointIdx) continue;
        const cgltf_node* node = skin.joints[j];
        for (cgltf_size c = 0; c < node->children_count; ++c)
            if (node->children[c] == target) return static_cast<int32_t>(j);
    }
    return -1; // root
}

} // namespace

std::unique_ptr<SkeletalMeshAsset> SkeletalMeshImporter::import(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& contentRoot,
    const std::filesystem::path& relativeOutputDir,
    const ImportSettings&        settings)
{
    cgltf_options options{};
    cgltf_data*   data = nullptr;

    cgltf_result res = cgltf_parse_file(&options, sourcePath.string().c_str(), &data);
    if (res != cgltf_result_success)
    {
        logError(sourcePath.string() + ": parse failed");
        return nullptr;
    }
    res = cgltf_load_buffers(&options, data, sourcePath.string().c_str());
    if (res != cgltf_result_success)
    {
        logError(sourcePath.string() + ": buffer load failed");
        cgltf_free(data);
        return nullptr;
    }

    auto mesh = std::make_unique<SkeletalMeshAsset>();
    mesh->type = HE::AssetType::SkeletalMesh;
    const std::string stem = sourcePath.stem().string();
    mesh->name = stem;

    // ── Geometry ────────────────────────────────────────────────────────────────
    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
    {
        const cgltf_node& node = data->nodes[ni];
        if (!node.mesh) continue;

        // Skinned nodes: the skin matrices handle world placement; baking the node
        // transform into vertices would corrupt the IBM-based skinning math.
        //
        // NEVER transpose here: cgltf_node_transform_world writes a COLUMN-major
        // float[16] (translation lands in mat[12..14], exactly as the glTF spec
        // stores node.matrix), and glm::make_mat4 is a raw memcpy into glm's
        // column-major mat4 — so it consumes that layout directly. An earlier
        // glm::transpose() here silently dropped the translation (the transposed
        // 4th column became (0,0,0,1)) and inverted the rotation of every
        // non-skinned node inside a skeleton file. MeshImporter.cpp gets this right.
        float mat[16];
        cgltf_node_transform_world(&node, mat);
        glm::mat4 world = (node.skin != nullptr)
            ? glm::mat4(1.0f)
            : glm::make_mat4(mat);

        Importer::MeshVertexStreams streams{ mesh->vertices, mesh->normals, mesh->uvs };

        for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi)
        {
            // Positions/normals/UVs (including the glTF→engine V flip) are the
            // static importer's code path; only the skin streams are extra here.
            const auto attrs = Importer::appendPrimitive(
                node.mesh->primitives[pi], world, settings.uniformScale,
                streams, mesh->indices);
            if (!attrs.position) continue;

            Importer::appendSkinning(attrs, mesh->boneIDs, mesh->boneWeights);
        }
    }

    if (mesh->vertices.empty())
    {
        logError(sourcePath.string() + ": no geometry found");
        cgltf_free(data);
        return nullptr;
    }

    // ── Skeleton ────────────────────────────────────────────────────────────────
    if (data->skins_count > 0)
    {
        const cgltf_skin& skin   = data->skins[0];
        auto              jmap   = buildJointMap(skin);

        mesh->skeleton.resize(skin.joints_count);
        for (cgltf_size ji = 0; ji < skin.joints_count; ++ji)
        {
            SkeletonJoint& out = mesh->skeleton[ji];
            out.name   = skin.joints[ji]->name ? skin.joints[ji]->name : ("joint_" + std::to_string(ji));
            out.parent = findParent(skin, ji, jmap);

            // Read inverse bind matrix (column-major float[16])
            if (skin.inverse_bind_matrices && ji < skin.inverse_bind_matrices->count)
            {
                cgltf_accessor_read_float(skin.inverse_bind_matrices, ji,
                                          out.inverseBindMatrix.data(), 16);
            }
            else
            {
                // Identity
                std::fill(out.inverseBindMatrix.begin(), out.inverseBindMatrix.end(), 0.0f);
                out.inverseBindMatrix[0] = out.inverseBindMatrix[5] =
                out.inverseBindMatrix[10] = out.inverseBindMatrix[15] = 1.0f;
            }
        }
    }

    // ── Material ────────────────────────────────────────────────────────────────
    // `materialPath` lands in chunk MREF, which every renderer resolves as a
    // MATERIAL reference (ContentManager::resolveMaterialRef). This used to store
    // the base-color TEXTURE path directly, so the lookup always came back empty
    // and skinned meshes rendered untextured; import the same texture+material
    // pair the static MeshImporter writes instead.
    if (settings.importMaterials)
        mesh->materialPath = Importer::importBaseColorMaterial(
            data, sourcePath, contentRoot, relativeOutputDir, stem);

    cgltf_free(data);

    // ── Write asset ─────────────────────────────────────────────────────────────
    mesh->path = Importer::toAssetPath(relativeOutputDir / (stem + "_skeletal.hasset"));
    if (!Importer::writeAsset(*mesh, contentRoot))
        return nullptr;

    return mesh;
}
