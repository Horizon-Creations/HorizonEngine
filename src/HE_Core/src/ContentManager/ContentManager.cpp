#include "ContentManager/ContentManager.h"
#include "ContentManager/HAsset.h"
#include "Diagnostics/Logger.h"
#include "Diagnostics/Profiler.h"
#include <cstring>

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

HE::UUID ContentManager::loadAsset(const std::string& relativePath)
{
	HE_PROFILE_SCOPE_N("ContentManager::load");
	if (isLoaded(relativePath))
		return m_pathToUUID.at(relativePath);

	const std::string fullPath = m_contentRoot + "/" + relativePath;

	HAsset::Reader reader;
	if (!reader.open(fullPath))
		return HE::UUID();

	const HE::AssetType type = static_cast<HE::AssetType>(reader.assetType());

	const auto* metaChunk = reader.findChunk(HAsset::CHUNK_META);
	if (!metaChunk) return HE::UUID();

	HE::UUID    id;
	std::string assetName, assetPath;
	if (!readMetaChunk(*metaChunk, reader.header().version, id, assetName, assetPath))
		return HE::UUID();

	if (id == HE::UUID{})
	{
		// v1 file without persisted UUID — references to this asset will not
		// survive a restart until the file is re-saved in the new format.
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
		if (const auto* c = reader.findChunk(HAsset::CHUNK_VERT)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.vertices); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_INDX)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.indices); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_NORM)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.normals); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_TEXC)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.uvs); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_BONE)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.boneIDs); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_BWGT)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.boneWeights); }
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
			// PBR scalars — optional tail; older materials keep the defaults.
			HAsset::Reader::readPOD(c->data,o,a.baseColor[0]);
			HAsset::Reader::readPOD(c->data,o,a.baseColor[1]);
			HAsset::Reader::readPOD(c->data,o,a.baseColor[2]);
			HAsset::Reader::readPOD(c->data,o,a.metallic);
			HAsset::Reader::readPOD(c->data,o,a.roughness);
			HAsset::Reader::readPOD(c->data,o,a.opacity); // optional tail; defaults to 1.0
		}
		handle = m_materialAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::Scene:
	{
		SceneAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_SCNE)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.objectPaths); }
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
	default:
		return HE::UUID();
	}

	m_handleToUUID[id]         = handle;
	m_assetTypeIndex[id]       = type;
	m_pathToUUID[relativePath] = id;
	{
		std::error_code ec;
		auto mtime = std::filesystem::last_write_time(fullPath, ec);
		if (!ec) m_pathMtime[relativePath] = mtime;
	}
	return id;
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

const StaticMeshAsset*   ContentManager::getStaticMesh(HE::UUID id) const   { return lookupAsset(m_handleToUUID, m_staticMeshAssets, id); }
const SkeletalMeshAsset* ContentManager::getSkeletalMesh(HE::UUID id) const { return lookupAsset(m_handleToUUID, m_skeletalMeshAssets, id); }
const TextureAsset*      ContentManager::getTexture(HE::UUID id) const      { return lookupAsset(m_handleToUUID, m_textureAssets, id); }
const MaterialAsset*     ContentManager::getMaterial(HE::UUID id) const     { return lookupAsset(m_handleToUUID, m_materialAssets, id); }
const AudioAsset*        ContentManager::getAudio(HE::UUID id) const        { return lookupAsset(m_handleToUUID, m_audioAssets, id); }
const ScriptAsset*       ContentManager::getScript(HE::UUID id) const       { return lookupAsset(m_handleToUUID, m_scriptAssets, id); }
const ShaderAsset*       ContentManager::getShader(HE::UUID id) const       { return lookupAsset(m_handleToUUID, m_shaderAssets, id); }
const PrefabAsset*       ContentManager::getPrefab(HE::UUID id) const       { return lookupAsset(m_handleToUUID, m_prefabAssets, id); }

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

HE::UUID ContentManager::registerStaticMesh(StaticMeshAsset asset) { return registerRuntimeAsset(m_staticMeshAssets, std::move(asset), HE::AssetType::StaticMesh); }
HE::UUID ContentManager::registerTexture(TextureAsset asset)       { return registerRuntimeAsset(m_textureAssets,    std::move(asset), HE::AssetType::Texture);    }
HE::UUID ContentManager::registerMaterial(MaterialAsset asset)     { return registerRuntimeAsset(m_materialAssets,   std::move(asset), HE::AssetType::Material);   }
HE::UUID ContentManager::registerPrefab(PrefabAsset asset)         { return registerRuntimeAsset(m_prefabAssets,     std::move(asset), HE::AssetType::Prefab);     }
HE::UUID ContentManager::registerAudio(AudioAsset asset)           { return registerRuntimeAsset(m_audioAssets,      std::move(asset), HE::AssetType::Audio);      }
HE::UUID ContentManager::registerScript(ScriptAsset asset)         { return registerRuntimeAsset(m_scriptAssets,     std::move(asset), HE::AssetType::Script);     }

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
		tryRemove(m_shaderAssets);
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
}