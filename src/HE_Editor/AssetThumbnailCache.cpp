#include "AssetThumbnailCache.h"
#include "EditorAssetTypeCache.h"      // path → AssetType (cached HAsset header sniff)
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <MaterialGraph/MaterialGraph.h> // material-FUNCTION tiles wrap the graph
#include <Renderer/IRenderer.h>
#include <Types/Enums.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace AssetThumbnailCache
{
namespace
{
	// Rendered edge length. The grid draws tiles at 60 logical px, so 128 keeps
	// them crisp on a 2× display without making the cache files large.
	constexpr uint32_t kThumbSize = 128;

	// Per-frame work caps. A render is a synchronous GPU round-trip plus loading
	// the asset — a handful per frame fills a folder in well under a second while
	// staying invisible in the frame time. Reading a .hthumb and uploading it is
	// far cheaper, so that budget is larger.
	constexpr int kRendersPerFrame = 2;
	constexpr int kUploadsPerFrame = 8;

	// How often an entry's source file is re-stat'ed for external changes. Every
	// frame would mean one stat per visible tile per frame for no benefit.
	constexpr double kStatIntervalSec = 1.5;

	enum class State { Unknown, Ready, Unsupported };

	struct Entry
	{
		void*    texture   = nullptr; // IRenderer texture handle (owned)
		uint64_t stamp     = 0;       // source (mtime, size) the texture was built from
		double   lastStat  = -1.0e9;  // when `stamp` was last verified against disk
		State    state     = State::Unknown;
	};

	IRenderer*      s_renderer = nullptr;
	ContentManager* s_content  = nullptr;
	std::string     s_cacheDir;
	std::unordered_map<std::string, Entry> s_entries;
	double s_now           = 0.0;
	int    s_rendersLeft   = 0;
	int    s_uploadsLeft   = 0;
	// Textures dropped during a frame that has already DRAWN them. Freeing one on
	// the spot is a use-after-free: the tile is in this frame's ImGui draw list as
	// a raw handle, and the backend only dereferences it when the frame is
	// submitted — after the Content Browser's rename/delete/save handlers have run.
	// Freed at the next beginFrame instead, by which time that draw data is gone.
	std::vector<void*> s_pendingDestroy;

	void releaseTexture(Entry& e)
	{
		if (e.texture) s_pendingDestroy.push_back(e.texture);
		e.texture = nullptr;
	}

	void drainPendingDestroy()
	{
		if (s_renderer)
			for (void* t : s_pendingDestroy) s_renderer->DestroyImGuiTexture(t);
		s_pendingDestroy.clear();
	}

	// The asset types a thumbnail can be rendered for. Anything else keeps its
	// per-type icon. MaterialFunction maps onto the Material kind because that is
	// how it is drawn — see makeMaterialFunctionScratch.
	bool thumbnailKindOf(const std::string& absPath, ThumbnailKind& out)
	{
		switch (EditorAssetTypeCache::assetTypeOf(absPath))
		{
			case HE::AssetType::Material:         out = ThumbnailKind::Material;     return true;
			case HE::AssetType::MaterialFunction: out = ThumbnailKind::Material;     return true;
			case HE::AssetType::StaticMesh:       out = ThumbnailKind::StaticMesh;   return true;
			case HE::AssetType::SkeletalMesh:     out = ThumbnailKind::SkeletalMesh; return true;
			// Textures never reach the renderer — makeTextureThumbnail draws them on
			// the CPU from the asset's own pixels. The kind is unused for them.
			case HE::AssetType::Texture:          out = ThumbnailKind::Material;     return true;
			default: return false;
		}
	}

	// ── Material functions ───────────────────────────────────────────────────
	// A function is a reusable SUB-graph: it has no surface of its own, so there
	// is nothing to hand the renderer directly. The Material Editor already
	// solves this for its own preview by wrapping the function in a throwaway
	// material — Output ← FunctionCall, lit — and rendering that; a tile does the
	// same so the grid and the function's editor tab show the same picture.
	//
	// One scratch material is reused for every function. Tiles are rendered one
	// at a time and then cached, so a second live one never exists, and the
	// renderer keys its program on the SOURCE hash — a rewritten scratch
	// naturally lands on a different program without any cache juggling.
	HE::UUID s_fnScratchMaterial{};

	// Non-caching function-graph loader for codegen to resolve nested
	// FunctionCall nodes. Deliberately not the Material Editor's cached one:
	// that cache belongs to the open tabs and is invalidated on save, whereas a
	// tile is generated once and then lives on disk. The map only exists because
	// MatFunctionLoader hands back a POINTER, which has to stay alive.
	const HE::MaterialGraph* loadFunctionGraph(const std::string& relPath)
	{
		static std::map<std::string, HE::MaterialGraph> graphs;
		if (relPath.empty() || !s_content) return nullptr;
		if (auto it = graphs.find(relPath); it != graphs.end()) return &it->second;
		const HE::UUID id = s_content->loadAsset(relPath);
		const MaterialFunctionAsset* fn = s_content->getMaterialFunction(id);
		if (!fn) return nullptr;
		HE::MaterialGraph g;
		if (!fn->nodeGraphJson.empty() && !HE::materialGraphFromJson(fn->nodeGraphJson, g))
			return nullptr;
		return &(graphs[relPath] = std::move(g));
	}

	// The scratch material that renders material function `relPath`, or a null
	// UUID when the function has no output pin (nothing to show).
	HE::UUID makeMaterialFunctionScratch(const std::string& relPath)
	{
		if (!s_content) return {};
		const HE::MaterialGraph* fnGraph = loadFunctionGraph(relPath);
		if (!fnGraph) return {};
		bool hasOutput = false;
		for (const auto& n : fnGraph->nodes)
			if (n.type == HE::MatNodeType::FnOutput) { hasOutput = true; break; }
		if (!hasOutput) return {};

		if (s_fnScratchMaterial == HE::UUID{})
		{
			MaterialAsset scratch;
			scratch.type = HE::AssetType::Material;
			scratch.name = "__thumbnailFunctionPreview";
			s_fnScratchMaterial = s_content->registerMaterial(std::move(scratch));
		}
		MaterialAsset* sm = s_content->getMaterialMutable(s_fnScratchMaterial);
		if (!sm) return {};

		HE::MaterialGraph pg;
		const int out = pg.addNode(HE::MatNodeType::Output);
		if (HE::MatGraphNode* n = pg.findNode(out)) n->p[0] = 1.0f;  // lit — read it as a surface
		const int call = pg.addNode(HE::MatNodeType::FunctionCall);
		if (HE::MatGraphNode* n = pg.findNode(call)) n->s = relPath;
		pg.connect(call, 0, out, HE::kMatOutputBaseColorPin); // first FnOutput → BaseColor

		const HE::MatShaderGen gen = HE::generateFragment(pg, loadFunctionGraph);
		if (gen.glsl.empty()) return {};
		sm->customShaderFragGlsl = gen.glsl;
		sm->customShaderVertGlsl = gen.vertexBody;
		sm->shaderParamData.clear();
		for (const auto& slot : gen.params)
			sm->shaderParamData.insert(sm->shaderParamData.end(), slot.value, slot.value + 4);
		sm->graphTexturePaths = gen.textures;
		sm->graphTextureIds.clear();   // loose editor assets resolve by path
		if (s_renderer) s_renderer->InvalidateMaterial(s_fnScratchMaterial);
		return s_fnScratchMaterial;
	}

	// ── Textures ─────────────────────────────────────────────────────────────
	// A texture is its own best thumbnail, and producing one needs no GPU at all:
	// the asset already holds the pixels. Done on the CPU rather than through the
	// renderer so it costs no synchronous round-trip and works on every backend.
	//
	// Cooked (BCn) textures are the one gap — their bytes are block-compressed and
	// only the backends can decode them, so those keep the texture glyph. Editor
	// content is RGBA8 (that is what the importer writes); cooking happens at pack
	// time, so this covers everything the Content Browser normally shows.
	bool makeTextureThumbnail(const HE::UUID& id, std::vector<uint8_t>& out)
	{
		const TextureAsset* tex = s_content->getTexture(id);
		if (!tex || tex->format != TextureFormat::RGBA8) return false;
		const uint32_t w = tex->width, h = tex->height, ch = tex->channels;
		if (w == 0 || h == 0 || (ch != 1 && ch != 3 && ch != 4)) return false;
		if (tex->data.size() < static_cast<size_t>(w) * h * ch) return false;

		const int S = static_cast<int>(kThumbSize);
		out.assign(static_cast<size_t>(S) * S * 4, 0);

		// Letterbox rather than stretch — a 4:1 trim sheet squashed into a square
		// tile is unrecognisable, and the tile has a transparent background to
		// letterbox into anyway.
		const float scale = std::min(static_cast<float>(S) / w, static_cast<float>(S) / h);
		const int dw = std::max(1, static_cast<int>(w * scale));
		const int dh = std::max(1, static_cast<int>(h * scale));
		const int ox = (S - dw) / 2, oy = (S - dh) / 2;

		// Box filter, but with a CAPPED sample count per destination pixel: a 4K
		// source averaged in full would be 16M reads inside the UI frame. 4x4
		// evenly spread samples bound the cost at ~260k regardless of source size
		// and still look like a proper downscale.
		constexpr int kMaxTaps = 4;
		for (int y = 0; y < dh; ++y)
		{
			const uint32_t sy0 = static_cast<uint32_t>((static_cast<float>(y)     / dh) * h);
			const uint32_t sy1 = std::max(sy0 + 1, static_cast<uint32_t>((static_cast<float>(y + 1) / dh) * h));
			for (int x = 0; x < dw; ++x)
			{
				const uint32_t sx0 = static_cast<uint32_t>((static_cast<float>(x)     / dw) * w);
				const uint32_t sx1 = std::max(sx0 + 1, static_cast<uint32_t>((static_cast<float>(x + 1) / dw) * w));
				const uint32_t stepX = std::max(1u, (sx1 - sx0) / kMaxTaps);
				const uint32_t stepY = std::max(1u, (sy1 - sy0) / kMaxTaps);

				uint32_t acc[4] = { 0, 0, 0, 0 };
				uint32_t taps = 0;
				for (uint32_t sy = sy0; sy < sy1 && sy < h; sy += stepY)
					for (uint32_t sx = sx0; sx < sx1 && sx < w; sx += stepX)
					{
						const uint8_t* p = &tex->data[(static_cast<size_t>(sy) * w + sx) * ch];
						if (ch == 1)      { acc[0] += p[0]; acc[1] += p[0]; acc[2] += p[0]; acc[3] += 255; }
						else if (ch == 3) { acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += 255; }
						else              { acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3]; }
						++taps;
					}
				if (taps == 0) continue;

				// Composite over a checkerboard: a texture with alpha drawn straight
				// onto the dark tile background reads as "half the image is missing".
				const float a = static_cast<float>(acc[3] / taps) / 255.0f;
				const uint8_t bg = (((x + ox) / 16 + (y + oy) / 16) & 1) ? 90 : 130;
				uint8_t* dst = &out[(static_cast<size_t>(y + oy) * S + (x + ox)) * 4];
				for (int c = 0; c < 3; ++c)
					dst[c] = static_cast<uint8_t>((acc[c] / taps) * a + bg * (1.0f - a));
				dst[3] = 255;
			}
		}
		return true;
	}

	// Content-relative path (or the absolute one when the asset lives under
	// neither root) — the same key the ContentManager addresses assets by, and
	// what the cache file records so a name-hash collision can be detected.
	std::string relPathOf(const std::string& absPath)
	{
		if (!s_content) return absPath;
		const std::string rel = s_content->toContentRelativePath(absPath);
		return rel.empty() ? absPath : rel;
	}
}

