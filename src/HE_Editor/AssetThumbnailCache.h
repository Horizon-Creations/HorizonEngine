#pragma once
#include <Types/UUID.h>
#include <cstdint>
#include <string>
#include <vector>

class IRenderer;
class ContentManager;

// ── Content-Browser asset thumbnails ─────────────────────────────────────────
// Meshes and materials show a rendered preview image in the asset grid instead
// of a generic per-extension icon. Producing one means an offscreen GPU render
// (IRenderer::RenderAssetThumbnail) plus loading the asset, so it happens at
// most ONCE per asset version and is cached at two levels:
//
//   • on disk, one <hash>.hthumb per asset under the project's
//     Saved/Thumbnails — survives editor restarts, so a project only pays the
//     render cost the first time an asset is seen;
//   • in memory, as an uploaded ImGui texture, so drawing a tile is a plain
//     ImGui::Image with no per-frame render.
//
// Staleness is the source .hasset's (mtime, size): saving a material, re-
// importing a mesh or editing a file outside the editor all change it, and the
// next re-stat re-renders the tile. Explicit invalidate() calls just make that
// instant rather than waiting for the throttled poll.
//
// Rendering is budgeted per frame (see kRendersPerFrame in the .cpp): opening a
// folder with 300 assets must not stall on 300 synchronous GPU readbacks, so
// tiles fill in over the next handful of frames and unfinished ones fall back to
// their icon. UI-thread only, like the rest of the editor's caches.
namespace AssetThumbnailCache
{
	// Point the cache at the loaded project. `cacheDir` is where .hthumb files
	// live (the caller owns the layout — normally <projectRoot>/Saved/Thumbnails).
	// Changing the renderer or the directory drops every in-memory texture, so
	// this is also the project-switch hook. Cheap to call every frame with
	// unchanged arguments.
	void setContext(IRenderer* renderer, ContentManager* cm, const std::string& cacheDir);

	// Refill the per-frame render budget. Call once per editor frame, before any
	// get(). `now` is a monotonically rising seconds clock (ImGui::GetTime()) and
	// drives the throttled staleness re-stat.
	void beginFrame(double now);

	// The thumbnail for the asset at `absPath`, ready to hand to ImGui::Image, or
	// nullptr when there is none: an asset type without a preview, a render that
	// failed, or a tile that has not been produced yet (the caller draws its icon
	// fallback and asks again next frame).
	void* get(const std::string& absPath);

	// Drop `absPath`'s thumbnail — both the texture and the .hthumb file — so the
	// next get() re-renders it. Call wherever an asset is written, renamed or
	// deleted; the throttled staleness check would catch a write on its own, this
	// just makes the update immediate.
	void invalidate(const std::string& absPath);

	// Drop every in-memory texture (the disk cache stays: it is keyed by content
	// stamp, so it stays valid). For project switches and editor shutdown.
	void clear();

	// Release GPU textures while the renderer is still alive. Call from the
	// editor's shutdown before the renderer is destroyed.
	void shutdown();

	// A material FUNCTION has no surface of its own — it is a reusable sub-graph.
	// Its tile is rendered through a scratch material that wraps it (Output ←
	// FunctionCall, lit), the same construction the Material Editor uses to
	// preview a function in its own tab, so the grid and the tab agree. get()
	// calls this internally; it is public because it is the interesting half of
	// the material-function path and is worth testing without a GPU.
	//
	// `relPath` is content-relative. Returns a null UUID when the function has no
	// output pin (nothing to show), when it cannot be loaded, or when no
	// ContentManager is set. The returned material is REUSED by the next call —
	// render before asking for another.
	HE::UUID materialFunctionScratch(const std::string& relPath);

	// A texture shows ITSELF rather than a glyph, drawn on the CPU from the
	// asset's own pixels — no GPU round-trip, works on every backend. Aspect is
	// letterboxed (a 4:1 trim sheet squashed square is unrecognisable) and alpha
	// is composited over a checkerboard (otherwise a transparent texture reads as
	// half-missing on the dark tile). Fills `out` with kThumbSize² RGBA8.
	//
	// False for a cooked (BCn) texture: those bytes are block-compressed and only
	// the backends can decode them, so they keep the texture glyph. Editor content
	// is RGBA8 — cooking happens at pack time. Public for tests.
	bool textureThumbnail(const HE::UUID& textureId, std::vector<uint8_t>& out);

	// Edge length of a generated tile, in pixels.
	uint32_t thumbnailSize();

	// A prefab is a CBOR entity subtree, not a shape — its tile is the first mesh
	// found inside it, which is exactly right for the common case (a prop) and
	// shows the primary part rather than nothing for an assembly. Resolves the
	// blob by instantiating it into a throwaway world, so no second CBOR reader
	// has to exist. False when the prefab holds no mesh at all (pure logic or
	// audio), where the glyph is the honest answer. Public for tests.
	bool prefabMesh(const HE::UUID& prefabId, HE::UUID& meshIdOut, bool& isSkeletalOut);

	// A font's tile is a sample set IN THAT FONT — the one preview that answers
	// what you actually want to know about a face, which no glyph could. Baked
	// through the engine's own UIFontCache, so a font that renders in-game renders
	// here identically. Fills `out` with kThumbSize² RGBA8 (white coverage on
	// transparent). Public for tests.
	bool fontThumbnail(const HE::UUID& fontId, std::vector<uint8_t>& out);

	// A particle asset is an emitter GRAPH, so its tile is a snapshot of the
	// effect actually running: the pool is stepped here (ParticleSystem::stepPool
	// — the renderer never simulates) with a FIXED seed, because a cached tile
	// must not change every time it is regenerated. Public for tests.
	bool particleThumbnail(const HE::UUID& particleId, std::vector<uint8_t>& out);

	// ── Cache-file format (exposed for tests) ────────────────────────────────
	// A .hthumb is a 32-byte header, the asset's content-relative path, then
	// `size`×`size` top-down RGBA8 pixels. The path is stored so a hash collision
	// on the file NAME can never surface another asset's picture: a mismatch is
	// treated as a miss and overwritten.
	struct FileHeader
	{
		char     magic[4];      // "HTHB"
		uint32_t version;       // kVersion
		uint32_t size;          // edge length in pixels
		uint32_t pathLen;       // bytes of relative path following this header
		uint64_t sourceStamp;   // source asset's (mtime, size) fingerprint
		uint64_t reserved;      // 0
	};
	static_assert(sizeof(FileHeader) == 32, "on-disk header must stay 32 bytes");
	constexpr uint32_t kVersion = 1;

	// Write/read one cache file. `pixels` must hold size*size*4 bytes. readFile
	// fails (returns false) when the file is missing, malformed, written by
	// another version, for a different path, or no longer matches `sourceStamp` —
	// i.e. every case where the caller has to re-render.
	bool writeFile(const std::string& file, const std::string& relPath, uint64_t sourceStamp,
	               uint32_t size, const std::vector<uint8_t>& pixels);
	bool readFile(const std::string& file, const std::string& relPath, uint64_t sourceStamp,
	              uint32_t& sizeOut, std::vector<uint8_t>& pixelsOut);

	// The .hthumb file name for a content-relative asset path (hash + extension,
	// no directory). Stable across runs and platforms.
	std::string fileNameFor(const std::string& relPath);

	// (mtime, size) fingerprint of `absPath`; 0 when the file does not exist.
	// Any write to the asset changes it, which is what drives re-rendering.
	uint64_t sourceStampOf(const std::string& absPath);
}
