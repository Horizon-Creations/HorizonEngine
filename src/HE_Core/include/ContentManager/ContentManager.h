#pragma once
#include "Types/Defines.h"
#include "Types/UUID.h"
#include "Types/Enums.h"
#include "../SlotMap.h"
#include "Assets.h"
#include "ContentManager/DefaultAssets.h"
#include <filesystem>
#include <string>
#include <vector>
#include <unordered_map>

// Forward-declare so ContentManager can use it as a return type.
template<typename T> class AssetRef;

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

	// Check all disk-backed assets for file changes and reload any that have
	// been modified since the last load. Returns the UUIDs of reloaded assets
	// (same UUIDs — existing references remain valid). Virtual mem:// paths are
	// silently skipped. Safe to call every frame from the editor tick.
	std::vector<HE::UUID> pollHotReload();

	const std::string& contentRoot() const { return m_contentRoot; }
	// Point the manager at a different content directory (e.g. when the
	// editor opens a project). Previously loaded assets stay registered.
	void setContentRoot(std::string root) { m_contentRoot = std::move(root); }

	// ── Pin-based ref-counted access ──────────────────────────────────────
	// Returns a handle that keeps the asset pinned in memory for as long as it
	// is alive (RAII). unloadAsset() returns false while any handle is alive,
	// preventing use-after-free in the renderer and enabling safe LRU eviction.
	// The handle is null (operator bool() == false) when the UUID is unknown.
	AssetRef<StaticMeshAsset>   acquireStaticMesh(HE::UUID id);
	AssetRef<SkeletalMeshAsset> acquireSkeletalMesh(HE::UUID id);
	AssetRef<TextureAsset>      acquireTexture(HE::UUID id);
	AssetRef<MaterialAsset>     acquireMaterial(HE::UUID id);
	AssetRef<AudioAsset>        acquireAudio(HE::UUID id);
	AssetRef<ScriptAsset>       acquireScript(HE::UUID id);
	AssetRef<ShaderAsset>       acquireShader(HE::UUID id);

	// Pin bookkeeping — called by AssetRef; do not call directly.
	void pinAsset(HE::UUID id);
	void unpinAsset(HE::UUID id);
	bool isPinned(HE::UUID id) const;

	// ── Asset enumeration ──────────────────────────────────────────────────
	// Returns the UUIDs of all currently loaded/registered assets.
	std::vector<HE::UUID> enumerateIds() const;
	// Returns only UUIDs of assets of the given type.
	std::vector<HE::UUID> enumerateIds(HE::AssetType type) const;
	// Total number of loaded/registered assets (all types).
	size_t assetCount() const { return m_handleToUUID.size(); }
	// Returns the AssetType for a loaded/registered UUID, or Unknown if not found.
	HE::AssetType assetType(HE::UUID id) const;

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

	std::unordered_map<HE::UUID, SlotHandle>                              m_handleToUUID;
	std::unordered_map<HE::UUID, HE::AssetType>                          m_assetTypeIndex; // mirrors m_handleToUUID with type info
	std::unordered_map<std::string, HE::UUID>                            m_pathToUUID;
	std::unordered_map<std::string, std::filesystem::file_time_type>     m_pathMtime;      // disk mtime at last load
	std::unordered_map<HE::UUID, int>                                    m_pinCounts;      // active AssetRef handles per asset
};

// ─────────────────────────────────────────────────────────────────────────────
// AssetRef<T> — RAII pin handle returned by ContentManager::acquireXxx()
//
// Calling acquireXxx() increments an internal pin counter; the counter
// decrements when the last AssetRef for that UUID is destroyed. While any
// AssetRef is alive, ContentManager::unloadAsset() returns false for that
// UUID, preventing use-after-free. The raw pointer is guaranteed stable for
// the lifetime of the handle (as long as the ContentManager outlives it).
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
class AssetRef {
public:
    AssetRef() = default;

    // Internal constructor — called by ContentManager::acquireXxx().
    // Pins the asset only when ptr is non-null (null means UUID not found).
    AssetRef(ContentManager* cm, HE::UUID id, const T* ptr)
        : m_cm(ptr ? cm : nullptr), m_id(id), m_ptr(ptr)
    {
        if (m_cm) m_cm->pinAsset(m_id);
    }

    AssetRef(const AssetRef& o)
        : m_cm(o.m_cm), m_id(o.m_id), m_ptr(o.m_ptr)
    {
        if (m_cm) m_cm->pinAsset(m_id);
    }

    AssetRef(AssetRef&& o) noexcept
        : m_cm(o.m_cm), m_id(o.m_id), m_ptr(o.m_ptr)
    {
        o.m_cm  = nullptr;
        o.m_ptr = nullptr;
    }

    ~AssetRef() { reset(); }

    AssetRef& operator=(AssetRef o) noexcept
    {
        std::swap(m_cm,  o.m_cm);
        std::swap(m_id,  o.m_id);
        std::swap(m_ptr, o.m_ptr);
        return *this;
    }

    void reset()
    {
        if (m_cm) m_cm->unpinAsset(m_id);
        m_cm  = nullptr;
        m_ptr = nullptr;
    }

    const T*  get()          const { return m_ptr; }
    const T*  operator->()   const { return m_ptr; }
    const T&  operator*()    const { return *m_ptr; }
    explicit  operator bool() const { return m_ptr != nullptr; }
    HE::UUID  id()           const { return m_id; }

private:
    ContentManager* m_cm  = nullptr;
    HE::UUID        m_id  = {};
    const T*        m_ptr = nullptr;
};

// Inline definitions of acquireXxx — placed here so AssetRef<T> is complete.
inline AssetRef<StaticMeshAsset>   ContentManager::acquireStaticMesh(HE::UUID id)   { return { this, id, getStaticMesh(id) }; }
inline AssetRef<SkeletalMeshAsset> ContentManager::acquireSkeletalMesh(HE::UUID id) { return { this, id, getSkeletalMesh(id) }; }
inline AssetRef<TextureAsset>      ContentManager::acquireTexture(HE::UUID id)      { return { this, id, getTexture(id) }; }
inline AssetRef<MaterialAsset>     ContentManager::acquireMaterial(HE::UUID id)     { return { this, id, getMaterial(id) }; }
inline AssetRef<AudioAsset>        ContentManager::acquireAudio(HE::UUID id)        { return { this, id, getAudio(id) }; }
inline AssetRef<ScriptAsset>       ContentManager::acquireScript(HE::UUID id)       { return { this, id, getScript(id) }; }
inline AssetRef<ShaderAsset>       ContentManager::acquireShader(HE::UUID id)       { return { this, id, getShader(id) }; }