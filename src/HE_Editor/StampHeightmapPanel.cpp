#include "StampHeightmapPanel.h"
#include "StampHeightmap.h"
#include "EditorApplication.h"     // AppContext, EditorUndo
#include "EditorToolbar.h"         // the shared toolbar strip: menus, modes, Save
#include "EditorWidgets.h"
#include "EditorHelp.h"            // "Stamp Heightmap/<label>" scope for its controls
#include "EditorCamera.h"          // the Scene window's camera, for the test landscape
#include "EditorViewportNav.h"     // ... and its navigation grammar
#include "LandscapeWorldTools.h"   // the Stamp tool's heightmap file

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/TerrainSystem.h>
#include <HorizonScene/Components/LandscapeStampComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <Diagnostics/Log.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
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
namespace T = EditorToolbar;

// The modes of the bar, in the order a heightmap is usually made: a landform from a seed, corrections by hand, the pass that
// makes the hand disappear, weather, finishing. Any order works; each mode shows only its own options on the left.
enum Mode { ModeGenerate, ModeSketch, ModeNaturalize, ModeErode, ModeFilters, ModeSelect };
enum Tool { Raise, Lower, Smooth, Flatten, Line, Transition, Select, Deselect, NoTool };
const char* const kSketchTools[] = { "Raise", "Lower", "Smooth", "Flatten", "Line", "Transition" };
const char* const kSelectBrushes[] = { "Add to Selection", "Remove from Selection" };
const char* const kShapes[] = { "Hills", "Mountain", "Mountain Range", "Plateau", "Crater", "Volcano", "Dunes", "Island" };
const char* const kBlends[] = { "Replace", "Add", "Subtract", "Lighten (Max)", "Darken (Min)" };
// The test landscape's grid: 2^9 + 1 a side, four times as fine as it was. A 256 picture is then drawn finer than it is stored.
constexpr uint32_t kPreviewResolution = 513;

struct State
{
	SH::Map map;
	SH::Map mask;                      // the selection, 0..1; no values: everything
	std::vector<SH::Map> undo, redo;   // the picture before each step, newest last
	int   mode = ModeGenerate;
	int   sketchTool = Raise, selectBrush = 0;
	float radius = 24.0f, strength = 0.5f, startHeight = 1.0f, endHeight = 1.0f;

	int   shape = static_cast<int>(SH::Shape::Mountain), blend = 0, seed = 1, detail = 6;
	float scale = 0.35f, roughness = 0.5f, warp = 0.3f, genHeight = 1.0f;
	float natSmoothing = 0.04f, natDetail = 0.35f, natWarp = 0.25f, natErosion = 0.5f;
	float erosion = 0.5f, blurRadius = 4.0f, terraceSharpness = 0.7f, fadeWidth = 0.3f;
	int   thermalPasses = 4, terraceSteps = 8;

	std::string name = "Stamp";
	bool  live = true;
	bool  fileDirty = false;           // changes the file does not hold yet
	std::string boundFile;             // the file Save or Open named: Live Preview writes only to it, never over a file nobody chose
	double lastWrite = 0.0;
	void* texture = nullptr;           // the picture as the renderer's ImGui texture
	bool  textureDirty = true;
	std::vector<void*> retired;        // textures a draw list of the last frame may still name
	bool  dragging = false;
	ImVec2 dragStart{}, dragLast{};    // in pixels of the picture
	std::string status;

	// The test landscape: a one-terrain world drawn by the renderer's world preview.
	std::unique_ptr<HorizonWorld> world;
	Entity terrain = entt::null;
	ContentManager* worldContent = nullptr;   // whose assets the terrain's chunks, material and texture are; another one (another project) starts over
	HE::UUID groundTexture, groundMaterial;
	EditorCamera cam;
	bool  camFramed = false, previewDirty = true, previewSaid = false, groundColours = true;
	double lastPreview = 0.0;
	float previewSize = 256.0f, previewHeight = 80.0f, timeOfDay = 0.33f;
	// What the ground colours are multiplied with (the material's base colour). Well below 1: a white sheet under the preview's
	// sun burns out. The preview has no exposure of its own to turn down, so this is the brightness control; the sky keeps its own.
	float groundBrightness = 0.35f;
	float clouds = 0.35f;              // WorldPreviewEnv::cloudCoverage: overcast dims the sun
};
State& st() { static State s; return s; }

