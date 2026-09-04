#include "ContentManager/ContentManager.h"
#include <cstdint>
#include "MaterialGraph/MaterialGraph.h" // instance sync: switch-permutation regenerate
#include "ContentManager/HAsset.h"
#include "ContentManager/AssetRefRetarget.h" // move/rename: carry path references over
#include "Hpak/HpakReader.h"
#include "Hpak/ProjectExporter.h"        // sceneUuidForPath + the reserved pak entry names
#include "JobSystem/JobSystem.h"
#include "Diagnostics/Logger.h"
#include "Diagnostics/Profiler.h"
#include "Diagnostics/GlobalState.h" // engineContentCacheDir() — the third "Engine/" resolution tier
#include <nlohmann/json.hpp>             // parse the pak's __asset_index__ / __asset_types__
#include <Types/TypeRegistry.h>          // struct/enum defs mirror into the registry on load
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>

// ─── Internal helpers ─────────────────────────────────────────────────────────

// A mesh without texture coordinates (the built-in cube, or imported geometry that
// carried none) leaves vUV = (0,0) at every vertex — which collapses UV-space material
// nodes (Noise/FBM/Checker/TextureSample) and any texturing to a single point (e.g. noise
// renders solid black). Generate box-projection UVs from position + normal so such meshes
// still look right: each vertex projects onto the plane of its dominant normal axis
// (triplanar-style), giving every cube face a clean unwrap. No-op when UVs already match.
static void ensureMeshUVs(StaticMeshAsset& m)
{
    const size_t vcount = m.vertices.size() / 3;
    if (vcount == 0 || m.uvs.size() == vcount * 2) return;

    m.uvs.assign(vcount * 2, 0.0f);
    const bool haveN = m.normals.size() == vcount * 3;
    for (size_t i = 0; i < vcount; ++i)
    {
        const float px = m.vertices[i * 3 + 0];
        const float py = m.vertices[i * 3 + 1];
        const float pz = m.vertices[i * 3 + 2];
        float ax = 0.0f, ay = 0.0f, az = 1.0f; // default to Z-projection when no normals
        if (haveN)
        {
            ax = std::fabs(m.normals[i * 3 + 0]);
            ay = std::fabs(m.normals[i * 3 + 1]);
            az = std::fabs(m.normals[i * 3 + 2]);
        }
        float u, v;
        if (ax >= ay && ax >= az)      { u = pz; v = py; } // X-facing face
        else if (ay >= ax && ay >= az) { u = px; v = pz; } // Y-facing face
        else                           { u = px; v = py; } // Z-facing face
        // Unit primitives span [-0.5, 0.5] → map to [0, 1]; larger meshes just tile.
        m.uvs[i * 2 + 0] = u + 0.5f;
        m.uvs[i * 2 + 1] = v + 0.5f;
    }
}

// META layout: uint16 type, uint64 id.hi, uint64 id.lo, string name, string path,
// and — appended, optional — string sourcePath.
//
// The import source lives in META rather than in a chunk of its own because it is
// the same KIND of fact as name and path: what this asset is, not what it points
// at. That distinction is already load-bearing elsewhere —
// AssetRefScan::hassetHoldsRef skips META wholesale ("the asset's OWN name and
// path") so that a query for a folder does not report every asset inside it as a
// referrer of itself. A source like /Users/me/Art/Meshes/rock.fbx sitting in any
// OTHER chunk would be scanned as reference text, and a "who still uses
// Meshes/?" delete query would substring-match it and name an innocent asset.
//
// Appending is safe for the four readers outside this function that parse META by
// hand (Importer::existingUUID, AssetRefScan::assetUuidOfFile,
// HpakWriter::metaFromHasset, EngineContentPublish::readAssetUuid): every one of
// them reads a prefix and stops. Nothing rebuilds META from parsed fields — the
// move/rename rewrite (AssetRefRetarget::retargetBlob) copies each chunk through
// whole — so the tail survives the operations that touch an asset in place.
//
// One place drops it ON PURPOSE: the packer (HpakWriter's stripImportSourceForPack)
// blanks sourcePath in every packed asset. It is an absolute path on the author's
// machine, it means nothing on a player's, and shipping it hands out their user
// name and directory layout. Only editor Reimport ever reads the field, so a
// packed asset that carries an empty one loses nothing.
static std::vector<uint8_t> buildMetaChunk(const RuntimeAsset& a)
{
    std::vector<uint8_t> buf;
    HAsset::Writer::appendPOD(buf, static_cast<uint16_t>(a.type));
    HAsset::Writer::appendPOD(buf, a.id.hi);
    HAsset::Writer::appendPOD(buf, a.id.lo);
    HAsset::Writer::appendString(buf, a.name);
    HAsset::Writer::appendString(buf, a.path);
    HAsset::Writer::appendString(buf, a.sourcePath);
    return buf;
}

// fileVersion: v1 META has no UUID — idOut stays invalid and the caller
// must generate (and ideally persist) a fresh one.
//
// sourceOut is an append-only TAIL, not a new format version: every asset written
// before it existed simply runs out of bytes there, and the bounds-checked
// readString leaves the string empty instead of failing the whole parse. Treating
// a missing tail as a parse failure would make every pre-existing .hasset in
// every project unloadable. Same idiom the MTRL chunk has used for its own
// growing tail.
static bool readMetaChunk(const HAsset::Reader::Chunk& c, uint16_t fileVersion,
                          HE::UUID& idOut, std::string& nameOut, std::string& pathOut,
                          std::string* sourceOut = nullptr)
{
    size_t off = sizeof(uint16_t); // type already known from file header
    if (fileVersion >= 2)
    {
        if (!HAsset::Reader::readPOD(c.data, off, idOut.hi)) return false;
        if (!HAsset::Reader::readPOD(c.data, off, idOut.lo)) return false;
    }
    if (!HAsset::Reader::readString(c.data, off, nameOut)) return false;
    if (!HAsset::Reader::readString(c.data, off, pathOut)) return false;
    if (sourceOut && !HAsset::Reader::readString(c.data, off, *sourceOut))
        sourceOut->clear();
    return true;
}

