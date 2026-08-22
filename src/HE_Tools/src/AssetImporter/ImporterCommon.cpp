#include "ImporterCommon.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include "ContentManager/ContentManager.h"
#include "ContentManager/HAsset.h"
#include "Diagnostics/Logger.h"
// The whole importer set, because importSource() below is the one place that
// decides which of them a given file belongs to.
#include "AnimationClipImporter.h"
#include "AudioImporter.h"
#include "FontImporter.h"
#include "MaterialImporter.h"
#include "MeshImporter.h"
#include "SkeletalMeshImporter.h"
#include "TextureImporter.h"

// Declarations only — CGLTF_IMPLEMENTATION is defined exactly once, in
// MeshImporter.cpp, for the whole HorizonImporters link unit.
#include "cgltf.h"

#include <glm/glm.hpp>

namespace Importer
{

std::string toAssetPath(const std::filesystem::path& relativePath)
{
	std::string s = relativePath.generic_string();
	// Strip a leading "./" that std::filesystem::relative sometimes produces
	if (s.rfind("./", 0) == 0)
		s.erase(0, 2);
	return s;
}

ResolvedOutput resolveOutput(const std::string&           explicitPath,
                             const std::filesystem::path& relativeOutputDir,
                             const std::string&           derivedStem)
{
	ResolvedOutput out;
	out.path = explicitPath.empty()
		? toAssetPath(relativeOutputDir / (derivedStem + ".hasset"))
		: explicitPath;
	// The name is read back OFF the resolved path, not taken from derivedStem: a
	// re-import of a renamed asset would otherwise write the source's stem into
	// the META of a file called something else, and that name is what the engine
	// reports the asset as wherever the file name is not what is shown.
	out.name = std::filesystem::path(out.path).stem().string();
	return out;
}

bool gltfHasSkin(const std::filesystem::path& sourcePath)
{
	cgltf_options options{};
	cgltf_data*   data = nullptr;
	// cgltf_parse_file reads the JSON only — skins_count is filled in without
	// touching the (potentially large) .bin buffers or embedded images.
	if (cgltf_parse_file(&options, sourcePath.string().c_str(), &data) != cgltf_result_success)
		return false;
	const bool skinned = data->skins_count > 0;
	cgltf_free(data);
	return skinned;
}

namespace
{
// What one .hasset's META chunk says about itself. Everything is optional in the
// sense that a file which does not carry it leaves the field at its default —
// this is an answer ("no id", "no recorded source"), never an error.
struct MetaFields
{
	HE::UUID    id;
	std::string name;
	std::string path;
	std::string source;
};

// Reads META WITHOUT pulling the rest of the file into memory: HAsset::Reader::open
// materialises every chunk payload, so asking a mesh for its 16-byte id used to
// read its whole vertex buffer — and sourceFileOf() below is called once per FRAME
// by the Content Browser while a context menu is open, i.e. that whole payload was
// re-read every frame the user hovered a large asset. Same streaming shape as
// HE::AssetRefs::assetUuidOfFile, which exists for exactly this reason.
//
// Opened at the end so the real file size is known up front: every declared size in
// the file is untrusted and has to be bounded against it before a byte is
// allocated. Both HAsset readers carry that check with a comment saying a corrupt
// size would otherwise resize() to gigabytes; keeping the seek-past-payload shape
// means keeping the bound with it.
//
// Field order mirrors ContentManager's buildMetaChunk: type, hi, lo, name, path,
// source. The source is an append-only TAIL — an asset written before the field
// existed simply runs out of bytes there, which leaves `source` empty rather than
// failing the parse and making every existing .hasset look unreadable.
bool readMetaFields(const std::filesystem::path& file, MetaFields& out)
{
	std::ifstream f(file, std::ios::binary | std::ios::ate);
	if (!f.is_open()) return false;

	const std::streamoff fileEnd = f.tellg();
	if (fileEnd < static_cast<std::streamoff>(sizeof(HAsset::FileHeader))) return false;
	const uint64_t fileSize = static_cast<uint64_t>(fileEnd);
	f.seekg(0, std::ios::beg);

	HAsset::FileHeader hdr{};
	f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
	if (!f || std::memcmp(hdr.magic, HAsset::k_magic, 4) != 0) return false;
	// Pre-v2 META has no UUID, so its name/path sit where the id would be — reading
	// it with this layout would hand back garbage rather than "nothing recorded".
	if (hdr.version < 2) return false;

	uint64_t offset = sizeof(HAsset::FileHeader);
	for (uint32_t i = 0; i < hdr.chunk_count; ++i)
	{
		if (offset + sizeof(HAsset::ChunkHeader) > fileSize) return false;
		HAsset::ChunkHeader ch{};
		f.read(reinterpret_cast<char*>(&ch), sizeof(ch));
		if (!f) return false;
		offset += sizeof(HAsset::ChunkHeader);
		// Compared against the REMAINDER, never as offset + size, which can wrap:
		// size is an untrusted uint64 straight out of the file.
		if (ch.size > fileSize - offset) return false;

		if (ch.id != HAsset::CHUNK_META)
		{
			f.seekg(static_cast<std::streamoff>(ch.size), std::ios::cur);
			if (!f) return false;
			offset += ch.size;
			continue;
		}

		std::vector<uint8_t> meta(static_cast<size_t>(ch.size));
		if (ch.size > 0)
			f.read(reinterpret_cast<char*>(meta.data()),
			       static_cast<std::streamsize>(ch.size));
		if (!f) return false;

		size_t off = sizeof(uint16_t); // skip asset type
		if (!HAsset::Reader::readPOD(meta, off, out.id.hi))   return false;
		if (!HAsset::Reader::readPOD(meta, off, out.id.lo))   return false;
		if (!HAsset::Reader::readString(meta, off, out.name)) return false;
		if (!HAsset::Reader::readString(meta, off, out.path)) return false;
		HAsset::Reader::readString(meta, off, out.source);    // optional tail
		return true;
	}
	return false;   // a well-formed file with no META: nothing recorded
}
} // namespace

static HE::UUID existingUUID(const std::filesystem::path& file)
{
	MetaFields meta;
	if (!readMetaFields(file, meta))
		return HE::UUID{};
	return meta.id;
}

std::string sourceFileOf(const std::filesystem::path& assetFile)
{
	MetaFields meta;
	if (!readMetaFields(assetFile, meta))
		return {};
	return meta.source;
}

bool writeAsset(RuntimeAsset& asset, const std::filesystem::path& contentRoot,
                const std::filesystem::path& sourceFile)
{
	const std::filesystem::path target = contentRoot / asset.path;

	std::error_code ec;
	std::filesystem::create_directories(target.parent_path(), ec);

	// Re-import: keep the identity the asset already has on disk.
	if (asset.id == HE::UUID{})
		asset.id = existingUUID(target);

	if (!sourceFile.empty())
	{
		// Absolute, and with any "../" collapsed: the asset compiler is handed
		// relative paths from a command line whose working directory nothing later
		// remembers, so storing them verbatim records a source that resolves to a
		// different file — or to none — the next time anyone reads it.
		std::error_code srcEc;
		std::filesystem::path abs = std::filesystem::weakly_canonical(sourceFile, srcEc);
		if (srcEc || abs.empty())
		{
			abs = std::filesystem::absolute(sourceFile, srcEc);
			if (srcEc) abs = sourceFile;
		}
		asset.sourcePath = abs.generic_string();
	}
	else if (asset.sourcePath.empty())
	{
		// A rewrite with no source of its own must not erase the one already on
		// disk — sidecar writes (a mesh's generated material, an animation clip)
		// go through here too, and they would otherwise blank out the provenance
		// of an asset the user had imported directly.
		asset.sourcePath = sourceFileOf(target);
	}

	ContentManager cm(contentRoot.string());
	if (!cm.saveAsset(asset))
	{
		HE_LOG_ERROR(Tool, "%s",
			("Importer: failed to write " + target.string()).c_str());
		return false;
	}
	return true;
}

// ─── Shared glTF geometry path ───────────────────────────────────────────────

GltfPrimitiveAttributes appendPrimitive(const cgltf_primitive& prim,
                                        const glm::mat4&       world,
                                        float                  uniformScale,
                                        MeshVertexStreams      out,
                                        std::vector<uint32_t>& indices,
                                        int                    uvSet)
{
	if (prim.type != cgltf_primitive_type_triangles)
		return {};

	GltfPrimitiveAttributes attrs;
	const cgltf_accessor*   uv0 = nullptr;   // fallback when the wanted set is absent
	for (cgltf_size i = 0; i < prim.attributes_count; ++i)
	{
		const cgltf_attribute& attr = prim.attributes[i];
		switch (attr.type)
		{
		case cgltf_attribute_type_position: attrs.position = attr.data; break;
		case cgltf_attribute_type_normal:   attrs.normal   = attr.data; break;
		case cgltf_attribute_type_texcoord:
			if (attr.index == uvSet) { attrs.uv = attr.data; attrs.uvSet = uvSet; }
			if (attr.index == 0)     uv0 = attr.data;
			break;
		case cgltf_attribute_type_joints:   if (attr.index == 0) attrs.joints  = attr.data; break;
		case cgltf_attribute_type_weights:  if (attr.index == 0) attrs.weights = attr.data; break;
		default: break;
		}
	}
	// The material asks for a set this primitive does not carry. TEXCOORD_0 is the
	// only sane fallback — leaving the UVs at (0,0) would collapse the whole
	// primitive onto one texel — and the returned uvSet tells the caller to say so.
	if (!attrs.uv && uv0) { attrs.uv = uv0; attrs.uvSet = 0; }
	if (!attrs.position)
		return {};

	const uint32_t  baseVertex = static_cast<uint32_t>(out.positions.size() / 3);
	const glm::mat3 normalMat  = glm::transpose(glm::inverse(glm::mat3(world)));

	for (cgltf_size v = 0; v < attrs.position->count; ++v)
	{
		float p[3] = {};
		cgltf_accessor_read_float(attrs.position, v, p, 3);
		glm::vec3 wp = glm::vec3(world * glm::vec4(p[0], p[1], p[2], 1.0f)) * uniformScale;
		out.positions.insert(out.positions.end(), { wp.x, wp.y, wp.z });

		if (attrs.normal && v < attrs.normal->count)
		{
			float n[3] = {};
			cgltf_accessor_read_float(attrs.normal, v, n, 3);
			glm::vec3 wn = glm::normalize(normalMat * glm::vec3(n[0], n[1], n[2]));
			out.normals.insert(out.normals.end(), { wn.x, wn.y, wn.z });
		}
		else
			out.normals.insert(out.normals.end(), { 0.0f, 0.0f, 0.0f });

		if (attrs.uv && v < attrs.uv->count)
		{
			float uv[2] = {};
			cgltf_accessor_read_float(attrs.uv, v, uv, 2);
			// glTF puts the UV origin at the TOP-left; the engine is GL-style
			// BOTTOM-left (TextureImporter flips images on import, the Metal
			// backend flips V at sample time to match). Taking glTF's V verbatim
			// rendered every imported mesh's texture vertically mirrored.
			out.uvs.insert(out.uvs.end(), { uv[0], 1.0f - uv[1] });
		}
		else
			out.uvs.insert(out.uvs.end(), { 0.0f, 0.0f });
	}

	if (prim.indices)
	{
		for (cgltf_size i = 0; i < prim.indices->count; ++i)
			indices.push_back(baseVertex
				+ static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i)));
	}
	else
	{
		for (cgltf_size i = 0; i < attrs.position->count; ++i)
			indices.push_back(baseVertex + static_cast<uint32_t>(i));
	}

	return attrs;
}