const SH::Map* maskOf(const State& s) { return s.mask.v.size() == s.map.v.size() && !s.mask.v.empty() ? &s.mask : nullptr; }

// "Heightmaps/<name>.pgm": relative to the project's folder, the way a stamp's heightmap file is resolved (LandscapeStamps.cpp).
std::string relativeFile(const State& s)
{
	const std::string name = std::filesystem::path(s.name).filename().string();   // a name, not a way out of the folder
	return "Heightmaps/" + (name.empty() ? std::string("Stamp") : name) + ".pgm";
}
std::filesystem::path projectDir(const AppContext& ctx) { return std::filesystem::path(ctx.contentManager->contentRoot()).parent_path(); }

bool saveFile(AppContext& ctx, State& s)
{
	std::string error;
	if (!SH::writePgm16(s.map, (projectDir(ctx) / relativeFile(s)).string(), error)) { s.status = error; return false; }
	s.fileDirty = false;
	s.boundFile = relativeFile(s);
	s.lastWrite = ImGui::GetTime();
	s.status = "Saved " + relativeFile(s);
	// shortcut: every stamp's picture is read again, not only this file's; give LandscapeStampSync a per-file reload when scenes hold many large stamps
	if (ctx.reloadLandscapeStamps) ctx.reloadLandscapeStamps();
	return true;
}

// Before a step that changes the picture: it can be taken back.
void pushUndo(State& s)
{
	if (s.undo.size() >= 16) s.undo.erase(s.undo.begin());
	s.undo.push_back(s.map);
	s.redo.clear();
}
void changed(State& s) { s.textureDirty = true; s.fileDirty = true; s.previewDirty = true; }
// The picture got another size (New, Open, an undo across one): a selection of the old size means nothing.
void sizeChanged(State& s) { if (s.mask.v.size() != s.map.v.size()) s.mask = {}; }

void undoStep(State& s)
{
	if (s.undo.empty()) return;
	s.redo.push_back(std::move(s.map));
	s.map = std::move(s.undo.back());
	s.undo.pop_back();
	sizeChanged(s);
	changed(s);
}
void redoStep(State& s)
{
	if (s.redo.empty()) return;
	s.undo.push_back(std::move(s.map));
	s.map = std::move(s.redo.back());
	s.redo.pop_back();
	sizeChanged(s);
	changed(s);
}
void newPicture(State& s, int size)
{
	pushUndo(s);
	SH::fill(s.map, size, size, 0.0f);
	sizeChanged(s);
	changed(s);
}

void uploadTexture(AppContext& ctx, State& s)
{
	for (void* t : s.retired) ctx.renderer->DestroyImGuiTexture(t);
	s.retired.clear();
	if (!s.textureDirty) return;
	if (s.texture) s.retired.push_back(s.texture);
	const SH::Map* mask = maskOf(s);
	std::vector<uint8_t> rgba(s.map.v.size() * 4);
	for (size_t i = 0; i < s.map.v.size(); ++i)
	{
		const float g = std::clamp(s.map.v[i], 0.0f, 1.0f), sel = mask ? std::clamp(mask->v[i], 0.0f, 1.0f) : 1.0f;
		// Outside the selection the picture is dimmed toward blue.
		const float rgb[3] = { g * (0.4f + 0.6f * sel), g * (0.45f + 0.55f * sel), g * (0.5f + 0.5f * sel) + 0.2f * (1.0f - sel) };
		for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = static_cast<uint8_t>(std::lround(std::clamp(rgb[c], 0.0f, 1.0f) * 255.0f));
		rgba[i * 4 + 3] = 255;
	}
	s.texture = ctx.renderer->CreateImGuiTexture(rgba.data(), s.map.w, s.map.h);
	s.textureDirty = false;
}

void releasePreview(AppContext& ctx, State& s)
{
	if (s.world && s.worldContent == ctx.contentManager)
	{
		TerrainSystem::releaseWorld(*s.world, *ctx.contentManager, ctx.renderer);
		ctx.contentManager->unloadAsset(s.groundMaterial);
		ctx.contentManager->unloadAsset(s.groundTexture);
	}
	s.world.reset();
	s.terrain = entt::null;
	s.groundTexture = s.groundMaterial = HE::UUID{};
	s.previewDirty = true;
}

