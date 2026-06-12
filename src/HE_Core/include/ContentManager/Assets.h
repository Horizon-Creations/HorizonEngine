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