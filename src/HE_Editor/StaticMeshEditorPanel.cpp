#include "StaticMeshEditorPanel.h"
#include "EditorToolbar.h"   // shared toolbar strip
#include <cstdint>
#include "EditorApplication.h" // AppContext
#include "EditorAssetTypeCache.h" // shared, invalidatable path → AssetType sniff
#include "EditorPanelState.h" // shared per-tab state map + lazy asset open
#include "EditorInput.h"            // pointer-device grammar (trackpad swipe vs mouse wheel)
#include "EditorHelp.h"             // "Mesh Viewer/<label>" scope for the tooltips
#include "EditorWidgets.h"          // WrapText — text wraps at the pane edge, never runs off it
#include "EditorCamera.h"           // the Scene window's camera, one per tab
#include "EditorViewportNav.h"      // …and its navigation grammar, shared
#include "MeshMaterialSlots.h"      // the material-slot list, shared with the skeletal tab
#include "AutoLodDialog.h"          // Generate LODs from the Detail levels section
#include "AutoLod.h"                // merging a mesh's old _LOD<n> files into it
#include <ContentManager/ContentManager.h>
#include <Diagnostics/Log.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <Types/Enums.h>
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace StaticMeshEditorPanel
{

namespace
{

struct UvStats
{
	bool   haveUVs      = false;
	bool   allZero      = true;   // no real unwrap (every UV at the origin)
	size_t outsideTile  = 0;      // verts outside the 0..1 tile → the texture repeats
	float  minU = 0, maxU = 0, minV = 0, maxV = 0;
};

struct State
{
	bool        loaded = false;
	std::string relPath;
	std::string name;
	HE::UUID    meshId;

	// UV view: pan/zoom over the 0..1 tile. 1.0 = the tile exactly fills the
	// square canvas.
	float   uvZoom = 1.0f;
	ImVec2  uvPan{ 0.0f, 0.0f };
	bool    showTileGrid = true;
	bool    flipV        = false;   // preview the other V convention at a glance

	UvStats stats;
	bool    statsDone = false;

	// Which detail level the tab shows: 0 = the mesh, n = its n-th level (the
	// levels live inside the asset, StaticMeshAsset::lods). Views, stats and the
	// preview entity follow it; the slots and the bake stay the mesh's own.
	int     lod      = 0;
	int     statsLod = -1;       // the level `stats` were computed for

	// ── 3D view ──────────────────────────────────────────────────────────────
	// A viewer for a mesh has to answer "how does this catch light", which a UV
	// unwrap cannot and a fixed headlight answers badly. So: the real renderer,
	// over a one-entity world, under a sky whose time of day is a slider.
	bool showUv = false;     // the unwrap instead of the 3D view (toolbar)
	std::unique_ptr<HorizonWorld> world;   // one entity, one MeshComponent
	EditorCamera cam;
	bool         camFramed = false;
	float        timeOfDay = 0.32f;        // mid-morning: a raking light, so form reads
	// Sky or studio. The sky answers "how does this read outdoors at this hour";
	// the studio light answers "what is the SHAPE" — no colour cast, no time of
	// day, the same fixed key light every thumbnail is rendered under. Both are
	// real questions about a mesh, so neither is the only option.
	bool         useSky = true;
	// Ground + grid + origin marker. On by default (scale is hard to read against
	// nothing), off for a clean look at the silhouette.
	bool         showGrid3d = true;

	// The material slots' undo stack + missing-path cache. The one thing this
	// tab EDITS: a slot change is written into the asset and saved at once.
	HE::Ed::MeshMaterialSlots::Session slots;
};

AssetPanelState<State> s_states;

// Interleaved (cooked) assets keep uv in floats 6/7 of each 8-float vertex; loose
// assets keep a separate uvs array. Returns false when the mesh has no UVs.
bool readUV(const StaticMeshAsset& m, size_t vertex, float& u, float& v)
{
	if (m.cooked)
	{
		const size_t base = vertex * 8;
		if (base + 7 >= m.interleaved.size()) return false;
		u = m.interleaved[base + 6];
		v = m.interleaved[base + 7];
		return true;
	}
	if (vertex * 2 + 1 >= m.uvs.size()) return false;
	u = m.uvs[vertex * 2 + 0];
	v = m.uvs[vertex * 2 + 1];
	return true;
}

size_t vertexCount(const StaticMeshAsset& m)
{
	return m.cooked ? static_cast<size_t>(m.vertexCount) : m.vertices.size() / 3;
}

UvStats computeStats(const StaticMeshAsset& m)
{
	UvStats s;
	const size_t n = vertexCount(m);
	bool first = true;
	for (size_t i = 0; i < n; ++i)
	{
		float u, v;
		if (!readUV(m, i, u, v)) continue;
		s.haveUVs = true;
		if (u != 0.0f || v != 0.0f) s.allZero = false;
		if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) ++s.outsideTile;
		if (first) { s.minU = s.maxU = u; s.minV = s.maxV = v; first = false; }
		else
		{
			s.minU = std::min(s.minU, u); s.maxU = std::max(s.maxU, u);
			s.minV = std::min(s.minV, v); s.maxV = std::max(s.maxV, v);
		}
	}
	if (!s.haveUVs) s.allZero = false;
	return s;
}

// Line segments the UV view will draw before it starts dropping triangles. A
// dense mesh can hold hundreds of thousands of edges and ImGui pushes every one
// through the CPU — the cap keeps the tab responsive, and the caller SAYS when
// it truncated rather than quietly showing a partial unwrap.
constexpr size_t kMaxUvSegments = 120000;

void drawUvView(AppContext& ctx, const StaticMeshAsset& mesh, State& st, const ImVec2& size)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	// The canvas fills the whole pane; only the 0..1 TILE is sized off the short
	// side. A square canvas left a dead strip beside it in any non-square pane —
	// no drawing, no pan/zoom input, looking simply broken.
	const ImVec2 canvasSize(std::max(64.0f, size.x), std::max(64.0f, size.y));
	const float  side = std::min(canvasSize.x, canvasSize.y);

	ImGui::InvisibleButton("##uvcanvas", canvasSize,
		ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
		ImGuiButtonFlags_MouseButtonRight);
	const bool hovered = ImGui::IsItemHovered();

	// Pan with any button's drag (RMB steers the main viewport, so that muscle
	// memory lands here too), zoom on the wheel around the cursor.
	if (ImGui::IsItemActive() &&
	    (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
	     ImGui::IsMouseDragging(ImGuiMouseButton_Right)))
	{
		const ImVec2 d = ImGui::GetIO().MouseDelta;
		st.uvPan.x += d.x; st.uvPan.y += d.y;
	}
	ImGuiIO& io = ImGui::GetIO();
	if (hovered && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f))
	{
		// Trackpad grammar: the two-finger SWIPE pans (no held press needed —
		// exactly the gesture a pad is comfortable with), zoom moves behind
		// Cmd/Ctrl+scroll. Mouse grammar: wheel zooms, exactly as before.
		// Modifier first, so a zoom can never fall through into a pan.
		const bool zoomMod = io.KeyCtrl || io.KeySuper;
		if (EditorInput::trackpadPointer(ctx) && !zoomMod)
		{
			st.uvPan.x += io.MouseWheelH * 16.0f;
			st.uvPan.y += io.MouseWheel  * 16.0f;
		}
		else if (io.MouseWheel != 0.0f)
		{
			const float prev = st.uvZoom;
			st.uvZoom = std::clamp(st.uvZoom * (1.0f + io.MouseWheel * 0.12f), 0.15f, 40.0f);
			// Keep the point under the cursor put.
			const ImVec2 m = ImGui::GetMousePos();
			const float k = st.uvZoom / prev;
			st.uvPan.x = m.x - origin.x - (m.x - origin.x - st.uvPan.x) * k;
			st.uvPan.y = m.y - origin.y - (m.y - origin.y - st.uvPan.y) * k;
		}
	}

	const float tile = side * st.uvZoom;
	auto toScreen = [&](float u, float v) {
		// V grows UPWARD in the engine's GL-style convention, so the tile's v=0
		// edge is drawn at the BOTTOM — otherwise every unwrap would look
		// vertically mirrored against the actual texture.
		const float vv = st.flipV ? v : (1.0f - v);
		return ImVec2(origin.x + st.uvPan.x + u  * tile,
		              origin.y + st.uvPan.y + vv * tile);
	};

	dl->PushClipRect(origin, ImVec2(origin.x + canvasSize.x, origin.y + canvasSize.y), true);
	dl->AddRectFilled(origin, ImVec2(origin.x + canvasSize.x, origin.y + canvasSize.y),
	                  IM_COL32(24, 24, 28, 255));

	// The 0..1 tile + a 4x4 sub-grid, so overflow past the tile is obvious.
	if (st.showTileGrid)
	{
		const ImVec2 tl = toScreen(0.0f, 1.0f), br = toScreen(1.0f, 0.0f);
		dl->AddRectFilled(tl, br, IM_COL32(255, 255, 255, 10));
		for (int i = 1; i < 4; ++i)
		{
			const float f = static_cast<float>(i) / 4.0f;
			dl->AddLine(toScreen(f, 0.0f), toScreen(f, 1.0f), IM_COL32(255, 255, 255, 22));
			dl->AddLine(toScreen(0.0f, f), toScreen(1.0f, f), IM_COL32(255, 255, 255, 22));
		}
		dl->AddRect(tl, br, IM_COL32(255, 210, 110, 160));
	}

	// UV wireframe: one line per triangle edge.
	size_t drawn = 0;
	bool   truncated = false;
	const ImU32 edge = IM_COL32(120, 200, 255, 190);
	for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
	{
		if (drawn + 3 > kMaxUvSegments) { truncated = true; break; }
		float u[3], v[3];
		bool ok = true;
		for (int k = 0; k < 3; ++k)
			if (!readUV(mesh, mesh.indices[t + k], u[k], v[k])) { ok = false; break; }
		if (!ok) continue;
		const ImVec2 p0 = toScreen(u[0], v[0]);
		const ImVec2 p1 = toScreen(u[1], v[1]);
		const ImVec2 p2 = toScreen(u[2], v[2]);
		dl->AddLine(p0, p1, edge);
		dl->AddLine(p1, p2, edge);
		dl->AddLine(p2, p0, edge);
		drawn += 3;
	}
	dl->PopClipRect();

	if (truncated)
		ImGui::TextDisabled("Showing the first %zu edges of %zu — zoom is unaffected, "
		                    "the rest are omitted for responsiveness.",
		                    drawn, mesh.indices.size());
}

