#pragma once
#include "Types/Defines.h"
#include "Types/UUID.h"
#include "Types/Enums.h"
#include "../SlotMap.h"
#include "Assets.h"
#include <string>
#include <unordered_map>

class HE_API ContentManager
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

	// Typed lookup of a loaded asset. Returns nullptr when the UUID is unknown
	// or refers to an asset of a different type.
	const StaticMeshAsset*   getStaticMesh(HE::UUID id) const;
	const SkeletalMeshAsset* getSkeletalMesh(HE::UUID id) const;
	const TextureAsset*      getTexture(HE::UUID id) const;
	const MaterialAsset*     getMaterial(HE::UUID id) const;
	const AudioAsset*        getAudio(HE::UUID id) const;
	const ScriptAsset*       getScript(HE::UUID id) const;
	const ShaderAsset*       getShader(HE::UUID id) const;

	const std::string& contentRoot() const { return m_contentRoot; }
	// Point the manager at a different content directory (e.g. when the
	// editor opens a project). Previously loaded assets stay registered.
	void setContentRoot(std::string root) { m_contentRoot = std::move(root); }

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