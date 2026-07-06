#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include "Types/Enums.h"
#include "Types/UUID.h"
#include "Scripting/ScriptTypes.h"

// One precompiled material shader variant for a specific graphics backend (baked at
// export time so the shipped game never cross-compiles). `vertex`/`fragment` hold the
// backend-native source (MSL / HLSL / desktop-GLSL text) or, for Vulkan, raw SPIR-V bytes.
struct MaterialShaderVariant
{
	uint8_t     backend = 0; // HE::RendererBackend
	std::string vertex;
	std::string fragment;
};

namespace HE
{
	// PSHD chunk (de)serialization — the single source of truth for the precompiled-
	// shader byte layout, shared by the exporter (encode) and the runtime (decode).
	std::vector<uint8_t> encodeMaterialShaderVariants(const std::vector<MaterialShaderVariant>& v);
	std::vector<MaterialShaderVariant> decodeMaterialShaderVariants(const std::vector<uint8_t>& bytes);
}

struct ContentAsset
{
	// Stable identity — written into the META chunk on save and reused on
	// load, so scene references (e.g. MeshComponent::meshAssetId) survive
	// engine restarts. Never regenerate for an asset that already has one.
	HE::UUID      id;
	std::string   name;
	std::string   path;
	HE::AssetType type = HE::AssetType::Unknown;
};

struct RuntimeAsset : public ContentAsset {};

struct StaticMeshAsset : public RuntimeAsset
{
	std::string            materialPath;
	HE::UUID               materialId;   // pack-time baked from materialPath; {} for loose/editor assets
	std::vector<float>     vertices;
	std::vector<uint32_t>  indices;
	std::vector<float>     normals;
	std::vector<float>     uvs;

	// Pack-time COOKED GPU-ready form (CHUNK_MVBO). When `cooked` is true,
	// `interleaved` holds vertexCount*8 floats (pos3+norm3+uv2 — the exact layout
	// both backends upload) and boundsMin/Max hold the precomputed AABB; the SoA
	// arrays above are then empty. Loose/editor assets are not cooked (SoA path).
	bool                   cooked      = false;
	std::vector<float>     interleaved;            // vertexCount * 8
	uint32_t               vertexCount = 0;
	float                  boundsMin[3] = { 0.0f, 0.0f, 0.0f };
	float                  boundsMax[3] = { 0.0f, 0.0f, 0.0f };
};

// One joint in the skeleton hierarchy.
// inverseBindMatrix is column-major (16 floats, glm::mat4 layout).
struct SkeletonJoint
{
	std::string            name;
	int32_t                parent          = -1; // -1 = root
	std::array<float, 16>  inverseBindMatrix = {};
};

struct SkeletalMeshAsset : public RuntimeAsset
{
	std::string                 materialPath;
	HE::UUID                    materialId;   // pack-time baked; {} for loose/editor assets
	std::vector<float>          vertices;
	std::vector<uint32_t>       indices;
	std::vector<float>          normals;
	std::vector<float>          uvs;
	std::vector<uint32_t>       boneIDs;      // 4 joints per vertex, flat
	std::vector<float>          boneWeights;  // 4 weights per vertex, flat
	std::vector<SkeletonJoint>  skeleton;     // joint hierarchy
};

struct MaterialAsset : public RuntimeAsset
{
	std::string                  shaderPath;
	std::vector<std::string>     texturePaths;
	// Pack-time baked UUID equivalents (index-parallel to texturePaths). Empty for
	// loose/editor assets. Populated from the MTLU chunk when a packed asset is read.
	HE::UUID                     shaderId;
	std::vector<HE::UUID>        textureIds;

	// PBR scalars (metallic-roughness workflow). baseColor multiplies the
	// albedo (texture or flat); metallic/roughness drive the lighting. Appended
	// to the MTRL chunk after the texture list — materials written before these
	// existed load with the defaults.
	float baseColor[3] = { 1.0f, 1.0f, 1.0f };
	float metallic     = 0.0f;
	float roughness    = 0.5f;
	// Surface opacity (1 = fully opaque). Below 1 the object is drawn in the
	// sorted, alpha-blended transparency pass instead of the opaque pass.
	// Appended after the PBR scalars (same backward-compatible MTRL tail).
	float opacity      = 1.0f;
	// Disable backface culling for this material. Needed for terrain/quads
	// that must be visible from below (e.g. at grazing camera angles).
	bool doubleSided   = false;

