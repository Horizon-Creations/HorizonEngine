#include "TextureViewerPanel.h"
#include "EditorApplication.h"    // AppContext
#include "EditorAssetTypeCache.h" // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h"     // shared per-tab state map
#include "EditorHelp.h"           // "Texture Viewer/<label>" scope for the tooltips
#include "EditorWidgets.h"        // button, checkbox, WrapText
#include "ImporterCommon.h"       // Importer::importSource / resolveOutput / sourceFamilyPattern
#include "TextureImporter.h"      // raw-source decode with the importer's own settings
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Diagnostics/Logger.h>
#include <Renderer/IRenderer.h>
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>

namespace TextureViewerPanel
{

namespace
{

struct State
{
	bool        loaded    = false;
	bool        isRawFile = false;
	std::string name;
	std::string relPath;
	std::string error;         // why there is no picture, when there is none
	std::string sourceFile;    // the file an asset was imported from, if it says

	// What the asset stores — copied out at load time. A TextureAsset* would be a
	// pointer into the ContentManager's dense storage, which the next loadAsset
	// anywhere in the editor is free to move.
	uint32_t      width = 0, height = 0, channels = 0, mipLevels = 1;
	TextureFormat format = TextureFormat::RGBA8;
	bool          srgb   = false;
	size_t        bytes  = 0;

	std::vector<uint8_t> pixels;   // level 0, RGBA8, top-down: the readout source

	unsigned channelMask   = kChannelAll;
	unsigned uploadedMask  = 0;     // what `texture` currently shows (0 = nothing)
	void*    texture       = nullptr;
	bool     uploadFailed  = false;
	bool     checker       = true;

	// View: `fit` recomputes the zoom from the canvas every frame; any wheel or
	// 1:1 leaves it. `zoom` is screen pixels per texel, `pan` the image centre's
	// offset from the canvas centre.
	bool   fit  = true;
	float  zoom = 1.0f;
	ImVec2 pan{ 0.0f, 0.0f };