HE::UUID materialFunctionScratch(const std::string& relPath)
{
	return makeMaterialFunctionScratch(relPath);
}

bool textureThumbnail(const HE::UUID& textureId, std::vector<uint8_t>& out)
{
	return s_content && makeTextureThumbnail(textureId, out);
}

uint32_t thumbnailSize() { return kThumbSize; }

void setContext(IRenderer* renderer, ContentManager* cm, const std::string& cacheDir)
{
	if (renderer == s_renderer && cm == s_content && cacheDir == s_cacheDir) return;
	// A different renderer or project means every cached texture belongs to a
	// world that is going away. Retire them against the renderer that made them —
	// still the current one at this point, so the deferred queue must be flushed
	// BEFORE the swap or it would free them against the new one.
	clear();
	drainPendingDestroy();
	s_renderer = renderer;
	s_content  = cm;
	s_cacheDir = cacheDir;
}

void beginFrame(double now)
{
	drainPendingDestroy(); // last frame's draw data is submitted — safe now
	s_now         = now;
	s_rendersLeft = kRendersPerFrame;
	s_uploadsLeft = kUploadsPerFrame;
}

void* get(const std::string& absPath)
{
	if (absPath.empty() || !s_renderer || !s_content) return nullptr;

	// Type first, BEFORE an entry exists: the grid asks for every file it draws,
	// and scripts/textures/scenes must not each cost a map entry and a periodic
	// stat. The answer comes from the shared header-sniff cache, which the editor
	// already drops whenever the content tree changes — so an asset appearing at a
	// path that used to hold something else is picked up there, not here.
	ThumbnailKind kind;
	if (!thumbnailKindOf(absPath, kind)) return nullptr;

	Entry& e = s_entries[absPath];

	// ── Staleness: re-stat at most every kStatIntervalSec. A changed stamp drops
	// the texture and re-renders — that is what makes an edited material or a
	// re-imported mesh update its tile without any explicit invalidation.
	if (s_now - e.lastStat >= kStatIntervalSec)
	{
		e.lastStat = s_now;
		const uint64_t stamp = sourceStampOf(absPath);
		if (stamp != e.stamp)
		{
			releaseTexture(e);
			e.stamp = stamp;
			e.state = State::Unknown;
		}
	}

	if (e.state == State::Ready)       return e.texture;
	if (e.state == State::Unsupported) return nullptr;
	if (e.stamp == 0) { e.state = State::Unsupported; return nullptr; } // file is gone

	const std::string relPath = relPathOf(absPath);
	const std::string file    = s_cacheDir.empty()
		? std::string{} : (fs::path(s_cacheDir) / fileNameFor(relPath)).string();

	// ── Level 1: the on-disk cache. Only the upload budget applies.
	if (!file.empty() && s_uploadsLeft > 0)
	{
		uint32_t size = 0;
		std::vector<uint8_t> pixels;
		if (readFile(file, relPath, e.stamp, size, pixels))
		{
			--s_uploadsLeft;
			e.texture = s_renderer->CreateImGuiTexture(pixels.data(), (int)size, (int)size);
			e.state   = e.texture ? State::Ready : State::Unsupported;
			return e.texture;
		}
	}

	// ── Level 2: render it. Budgeted — an unbudgeted frame simply retries later,
	// with the tile showing its icon fallback in the meantime.
	if (s_rendersLeft <= 0) return nullptr;

	// A material FUNCTION is rendered through a scratch material standing in for
	// it; everything else is drawn as itself.
	const HE::AssetType type = EditorAssetTypeCache::assetTypeOf(absPath);
	const bool isFunction = type == HE::AssetType::MaterialFunction;
	// Whether the asset was ALREADY in memory decides if we may drop it again
	// below: generating a tile must not evict something the scene is using, but it
	// also must not permanently resident every 4K texture in a browsed folder.
	const bool wasLoaded = s_content->isLoaded(relPath);
	const HE::UUID id = isFunction ? makeMaterialFunctionScratch(relPath)
	                               : s_content->loadAsset(relPath);
	if (id == HE::UUID{}) { e.state = State::Unsupported; return nullptr; }

	std::vector<uint8_t> pixels;
	--s_rendersLeft;
	const bool produced = (type == HE::AssetType::Texture)
		? makeTextureThumbnail(id, pixels)
		: s_renderer->RenderAssetThumbnail(*s_content, kind, id, kThumbSize, pixels);

	// The asset was pulled into memory only to draw a 128px tile — let it go
	// again. The renderer keeps its own GPU copy of a mesh it just uploaded, so
	// this costs nothing but the CPU-side bytes.
	if (!wasLoaded && !isFunction) s_content->unloadAsset(id);

	if (!produced)
	{
		// No thumbnail path on this backend, or the asset would not resolve. Mark
		// it Unsupported so it is not retried every frame; a later edit to the
		// asset (new stamp) clears that again.
		e.state = State::Unsupported;
		return nullptr;
	}

	if (!file.empty())
	{
		std::error_code ec;
		fs::create_directories(s_cacheDir, ec);
		writeFile(file, relPath, e.stamp, kThumbSize, pixels); // best effort
	}
	e.texture = s_renderer->CreateImGuiTexture(pixels.data(), (int)kThumbSize, (int)kThumbSize);
	e.state   = e.texture ? State::Ready : State::Unsupported;
	return e.texture;
}