	// Material-system M1/M3: canonical-GLSL fragment source (Vulkan semantics).
	// Empty → the material renders with the engine's built-in PBR uber-shader. When set,
	// the renderer cross-compiles it (glslang→SPIR-V→SPIRV-Cross), caches the resulting
	// pipeline per source hash, and selects it for this material's draws. Interface:
	//   layout(location=0) in vec3 vNormal;  layout(location=1) in vec3 vColor;
	//   layout(location=2) in vec2 vUV;      layout(location=0) out vec4 oColor;
	// With a node graph (below) this is the GENERATED artifact — the runtime only ever
	// reads this string; hand-written GLSL remains the escape hatch when no graph exists.
	std::string customShaderFragGlsl;

	// Material-system M3: the node graph (JSON, see HE::MaterialGraph) — the SOURCE OF
	// TRUTH the material editor edits. The editor regenerates customShaderFragGlsl from
	// it on every change; shaders are not a user-facing asset type.
	std::string nodeGraphJson;

	// Blend mode baked from the graph's Output node (HE::MatBlendMode as uint8):
	// 0 Opaque, 1 Masked (shader discards — stays in the opaque pass), 2 Translucent
	// (routed into the sorted alpha-blend pass regardless of the scalar opacity).
	uint8_t blendMode = 0;

	// World-Position-Offset vertex BODY (canonical GLSL statements, ends in `vec3 heWpo`),
	// generated when the graph's WPO pin is connected. Empty → the standard shared vertex.
	// The renderers wrap it per backend (MaterialShaderLibrary::customVertex) and key the
	// pipeline on fragment+vertex together.
	std::string customShaderVertGlsl;

	// Exposed graph parameters (Param nodes), 4 floats per HeParams UBO slot, in slot
	// order. Generated alongside customShaderFragGlsl; the renderer uploads this per
	// material — editing a parameter VALUE never recompiles the shader.
	std::vector<float> shaderParamData;

	// Parameter names in slot order (parallel to shaderParamData's vec4 slots) — lets
	// runtime code / scripts set a parameter BY NAME (setMaterialParam). Generated with
	// shaderParamData from the graph's Param nodes. Empty for hand-written shaders.
	std::vector<std::string> graphParamNames;

	// Parameter widget kinds (HE::MatParamKind as uint8) in slot order, parallel to
	// graphParamNames — lets typed editors (central panel, entity Details) render the
	// right widget (color picker / float / vec2 / vec4 / bool) without the node graph.
	std::vector<uint8_t> graphParamTypes;

	// Parameter METADATA in slot order, parallel to graphParamNames (all appended to the
	// MTRL tail — older materials load with empty vectors → plain widgets, no groups):
	// - graphParamMinMax: 2 floats per slot (min, max); min < max → slider UI.
	// - graphParamGroups/Tooltips: panel group header ("" = ungrouped) + hover help.
	std::vector<float>       graphParamMinMax;
	std::vector<std::string> graphParamGroups;
	std::vector<std::string> graphParamTooltips;

	// ── Material INSTANCE ("ein Master-Material, viele Varianten") ─────────────────
	// Non-empty parentMaterialPath marks this asset as an instance of that material: it
	// has NO graph of its own — ContentManager::syncMaterialInstance copies the parent's
	// generated shader (same source hash → the SAME cached pipeline, zero recompiles) and
	// re-applies this instance's overrides. instanceOverriddenParams lists BY NAME which
	// param slots keep the instance's own value; everything else follows the parent.
	// Static-switch overrides bake a different PERMUTATION: sync regenerates the shader
	// from the parent's graph with the override map (own hash → own cached pipeline).
	std::string              parentMaterialPath;
	std::vector<std::string> instanceOverriddenParams;
	std::vector<std::string> instanceSwitchNames;   // switch overrides: name…
	std::vector<uint8_t>     instanceSwitchValues;  // …+ value 0/1 (parallel)

	// Project textures the node graph's Texture Sample nodes reference, in slot order
	// (heTexP0..). Loose assets keep paths; packing bakes them to graphTextureIds (MTLU).
	std::vector<std::string> graphTexturePaths;
	std::vector<HE::UUID>    graphTextureIds;

	// Precompiled per-backend shaders baked into the .hpak at export time (CHUNK_PSHD).
	// Empty for loose editor assets → the renderer cross-compiles customShaderFragGlsl at
	// runtime; when present, the renderer uses the variant for the active backend directly
	// (no glslang/SPIRV-Cross at runtime). The exporter fills only the chosen backends.
	std::vector<MaterialShaderVariant> precompiledShaders;
};

