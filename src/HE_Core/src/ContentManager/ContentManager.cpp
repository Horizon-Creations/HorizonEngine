#include "ContentManager/ContentManager.h"
#include "ContentManager/HAsset.h"
#include "Hpak/HpakReader.h"
#include "JobSystem/JobSystem.h"
#include "Diagnostics/Logger.h"
#include "Diagnostics/Profiler.h"
#include <cstring>
#include <fstream>

// ─── Internal helpers ─────────────────────────────────────────────────────────

static std::vector<uint8_t> buildMetaChunk(const RuntimeAsset& a)
{
    std::vector<uint8_t> buf;
    HAsset::Writer::appendPOD(buf, static_cast<uint16_t>(a.type));
    HAsset::Writer::appendPOD(buf, a.id.hi);
    HAsset::Writer::appendPOD(buf, a.id.lo);
    HAsset::Writer::appendString(buf, a.name);
    HAsset::Writer::appendString(buf, a.path);
    return buf;
}

// fileVersion: v1 META has no UUID — idOut stays invalid and the caller
// must generate (and ideally persist) a fresh one.
static bool readMetaChunk(const HAsset::Reader::Chunk& c, uint16_t fileVersion,
                          HE::UUID& idOut, std::string& nameOut, std::string& pathOut)
{
    size_t off = sizeof(uint16_t); // type already known from file header
    if (fileVersion >= 2)
    {
        if (!HAsset::Reader::readPOD(c.data, off, idOut.hi)) return false;
        if (!HAsset::Reader::readPOD(c.data, off, idOut.lo)) return false;
    }
    if (!HAsset::Reader::readString(c.data, off, nameOut)) return false;
    if (!HAsset::Reader::readString(c.data, off, pathOut)) return false;
    return true;
}

// ─── getAssetType ─────────────────────────────────────────────────────────────
HE::AssetType ContentManager::getAssetType(const std::string path) const
{
	std::ifstream f(path, std::ios::binary);
	if (!f.is_open()) return HE::AssetType::Unknown;

	HAsset::FileHeader hdr{};
	f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
	if (!f || std::memcmp(hdr.magic, HAsset::k_magic, 4) != 0)
		return HE::AssetType::Unknown;

	return static_cast<HE::AssetType>(hdr.asset_type);
}

// ─── parseAndRegisterAsset ────────────────────────────────────────────────────
// Shared core: parse an already-opened reader and register the asset.
// relativePath is stored as asset.path and keyed in m_pathToUUID.
// fullPath is only used for mtime (empty string → skip mtime).
HE::UUID ContentManager::parseAndRegisterAsset(const std::string& relativePath,
                                                const std::string& fullPath,
                                                HAsset::Reader&    reader)
{
	const HE::AssetType type = static_cast<HE::AssetType>(reader.assetType());

	const auto* metaChunk = reader.findChunk(HAsset::CHUNK_META);
	if (!metaChunk) return HE::UUID();

	HE::UUID    id;
	std::string assetName, assetPath;
	if (!readMetaChunk(*metaChunk, reader.header().version, id, assetName, assetPath))
		return HE::UUID();

	if (id == HE::UUID{})
	{
		id = HE::UUID::generate();
		Logger::Log(Logger::LogLevel::Warning,
			("ContentManager: asset has no persisted UUID (pre-v2 file), generated transient id: " + relativePath).c_str());
	}

	SlotHandle handle{};

	switch (type)
	{
	case HE::AssetType::StaticMesh:
	{
		StaticMeshAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_MREF)) { size_t o=0; HAsset::Reader::readString(c->data,o,a.materialPath); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_MRFU)) { size_t o=0; HAsset::Reader::readPOD(c->data,o,a.materialId.hi); HAsset::Reader::readPOD(c->data,o,a.materialId.lo); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_VERT)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.vertices); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_INDX)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.indices); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_NORM)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.normals); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_TEXC)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.uvs); }
		handle = m_staticMeshAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::SkeletalMesh:
	{
		SkeletalMeshAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_MREF)) { size_t o=0; HAsset::Reader::readString(c->data,o,a.materialPath); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_MRFU)) { size_t o=0; HAsset::Reader::readPOD(c->data,o,a.materialId.hi); HAsset::Reader::readPOD(c->data,o,a.materialId.lo); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_VERT)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.vertices); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_INDX)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.indices); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_NORM)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.normals); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_TEXC)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.uvs); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_BONE)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.boneIDs); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_BWGT)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.boneWeights); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_SKEL))
		{
			size_t o=0; uint32_t count=0;
			HAsset::Reader::readPOD(c->data,o,count);
			a.skeleton.resize(count);
			for (auto& j : a.skeleton)
			{
				HAsset::Reader::readString(c->data,o,j.name);
				HAsset::Reader::readPOD(c->data,o,j.parent);
				for (float& f : j.inverseBindMatrix) HAsset::Reader::readPOD(c->data,o,f);
			}
		}
		handle = m_skeletalMeshAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::Texture:
	{
		TextureAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_TXMI))
		{
			size_t o=0;
			HAsset::Reader::readPOD(c->data,o,a.width);
			HAsset::Reader::readPOD(c->data,o,a.height);
			HAsset::Reader::readPOD(c->data,o,a.channels);
		}
		if (const auto* c = reader.findChunk(HAsset::CHUNK_PIXL)) a.data = c->data;
		handle = m_textureAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::Material:
	{
		MaterialAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_MTRL))
		{
			size_t o=0;
			HAsset::Reader::readString(c->data,o,a.shaderPath);
			HAsset::Reader::readVec(c->data,o,a.texturePaths);
			HAsset::Reader::readPOD(c->data,o,a.baseColor[0]);
			HAsset::Reader::readPOD(c->data,o,a.baseColor[1]);
			HAsset::Reader::readPOD(c->data,o,a.baseColor[2]);
			HAsset::Reader::readPOD(c->data,o,a.metallic);
			HAsset::Reader::readPOD(c->data,o,a.roughness);
			HAsset::Reader::readPOD(c->data,o,a.opacity);
		}
		if (const auto* c = reader.findChunk(HAsset::CHUNK_MTLU))
		{
			size_t o=0;
			HAsset::Reader::readPOD(c->data,o,a.shaderId.hi);
			HAsset::Reader::readPOD(c->data,o,a.shaderId.lo);
			HAsset::Reader::readVec(c->data,o,a.textureIds);
		}
		handle = m_materialAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::Scene:
	{
		SceneAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_SCNE)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.objectPaths); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_SCNU)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.objectIds); }
		handle = m_sceneAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::Script:
	{
		ScriptAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_SRC))
			a.sourceCode.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
		handle = m_scriptAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::Audio:
	{
		AudioAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_AUMI))
		{ size_t o=0; HAsset::Reader::readPOD(c->data,o,a.sampleRate); HAsset::Reader::readPOD(c->data,o,a.channels); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_PCMD)) a.audioData = c->data;
		handle = m_audioAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::Font:
	{
		FontAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_FNTI)) { size_t o=0; HAsset::Reader::readPOD(c->data,o,a.size); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_FNTD)) a.fontData = c->data;
		handle = m_fontAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::Shader:
	{
		ShaderAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_SRC))
			a.sourceCode.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
		handle = m_shaderAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::AnimationClip:
	{
		AnimationClipAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_ANIM))
		{
			size_t o = 0;
			HAsset::Reader::readPOD(c->data, o, a.duration);
			uint32_t channelCount = 0;
			HAsset::Reader::readPOD(c->data, o, channelCount);
			a.channels.resize(channelCount);
			for (auto& ch : a.channels)
			{
				uint8_t pathByte = 0;
				HAsset::Reader::readPOD(c->data, o, ch.jointIndex);
				HAsset::Reader::readPOD(c->data, o, pathByte);
				ch.path = static_cast<AnimPathType>(pathByte);
				HAsset::Reader::readVec(c->data, o, ch.times);
				HAsset::Reader::readVec(c->data, o, ch.values);
			}
		}
		handle = m_animClipAssets.insert(std::move(a)); break;
	}
	default:
		return HE::UUID();
	}

	m_handleToUUID[id]         = handle;
	m_assetTypeIndex[id]       = type;
	m_pathToUUID[relativePath] = id;
	if (!fullPath.empty())
	{
		std::error_code ec;
		auto mtime = std::filesystem::last_write_time(fullPath, ec);
		if (!ec) m_pathMtime[relativePath] = mtime;
	}
	return id;
}