// ── The bar: menus, modes, undo, Save ──
void drawFileMenu(AppContext& ctx, State& s)
{
	ImGui::SetNextWindowSizeConstraints(ImVec2(280.0f, 0.0f), ImVec2(420.0f, FLT_MAX));
	if (!ImGui::BeginPopup("##shFileMenu")) return;
	if (ImGui::BeginMenu("New Picture"))
	{
		if (EditorWidgets::menuItem("128 x 128")) newPicture(s, 128);
		if (EditorWidgets::menuItem("256 x 256")) newPicture(s, 256);
		if (EditorWidgets::menuItem("512 x 512")) newPicture(s, 512);
		if (EditorWidgets::menuItem("1024 x 1024")) newPicture(s, 1024);
		ImGui::EndMenu();
	}
	Row::inputText("Name", &s.name);
	if (EditorWidgets::menuItem("Open")) open(ctx, relativeFile(s));
	if (EditorWidgets::menuItem("Save", "Ctrl+S")) saveFile(ctx, s);
	ImGui::Separator();
	if (EditorWidgets::menuItem("Live Preview", nullptr, s.live)) s.live = !s.live;
	auto& reg = ctx.world->registry();
	const Entity sel = ctx.selection.primary();
	LandscapeStampComponent* stamp = reg.valid(sel) ? reg.try_get<LandscapeStampComponent>(sel) : nullptr;
	if (EditorWidgets::menuItem("Assign to Selected Stamp", nullptr, false, stamp != nullptr) && saveFile(ctx, s))
	{
		if (ctx.undoSys) ctx.undoSys->snapshotNow("Assign Stamp Heightmap");
		stamp->heightmapFile = relativeFile(s);
		s.status = "The selected stamp now reads " + relativeFile(s);
	}
	if (EditorWidgets::menuItem("Use in Stamp Tool") && saveFile(ctx, s))
	{
		LandscapeWorldTools::setStampToolFile(relativeFile(s));
		s.status = "The Stamp tool of the Landscape mode now places " + relativeFile(s);
	}
	ImGui::EndPopup();
}

void drawEditMenu(State& s)
{
	if (!ImGui::BeginPopup("##shEditMenu")) return;
	if (EditorWidgets::menuItem("Undo Step", nullptr, false, !s.undo.empty())) undoStep(s);
	if (EditorWidgets::menuItem("Redo Step", nullptr, false, !s.redo.empty())) redoStep(s);
	ImGui::Separator();
	const bool has = maskOf(s) != nullptr;
	if (EditorWidgets::menuItem("Clear Selection", nullptr, false, has)) { s.mask = {}; s.textureDirty = true; }
	if (EditorWidgets::menuItem("Invert Selection", nullptr, false, has)) { for (float& v : s.mask.v) v = 1.0f - v; s.textureDirty = true; }
	ImGui::EndPopup();
}

void applyGroundBrightness(AppContext& ctx, State& s)
{
	if (s.worldContent != ctx.contentManager) return;
	if (MaterialAsset* mat = ctx.contentManager->getMaterialMutable(s.groundMaterial))
	{
		mat->baseColor[0] = mat->baseColor[1] = mat->baseColor[2] = s.groundBrightness;
		ctx.renderer->InvalidateMaterial(s.groundMaterial);
	}
}

void drawViewMenu(AppContext& ctx, State& s)
{
	ImGui::SetNextWindowSizeConstraints(ImVec2(280.0f, 0.0f), ImVec2(420.0f, FLT_MAX));
	if (!ImGui::BeginPopup("##shViewMenu")) return;
	if (Row::dragFloat("Test Size", &s.previewSize, 1.0f, 16.0f, 8192.0f, "%.0f m")) { s.previewDirty = true; s.camFramed = false; }
	if (Row::dragFloat("Test Height", &s.previewHeight, 0.5f, 1.0f, 4096.0f, "%.0f m")) s.previewDirty = true;
	Row::sliderFloat("Time of Day", &s.timeOfDay, 0.2f, 0.8f);
	if (Row::sliderFloat("Brightness", &s.groundBrightness, 0.05f, 1.0f)) applyGroundBrightness(ctx, s);
	Row::sliderFloat("Clouds", &s.clouds, 0.0f, 1.0f);
	if (EditorWidgets::menuItem("Ground Colours", nullptr, s.groundColours)) { s.groundColours = !s.groundColours; s.previewDirty = true; }
	if (EditorWidgets::menuItem("Frame Test Landscape", "F")) s.camFramed = false;
	ImGui::EndPopup();
}

