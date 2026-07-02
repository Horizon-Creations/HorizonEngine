#pragma once
#include "Types/Defines.h"
#include "Types/UUID.h"
#include "Types/Enums.h"
#include "../SlotMap.h"
#include "Assets.h"
#include "ContentManager/DefaultAssets.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_set>
#include <vector>
#include <unordered_map>

// Forward-declares
template<typename T> class AssetRef;
namespace HAsset { class Reader; }
class HpakReader;

class HE_API ContentManager
{
public:
	// Defined in the .cpp: ContentManager owns std::unique_ptr<HpakReader> mounts,
	// so its special members must be instantiated where HpakReader is complete.
	ContentManager();
	explicit ContentManager(std::string contentPath);
	~ContentManager();

	HE::UUID loadAsset(const std::string& relativePath);
	bool unloadAsset(HE::UUID id);
	bool isLoaded(HE::UUID id) const;
	bool isLoaded(const std::string& relativePath) const;
	bool saveAsset(RuntimeAsset& asset);

	// Typed lookup of a loaded asset. Returns nullptr when the UUID is unknown
	// or refers to an asset of a different type.
	const StaticMeshAsset*     getStaticMesh(HE::UUID id) const;
	const SkeletalMeshAsset*   getSkeletalMesh(HE::UUID id) const;
	const TextureAsset*        getTexture(HE::UUID id) const;
	const MaterialAsset*       getMaterial(HE::UUID id) const;
	const AudioAsset*          getAudio(HE::UUID id) const;
	const ScriptAsset*         getScript(HE::UUID id) const;
	const ShaderAsset*         getShader(HE::UUID id) const;
	const PrefabAsset*         getPrefab(HE::UUID id) const;
	const AnimationClipAsset*  getAnimationClip(HE::UUID id) const;
	const PropertyAnimClipAsset* getPropertyAnimClip(HE::UUID id) const;

	// ── Dual-mode reference resolution (used by the renderer backends) ─────────
	// Resolve an asset→asset reference that carries a pack-time baked UUID and/or
	// an editor path. Packed builds have the UUID (path strings are dropped at pack
	// time); loose editor content has only the path. Prefers the UUID: ensures the
	// asset is resident (on-demand from a mounted pak — usually a no-op because the
	// streaming closure already loaded it) and returns it. Falls back to the path
	// via loadAsset(). Returns nullptr when neither resolves. MAIN-THREAD ONLY.
	const MaterialAsset* resolveMaterialRef(HE::UUID bakedId, const std::string& path);
	const TextureAsset*  resolveTextureRef (HE::UUID bakedId, const std::string& path);

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
	HE::UUID registerSkeletalMesh(SkeletalMeshAsset asset);
	HE::UUID registerTexture(TextureAsset asset);
	HE::UUID registerMaterial(MaterialAsset asset);
	HE::UUID registerPrefab(PrefabAsset asset);
	HE::UUID registerAudio(AudioAsset asset);
	HE::UUID registerScript(ScriptAsset asset);
	HE::UUID registerAnimationClip(AnimationClipAsset asset);
	HE::UUID registerPropertyAnimClip(PropertyAnimClipAsset asset);

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

	// Load all assets packed in a .hpak archive. Each entry's raw .hasset blob
	// is parsed and registered exactly like loadAsset() would. Already-loaded
	// UUIDs are skipped. Pass a 32-byte key if the pak was encrypted; nullptr
	// for unencrypted. Returns true when the file was opened; individual entry
	// parse failures are silently skipped.
	bool loadPak(const std::string& path, const uint8_t key[32] = nullptr);

	// ── On-demand pak mounting (streaming) ─────────────────────────────────────
	// Mount a .hpak WITHOUT parsing its entries: the archive is kept open and a
	// UUID→entry residency index is built, so assets load lazily on first access
	// (acquireXxx / ensureResident) instead of all up front. Mounting is an overlay
	// stack — a later mount SHADOWS an earlier one for the same UUID (patch/DLC/mod
	// semantics) and contributes new UUIDs as additions. Pass a 32-byte key for an
	// encrypted pak. Returns true when the archive opened.
	bool mountPak(const std::string& path, const uint8_t key[32] = nullptr);

	// Ensure the asset is resident, loading it from the highest-priority mount that
	// provides it if necessary. MAIN-THREAD ONLY (mutates the SlotMaps — never call
	// during rendering). Returns true if the asset is resident afterwards.
	bool ensureResident(HE::UUID id);