// ─── loadAsset ────────────────────────────────────────────────────────────────
HE::UUID ContentManager::loadAsset(const std::string& relativePath)
{
	HE_PROFILE_SCOPE_N("ContentManager::load");
	if (isLoaded(relativePath))
		return m_pathToUUID.at(relativePath);

	const std::string fullPath = m_contentRoot + "/" + relativePath;

	HAsset::Reader reader;
	if (!reader.open(fullPath))
		return HE::UUID();

	return parseAndRegisterAsset(relativePath, fullPath, reader);
}

// ─── loadAssetAsync ───────────────────────────────────────────────────────────
void ContentManager::loadAssetAsync(const std::string& relativePath,
                                     std::function<void(HE::UUID)> callback)
{
	if (isLoaded(relativePath))
	{
		if (callback) callback(m_pathToUUID.at(relativePath));
		return;
	}

	{
		std::unique_lock<std::mutex> lock(m_pendingMutex);
		if (m_pendingPaths.count(relativePath))
			return; // already in flight — coalesce
		m_pendingPaths.insert(relativePath);
	}

	const std::string fullPath = m_contentRoot + "/" + relativePath;

	globalPool().submit([this, relativePath, fullPath,
	                     cb = std::move(callback)]() mutable
	{
		AsyncResult result;
		result.relativePath = relativePath;
		result.fullPath     = fullPath;
		result.callback     = std::move(cb);

		std::ifstream f(fullPath, std::ios::binary);
		if (f)
		{
			result.fileBytes.assign(std::istreambuf_iterator<char>(f),
			                        std::istreambuf_iterator<char>());
		}
		else
		{
			result.failed = true;
		}

		std::unique_lock<std::mutex> lock(m_resultsMutex);
		m_asyncResults.push(std::move(result));
	});
}

// ─── pollAsyncResults ─────────────────────────────────────────────────────────
std::vector<HE::UUID> ContentManager::pollAsyncResults(size_t maxRegistrations)
{
	std::vector<AsyncResult> ready;
	{
		std::unique_lock<std::mutex> lock(m_resultsMutex);
		// Pull at most `maxRegistrations` completed jobs; leave the rest queued so
		// a burst is spread over frames (registration/parse runs on this thread).
		while (!m_asyncResults.empty() && ready.size() < maxRegistrations)
		{
			ready.push_back(std::move(m_asyncResults.front()));
			m_asyncResults.pop();
		}
	}

	std::vector<HE::UUID> registered;
	for (auto& r : ready)
	{
		{
			std::unique_lock<std::mutex> lock(m_pendingMutex);
			m_pendingPaths.erase(r.relativePath);
		}

		HE::UUID id;
		if (!r.failed && !r.fileBytes.empty())
		{
			HAsset::Reader reader;
			if (reader.openData(r.fileBytes))
			{
				// Register under the asset's REAL embedded META path, not the
				// synthetic "pak://hi-lo" coalesce key, so a path-based resolver
				// (loadAsset(materialPath)) hits the m_pathToUUID cache instead of
				// falling through to a disk read that fails in a pak-only build.
				std::string registerPath = r.relativePath;
				if (registerPath.rfind("pak://", 0) == 0)
				{
					if (const auto* meta = reader.findChunk(HAsset::CHUNK_META))
					{
						HE::UUID mid; std::string mname, mpath;
						if (readMetaChunk(*meta, reader.header().version, mid, mname, mpath) && !mpath.empty())
							registerPath = mpath;
					}
				}
				id = parseAndRegisterAsset(registerPath, r.fullPath, reader);
			}
		}

		if (id != HE::UUID{})
		{
			registered.push_back(id);
			// Reference-graph frontier: stream this asset's baked UUID dependencies
			// (mesh→material, material→textures) so the closure loads with pure UUID
			// traversal — no path lookups. No-op for loose assets (empty ref UUIDs).
			expandFrontier(id);
		}
		if (r.callback) r.callback(id);
	}
	return registered;
}

