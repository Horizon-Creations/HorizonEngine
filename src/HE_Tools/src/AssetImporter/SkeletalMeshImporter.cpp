#include "SkeletalMeshImporter.h"
#include "TextureImporter.h"
#include "ImporterCommon.h"
#include "Diagnostics/Logger.h"

// cgltf may already be defined via MeshImporter.cpp in the same link unit —
// include the header only here (implementation defined exactly once elsewhere).
#include "cgltf.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <unordered_map>
#include <cstring>

namespace
{

void logError(const std::string& msg)
{
    Logger::Log(Logger::LogLevel::Error, ("SkeletalMeshImporter: " + msg).c_str());
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

        // World transform of this node
        float mat[16];
        cgltf_node_transform_world(&node, mat);
        glm::mat4 world = glm::transpose(glm::make_mat4(mat));

        const glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(world)));

        for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi)
        {
            const cgltf_primitive& prim = node.mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            const cgltf_accessor* posAcc    = nullptr;
            const cgltf_accessor* normAcc   = nullptr;
            const cgltf_accessor* uvAcc     = nullptr;
            const cgltf_accessor* jointsAcc = nullptr;
            const cgltf_accessor* weightsAcc= nullptr;

            for (cgltf_size a = 0; a < prim.attributes_count; ++a)
            {
                const cgltf_attribute& attr = prim.attributes[a];
                switch (attr.type)
                {
                case cgltf_attribute_type_position: posAcc     = attr.data; break;
                case cgltf_attribute_type_normal:   normAcc    = attr.data; break;
                case cgltf_attribute_type_texcoord: if (attr.index == 0) uvAcc = attr.data; break;
                case cgltf_attribute_type_joints:   if (attr.index == 0) jointsAcc  = attr.data; break;
                case cgltf_attribute_type_weights:  if (attr.index == 0) weightsAcc = attr.data; break;
                default: break;
                }
            }
            if (!posAcc) continue;

            const uint32_t baseVertex = static_cast<uint32_t>(mesh->vertices.size() / 3);

            for (cgltf_size v = 0; v < posAcc->count; ++v)
            {
                float p[3] = {};
                cgltf_accessor_read_float(posAcc, v, p, 3);
                glm::vec3 wp = glm::vec3(world * glm::vec4(p[0], p[1], p[2], 1.0f))
                               * settings.uniformScale;
                mesh->vertices.insert(mesh->vertices.end(), { wp.x, wp.y, wp.z });

                if (normAcc && v < normAcc->count) {
                    float n[3] = {};
                    cgltf_accessor_read_float(normAcc, v, n, 3);
                    glm::vec3 wn = glm::normalize(normalMat * glm::vec3(n[0], n[1], n[2]));
                    mesh->normals.insert(mesh->normals.end(), { wn.x, wn.y, wn.z });
                } else {
                    mesh->normals.insert(mesh->normals.end(), { 0.f, 0.f, 0.f });
                }

                if (uvAcc && v < uvAcc->count) {
                    float uv[2] = {};
                    cgltf_accessor_read_float(uvAcc, v, uv, 2);
                    mesh->uvs.insert(mesh->uvs.end(), { uv[0], uv[1] });
                } else {
                    mesh->uvs.insert(mesh->uvs.end(), { 0.f, 0.f });
                }

                // Skinning: 4 joints + 4 weights per vertex
                uint32_t jids[4] = {};
                float    wts[4]  = { 1.0f, 0.0f, 0.0f, 0.0f };
                if (jointsAcc && v < jointsAcc->count)
                {
                    float jf[4] = {};
                    cgltf_accessor_read_float(jointsAcc, v, jf, 4);
                    for (int k = 0; k < 4; ++k)
                        jids[k] = static_cast<uint32_t>(jf[k]);
                }
                if (weightsAcc && v < weightsAcc->count)
                    cgltf_accessor_read_float(weightsAcc, v, wts, 4);

                mesh->boneIDs.insert(mesh->boneIDs.end(), { jids[0], jids[1], jids[2], jids[3] });
                mesh->boneWeights.insert(mesh->boneWeights.end(), { wts[0], wts[1], wts[2], wts[3] });
            }

            if (prim.indices)
            {
                for (cgltf_size ii = 0; ii < prim.indices->count; ++ii)
                    mesh->indices.push_back(baseVertex +
                        static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, ii)));
            }
            else
            {
                for (cgltf_size ii = 0; ii < posAcc->count; ++ii)
                    mesh->indices.push_back(baseVertex + static_cast<uint32_t>(ii));
            }
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
    if (settings.importMaterials && data->materials_count > 0)
    {
        const cgltf_material& mat = data->materials[0];
        if (mat.has_pbr_metallic_roughness && mat.pbr_metallic_roughness.base_color_texture.texture)
        {
            const cgltf_texture* tex = mat.pbr_metallic_roughness.base_color_texture.texture;
            if (tex && tex->image)
            {
                const cgltf_image* img = tex->image;
                const std::string texName = stem + "_basecolor";
                std::string texPath;

                if (img->uri && std::strncmp(img->uri, "data:", 5) != 0)
                {
                    char decoded[1024];
                    std::strncpy(decoded, img->uri, sizeof(decoded) - 1);
                    decoded[sizeof(decoded) - 1] = '\0';
                    cgltf_decode_uri(decoded);
                    auto t = TextureImporter::import(sourcePath.parent_path() / decoded,
                                                     contentRoot, relativeOutputDir);
                    texPath = t ? t->path : std::string{};
                }
                else if (img->buffer_view && img->buffer_view->buffer && img->buffer_view->buffer->data)
                {
                    const auto* bytes = static_cast<const uint8_t*>(img->buffer_view->buffer->data)
                                      + img->buffer_view->offset;
                    auto t = TextureImporter::decodeFromMemory(bytes, img->buffer_view->size);
                    if (t) {
                        t->type = HE::AssetType::Texture; t->name = texName;
                        t->path = Importer::toAssetPath(relativeOutputDir / (texName + ".hasset"));
                        if (Importer::writeAsset(*t, contentRoot)) texPath = t->path;
                    }
                }

                if (!texPath.empty())
                    mesh->materialPath = texPath;
            }
        }
    }

    cgltf_free(data);

    // ── Write asset ─────────────────────────────────────────────────────────────
    mesh->path = Importer::toAssetPath(relativeOutputDir / (stem + "_skeletal.hasset"));
    if (!Importer::writeAsset(*mesh, contentRoot))
        return nullptr;

    return mesh;
}