void drawBar(AppContext& ctx, State& s)
{
	// OpenPopup and BeginPopup have to meet in one ID scope, and the bar's cells push their own: remember, open below.
	const char* openMenu = nullptr;
	{
		T::Bar bar;
		bar.group();
		if (bar.item("##shFile", T::iconFolder, "File", false, true, "New picture, open, save, hand the picture to a stamp")) openMenu = "##shFileMenu";
		if (bar.item("##shEdit", T::iconList, "Edit", false, true, "Undo, redo, the selection")) openMenu = "##shEditMenu";
		if (bar.item("##shView", T::iconEye, "View", false, true, "The test landscape: size, height, sun, colours")) openMenu = "##shViewMenu";
		bar.endGroup();
		bar.gap();
		bar.group();
		const auto mode = [&](const char* id, T::IconFn icon, const char* label, int m, const char* tip) {
			if (bar.item(id, icon, label, s.mode == m, true, tip)) s.mode = m;
		};
		mode("##shGenerate", T::iconMountain, "Generate", ModeGenerate, "Make a landform from a seed");
		mode("##shSketch", T::iconBrush, "Sketch", ModeSketch, "Draw heights roughly with brushes, lines and transitions");
		mode("##shNaturalize", T::iconSparkle, "Naturalize", ModeNaturalize, "Turn a rough sketch into ground that looks grown");
		mode("##shErode", T::iconWave, "Erode", ModeErode, "Let water and gravity work on the ground");
		mode("##shFilters", T::iconLayers, "Filters", ModeFilters, "Blur, terrace, fade the edges, normalize, invert");
		mode("##shSelect", T::iconCircle, "Select", ModeSelect, "Paint the region the other modes are limited to");
		bar.endGroup();
		bar.gap();
		bar.group();
		if (bar.item("##shUndo", T::iconHistory, "Undo", false, !s.undo.empty(), "Take back the last step")) undoStep(s);
		if (bar.item("##shRedo", T::iconRefresh, "Redo", false, !s.redo.empty(), "Bring back the step Undo took away")) redoStep(s);
		bar.endGroup();
		bar.markDirty(s.fileDirty);
		if (T::saveButton(bar, true)) saveFile(ctx, s);   // with the editor's save hook this goes the Ctrl+S way (save() below)
	}
	if (openMenu) ImGui::OpenPopup(openMenu);
	drawFileMenu(ctx, s);
	drawEditMenu(s);
	drawViewMenu(ctx, s);
}

// ── The left column: the options of the mode that is on, and its one action ──
void hint(const char* text)
{
	EditorWidgets::WrapText wrap;
	ImGui::TextDisabled("%s", text);
}