// ── The 3D view ─────────────────────────────────────────────────────────────
// The mesh as the engine actually draws it, in a world of its own: one entity
// with this mesh on it, handed to the renderer's world-preview path. That is
// what buys the sky, the ground, the grid and the Scene window's navigation for
// the price of building a one-entity world.
void ensurePreviewWorld(State& st)
{
	if (st.world) return;
	st.world = std::make_unique<HorizonWorld>();
	const Entity e = st.world->createEntity(st.name.empty() ? "Mesh" : st.name);
	st.world->addComponent(e, TransformComponent{});
	MeshComponent mc;
	mc.meshAssetId = st.meshId;
	st.world->addComponent(e, mc);
}

void drawMeshView(AppContext& ctx, const StaticMeshAsset& mesh, State& st, const ImVec2& size)
{
	if (!ctx.renderer || !ctx.contentManager)
	{
		ImGui::TextDisabled("(no renderer)");
		return;
	}
	ensurePreviewWorld(st);
	if (!st.world) return;

	// Framed on the mesh's own bounds the first time — the same numbers the
	// stats pane prints, so what you read on the left is what you are looking at.
	if (!st.camFramed)
	{
		const glm::vec3 lo(mesh.boundsMin[0], mesh.boundsMin[1], mesh.boundsMin[2]);
		const glm::vec3 hi(mesh.boundsMax[0], mesh.boundsMax[1], mesh.boundsMax[2]);
		const glm::vec3 c = (lo + hi) * 0.5f;
		const float     r = std::max(0.25f, glm::length(hi - lo) * 0.5f);
		st.cam.focusOn(c, r);
		st.camFramed = true;
	}

	const ImVec2 av(std::max(64.0f, size.x), std::max(64.0f, size.y));
	const ImVec2 org = ImGui::GetCursorScreenPos();

	WorldPreviewEnv env;
	env.sky       = st.useSky;
	env.timeOfDay = st.timeOfDay;
	env.grid      = st.showGrid3d;
	void* tex = ctx.renderer->RenderWorldPreview(*ctx.contentManager, *st.world,
		static_cast<uint32_t>(av.x), static_cast<uint32_t>(av.y),
		st.cam.makeOverride(), glm::vec3(0.0f), env);
	if (!tex)
	{
		ImGui::TextDisabled("(no 3D preview on this backend — the UV view still works)");
		return;
	}

	const bool flipY = (ctx.backend == HE::RendererBackend::OpenGL);
	ImGui::Image(reinterpret_cast<ImTextureID>(tex), av,
		flipY ? ImVec2(0, 1) : ImVec2(0, 0), flipY ? ImVec2(1, 0) : ImVec2(1, 1));
	// Hover off the IMAGE, with nothing laid over it: an item covering the
	// viewport is what stopped the class editor's gizmo from being grabbed, and
	// the rule is worth keeping even where there is no gizmo.
	const bool hovered = ImGui::IsItemHovered();

	EditorCamera::Input cin;
	// Owner = this tab's state, so the editor's per-frame "the scene viewport is
	// not drawn, drop its capture" guard cannot end a look that started here.
	const bool navigating = EditorViewportNav::gather(
		ctx, &st, hovered, ImGui::GetIO().DeltaTime, av.y, cin);
	if (hovered && !ImGui::GetIO().WantTextInput && !navigating &&
	    ImGui::IsKeyPressed(ImGuiKey_F))
		st.camFramed = false;   // F re-frames, the same key as in the Scene window
	st.cam.update(cin);
	(void)org;
}