void invalidate(const std::string& absPath)
{
	const auto it = s_entries.find(absPath);
	if (it != s_entries.end())
	{
		releaseTexture(it->second);
		s_entries.erase(it);
	}
	// Drop the file too: an asset that was renamed or deleted would otherwise
	// leave its .hthumb behind forever, and a rewritten one is re-rendered anyway.
	if (!s_cacheDir.empty())
	{
		std::error_code ec;
		fs::remove(fs::path(s_cacheDir) / fileNameFor(relPathOf(absPath)), ec);
	}
}

void clear()
{
	for (auto& [path, e] : s_entries) releaseTexture(e);
	s_entries.clear();
}

void shutdown()
{
	// No frame is in flight here (the editor tears ImGui down first), so the
	// deferred queue is drained straight away rather than waiting for a beginFrame
	// that will never come.
	clear();
	drainPendingDestroy();
	s_renderer = nullptr;
	s_content  = nullptr;
	s_cacheDir.clear();
}

std::string fileNameFor(const std::string& relPath)
{
	// FNV-1a over the path. Not a security hash — the stored path inside the file
	// is what actually decides whether a hit belongs to this asset.
	uint64_t h = 1469598103934665603ull;
	for (const unsigned char c : relPath) { h ^= c; h *= 1099511628211ull; }
	char buf[17];
	std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
	return std::string(buf) + ".hthumb";
}

