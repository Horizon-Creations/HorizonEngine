#pragma once
#include "EditorUI.h"
#include <imgui.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct TextureAsset;

// Texture viewer — a top-level tab opened by double-clicking a Texture .hasset
// (or a raw .png/.jpg/… source) in the Content Browser, and opened on its own
// right after a single image was imported. Before it existed a double-click on
// a texture did nothing, and the only picture of one anywhere in the editor was
// its 128 px Content Browser tile (Thema 92, Schritt 4).
//
// It shows the image at full resolution, zoomable and pannable, with the
// checkerboard only BEHIND it (so transparency reads as transparency and the
// picture itself stays untouched), the R/G/B/A channels on their own, the pixel
// under the pointer, and what the asset stores: size, format, sRGB, mips, bytes.
//
// Orientation. The importer stores rows bottom-up (TextureImporter flips on
// load, which the mesh UV convention depends on), so row 0 of the asset is the
// BOTTOM of the picture. toDisplayRgba() is the one place that turns that back
// into a top-down image; a raw source is decoded with the importer's own
// default settings and goes through the same function, so the preview of a
// file not yet imported is exactly what the import will then store.
//
// Raw sources are openable for the same reason the audio tab opens a raw .wav:
// this is where you look at an image before deciding to import it. The tab
// offers the Import button, and after the import it turns into the tab of the
// new asset (see takeOpenRequest).
namespace TextureViewerPanel
{
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// A Texture .hasset (header sniff via EditorAssetTypeCache) or a raw image
	// source the importer reads (isImageSource).
	bool isTextureAsset(const std::string& path);

	// The extensions the importer routes to TextureImporter — asked of
	// Importer::sourceFamilyPattern, so the two lists cannot drift.
	bool isImageSource(const std::string& path);

	// Drop cached state for `path` (tab close, rename/delete). Its GPU texture is
	// freed at the next beginFrame, not now: the tab may have drawn it this frame.
	void forget(const std::string& assetPath);

	// Once per frame, before any tab is drawn: frees the textures forget() and a
	// channel switch retired in the frame before (by now their draw data is gone).
	void beginFrame();

	// ── Opening the tab from elsewhere ───────────────────────────────────────
	// Same find-or-push flow as MaterialEditorPanel::takeOpenRequest. `replacing`
	// names a tab that should BECOME this one instead of a second tab opening
	// beside it — the raw-source tab after its Import button ran. EditorUI drains
	// it at the top of the frame, where no panel holds a reference into ctx.tabs.
	struct OpenRequest
	{
		std::string path;
		std::string replacing;
	};
	void        requestOpen(const std::string& absPath, const std::string& replacing = {});
	OpenRequest takeOpenRequest();

	// Import one image source into <root>/<relDir> through Importer::importSource
	// and return the absolute path of the .hasset it wrote, empty on failure.
	// The path is the importer's own (Importer::resolveOutput), so the tab opened
	// on it is the asset that was actually written.
	std::string importImage(const std::filesystem::path& source,
	                        const std::filesystem::path& root,
	                        const std::filesystem::path& relDir);

	// ── Pure pixel work (no ImGui, no GPU — tests/test_texture_viewer.cpp) ──
	enum ChannelMask : unsigned
	{
		kChannelR = 1u, kChannelG = 2u, kChannelB = 4u, kChannelA = 8u,
		kChannelAll = kChannelR | kChannelG | kChannelB | kChannelA,
	};

	// Level 0 of `tex` as straight RGBA8, rows TOP-DOWN (row 0 = top of the
	// picture), 1- and 3-channel data widened. False for data this cannot read:
	// block-compressed (cooked) formats, a size that does not match the bytes.
	bool toDisplayRgba(const TextureAsset& tex, std::vector<uint8_t>& out);

	// `src` (RGBA8) filtered to the channels in `mask`. One colour channel alone
	// is shown as grey — a mask or a roughness map reads as brightness, not as
	// "how red". Alpha alone is grey too. Alpha switched off means opaque.
	void applyChannelMask(const std::vector<uint8_t>& src, unsigned mask,
	                      std::vector<uint8_t>& out);

	// ── Test hook ────────────────────────────────────────────────────────────
	// Where the viewer's GPU texture comes from. Unset (the editor) it is
	// ctx.renderer->CreateImGuiTexture / DestroyImGuiTexture; the headless shot
	// installs the software rasterizer's registry instead. Pass empty functions
	// to go back to the renderer.
	using TextureUpload  = std::function<void*(const void* rgba8, int width, int height)>;
	using TextureRelease = std::function<void(void*)>;
	void setTextureHooks(TextureUpload upload, TextureRelease release);
}