// ── Material slots ──────────────────────────────────────────────────────────
// Writes an edited slot table into the asset and saves it. Fetched BY ID here,
// never handed a pointer: the picker that produced the edit may have loaded a
// material, and a load moves every mesh pointer taken before it.
void commitSlots(AppContext& ctx, State& st, const HE::Ed::MeshMaterialSlots::Table& table)
{
	if (!ctx.contentManager) return;
	StaticMeshAsset* mesh = ctx.contentManager->getStaticMeshMutable(st.meshId);
	if (!mesh) return;
	HE::Ed::MeshMaterialSlots::applyTo(table, *mesh);
	// The GPU copy keeps the texture of the material it was uploaded with, and
	// this tab's preview draws that texture. The save no longer comes back as a
	// hot reload (which used to invalidate the mesh), so drop the copy here.
	if (ctx.renderer) ctx.renderer->InvalidateMesh(st.meshId);
	if (ctx.contentManager->saveAsset(*mesh))
		HE_LOG_INFO(Editor, "%s", ("StaticMeshEditor: saved material slots of '" + st.relPath + "'").c_str());
	else
		HE_LOG_WARN(Editor, "%s", ("StaticMeshEditor: could not save '" + st.relPath + "'").c_str());
}

// The export-bake switches go into the mesh asset and are saved the moment one
// flips, like a material slot. Nothing is invalidated: the preview never reads them.
void commitBake(AppContext& ctx, State& st, const MeshBakeSettings& bake)
{
	if (!ctx.contentManager) return;
	StaticMeshAsset* mesh = ctx.contentManager->getStaticMeshMutable(st.meshId);
	if (!mesh) return;
	mesh->bake = bake;
	if (ctx.contentManager->saveAsset(*mesh))
		HE_LOG_INFO(Editor, "%s", ("StaticMeshEditor: saved export bake switches of '" + st.relPath + "'").c_str());
	else
		HE_LOG_WARN(Editor, "%s", ("StaticMeshEditor: could not save '" + st.relPath + "'").c_str());
}

