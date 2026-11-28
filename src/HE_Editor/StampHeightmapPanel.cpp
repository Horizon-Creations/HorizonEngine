#include "StampHeightmapPanel.h"
#include "StampHeightmap.h"
#include "EditorApplication.h"     // AppContext, EditorUndo
#include "EditorWidgets.h"
#include "EditorHelp.h"            // "Stamp Heightmap/<label>" scope for its controls
#include "LandscapeWorldTools.h"   // the Stamp tool's heightmap file

#include <HorizonScene/Components/LandscapeStampComponent.h>
#include <ContentManager/ContentManager.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace StampHeightmapPanel
{
#ifdef HE_IMGUI_ENABLED
namespace
{
namespace Row = EditorWidgets::Row;
namespace SH = HE::StampHeightmap;

enum Tool { Raise, Lower, Smooth, Line, Transition };
const char* const kTools[] = { "Raise", "Lower", "Smooth", "Line", "Transition" };
const char* const kSizes[] = { "128", "256", "512", "1024" };
const int kSizeValues[] = { 128, 256, 512, 1024 };

struct State
{
	SH::Map map;
	std::vector<SH::Map> undo;         // the picture before each stroke, newest last
	int   tool = Raise;
	int   sizeIndex = 1;
	float radius = 24.0f, strength = 0.5f, startHeight = 1.0f, endHeight = 1.0f;
	std::string name = "Stamp";
	bool  live = true;
	bool  fileDirty = false;           // strokes the file does not hold yet
	std::string boundFile;             // the file Save or Load named: Live Preview writes only to it, never over a file nobody chose
	double lastWrite = 0.0;
	void* texture = nullptr;           // the picture as the renderer's ImGui texture
	bool  textureDirty = true;
	std::vector<void*> retired;        // textures a draw list of the last frame may still name
	bool  dragging = false;
	ImVec2 dragStart{}, dragLast{};    // in pixels of the picture
	std::string status;
};
State& st() { static State s; return s; }

// "Heightmaps/<name>.pgm": relative to the project's folder, the way a stamp's heightmap file is resolved (LandscapeStamps.cpp).
std::string relativeFile(const State& s)
{
	const std::string name = std::filesystem::path(s.name).filename().string();   // a name, not a way out of the folder
	return "Heightmaps/" + (name.empty() ? std::string("Stamp") : name) + ".pgm";
}
std::string absoluteFile(const AppContext& ctx, const State& s)
{
	return (std::filesystem::path(ctx.contentManager->contentRoot()).parent_path() / relativeFile(s)).string();
}

bool save(AppContext& ctx, State& s)
{
	std::string error;
	if (!SH::writePgm16(s.map, absoluteFile(ctx, s), error)) { s.status = error; return false; }
	s.fileDirty = false;
	s.boundFile = relativeFile(s);
	s.lastWrite = ImGui::GetTime();
	s.status = "Saved " + relativeFile(s);
	// shortcut: every stamp's picture is read again, not only this file's; give LandscapeStampSync a per-file reload when scenes hold many large stamps
	if (ctx.reloadLandscapeStamps) ctx.reloadLandscapeStamps();
	return true;
}

void pushUndo(State& s)
{
	if (s.undo.size() >= 32) s.undo.erase(s.undo.begin());
	s.undo.push_back(s.map);
}
void changed(State& s) { s.textureDirty = true; s.fileDirty = true; }

void uploadTexture(AppContext& ctx, State& s)
{
	for (void* t : s.retired) ctx.renderer->DestroyImGuiTexture(t);
	s.retired.clear();
	if (!s.textureDirty) return;
	if (s.texture) s.retired.push_back(s.texture);
	std::vector<uint8_t> rgba(s.map.v.size() * 4);
	for (size_t i = 0; i < s.map.v.size(); ++i)
	{
		const uint8_t g = static_cast<uint8_t>(std::lround(std::clamp(s.map.v[i], 0.0f, 1.0f) * 255.0f));
		rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = g;
		rgba[i * 4 + 3] = 255;
	}
	s.texture = ctx.renderer->CreateImGuiTexture(rgba.data(), s.map.w, s.map.h);
	s.textureDirty = false;
}

void drawControls(AppContext& ctx, State& s)
{
	Row::combo("Tool", &s.tool, kTools, IM_ARRAYSIZE(kTools));
	if (s.tool != Transition) Row::dragFloat("Radius", &s.radius, 0.5f, 1.0f, 512.0f, "%.0f px");
	Row::sliderFloat("Strength", &s.strength, 0.0f, 1.0f);
	if (s.tool == Line || s.tool == Transition)
	{
		Row::sliderFloat("Start Height", &s.startHeight, 0.0f, 1.0f);
		Row::sliderFloat("End Height", &s.endHeight, 0.0f, 1.0f);
	}
	ImGui::BeginDisabled(s.undo.empty());
	if (EditorWidgets::button("Undo Stroke"))
	{
		s.map = std::move(s.undo.back());
		s.undo.pop_back();
		changed(s);
	}
	ImGui::EndDisabled();

	ImGui::SeparatorText("Picture");
	Row::combo("Resolution", &s.sizeIndex, kSizes, IM_ARRAYSIZE(kSizes));
	if (EditorWidgets::button("New"))
	{
		pushUndo(s);
		SH::fill(s.map, kSizeValues[s.sizeIndex], kSizeValues[s.sizeIndex], 0.0f);
		changed(s);
	}
	ImGui::SameLine();
	if (EditorWidgets::button("Fill"))
	{
		pushUndo(s);
		SH::fill(s.map, s.map.w, s.map.h, s.startHeight);
		changed(s);
	}

	ImGui::SeparatorText("File");
	Row::inputText("Name", &s.name);
	if (EditorWidgets::button("Save")) save(ctx, s);
	ImGui::SameLine();
	if (EditorWidgets::button("Load"))
	{
		SH::Map loaded;
		std::string error;
		if (SH::read(loaded, absoluteFile(ctx, s), error))
		{
			pushUndo(s);
			s.map = std::move(loaded);
			s.textureDirty = true;
			s.fileDirty = false;
			s.boundFile = relativeFile(s);
			s.status = "Loaded " + relativeFile(s);
		}
		else s.status = error;
	}
	EditorWidgets::checkbox("Live Preview", &s.live);

	ImGui::SeparatorText("Landscape");
	auto& reg = ctx.world->registry();
	const Entity sel = ctx.selection.primary();
	LandscapeStampComponent* stamp = reg.valid(sel) ? reg.try_get<LandscapeStampComponent>(sel) : nullptr;
	ImGui::BeginDisabled(!stamp);
	if (EditorWidgets::button("Assign to Selected Stamp") && save(ctx, s))
	{
		if (ctx.undoSys) ctx.undoSys->snapshotNow("Assign Stamp Heightmap");
		stamp->heightmapFile = relativeFile(s);
	}
	ImGui::EndDisabled();
	if (EditorWidgets::button("Use in Stamp Tool") && save(ctx, s))
	{
		LandscapeWorldTools::setStampToolFile(relativeFile(s));
		s.status = "The Stamp tool of the Landscape mode now places " + relativeFile(s);
	}
	if (!s.status.empty()) ImGui::TextDisabled("%s", s.status.c_str());
}

void drawCanvas(State& s)
{
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const float scale = std::min(avail.x / static_cast<float>(s.map.w), avail.y / static_cast<float>(s.map.h));
	if (scale <= 0.0f) return;
	const ImVec2 p0 = ImGui::GetCursorScreenPos();
	const ImVec2 size(s.map.w * scale, s.map.h * scale);
	ImGui::InvisibleButton("##stampHeightmapCanvas", size);
	const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	if (s.texture) dl->AddImage(reinterpret_cast<ImTextureID>(s.texture), p0, ImVec2(p0.x + size.x, p0.y + size.y));
	else
	{
		// A backend without ImGui textures (Software): the picture as 64 x 64 flat cells, coarse but paintable.
		const int n = 64;
		for (int cy = 0; cy < n; ++cy)
			for (int cx = 0; cx < n; ++cx)
			{
				const float v = s.map.v[static_cast<size_t>((cy * 2 + 1) * s.map.h / (n * 2)) * s.map.w + (cx * 2 + 1) * s.map.w / (n * 2)];
				dl->AddRectFilled(ImVec2(p0.x + size.x * cx / n, p0.y + size.y * cy / n),
				                  ImVec2(p0.x + size.x * (cx + 1) / n, p0.y + size.y * (cy + 1) / n), ImGui::GetColorU32(ImVec4(v, v, v, 1.0f)));
			}
	}
	dl->AddRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), ImGui::GetColorU32(ImGuiCol_Border));

	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const ImVec2 px((mouse.x - p0.x) / scale - 0.5f, (mouse.y - p0.y) / scale - 0.5f);   // the mouse in pixels of the picture
	const auto screen = [&](const ImVec2& p) { return ImVec2(p0.x + (p.x + 0.5f) * scale, p0.y + (p.y + 0.5f) * scale); };
	const bool dragTool = s.tool == Line || s.tool == Transition;   // applied when the button is let go

	if (active && !s.dragging)
	{
		pushUndo(s);
		s.dragging = true;
		s.dragStart = s.dragLast = px;
	}
	if (active && !dragTool)
	{
		// Dabs a quarter radius apart along the way the mouse went; a resting mouse keeps adding by time.
		const float dist = std::hypot(px.x - s.dragLast.x, px.y - s.dragLast.y);
		const int steps = static_cast<int>(dist / std::max(1.0f, s.radius * 0.25f));
		const float amount = steps > 0 ? s.strength * 0.05f : s.strength * ImGui::GetIO().DeltaTime * 2.0f;
		for (int i = 1; i <= std::max(1, steps); ++i)
		{
			const float t = steps > 0 ? static_cast<float>(i) / steps : 1.0f;
			const float x = s.dragLast.x + (px.x - s.dragLast.x) * t, y = s.dragLast.y + (px.y - s.dragLast.y) * t;
			if (s.tool == Smooth) SH::smooth(s.map, x, y, s.radius, s.strength);
			else SH::dab(s.map, x, y, s.radius, s.tool == Lower ? -amount : amount);
		}
		if (steps > 0) s.dragLast = px;
		changed(s);
	}
	if (s.dragging && !active)
	{
		if (s.tool == Line) SH::line(s.map, s.dragStart.x, s.dragStart.y, px.x, px.y, s.radius, s.startHeight, s.endHeight, s.strength);
		if (s.tool == Transition) SH::gradient(s.map, s.dragStart.x, s.dragStart.y, px.x, px.y, s.startHeight, s.endHeight, s.strength);
		if (dragTool) changed(s);
		s.dragging = false;
	}

	const ImU32 accent = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
	if (s.dragging && dragTool) dl->AddLine(screen(s.dragStart), screen(px), accent, 2.0f);
	if (hovered && s.tool != Transition) dl->AddCircle(screen(px), s.radius * scale, accent, 48);
}
} // namespace