	// Number of currently mounted archives.
	size_t mountedPakCount() const { return m_mounts.size(); }

	// Parse a raw .hasset blob from memory and register it by its embedded UUID.
	// Returns the UUID on success, an empty UUID on parse failure.
	HE::UUID loadAssetFromMemory(const std::vector<uint8_t>& hassetData);

	// Async streaming — submit a background I/O job that reads the file on a
	// worker thread; call pollAsyncResults() each frame to register completed
	// assets and fire callbacks on the main thread (safe, no locking needed on
	// the SlotMaps). If the asset is already loaded the callback fires immediately
	// with the existing UUID; duplicate in-flight requests for the same path are
	// coalesced into one job (only the first callback fires — subsequent callers
	// should use pollAsyncResults' return value or check isLoaded themselves).
	void     loadAssetAsync(const std::string& relativePath,
	                        std::function<void(HE::UUID)> callback = {});
	// Async-load an asset by UUID from a mounted pak (see mountPak): the worker
	// reads + decodes the entry off-thread, pollAsyncResults() registers it on the
	// main thread. Fires the callback immediately if already resident; the callback
	// gets an empty UUID if the id is in no mount. Duplicate in-flight requests for
	// the same UUID are coalesced.
	void     loadAssetAsync(HE::UUID id, std::function<void(HE::UUID)> callback = {});
	// Kick off async background loads for every mounted-but-not-yet-resident asset
	// (stream a whole pak without blocking startup). Drain via pollAsyncResults().
	// UUIDs in `exclude` are skipped (e.g. the packed startup scene, which is not a
	// parseable asset). Returns the number of load jobs submitted.
	size_t   streamMountedAssets(const std::unordered_set<HE::UUID>& exclude = {});

	// Read the raw (decoded) bytes of a mounted entry WITHOUT parsing/registering
	// it as an asset — for non-asset payloads packed into the .hpak, e.g. the
	// binary startup scene. Empty vector if the UUID is in no mount or read fails.
	std::vector<uint8_t> readMountedEntry(HE::UUID id);
	// Drain completed async jobs and register each asset + fire callbacks.
	// Call once per frame from the main/game thread. Registration (parse + insert)
	// runs here on the main thread, so `maxRegistrations` caps how many assets are
	// processed per call — pass a small budget while streaming so a burst of
	// simultaneously-finished loads is spread across frames instead of freezing one.
	// The rest stay queued for the next call. Default: unlimited (drain everything).
	// Returns the UUIDs of assets registered this call.
	std::vector<HE::UUID> pollAsyncResults(size_t maxRegistrations = SIZE_MAX);
	// True while a background job for this relative path is in flight.
	bool isAsyncPending(const std::string& relativePath) const;

	const std::string& contentRoot() const { return m_contentRoot; }
	// Point the manager at a different content directory (e.g. when the
	// editor opens a project). Previously loaded assets stay registered.
	void setContentRoot(std::string root) { m_contentRoot = std::move(root); }

	// ── Pin-based ref-counted access ──────────────────────────────────────
	// Returns a handle that keeps the asset pinned in memory for as long as it
	// is alive (RAII). unloadAsset() returns false while any handle is alive,
	// preventing use-after-free in the renderer and enabling safe LRU eviction.
	// The handle is null (operator bool() == false) when the UUID is unknown.
	AssetRef<StaticMeshAsset>    acquireStaticMesh(HE::UUID id);
	AssetRef<SkeletalMeshAsset>  acquireSkeletalMesh(HE::UUID id);
	AssetRef<TextureAsset>       acquireTexture(HE::UUID id);
	AssetRef<MaterialAsset>      acquireMaterial(HE::UUID id);
	AssetRef<AudioAsset>         acquireAudio(HE::UUID id);
	AssetRef<ScriptAsset>        acquireScript(HE::UUID id);
	AssetRef<ShaderAsset>        acquireShader(HE::UUID id);
	AssetRef<PrefabAsset>        acquirePrefab(HE::UUID id);
	AssetRef<AnimationClipAsset>      acquireAnimationClip(HE::UUID id);
	AssetRef<PropertyAnimClipAsset>   acquirePropertyAnimClip(HE::UUID id);

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