// ─── isAsyncPending ───────────────────────────────────────────────────────────
bool ContentManager::isAsyncPending(const std::string& relativePath) const
{
	std::unique_lock<std::mutex> lock(m_pendingMutex);
	return m_pendingPaths.count(relativePath) > 0;
}

// ─── loadAssetAsync (by UUID, from a mounted pak) ─────────────────────────────
void ContentManager::loadAssetAsync(HE::UUID id, std::function<void(HE::UUID)> callback)
{
	if (isLoaded(id))
	{
		if (callback) callback(id);
		return;
	}

	const auto it = m_pakResidency.find(id);
	if (it == m_pakResidency.end())
	{
		if (callback) callback(HE::UUID{}); // not provided by any mount
		return;
	}
	const MountedPak& mount = m_mounts[it->second];

	// Coalesce by a synthetic per-UUID key so it shares the pending set with the
	// path-based overload without colliding with real relative paths.
	const std::string coalesceKey =
		"pak://" + std::to_string(id.hi) + "-" + std::to_string(id.lo);
	{
		std::unique_lock<std::mutex> lock(m_pendingMutex);
		if (m_pendingPaths.count(coalesceKey)) return; // already in flight
		m_pendingPaths.insert(coalesceKey);
	}

	// Capture everything the worker needs by value — it must not touch the shared
	// mount reader (single ifstream, not thread-safe), so it opens its own.
	const std::string       path = mount.path;
	const bool              enc  = mount.encrypted;
	std::array<uint8_t, 32> key  = mount.key;

	globalPool().submit([this, id, path, enc, key, coalesceKey,
	                     cb = std::move(callback)]() mutable
	{
		AsyncResult result;
		result.relativePath = coalesceKey;
		result.callback     = std::move(cb);

		HpakReader reader; // worker-local; safe for concurrent reads across jobs
		if (reader.open(path))
		{
			auto data = reader.readEntry(id, enc ? key.data() : nullptr);
			if (!data.empty()) result.fileBytes = std::move(data); // decoded .hasset
			else               result.failed = true;
		}
		else result.failed = true;

		std::unique_lock<std::mutex> lock(m_resultsMutex);
		m_asyncResults.push(std::move(result));
	});
}

// ─── streamMountedAssets ──────────────────────────────────────────────────────
size_t ContentManager::streamMountedAssets(const std::unordered_set<HE::UUID>& exclude)
{
	size_t submitted = 0;
	for (const auto& [id, mountIdx] : m_pakResidency)
	{
		(void)mountIdx;
		if (isLoaded(id) || exclude.count(id)) continue;
		loadAssetAsync(id);
		++submitted;
	}
	return submitted;
}

// ─── Dual-mode reference resolution ───────────────────────────────────────────
const MaterialAsset* ContentManager::resolveMaterialRef(HE::UUID bakedId, const std::string& path)
{
	if (bakedId != HE::UUID{})
	{
		ensureResident(bakedId);          // no-op when already streamed in
		return getMaterial(bakedId);
	}
	if (!path.empty())
		return getMaterial(loadAsset(path)); // editor / loose-content fallback
	return nullptr;
}

const TextureAsset* ContentManager::resolveTextureRef(HE::UUID bakedId, const std::string& path)
{
	if (bakedId != HE::UUID{})
	{
		ensureResident(bakedId);
		return getTexture(bakedId);
	}
	if (!path.empty())
		return getTexture(loadAsset(path));
	return nullptr;
}

// ─── expandFrontier (reference-graph closure via baked UUID refs) ─────────────
void ContentManager::expandFrontier(HE::UUID id)
{
	auto enqueue = [&](HE::UUID dep) {
		// loadAssetAsync(uuid) itself skips already-resident + non-mounted UUIDs
		// and coalesces duplicates, so this is safe to call unconditionally.
		if (dep != HE::UUID{} && !isLoaded(dep)) loadAssetAsync(dep);
	};
	switch (assetType(id))
	{
	case HE::AssetType::StaticMesh:
		if (const auto* a = getStaticMesh(id)) enqueue(a->materialId);
		break;
	case HE::AssetType::SkeletalMesh:
		if (const auto* a = getSkeletalMesh(id)) enqueue(a->materialId);
		break;
	case HE::AssetType::Material:
		if (const auto* a = getMaterial(id))
		{
			enqueue(a->shaderId);
			for (HE::UUID t : a->textureIds) enqueue(t);
		}
		break;
	default:
		break;
	}
}

// ─── readMountedEntry (raw, non-asset payload) ────────────────────────────────
std::vector<uint8_t> ContentManager::readMountedEntry(HE::UUID id)
{
	const auto it = m_pakResidency.find(id);
	if (it == m_pakResidency.end()) return {};
	MountedPak& mount = m_mounts[it->second];
	if (!mount.reader) return {};
	return mount.reader->readEntry(id, mount.encrypted ? mount.key.data() : nullptr);
}