uint64_t sourceStampOf(const std::string& absPath)
{
	std::error_code ec;
	const auto mtime = fs::last_write_time(absPath, ec);
	if (ec) return 0;
	const auto bytes = fs::file_size(absPath, ec);
	if (ec) return 0;
	const uint64_t ticks =
		static_cast<uint64_t>(mtime.time_since_epoch().count());
	// Mix rather than concatenate: the file size alone would collide constantly
	// and the tick count's low bits carry the resolution that matters.
	uint64_t stamp = ticks * 1099511628211ull ^ static_cast<uint64_t>(bytes);
	return stamp == 0 ? 1 : stamp; // 0 is reserved for "no such file"
}

bool writeFile(const std::string& file, const std::string& relPath, uint64_t sourceStamp,
               uint32_t size, const std::vector<uint8_t>& pixels)
{
	if (size == 0 || pixels.size() != static_cast<size_t>(size) * size * 4) return false;

	FileHeader hdr{};
	std::memcpy(hdr.magic, "HTHB", 4);
	hdr.version     = kVersion;
	hdr.size        = size;
	hdr.pathLen     = static_cast<uint32_t>(relPath.size());
	hdr.sourceStamp = sourceStamp;
	hdr.reserved    = 0;

	// Write to a sibling temp file and rename: a cache file half-written by a
	// crashed or killed editor would otherwise be read back as a valid header
	// with a truncated pixel block on the next launch.
	const std::string tmp = file + ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f) return false;
		f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
		f.write(relPath.data(), static_cast<std::streamsize>(relPath.size()));
		f.write(reinterpret_cast<const char*>(pixels.data()),
		        static_cast<std::streamsize>(pixels.size()));
		if (!f) { f.close(); std::error_code rc; fs::remove(tmp, rc); return false; }
	}
	std::error_code ec;
	fs::rename(tmp, file, ec);
	if (ec) { std::error_code rc; fs::remove(tmp, rc); return false; }
	return true;
}