void appendSkinning(const GltfPrimitiveAttributes& attrs,
                    std::vector<uint32_t>&         boneIDs,
                    std::vector<float>&            boneWeights)
{
	if (!attrs.position)
		return;

	for (cgltf_size v = 0; v < attrs.position->count; ++v)
	{
		// Skinning: 4 joints + 4 weights per vertex
		uint32_t jids[4] = {};
		float    wts[4]  = { 1.0f, 0.0f, 0.0f, 0.0f };
		if (attrs.joints && v < attrs.joints->count)
		{
			float jf[4] = {};
			cgltf_accessor_read_float(attrs.joints, v, jf, 4);
			for (int k = 0; k < 4; ++k)
				jids[k] = static_cast<uint32_t>(jf[k]);
		}
		if (attrs.weights && v < attrs.weights->count)
			cgltf_accessor_read_float(attrs.weights, v, wts, 4);

		boneIDs.insert(boneIDs.end(), { jids[0], jids[1], jids[2], jids[3] });
		boneWeights.insert(boneWeights.end(), { wts[0], wts[1], wts[2], wts[3] });
	}
}

// ─── Source routing ──────────────────────────────────────────────────────────

namespace
{
enum class SourceKind { None, Mesh, Texture, Audio, Material, Font };

SourceKind classifySource(const std::filesystem::path& sourcePath)
{
	std::string ext = sourcePath.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	if (ext == ".gltf" || ext == ".glb")                      return SourceKind::Mesh;
	if (ext == ".png"  || ext == ".jpg" || ext == ".jpeg" ||
	    ext == ".tga"  || ext == ".bmp" || ext == ".hdr")     return SourceKind::Texture;
	if (ext == ".wav")                                        return SourceKind::Audio;
	if (ext == ".hmat")                                       return SourceKind::Material;
	if (ext == ".ttf"  || ext == ".otf")                      return SourceKind::Font;
	return SourceKind::None;
}
} // namespace