void drawModeOptions(State& s)
{
	const bool selection = maskOf(s) != nullptr;
	const char* const where = selection ? "Works in the selection." : "Works on the whole picture. Paint a region in Select to limit it.";
	switch (s.mode)
	{
	case ModeGenerate:
	{
		ImGui::SeparatorText("Generate");
		hint("A landform from a seed. The same seed gives the same ground, another one a sibling of it.");
		Row::combo("Landform", &s.shape, kShapes, IM_ARRAYSIZE(kShapes));
		Row::combo("Blend", &s.blend, kBlends, IM_ARRAYSIZE(kBlends));
		Row::inputInt("Seed", &s.seed);
		Row::sliderFloat("Feature Size", &s.scale, 0.05f, 1.5f);
		Row::sliderInt("Octaves", &s.detail, 1, 9);
		Row::sliderFloat("Roughness", &s.roughness, 0.2f, 0.8f);
		Row::sliderFloat("Warp##gen", &s.warp, 0.0f, 1.0f);
		Row::sliderFloat("Peak Height", &s.genHeight, 0.0f, 1.0f);
		const bool generate = EditorWidgets::primaryButton("Generate");
		ImGui::SameLine();
		const bool again = EditorWidgets::button("New Seed");   // the same landform, another sibling of it
		if (again) ++s.seed;
		if (generate || again)
		{
			pushUndo(s);
			SH::GenParams p;
			p.shape = static_cast<SH::Shape>(std::clamp(s.shape, 0, static_cast<int>(SH::Shape::Count) - 1));
			p.blend = static_cast<SH::Blend>(std::clamp(s.blend, 0, static_cast<int>(SH::Blend::Count) - 1));
			p.seed = static_cast<uint32_t>(s.seed);
			p.scale = s.scale; p.detail = s.detail; p.roughness = s.roughness; p.warp = s.warp; p.height = s.genHeight;
			SH::generate(s.map, p, maskOf(s));
			changed(s);
		}
		hint(where);
		break;
	}
	case ModeSketch:
	{
		ImGui::SeparatorText("Sketch");
		hint("Draw on the picture where the ground is high. Rough is enough: Naturalize works it out.");
		Row::combo("Tool", &s.sketchTool, kSketchTools, IM_ARRAYSIZE(kSketchTools));
		if (s.sketchTool != Transition) Row::dragFloat("Radius", &s.radius, 0.5f, 1.0f, 512.0f, "%.0f px");
		Row::sliderFloat("Strength", &s.strength, 0.0f, 1.0f);
		if (s.sketchTool == Line || s.sketchTool == Transition || s.sketchTool == Flatten)
			Row::sliderFloat("Start Height", &s.startHeight, 0.0f, 1.0f);
		if (s.sketchTool == Line || s.sketchTool == Transition)
			Row::sliderFloat("End Height", &s.endHeight, 0.0f, 1.0f);
		if (EditorWidgets::button("Fill"))
		{
			pushUndo(s);
			SH::fill(s.map, s.map.w, s.map.h, s.startHeight);
			changed(s);
		}
		break;
	}
	case ModeNaturalize:
	{
		ImGui::SeparatorText("Naturalize");
		hint("Takes the picture as a rough statement of where the ground is high: brush marks are blurred away, outlines bent, ridged relief added, the result eroded.");
		Row::sliderFloat("Smoothing", &s.natSmoothing, 0.0f, 0.15f, "%.3f");
		Row::sliderFloat("Relief", &s.natDetail, 0.0f, 1.0f);
		Row::sliderFloat("Warp##nat", &s.natWarp, 0.0f, 1.0f);
		Row::sliderFloat("Erosion##nat", &s.natErosion, 0.0f, 1.0f);
		Row::inputInt("Seed##nat", &s.seed);
		if (EditorWidgets::primaryButton("Naturalize Sketch"))
		{
			pushUndo(s);
			SH::RefineParams p;
			p.seed = static_cast<uint32_t>(s.seed);
			p.smoothing = s.natSmoothing; p.detail = s.natDetail; p.warp = s.natWarp; p.erosion = s.natErosion;
			SH::refine(s.map, p, maskOf(s));
			changed(s);
		}
		hint(where);
		break;
	}
	case ModeErode:
	{
		ImGui::SeparatorText("Erode");
		hint("Water carves channels and fills hollows; gravity lets slopes that are too steep slide. Press again for more.");
		Row::sliderFloat("Water", &s.erosion, 0.0f, 1.0f);
		Row::sliderInt("Gravity Passes", &s.thermalPasses, 0, 40);
		Row::inputInt("Seed##erode", &s.seed);
		if (EditorWidgets::primaryButton("Erode")) { pushUndo(s); SH::erode(s.map, s.erosion, s.thermalPasses, static_cast<uint32_t>(s.seed), maskOf(s)); changed(s); }
		hint(where);
		break;
	}
	case ModeFilters:
	{
		ImGui::SeparatorText("Soften");
		Row::dragFloat("Blur Radius", &s.blurRadius, 0.25f, 1.0f, 128.0f, "%.0f px");
		if (EditorWidgets::button("Blur")) { pushUndo(s); SH::blur(s.map, s.blurRadius, maskOf(s)); changed(s); }
		ImGui::SeparatorText("Terraces");
		Row::sliderInt("Terrace Steps", &s.terraceSteps, 2, 32);
		Row::sliderFloat("Terrace Sharpness", &s.terraceSharpness, 0.0f, 1.0f);
		if (EditorWidgets::button("Terrace")) { pushUndo(s); SH::terrace(s.map, s.terraceSteps, s.terraceSharpness, maskOf(s)); changed(s); }
		ImGui::SeparatorText("Border");
		Row::sliderFloat("Edge Fade Width", &s.fadeWidth, 0.02f, 1.0f);
		if (EditorWidgets::button("Fade Edges")) { pushUndo(s); SH::fadeEdges(s.map, s.fadeWidth); changed(s); }
		ImGui::SeparatorText("Heights");
		if (EditorWidgets::button("Normalize")) { pushUndo(s); SH::normalize(s.map); changed(s); }
		ImGui::SameLine();
		if (EditorWidgets::button("Invert")) { pushUndo(s); SH::invert(s.map, maskOf(s)); changed(s); }
		hint(where);
		break;
	}
	default:
	{
		ImGui::SeparatorText("Select");
		hint("Paint the region Generate, Naturalize, Erode and the filters are limited to. What is outside is dimmed blue. Feather the edge and what is made inside blends into the rest.");
		Row::combo("Brush", &s.selectBrush, kSelectBrushes, IM_ARRAYSIZE(kSelectBrushes));
		Row::dragFloat("Radius", &s.radius, 0.5f, 1.0f, 512.0f, "%.0f px");
		ImGui::BeginDisabled(!selection);
		if (EditorWidgets::button("Feather Selection")) { SH::blur(s.mask, std::max(2.0f, s.radius * 0.5f)); s.textureDirty = true; }
		if (EditorWidgets::button("Clear Selection")) { s.mask = {}; s.textureDirty = true; }
		ImGui::SameLine();
		if (EditorWidgets::button("Invert Selection")) { for (float& v : s.mask.v) v = 1.0f - v; s.textureDirty = true; }
		ImGui::EndDisabled();
		break;
	}
	}
	if (!s.status.empty())
	{
		ImGui::Spacing();
		hint(s.status.c_str());
	}
}

