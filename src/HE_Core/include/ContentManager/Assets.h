#pragma once
#include <cstdint>
#include <string>
#include <vector>
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

struct SkeletalMeshAsset : public RuntimeAsset
{
	std::string            materialPath;
	std::vector<float>     vertices;
	std::vector<uint32_t>  indices;
	std::vector<float>     normals;
	std::vector<float>     uvs;
	std::vector<uint32_t>  boneIDs;
	std::vector<float>     boneWeights;
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