// ─── saveAsset ────────────────────────────────────────────────────────────────
bool ContentManager::saveAsset(RuntimeAsset& asset)
{
	// First save of a fresh asset — mint its permanent identity now so the
	// META chunk never hits disk without one.
	if (asset.id == HE::UUID{})
		asset.id = HE::UUID::generate();

	const std::string fullPath = m_contentRoot + "/" + asset.path;
	const uint16_t    typeId   = static_cast<uint16_t>(asset.type);

	HAsset::Writer w;

	// META chunk — common to all
	{ auto m = buildMetaChunk(asset); w.addChunk(HAsset::CHUNK_META, m.data(), m.size()); }

	switch (asset.type)
	{
	case HE::AssetType::StaticMesh:
	{
		auto& a = static_cast<StaticMeshAsset&>(asset);
		{ std::vector<uint8_t> b; HAsset::Writer::appendString(b,a.materialPath); w.addChunk(HAsset::CHUNK_MREF,b.data(),b.size()); }
		{ std::vector<uint8_t> b; HAsset::Writer::appendVec(b,a.vertices);   w.addChunk(HAsset::CHUNK_VERT,b.data(),b.size()); }
		{ std::vector<uint8_t> b; HAsset::Writer::appendVec(b,a.indices);    w.addChunk(HAsset::CHUNK_INDX,b.data(),b.size()); }
		{ std::vector<uint8_t> b; HAsset::Writer::appendVec(b,a.normals);    w.addChunk(HAsset::CHUNK_NORM,b.data(),b.size()); }
		{ std::vector<uint8_t> b; HAsset::Writer::appendVec(b,a.uvs);        w.addChunk(HAsset::CHUNK_TEXC,b.data(),b.size()); }
		break;
	}
	case HE::AssetType::SkeletalMesh:
	{
		auto& a = static_cast<SkeletalMeshAsset&>(asset);
		{ std::vector<uint8_t> b; HAsset::Writer::appendString(b,a.materialPath); w.addChunk(HAsset::CHUNK_MREF,b.data(),b.size()); }
		{ std::vector<uint8_t> b; HAsset::Writer::appendVec(b,a.vertices);    w.addChunk(HAsset::CHUNK_VERT,b.data(),b.size()); }
		{ std::vector<uint8_t> b; HAsset::Writer::appendVec(b,a.indices);     w.addChunk(HAsset::CHUNK_INDX,b.data(),b.size()); }
		{ std::vector<uint8_t> b; HAsset::Writer::appendVec(b,a.normals);     w.addChunk(HAsset::CHUNK_NORM,b.data(),b.size()); }
		{ std::vector<uint8_t> b; HAsset::Writer::appendVec(b,a.uvs);         w.addChunk(HAsset::CHUNK_TEXC,b.data(),b.size()); }
		{ std::vector<uint8_t> b; HAsset::Writer::appendVec(b,a.boneIDs);     w.addChunk(HAsset::CHUNK_BONE,b.data(),b.size()); }
		{ std::vector<uint8_t> b; HAsset::Writer::appendVec(b,a.boneWeights); w.addChunk(HAsset::CHUNK_BWGT,b.data(),b.size()); }
		if (!a.skeleton.empty())
		{
			std::vector<uint8_t> b;
			HAsset::Writer::appendPOD(b, static_cast<uint32_t>(a.skeleton.size()));
			for (const auto& j : a.skeleton)
			{
				HAsset::Writer::appendString(b, j.name);
				HAsset::Writer::appendPOD(b, j.parent);
				for (float f : j.inverseBindMatrix) HAsset::Writer::appendPOD(b, f);
			}
			w.addChunk(HAsset::CHUNK_SKEL, b.data(), b.size());
		}
		break;
	}
	case HE::AssetType::Texture:
	{
		auto& a = static_cast<TextureAsset&>(asset);
		{ std::vector<uint8_t> b; HAsset::Writer::appendPOD(b,a.width); HAsset::Writer::appendPOD(b,a.height); HAsset::Writer::appendPOD(b,a.channels); w.addChunk(HAsset::CHUNK_TXMI,b.data(),b.size()); }
		w.addChunk(HAsset::CHUNK_PIXL, a.data.data(), a.data.size());
		break;
	}
	case HE::AssetType::Material:
	{
		auto& a = static_cast<MaterialAsset&>(asset);
		std::vector<uint8_t> b; HAsset::Writer::appendString(b,a.shaderPath); HAsset::Writer::appendVec(b,a.texturePaths);
		HAsset::Writer::appendPOD(b,a.baseColor[0]); HAsset::Writer::appendPOD(b,a.baseColor[1]); HAsset::Writer::appendPOD(b,a.baseColor[2]);
		HAsset::Writer::appendPOD(b,a.metallic); HAsset::Writer::appendPOD(b,a.roughness);
		HAsset::Writer::appendPOD(b,a.opacity);
		w.addChunk(HAsset::CHUNK_MTRL,b.data(),b.size());
		break;
	}
	case HE::AssetType::Scene:
	{
		auto& a = static_cast<SceneAsset&>(asset);
		std::vector<uint8_t> b; HAsset::Writer::appendVec(b,a.objectPaths);
		w.addChunk(HAsset::CHUNK_SCNE,b.data(),b.size());
		break;
	}
	case HE::AssetType::Script:
	{
		auto& a = static_cast<ScriptAsset&>(asset);
		w.addChunk(HAsset::CHUNK_SRC, a.sourceCode.data(), a.sourceCode.size());
		break;
	}
	case HE::AssetType::Audio:
	{
		auto& a = static_cast<AudioAsset&>(asset);
		{ std::vector<uint8_t> b; HAsset::Writer::appendPOD(b,a.sampleRate); HAsset::Writer::appendPOD(b,a.channels); w.addChunk(HAsset::CHUNK_AUMI,b.data(),b.size()); }
		w.addChunk(HAsset::CHUNK_PCMD, a.audioData.data(), a.audioData.size());
		break;
	}
	case HE::AssetType::Font:
	{
		auto& a = static_cast<FontAsset&>(asset);
		{ std::vector<uint8_t> b; HAsset::Writer::appendPOD(b,a.size); w.addChunk(HAsset::CHUNK_FNTI,b.data(),b.size()); }
		w.addChunk(HAsset::CHUNK_FNTD, a.fontData.data(), a.fontData.size());
		break;
	}
	case HE::AssetType::Shader:
	{
		auto& a = static_cast<ShaderAsset&>(asset);
		w.addChunk(HAsset::CHUNK_SRC, a.sourceCode.data(), a.sourceCode.size());
		break;
	}
	case HE::AssetType::AnimationClip:
	{
		auto& a = static_cast<AnimationClipAsset&>(asset);
		std::vector<uint8_t> b;
		HAsset::Writer::appendPOD(b, a.duration);
		HAsset::Writer::appendPOD(b, static_cast<uint32_t>(a.channels.size()));
		for (const auto& ch : a.channels)
		{
			HAsset::Writer::appendPOD(b, ch.jointIndex);
			HAsset::Writer::appendPOD(b, static_cast<uint8_t>(ch.path));
			HAsset::Writer::appendVec(b, ch.times);
			HAsset::Writer::appendVec(b, ch.values);
		}
		w.addChunk(HAsset::CHUNK_ANIM, b.data(), b.size());
		break;
	}
	default:
		return false;
	}

	return w.write(fullPath, typeId);
}