void DrawWindow(AppContext& ctx, bool& open)
{
	// Every control here is looked up as "Stamp Heightmap/<its label>".
	HE::Ed::Help::Scope helpScope("Stamp Heightmap");
	if (!open) return;

	ImGui::SetNextWindowSize(ImVec2(820.0f, 560.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Stamp Heightmap", &open)) { ImGui::End(); return; }
	if (!ctx.projectLoaded || !ctx.world || !ctx.contentManager || !ctx.renderer)
	{
		ImGui::TextDisabled("No project is open.");
		ImGui::End();
		return;
	}
	State& s = st();
	if (s.map.v.empty()) SH::fill(s.map, 256, 256, 0.0f);

	ImGui::BeginChild("##stampHeightmapControls", ImVec2(280.0f, 0.0f));
	drawControls(ctx, s);
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("##stampHeightmapCanvasArea");
	uploadTexture(ctx, s);
	drawCanvas(s);
	ImGui::EndChild();

	// Live: the file follows the strokes (during one, at most seven times a second), and the stamps that read it with it.
	if (s.live && s.fileDirty && s.boundFile == relativeFile(s) && (!s.dragging || ImGui::GetTime() - s.lastWrite > 0.15)) save(ctx, s);
	ImGui::End();
}

#else
void DrawWindow(AppContext&, bool&) {}
#endif
}