bool isImportableSource(const std::filesystem::path& sourcePath)
{
	return classifySource(sourcePath) != SourceKind::None;
}

bool importSource(const std::filesystem::path& sourcePath,
                  const std::filesystem::path& contentRoot,
                  const std::filesystem::path& relativeOutputDir,
                  const OutputTargets&         outputs)
{
	switch (classifySource(sourcePath))
	{
	case SourceKind::Mesh:
		if (gltfHasSkin(sourcePath))
		{
			// Rigged source: MeshImporter drops the skeleton and the JOINTS_0 /
			// WEIGHTS_0 attributes and registers bind-pose geometry as a StaticMesh,
			// so the mesh could never afterwards be picked as a SkeletalMesh.
			if (!SkeletalMeshImporter::import(sourcePath, contentRoot, relativeOutputDir,
			                                  SkeletalMeshImporter::ImportSettings{}, outputs))
				return false;
			// The animations usually live in the same glTF and become their own
			// assets (referenced by the AnimatorStateMachine). They are NOT sidecars
			// of the mesh: nothing on the mesh names them, so a re-import cannot
			// redirect them the way it redirects the material — see reimport().
			AnimationClipImporter::importAndWrite(sourcePath, contentRoot, relativeOutputDir);
			return true;
		}
		return MeshImporter::import(sourcePath, contentRoot, relativeOutputDir,
		                            MeshImporter::ImportSettings{}, outputs)     != nullptr;
	case SourceKind::Texture:
		return TextureImporter::import(sourcePath, contentRoot, relativeOutputDir,
		                               TextureImporter::ImportSettings{}, outputs) != nullptr;
	case SourceKind::Audio:
		return AudioImporter::import(sourcePath, contentRoot, relativeOutputDir,
		                             AudioImporter::ImportSettings{}, outputs)    != nullptr;
	case SourceKind::Material:
		return MaterialImporter::import(sourcePath, contentRoot, relativeOutputDir,
		                                outputs)                                  != nullptr;
	case SourceKind::Font:
		// 0 = "whatever FontImporter bakes at by default" — spelling the number out
		// here is how the two would drift apart.
		return FontImporter::import(sourcePath, contentRoot, relativeOutputDir,
		                            /*bakeSize=*/0, outputs)                      != nullptr;
	case SourceKind::None:
		HE_LOG_ERROR(Tool, "%s",
			("Importer: no importer for " + sourcePath.string()).c_str());
		return false;
	}
	return false;
}