// ─── Typed getters ───────────────────────────────────────────────────────────
template<typename T>
static const T* lookupAsset(const std::unordered_map<HE::UUID, SlotHandle>& index,
                            const SlotMap<T>& map, HE::UUID id)
{
	auto it = index.find(id);
	if (it == index.end()) return nullptr;
	// SlotHandles are per-SlotMap, so the same {index,generation} can be valid in
	// several maps. Confirm the stored asset really carries this UUID, otherwise a
	// wrong-type lookup (e.g. getStaticMesh on a material id) would alias.
	const T* a = map.get(it->second);
	return (a && a->id == id) ? a : nullptr;
}

const StaticMeshAsset*    ContentManager::getStaticMesh(HE::UUID id) const    { return lookupAsset(m_handleToUUID, m_staticMeshAssets, id); }
const SkeletalMeshAsset*  ContentManager::getSkeletalMesh(HE::UUID id) const  { return lookupAsset(m_handleToUUID, m_skeletalMeshAssets, id); }
const TextureAsset*       ContentManager::getTexture(HE::UUID id) const       { return lookupAsset(m_handleToUUID, m_textureAssets, id); }
const MaterialAsset*      ContentManager::getMaterial(HE::UUID id) const      { return lookupAsset(m_handleToUUID, m_materialAssets, id); }
const AudioAsset*         ContentManager::getAudio(HE::UUID id) const         { return lookupAsset(m_handleToUUID, m_audioAssets, id); }
const ScriptAsset*        ContentManager::getScript(HE::UUID id) const        { return lookupAsset(m_handleToUUID, m_scriptAssets, id); }
const ShaderAsset*        ContentManager::getShader(HE::UUID id) const        { return lookupAsset(m_handleToUUID, m_shaderAssets, id); }
const PrefabAsset*        ContentManager::getPrefab(HE::UUID id) const        { return lookupAsset(m_handleToUUID, m_prefabAssets, id); }
const AnimationClipAsset*      ContentManager::getAnimationClip(HE::UUID id) const      { return lookupAsset(m_handleToUUID, m_animClipAssets,     id); }
const PropertyAnimClipAsset*   ContentManager::getPropertyAnimClip(HE::UUID id) const   { return lookupAsset(m_handleToUUID, m_propAnimClipAssets, id); }

MaterialAsset* ContentManager::getMaterialMutable(HE::UUID id)
{
	auto it = m_handleToUUID.find(id);
	if (it == m_handleToUUID.end()) return nullptr;
	MaterialAsset* a = m_materialAssets.get(it->second);
	return (a && a->id == id) ? a : nullptr; // reject wrong-type aliasing
}

// ─── Runtime (in-memory) asset registration ──────────────────────────────────
template<typename T>
HE::UUID ContentManager::registerRuntimeAsset(SlotMap<T>& map, T asset, HE::AssetType type)
{
	if (asset.id == HE::UUID{})
		asset.id = HE::UUID::generate();
	asset.type = type;

	const HE::UUID    id   = asset.id;
	const std::string path = asset.path;

	SlotHandle handle = map.insert(std::move(asset));
	m_handleToUUID[id]   = handle;
	m_assetTypeIndex[id] = type;
	if (!path.empty())
		m_pathToUUID[path] = id; // virtual path, optional
	return id;
}

template<typename T>
bool ContentManager::replaceRuntimeAsset(SlotMap<T>& map, HE::UUID id, T asset)
{
	auto it = m_handleToUUID.find(id);
	if (it == m_handleToUUID.end())
		return false;
	T* existing = map.get(it->second);
	// Guard against a handle that is valid in a different SlotMap (the UUID
	// belongs to another asset type) — the stored id must match.
	if (!existing || !(existing->id == id))
		return false;

	// Only the payload changes; keep the asset's identity + (virtual) path.
	asset.id   = id;
	asset.type = existing->type;
	asset.name = existing->name;
	asset.path = existing->path;
	*existing  = std::move(asset);
	return true;
}

HE::UUID ContentManager::registerStaticMesh(StaticMeshAsset asset)       { return registerRuntimeAsset(m_staticMeshAssets,  std::move(asset), HE::AssetType::StaticMesh);    }
HE::UUID ContentManager::registerSkeletalMesh(SkeletalMeshAsset asset)   { return registerRuntimeAsset(m_skeletalMeshAssets, std::move(asset), HE::AssetType::SkeletalMesh); }
HE::UUID ContentManager::registerTexture(TextureAsset asset)             { return registerRuntimeAsset(m_textureAssets,     std::move(asset), HE::AssetType::Texture);       }
HE::UUID ContentManager::registerMaterial(MaterialAsset asset)           { return registerRuntimeAsset(m_materialAssets,    std::move(asset), HE::AssetType::Material);      }
HE::UUID ContentManager::registerPrefab(PrefabAsset asset)               { return registerRuntimeAsset(m_prefabAssets,      std::move(asset), HE::AssetType::Prefab);        }
HE::UUID ContentManager::registerAudio(AudioAsset asset)                 { return registerRuntimeAsset(m_audioAssets,       std::move(asset), HE::AssetType::Audio);         }
HE::UUID ContentManager::registerScript(ScriptAsset asset)               { return registerRuntimeAsset(m_scriptAssets,      std::move(asset), HE::AssetType::Script);        }
HE::UUID ContentManager::registerAnimationClip(AnimationClipAsset asset)       { return registerRuntimeAsset(m_animClipAssets,     std::move(asset), HE::AssetType::AnimationClip);     }
HE::UUID ContentManager::registerPropertyAnimClip(PropertyAnimClipAsset asset) { return registerRuntimeAsset(m_propAnimClipAssets, std::move(asset), HE::AssetType::PropertyAnimClip); }

