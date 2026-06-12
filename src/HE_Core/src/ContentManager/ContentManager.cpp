#include "ContentManager/ContentManager.h"
#include "ContentManager/HAsset.h"
#include "Diagnostics/Logger.h"
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
	m_pathToUUID[relativePath] = id;
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

// ─── unloadAsset ─────────────────────────────────────────────────────────────
bool ContentManager::unloadAsset(HE::UUID id)
{
	if (!m_handleToUUID.contains(id))
		return false;

	SlotHandle handle = m_handleToUUID[id];

	for (auto it = m_pathToUUID.begin(); it != m_pathToUUID.end(); ++it)
		if (it->second == id) { m_pathToUUID.erase(it); break; }

	m_handleToUUID.erase(id);

	if (m_staticMeshAssets.isValid(handle))   { m_staticMeshAssets.remove(handle);   return true; }
	if (m_skeletalMeshAssets.isValid(handle)) { m_skeletalMeshAssets.remove(handle); return true; }
	if (m_textureAssets.isValid(handle))      { m_textureAssets.remove(handle);      return true; }
	if (m_materialAssets.isValid(handle))     { m_materialAssets.remove(handle);     return true; }
	if (m_sceneAssets.isValid(handle))        { m_sceneAssets.remove(handle);        return true; }
	if (m_scriptAssets.isValid(handle))       { m_scriptAssets.remove(handle);       return true; }
	if (m_audioAssets.isValid(handle))        { m_audioAssets.remove(handle);        return true; }
	if (m_fontAssets.isValid(handle))         { m_fontAssets.remove(handle);         return true; }
	if (m_shaderAssets.isValid(handle))       { m_shaderAssets.remove(handle);       return true; }
	return false;
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