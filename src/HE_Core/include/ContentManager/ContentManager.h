#pragma once
#include "Types/Defines.h"
#include "Types/UUID.h"
#include "Types/Enums.h"
#include "../SlotMap.h"
#include "Assets.h"
#include "ContentManager/DefaultAssets.h"
#include <string>
#include <vector>
#include <unordered_map>

class HE_API ContentManager
{
public:
	ContentManager()                          { initDefaultAssets(); }
	ContentManager(std::string contentPath) : m_contentRoot(std::move(contentPath)) { initDefaultAssets(); }
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

	// Mutable access to a loaded material, for in-editor editing. Edits are
	// visible immediately to any renderer sharing this manager; persist them to
	// disk with saveAsset(). Returns nullptr when the UUID is not a material.
	MaterialAsset*           getMaterialMutable(HE::UUID id);

	// ── Runtime (in-memory) assets ─────────────────────────────────────────
	// Register a runtime-generated asset that has no .hasset file on disk:
	// procedural meshes (e.g. terrain), generated/default textures, fallback or
	// editor-created materials. Mints a UUID if the asset has none, indexes it
	// exactly like a loaded asset (so getStaticMesh()/the renderer resolve it by
	// UUID), and returns that UUID. A non-empty asset.path is also registered in
	// the path index — use a "mem://"-style virtual path to avoid clashing with
	// real files. The asset is moved in; persist it later with saveAsset() if a
	// real file is wanted.
	HE::UUID registerStaticMesh(StaticMeshAsset asset);
	HE::UUID registerTexture(TextureAsset asset);
	HE::UUID registerMaterial(MaterialAsset asset);

	// Replace a registered asset's payload in place, keeping its UUID so existing
	// references stay valid (e.g. regenerating a procedural terrain mesh after a
	// parameter change). Returns false when the UUID is not a registered asset of
	// the requested type. The original identity (id/name/path) is preserved.
	bool replaceStaticMesh(HE::UUID id, StaticMeshAsset asset);
	bool replaceTexture(HE::UUID id, TextureAsset asset);
	bool replaceMaterial(HE::UUID id, MaterialAsset asset);

	const std::string& contentRoot() const { return m_contentRoot; }
	// Point the manager at a different content directory (e.g. when the
	// editor opens a project). Previously loaded assets stay registered.
	void setContentRoot(std::string root) { m_contentRoot = std::move(root); }

	// ── Asset enumeration ──────────────────────────────────────────────────
	// Returns the UUIDs of all currently loaded/registered assets.
	std::vector<HE::UUID> enumerateIds() const;
	// Returns only UUIDs of assets of the given type.
	std::vector<HE::UUID> enumerateIds(HE::AssetType type) const;
	// Total number of loaded/registered assets (all types).
	size_t assetCount() const { return m_handleToUUID.size(); }

private:

	HE::AssetType getAssetType(const std::string path) const;
	void          initDefaultAssets();

	// Shared indexing for runtime asset registration / in-place replacement.
	// Defined in the .cpp; only instantiated by the typed wrappers above.
	template<typename T>
	HE::UUID registerRuntimeAsset(SlotMap<T>& map, T asset, HE::AssetType type);
	template<typename T>
	bool     replaceRuntimeAsset(SlotMap<T>& map, HE::UUID id, T asset);

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

	std::unordered_map<HE::UUID, SlotHandle>      m_handleToUUID;
	std::unordered_map<HE::UUID, HE::AssetType>  m_assetTypeIndex; // mirrors m_handleToUUID with type info
	std::unordered_map<std::string, HE::UUID>    m_pathToUUID;
};