bool ContentManager::replaceStaticMesh(HE::UUID id, StaticMeshAsset asset) { return replaceRuntimeAsset(m_staticMeshAssets, id, std::move(asset)); }
bool ContentManager::replaceTexture(HE::UUID id, TextureAsset asset)       { return replaceRuntimeAsset(m_textureAssets,    id, std::move(asset)); }
bool ContentManager::replaceMaterial(HE::UUID id, MaterialAsset asset)     { return replaceRuntimeAsset(m_materialAssets,   id, std::move(asset)); }

// ─── Pin bookkeeping ─────────────────────────────────────────────────────────
void ContentManager::pinAsset(HE::UUID id)
{
	++m_pinCounts[id];
}

void ContentManager::unpinAsset(HE::UUID id)
{
	auto it = m_pinCounts.find(id);
	if (it != m_pinCounts.end() && --it->second <= 0)
		m_pinCounts.erase(it);
}

bool ContentManager::isPinned(HE::UUID id) const
{
	auto it = m_pinCounts.find(id);
	return it != m_pinCounts.end() && it->second > 0;
}

// ─── unloadAsset ─────────────────────────────────────────────────────────────
bool ContentManager::unloadAsset(HE::UUID id)
{
	if (isPinned(id))
		return false; // active AssetRef handle(s) hold a pin — refuse eviction

	auto it = m_handleToUUID.find(id);
	if (it == m_handleToUUID.end())
		return false;
	const SlotHandle handle = it->second;

	// Remove from whichever map actually holds *this* asset. The id check guards
	// against the same SlotHandle being valid in another map (see lookupAsset).
	auto tryRemove = [&](auto& map) -> bool
	{
		auto* a = map.get(handle);
		if (!a || !(a->id == id)) return false;
		map.remove(handle);
		return true;
	};

	const bool removed =
		tryRemove(m_staticMeshAssets)   || tryRemove(m_skeletalMeshAssets) ||
		tryRemove(m_textureAssets)      || tryRemove(m_materialAssets)     ||
		tryRemove(m_sceneAssets)        || tryRemove(m_scriptAssets)       ||
		tryRemove(m_audioAssets)        || tryRemove(m_fontAssets)         ||
		tryRemove(m_shaderAssets)       || tryRemove(m_animClipAssets) ||
		tryRemove(m_propAnimClipAssets);
	if (!removed)
		return false;

	for (auto pit = m_pathToUUID.begin(); pit != m_pathToUUID.end(); ++pit)
		if (pit->second == id) { m_pathMtime.erase(pit->first); m_pathToUUID.erase(pit); break; }
	m_handleToUUID.erase(id);
	m_assetTypeIndex.erase(id);
	return true;
}

// ─── isLoaded ────────────────────────────────────────────────────────────────
bool ContentManager::isLoaded(HE::UUID id) const
{
	return m_handleToUUID.contains(id);
}

bool ContentManager::isLoaded(const std::string& relativePath) const
{
	return m_pathToUUID.contains(relativePath);
}

// ─── Asset enumeration ───────────────────────────────────────────────────────
std::vector<HE::UUID> ContentManager::enumerateIds() const
{
	std::vector<HE::UUID> out;
	out.reserve(m_handleToUUID.size());
	for (const auto& [id, _] : m_handleToUUID)
		out.push_back(id);
	return out;
}

std::vector<HE::UUID> ContentManager::enumerateIds(HE::AssetType type) const
{
	std::vector<HE::UUID> out;
	for (const auto& [id, t] : m_assetTypeIndex)
		if (t == type)
			out.push_back(id);
	return out;
}

// ─── assetType ───────────────────────────────────────────────────────────────
HE::AssetType ContentManager::assetType(HE::UUID id) const
{
	auto it = m_assetTypeIndex.find(id);
	return it != m_assetTypeIndex.end() ? it->second : HE::AssetType::Unknown;
}

// ─── pollHotReload ───────────────────────────────────────────────────────────
std::vector<HE::UUID> ContentManager::pollHotReload()
{
	namespace fs = std::filesystem;
	std::vector<HE::UUID> changed;

	// Snapshot paths — unloadAsset/loadAsset mutate m_pathMtime during the loop.
	std::vector<std::string> paths;
	paths.reserve(m_pathMtime.size());
	for (const auto& [p, _] : m_pathMtime)
		paths.push_back(p);

	for (const auto& relPath : paths)
	{
		const std::string fullPath = m_contentRoot + "/" + relPath;
		std::error_code ec;
		const auto mtime = fs::last_write_time(fullPath, ec);
		if (ec) continue; // file deleted or inaccessible

		auto storedIt = m_pathMtime.find(relPath);
		if (storedIt == m_pathMtime.end() || mtime == storedIt->second)
			continue; // not in map yet or unchanged

		// Skip files that aren't valid .hasset yet (mid-write / partial save).
		// Avoids evicting the live asset before the new version is readable.
		if (getAssetType(fullPath) == HE::AssetType::Unknown) continue;

		// File changed — unload old entry (removes from m_pathMtime) then reload.
		auto pathIt = m_pathToUUID.find(relPath);
		if (pathIt == m_pathToUUID.end()) continue;
		unloadAsset(pathIt->second);

		const HE::UUID newId = loadAsset(relPath); // re-records mtime
		if (!(newId == HE::UUID{}))
			changed.push_back(newId);
	}
	return changed;
}

// ─── loadAssetFromMemory ─────────────────────────────────────────────────────
HE::UUID ContentManager::loadAssetFromMemory(const std::vector<uint8_t>& hassetData)
{
	HAsset::Reader reader;
	if (!reader.openData(hassetData)) return HE::UUID{};

	// Recover the embedded asset path so the registered asset keeps a sensible
	// path (and path→UUID key), then delegate to parseAndRegisterAsset — the
	// single, complete per-type parser. Previously this function carried its own
	// copy of the switch that only handled a subset of types, so SkeletalMesh /
	// Scene / Script / Font / Shader entries loaded from a .hpak were silently
	// dropped. There is now one parser shared by the disk and in-memory paths.
	std::string relativePath;
	if (const auto* metaChunk = reader.findChunk(HAsset::CHUNK_META))
	{
		HE::UUID id; std::string name;
		readMetaChunk(*metaChunk, reader.header().version, id, name, relativePath);
	}

	return parseAndRegisterAsset(relativePath, /*fullPath=*/std::string{}, reader);
}