// ── The picture ──
void drawCanvas(State& s, const ImVec2& avail)
{
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

	// Which tool the mouse holds: the Sketch mode's, the Select mode's brush, none in the modes that work by a button.
	const int tool = s.mode == ModeSketch ? s.sketchTool : s.mode == ModeSelect ? (s.selectBrush == 0 ? Select : Deselect) : NoTool;
	if (tool == NoTool) { s.dragging = false; return; }

	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const ImVec2 px((mouse.x - p0.x) / scale - 0.5f, (mouse.y - p0.y) / scale - 0.5f);   // the mouse in pixels of the picture
	const auto screen = [&](const ImVec2& p) { return ImVec2(p0.x + (p.x + 0.5f) * scale, p0.y + (p.y + 0.5f) * scale); };
	const bool dragTool = tool == Line || tool == Transition;   // applied when the button is let go
	const bool selectTool = tool == Select || tool == Deselect; // paints the selection, not the picture

	if (active && !s.dragging)
	{
		if (!selectTool) pushUndo(s);
		// The first Add stroke starts from nothing selected, the first Remove from everything.
		else if (!maskOf(s)) SH::fill(s.mask, s.map.w, s.map.h, tool == Select ? 0.0f : 1.0f);
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
			switch (tool)
			{
			case Smooth:   SH::smooth(s.map, x, y, s.radius, s.strength); break;
			case Flatten:  SH::flatten(s.map, x, y, s.radius, s.startHeight, s.strength * 0.5f); break;
			case Select:   SH::dab(s.mask, x, y, s.radius, 1.0f); break;
			case Deselect: SH::dab(s.mask, x, y, s.radius, -1.0f); break;
			default:       SH::dab(s.map, x, y, s.radius, tool == Lower ? -amount : amount); break;
			}
		}
		if (steps > 0) s.dragLast = px;
		if (selectTool) s.textureDirty = true;
		else changed(s);
	}
	if (s.dragging && !active)
	{
		if (tool == Line) SH::line(s.map, s.dragStart.x, s.dragStart.y, px.x, px.y, s.radius, s.startHeight, s.endHeight, s.strength);
		if (tool == Transition) SH::gradient(s.map, s.dragStart.x, s.dragStart.y, px.x, px.y, s.startHeight, s.endHeight, s.strength);
		if (dragTool) changed(s);
		s.dragging = false;
	}

	const ImU32 accent = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
	if (s.dragging && dragTool) dl->AddLine(screen(s.dragStart), screen(px), accent, 2.0f);
	if (hovered && tool != Transition) dl->AddCircle(screen(px), s.radius * scale, accent, 48);
}

