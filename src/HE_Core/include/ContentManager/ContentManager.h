#pragma once
#include "Types/UUID.h"
#include "Types/Enums.h"
#include "../SlotMap.h"
#include "Assets.h"
#include <string>
#include <unordered_map>

class ContentManager
{
public:
	ContentManager() = default;
	ContentManager(std::string contentPath) : m_contentRoot(std::move(contentPath)) {}
	~ContentManager() = default;

	HE::UUID loadAsset(const std::string& relativePath);
	bool unloadAsset(HE::UUID id);
	bool isLoaded(HE::UUID id) const;
	bool isLoaded(const std::string& relativePath) const;
	bool saveAsset(RuntimeAsset& asset);

private:

	HE::AssetType getAssetType(const std::string path) const;

	std::string m_contentRoot;

	SlotMap<StaticMeshAsset>   m_staticMeshAssets;
	SlotMap<SkeletalMeshAsset> m_skeletalMeshAssets;
	SlotMap<TextureAsset>      m_textureAssets;
	SlotMap<MaterialAsset>     m_materialAssets;
	SlotMap<SceneAsset>        m_sceneAssets;
	SlotMap<ScriptAsset>       m_scriptAssets;
	SlotMap<AudioAsset>        m_audioAssets;
	SlotMap<FontAsset>         m_fontAssets;
	SlotMap<ShaderAsset>       m_shaderAssets;

	std::unordered_map<HE::UUID, SlotHandle>    m_handleToUUID;
	std::unordered_map<std::string, HE::UUID>   m_pathToUUID;
};