// ─── loadPak ─────────────────────────────────────────────────────────────────
bool ContentManager::loadPak(const std::string& path, const uint8_t key[32])
{
	HpakReader reader;
	if (!reader.open(path)) return false;

	for (const auto& id : reader.enumerate())
	{
		if (isLoaded(id)) continue;
		auto data = reader.readEntry(id, key);
		if (!data.empty())
			loadAssetFromMemory(data);
	}
	return true;
}

// ─── mountPak / ensureResident (on-demand streaming) ──────────────────────────
// Special members live here (not in the header) because ContentManager owns
// std::unique_ptr<HpakReader> mounts and HpakReader is only complete in this TU.
ContentManager::ContentManager() { initDefaultAssets(); }
ContentManager::ContentManager(std::string contentPath)
	: m_contentRoot(std::move(contentPath)) { initDefaultAssets(); }
ContentManager::~ContentManager() = default;

bool ContentManager::mountPak(const std::string& path, const uint8_t key[32])
{
	auto reader = std::make_unique<HpakReader>();
	if (!reader->open(path)) return false;

	MountedPak mount;
	mount.path      = path;
	mount.encrypted = (key != nullptr);
	if (key) std::memcpy(mount.key.data(), key, 32);

	const size_t index = m_mounts.size();
	// Register every UUID this archive provides. A later mount overwrites the
	// residency entry for an existing UUID → it shadows earlier mounts (overlay),
	// while new UUIDs are simply added.
	for (const auto& id : reader->enumerate())
		m_pakResidency[id] = index;

	mount.reader = std::move(reader);
	m_mounts.push_back(std::move(mount));
	return true;
}

bool ContentManager::ensureResident(HE::UUID id)
{
	if (isLoaded(id)) return true;

	const auto it = m_pakResidency.find(id);
	if (it == m_pakResidency.end()) return false;

	MountedPak& mount = m_mounts[it->second];
	if (!mount.reader) return false;
	auto data = mount.reader->readEntry(id, mount.encrypted ? mount.key.data() : nullptr);
	if (data.empty()) return false;

	return loadAssetFromMemory(data) != HE::UUID{};
}

