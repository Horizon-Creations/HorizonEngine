#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include "Types/Enums.h"
#include "Types/UUID.h"

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
	std::vector<float>     vertices;
	std::vector<uint32_t>  indices;
	std::vector<float>     normals;
	std::vector<float>     uvs;
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
};

struct SceneAsset : public RuntimeAsset
{
	std::vector<std::string> objectPaths;
};

struct ScriptAsset : public RuntimeAsset
{
	std::string sourceCode;
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

struct TextureAsset : public RuntimeAsset
{
	std::vector<uint8_t> data;     // raw pixel bytes (RGBA8 unless channels says otherwise)
	size_t               width    = 0;
	size_t               height   = 0;
	size_t               channels = 0;
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