// ── The test landscape ──
void drawPreview(AppContext& ctx, State& s, const ImVec2& avail)
{
	if (s.world && s.worldContent != ctx.contentManager) releasePreview(ctx, s);   // another project: its terrain belongs to no one here
	if (!s.world)
	{
		s.world = std::make_unique<HorizonWorld>();
		s.worldContent = ctx.contentManager;
		s.terrain = s.world->createEntity("Test Landscape");
		s.world->addComponent(s.terrain, TransformComponent{});
		TerrainComponent tc;
		tc.resolution = kPreviewResolution;
		tc.lodDistanceScale = 16.0f;   // one object looked at as a whole: no chunk of it is "far"
		s.world->addComponent(s.terrain, tc);

		// The ground's material: the picture as colours (StampHeightmap::groundColours), laid once over the whole terrain.
		TextureAsset tex;
		tex.type = HE::AssetType::Texture;
		tex.name = "StampHeightmapGround";
		tex.width = tex.height = 1; tex.channels = 4;
		tex.srgb = true;
		tex.data.assign(4, 0xB8);
		s.groundTexture = ctx.contentManager->registerTexture(std::move(tex));
		MaterialAsset mat;
		mat.type = HE::AssetType::Material;
		mat.name = "StampHeightmapGround";
		mat.baseColor[0] = mat.baseColor[1] = mat.baseColor[2] = s.groundBrightness;
		mat.roughness = 0.95f;
		mat.texturePaths = { "StampHeightmapGround.tex" };
		mat.textureIds = { s.groundTexture };
		s.groundMaterial = ctx.contentManager->registerMaterial(std::move(mat));
		s.world->addComponent(s.terrain, MaterialComponent{ s.groundMaterial });
		s.previewDirty = true;
	}
	// The terrain follows the picture: at once when a button made the change, ten times a second during a stroke (513 x 513
	// heights and the chunks from them are too much for every frame of one).
	if (s.previewDirty && (!s.dragging || ImGui::GetTime() - s.lastPreview > 0.1))
	{
		// The picture resampled onto the terrain's grid, in metres. Row 0 of the picture is the terrain's -Z edge, as on a stamp.
		TerrainComponent& tc = s.world->registry().get<TerrainComponent>(s.terrain);
		tc.sizeX = tc.sizeZ = s.previewSize;
		tc.resolution = kPreviewResolution;
		const int res = static_cast<int>(kPreviewResolution);
		tc.sculptHeights.resize(static_cast<size_t>(res) * res);
		for (int z = 0; z < res; ++z)
			for (int x = 0; x < res; ++x)
			{
				const float fx = static_cast<float>(x) / (res - 1) * (s.map.w - 1), fy = static_cast<float>(z) / (res - 1) * (s.map.h - 1);
				const int ix = std::min(static_cast<int>(fx), std::max(0, s.map.w - 2)), iy = std::min(static_cast<int>(fy), std::max(0, s.map.h - 2));
				const int jx = std::min(ix + 1, s.map.w - 1), jy = std::min(iy + 1, s.map.h - 1);
				const float tx = fx - ix, ty = fy - iy;
				const auto at = [&](int px, int py) { return s.map.v[static_cast<size_t>(py) * s.map.w + px]; };
				const float top = at(ix, iy) + (at(jx, iy) - at(ix, iy)) * tx, bottom = at(ix, jy) + (at(jx, jy) - at(ix, jy)) * tx;
				tc.sculptHeights[static_cast<size_t>(z) * res + x] = (top + (bottom - top) * ty) * s.previewHeight;
			}
		tc.dirty = true;
		TerrainSystem::updateTerrains(*s.world, *ctx.contentManager, ctx.renderer);
		if (TextureAsset* tex = ctx.contentManager->getTextureMutable(s.groundTexture))
		{
			SH::groundColours(s.map, s.previewSize, s.previewHeight, s.groundColours, tex->data);
			tex->width = static_cast<uint32_t>(s.map.w);
			tex->height = static_cast<uint32_t>(s.map.h);
			ctx.renderer->InvalidateTexture(s.groundTexture);
		}
		s.previewDirty = false;
		s.lastPreview = ImGui::GetTime();
	}
	if (!s.camFramed)
	{
		s.cam.focusOn(glm::vec3(0.0f, s.previewHeight * 0.3f, 0.0f), s.previewSize * 0.6f);
		s.camFramed = true;
	}

	const ImVec2 av(std::max(64.0f, avail.x), std::max(64.0f, avail.y));
	WorldPreviewEnv env;
	env.sky = true;
	env.timeOfDay = s.timeOfDay;
	env.cloudCoverage = s.clouds;
	env.grid = false;
	void* tex = ctx.renderer->RenderWorldPreview(*ctx.contentManager, *s.world, static_cast<uint32_t>(av.x), static_cast<uint32_t>(av.y),
	                                              s.cam.makeOverride(), glm::vec3(0.0f), env);
	if (!s.previewSaid)
	{
		s.previewSaid = true;
		HE_LOG_INFO(Editor, "Stamp Heightmap: test landscape %s", tex ? "drawn by the renderer's world preview" : "not available on this backend");
	}
	if (!tex)
	{
		ImGui::TextDisabled("(no 3D preview on this backend; the picture still works)");
		return;
	}
	const bool flipY = (ctx.backend == HE::RendererBackend::OpenGL);
	ImGui::Image(reinterpret_cast<ImTextureID>(tex), av, flipY ? ImVec2(0, 1) : ImVec2(0, 0), flipY ? ImVec2(1, 0) : ImVec2(1, 1));
	const bool hovered = ImGui::IsItemHovered();
	EditorCamera::Input cin;
	const bool navigating = EditorViewportNav::gather(ctx, &s, hovered, ImGui::GetIO().DeltaTime, av.y, cin);
	if (hovered && !ImGui::GetIO().WantTextInput && !navigating && ImGui::IsKeyPressed(ImGuiKey_F)) s.camFramed = false;   // F frames, as in the Scene window
	s.cam.update(cin);
}
} // namespace