// ─── initDefaultAssets ───────────────────────────────────────────────────────
void ContentManager::initDefaultAssets()
{
	// ── Default cube mesh (kDefaultCubeMeshId) ────────────────────────────────
	// Unit cube: 24 vertices (6 faces × 4 verts), each with a unique normal so
	// that hard-edge face normals are preserved.  Identical geometry to the
	// OpenGL/Metal backend built-in fallback cubes.
	// Vertex order: pairs of opposite faces, interleaved per face pair.
	static const float kCubePos[] = {
		// +X                           // -X
		 0.5f,-0.5f,-0.5f,  -0.5f,-0.5f, 0.5f,
		 0.5f, 0.5f,-0.5f,  -0.5f, 0.5f, 0.5f,
		 0.5f, 0.5f, 0.5f,  -0.5f, 0.5f,-0.5f,
		 0.5f,-0.5f, 0.5f,  -0.5f,-0.5f,-0.5f,
		// +Y                           // -Y
		-0.5f, 0.5f,-0.5f,  -0.5f,-0.5f, 0.5f,
		-0.5f, 0.5f, 0.5f,  -0.5f,-0.5f,-0.5f,
		 0.5f, 0.5f, 0.5f,   0.5f,-0.5f,-0.5f,
		 0.5f, 0.5f,-0.5f,   0.5f,-0.5f, 0.5f,
		// +Z                           // -Z
		-0.5f,-0.5f, 0.5f,   0.5f,-0.5f,-0.5f,
		 0.5f,-0.5f, 0.5f,  -0.5f,-0.5f,-0.5f,
		 0.5f, 0.5f, 0.5f,  -0.5f, 0.5f,-0.5f,
		-0.5f, 0.5f, 0.5f,   0.5f, 0.5f,-0.5f,
	};
	static const float kCubeNrm[] = {
		// +X                     // -X
		 1,0,0,  -1,0,0,
		 1,0,0,  -1,0,0,
		 1,0,0,  -1,0,0,
		 1,0,0,  -1,0,0,
		// +Y                     // -Y
		 0,1,0,   0,-1,0,
		 0,1,0,   0,-1,0,
		 0,1,0,   0,-1,0,
		 0,1,0,   0,-1,0,
		// +Z                     // -Z
		 0,0,1,   0,0,-1,
		 0,0,1,   0,0,-1,
		 0,0,1,   0,0,-1,
		 0,0,1,   0,0,-1,
	};
	static const uint32_t kCubeIdx[] = {
		 0, 2, 4,  0, 4, 6,    1, 3, 5,  1, 5, 7,
		 8,10,12,  8,12,14,    9,11,13,  9,13,15,
		16,18,20, 16,20,22,   17,19,21, 17,21,23,
	};
	static constexpr int kCubeVerts = 24;

	StaticMeshAsset cube;
	cube.id   = HE::kDefaultCubeMeshId;
	cube.name = "DefaultCube";
	cube.path = "mem://default_cube";
	cube.vertices.assign(kCubePos, kCubePos + kCubeVerts * 3);
	cube.normals .assign(kCubeNrm, kCubeNrm + kCubeVerts * 3);
	cube.indices .assign(kCubeIdx, kCubeIdx + sizeof(kCubeIdx)/sizeof(kCubeIdx[0]));
	registerStaticMesh(std::move(cube));

	// ── Default billboard quad mesh (kDefaultQuadMeshId) ─────────────────────
	// 1×1 quad in the XY plane, normal +Z. Used as the default particle mesh.
	// CCW winding viewed from +Z; two triangles (0,1,2) and (0,2,3).
	static const float kQuadPos[] = {
		-0.5f,-0.5f, 0.0f,
		 0.5f,-0.5f, 0.0f,
		 0.5f, 0.5f, 0.0f,
		-0.5f, 0.5f, 0.0f,
	};
	static const float kQuadNrm[] = {
		0,0,1,  0,0,1,  0,0,1,  0,0,1,
	};
	static const float kQuadUV[] = {
		0,0,  1,0,  1,1,  0,1,
	};
	static const uint32_t kQuadIdx[] = { 0,1,2,  0,2,3 };

	StaticMeshAsset quad;
	quad.id   = HE::kDefaultQuadMeshId;
	quad.name = "DefaultQuad";
	quad.path = "mem://default_quad";
	quad.vertices.assign(kQuadPos, kQuadPos + 12);
	quad.normals .assign(kQuadNrm, kQuadNrm + 12);
	quad.uvs     .assign(kQuadUV,  kQuadUV  + 8);
	quad.indices .assign(kQuadIdx, kQuadIdx + 6);
	registerStaticMesh(std::move(quad));

	// ── Default snowflake mesh (kDefaultSnowflakeMeshId) ──────────────────────
	// Flat 6-pointed star in the XY plane (normal +Z): centre vertex + 12 rim verts
	// alternating outer/inner radius, 12 triangles fanned from the centre. Billboarded
	// by the weather system so snow reads as a flake shape instead of a square.
	{
		StaticMeshAsset flake;
		flake.id   = HE::kDefaultSnowflakeMeshId;
		flake.name = "DefaultSnowflake";
		flake.path = "mem://default_snowflake";
		flake.vertices = { 0.0f, 0.0f, 0.0f };
		flake.normals  = { 0.0f, 0.0f, 1.0f };
		flake.uvs      = { 0.5f, 0.5f };
		constexpr int kPoints = 6;
		for (int i = 0; i < kPoints * 2; ++i)
		{
			const float r   = (i % 2 == 0) ? 0.5f : 0.2f; // outer tip / inner notch
			const float ang = (3.14159265f / kPoints) * static_cast<float>(i);
			const float x = std::cos(ang) * r;
			const float y = std::sin(ang) * r;
			flake.vertices.insert(flake.vertices.end(), { x, y, 0.0f });
			flake.normals .insert(flake.normals.end(),  { 0.0f, 0.0f, 1.0f });
			flake.uvs     .insert(flake.uvs.end(),      { x + 0.5f, y + 0.5f });
		}
		for (uint32_t i = 0; i < kPoints * 2; ++i)
		{
			const uint32_t a = 1u + i;
			const uint32_t b = 1u + ((i + 1) % (kPoints * 2));
			flake.indices.insert(flake.indices.end(), { 0u, a, b });
		}
		registerStaticMesh(std::move(flake));
	}

	// ── Default white texture (kDefaultWhiteTextureId) ────────────────────────
	// 1×1 RGBA8 opaque white — neutral placeholder; multiplied with any colour
	// leaves it unchanged.
	TextureAsset white;
	white.id       = HE::kDefaultWhiteTextureId;
	white.name     = "DefaultWhite";
	white.path     = "mem://default_white";
	white.data     = { 255, 255, 255, 255 };
	white.width    = 1;
	white.height   = 1;
	white.channels = 4;
	registerTexture(std::move(white));

	// ── Default material (kDefaultMaterialId) ─────────────────────────────────
	// PBR defaults: white base colour, non-metallic, mid roughness, fully opaque.
	MaterialAsset mat;
	mat.id         = HE::kDefaultMaterialId;
	mat.name       = "DefaultMaterial";
	mat.path       = "mem://default_material";
	registerMaterial(std::move(mat));

	// ── Default grid texture (kDefaultGridTextureId) ──────────────────────────
	// 128×128 RGBA8: cool light-grey cells with thin slate-blue grid lines and
	// accent corner dots. Tile-friendly; gives terrain a clean technical look.
	{
		constexpr int kGridSize = 128;
		constexpr int kCell     = 32; // 4 cells across the texture
		// Background: very light cool grey
		constexpr uint8_t kBgR = 228, kBgG = 231, kBgB = 238;
		// Primary grid lines: slate blue-grey
		constexpr uint8_t kLnR = 108, kLnG = 116, kLnB = 138;
		// Corner accent dots: slightly lighter than the line colour
		constexpr uint8_t kDtR = 148, kDtG = 155, kDtB = 172;
		std::vector<uint8_t> pixels;
		pixels.reserve(kGridSize * kGridSize * 4);
		for (int y = 0; y < kGridSize; ++y) {
			for (int x = 0; x < kGridSize; ++x) {
				// Single-pixel grid lines on every cell boundary
				bool edge = (x % kCell == 0) || (y % kCell == 0);
				// 3×3 accent dot at every cell corner
				bool corner = ((x % kCell) <= 1 && (x % kCell) >= 0) &&
				              ((y % kCell) <= 1 && (y % kCell) >= 0) &&
				              (x % kCell + y % kCell < 2);
				uint8_t r, g, b;
				if (corner)       { r = kDtR; g = kDtG; b = kDtB; }
				else if (edge)    { r = kLnR; g = kLnG; b = kLnB; }
				else              { r = kBgR; g = kBgG; b = kBgB; }
				pixels.push_back(r);
				pixels.push_back(g);
				pixels.push_back(b);
				pixels.push_back(255);
			}
		}
		TextureAsset grid;
		grid.id       = HE::kDefaultGridTextureId;
		grid.name     = "DefaultGridTexture";
		grid.path     = "mem://default_grid_tex";
		grid.data     = std::move(pixels);
		grid.width    = kGridSize;
		grid.height   = kGridSize;
		grid.channels = 4;
		registerTexture(std::move(grid));
	}

	// ── Default terrain material (kDefaultTerrainMaterialId) ─────────────────
	// Flat neutral grey, no texture — keeps terrain readable without visual noise.
	MaterialAsset terrainMat;
	terrainMat.id            = HE::kDefaultTerrainMaterialId;
	terrainMat.name          = "DefaultTerrainMaterial";
	terrainMat.path          = "mem://default_terrain_material";
	terrainMat.baseColor[0]  = 0.50f;
	terrainMat.baseColor[1]  = 0.52f;
	terrainMat.baseColor[2]  = 0.50f;
	terrainMat.roughness     = 0.8f;
	terrainMat.doubleSided   = true;
	registerMaterial(std::move(terrainMat));
}