bool reimport(const std::filesystem::path& assetFile,
              const std::filesystem::path& contentRoot)
{
	const std::string source = sourceFileOf(assetFile);
	if (source.empty())
	{
		HE_LOG_ERROR(Tool, "%s",
			("Importer: " + assetFile.string() + " records no import source").c_str());
		return false;
	}

	std::error_code ec;
	if (!std::filesystem::is_regular_file(source, ec) || ec)
	{
		// The usual cause is a project opened on a different machine: the recorded
		// path is absolute and points into someone else's DCC folder. Saying so
		// beats silently importing nothing.
		HE_LOG_ERROR(Tool, "%s",
			("Importer: source file is gone: " + source).c_str());
		return false;
	}

	// The re-import writes back into the folder the asset lives in TODAY. Deriving
	// it from the source instead would recreate the asset wherever the source
	// happens to sit — a second copy with a second UUID, while every scene keeps
	// pointing at the first.
	std::filesystem::path relDir =
		std::filesystem::relative(assetFile.parent_path(), contentRoot, ec);
	if (ec)
	{
		HE_LOG_ERROR(Tool, "%s",
			("Importer: cannot place " + assetFile.string() + " relative to " +
			 contentRoot.string() + ": " + ec.message()).c_str());
		return false;
	}
	if (relDir == ".") relDir.clear();
	// An asset outside the content root yields a "../.."-shaped relative path, and
	// following it would write the re-imported asset outside the project entirely.
	if (!relDir.empty() && *relDir.begin() == "..")
	{
		HE_LOG_ERROR(Tool, "%s",
			("Importer: " + assetFile.string() + " is not inside " + contentRoot.string()).c_str());
		return false;
	}

	// The folder alone is not enough. Every importer names its output after the
	// SOURCE stem, so re-importing an asset the user renamed after importing it
	// (rock_v1.hasset → Rock.hasset) wrote a brand-new rock_v1.hasset with a new
	// uuid beside it and left Rock.hasset — the file every scene references —
	// untouched, while still reporting success. The asset that was clicked is the
	// one that has to be written, so its own path is handed down.
	OutputTargets outputs;
	outputs.asset = toAssetPath(std::filesystem::relative(assetFile, contentRoot, ec));
	if (ec || outputs.asset.empty())
	{
		HE_LOG_ERROR(Tool, "%s",
			("Importer: cannot place " + assetFile.string() + " relative to " +
			 contentRoot.string()).c_str());
		return false;
	}

	// The same trap one level down: a mesh import also writes a material and a
	// base-colour texture named after the source. Both are read back OFF the mesh
	// (MREF → MTRL, kept current by the rename retarget) instead of re-derived, so
	// a renamed sidecar is overwritten in place rather than duplicated. An empty
	// list is not a failure — it means this asset has no sidecars yet, e.g. a glTF
	// that has only NOW gained a base-colour texture, and those outputs are
	// legitimately new files under source-derived names.
	const std::vector<std::string> sidecars = meshSidecarAssets(assetFile, contentRoot);
	if (sidecars.size() > 0) outputs.material = sidecars[0];
	if (sidecars.size() > 1) outputs.texture  = sidecars[1];

	// A rigged glTF also produces its animation clips. Those are NOT sidecars —
	// no chunk on the mesh names them, and the clips record no source of their
	// own — so there is nothing to redirect them by, and a clip the user renamed
	// comes back under its old name as a second asset. That case cannot be told
	// apart from the legitimate one (the artist added an animation to the glTF),
	// so it is reported rather than refused: failing here would make Reimport
	// unusable for the ordinary "the source grew" workflow.
	std::vector<std::string> clipsBefore;
	if (classifySource(source) == SourceKind::Mesh && gltfHasSkin(source))
		for (const std::string& rel : AnimationClipImporter::outputPaths(source, relDir))
			if (std::filesystem::exists(contentRoot / rel, ec))
				clipsBefore.push_back(rel);

	// Re-importing a static mesh whose source has since been rigged rewrites it as
	// a SkeletalMesh in place, under the same file and the same uuid. That is the
	// intended outcome — the alternative is a second asset again — but it does
	// change what every reference to it resolves to.
	if (!importSource(source, contentRoot, relDir, outputs))
		return false;

	if (classifySource(source) == SourceKind::Mesh && gltfHasSkin(source))
	{
		for (const std::string& rel : AnimationClipImporter::outputPaths(source, relDir))
		{
			if (std::find(clipsBefore.begin(), clipsBefore.end(), rel) != clipsBefore.end())
				continue;
			if (!std::filesystem::exists(contentRoot / rel, ec))
				continue;
			HE_LOG_WARN(Tool, "%s",
				("Importer: reimport of " + assetFile.filename().string()
				 + " created a new animation clip " + rel
				 + " — if you renamed that clip, the renamed one is now stale").c_str());
		}
	}
	return true;
}