void open(AppContext& ctx, const std::string& file)
{
	if (file.empty() || !ctx.contentManager) return;
	State& s = st();
	std::filesystem::path p(file);
	if (p.is_relative()) p = projectDir(ctx) / p;
	SH::Map loaded;
	std::string error;
	if (!SH::read(loaded, p.string(), error)) { s.status = error; return; }
	if (!s.map.v.empty()) pushUndo(s);   // the picture that was open can be had back
	s.map = std::move(loaded);
	s.name = p.stem().string();
	sizeChanged(s);
	changed(s);
	// Only a file of this editor's own folder is written back to by Live Preview; a picture from elsewhere is a copy until saved.
	std::error_code ec;
	const bool own = std::filesystem::equivalent(p, projectDir(ctx) / relativeFile(s), ec);
	s.boundFile = own ? relativeFile(s) : std::string();
	s.fileDirty = !own;
	s.status = "Opened " + p.filename().string();
}

bool save(AppContext& ctx)
{
	return ctx.projectLoaded && ctx.contentManager && saveFile(ctx, st());
}

void render(AppContext& ctx, const ImVec2& pos, const ImVec2& size)
{
	// Every control here is looked up as "Stamp Heightmap/<its label>".
	HE::Ed::Help::Scope helpScope("Stamp Heightmap");
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin("##StampHeightmapTab", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoSavedSettings);
	if (!ctx.projectLoaded || !ctx.world || !ctx.contentManager || !ctx.renderer)
	{
		ImGui::TextDisabled("Open a project to make a stamp heightmap.");
		ImGui::End();
		return;
	}
	State& s = st();
	if (s.map.v.empty())
	{
		SH::fill(s.map, 256, 256, 0.0f);
		// HE_DUMP_STAMPHEIGHTMAP: a headless run opens the tab with a mountain in it, so the picture and the test landscape have
		// something to show, and writes it to Heightmaps/Stamp.pgm for whoever drives the run to look at.
		if (std::getenv("HE_DUMP_STAMPHEIGHTMAP"))
		{
			SH::generate(s.map, {});
			SH::refine(s.map, {});
			saveFile(ctx, s);
		}
	}

	drawBar(ctx, s);

	ImGui::BeginChild("##stampHeightmapControls", ImVec2(300.0f, 0.0f));
	drawModeOptions(s);
	ImGui::EndChild();
	ImGui::SameLine();

	// The picture, then the test landscape, half the remaining width each.
	const ImVec2 rest = ImGui::GetContentRegionAvail();
	const float half = std::max(64.0f, (rest.x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
	ImGui::BeginChild("##stampHeightmapCanvasArea", ImVec2(half, 0.0f));
	uploadTexture(ctx, s);
	drawCanvas(s, ImGui::GetContentRegionAvail());
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("##stampHeightmapPreviewArea", ImVec2(half, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	drawPreview(ctx, s, ImGui::GetContentRegionAvail());
	ImGui::EndChild();

	// Live: the file follows the changes (during a stroke, at most seven times a second), and the stamps that read it with it.
	if (s.live && s.fileDirty && s.boundFile == relativeFile(s) && (!s.dragging || ImGui::GetTime() - s.lastWrite > 0.15)) saveFile(ctx, s);
	ImGui::End();
}

#else
void render(AppContext&, const ImVec2&, const ImVec2&) {}
void open(AppContext&, const std::string&) {}
bool save(AppContext&) { return false; }
#endif
}