// The picker for one slot, as the panels draw it. undo = false: the world's undo
// system snapshots entities, and a mesh asset's slot is not one — the tab's own
// Session is what Cmd+Z steps through here.
EditorWidgets::SlotAction drawSlotWidget(AppContext& ctx, size_t index, HE::UUID& target,
                                         const char* emptyText)
{
	char idSuffix[24];
	std::snprintf(idSuffix, sizeof(idSuffix), "smslot%zu", index);
	return EditorWidgets::assetDropSlot(ctx, nullptr, target, HE::AssetType::Material, idSuffix,
	                                    emptyText, "material", /*showClear=*/true, /*undo=*/false);
}

} // namespace

bool isStaticMeshAsset(const std::string& path)
{
	return EditorAssetTypeCache::is(path, HE::AssetType::StaticMesh);
}

void forget(const std::string& assetPath) { s_states.forget(assetPath); }

void render(AppContext& ctx, const std::string& assetPath, const ImVec2& pos, const ImVec2& size)
{
	State& st = s_states[assetPath];
	HE::Ed::Help::Scope helpScope("Mesh Viewer");
	if (!st.loaded && ctx.contentManager)
	{
		st.meshId = openPanelAsset(ctx, assetPath, st.name, st.relPath);
		st.loaded = true;
	}

	// A REAL host window pinned to the tab area, not a bare BeginChild: with no
	// window open, every ImGui call lands in the implicit "Debug" window — which
	// has a title bar and is user-movable, so the whole tab appeared inside a
	// draggable floating window. Same setup as ScriptEditorPanel.
	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(size, ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(0.0f, 0.0f));
	ImGui::Begin("##StaticMeshEditor", nullptr,
		ImGuiWindowFlags_NoTitleBar         | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove             | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoScrollbar        | ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoSavedSettings    | ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoDocking);
	ImGui::PopStyleVar(2);

	const StaticMeshAsset* mesh = ctx.contentManager
		? ctx.contentManager->getStaticMesh(st.meshId) : nullptr;
	if (!mesh)
	{
		// Almost everything this tab writes is longer than the room it gets — a
		// content path, a material path, a mesh name an importer lifted out of a
		// DCC file — and this line is the worst of them. Unwrapped, ImGui runs it
		// straight past the right edge and cuts it there, so "Could not load
		// 'Characters/Hero/Meshes/body.hasset' as a static mesh" arrives as
		// "Could not load 'Characters/Her": the half that names the file, which
		// is the only half worth reading, silently gone. The wrap position lives
		// on the window, so it has to be pushed inside each one — hence a scope
		// here and one in each of the two panes below, and none around the
		// toolbar, which paints its cells straight into the draw list and never
		// consults the wrap position at all.
		{
			EditorWidgets::WrapText wrap;
			ImGui::TextDisabled("Could not load '%s' as a static mesh.", st.name.c_str());
		}
		ImGui::End();
		return;
	}
	// The level on show: the mesh itself, or one of the levels inside it.
	st.lod = std::clamp(st.lod, 0, static_cast<int>(mesh->lods.size()));
	const HE::UUID shownId = st.lod > 0 ? mesh->lods[static_cast<size_t>(st.lod - 1)].id : st.meshId;
	const StaticMeshAsset* shown = ctx.contentManager->getStaticMesh(shownId);
	if (!shown) { shown = mesh; st.lod = 0; }
	if (!st.statsDone || st.statsLod != st.lod)
	{
		st.stats = computeStats(*shown); st.statsDone = true; st.statsLod = st.lod;
	}
	if (st.world)
		for (auto [e, mc] : st.world->registry().view<MeshComponent>().each())
			mc.meshAssetId = shownId;

	// ── Toolbar ──────────────────────────────────────────────────────────────
	// No Save on the strip: the one edit this tab makes (a material slot, in the
	// left pane) is saved the moment it is made. What the strip carries is what
	// is open and the two switches that change how the unwrap reads.
	{
		namespace T = EditorToolbar;
		T::Bar bar;
		T::assetHeader(bar, assetPath, false);

		// Which of the two views the right pane holds. The 3D view is the one you
		// open a mesh to see; the unwrap is what you switch to when the question
		// is about texturing.
		// Two of the cells carry a label, so the width is measured, not counted:
		// counting them as icon cells pushed the last one off the right edge.
		bar.rightGroup(st.showUv ? bar.labelGroupWidth({ "3D", "UV", nullptr, nullptr, nullptr })
		                         : bar.labelGroupWidth({ "3D", "UV", nullptr, nullptr }));
		if (bar.item("##sm3d", T::iconLayers, "3D", !st.showUv, true,
		             "The mesh in a lit world"))
			st.showUv = false;
		if (bar.item("##smuv", T::iconGrid, "UV", st.showUv, true,
		             "The unwrap over the 0..1 tile"))
			st.showUv = true;
		if (st.showUv)
		{
			if (bar.item("##smgrid", T::iconGrid, nullptr, st.showTileGrid, true,
			             "Show the 0..1 tile grid"))
			{
				st.showTileGrid = !st.showTileGrid;
			}
			if (bar.item("##smflip", T::iconFlip, nullptr, st.flipV, true,
			             "Flip V.\nThe engine uses a GL-style bottom-left UV origin; flip to "
			             "preview\nhow the unwrap reads under the other convention."))
			{
				st.flipV = !st.flipV;
			}
		}
		else if (bar.item("##sm3dgrid", T::iconGrid, nullptr, st.showGrid3d, true,
		                  "Show the ground and its grid"))
		{
			st.showGrid3d = !st.showGrid3d;
		}
		if (bar.item("##smfit", T::iconFit, nullptr, false, true,
		             st.showUv ? "Reset the UV view" : "Frame the mesh (F)"))
		{
			if (st.showUv) { st.uvZoom = 1.0f; st.uvPan = ImVec2(0.0f, 0.0f); }
			else           { st.camFramed = false; }
		}
		bar.endGroup();

		// The detail levels inside the asset, one cell each — what the runtime swaps
		// by distance, shown one at a time here. After the view group, so it sits
		// left of it (right groups stack leftwards) and the view switch stays where
		// every asset tab has it.
		if (!mesh->lods.empty())
		{
			std::vector<std::string> names;
			for (size_t i = 0; i <= mesh->lods.size(); ++i) names.push_back("LOD " + std::to_string(i));
			float w = T::kWellPad * 2.0f + T::kSegGap * static_cast<float>(names.size() - 1);
			for (const std::string& n : names) w += T::cellWidth(bar.metrics(), n.c_str());
			bar.rightGroup(w);
			for (size_t i = 0; i < names.size(); ++i)
			{
				const std::string id = "##smlod" + std::to_string(i);
				const uint32_t tris = i == 0 ? static_cast<uint32_t>(mesh->indices.size() / 3)
				                             : mesh->lods[i - 1].triangles;
				const std::string tip = (i == 0 ? std::string("The mesh itself") : "Detail level " + std::to_string(i))
				                      + ", " + std::to_string(tris) + " triangles";
				if (bar.item(id.c_str(), nullptr, names[i].c_str(), st.lod == static_cast<int>(i), true,
				             tip.c_str(), "Mesh Viewer/LOD"))
					st.lod = static_cast<int>(i);
			}
			bar.endGroup();
		}
	}

	// ── Left: stats + UV health ──────────────────────────────────────────────
	ImGui::BeginChild("##smInfo", ImVec2(280.0f, 0.0f), true);
	{
		// 280 px of column with a material path and four full sentences in it.
		EditorWidgets::WrapText wrap;

		const size_t verts = vertexCount(*shown);
		ImGui::SeparatorText(st.lod > 0 ? ("Geometry, LOD " + std::to_string(st.lod)).c_str() : "Geometry");
		ImGui::Text("Vertices  %zu", verts);
		ImGui::Text("Triangles %zu", shown->indices.size() / 3);
		ImGui::Text("Format    %s", mesh->cooked ? "cooked (interleaved)" : "loose (SoA)");
		ImGui::Text("Bounds X  %.3f .. %.3f", mesh->boundsMin[0], mesh->boundsMax[0]);
		ImGui::Text("       Y  %.3f .. %.3f", mesh->boundsMin[1], mesh->boundsMax[1]);
		ImGui::Text("       Z  %.3f .. %.3f", mesh->boundsMin[2], mesh->boundsMax[2]);

		// ── Material slots ───────────────────────────────────────────────
		// One row per section, each a material picker — the only thing in
		// this tab that edits the asset. Drawn on a COPY of the table: the
		// picker can load a material, and a load moves `mesh`, which is why it
		// is re-fetched right after. Nothing else in this pane reads it; the
		// right pane does, and gives up below if it is gone.
		{
			namespace MS = HE::Ed::MeshMaterialSlots;
			MS::Table table = MS::tableOf(*mesh);
			const bool changed = MS::draw(ctx.contentManager, table, st.slots,
				[&](size_t i, HE::UUID& target, const char* emptyText) {
					return drawSlotWidget(ctx, i, target, emptyText);
				});
			if (changed) commitSlots(ctx, st, table);
			mesh = ctx.contentManager->getStaticMesh(st.meshId);
		}

		// ── Export bake ──────────────────────────────────────────────────
		// Which texture bakes the exporter may apply to this mesh. Saved at once
		// like a slot change; the editor's own view never uses them.
		ImGui::SeparatorText("Export bake");
		{
			MeshBakeSettings bake = mesh->bake;
			bool touched = false;
			touched |= EditorWidgets::checkbox("Vertex colors",   &bake.vertexColors);
			touched |= EditorWidgets::checkbox("Merge textures",  &bake.mergeTextures);
			touched |= EditorWidgets::checkbox("One UV texture",  &bake.uvTexture);
			if (touched) commitBake(ctx, st, bake);
			mesh = ctx.contentManager->getStaticMesh(st.meshId);
			ImGui::TextDisabled("Applies to simple texture-only\nmaterials at export.");
		}

		// ── Detail levels ────────────────────────────────────────────────
		// Kept inside this asset; the toolbar switches the view between them.
		ImGui::SeparatorText("Detail levels");
		if (mesh->lods.empty())
			EditorWidgets::hint("None. Generate LODs makes simpler versions inside this asset; "
			                    "every entity that shows the mesh then switches them by distance.");
		else
			for (size_t i = 0; i < mesh->lods.size(); ++i)
				ImGui::Text("LOD %zu  %u tris  %.3f m off", i + 1, mesh->lods[i].triangles, mesh->lods[i].worldError);
		if (EditorWidgets::button("Generate LODs...##sm"))
		{
			AutoLodDialog::Request req;
			req.kind = AutoLodDialog::Kind::MeshAsset;
			req.mesh = st.meshId;
			AutoLodDialog::open(req);
		}
		// A mesh from before the levels moved into the asset: its levels are files
		// beside it. One press rebuilds them inside and removes the files; the ids
		// stay, so every scene and foliage layer that names a level still finds it.
		if (mesh->lods.empty() && !mesh->path.empty() && !st.relPath.empty())
		{
			const std::string root = ctx.contentManager->contentRoot();
			const std::vector<std::string> old = AutoLod::findLodSidecars(root, mesh->path);
			if (!old.empty())
			{
				EditorWidgets::hint("%zu level file(s) beside this mesh (%s ...) are from an older "
				                    "version of the engine.", old.size(), old.front().c_str());
				if (EditorWidgets::button("Merge Level Files"))
				{
					AutoLod::Settings set;
					set.ratios = AutoLod::ratiosFor(static_cast<int>(old.size()), 0.5f, 0.1f);
					const StaticMeshAsset src = *mesh;   // a copy: the refresh below moves the pool
					const AutoLod::Chain chain = AutoLod::generate(src, root, set);
					if (chain.error.empty())
					{
						ctx.contentManager->refreshMeshLods(st.meshId);
						ctx.contentRefreshPending = true;
					}
					else
						HE_LOG_WARN(Editor, "%s", ("Merge Level Files: " + chain.error).c_str());
					mesh = ctx.contentManager->getStaticMesh(st.meshId);
				}
			}
		}
		// Re-taken like `mesh`: a merge registers levels, which can move the pool.
		shown = ctx.contentManager->getStaticMesh(shownId);
		if (!shown) shown = mesh;

		ImGui::SeparatorText("UVs");
		if (!st.stats.haveUVs)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "No texture coordinates.");
			ImGui::TextWrapped("Every texture sample collapses to one texel. Loose meshes get "
			                   "box-projected UVs on import; this one has none at all.");
		}
		else if (st.stats.allZero)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "All UVs are (0,0).");
			ImGui::TextWrapped("The mesh carries a UV set but it is unwrapped to a single point — "
			                   "textures will show one flat colour.");
		}
		else
		{
			ImGui::Text("Range U   %.3f .. %.3f", st.stats.minU, st.stats.maxU);
			ImGui::Text("Range V   %.3f .. %.3f", st.stats.minV, st.stats.maxV);
			if (st.stats.outsideTile > 0)
			{
				ImGui::TextColored(ImVec4(0.9f, 0.85f, 0.4f, 1.0f),
				                   "%zu verts outside the 0..1 tile", st.stats.outsideTile);
				ImGui::TextWrapped("That is fine for a tiling texture — the pattern repeats — "
				                   "but not for an atlas.");
			}
			else
				ImGui::TextDisabled("Fully inside the 0..1 tile.");
		}

		// Tile grid, Flip V and Reset live on the toolbar now — one place for the
		// controls, and this pane keeps the readouts it alone can give.
		ImGui::SeparatorText("View");
		if (st.showUv)
		{
			ImGui::Text("Zoom %.2fx", st.uvZoom);
			ImGui::TextDisabled(EditorInput::trackpadPointer(ctx)
				? "Drag or two-finger swipe to pan,\nCmd/Ctrl+scroll to zoom."
				: "Drag to pan, wheel to zoom.");
		}
		else
		{
			// Sky or studio: two different questions about a mesh. Outdoors at a
			// given hour, or the shape alone under a fixed key light with no
			// colour cast — the light every thumbnail is rendered under.
			if (ImGui::RadioButton("Sky", st.useSky)) st.useSky = true;
			EditorWidgets::helpForLabel("Sky");
			ImGui::SameLine();
			if (ImGui::RadioButton("Studio", !st.useSky)) st.useSky = false;
			EditorWidgets::helpForLabel("Studio");
			// A labelled checkbox, not just the toolbar icon: an unlabelled grid
			// glyph among four others is not something anyone finds.
			EditorWidgets::checkbox("Ground grid", &st.showGrid3d);

			// One slider, because one number is what actually changes the answer:
			// where the sun is. Everything else about the sky follows from it.
			// Shown as a clock inside the slider, like the Sky panel does — "0.32"
			// says nothing, "07:40" says which light you are judging the mesh in.
			ImGui::BeginDisabled(!st.useSky);
			int minutes = static_cast<int>(st.timeOfDay * 1440.0f) % 1440;
			if (minutes < 0) minutes += 1440;
			char clock[8];
			std::snprintf(clock, sizeof(clock), "%02d:%02d", minutes / 60, minutes % 60);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##smtod", &st.timeOfDay, 0.0f, 1.0f, clock,
			                   ImGuiSliderFlags_NoRoundToFormat);
			ImGui::TextDisabled("Time of day");
			ImGui::EndDisabled();
			ImGui::Spacing();
			ImGui::TextDisabled(EditorInput::trackpadPointer(ctx)
				? "Two-finger tap to fly (WASDQE),\nAlt+drag to orbit, scroll to zoom, F to frame."
				: "Right-drag to look (WASDQE),\nAlt+drag to orbit, wheel to zoom, F to frame.");
		}
	}
	ImGui::EndChild();

	// The slot picker above may have moved the asset pool; `mesh` was taken
	// again after it, and an asset that is no longer there ends the frame here —
	// outside the pane, so its wrap scope has already been popped in order.
	if (!mesh)
	{
		ImGui::End();
		return;
	}

	// ── Right: the mesh, or its unwrap ───────────────────────────────────────
	ImGui::SameLine();
	// NoScrollWithMouse: in the 3D view the wheel is the camera's dolly, and
	// without this the child eats it first.
	ImGui::BeginChild("##smUv", ImVec2(0.0f, 0.0f), true,
		st.showUv ? 0 : (ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar));
	{
		// The unwrap itself is immune — every edge goes through the draw list at a
		// computed position, which no wrap position can reach. What this scope is
		// for is the one sentence drawUvView emits when it truncates.
		EditorWidgets::WrapText wrap;
		if (st.showUv) drawUvView  (ctx, *shown, st, ImGui::GetContentRegionAvail());
		else           drawMeshView(ctx, *shown, st, ImGui::GetContentRegionAvail());
	}
	ImGui::EndChild();

	// Cmd/Ctrl+Z over the tab steps the slot edits back (and forward again);
	// each step is written and saved like the edit it undoes. In the host
	// window, after both panes, so "hovered" covers the whole tab.
	{
		namespace MS = HE::Ed::MeshMaterialSlots;
		MS::Table restored;
		if (MS::handleUndoKeys(restored, st.slots)) commitSlots(ctx, st, restored);
	}

	ImGui::End();
}

} // namespace StaticMeshEditorPanel