// ─── sniffAssetTypeFromFile ───────────────────────────────────────────────────
HE::AssetType ContentManager::sniffAssetTypeFromFile(const std::string& path) const
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
	std::string assetName, assetPath, assetSource;
	if (!readMetaChunk(*metaChunk, reader.header().version, id, assetName, assetPath, &assetSource))
		return HE::UUID();

	if (id == HE::UUID{})
	{
		id = HE::UUID::generate();
		HE_LOG_WARN(Asset, "%s",
			("ContentManager: asset has no persisted UUID (pre-v2 file), generated transient id: " + relativePath).c_str());
	}

	SlotHandle handle{};

	// assetSource is carried into the seven types an importer can produce, and only
	// those: everything else here (scenes, scripts, widgets, graphs, type
	// definitions…) is authored in the editor and can never have a source file.
	// Carrying it is not cosmetic — saveAsset rebuilds META from the in-memory
	// asset, so a material imported from a .hmat and then edited in the Material
	// Editor would lose the source it came from on the next save.
	switch (type)
	{
	case HE::AssetType::StaticMesh:
	{
		StaticMeshAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		a.sourcePath = assetSource;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_MREF)) { size_t o=0; HAsset::Reader::readString(c->data,o,a.materialPath); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_MRFU)) { size_t o=0; HAsset::Reader::readPOD(c->data,o,a.materialId.hi); HAsset::Reader::readPOD(c->data,o,a.materialId.lo); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_INDX)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.indices); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_MVBO))
		{
			// Cooked (pack-time) form: interleaved GPU-ready vertices + baked AABB.
			size_t o = 0;
			HAsset::Reader::readPOD(c->data, o, a.vertexCount);
			for (int i = 0; i < 3; ++i) HAsset::Reader::readPOD(c->data, o, a.boundsMin[i]);
			for (int i = 0; i < 3; ++i) HAsset::Reader::readPOD(c->data, o, a.boundsMax[i]);
			HAsset::Reader::readVec(c->data, o, a.interleaved);
			a.cooked = a.vertexCount > 0 && a.interleaved.size() == static_cast<size_t>(a.vertexCount) * 8;
		}
		else
		{
			// Raw SoA form (loose/editor assets).
			if (const auto* c = reader.findChunk(HAsset::CHUNK_VERT)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.vertices); }
			if (const auto* c = reader.findChunk(HAsset::CHUNK_NORM)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.normals); }
			if (const auto* c = reader.findChunk(HAsset::CHUNK_TEXC)) { size_t o=0; HAsset::Reader::readVec(c->data,o,a.uvs); }
			// Object-space AABB, so the extractor can cull against real bounds (the
			// cooked path already carries a baked AABB). Cheap one-time scan on load.
			if (!a.vertices.empty())
			{
				a.boundsMin[0] = a.boundsMin[1] = a.boundsMin[2] =  1e30f;
				a.boundsMax[0] = a.boundsMax[1] = a.boundsMax[2] = -1e30f;
				for (size_t v = 0; v + 2 < a.vertices.size(); v += 3)
					for (int k = 0; k < 3; ++k)
					{
						a.boundsMin[k] = std::min(a.boundsMin[k], a.vertices[v + k]);
						a.boundsMax[k] = std::max(a.boundsMax[k], a.vertices[v + k]);
					}
			}
			ensureMeshUVs(a); // loose meshes with no TEXC chunk get box-projected UVs
		}
		handle = m_staticMeshAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::SkeletalMesh:
	{
		SkeletalMeshAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		a.sourcePath = assetSource;
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
		a.sourcePath = assetSource;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_TXMI))
		{
			size_t o=0;
			// Handles both the current uint32 layout and the legacy size_t one.
			HAsset::readTextureHeader(c->data,o,a.width,a.height,a.channels);
			// Cook tail (mipLevels, format, srgb) — guarded so pre-cook TXMI
			// chunks (width/height/channels only) keep their defaults.
			if (o + sizeof(uint32_t) <= c->data.size()) HAsset::Reader::readPOD(c->data,o,a.mipLevels);
			if (o + 1 <= c->data.size()) { uint8_t f=0; HAsset::Reader::readPOD(c->data,o,f); a.format = static_cast<TextureFormat>(f); }
			if (o + 1 <= c->data.size()) { uint8_t s=0; HAsset::Reader::readPOD(c->data,o,s); a.srgb = s != 0; }
			if (a.mipLevels == 0) a.mipLevels = 1;
		}
		if (const auto* c = reader.findChunk(HAsset::CHUNK_PIXL)) a.data = c->data;
		handle = m_textureAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::Material:
	{
		MaterialAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		a.sourcePath = assetSource;
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
			// Optional custom shader + node graph + exposed-parameter data (appended after
			// the PBR tail; absent in older materials → bounds-checked reader defaults).
			HAsset::Reader::readString(c->data,o,a.customShaderFragGlsl);
			HAsset::Reader::readString(c->data,o,a.nodeGraphJson);
			uint32_t paramFloats = 0;
			if (HAsset::Reader::readPOD(c->data,o,paramFloats) && paramFloats <= 16 * 4)
			{
				a.shaderParamData.resize(paramFloats);
				for (uint32_t i = 0; i < paramFloats; ++i)
					HAsset::Reader::readPOD(c->data,o,a.shaderParamData[i]);
			}
			HAsset::Reader::readVec(c->data,o,a.graphTexturePaths); // node-graph textures (paths)
			HAsset::Reader::readVec(c->data,o,a.graphParamNames);  // param names (slot order)
			HAsset::Reader::readVec(c->data,o,a.graphParamTypes);  // param widget kinds (slot order)
			// Param metadata + material-instance tail (absent in older files → defaults).
			HAsset::Reader::readVec(c->data,o,a.graphParamMinMax);       // 2 floats per slot
			HAsset::Reader::readVec(c->data,o,a.graphParamGroups);
			HAsset::Reader::readVec(c->data,o,a.graphParamTooltips);
			HAsset::Reader::readString(c->data,o,a.parentMaterialPath);  // non-empty = instance
			HAsset::Reader::readVec(c->data,o,a.instanceOverriddenParams);
			HAsset::Reader::readVec(c->data,o,a.instanceSwitchNames);
			HAsset::Reader::readVec(c->data,o,a.instanceSwitchValues);
			HAsset::Reader::readPOD(c->data,o,a.blendMode);              // 0 opaque/1 masked/2 translucent
			HAsset::Reader::readString(c->data,o,a.customShaderVertGlsl);// WPO vertex body
			HAsset::Reader::readVec(c->data,o,a.graphLayerNames);        // landscape paint layers
			// Deferred G-buffer fragment variant (append-only tail; absent in older
			// files → empty → forward-routed in deferred). For assets WITH a graph
			// the load-time regeneration overwrites it anyway; the serialized copy
			// is what lets PACKAGED materials (graph stripped) render deferred.
			HAsset::Reader::readString(c->data,o,a.customShaderGBufGlsl);
			// GI-hit approximation (absent in older files → the neutral in-struct
			// defaults, i.e. white/black = the pre-approx look). readPOD leaves
			// `out` untouched at EOF, so the partial-tail case is safe too.
			for (int k = 0; k < 3; ++k) HAsset::Reader::readPOD(c->data,o,a.approxBaseColor[k]);
			for (int k = 0; k < 3; ++k) HAsset::Reader::readPOD(c->data,o,a.approxEmissive[k]);
			HAsset::Reader::readPOD(c->data,o,a.approxBaseColorSlot);
			HAsset::Reader::readPOD(c->data,o,a.approxEmissiveSlot);
			HAsset::Reader::readPOD(c->data,o,a.approxMetallic);
			HAsset::Reader::readPOD(c->data,o,a.approxRoughness);
			// Landscape layer split (appended after the scalars; absent → count 0
			// = "not layer-blended", i.e. approxBaseColor alone, as before).
			for (int i = 0; i < 4; ++i)
				for (int k = 0; k < 3; ++k) HAsset::Reader::readPOD(c->data,o,a.approxLayerColor[i][k]);
			HAsset::Reader::readPOD(c->data,o,a.approxLayerCount);
		}
		// Baked graph-texture UUIDs live in MTLU alongside shaderId/textureIds.
		if (const auto* c = reader.findChunk(HAsset::CHUNK_MTLU))
		{
			size_t o=0;
			HAsset::Reader::readPOD(c->data,o,a.shaderId.hi);
			HAsset::Reader::readPOD(c->data,o,a.shaderId.lo);
			HAsset::Reader::readVec(c->data,o,a.textureIds);
			HAsset::Reader::readVec(c->data,o,a.graphTextureIds); // baked node-graph textures
		}
		if (const auto* c = reader.findChunk(HAsset::CHUNK_PSHD)) // precompiled shaders (packed)
			a.precompiledShaders = HE::decodeMaterialShaderVariants(c->data);
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
		if (const auto* c = reader.findChunk(HAsset::CHUNK_SLNG); c && !c->data.empty())
			a.language = static_cast<HE::ScriptLanguage>(c->data[0]); // absent → default Lua
		handle = m_scriptAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::MaterialFunction:
	{
		MaterialFunctionAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_MGRF))
			a.nodeGraphJson.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
		handle = m_materialFunctionAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::Widget:
	{
		UIWidgetAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_UIWT))
			a.treeJson.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
		if (const auto* c = reader.findChunk(HAsset::CHUNK_UIWG))
			a.graphJson.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
		handle = m_widgetAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::HorizonCodeClass:
	{
		HorizonCodeClassAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_HCGR))
			a.graphJson.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
		if (const auto* c = reader.findChunk(HAsset::CHUNK_HCBC)) // absent → plain Object
			a.baseClass.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
		if (const auto* c = reader.findChunk(HAsset::CHUNK_HCCP)) // absent → no components
			a.componentBlob = c->data;
		handle = m_hcClassAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::InputAction:
	{
		InputActionAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_IACT))
			a.json.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
		handle = m_inputActionAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::InputMappingContext:
	{
		InputMappingContextAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_IMAP))
			a.json.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
		handle = m_inputMappingAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::ParticleSystem:
	{
		ParticleGraphAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_PTGR))
			a.nodeGraphJson.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
		if (const auto* c = reader.findChunk(HAsset::CHUNK_PPSD)) // precompiled shaders (packed)
			a.precompiledShaders = HE::decodeParticleShaderVariants(c->data);
		handle = m_particleGraphAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::AnimatorStateMachine:
	{
		AnimatorStateMachineAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_ASMG))
			a.graphJson.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
		if (const auto* c = reader.findChunk(HAsset::CHUNK_ASSY))
			a.syncGraphJson.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
		handle = m_animatorStateMachineAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::StructType:
	{
		StructTypeAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_STDF))
			a.json.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
		// Side effect: the TypeRegistry mirrors every loaded definition, so lazy
		// pak loads keep it fresh too (same for EnumType below).
		//
		// The display name is the FILE STEM, not the name persisted in CHUNK_META
		// (TypeRegistry's own contract, and what TypeAssetPanel writes back on
		// save). Renaming an asset only renames the file — the META name keeps
		// whatever it was created as, so trusting it showed every renamed type as
		// "NewStruct"/"NewEnum" in the type dropdowns after the next project load.
		{
			HE::StructDef def;
			def.name = std::filesystem::path(relativePath).stem().string();
			def.assetPath = relativePath;
			if (HE::TypeRegistry::structFromJson(a.json, def))
				HE::TypeRegistry::instance().registerStruct(std::move(def));
		}
		handle = m_structTypeAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::EnumType:
	{
		EnumTypeAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_ENDF))
			a.json.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
		{
			HE::EnumDef def;
			def.name = std::filesystem::path(relativePath).stem().string();
			def.assetPath = relativePath;
			if (HE::TypeRegistry::enumFromJson(a.json, def))
				HE::TypeRegistry::instance().registerEnum(std::move(def));
		}
		handle = m_enumTypeAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::SaveGameTemplate:
	{
		SaveGameTemplateAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_SGTP))
			a.json.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
		handle = m_saveTemplateAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::Theme:
	{
		ThemeAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_THEM))
			a.json.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
		handle = m_themeAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::Audio:
	{
		AudioAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		a.sourcePath = assetSource;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_AUMI))
		{ size_t o=0; HAsset::Reader::readPOD(c->data,o,a.sampleRate); HAsset::Reader::readPOD(c->data,o,a.channels); }
		if (const auto* c = reader.findChunk(HAsset::CHUNK_PCMD)) a.audioData = c->data;
		handle = m_audioAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::Font:
	{
		FontAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		a.sourcePath = assetSource;
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
	case HE::AssetType::Prefab:
	{
		// The CBOR subtree is opaque here on purpose: decoding it needs the
		// component types, which live in HE_Scene — a layer HE_Core must not
		// depend on. SceneSerializer::instantiatePrefab is the only thing that
		// ever looks inside, and it is handed these bytes verbatim.
		PrefabAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		if (const auto* c = reader.findChunk(HAsset::CHUNK_PFAB)) a.data = c->data;
		handle = m_prefabAssets.insert(std::move(a)); break;
	}
	case HE::AssetType::AnimationClip:
	{
		AnimationClipAsset a{}; a.id = id; a.type = type; a.name = assetName; a.path = relativePath;
		a.sourcePath = assetSource;
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
	// Material INSTANCES derive their effective shader/params from their parent —
	// runs after the registry maps above so the recursive parent load is safe.
	if (type == HE::AssetType::Material)
	{
		if (const MaterialAsset* m = getMaterial(id); m && !m->parentMaterialPath.empty())
			syncMaterialInstance(id);
		// BASE materials with a node graph: regenerate the baked GLSL from the graph
		// (the source of truth) so assets saved under an older codegen automatically
		// pick up shader-library upgrades (e.g. heLit → heLitP shadowing) without a
		// manual re-save. Skipped when precompiled per-backend blobs are present
		// (packed builds — the blobs were compiled from the baked GLSL and must
		// stay consistent with it).
		else if (m && !m->nodeGraphJson.empty() && m->precompiledShaders.empty())
			regenerateMaterialFromGraph(id);
	}
	return id;
}

// ─── Graph → material codegen helpers (shared by base + instance regeneration) ─
// Function-graph resolution for HE::generateFragment. `fnStore` OWNS the parsed
// graphs and must outlive the call: generateFragment keeps the raw pointers the
// loader hands back. `cm` supplies loadAsset/getMaterialFunction.
static HE::MatFunctionLoader makeMatFunctionLoader(
	ContentManager& cm, std::map<std::string, HE::MaterialGraph>& fnStore)
{
	return [&cm, &fnStore](const std::string& path) -> const HE::MaterialGraph*
	{
		if (auto it = fnStore.find(path); it != fnStore.end()) return &it->second;
		const MaterialFunctionAsset* fn = cm.getMaterialFunction(cm.loadAsset(path));
		if (!fn || fn->nodeGraphJson.empty()) return nullptr;
		HE::MaterialGraph fg;
		if (!HE::materialGraphFromJson(fn->nodeGraphJson, fg)) return nullptr;
		return &(fnStore[path] = std::move(fg));
	};
}

// Replace a material's exposed-parameter arrays with a fresh codegen result (slot
// order = gen.params order). `preserved` (optional) supplies values to keep BY NAME:
// regenerateMaterialFromGraph passes the material's current values (a user edits
// values without recompiling), syncMaterialInstance passes nullptr because it
// re-applies only its explicitly overridden slots afterwards.
static void applyGraphParams(MaterialAsset& m, const HE::MatShaderGen& gen,
                             const std::map<std::string, std::array<float, 4>>* preserved)
{
	m.graphParamNames.clear(); m.graphParamTypes.clear();
	m.graphParamMinMax.clear(); m.graphParamGroups.clear();
	m.graphParamTooltips.clear(); m.shaderParamData.clear();
	for (const auto& slot : gen.params)
	{
		m.graphParamNames.push_back(slot.name);
		m.graphParamTypes.push_back(static_cast<uint8_t>(slot.kind));
		m.graphParamMinMax.insert(m.graphParamMinMax.end(), { slot.minV, slot.maxV });
		m.graphParamGroups.push_back(slot.group);
		m.graphParamTooltips.push_back(slot.tooltip);
		const std::array<float, 4>* keep = nullptr;
		if (preserved)
			if (auto it = preserved->find(slot.name); it != preserved->end()) keep = &it->second;
		if (keep) m.shaderParamData.insert(m.shaderParamData.end(), keep->begin(), keep->end());
		else      m.shaderParamData.insert(m.shaderParamData.end(), slot.value, slot.value + 4);
	}
}

// Bake the graph's CPU-approximated BaseColor/Emissive into the asset (see
// MaterialAsset::approxBaseColor): constants as folded, Param-driven pins as the
// slot index into shaderParamData so consumers read the LIVE value. Runs with
// the codegen (regenerate/sync) — graphParamNames must already be final.
static void applyApproxSurface(MaterialAsset& m, const HE::MaterialGraph& g,
                               const std::map<std::string, bool>* switchOverrides = nullptr)
{
	const HE::MatApproxSurface ap = HE::matGraphApproxSurface(g, switchOverrides);
	for (int k = 0; k < 3; ++k)
	{
		m.approxBaseColor[k] = ap.baseColor[k];
		m.approxEmissive[k]  = ap.emissive[k];
	}
	m.approxMetallic  = ap.metallic;
	m.approxRoughness = ap.roughness;
	m.approxLayerCount = ap.layerCount;
	for (int i = 0; i < 4; ++i)
		for (int k = 0; k < 3; ++k) m.approxLayerColor[i][k] = ap.layerColor[i][k];
	auto slotOf = [&](const std::string& name) -> int32_t
	{
		if (name.empty()) return -1;
		for (size_t i = 0; i < m.graphParamNames.size(); ++i)
			if (m.graphParamNames[i] == name) return static_cast<int32_t>(i);
		return -1;
	};
	m.approxBaseColorSlot = slotOf(ap.baseColorParam);
	m.approxEmissiveSlot  = slotOf(ap.emissiveParam);
}

// Snapshot a material's current param values by name (slot i ↔ floats [i*4, i*4+4)).
static std::map<std::string, std::array<float, 4>> snapshotGraphParams(const MaterialAsset& m)
{
	std::map<std::string, std::array<float, 4>> values;
	for (size_t i = 0; i < m.graphParamNames.size(); ++i)
		if (i * 4 + 3 < m.shaderParamData.size())
			values[m.graphParamNames[i]] = { m.shaderParamData[i*4+0], m.shaderParamData[i*4+1],
			                                 m.shaderParamData[i*4+2], m.shaderParamData[i*4+3] };
	return values;
}

// Re-run the graph → GLSL codegen for a BASE material at load time. Mirrors
// syncMaterialInstance's regeneration half: param VALUES are preserved by name
// (users edit values without recompiling, so the baked shaderParamData can
// legitimately differ from the graph's node defaults).
void ContentManager::regenerateMaterialFromGraph(HE::UUID materialId)
{
	MaterialAsset* mat = getMaterialMutable(materialId);
	if (!mat || mat->nodeGraphJson.empty()) return;

	HE::MaterialGraph g;
	if (!HE::materialGraphFromJson(mat->nodeGraphJson, g)) return;

	const std::map<std::string, std::array<float, 4>> oldValues = snapshotGraphParams(*mat);

	// Function graphs resolve through this manager; storage must outlive the call.
	std::map<std::string, HE::MaterialGraph> fnStore;
	HE::MatFunctionLoader loader = makeMatFunctionLoader(*this, fnStore);
	const HE::MatShaderGen gen = HE::generateFragment(g, loader, nullptr);
	if (gen.glsl.empty()) return;

	// Re-fetched for the reason spelled out in syncMaterialInstance: the
	// function loader inside generateFragment goes through this manager, and
	// loadAsset decides the pool from the FILE's own type.
	mat = getMaterialMutable(materialId);
	if (!mat) return;

	mat->customShaderFragGlsl = gen.glsl;
	mat->customShaderGBufGlsl = gen.glslGBuffer;
	mat->customShaderVertGlsl = gen.vertexBody;
	mat->blendMode            = gen.blendMode;
	mat->domain               = gen.domain;
	mat->graphTexturePaths    = gen.textures;
	mat->graphLayerNames      = gen.layerNames;   // landscape paint layers
	applyGraphParams(*mat, gen, &oldValues);      // keep every user-edited value
	applyApproxSurface(*mat, g);                  // GI-hit colours (needs final param slots)
}

// ─── Material instances ───────────────────────────────────────────────────────
void ContentManager::syncMaterialInstance(HE::UUID instanceId)
{
	// Instance chains (instance of an instance) are legal; a cycle would recurse
	// forever through loadAsset → guard with a small depth cap.
	static thread_local int s_depth = 0;
	if (s_depth > 8) { HE_LOG_WARN(Asset, "%s",
		"ContentManager: material-instance chain too deep / cyclic — sync aborted"); return; }
	s_depth++;

	// The parent path is COPIED out of the instance before anything is loaded,
	// and both pointers are taken AFTER. Loading the parent inserts into
	// m_materialAssets — the very pool the instance lives in, which is a dense
	// vector — so the old code's `inst` pointed at moved memory for the whole
	// rest of this function, and the string it handed to loadAsset died inside
	// that call. This fires on the ordinary path: opening a project whose
	// material instance is registered before its parent, which is most of them.
	std::string parentPath;
	if (const MaterialAsset* seed = getMaterial(instanceId))
		parentPath = seed->parentMaterialPath;
	if (parentPath.empty()) { s_depth--; return; }

	const HE::UUID parentId = loadAsset(parentPath);
	MaterialAsset*       inst   = getMaterialMutable(instanceId);
	const MaterialAsset* parent = getMaterial(parentId);
	if (!inst || !parent) { s_depth--; return; }

	// Preserve the instance's current values for slots it overrides (matched BY NAME,
	// so parent slot reordering can't mis-assign them).
	auto overridden = [&](const std::string& nm)
	{
		return std::find(inst->instanceOverriddenParams.begin(),
		                 inst->instanceOverriddenParams.end(), nm)
		       != inst->instanceOverriddenParams.end();
	};
	const std::map<std::string, std::array<float, 4>> oldValues = snapshotGraphParams(*inst);

	if (!inst->instanceSwitchNames.empty() && !parent->nodeGraphJson.empty())
	{
		// Switch overrides → REGENERATE from the parent's graph with the override map.
		// The resulting source differs from the parent's → its own pipeline-cache entry;
		// identical override sets across instances share one entry (hash-keyed).
		HE::MaterialGraph g;
		if (HE::materialGraphFromJson(parent->nodeGraphJson, g))
		{
			std::map<std::string, bool> ov;
			for (size_t i = 0; i < inst->instanceSwitchNames.size() &&
			                    i < inst->instanceSwitchValues.size(); ++i)
				ov[inst->instanceSwitchNames[i]] = inst->instanceSwitchValues[i] != 0;
			// Function graphs resolve through this manager; storage must outlive the call.
			std::map<std::string, HE::MaterialGraph> fnStore;
			HE::MatFunctionLoader loader = makeMatFunctionLoader(*this, fnStore);
			const HE::MatShaderGen gen = HE::generateFragment(g, loader, &ov);
			// The loader above resolves material functions THROUGH this manager,
			// so it loads. A function path normally names a MaterialFunction, a
			// different pool — but loadAsset takes the type from the FILE, not
			// from the caller, so a path that names a material lands in this one
			// and moves both pointers. Re-fetching costs two lookups and closes
			// the case entirely.
			inst   = getMaterialMutable(instanceId);
			parent = getMaterial(parentId);
			if (!inst || !parent) { s_depth--; return; }
			inst->customShaderFragGlsl = gen.glsl;
			inst->customShaderGBufGlsl = gen.glslGBuffer;
			// nullptr: start from the permutation's defaults — the overridden slots are
			// re-applied from oldValues below, the rest must follow the parent.
			applyGraphParams(*inst, gen, nullptr);
			inst->graphTexturePaths = gen.textures;
			inst->graphLayerNames      = gen.layerNames; // landscape paint layers
			inst->blendMode            = gen.blendMode;
			inst->domain               = gen.domain;
			inst->customShaderVertGlsl = gen.vertexBody;
			applyApproxSurface(*inst, g, &ov); // GI-hit colours FOR THE PERMUTATION (switches taken)
			// A regenerated permutation invalidates any parent-baked shaders.
			inst->precompiledShaders.clear();
		}
	}
	else
	{
		// Pure param overrides: byte-identical shader → the SAME cached pipeline.
		inst->customShaderFragGlsl = parent->customShaderFragGlsl;
		inst->customShaderGBufGlsl = parent->customShaderGBufGlsl;
		inst->graphParamNames      = parent->graphParamNames;
		inst->graphParamTypes      = parent->graphParamTypes;
		inst->graphParamMinMax     = parent->graphParamMinMax;
		inst->graphParamGroups     = parent->graphParamGroups;
		inst->graphParamTooltips   = parent->graphParamTooltips;
		inst->graphTexturePaths    = parent->graphTexturePaths;
		inst->graphTextureIds      = parent->graphTextureIds;
		// Same shader = same layer declaration. Without this an INSTANCE of a
		// landscape material advertised no paint layers at all, so the Landscape
		// panel refused to paint with it ("needs a material with a Landscape
		// Layer Blend node") even though its shader samples the weightmap.
		inst->graphLayerNames      = parent->graphLayerNames;
		inst->shaderParamData      = parent->shaderParamData;
		inst->precompiledShaders   = parent->precompiledShaders;
		inst->blendMode            = parent->blendMode;
		inst->customShaderVertGlsl = parent->customShaderVertGlsl;
		// Same shader = same fold; the slots stay valid because the param layout
		// is copied verbatim above — an overridden colour slot then reads the
		// INSTANCE's value live (exactly the point of the slot indirection).
		for (int k = 0; k < 3; ++k)
		{
			inst->approxBaseColor[k] = parent->approxBaseColor[k];
			inst->approxEmissive[k]  = parent->approxEmissive[k];
		}
		inst->approxBaseColorSlot = parent->approxBaseColorSlot;
		inst->approxEmissiveSlot  = parent->approxEmissiveSlot;
		inst->approxMetallic      = parent->approxMetallic;
		inst->approxRoughness     = parent->approxRoughness;
		inst->approxLayerCount    = parent->approxLayerCount;
		for (int i = 0; i < 4; ++i)
			for (int k = 0; k < 3; ++k)
				inst->approxLayerColor[i][k] = parent->approxLayerColor[i][k];
	}
	// Re-apply the instance's own values on overridden slots.
	for (size_t i = 0; i < inst->graphParamNames.size(); ++i)
	{
		if (!overridden(inst->graphParamNames[i])) continue;
		auto it = oldValues.find(inst->graphParamNames[i]);
		if (it == oldValues.end() || i * 4 + 3 >= inst->shaderParamData.size()) continue;
		for (int k = 0; k < 4; ++k) inst->shaderParamData[i * 4 + k] = it->second[k];
	}
	// Base surface state follows the parent (instances only vary params/switches).
	for (int k = 0; k < 3; ++k) inst->baseColor[k] = parent->baseColor[k];
	inst->metallic = parent->metallic; inst->roughness = parent->roughness;
	inst->opacity  = parent->opacity;  inst->doubleSided = parent->doubleSided;
	s_depth--;
}

void ContentManager::syncMaterialInstancesOf(const std::string& parentRelPath)
{
	for (const HE::UUID& id : enumerateIds(HE::AssetType::Material))
		if (const MaterialAsset* m = getMaterial(id); m && m->parentMaterialPath == parentRelPath)
			syncMaterialInstance(id);
}

// ─── resolveAbsolutePath / toContentRelativePath ──────────────────────────────
// The reserved "Engine/" prefix addresses m_engineContentRoot instead of the
// project's m_contentRoot (mirrors Unreal's /Engine/ vs /Game/). Engine
// defaults are read-only from a project's point of view: editing one and
// hitting Save does NOT touch the shared default — it writes a per-project
// override to "<contentRoot>/Engine/<rest>" (a plain subfolder of the
// project's own Content/, so the existing Content folder tree/scan already
// sees it — see refreshEngineFolder's merge for how the Content Browser
// shows it back under the "Engine" tree instead). Reads prefer the override
// when one exists, else fall back to the shared default — same identity
// (UUID/path), just a different physical file backing it for this project.
//
// HE_ENGINE_CONTENT_EDITABLE=1 is the engine-authoring escape hatch: with it
// set, saves of "Engine/..." paths go straight to the shared default instead
// of creating an override — that's how the default library itself gets
// built/edited, since there's no other path from the same graph editors.
static constexpr char kEnginePrefix[] = "Engine/";
static constexpr size_t kEnginePrefixLen = sizeof(kEnginePrefix) - 1;

bool ContentManager::isEngineContentDevMode()
{
	const char* v = std::getenv("HE_ENGINE_CONTENT_EDITABLE");
	return v && *v && std::atoi(v) != 0;
}

std::string ContentManager::resolveAbsolutePath(const std::string& relativePath) const
{
	if (!m_engineContentRoot.empty() && relativePath.rfind(kEnginePrefix, 0) == 0)
	{
		// A project override (plain "Content/Engine/..." file) shadows the
		// shared default when present.
		const std::string overridePath = m_contentRoot + "/" + relativePath;
		if (std::filesystem::exists(overridePath)) return overridePath;
		const std::string defaultPath = m_engineContentRoot + "/" + relativePath.substr(kEnginePrefixLen);
		if (std::filesystem::exists(defaultPath)) return defaultPath;
		// Neither the override nor the shipped default has this file — it may
		// have been fetched once by the EngineContent SFTP sync (HE_ContentSync)
		// into the shared, per-machine download cache, in which case every
		// project on this machine can use it from here on without re-downloading.
		// Checked LAST: a real shipped default always wins over a cached copy.
		const std::string cachedPath =
			GlobalState::engineContentCacheDir().string() + "/" + relativePath.substr(kEnginePrefixLen);
		if (std::filesystem::exists(cachedPath)) return cachedPath;
		return defaultPath; // preserves prior behaviour: the canonical location even when absent
	}
	return m_contentRoot + "/" + relativePath;
}

std::string ContentManager::resolveSavePath(const std::string& relativePath) const
{
	if (!m_engineContentRoot.empty() && relativePath.rfind(kEnginePrefix, 0) == 0 && isEngineContentDevMode())
		return m_engineContentRoot + "/" + relativePath.substr(kEnginePrefixLen);
	// Normal mode (and every non-"Engine/" path, always): the project's own
	// Content root — for "Engine/..." this IS the override location.
	return m_contentRoot + "/" + relativePath;
}

bool ContentManager::isEngineDefaultPath(const std::string& absolutePath) const
{
	if (m_engineContentRoot.empty()) return false;
	std::error_code ec;
	const std::filesystem::path rel = std::filesystem::relative(absolutePath, m_engineContentRoot, ec);
	return !ec && !rel.empty() && rel.native()[0] != '.';
}

bool ContentManager::isEngineOverridePath(const std::string& absolutePath) const
{
	if (m_contentRoot.empty()) return false;
	const std::string overrideRoot = m_contentRoot + "/" + kEnginePrefix; // "<contentRoot>/Engine/"
	std::error_code ec;
	const std::filesystem::path rel = std::filesystem::relative(absolutePath, overrideRoot, ec);
	return !ec && !rel.empty() && rel.native()[0] != '.';
}

std::string ContentManager::toContentRelativePath(const std::string& absolutePath) const
{
	namespace fs = std::filesystem;
	std::error_code ec;
	if (!m_engineContentRoot.empty())
	{
		const fs::path rel = fs::relative(absolutePath, m_engineContentRoot, ec);
		if (!ec && !rel.empty() && rel.native()[0] != '.')
			return std::string(kEnginePrefix) + rel.generic_string();
	}
	// The SFTP download cache is a third EngineContent root (see
	// resolveAbsolutePath), so a path inside it is an "Engine/..." path too.
	// Without this branch anything opened from the cache — every asset the
	// Content Browser just downloaded and auto-opened — produced an EMPTY
	// relative path here, and an empty path loads nothing: the tab opened and
	// stayed blank. Checked after the shipped default, mirroring the read-side
	// precedence exactly.
	ec.clear();
	{
		const fs::path cacheRoot = GlobalState::engineContentCacheDir();
		if (!cacheRoot.empty())
		{
			const fs::path rel = fs::relative(absolutePath, cacheRoot, ec);
			if (!ec && !rel.empty() && rel.native()[0] != '.')
				return std::string(kEnginePrefix) + rel.generic_string();
		}
	}
	ec.clear();
	const fs::path rel = fs::relative(absolutePath, m_contentRoot, ec);
	if (ec || rel.empty() || rel.native()[0] == '.') return std::string(); // outside both roots
	return rel.generic_string();
}

// ─── loadAsset ────────────────────────────────────────────────────────────────
HE::UUID ContentManager::loadAsset(const std::string& relativePath)
{
	HE_PROFILE_SCOPE_N("ContentManager::load");
	if (isLoaded(relativePath))
		return m_pathToUUID.at(relativePath);

	// Packed build: resolve the content path to a mounted-pak UUID via the asset
	// index, then load that entry synchronously (registers it under its embedded
	// path). This is what makes a HorizonCode-created widget — referenced only by
	// path, never reached by the scene's UUID reference closure — resolve in a
	// pak-only build. Loose files on disk still take the path below.
	if (const auto it = m_pakPathIndex.find(relativePath); it != m_pakPathIndex.end())
	{
		const HE::UUID id = it->second;
		if (isLoaded(id)) { m_pathToUUID[relativePath] = id; return id; }
		const auto bytes = readMountedEntry(id);
		if (!bytes.empty())
		{
			const HE::UUID rid = loadAssetFromMemory(bytes);
			if (rid != HE::UUID{}) { m_pathToUUID[relativePath] = rid; return rid; }
		}
	}

	const std::string fullPath = resolveAbsolutePath(relativePath);

	HAsset::Reader reader;
	if (!reader.open(fullPath))
	{
		// The most-reported "why is my asset not there" path: the reference is
		// fine, the file simply is not where the content root says it should be.
		HE_LOG_ERROR(Asset, "Cannot load asset '%s': no readable .hasset at '%s'",
		             relativePath.c_str(), fullPath.c_str());
		return HE::UUID();
	}

	const HE::UUID id = parseAndRegisterAsset(relativePath, fullPath, reader);
	if (id == HE::UUID{})
		HE_LOG_ERROR(Asset, "Asset '%s' opened but could not be parsed or registered",
		             relativePath.c_str());
	else
		HE_LOG_DEBUG(Asset, "Loaded asset '%s'", relativePath.c_str());
	return id;
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

	// Packed build: a path that only exists in a mounted pak streams by UUID (the
	// worker-thread disk read below would fail — there's no loose file).
	if (const auto it = m_pakPathIndex.find(relativePath); it != m_pakPathIndex.end())
	{
		loadAssetAsync(it->second, std::move(callback));
		return;
	}

	auto progress = std::make_shared<AsyncProgress>();
	{
		std::unique_lock<std::mutex> lock(m_pendingMutex);
		if (m_pendingPaths.count(relativePath))
			return; // already in flight — coalesce
		m_pendingPaths.insert(relativePath);
		m_pendingProgress[relativePath] = progress;
	}

	const std::string fullPath = resolveAbsolutePath(relativePath);

	// Captures the sink and the progress cell, never `this` — the job may outlive
	// this ContentManager.
	globalPool().submit([relativePath, fullPath, sink = m_asyncSink, progress,
	                     cb = std::move(callback)]() mutable
	{
		AsyncResult result;
		result.relativePath = relativePath;
		result.fullPath     = fullPath;
		result.callback     = std::move(cb);

		std::ifstream f(fullPath, std::ios::binary | std::ios::ate);
		if (f)
		{
			// Read in blocks rather than in one istreambuf_iterator gulp: the caller
			// gets a live byte count (asyncProgress → the editor's progress bar for a
			// heavy mesh), and a sized single allocation beats the iterator's repeated
			// grow-and-copy on a several-hundred-megabyte asset.
			const std::streamoff end = f.tellg();
			const std::uint64_t total = end > 0 ? static_cast<std::uint64_t>(end) : 0;
			progress->bytesTotal.store(total, std::memory_order_relaxed);
			f.seekg(0, std::ios::beg);
			result.fileBytes.resize(static_cast<size_t>(total));
			constexpr std::uint64_t kBlock = 4ull << 20; // 4 MiB
			std::uint64_t done = 0;
			while (done < total)
			{
				const std::uint64_t n = std::min(kBlock, total - done);
				if (!f.read(reinterpret_cast<char*>(result.fileBytes.data() + done),
				            static_cast<std::streamsize>(n)))
				{
					result.fileBytes.resize(static_cast<size_t>(done + (std::uint64_t)f.gcount()));
					break;
				}
				done += n;
				progress->bytesRead.store(done, std::memory_order_relaxed);
			}
			if (result.fileBytes.empty()) result.failed = true;
		}
		else
		{
			result.failed = true;
		}

		std::unique_lock<std::mutex> lock(sink->mutex);
		sink->results.push(std::move(result));
	}, "AssetLoad");
}

// ─── pollAsyncResults ─────────────────────────────────────────────────────────
std::vector<HE::UUID> ContentManager::pollAsyncResults(size_t maxRegistrations)
{
	// Drain remote-materialization completions FIRST, on this (main) thread —
	// see registerRemoteAsset()/loadAssetAsync(UUID)'s remote branch and
	// RemoteReadySink's comment in the header for why this cannot happen inside
	// materialize's own completion callback. A success re-enters loadAssetAsync,
	// which now resolves through the normal (just-updated) disk registry and
	// queues a real async read — that read's own AsyncResult surfaces on a LATER
	// pollAsyncResults() call, not this one.
	{
		std::vector<PendingRemoteReady> remoteReady;
		{
			std::lock_guard<std::mutex> lock(m_remoteReadySink->mutex);
			remoteReady.swap(m_remoteReadySink->ready);
		}
		for (auto& r : remoteReady)
		{
			{
				std::unique_lock<std::mutex> lock(m_pendingMutex);
				m_pendingPaths.erase(r.coalesceKey);
			}
			if (r.success)
			{
				m_diskRegistry[r.id] = r.relativePath;
				m_remoteAssets.erase(r.id);
				loadAssetAsync(r.id, r.callback);
			}
			else if (r.callback)
			{
				r.callback(HE::UUID{});
			}
		}
	}

	std::vector<AsyncResult> ready;
	{
		std::unique_lock<std::mutex> lock(m_asyncSink->mutex);
		// Pull at most `maxRegistrations` completed jobs; leave the rest queued so
		// a burst is spread over frames (registration/parse runs on this thread).
		while (!m_asyncSink->results.empty() && ready.size() < maxRegistrations)
		{
			ready.push_back(std::move(m_asyncSink->results.front()));
			m_asyncSink->results.pop();
		}
	}

	std::vector<HE::UUID> registered;
	for (auto& r : ready)
	{
		{
			std::unique_lock<std::mutex> lock(m_pendingMutex);
			m_pendingPaths.erase(r.relativePath);
			m_pendingProgress.erase(r.relativePath); // the worker's cell dies with it
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

size_t ContentManager::asyncInFlightCount() const
{
	std::unique_lock<std::mutex> lock(m_pendingMutex);
	return m_pendingPaths.size();
}

// ─── asyncProgress ────────────────────────────────────────────────────────────
bool ContentManager::asyncProgress(const std::string& relativePath,
                                    std::uint64_t& bytesRead, std::uint64_t& bytesTotal) const
{
	std::shared_ptr<AsyncProgress> cell;
	{
		std::unique_lock<std::mutex> lock(m_pendingMutex);
		const auto it = m_pendingProgress.find(relativePath);
		if (it == m_pendingProgress.end()) return false;
		cell = it->second; // keep it alive past the lock
	}
	bytesRead  = cell->bytesRead.load(std::memory_order_relaxed);
	bytesTotal = cell->bytesTotal.load(std::memory_order_relaxed);
	return true;
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
		// Not in any mounted pak — fall back to loose content on disk via the
		// registry (async path-based load; coalesced by path).
		if (const auto d = m_diskRegistry.find(id); d != m_diskRegistry.end())
		{
			loadAssetAsync(d->second, std::move(callback));
			return;
		}
		// A remote-only EngineContent asset (see registerRemoteAsset): materialize
		// it first, then load it the normal way. Coalesced by the same synthetic-key
		// mechanism as the pak branch below, on its own "remote://" namespace so it
		// cannot collide with a pak UUID or a real relative path.
		if (const auto r = m_remoteAssets.find(id); r != m_remoteAssets.end())
		{
			const std::string coalesceKey =
				"remote://" + std::to_string(id.hi) + "-" + std::to_string(id.lo);
			{
				std::unique_lock<std::mutex> lock(m_pendingMutex);
				if (m_pendingPaths.count(coalesceKey))
				{
					// Already materializing — like the pak branch, this caller's
					// callback is not chained; it can retry or poll isLoaded().
					if (callback) callback(HE::UUID{});
					return;
				}
				m_pendingPaths.insert(coalesceKey);
			}

			const std::string relativePath = r->second.relativePath;
			const auto&       materialize  = r->second.materialize;
			// Captures the sink, never `this` — materialize's completion can fire
			// long after this ContentManager is gone (a network download outlives a
			// closed project). See RemoteReadySink's comment in the header.
			auto sink = m_remoteReadySink;
			materialize([sink, id, relativePath, coalesceKey, callback](bool success)
			{
				std::lock_guard<std::mutex> lock(sink->mutex);
				sink->ready.push_back(PendingRemoteReady{ id, relativePath, coalesceKey, success, callback });
			});
			return;
		}
		if (callback) callback(HE::UUID{}); // unknown UUID
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

	// Captures the sink, never `this` — the job may outlive this ContentManager.
	globalPool().submit([id, path, enc, key, coalesceKey, sink = m_asyncSink,
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

		std::unique_lock<std::mutex> lock(sink->mutex);
		sink->results.push(std::move(result));
	}, "AssetLoadPak");
}

// ─── registerRemoteAsset ───────────────────────────────────────────────────────
void ContentManager::registerRemoteAsset(HE::UUID id, std::string relativePath,
                                          std::function<void(std::function<void(bool)>)> materialize)
{
	// Already resolvable some other way — a real local/override/mounted file
	// always wins, and there is nothing for a materializer to add.
	if (isLoaded(id) || m_diskRegistry.count(id)) return;

	RemoteAssetEntry entry;
	entry.relativePath = std::move(relativePath);
	entry.materialize   = std::move(materialize);
	m_remoteAssets[id]  = std::move(entry);
}

// ─── forgetDiskAsset ─────────────────────────────────────────────────────────
bool ContentManager::forgetDiskAsset(HE::UUID id)
{
	return m_diskRegistry.erase(id) > 0;
}

// ─── streamMountedAssets ──────────────────────────────────────────────────────
size_t ContentManager::streamMountedAssets(const std::unordered_set<HE::UUID>& exclude)
{
	// Reserved pak entries are NOT streamable assets: the asset path index, the
	// asset TYPE index, the scene index and the user-type index are JSON metadata
	// blobs read directly by their known UUID (via readMountedEntry), never parsed
	// as .hasset — as is the packed GameInstance graph. Streaming any of them would
	// burn an async job on a guaranteed parse failure. EVERY reserved name from
	// ProjectExporter.h belongs in this list; kTypeIndexEntry was missing and only
	// went unnoticed because the exporter omits it for a project without any
	// Struct/Enum/SaveGameTemplate asset. (kSceneIndexEntry mirrors
	// HE::api::scene::kSceneIndexEntry.)
	const HE::UUID assetIdx = sceneUuidForPath(kAssetPathIndexEntry);
	const HE::UUID typeIdx  = sceneUuidForPath(kAssetTypeIndexEntry);
	const HE::UUID sceneIdx = sceneUuidForPath(kSceneIndexEntry);
	const HE::UUID userTypes = sceneUuidForPath(kTypeIndexEntry);
	const HE::UUID giIdx    = sceneUuidForPath(kGameInstanceEntry);
	size_t submitted = 0;
	for (const auto& [id, mountIdx] : m_pakResidency)
	{
		(void)mountIdx;
		if (isLoaded(id) || exclude.count(id) ||
		    id == assetIdx || id == typeIdx || id == sceneIdx ||
		    id == userTypes || id == giIdx) continue;
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

	const std::string fullPath = resolveSavePath(asset.path);
	const uint16_t    typeId   = static_cast<uint16_t>(asset.type);
	if (!m_engineContentRoot.empty() && asset.path.rfind(kEnginePrefix, 0) == 0 && !isEngineContentDevMode())
		HE_LOG_INFO(Asset, "%s",
			("ContentManager: '" + asset.path + "' is an engine default — saved a project-local copy to " + fullPath).c_str());

	HAsset::Writer w;

	// META chunk — common to all
	{ auto m = buildMetaChunk(asset); w.addChunk(HAsset::CHUNK_META, m.data(), m.size()); }

	switch (asset.type)
	{
	case HE::AssetType::StaticMesh:
	{
		auto& a = static_cast<StaticMeshAsset&>(asset);
		{ std::vector<uint8_t> b; HAsset::Writer::appendString(b,a.materialPath); w.addChunk(HAsset::CHUNK_MREF,b.data(),b.size()); }
		{ std::vector<uint8_t> b; HAsset::Writer::appendVec(b,a.indices);    w.addChunk(HAsset::CHUNK_INDX,b.data(),b.size()); }
		if (a.cooked)
		{
			// Round-trip the cooked form (its SoA arrays are empty) — defensive:
			// the editor edits loose (raw) assets, but never lose geometry here.
			std::vector<uint8_t> b;
			HAsset::Writer::appendPOD(b, a.vertexCount);
			for (int i = 0; i < 3; ++i) HAsset::Writer::appendPOD(b, a.boundsMin[i]);
			for (int i = 0; i < 3; ++i) HAsset::Writer::appendPOD(b, a.boundsMax[i]);
			HAsset::Writer::appendVec(b, a.interleaved);
			w.addChunk(HAsset::CHUNK_MVBO, b.data(), b.size());
		}
		else
		{
			{ std::vector<uint8_t> b; HAsset::Writer::appendVec(b,a.vertices); w.addChunk(HAsset::CHUNK_VERT,b.data(),b.size()); }
			{ std::vector<uint8_t> b; HAsset::Writer::appendVec(b,a.normals);  w.addChunk(HAsset::CHUNK_NORM,b.data(),b.size()); }
			{ std::vector<uint8_t> b; HAsset::Writer::appendVec(b,a.uvs);      w.addChunk(HAsset::CHUNK_TEXC,b.data(),b.size()); }
		}
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
		{ std::vector<uint8_t> b; HAsset::appendTextureHeader(b,a.width,a.height,a.channels);
		  HAsset::Writer::appendPOD(b, a.mipLevels);
		  HAsset::Writer::appendPOD(b, static_cast<uint8_t>(a.format));
		  HAsset::Writer::appendPOD(b, static_cast<uint8_t>(a.srgb ? 1 : 0));
		  w.addChunk(HAsset::CHUNK_TXMI,b.data(),b.size()); }
		w.addChunk(HAsset::CHUNK_PIXL, a.data.data(), a.data.size());
		break;
	}
	case HE::AssetType::Material:
	{
		// ⚠ FIELD-SYNCHRONISED WITH rewriteRefsForPack's MTRL branch (HpakWriter.cpp):
		// the packer re-serializes this chunk by walking the field order below to find
		// byte offsets (it copies the scalar tail verbatim rather than parsing it).
		// Adding/removing/reordering a field here without mirroring it there makes the
		// packer cut at the wrong offsets — packed materials then lose params or their
		// WPO vertex body, silently (all reads are bounds-checked and just stop early).
		auto& a = static_cast<MaterialAsset&>(asset);
		std::vector<uint8_t> b; HAsset::Writer::appendString(b,a.shaderPath); HAsset::Writer::appendVec(b,a.texturePaths);
		HAsset::Writer::appendPOD(b,a.baseColor[0]); HAsset::Writer::appendPOD(b,a.baseColor[1]); HAsset::Writer::appendPOD(b,a.baseColor[2]);
		HAsset::Writer::appendPOD(b,a.metallic); HAsset::Writer::appendPOD(b,a.roughness);
		HAsset::Writer::appendPOD(b,a.opacity);
		HAsset::Writer::appendString(b,a.customShaderFragGlsl); // optional custom shader (back-compatible tail)
		HAsset::Writer::appendString(b,a.nodeGraphJson);        // optional node graph (source of truth)
		HAsset::Writer::appendPOD(b,static_cast<uint32_t>(a.shaderParamData.size()));
		for (float f : a.shaderParamData) HAsset::Writer::appendPOD(b,f); // exposed params (HeParams)
		HAsset::Writer::appendVec(b,a.graphTexturePaths);                 // node-graph textures (paths)
		HAsset::Writer::appendVec(b,a.graphParamNames);                   // param names (slot order)
		HAsset::Writer::appendVec(b,a.graphParamTypes);                   // param widget kinds (slot order)
		HAsset::Writer::appendVec(b,a.graphParamMinMax);                  // slider ranges (2/slot)
		HAsset::Writer::appendVec(b,a.graphParamGroups);                  // panel groups
		HAsset::Writer::appendVec(b,a.graphParamTooltips);                // hover help
		HAsset::Writer::appendString(b,a.parentMaterialPath);             // instance parent
		HAsset::Writer::appendVec(b,a.instanceOverriddenParams);          // overridden slots (names)
		HAsset::Writer::appendVec(b,a.instanceSwitchNames);               // switch overrides…
		HAsset::Writer::appendVec(b,a.instanceSwitchValues);              // …(name + 0/1)
		HAsset::Writer::appendPOD(b,a.blendMode);                         // blend mode
		HAsset::Writer::appendString(b,a.customShaderVertGlsl);           // WPO vertex body
		HAsset::Writer::appendVec(b,a.graphLayerNames);                   // landscape paint layers
		// Deferred G-buffer fragment variant — post-v9 tail, which the packer
		// (HpakWriter's MTRL branch) copies byte-verbatim into shipped paks, so
		// packaged games can build the G-buffer pipeline without the node graph.
		HAsset::Writer::appendString(b,a.customShaderGBufGlsl);
		// GI-hit approximation (matGraphApproxSurface) — packaged materials
		// (graph stripped) keep their reflected colour through this tail.
		for (int k = 0; k < 3; ++k) HAsset::Writer::appendPOD(b,a.approxBaseColor[k]);
		for (int k = 0; k < 3; ++k) HAsset::Writer::appendPOD(b,a.approxEmissive[k]);
		HAsset::Writer::appendPOD(b,a.approxBaseColorSlot);
		HAsset::Writer::appendPOD(b,a.approxEmissiveSlot);
		HAsset::Writer::appendPOD(b,a.approxMetallic);
		HAsset::Writer::appendPOD(b,a.approxRoughness);
		// Landscape layer split — lets a terrain's GI hit weight the layers by its
		// own paint instead of settling for their average.
		for (int i = 0; i < 4; ++i)
			for (int k = 0; k < 3; ++k) HAsset::Writer::appendPOD(b,a.approxLayerColor[i][k]);
		HAsset::Writer::appendPOD(b,a.approxLayerCount);
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
		const uint8_t lang = static_cast<uint8_t>(a.language);
		w.addChunk(HAsset::CHUNK_SLNG, &lang, 1);
		break;
	}
	case HE::AssetType::MaterialFunction:
	{
		auto& a = static_cast<MaterialFunctionAsset&>(asset);
		w.addChunk(HAsset::CHUNK_MGRF, a.nodeGraphJson.data(), a.nodeGraphJson.size());
		break;
	}
	case HE::AssetType::Widget:
	{
		auto& a = static_cast<UIWidgetAsset&>(asset);
		w.addChunk(HAsset::CHUNK_UIWT, a.treeJson.data(), a.treeJson.size());
		if (!a.graphJson.empty())
			w.addChunk(HAsset::CHUNK_UIWG, a.graphJson.data(), a.graphJson.size());
		break;
	}
	case HE::AssetType::HorizonCodeClass:
	{
		auto& a = static_cast<HorizonCodeClassAsset&>(asset);
		if (!a.graphJson.empty())
			w.addChunk(HAsset::CHUNK_HCGR, a.graphJson.data(), a.graphJson.size());
		if (!a.baseClass.empty())
			w.addChunk(HAsset::CHUNK_HCBC, a.baseClass.data(), a.baseClass.size());
		if (!a.componentBlob.empty())
			w.addChunk(HAsset::CHUNK_HCCP, a.componentBlob.data(), a.componentBlob.size());
		break;
	}
	case HE::AssetType::InputAction:
	{
		auto& a = static_cast<InputActionAsset&>(asset);
		if (!a.json.empty())
			w.addChunk(HAsset::CHUNK_IACT, a.json.data(), a.json.size());
		break;
	}
	case HE::AssetType::InputMappingContext:
	{
		auto& a = static_cast<InputMappingContextAsset&>(asset);
		if (!a.json.empty())
			w.addChunk(HAsset::CHUNK_IMAP, a.json.data(), a.json.size());
		break;
	}
	case HE::AssetType::ParticleSystem:
	{
		auto& a = static_cast<ParticleGraphAsset&>(asset);
		w.addChunk(HAsset::CHUNK_PTGR, a.nodeGraphJson.data(), a.nodeGraphJson.size());
		break;
	}
	case HE::AssetType::AnimatorStateMachine:
	{
		auto& a = static_cast<AnimatorStateMachineAsset&>(asset);
		w.addChunk(HAsset::CHUNK_ASMG, a.graphJson.data(), a.graphJson.size());
		// Only when there is one — an absent chunk is how every asset written
		// before the sync graph existed keeps loading unchanged.
		if (!a.syncGraphJson.empty())
			w.addChunk(HAsset::CHUNK_ASSY, a.syncGraphJson.data(), a.syncGraphJson.size());
		break;
	}
	case HE::AssetType::StructType:
	{
		auto& a = static_cast<StructTypeAsset&>(asset);
		if (!a.json.empty())
			w.addChunk(HAsset::CHUNK_STDF, a.json.data(), a.json.size());
		break;
	}
	case HE::AssetType::EnumType:
	{
		auto& a = static_cast<EnumTypeAsset&>(asset);
		if (!a.json.empty())
			w.addChunk(HAsset::CHUNK_ENDF, a.json.data(), a.json.size());
		break;
	}
	case HE::AssetType::SaveGameTemplate:
	{
		auto& a = static_cast<SaveGameTemplateAsset&>(asset);
		if (!a.json.empty())
			w.addChunk(HAsset::CHUNK_SGTP, a.json.data(), a.json.size());
		break;
	}
	case HE::AssetType::Theme:
	{
		auto& a = static_cast<ThemeAsset&>(asset);
		if (!a.json.empty())
			w.addChunk(HAsset::CHUNK_THEM, a.json.data(), a.json.size());
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
	case HE::AssetType::Prefab:
	{
		// Written unconditionally, like Audio's PCMD: a prefab whose blob went
		// missing is not a smaller prefab, it is an entity subtree that spawns
		// nothing — and an empty chunk says so on the next load instead of the
		// file quietly keeping whatever bytes it had before.
		auto& a = static_cast<PrefabAsset&>(asset);
		w.addChunk(HAsset::CHUNK_PFAB, a.data.data(), a.data.size());
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

	// The override location (Content/Engine/<rest>) may not exist yet on the
	// first save of a given engine default — unlike ordinary project saves,
	// which are always into an already-browsed (thus already-existing) folder.
	{
		std::error_code ec;
		std::filesystem::create_directories(std::filesystem::path(fullPath).parent_path(), ec);
	}

	if (!w.write(fullPath, typeId))
	{
		// Losing a save silently is the worst possible failure mode in an editor.
		HE_LOG_ERROR(Asset, "Failed to write asset '%s' to '%s' — the change was NOT saved",
		             asset.path.c_str(), fullPath.c_str());
		return false;
	}
	HE_LOG_INFO(Asset, "Saved asset '%s' (type %u) to '%s'",
	            asset.path.c_str(), static_cast<unsigned>(typeId), fullPath.c_str());

	// Tell whoever is listening that this asset's bytes changed. Fired only on a
	// successful write, so a collaboration session never publishes a save that
	// did not happen. ContentManager itself stays unaware of who listens.
	if (m_onAssetSaved) m_onAssetSaved(asset.path, fullPath);
	return true;
}

// ─── Shader-variant chunk codecs (PSHD / PPSD) ───────────────────────────────
// The single source of truth for the precompiled-shader byte layout, shared by
// the exporter (encode) and the runtime loader above (decode). Material (PSHD)
// and particle (PPSD) variants have identical wire shapes and differ only in the
// C++ type they carry (see MaterialShaderVariant / ParticleShaderVariant in
// Assets.h), so one template serves both — a layout change stays in one place.
namespace
{
// Does this variant type carry a UI vertex? Material does (A3b — one variant serves the
// mesh and the UI path), Particle does not. Detected instead of specialised so the two
// codecs stay ONE function: the layout lives in one place, which is the point of the
// template.
template<typename Variant>
inline constexpr bool kHasUIVertex = requires(Variant v) { v.uiVertex; };

// A record grew a field, and the record is repeated — so a reader that stops early
// mis-parses everything AFTER it, not just the tail. Hence a version, and hence it is
// written where an old blob can never be mistaken for it: v1 starts with the variant
// COUNT, and encode returns {} for an empty list, so a leading 0 is impossible in v1
// and marks "versioned header follows".
constexpr uint8_t kShaderVariantVersionMark = 0;
constexpr uint8_t kShaderVariantVersion     = 2; // v2 = + uiVertex per record

template<typename Variant>
std::vector<uint8_t> encodeShaderVariants(const std::vector<Variant>& vars)
{
	// No variants → no blob, so the exporter's `if (!pshd.empty())` guard skips the
	// chunk entirely (rather than baking a count-0 PSHD that decodes to nothing).
	if (vars.empty()) return {};
	std::vector<uint8_t> b;
	if constexpr (kHasUIVertex<Variant>)
	{
		HAsset::Writer::appendPOD(b, kShaderVariantVersionMark);
		HAsset::Writer::appendPOD(b, kShaderVariantVersion);
	}
	HAsset::Writer::appendPOD(b, static_cast<uint8_t>(std::min<size_t>(vars.size(), 255)));
	for (const auto& v : vars)
	{
		HAsset::Writer::appendPOD(b, v.backend);
		HAsset::Writer::appendString(b, v.vertex);
		HAsset::Writer::appendString(b, v.fragment);
		if constexpr (kHasUIVertex<Variant>) HAsset::Writer::appendString(b, v.uiVertex);
	}
	return b;
}

template<typename Variant>
std::vector<Variant> decodeShaderVariants(const std::vector<uint8_t>& bytes)
{
	std::vector<Variant> out;
	size_t o = 0; uint8_t count = 0;
	if (!HAsset::Reader::readPOD(bytes, o, count)) return out;
	uint8_t version = 1;
	if (count == kShaderVariantVersionMark)
	{
		// Versioned header: the 0 was the mark, the real count follows the version.
		if (!HAsset::Reader::readPOD(bytes, o, version)) return out;
		if (!HAsset::Reader::readPOD(bytes, o, count))   return out;
	}
	out.reserve(count);
	for (uint8_t i = 0; i < count; ++i)
	{
		Variant v;
		if (!HAsset::Reader::readPOD(bytes, o, v.backend))     break;
		if (!HAsset::Reader::readString(bytes, o, v.vertex))   break;
		if (!HAsset::Reader::readString(bytes, o, v.fragment)) break;
		if constexpr (kHasUIVertex<Variant>)
		{
			// v1 paks simply have no UI vertex; the renderer then cross-compiles it,
			// exactly as it did before the field existed.
			if (version >= 2 && !HAsset::Reader::readString(bytes, o, v.uiVertex)) break;
		}
		out.push_back(std::move(v));
	}
	return out;
}
} // namespace

namespace HE
{
std::vector<uint8_t> encodeMaterialShaderVariants(const std::vector<MaterialShaderVariant>& vars)
{ return encodeShaderVariants(vars); }
std::vector<MaterialShaderVariant> decodeMaterialShaderVariants(const std::vector<uint8_t>& bytes)
{ return decodeShaderVariants<MaterialShaderVariant>(bytes); }
std::vector<uint8_t> encodeParticleShaderVariants(const std::vector<ParticleShaderVariant>& vars)
{ return encodeShaderVariants(vars); }
std::vector<ParticleShaderVariant> decodeParticleShaderVariants(const std::vector<uint8_t>& bytes)
{ return decodeShaderVariants<ParticleShaderVariant>(bytes); }
} // namespace HE

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

// Mutable twin of lookupAsset — same UUID re-check against SlotHandle aliasing.
template<typename T>
static T* lookupAssetMutable(const std::unordered_map<HE::UUID, SlotHandle>& index,
                             SlotMap<T>& map, HE::UUID id)
{
	auto it = index.find(id);
	if (it == index.end()) return nullptr;
	T* a = map.get(it->second);
	return (a && a->id == id) ? a : nullptr; // reject wrong-type aliasing
}

const StaticMeshAsset*    ContentManager::getStaticMesh(HE::UUID id) const    { return lookupAsset(m_handleToUUID, m_staticMeshAssets, id); }
const SkeletalMeshAsset*  ContentManager::getSkeletalMesh(HE::UUID id) const  { return lookupAsset(m_handleToUUID, m_skeletalMeshAssets, id); }
const TextureAsset*       ContentManager::getTexture(HE::UUID id) const       { return lookupAsset(m_handleToUUID, m_textureAssets, id); }
const MaterialAsset*      ContentManager::getMaterial(HE::UUID id) const      { return lookupAsset(m_handleToUUID, m_materialAssets, id); }
const AudioAsset*         ContentManager::getAudio(HE::UUID id) const         { return lookupAsset(m_handleToUUID, m_audioAssets, id); }
const FontAsset*          ContentManager::getFont(HE::UUID id) const          { return lookupAsset(m_handleToUUID, m_fontAssets, id); }
const ScriptAsset*        ContentManager::getScript(HE::UUID id) const        { return lookupAsset(m_handleToUUID, m_scriptAssets, id); }
const MaterialFunctionAsset* ContentManager::getMaterialFunction(HE::UUID id) const { return lookupAsset(m_handleToUUID, m_materialFunctionAssets, id); }
MaterialFunctionAsset*    ContentManager::getMaterialFunctionMutable(HE::UUID id) { return lookupAssetMutable(m_handleToUUID, m_materialFunctionAssets, id); }
const UIWidgetAsset*      ContentManager::getWidget(HE::UUID id) const        { return lookupAsset(m_handleToUUID, m_widgetAssets, id); }
UIWidgetAsset*            ContentManager::getWidgetMutable(HE::UUID id)       { return lookupAssetMutable(m_handleToUUID, m_widgetAssets, id); }
const HorizonCodeClassAsset* ContentManager::getHorizonCodeClass(HE::UUID id) const { return lookupAsset(m_handleToUUID, m_hcClassAssets, id); }
HorizonCodeClassAsset*    ContentManager::getHorizonCodeClassMutable(HE::UUID id) { return lookupAssetMutable(m_handleToUUID, m_hcClassAssets, id); }
const InputActionAsset*   ContentManager::getInputAction(HE::UUID id) const   { return lookupAsset(m_handleToUUID, m_inputActionAssets, id); }
InputActionAsset*         ContentManager::getInputActionMutable(HE::UUID id)  { return lookupAssetMutable(m_handleToUUID, m_inputActionAssets, id); }
const InputMappingContextAsset* ContentManager::getInputMappingContext(HE::UUID id) const { return lookupAsset(m_handleToUUID, m_inputMappingAssets, id); }
InputMappingContextAsset* ContentManager::getInputMappingContextMutable(HE::UUID id) { return lookupAssetMutable(m_handleToUUID, m_inputMappingAssets, id); }
const ParticleGraphAsset* ContentManager::getParticleGraph(HE::UUID id) const { return lookupAsset(m_handleToUUID, m_particleGraphAssets, id); }
ParticleGraphAsset*       ContentManager::getParticleGraphMutable(HE::UUID id) { return lookupAssetMutable(m_handleToUUID, m_particleGraphAssets, id); }
const AnimatorStateMachineAsset* ContentManager::getAnimatorStateMachine(HE::UUID id) const { return lookupAsset(m_handleToUUID, m_animatorStateMachineAssets, id); }
AnimatorStateMachineAsset* ContentManager::getAnimatorStateMachineMutable(HE::UUID id) { return lookupAssetMutable(m_handleToUUID, m_animatorStateMachineAssets, id); }
const ShaderAsset*        ContentManager::getShader(HE::UUID id) const        { return lookupAsset(m_handleToUUID, m_shaderAssets, id); }
const PrefabAsset*        ContentManager::getPrefab(HE::UUID id) const        { return lookupAsset(m_handleToUUID, m_prefabAssets, id); }
const AnimationClipAsset*      ContentManager::getAnimationClip(HE::UUID id) const      { return lookupAsset(m_handleToUUID, m_animClipAssets,     id); }
const PropertyAnimClipAsset*   ContentManager::getPropertyAnimClip(HE::UUID id) const   { return lookupAsset(m_handleToUUID, m_propAnimClipAssets, id); }
const ThemeAsset*            ContentManager::getTheme(HE::UUID id) const { return lookupAsset(m_handleToUUID, m_themeAssets, id); }
ThemeAsset*                  ContentManager::getThemeMutable(HE::UUID id) { return lookupAssetMutable(m_handleToUUID, m_themeAssets, id); }
const SaveGameTemplateAsset* ContentManager::getSaveGameTemplate(HE::UUID id) const { return lookupAsset(m_handleToUUID, m_saveTemplateAssets, id); }
SaveGameTemplateAsset*       ContentManager::getSaveGameTemplateMutable(HE::UUID id) { return lookupAssetMutable(m_handleToUUID, m_saveTemplateAssets, id); }
const StructTypeAsset*    ContentManager::getStructType(HE::UUID id) const    { return lookupAsset(m_handleToUUID, m_structTypeAssets, id); }
StructTypeAsset*          ContentManager::getStructTypeMutable(HE::UUID id)   { return lookupAssetMutable(m_handleToUUID, m_structTypeAssets, id); }
const EnumTypeAsset*      ContentManager::getEnumType(HE::UUID id) const      { return lookupAsset(m_handleToUUID, m_enumTypeAssets, id); }
EnumTypeAsset*            ContentManager::getEnumTypeMutable(HE::UUID id)     { return lookupAssetMutable(m_handleToUUID, m_enumTypeAssets, id); }
MaterialAsset*            ContentManager::getMaterialMutable(HE::UUID id)     { return lookupAssetMutable(m_handleToUUID, m_materialAssets, id); }

bool ContentManager::setMaterialParam(HE::UUID id, const std::string& name,
                                      const float* values, int count)
{
	if (!values || count < 1) return false;
	MaterialAsset* a = getMaterialMutable(id);
	if (!a) return false;
	int slot = -1;
	for (size_t i = 0; i < a->graphParamNames.size(); ++i)
		if (a->graphParamNames[i] == name) { slot = (int)i; break; }
	if (slot < 0) return false;
	// Each parameter occupies one vec4 (4 floats) in shaderParamData, in slot order.
	const size_t base = (size_t)slot * 4;
	if (a->shaderParamData.size() < base + 4) a->shaderParamData.resize(base + 4, 0.0f);
	for (int i = 0; i < count && i < 4; ++i) a->shaderParamData[base + i] = values[i];
	return true;
}

bool ContentManager::getMaterialParam(HE::UUID id, const std::string& name, float out[4]) const
{
	const MaterialAsset* a = getMaterial(id);
	if (!a) return false;
	int slot = -1;
	for (size_t i = 0; i < a->graphParamNames.size(); ++i)
		if (a->graphParamNames[i] == name) { slot = (int)i; break; }
	if (slot < 0) return false;
	const size_t base = (size_t)slot * 4;
	for (int i = 0; i < 4; ++i)
		out[i] = (base + i < a->shaderParamData.size()) ? a->shaderParamData[base + i] : 0.0f;
	return true;
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

HE::UUID ContentManager::registerStaticMesh(StaticMeshAsset asset)       { ensureMeshUVs(asset); return registerRuntimeAsset(m_staticMeshAssets,  std::move(asset), HE::AssetType::StaticMesh);    }
HE::UUID ContentManager::registerSkeletalMesh(SkeletalMeshAsset asset)   { return registerRuntimeAsset(m_skeletalMeshAssets, std::move(asset), HE::AssetType::SkeletalMesh); }
HE::UUID ContentManager::registerTexture(TextureAsset asset)             { return registerRuntimeAsset(m_textureAssets,     std::move(asset), HE::AssetType::Texture);       }
HE::UUID ContentManager::registerMaterial(MaterialAsset asset)           { return registerRuntimeAsset(m_materialAssets,    std::move(asset), HE::AssetType::Material);      }
HE::UUID ContentManager::registerPrefab(PrefabAsset asset)               { return registerRuntimeAsset(m_prefabAssets,      std::move(asset), HE::AssetType::Prefab);        }
HE::UUID ContentManager::registerAudio(AudioAsset asset)                 { return registerRuntimeAsset(m_audioAssets,       std::move(asset), HE::AssetType::Audio);         }
HE::UUID ContentManager::registerScript(ScriptAsset asset)               { return registerRuntimeAsset(m_scriptAssets,      std::move(asset), HE::AssetType::Script);        }
HE::UUID ContentManager::registerMaterialFunction(MaterialFunctionAsset asset) { return registerRuntimeAsset(m_materialFunctionAssets, std::move(asset), HE::AssetType::MaterialFunction); }
HE::UUID ContentManager::registerWidget(UIWidgetAsset asset) { return registerRuntimeAsset(m_widgetAssets, std::move(asset), HE::AssetType::Widget); }
HE::UUID ContentManager::registerHorizonCodeClass(HorizonCodeClassAsset asset) { return registerRuntimeAsset(m_hcClassAssets, std::move(asset), HE::AssetType::HorizonCodeClass); }
HE::UUID ContentManager::registerInputAction(InputActionAsset asset)                 { return registerRuntimeAsset(m_inputActionAssets,  std::move(asset), HE::AssetType::InputAction); }
HE::UUID ContentManager::registerInputMappingContext(InputMappingContextAsset asset) { return registerRuntimeAsset(m_inputMappingAssets, std::move(asset), HE::AssetType::InputMappingContext); }
HE::UUID ContentManager::registerParticleGraph(ParticleGraphAsset asset) { return registerRuntimeAsset(m_particleGraphAssets, std::move(asset), HE::AssetType::ParticleSystem); }
HE::UUID ContentManager::registerAnimatorStateMachine(AnimatorStateMachineAsset asset) { return registerRuntimeAsset(m_animatorStateMachineAssets, std::move(asset), HE::AssetType::AnimatorStateMachine); }
HE::UUID ContentManager::registerAnimationClip(AnimationClipAsset asset)       { return registerRuntimeAsset(m_animClipAssets,     std::move(asset), HE::AssetType::AnimationClip);     }
HE::UUID ContentManager::registerPropertyAnimClip(PropertyAnimClipAsset asset) { return registerRuntimeAsset(m_propAnimClipAssets, std::move(asset), HE::AssetType::PropertyAnimClip); }
HE::UUID ContentManager::registerStructType(StructTypeAsset asset) { return registerRuntimeAsset(m_structTypeAssets, std::move(asset), HE::AssetType::StructType); }
HE::UUID ContentManager::registerSaveGameTemplate(SaveGameTemplateAsset asset) { return registerRuntimeAsset(m_saveTemplateAssets, std::move(asset), HE::AssetType::SaveGameTemplate); }
HE::UUID ContentManager::registerTheme(ThemeAsset asset) { return registerRuntimeAsset(m_themeAssets, std::move(asset), HE::AssetType::Theme); }
HE::UUID ContentManager::registerEnumType(EnumTypeAsset asset)     { return registerRuntimeAsset(m_enumTypeAssets,   std::move(asset), HE::AssetType::EnumType);   }

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

	// This chain MUST list every SlotMap member of the class. A type missing here
	// makes unloadAsset() return false for it, which silently disables hot reload
	// (pollHotReload unloads-then-reloads) and leaks the slot on manual delete —
	// exactly what happened to InputAction/InputMappingContext/ParticleGraph/
	// AnimatorStateMachine after those maps were added.
	const bool removed =
		tryRemove(m_staticMeshAssets)   || tryRemove(m_skeletalMeshAssets) ||
		tryRemove(m_textureAssets)      || tryRemove(m_materialAssets)     ||
		tryRemove(m_sceneAssets)        || tryRemove(m_scriptAssets)       ||
		tryRemove(m_audioAssets)        || tryRemove(m_fontAssets)         ||
		tryRemove(m_shaderAssets)       || tryRemove(m_animClipAssets) ||
		tryRemove(m_propAnimClipAssets) || tryRemove(m_materialFunctionAssets) ||
		tryRemove(m_widgetAssets)       || tryRemove(m_hcClassAssets)     ||
		tryRemove(m_prefabAssets)       || tryRemove(m_inputActionAssets) ||
		tryRemove(m_inputMappingAssets) || tryRemove(m_particleGraphAssets) ||
		tryRemove(m_animatorStateMachineAssets);
	if (!removed)
		return false;

	for (auto pit = m_pathToUUID.begin(); pit != m_pathToUUID.end(); ++pit)
		if (pit->second == id) { m_pathMtime.erase(pit->first); m_pathToUUID.erase(pit); break; }
	m_handleToUUID.erase(id);
	// The type index has TWO origins: loading an asset (this entry dies with the
	// load) and mounting a pak that ships an asset type index (that entry belongs
	// to the archive, not to the load). m_pakResidency is exactly the question
	// "can a mounted pak supply this UUID again?" — so it is the discriminator.
	// Dropping a pak asset's type here would make it undiscoverable forever:
	// discoverAssets would stop listing it, so nothing would ever ask
	// ensureResident to bring it back. Unload → discover → reload must be a loop.
	// A loose-content asset keeps the old behaviour; the content walk re-sniffs it.
	if (!m_pakResidency.count(id))
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

std::vector<HE::UUID> ContentManager::discoverAssets(HE::AssetType type)
{
	std::vector<HE::UUID>        out = enumerateIds(type);
	std::unordered_set<HE::UUID> seen(out.begin(), out.end());

	// The ids above may be pak entries that are known (mountPak read the pak's
	// asset type index) but NOT loaded. Every getXxx(id) accessor is a pure cache
	// lookup — it does not load — so handing those ids back unloaded would give
	// the caller a list of UUIDs whose assets all read as null, which is what
	// PlayerHost saw. Discovery therefore delivers usable assets, exactly like the
	// loose-content walk below, which has always ended in loadAsset(). Best-effort:
	// an id whose payload fails to load stays in the list (every caller null-checks
	// the accessor) rather than silently disappearing from the enumeration.
	// Safe against iterator invalidation: enumerateIds already returned a copy,
	// while ensureResident writes into m_assetTypeIndex.
	for (const HE::UUID& id : out)
		ensureResident(id);

	const std::string root = contentRoot();
	std::error_code ec;
	if (root.empty() || !std::filesystem::is_directory(root, ec)) return out;

	std::filesystem::recursive_directory_iterator it(root, ec), end;
	for (; it != end; it.increment(ec))
	{
		if (ec) break;
		if (!it->is_regular_file(ec) || it->path().extension() != ".hasset") continue;
		HAsset::Reader r;
		if (!r.open(it->path().string()) ||
		    r.assetType() != static_cast<uint16_t>(type)) continue;
		const std::string rel =
			std::filesystem::relative(it->path(), root, ec).generic_string();
		const HE::UUID id = loadAsset(rel);
		if (!(id == HE::UUID{}) && seen.insert(id).second) out.push_back(id);
	}
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
		const std::string fullPath = resolveAbsolutePath(relPath);
		std::error_code ec;
		const auto mtime = fs::last_write_time(fullPath, ec);
		if (ec) continue; // file deleted or inaccessible

		auto storedIt = m_pathMtime.find(relPath);
		if (storedIt == m_pathMtime.end() || mtime == storedIt->second)
			continue; // not in map yet or unchanged

		// Skip files that aren't valid .hasset yet (mid-write / partial save).
		// Avoids evicting the live asset before the new version is readable.
		if (sniffAssetTypeFromFile(fullPath) == HE::AssetType::Unknown) continue;

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

// ─── retargetAssetReferences ─────────────────────────────────────────────────
size_t ContentManager::retargetAssetReferences(const std::string& oldRel,
                                                const std::string& newRel,
                                                bool folder)
{
	// Both halves, in the order they must happen. Kept as one call for every
	// caller that is not a collaboration session and can afford to wait.
	retargetAssetReferencesInMemory(oldRel, newRel, folder);
	return retargetAssetReferencesOnDisk(oldRel, newRel, folder);
}

size_t ContentManager::retargetAssetReferencesOnDisk(const std::string& oldRel,
                                                     const std::string& newRel,
                                                     bool folder) const
{
	// One move is the degenerate batch. Kept as its own entry point because most
	// callers have exactly one and should not have to build a vector to say so.
	return retargetAssetReferencesOnDisk(
		std::vector<MoveSpec>{ MoveSpec{ oldRel, newRel, folder } });
}

size_t ContentManager::retargetAssetReferencesOnDisk(const std::vector<MoveSpec>& moves) const
{
	namespace fs = std::filesystem;
	if (m_contentRoot.empty() || moves.empty()) return 0;

	// Scene references are project-relative ("Content/Level.hescene"), so the
	// rules need the content directory's own name as their second form.
	const std::string contentDir = fs::path(m_contentRoot).filename().generic_string();
	std::vector<HE::AssetRefs::Rule> rules;
	std::string firstOld, lastNew;
	for (const MoveSpec& m : moves)
	{
		if (m.oldRelativePath.empty() || m.newRelativePath.empty() ||
		    m.oldRelativePath == m.newRelativePath) continue;
		const std::vector<HE::AssetRefs::Rule> one =
			HE::AssetRefs::moveRules(m.oldRelativePath, m.newRelativePath, m.folder, contentDir);
		rules.insert(rules.end(), one.begin(), one.end());
		if (firstOld.empty()) firstOld = m.oldRelativePath;
		lastNew = m.newRelativePath;
	}
	if (rules.empty()) return 0;

	// Engine DEFAULTS are read-only ground and never move; their project-local
	// overrides live under the content root, so one walk covers everything.
	size_t rewritten = HE::AssetRefs::retargetTree(m_contentRoot, rules);

	// The project manifest names the startup scene by the same project-relative
	// path a moved scene just invalidated.
	{
		std::error_code ec;
		const fs::path projectRoot = fs::path(m_contentRoot).parent_path();
		for (const auto& e : fs::directory_iterator(projectRoot, ec))
		{
			if (ec) break;
			std::error_code fec;
			if (!e.is_regular_file(fec) || fec || e.path().extension() != ".heproj") continue;
			std::string text;
			{
				std::ifstream f(e.path(), std::ios::binary);
				if (!f) continue;
				text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
			}
			if (!HE::AssetRefs::retargetJsonText(text, rules)) continue;
			// Temp + rename, like ProjectManager's own save of this very file: the
			// .heproj is the project — truncating it in place means a failed write
			// leaves a manifest that no longer opens, in exchange for a startup
			// scene path. Closed and checked BEFORE the rename, because the
			// destructor's flush happens after any `if (o)` written here would
			// have already reported success.
			const fs::path tmpPath = e.path().string() + ".tmp";
			{
				std::ofstream o(tmpPath, std::ios::binary | std::ios::trunc);
				if (!o) continue;
				o.write(text.data(), static_cast<std::streamsize>(text.size()));
				o.close();
				if (!o)
				{
					std::error_code rec;
					fs::remove(tmpPath, rec);
					continue;
				}
			}
			std::error_code rnec;
			fs::rename(tmpPath, e.path(), rnec);
			if (rnec)
			{
				std::error_code rec;
				fs::remove(tmpPath, rec);
				continue;
			}
			++rewritten;
		}
	}

	if (rewritten > 0)
	{
		const std::string what = moves.size() == 1
			? ("'" + firstOld + "' to '" + lastNew + "'")
			: (std::to_string(moves.size()) + " moved paths");
		HE_LOG_INFO(Asset, "%s",
			("ContentManager: retargeted " + std::to_string(rewritten) +
			 " file(s) for " + what).c_str());
	}
	return rewritten;
}

void ContentManager::retargetAssetReferencesInMemory(const std::string& oldRel,
                                                     const std::string& newRel,
                                                     bool folder)
{
	namespace fs = std::filesystem;
	if (m_contentRoot.empty() || oldRel.empty() || newRel.empty() || oldRel == newRel)
		return;

	const std::string contentDir = fs::path(m_contentRoot).filename().generic_string();
	const std::vector<HE::AssetRefs::Rule> rules =
		HE::AssetRefs::moveRules(oldRel, newRel, folder, contentDir);
	if (rules.empty()) return;

	// Re-key the in-memory indices: every one of them is keyed by (or holds) the
	// path the file no longer has. Without this, loadAsset() of the new path would
	// register a SECOND copy of the same UUID, saveAsset() of an already-open
	// asset would write back to the vacated location, and ensureResident() would
	// look the moved file up where it used to be.
	auto rekeyMap = [&](auto& map)
	{
		using MapT = std::decay_t<decltype(map)>;
		MapT moved;
		for (auto it = map.begin(); it != map.end(); )
		{
			std::string key = it->first;
			if (HE::AssetRefs::retargetValue(key, rules))
			{
				moved.emplace(std::move(key), std::move(it->second));
				it = map.erase(it);
			}
			else ++it;
		}
		for (auto& [k, v] : moved) map[k] = std::move(v);
	};
	rekeyMap(m_pathToUUID);
	rekeyMap(m_pathMtime);
	for (auto& [id, path] : m_diskRegistry) HE::AssetRefs::retargetValue(path, rules);

	// …and the `path` an already-loaded asset carries, which is where saveAsset()
	// writes it back to.
	auto rekeyAssetPaths = [&](auto& slotMap)
	{
		for (auto& asset : slotMap) HE::AssetRefs::retargetValue(asset.path, rules);
	};
	rekeyAssetPaths(m_staticMeshAssets);   rekeyAssetPaths(m_skeletalMeshAssets);
	rekeyAssetPaths(m_textureAssets);      rekeyAssetPaths(m_materialAssets);
	rekeyAssetPaths(m_sceneAssets);        rekeyAssetPaths(m_scriptAssets);
	rekeyAssetPaths(m_materialFunctionAssets); rekeyAssetPaths(m_widgetAssets);
	rekeyAssetPaths(m_hcClassAssets);      rekeyAssetPaths(m_inputActionAssets);
	rekeyAssetPaths(m_inputMappingAssets); rekeyAssetPaths(m_particleGraphAssets);
	rekeyAssetPaths(m_animatorStateMachineAssets);
	rekeyAssetPaths(m_audioAssets);        rekeyAssetPaths(m_fontAssets);
	rekeyAssetPaths(m_shaderAssets);       rekeyAssetPaths(m_prefabAssets);
	rekeyAssetPaths(m_animClipAssets);     rekeyAssetPaths(m_propAnimClipAssets);
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

	// Merge this pak's asset path index (path → UUID) so loadAsset("<path>") can
	// resolve a pak-only asset. Later mounts overwrite (overlay priority), matching
	// the residency rule above. Best-effort: an older pak without the index (or a
	// mod pak) simply contributes nothing here.
	{
		const HE::UUID idxId = sceneUuidForPath(kAssetPathIndexEntry);
		if (reader->hasEntry(idxId))
		{
			const auto bytes = reader->readEntry(idxId, key);
			if (!bytes.empty())
			{
				const auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
				if (j.is_object())
					for (auto it = j.begin(); it != j.end(); ++it)
					{
						if (!it.value().is_string()) continue;
						const std::string v = it.value().get<std::string>();
						const auto colon = v.find(':');
						if (colon == std::string::npos) continue;
						HE::UUID id{};
						try {
							id.hi = std::stoull(v.substr(0, colon));
							id.lo = std::stoull(v.substr(colon + 1));
						} catch (...) { continue; }
						m_pakPathIndex[it.key()] = id;
					}
			}
		}
	}

	// Merge this pak's asset TYPE index ("hi:lo" → HE::AssetType) so the manager
	// knows what a pak asset IS before anything loads it. Mounting used to
	// contribute residency and paths only, and a type was learned on load — so
	// enumerateIds(type)/discoverAssets(type) came back empty in a packaged game,
	// PlayerHost found no PlayerController class to instantiate, no BeginPlay ran
	// and the game booted into an empty world. Later mounts overwrite (overlay
	// priority), matching the residency rule above. Best-effort like the path
	// index: an older pak (or a mod pak) without the entry contributes nothing.
	{
		const HE::UUID idxId = sceneUuidForPath(kAssetTypeIndexEntry);
		if (reader->hasEntry(idxId))
		{
			const auto bytes = reader->readEntry(idxId, key);
			if (!bytes.empty())
			{
				const auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
				if (j.is_object())
					for (auto it = j.begin(); it != j.end(); ++it)
					{
						// Mirror image of the path index: here the "hi:lo" is the KEY
						// and the value is the AssetType as a number.
						if (!it.value().is_number_unsigned()) continue;
						const std::string& idKey = it.key();
						const auto colon = idKey.find(':');
						if (colon == std::string::npos) continue;
						HE::UUID id{};
						try {
							id.hi = std::stoull(idKey.substr(0, colon));
							id.lo = std::stoull(idKey.substr(colon + 1));
						} catch (...) { continue; }
						const auto t = static_cast<HE::AssetType>(it.value().get<uint32_t>());
						// Unknown says nothing a missing entry does not already say
						// (assetType() reports Unknown either way). A value from a
						// NEWER exporter that this build has no enumerator for is
						// kept as-is and simply never equals any type a caller asks
						// for — no range check needed to make that safe.
						if (t == HE::AssetType::Unknown) continue;
						m_assetTypeIndex[id] = t;
					}
			}
		}
	}

	mount.reader = std::move(reader);
	m_mounts.push_back(std::move(mount));
	return true;
}

size_t ContentManager::mountPakOverlays(const std::filesystem::path& dir)
{
	std::error_code ec;
	if (!std::filesystem::is_directory(dir, ec)) return 0;

	// Collect + sort by filename so mount order (= overlay priority) is
	// deterministic across platforms and directory-iteration orders.
	std::vector<std::filesystem::path> paks;
	for (const auto& p : std::filesystem::directory_iterator(dir, ec))
		if (p.is_regular_file(ec) && p.path().extension() == ".hpak")
			paks.push_back(p.path());
	std::sort(paks.begin(), paks.end(),
	          [](const auto& a, const auto& b) { return a.filename() < b.filename(); });

	size_t mounted = 0;
	for (const auto& p : paks)
		if (mountPak(p.string()))
			++mounted;
	return mounted;
}

// Index every .hasset under `root` into m_diskRegistry (UUID → relative path),
// each key prefixed with `pathPrefix` so loadAsset()/resolveAbsolutePath() route
// it back to the right root. Cheap header-only META sniff per file.
void ContentManager::scanDirInto(const std::string& rootStr, const std::string& pathPrefix)
{
	if (rootStr.empty()) return;
	std::error_code ec;
	const std::filesystem::path root(rootStr);
	if (!std::filesystem::is_directory(root, ec)) return;

	for (const auto& p : std::filesystem::recursive_directory_iterator(root, ec))
	{
		if (!p.is_regular_file(ec) || p.path().extension() != ".hasset") continue;

		// Stream just the header + chunk headers; read only the META payload and
		// skip everything else, so indexing stays cheap even for large assets.
		std::ifstream f(p.path(), std::ios::binary);
		if (!f) continue;
		HAsset::FileHeader hdr{};
		f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
		if (!f || std::memcmp(hdr.magic, HAsset::k_magic, 4) != 0) continue;

		for (uint32_t i = 0; i < hdr.chunk_count && f; ++i)
		{
			HAsset::ChunkHeader ch{};
			f.read(reinterpret_cast<char*>(&ch), sizeof(ch));
			if (!f) break;
			if (ch.id != HAsset::CHUNK_META)
			{
				f.seekg(static_cast<std::streamoff>(ch.size), std::ios::cur);
				continue;
			}
			HAsset::Reader::Chunk meta;
			meta.id = ch.id;
			meta.data.resize(static_cast<size_t>(ch.size));
			f.read(reinterpret_cast<char*>(meta.data.data()),
			       static_cast<std::streamsize>(ch.size));
			HE::UUID id; std::string name, metaPath;
			if (f && readMetaChunk(meta, hdr.version, id, name, metaPath) && id != HE::UUID{})
			{
				// Key by the file's ACTUAL location relative to its root (not the
				// META-embedded path) so moved/renamed files still resolve.
				const auto rel = std::filesystem::relative(p.path(), root, ec);
				if (!ec) m_diskRegistry[id] = pathPrefix + rel.generic_string();
			}
			break; // META handled — done with this file
		}
	}
}

size_t ContentManager::scanContentDirectory()
{
	m_diskRegistry.clear();
	// Project content first, then engine defaults. A project override
	// (Content/Engine/...) is picked up by the FIRST scan as "Engine/<rest>"
	// (its content-relative path already carries the prefix), and the engine
	// scan writes the same "Engine/<rest>" key for the shared default it
	// shadows — identical value, so order is immaterial; resolveAbsolutePath
	// still prefers the override at load time. Engine defaults with no override
	// are what the engine scan uniquely adds — WITHOUT it, a scene that
	// references a built-in mesh/material by UUID falls back to the default
	// cube on reload (the UUID resolves to nothing on disk).
	scanDirInto(m_contentRoot, "");
	scanDirInto(m_engineContentRoot, std::string(kEnginePrefix));
	// EngineContent assets fetched by a previous SFTP sync (HE_ContentSync) —
	// same "Engine/<rest>" key shape as the shared default scan above, so which
	// scan happens to write a given UUID first is immaterial (see the two
	// scanDirInto calls' comment): resolveAbsolutePath() re-derives the actual
	// override/default/cache precedence from the filesystem every time anyway.
	scanDirInto(GlobalState::engineContentCacheDir().string(), std::string(kEnginePrefix));
	return m_diskRegistry.size();
}

bool ContentManager::ensureResident(HE::UUID id)
{
	if (isLoaded(id)) return true;

	const auto it = m_pakResidency.find(id);
	if (it == m_pakResidency.end())
	{
		// Not in any mounted pak — fall back to loose content on disk.
		const auto d = m_diskRegistry.find(id);
		if (d != m_diskRegistry.end())
		{
			loadAsset(d->second);
			return isLoaded(id);
		}
		// A remote-only EngineContent asset (see registerRemoteAsset): materializing
		// it is a network operation, and ensureResident is synchronous/main-thread by
		// contract — waiting here would freeze the Editor on every reference to an
		// undownloaded default. Kick the same materialization loadAssetAsync() would
		// off in the background instead, and report "not resident YET", exactly what
		// this function already returns for any other not-yet-loaded UUID. A caller
		// that needs to know when it lands should call loadAssetAsync(id, callback).
		if (m_remoteAssets.count(id)) loadAssetAsync(id);
		return false;
	}

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

	// ── Default landscape weightmap (kDefaultLayer0WeightTextureId) ──────────
	// Bound for terrain chunks whose landscape has no painted weights yet, so a
	// layer-blend material shows layer 0 instead of an average or a black hole.
	TextureAsset w0;
	w0.id       = HE::kDefaultLayer0WeightTextureId;
	w0.name     = "DefaultLayer0Weight";
	w0.path     = "mem://default_layer0_weight";
	w0.data     = { 255, 0, 0, 0 };
	w0.width    = 1;
	w0.height   = 1;
	w0.channels = 4;
	registerTexture(std::move(w0));

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