// A reusable material sub-graph (node editor "Material Function"): its FnInput/FnOutput
// nodes are the interface; a material's FunctionCall node inlines it at codegen time.
struct MaterialFunctionAsset : public RuntimeAsset
{
	std::string nodeGraphJson;
};

struct SceneAsset : public RuntimeAsset
{
	std::vector<std::string> objectPaths;
	std::vector<HE::UUID>    objectIds;   // pack-time baked, index-parallel to objectPaths; empty for loose
};

struct ScriptAsset : public RuntimeAsset
{
	std::string    sourceCode;
	// Scripting language of sourceCode. Lua by default so pre-existing .hasset
	// files (which carry no language chunk) keep loading as Lua. Persisted as a
	// 1-byte CHUNK_SLNG; the asset is the single source of truth for language.
	ScriptLanguage language = ScriptLanguage::Lua;
};

struct AudioAsset : public RuntimeAsset
{
	std::vector<uint8_t> audioData;
	int                  sampleRate = 0;
	int                  channels   = 0;
};

struct FontAsset : public RuntimeAsset
{
	std::vector<uint8_t> fontData;
	int                  size = 0;
};

// GPU pixel format of a TextureAsset's `data`. RGBA8 is the raw/editor form;
// ASTC_4x4 is the pack-time cook target for GPUs that sample it (Apple-Silicon
// Metal). Each ASTC 4x4 block is 16 bytes; a level's byte size is
// ceil(w/4)*ceil(h/4)*16, levels concatenated in `data` like RGBA8 mips.
enum class TextureFormat : uint8_t { RGBA8 = 0, ASTC_4x4 = 1 };

struct TextureAsset : public RuntimeAsset
{
	// Pixel bytes. For mipLevels > 1 the levels are concatenated (level 0
	// full-resolution first, then each halved level). Every backend reads level
	// 0 as the leading width*height*channels bytes, so appended mips are
	// backward-compatible (ignored by consumers that don't sample them).
	std::vector<uint8_t> data;     // raw pixel bytes (RGBA8 unless channels says otherwise)
	size_t               width    = 0;
	size_t               height   = 0;
	size_t               channels = 0;

	// Pack-time cook metadata (CHUNK_TXMI tail; absent → the defaults below, so
	// old assets keep loading unchanged).
	uint32_t             mipLevels = 1;                       // levels stored in `data`
	TextureFormat        format    = TextureFormat::RGBA8;
	bool                 srgb      = false;                   // sample as sRGB (color) vs linear (data)
};

struct ShaderAsset : public RuntimeAsset
{
	std::string sourceCode;
};

// A CBOR-encoded snapshot of an entity subtree (root + descendants).
// Created by SceneSerializer::serializeSubtree and instantiated via
// SceneSerializer::instantiatePrefab. The format is identical to the
// full scene binary format but limited to the captured subtree.
struct PrefabAsset : public RuntimeAsset
{
	std::vector<uint8_t> data; // CBOR payload
};

// ── Animation ─────────────────────────────────────────────────────────────────

enum class AnimPathType : uint8_t
{
	Translation = 0,
	Rotation    = 1,   // quaternion, stored as xyzw (glTF convention)
	Scale       = 2,
};

// One animated property stream: times + values for a single joint and path.
// Translation/Scale: 3 floats per keyframe; Rotation: 4 floats per keyframe (xyzw).
struct AnimationChannel
{
	uint32_t           jointIndex = 0;
	AnimPathType       path       = AnimPathType::Translation;
	std::vector<float> times;   // keyframe timestamps in seconds
	std::vector<float> values;  // 3 or 4 floats per key depending on path
};

struct AnimationClipAsset : public RuntimeAsset
{
	float                         duration = 0.0f;  // total clip length in seconds
	std::vector<AnimationChannel> channels;
};

// ── Property Animation ────────────────────────────────────────────────────────
// Animates scalar properties of TransformComponent (position/rotation/scale)
// and MaterialAsset (baseColor, metallic, roughness, opacity) on a per-entity basis.

enum class PropTarget : uint8_t
{
    PosX = 0, PosY, PosZ,
    RotX, RotY, RotZ,
    ScaleX, ScaleY, ScaleZ,
    MatColorR, MatColorG, MatColorB,
    MatMetallic, MatRoughness, MatOpacity,
};

// One animated scalar stream: times + one float value per keyframe.
struct PropertyAnimChannel
{
    PropTarget         target = PropTarget::PosX;
    std::vector<float> times;
    std::vector<float> values; // one float per keyframe
};

struct PropertyAnimClipAsset : public RuntimeAsset
{
    float                             duration = 0.0f;
    std::vector<PropertyAnimChannel>  channels;
};