// ─── Re-import bookkeeping ───────────────────────────────────────────────────

std::vector<std::string> meshSidecarAssets(const std::filesystem::path& meshAsset,
                                           const std::filesystem::path& contentRoot)
{
	std::vector<std::string> out;

	HAsset::Reader reader;
	if (!reader.open(meshAsset.string()))
		return out;

	// Only the two mesh asset types carry MREF, so every other asset type leaves
	// here with an empty list.
	const auto* mref = reader.findChunk(HAsset::CHUNK_MREF);
	if (!mref)
		return out;

	std::string materialPath;
	size_t      off = 0;
	if (!HAsset::Reader::readString(mref->data, off, materialPath) || materialPath.empty())
		return out;
	out.push_back(materialPath);

	// …and the textures that material references, written by the same import.
	HAsset::Reader matReader;
	if (!matReader.open((contentRoot / materialPath).string()))
		return out;  // gone — the caller already sees the missing material
	const auto* mtrl = matReader.findChunk(HAsset::CHUNK_MTRL);
	if (!mtrl)
		return out;

	std::string              shaderPath;    // read past it: never a file on disk
	std::vector<std::string> texturePaths;
	size_t                   moff = 0;
	if (!HAsset::Reader::readString(mtrl->data, moff, shaderPath)) return out;
	if (!HAsset::Reader::readVec(mtrl->data, moff, texturePaths))  return out;
	const auto add = [&out](const std::string& tex)
	{
		if (!tex.empty() && std::find(out.begin(), out.end(), tex) == out.end())
			out.push_back(tex);
	};
	for (const std::string& tex : texturePaths)
		add(tex);
	// …and the NODE-GRAPH textures, which is where an imported PBR material keeps its
	// normal / ORM / emissive maps — texturePaths above holds only the legacy heTex0
	// slot (the base colour). Without them, importOutputsUpToDate calls a mesh current
	// after its normal map was deleted, and the asset compiler never regenerates it.
	//
	// Reading them means walking the MTRL tail up to that field, so the offsets below
	// MIRROR ContentManager's Material branch (both the reader and buildMaterialChunk).
	// A field inserted there before graphTexturePaths has to be inserted here too — a
	// mismatch makes this read a different field, which the bounds-checked readers
	// answer with an empty list rather than a crash: sidecars silently stop being
	// tracked. Everything up to graphTexturePaths is skipped, not interpreted.
	float    skipF = 0.0f;
	uint32_t paramFloats = 0;
	std::string skipS;
	for (int k = 0; k < 6; ++k)                                      // baseColor rgb, metallic, roughness, opacity
		if (!HAsset::Reader::readPOD(mtrl->data, moff, skipF)) return out;
	if (!HAsset::Reader::readString(mtrl->data, moff, skipS)) return out;  // customShaderFragGlsl
	if (!HAsset::Reader::readString(mtrl->data, moff, skipS)) return out;  // nodeGraphJson
	if (!HAsset::Reader::readPOD(mtrl->data, moff, paramFloats)) return out;
	// Same bound ContentManager applies — a corrupt count must not walk the offset
	// past the buffer (the readers would then simply stop, but the cap keeps the
	// two sides reading the same bytes).
	if (paramFloats > 16 * 4) return out;
	for (uint32_t i = 0; i < paramFloats; ++i)
		if (!HAsset::Reader::readPOD(mtrl->data, moff, skipF)) return out;
	std::vector<std::string> graphTextures;
	if (!HAsset::Reader::readVec(mtrl->data, moff, graphTextures)) return out;
	for (const std::string& tex : graphTextures)
		add(tex);

	return out;
}

bool importOutputsUpToDate(const std::filesystem::path&    source,
                           const std::filesystem::path&    contentRoot,
                           const std::filesystem::path&    primaryOutput,
                           const std::vector<std::string>& extraOutputs)
{
	std::error_code ec;
	const auto srcTime = std::filesystem::last_write_time(source, ec);
	if (ec) return false;

	const auto current = [&srcTime](const std::filesystem::path& output)
	{
		std::error_code e;
		const auto outTime = std::filesystem::last_write_time(output, e);
		return !e && outTime >= srcTime;
	};

	if (!current(primaryOutput))
		return false;
	for (const std::string& rel : meshSidecarAssets(primaryOutput, contentRoot))
		if (!current(contentRoot / rel))
			return false;
	for (const std::string& rel : extraOutputs)
		if (!current(contentRoot / rel))
			return false;
	return true;
}

} // namespace Importer