bool readFile(const std::string& file, const std::string& relPath, uint64_t sourceStamp,
              uint32_t& sizeOut, std::vector<uint8_t>& pixelsOut)
{
	std::ifstream f(file, std::ios::binary);
	if (!f) return false;

	FileHeader hdr{};
	f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
	if (!f || std::memcmp(hdr.magic, "HTHB", 4) != 0) return false;
	if (hdr.version != kVersion)         return false;
	if (hdr.sourceStamp != sourceStamp)  return false; // asset changed → re-render
	// Bound the size before it is squared into an allocation.
	if (hdr.size == 0 || hdr.size > 1024) return false;
	if (hdr.pathLen != relPath.size())    return false;

	std::string storedPath(hdr.pathLen, '\0');
	if (hdr.pathLen > 0)
	{
		f.read(storedPath.data(), static_cast<std::streamsize>(hdr.pathLen));
		if (!f || storedPath != relPath) return false; // name-hash collision
	}

	const size_t bytes = static_cast<size_t>(hdr.size) * hdr.size * 4;
	pixelsOut.resize(bytes);
	f.read(reinterpret_cast<char*>(pixelsOut.data()), static_cast<std::streamsize>(bytes));
	if (static_cast<size_t>(f.gcount()) != bytes) return false; // truncated
	sizeOut = hdr.size;
	return true;
}

} // namespace AssetThumbnailCache