	// Re-import (from the Content Browser, or another tab) rewrites the file under
	// an open tab. The stamp is re-read about once a second and a change reloads.
	std::filesystem::file_time_type stamp{};
	double                          lastStat = 0.0;
};

AssetPanelState<State> s_states;

TextureUpload  s_upload;
TextureRelease s_release;
IRenderer*     s_renderer = nullptr;       // the last one a render saw, for beginFrame
std::vector<void*> s_pendingRelease;

std::vector<OpenRequest> s_openRequests;

void retire(State& st)
{
	if (st.texture) s_pendingRelease.push_back(st.texture);
	st.texture      = nullptr;
	st.uploadedMask = 0;
}

void* upload(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h)
{
	if (s_upload) return s_upload(rgba.data(), static_cast<int>(w), static_cast<int>(h));
	return s_renderer ? s_renderer->CreateImGuiTexture(rgba.data(), static_cast<int>(w),
	                                                   static_cast<int>(h))
	                  : nullptr;
}

std::string lowerExtension(const std::string& path)
{
	std::string ext = std::filesystem::path(path).extension().string();
	if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
	std::transform(ext.begin(), ext.end(), ext.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return ext;
}

std::filesystem::file_time_type stampOf(const std::string& path)
{
	std::error_code ec;
	const auto t = std::filesystem::last_write_time(path, ec);
	return ec ? std::filesystem::file_time_type{} : t;
}

const char* formatName(TextureFormat f)
{
	switch (f)
	{
		case TextureFormat::RGBA8:    return "RGBA8";
		case TextureFormat::ASTC_4x4: return "ASTC 4x4";
		case TextureFormat::BC7:      return "BC7";
		case TextureFormat::BC3:      return "BC3";
	}
	return "unknown";
}

void formatBytes(size_t bytes, char* buf, size_t n)
{
	const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
	if (mb >= 1.0) std::snprintf(buf, n, "%.1f MB", mb);
	else           std::snprintf(buf, n, "%.1f KB", static_cast<double>(bytes) / 1024.0);
}

// Everything below the tab reads from here, so the two sources — an asset and a
// raw file — only differ in how they arrive at a TextureAsset.
void takeFrom(State& st, const TextureAsset& tex)
{
	st.width     = tex.width;
	st.height    = tex.height;
	st.channels  = tex.channels;
	st.mipLevels = tex.mipLevels;
	st.format    = tex.format;
	st.srgb      = tex.srgb;
	st.bytes     = tex.data.size();
	if (!toDisplayRgba(tex, st.pixels))
		st.error = textureFormatIsBlock4x4(tex.format)
			? "This texture is stored block-compressed (" + std::string(formatName(tex.format)) +
			  "), which only the GPU decodes. Editor content is RGBA8; compressed data comes "
			  "from a packaged build."
			: "The texture's pixel data does not match its size, so there is nothing to show.";
}

void load(AppContext& ctx, const std::string& assetPath, State& st)
{
	st = State{ .checker = st.checker, .fit = st.fit, .zoom = st.zoom, .pan = st.pan };
	st.name      = std::filesystem::path(assetPath).filename().string();
	st.isRawFile = isImageSource(assetPath);
	st.stamp     = stampOf(assetPath);
	st.relPath   = ctx.contentManager ? ctx.contentManager->toContentRelativePath(assetPath)
	                                  : std::string{};
	if (st.relPath.empty()) st.relPath = assetPath;

	if (st.isRawFile)
	{
		// Decoded here, never registered: a source file is not an asset. The
		// importer's DEFAULT settings on purpose — that is what Import will run,
		// so what this shows is what the asset will hold.
		std::ifstream in(assetPath, std::ios::binary);
		const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
		                                 std::istreambuf_iterator<char>());
		std::unique_ptr<TextureAsset> tex = bytes.empty()
			? nullptr : TextureImporter::decodeFromMemory(bytes.data(), bytes.size());
		if (!tex) st.error = "The image could not be decoded.";
		else      takeFrom(st, *tex);
		st.loaded = true;
		return;
	}

	// No ContentManager yet (project still loading): stay unloaded, retry next frame.
	if (!ctx.contentManager) return;
	ContentManager& cm = *ctx.contentManager;

	// Pulled in to copy the pixels and let go again, unless something else had
	// it resident already — the same courtesy the thumbnail cache extends.
	const bool     wasLoaded = cm.isLoaded(st.relPath);
	const HE::UUID id        = cm.loadAsset(st.relPath);
	if (const TextureAsset* tex = cm.getTexture(id)) takeFrom(st, *tex);
	else st.error = "This file is not a texture asset the content manager can load.";
	if (id != HE::UUID{} && !wasLoaded) cm.unloadAsset(id);

	st.sourceFile = Importer::sourceFileOf(assetPath);
	st.loaded     = true;
}

// The checkerboard that says "transparent here", drawn ONLY under the picture:
// the canvas around it stays the plain panel colour, so where the image ends is
// never in doubt. Cells are fixed on screen, not per texel — a 4K texture at
// fit would otherwise turn them into noise.
void drawChecker(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImVec2 clip0, ImVec2 clip1)
{
	constexpr float kCell = 12.0f;
	const ImVec2 a{ std::max(p0.x, clip0.x), std::max(p0.y, clip0.y) };
	const ImVec2 b{ std::min(p1.x, clip1.x), std::min(p1.y, clip1.y) };
	if (b.x <= a.x || b.y <= a.y) return;
	dl->AddRectFilled(a, b, IM_COL32(150, 150, 150, 255));
	const int x0 = static_cast<int>(std::floor((a.x - p0.x) / kCell));
	const int y0 = static_cast<int>(std::floor((a.y - p0.y) / kCell));
	for (int cy = y0; p0.y + cy * kCell < b.y; ++cy)
		for (int cx = x0 + ((x0 + cy) & 1); p0.x + cx * kCell < b.x; cx += 2)
		{
			const ImVec2 c0{ std::max(a.x, p0.x + cx * kCell), std::max(a.y, p0.y + cy * kCell) };
			const ImVec2 c1{ std::min(b.x, p0.x + (cx + 1) * kCell), std::min(b.y, p0.y + (cy + 1) * kCell) };
			if (c1.x > c0.x && c1.y > c0.y) dl->AddRectFilled(c0, c1, IM_COL32(105, 105, 105, 255));
		}
}

// Each drawing helper opens the tooltip scope itself rather than render(): the
// help audit (scripts/editor_help_audit.py) reads a file top to bottom, and a
// scope pushed in render() at the end would come after every control it covers.
void drawInfo(AppContext& ctx, const std::string& assetPath, State& st)
{
	HE::Ed::Help::Scope helpScope("Texture Viewer");
	char buf[64];
	{
		EditorWidgets::WrapText wrap;
		ImGui::TextUnformatted(st.name.c_str());
		ImGui::TextDisabled(st.isRawFile ? "Image file, not imported yet" : "Texture asset");
	}

	ImGui::SeparatorText("Image");
	if (st.width > 0)
	{
		ImGui::Text("Size      %u x %u", st.width, st.height);
		if (st.isRawFile)
			ImGui::TextUnformatted("Stored as RGBA8 once imported");
		else
		{
			ImGui::Text("Format    %s, %u channel%s", formatName(st.format), st.channels,
			            st.channels == 1 ? "" : "s");
			ImGui::Text("Colour    %s", st.srgb ? "sRGB (colour)" : "Linear (data)");
			ImGui::Text("Mips      %u", st.mipLevels);
			formatBytes(st.bytes, buf, sizeof(buf));
			ImGui::Text("Memory    %s", buf);
		}
	}
	if (!st.sourceFile.empty())
	{
		EditorWidgets::WrapText wrap;
		ImGui::TextDisabled("Imported from %s", st.sourceFile.c_str());
	}

	// ── Import, for a raw image file ─────────────────────────────────────────
	if (!st.isRawFile || !ctx.contentManager) return;
	ImGui::SeparatorText("Import");
	const std::filesystem::path src(assetPath);
	const bool engineLocked = ctx.contentManager->isEngineDefaultPath(assetPath) &&
	                          !ContentManager::isEngineContentDevMode();

	// Where the .hasset lands: beside the source — or, for a locked engine file,
	// in the project's own Content/Textures, which the project can reference.
	// Same rule as the audio tab's Import.
	std::filesystem::path root, relDir;
	if (engineLocked)
	{
		root   = ctx.contentManager->contentRoot();
		relDir = "Textures";
	}
	else
	{
		root = ctx.contentManager->isEngineDefaultPath(assetPath)
			? std::filesystem::path(ctx.contentManager->engineContentRoot())
			: std::filesystem::path(ctx.contentManager->contentRoot());
		std::error_code ec;
		relDir = std::filesystem::relative(src.parent_path(), root, ec);
		if (ec || relDir == ".") relDir.clear();
	}

	EditorWidgets::WrapText wrap;
	if (root.empty())
	{
		ImGui::TextDisabled("Open a project to import.");
		return;
	}
	const bool importable = st.error.empty();
	if (!importable) ImGui::BeginDisabled();
	if (EditorWidgets::button("Import as Texture Asset", ImVec2(-FLT_MIN, 0.0f)))
	{
		const std::string written = importImage(src, root, relDir);
		if (!written.empty())
		{
			ctx.contentRefreshPending = true;
			// This tab becomes the new asset's tab. Not done here: render() runs
			// with `assetPath` pointing INTO ctx.tabs.
			requestOpen(written, assetPath);
		}
		else
			HE_LOG_ERROR(Editor, "%s", ("Editor: texture import failed for " + assetPath).c_str());
	}
	if (!importable) ImGui::EndDisabled();
	ImGui::TextDisabled("Writes %s",
		Importer::resolveOutput({}, relDir, src.stem().string()).path.c_str());
	if (engineLocked)
		ImGui::TextDisabled("Engine content is read-only, so this goes to the project instead.");
	ImGui::TextDisabled("Imported as linear (data). The picture keeps the orientation shown here.");
}

void drawCanvas(State& st)
{
	HE::Ed::Help::Scope helpScope("Texture Viewer");
	// ── Toolbar row ─────────────────────────────────────────────────────────
	if (EditorWidgets::button("Fit")) st.fit = true;
	ImGui::SameLine();
	if (EditorWidgets::button("1:1")) { st.fit = false; st.zoom = 1.0f; st.pan = ImVec2(0.0f, 0.0f); }
	ImGui::SameLine();
	ImGui::TextDisabled("%.0f%%", st.zoom * 100.0f);
	ImGui::SameLine(0.0f, 18.0f);

	bool r = st.channelMask & kChannelR, g = st.channelMask & kChannelG,
	     b = st.channelMask & kChannelB, a = st.channelMask & kChannelA;
	bool changed = false;
	changed |= EditorWidgets::checkbox("Red", &r);   ImGui::SameLine();
	changed |= EditorWidgets::checkbox("Green", &g); ImGui::SameLine();
	changed |= EditorWidgets::checkbox("Blue", &b);  ImGui::SameLine();
	changed |= EditorWidgets::checkbox("Alpha", &a); ImGui::SameLine(0.0f, 18.0f);
	EditorWidgets::checkbox("Checkerboard", &st.checker);
	if (changed)
	{
		const unsigned mask = (r ? kChannelR : 0u) | (g ? kChannelG : 0u) |
		                      (b ? kChannelB : 0u) | (a ? kChannelA : 0u);
		// All four off shows nothing at all; keep the last channel instead.
		if (mask != 0) st.channelMask = mask;
	}

	// ── The picture ─────────────────────────────────────────────────────────
	if (st.pixels.empty()) return;
	if (st.uploadedMask != st.channelMask && !st.uploadFailed)
	{
		retire(st);
		if (st.channelMask == kChannelAll)
			st.texture = upload(st.pixels, st.width, st.height);
		else
		{
			std::vector<uint8_t> filtered;
			applyChannelMask(st.pixels, st.channelMask, filtered);
			st.texture = upload(filtered, st.width, st.height);
		}
		st.uploadedMask = st.channelMask;
		st.uploadFailed = st.texture == nullptr;
	}

	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const float  footH = ImGui::GetTextLineHeightWithSpacing();
	const ImVec2 size{ std::max(64.0f, avail.x), std::max(64.0f, avail.y - footH) };
	const ImVec2 c0 = ImGui::GetCursorScreenPos();
	const ImVec2 c1{ c0.x + size.x, c0.y + size.y };
	ImGui::InvisibleButton("##texCanvas", size,
	                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
	const bool hovered = ImGui::IsItemHovered();
	const bool active  = ImGui::IsItemActive();

	const float w = static_cast<float>(st.width), h = static_cast<float>(st.height);
	if (st.fit)
	{
		// A margin so the picture's edge is visible against the panel.
		st.zoom = std::max(1e-4f, std::min((size.x - 16.0f) / w, (size.y - 16.0f) / h));
		st.pan  = ImVec2(0.0f, 0.0f);
	}

	const ImVec2 centre{ (c0.x + c1.x) * 0.5f + st.pan.x, (c0.y + c1.y) * 0.5f + st.pan.y };
	ImVec2 p0{ centre.x - w * st.zoom * 0.5f, centre.y - h * st.zoom * 0.5f };

	ImGuiIO& io = ImGui::GetIO();
	if (hovered && io.MouseWheel != 0.0f)
	{
		// Zoom about the pointer: the texel under it stays under it.
		const float next = std::clamp(st.zoom * std::pow(1.2f, io.MouseWheel), 1.0f / 64.0f, 64.0f);
		const ImVec2 texel{ (io.MousePos.x - p0.x) / st.zoom, (io.MousePos.y - p0.y) / st.zoom };
		const ImVec2 np0{ io.MousePos.x - texel.x * next, io.MousePos.y - texel.y * next };
		st.pan.x += (np0.x + w * next * 0.5f) - (p0.x + w * st.zoom * 0.5f);
		st.pan.y += (np0.y + h * next * 0.5f) - (p0.y + h * st.zoom * 0.5f);
		st.zoom = next;
		st.fit  = false;
		p0 = np0;
	}
	if (active && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
	{
		st.pan.x += io.MouseDelta.x; st.pan.y += io.MouseDelta.y;
		p0.x     += io.MouseDelta.x; p0.y     += io.MouseDelta.y;
		st.fit = false;
	}
	if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) st.fit = true;

	const ImVec2 p1{ p0.x + w * st.zoom, p0.y + h * st.zoom };
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(c0, c1, true);
	if (st.checker) drawChecker(dl, p0, p1, c0, c1);
	if (st.texture)
		// uv (0,0) at the top-left: the uploaded pixels are already top-down.
		dl->AddImage(reinterpret_cast<ImTextureID>(st.texture), p0, p1,
		             ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
	else
		dl->AddText(ImVec2(c0.x + 8.0f, c0.y + 8.0f), IM_COL32(220, 160, 120, 255),
		            "The renderer could not create a preview texture of this size.");
	dl->AddRect(ImVec2(p0.x - 1.0f, p0.y - 1.0f), ImVec2(p1.x + 1.0f, p1.y + 1.0f),
	            IM_COL32(255, 255, 255, 40));
	dl->PopClipRect();

	// ── Footer: the texel under the pointer ─────────────────────────────────
	const int tx = static_cast<int>(std::floor((io.MousePos.x - p0.x) / st.zoom));
	const int ty = static_cast<int>(std::floor((io.MousePos.y - p0.y) / st.zoom));
	if (hovered && tx >= 0 && ty >= 0 && tx < static_cast<int>(st.width) && ty < static_cast<int>(st.height))
	{
		const uint8_t* px = &st.pixels[(static_cast<size_t>(ty) * st.width + tx) * 4];
		ImGui::Text("x %d  y %d   R %u  G %u  B %u  A %u", tx, ty, px[0], px[1], px[2], px[3]);
	}
	else
		ImGui::TextDisabled("Wheel to zoom, drag to pan, double-click to fit. y counts from the top.");
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────────────

bool isImageSource(const std::string& path)
{
	const std::string ext = lowerExtension(path);
	if (ext.empty()) return false;
	const std::string pattern = Importer::sourceFamilyPattern(Importer::SourceFamily::Texture);
	size_t start = 0;
	while (start <= pattern.size())
	{
		const size_t end = std::min(pattern.find(';', start), pattern.size());
		if (pattern.compare(start, end - start, ext) == 0) return true;
		start = end + 1;
	}
	return false;
}

bool isTextureAsset(const std::string& path)
{
	if (isImageSource(path)) return true;
	return EditorAssetTypeCache::is(path, HE::AssetType::Texture);
}

void forget(const std::string& assetPath)
{
	if (State* st = s_states.find(assetPath)) retire(*st);
	s_states.forget(assetPath);
}

void beginFrame()
{
	for (void* t : s_pendingRelease)
	{
		if (s_release)       s_release(t);
		else if (s_renderer) s_renderer->DestroyImGuiTexture(t);
	}
	s_pendingRelease.clear();
}

void requestOpen(const std::string& absPath, const std::string& replacing)
{
	s_openRequests.push_back({ absPath, replacing });
}

OpenRequest takeOpenRequest()
{
	if (s_openRequests.empty()) return {};
	OpenRequest r = std::move(s_openRequests.front());
	s_openRequests.erase(s_openRequests.begin());
	return r;
}

std::string importImage(const std::filesystem::path& source,
                        const std::filesystem::path& root,
                        const std::filesystem::path& relDir)
{
	if (!isImageSource(source.string())) return {};
	if (!Importer::importSource(source, root, relDir)) return {};
	// TextureImporter names its output exactly so (TextureImporter.cpp).
	const std::string rel = Importer::resolveOutput({}, relDir, source.stem().string()).path;
	return (root / rel).make_preferred().string();
}

bool toDisplayRgba(const TextureAsset& tex, std::vector<uint8_t>& out)
{
	out.clear();
	if (tex.format != TextureFormat::RGBA8) return false;
	const uint32_t w = tex.width, h = tex.height, ch = tex.channels;
	if (w == 0 || h == 0 || (ch != 1 && ch != 3 && ch != 4)) return false;
	if (tex.data.size() < static_cast<size_t>(w) * h * ch) return false;

	out.resize(static_cast<size_t>(w) * h * 4);
	for (uint32_t y = 0; y < h; ++y)
	{
		// Stored bottom-up: display row y is data row h-1-y.
		const uint8_t* src = &tex.data[static_cast<size_t>(h - 1 - y) * w * ch];
		uint8_t*       dst = &out[static_cast<size_t>(y) * w * 4];
		for (uint32_t x = 0; x < w; ++x, src += ch, dst += 4)
		{
			if (ch == 1)      { dst[0] = dst[1] = dst[2] = src[0]; dst[3] = 255; }
			else if (ch == 3) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255; }
			else              { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3]; }
		}
	}
	return true;
}

void applyChannelMask(const std::vector<uint8_t>& src, unsigned mask, std::vector<uint8_t>& out)
{
	out.resize(src.size());
	const unsigned colour = mask & (kChannelR | kChannelG | kChannelB);
	// Exactly one channel on (colour or alpha alone): show it as grey.
	int solo = -1;
	if      (mask == kChannelR) solo = 0;
	else if (mask == kChannelG) solo = 1;
	else if (mask == kChannelB) solo = 2;
	else if (mask == kChannelA) solo = 3;
	for (size_t i = 0; i + 3 < src.size(); i += 4)
	{
		if (solo >= 0)
		{
			out[i] = out[i + 1] = out[i + 2] = src[i + solo];
			out[i + 3] = 255;
			continue;
		}
		out[i]     = (colour & kChannelR) ? src[i]     : 0;
		out[i + 1] = (colour & kChannelG) ? src[i + 1] : 0;
		out[i + 2] = (colour & kChannelB) ? src[i + 2] : 0;
		out[i + 3] = (mask & kChannelA)   ? src[i + 3] : 255;
	}
}

void setTextureHooks(TextureUpload up, TextureRelease rel)
{
	s_upload  = std::move(up);
	s_release = std::move(rel);
}

void render(AppContext& ctx, const std::string& assetPath, const ImVec2& pos, const ImVec2& size)
{
	s_renderer = ctx.renderer;
	State& st = s_states[assetPath];

	// A re-import rewrote the file under the tab: start over from disk.
	if (st.loaded && ImGui::GetTime() - st.lastStat >= 1.0)
	{
		st.lastStat = ImGui::GetTime();
		if (stampOf(assetPath) != st.stamp)
		{
			retire(st);
			st.loaded = false;
		}
	}
	if (!st.loaded) load(ctx, assetPath, st);

	// A real host window pinned to the tab area (see AudioEditorPanel for why).
	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(size, ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::Begin("##TextureViewer", nullptr,
		ImGuiWindowFlags_NoTitleBar         | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove             | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoScrollbar        | ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoSavedSettings    | ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoDocking);
	ImGui::PopStyleVar();

	ImGui::BeginChild("##texInfo", ImVec2(260.0f, 0.0f), true);
	drawInfo(ctx, assetPath, st);
	if (!st.error.empty())
	{
		EditorWidgets::WrapText wrap;
		ImGui::Spacing();
		ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.45f, 1.0f), "%s", st.error.c_str());
	}
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild("##texCanvasPane", ImVec2(0.0f, 0.0f), true,
	                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	drawCanvas(st);
	ImGui::EndChild();

	ImGui::End();
}

} // namespace TextureViewerPanel