	// Shared parser called by loadAsset (sync) and pollAsyncResults (main-thread
	// drain). reader must already be opened; fullPath is only used for mtime.
	HE::UUID parseAndRegisterAsset(const std::string& relativePath,
	                                const std::string& fullPath,
	                                HAsset::Reader&    reader);

	// Reference-graph frontier: enqueue a just-registered asset's baked UUID
	// dependencies (mesh→material, material→textures) for async streaming. Called
	// from pollAsyncResults on the main thread. No-op for loose assets.
	void expandFrontier(HE::UUID id);

	// ── Async streaming state ─────────────────────────────────────────────────
	struct AsyncResult {
		std::string                    relativePath;
		std::string                    fullPath;
		std::vector<uint8_t>           fileBytes;   // populated on background thread
		std::function<void(HE::UUID)>  callback;
		bool                           failed = false;
	};

	mutable std::mutex              m_pendingMutex;
	std::unordered_set<std::string> m_pendingPaths;  // in-flight relative paths

	std::mutex                      m_resultsMutex;
	std::queue<AsyncResult>         m_asyncResults;  // ready to register (main thread)

	std::string m_contentRoot;

	SlotMap<StaticMeshAsset>    m_staticMeshAssets;
	SlotMap<SkeletalMeshAsset>  m_skeletalMeshAssets;
	SlotMap<TextureAsset>       m_textureAssets;
	SlotMap<MaterialAsset>      m_materialAssets;
	SlotMap<SceneAsset>         m_sceneAssets;
	SlotMap<ScriptAsset>        m_scriptAssets;
	SlotMap<AudioAsset>         m_audioAssets;
	SlotMap<FontAsset>          m_fontAssets;
	SlotMap<ShaderAsset>        m_shaderAssets;
	SlotMap<PrefabAsset>        m_prefabAssets;
	SlotMap<AnimationClipAsset>      m_animClipAssets;
	SlotMap<PropertyAnimClipAsset>   m_propAnimClipAssets;

	// ── Mounted paks (on-demand streaming) ─────────────────────────────────────
	struct MountedPak {
		std::unique_ptr<HpakReader> reader;   // main-thread synchronous reads (ensureResident)
		std::string                 path;     // reopened per async job (worker-thread reads)
		std::array<uint8_t, 32>     key{};
		bool                        encrypted = false;
	};
	std::vector<MountedPak>                       m_mounts;        // overlay stack (later = higher priority)
	std::unordered_map<HE::UUID, size_t>          m_pakResidency;  // UUID → index into m_mounts

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
// Each ensures the asset is resident first (loading it on demand from a mounted
// pak if needed), then pins it. Safe on the main thread; do not call while drawing.
inline AssetRef<StaticMeshAsset>    ContentManager::acquireStaticMesh(HE::UUID id)    { ensureResident(id); return { this, id, getStaticMesh(id) }; }
inline AssetRef<SkeletalMeshAsset>  ContentManager::acquireSkeletalMesh(HE::UUID id)  { ensureResident(id); return { this, id, getSkeletalMesh(id) }; }
inline AssetRef<TextureAsset>       ContentManager::acquireTexture(HE::UUID id)       { ensureResident(id); return { this, id, getTexture(id) }; }
inline AssetRef<MaterialAsset>      ContentManager::acquireMaterial(HE::UUID id)      { ensureResident(id); return { this, id, getMaterial(id) }; }
inline AssetRef<AudioAsset>         ContentManager::acquireAudio(HE::UUID id)         { ensureResident(id); return { this, id, getAudio(id) }; }
inline AssetRef<ScriptAsset>        ContentManager::acquireScript(HE::UUID id)        { ensureResident(id); return { this, id, getScript(id) }; }
inline AssetRef<ShaderAsset>        ContentManager::acquireShader(HE::UUID id)        { ensureResident(id); return { this, id, getShader(id) }; }
inline AssetRef<PrefabAsset>        ContentManager::acquirePrefab(HE::UUID id)        { ensureResident(id); return { this, id, getPrefab(id) }; }
inline AssetRef<AnimationClipAsset>      ContentManager::acquireAnimationClip(HE::UUID id)     { ensureResident(id); return { this, id, getAnimationClip(id) }; }
inline AssetRef<PropertyAnimClipAsset>   ContentManager::acquirePropertyAnimClip(HE::UUID id)  { ensureResident(id); return { this, id, getPropertyAnimClip(id) }; }