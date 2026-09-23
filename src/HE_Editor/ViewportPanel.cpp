#include "ViewportPanel.h"
#include <HorizonScene/Components/MaterialComponent.h> // a spawned mesh follows its MREF material
#include <HorizonScene/Components/PrefabInstanceComponent.h> // a dropped prefab remembers its file
#include <cstdint>
#include "EditorApplication.h"           // AppContext, EditorCamera, EditorUndo
#include "EditorInput.h"                 // pointer-device grammar (trackpad swipe vs mouse wheel)
#include "EditorViewportNav.h"           // shared orbit/pan/fly gesture grammar + look capture
#include "EditorTransformGizmo.h"        // shared move/rotate/scale gizmo
#include "EditorMarquee.h"               // which objects a drawn frame encloses
#include "ViewportPick.h"                // which object a click lands on (mesh before terrain)
#include "PreviewPick.h"                 // screenRay — the surface-snap probe's ray
#include <HorizonScene/TransformHierarchy.h>          // worldPositionOf — fresh, not a frame old
#include <HorizonScene/Components/TerrainComponent.h>      // vertex snap skips the landscape
#include <HorizonScene/Components/TerrainChunkComponent.h>
#include "TerrainTools.h"                // Landscape brush cursor + sculpt stroke
#include "CollabPresenceBar.h"           // name tags for the other people in the session
#include "McpCameraGizmos.h"             // …and for the MCP clients' screenshot cameras
#include "ViewportToolbar.h"             // the strip along the top of the Scene window
#include "ViewportViewMode.h"            // the headless HE_DUMP_VIEWMODE override on the mode push
#include "EditorWidgets.h"               // WrapText — text wraps at the pane edge, never runs off it
#include "EditorHelp.h"                  // the context menu's scope
#include "EditorTheme.h"                 // the stats overlay's accent line
#include "ViewportActions.h"             // hide / isolate / show all / group — headless, tested
#include "CameraBookmarks.h"             // the digit keys
#include "EditorShortcuts.h"             // the viewport's chords, rebindable in Preferences
#include <HorizonScene/HorizonScene.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/ScenePick.h>   // triangle-exact ray probe for drag-drop placement
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Types/Enums.h>
#include <Math/AABB.h>
#include <DebugDraw/DebugDraw.h>          // the ground grid rides the editor's debug-line channel
#include <glm/gtc/type_ptr.hpp>
#include <Diagnostics/Logger.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <ImGuizmo.h>
#endif

namespace ViewportPanel
{

// ── Show flags ──────────────────────────────────────────────────────────────
// Outside the ImGui guard on purpose: the ground grid below is pure geometry
// pushed into a debug line buffer, and the other flags are read by the debug
// block in EditorApplication and by the extractor, none of which is UI.
// Keeping the switches next to the only code that reads the grid's is what
// stops a second copy of "is the grid on" from appearing.
static ShowFlags s_showFlags;

ShowFlags& showFlags() { return s_showFlags; }

bool groundGridEnabled()           { return s_showFlags.groundGrid; }
void setGroundGridEnabled(bool on) { s_showFlags.groundGrid = on; }

const ShowFlagField* showFlagFields(int& outCount)
{
	static const ShowFlagField kFields[] = {
		{ "ViewportGroundGrid",         &ShowFlags::groundGrid,    "Ground Grid"   },   // the key the grid always had
		{ "ViewportShowSelection",      &ShowFlags::selection,     "Selection"     },
		{ "ViewportShowColliders",      &ShowFlags::colliders,     "Colliders"     },
		{ "ViewportShowJoints",         &ShowFlags::joints,        "Joints"        },
		{ "ViewportShowNavMesh",        &ShowFlags::navMesh,       "NavMesh"       },
		{ "ViewportShowEditorIcons",    &ShowFlags::editorIcons,   "Editor Icons"  },
		{ "ViewportShowGuides",         &ShowFlags::guides,        "Guides"        },
		{ "ViewportShowCollaborators",  &ShowFlags::collaborators, "Collaborators" },
		{ "ViewportShowScriptDebug",    &ShowFlags::scriptDebug,   "Script Debug"  },
		{ "ViewportShowStats",          &ShowFlags::stats,         "Stats"         },
	};
	outCount = static_cast<int>(sizeof(kFields) / sizeof(kFields[0]));
	return kFields;
}

void appendGroundGrid(const EditorCamera& cam, bool playing, DebugDrawBuffer& out)
{
	if (playing || !s_showFlags.groundGrid) return;

	const glm::vec3 eye  = cam.position();
	const float     camX = eye.x;
	const float     camZ = eye.z;

	// Cell size follows the camera's height over the grid plane. A fixed metre
	// grid is the thing that fails at both ends: from 500 m up it is a solid grey
	// smear, and it says nothing at all about the door handle you are looking at
	// from 20 cm. Snapped to a 1 / 2.5 / 5 / 10 ladder, because the grid is only
	// a scale reference if the spacing is a number somebody can count in.
	// The floor keeps a camera sitting exactly on the ground plane from asking
	// for an infinitely fine grid.
	const float height = std::max(2.0f, std::abs(eye.y));
	const float target = height * 0.12f;
	const float decade = std::pow(10.0f, std::floor(std::log10(target)));
	const float lead   = target / decade;  // 1 … 10
	const float step   = decade * (lead <= 1.0f ? 1.0f
	                             : lead <= 2.5f ? 2.5f
	                             : lead <= 5.0f ? 5.0f : 10.0f);

	// Outer edge, and the point where the minor lines give up. Both are measured
	// in cells so the grid always shows the same amount of ground no matter how
	// far up the camera is.
	const float radiusMajor = step * 40.0f;
	const float radiusMinor = radiusMajor * 0.55f;

	// LINEAR HDR values: the debug-line pass writes straight into the scene's
	// RGBA16F target and resolves through ACES + gamma, which lifts them a long
	// way (WorldPreviewGrid.h documents the same calibration for the studio
	// grid). Read them as "minor ≈ 0.38 on screen, major ≈ 0.50" — brighter than
	// the preview grid's, because this one sits over a lit sky rather than over a
	// dark studio backdrop.
	const glm::vec3 colMinor(0.115f, 0.115f, 0.125f);
	const glm::vec3 colMajor(0.210f, 0.210f, 0.225f);
	// The two axes through the world origin, in the gizmo's own red/blue: without
	// them the grid says how big things are but not which way they face.
	const glm::vec3 colAxisX(0.290f, 0.070f, 0.070f);
	const glm::vec3 colAxisZ(0.070f, 0.100f, 0.330f);

	// One grid line, clipped to a DISC around the camera and split into pieces
	// that dim toward the rim.
	//
	// A disc rather than a square because a square's corners reach 40% further
	// than its sides — which is exactly where the lines pile into a smear — and
	// its straight edge reads as a wall standing in the scene.
	//
	// The dimming is only half of the fade-out: the debug-line pass has no alpha
	// (opaque vec3 into an HDR target), so a line can be darkened but never made
	// transparent, and over a bright ground a darkened line stays a visible dark
	// streak. The half that actually does the work is the minor lines stopping at
	// `radiusMinor`, leaving a thinning major-line mesh where a full-density grid
	// would otherwise turn into a solid band at the horizon.
	auto emitLine = [&](bool alongZ, float offset, float radius, const glm::vec3& color)
	{
		// Perpendicular distance from the camera's ground point to the line; past
		// the radius it misses the disc entirely.
		const float d      = offset - (alongZ ? camX : camZ);
		const float chord2 = radius * radius - d * d;
		if (chord2 <= 0.0f) return;

		const float halfChord = std::sqrt(chord2);
		const float centre    = alongZ ? camZ : camX; // the disc's midpoint ALONG the line
		constexpr int kSegments = 12;
		for (int s = 0; s < kSegments; ++s)
		{
			const float t0 = centre - halfChord + 2.0f * halfChord * (static_cast<float>(s)     / kSegments);
			const float t1 = centre - halfChord + 2.0f * halfChord * (static_cast<float>(s + 1) / kSegments);
			const float tm = (t0 + t1) * 0.5f;
			const float r  = std::sqrt(d * d + (tm - centre) * (tm - centre));
			// Full brightness over the inner half, then an ease-out to nothing at
			// the rim.
			const float u    = std::clamp((r / radius - 0.5f) * 2.0f, 0.0f, 1.0f);
			const float fade = 1.0f - u * u;
			if (fade <= 0.02f) continue;
			const glm::vec3 a = alongZ ? glm::vec3(offset, 0.0f, t0) : glm::vec3(t0, 0.0f, offset);
			const glm::vec3 b = alongZ ? glm::vec3(offset, 0.0f, t1) : glm::vec3(t1, 0.0f, offset);
			out.line(a, b, color * fade);
		}
	};

	// Lines are anchored to WORLD coordinates, not to the camera: a grid that
	// slides along under the camera is a texture, not a ruler. Only the index
	// range follows the camera.
	auto emitFamily = [&](bool alongZ, float centreCoord, const glm::vec3& axisColor)
	{
		const int first = static_cast<int>(std::floor((centreCoord - radiusMajor) / step));
		const int last  = static_cast<int>(std::ceil ((centreCoord + radiusMajor) / step));
		for (int i = first; i <= last; ++i)
		{
			const bool axis  = (i == 0);
			const bool major = (i % 10 == 0);
			emitLine(alongZ, static_cast<float>(i) * step,
			         (axis || major) ? radiusMajor : radiusMinor,
			         axis ? axisColor : (major ? colMajor : colMinor));
		}
	};

	// The line at x = 0 runs along Z and IS the Z axis, and vice versa.
	emitFamily(/*alongZ=*/true,  camX, colAxisZ);
	emitFamily(/*alongZ=*/false, camZ, colAxisX);
}

#ifdef HE_IMGUI_ENABLED

// Last viewport RENDER resolution in framebuffer pixels (HiDPI-aware), captured when
// the viewport panel is drawn and shown in the footer beside the FPS counter.
static int s_viewportPxW = 0;
static int s_viewportPxH = 0;

// The navigation gesture grammar itself — orbit/pan/fly, the trackpad toggle and
// the relative-mouse capture — lives in EditorViewportNav, because the class
// editor's viewport has to navigate identically and a second copy would drift.

// A StaticMesh .hasset dropped from the Content Browser onto the Scene
// viewport image. Captured at the drop (the drop target must bind to the
// Image item), then processed AFTER the scene extract this frame, once the
// fresh camera view/projection are available to unproject the drop point.
static std::string s_viewportDropPath;   // absolute asset path ("" = nothing pending)
static ImVec2      s_viewportDropMouse{};// screen pos of the drop

// Manipulation state (active tool, gizmo orientation, snapping, the screen-space
// rotation ring). Edited by the toolbar and by the W/E/R shortcuts below,
// consumed by the gizmo — one owner for all three.
static ViewportToolbar::State s_tb;

// A context menu asked for this frame (right-click without a look, Menu key)
// and where to put it. Raised in the camera block, consumed after the pick
// section — the pick lambda that decides what the click landed on lives there.
static bool   s_contextMenuRequest = false;
static ImVec2 s_contextMenuAt{};

// Picking + sculpt AABB cache (keyed by mesh asset UUID)
static std::unordered_map<HE::UUID, HE::AABB> s_aabbCache;

// Local-space AABB of a mesh asset, cached. Cooked meshes (packed content) carry
// their bounds precomputed and their SoA vertex array empty, so both forms have
// to be read here — the loose editor path is the interleaved-less one.
static const HE::AABB* meshBounds(ContentManager& cm, const HE::UUID& meshId)
{
	auto it = s_aabbCache.find(meshId);
	if (it == s_aabbCache.end())
	{
		const StaticMeshAsset* mesh = cm.getStaticMesh(meshId);
		if (!mesh) return nullptr;
		HE::AABB box;
		if (mesh->cooked)
		{
			for (uint32_t i = 0; i < mesh->vertexCount; ++i)
				box.expand({ mesh->interleaved[i * 8 + 0],
				             mesh->interleaved[i * 8 + 1],
				             mesh->interleaved[i * 8 + 2] });
		}
		else
			box = HE::AABB::fromPositions(mesh->vertices.data(), mesh->vertices.size() / 3);
		it = s_aabbCache.emplace(meshId, box).first;
	}
	return it->second.isValid() ? &it->second : nullptr;
}

// What an entity without a readable mesh asset is measured as — the built-in
// fallback cube's own local box — is ViewportPick::fallbackBox(): picking, the
// marquee and the F-key framing all read it there, so clicking a thing and
// focusing it never disagree about how big it is.

// Mesh geometry for HE::ScenePick, served straight out of the content manager
// (plus the bounds cache above, so the cheap box reject costs nothing here).
// Cooked assets keep their vertices interleaved with normals and UVs and leave
// the SoA array empty, hence the stride.
static HE::ScenePick::MeshLookup meshLookup(ContentManager& cm)
{
	return [&cm](const HE::UUID& id, HE::ScenePick::MeshGeometry& out)
	{
		const StaticMeshAsset* mesh = cm.getStaticMesh(id);
		if (!mesh) return false;
		out.positions   = mesh->cooked ? mesh->interleaved.data() : mesh->vertices.data();
		out.stride      = mesh->cooked ? 8 : 3;
		out.vertexCount = mesh->cooked ? mesh->vertexCount : mesh->vertices.size() / 3;
		out.indices     = mesh->indices.data();
		out.indexCount  = mesh->indices.size();
		out.bounds      = meshBounds(cm, id);
		return true;
	};
}

// ── Focus on selection (F) ──────────────────────────────────────────────────

// Every entity under (and including) the selected one. F frames a SELECTION and
// a selection is a subtree: the root of a group carries no geometry of its own,
// so framing it alone would park the camera in front of an empty point.
static void collectSubtree(entt::registry& reg, Entity e,
                           std::unordered_set<uint32_t>& out)
{
	// The insert doubles as the recursion guard: HorizonWorld::reparentEntity
	// refuses to build a cycle, but a hand-edited .hescene never went through it.
	if (!out.insert(static_cast<uint32_t>(e)).second) return;
	if (const auto* h = reg.try_get<HierarchyComponent>(e))
		for (const Entity child : h->children)
			if (reg.valid(child)) collectSubtree(reg, child, out);
}

// The bounding sphere the F key frames, measured from the selection's REAL
// geometry. It used to come out of TransformComponent::scale, which is not a
// size at all: a 50 m building authored at scale 1 asked for a radius of 1.8 and
// the camera landed inside the walls, which reads as a broken shortcut rather
// than as a mismeasured one.
//
// The boxes come from the mesh assets (the cache picking already pays for),
// transformed by each render object's world matrix — NOT from
// RenderObject::worldBounds, which this viewport's own extractor leaves invalid
// because it runs without a ContentManager. Entities that draw nothing — a
// light, a camera, an empty group — fall back to the spread of their transforms.
// Their editor ICONS are not geometry for this purpose: the symbol is sized in
// fractions of the screen and shrinks as the camera nears, so measuring it
// would frame a light to a few centimetres and then closer with every press.
// The two boxes the framing (and the secondary viewports' selection outline)
// are built from: `geometry` is what the selection actually draws, `pivots`
// where its entities sit, drawn or not. Either may come back invalid.
static void selectionBoxes(HorizonWorld& world, ContentManager* cm, Entity sel,
                           const RenderWorld& snapshot,
                           HE::AABB& geometry, HE::AABB& pivots)
{
	auto& reg = world.registry();
	geometry = HE::AABB{};
	pivots   = HE::AABB{};
	if (!reg.valid(sel)) return;

	std::unordered_set<uint32_t> subtree;
	collectSubtree(reg, sel, subtree);

	auto expandFromObject = [&](const RenderObject& obj)
	{
		if (subtree.find(obj.entityId) == subtree.end()) return;
		if (HE::isEditorIconMaterial(obj.materialAssetId)) return;
		const HE::AABB* local = (cm && obj.meshAssetId != HE::UUID{})
		                      ? meshBounds(*cm, obj.meshAssetId) : nullptr;
		geometry.expand((local ? *local : ViewportPick::fallbackBox()).transformed(obj.transform));
	};
	for (const RenderObject& obj : snapshot.objects)               expandFromObject(obj);
	for (const SkinnedRenderObject& obj : snapshot.skinnedObjects) expandFromObject(obj);

	for (const uint32_t raw : subtree)
		if (const auto* t = reg.try_get<TransformComponent>(static_cast<Entity>(raw)))
			pivots.expand(glm::vec3(t->worldMatrix[3]));
}

static bool selectionFocusSphere(HorizonWorld& world, ContentManager* cm, Entity sel,
                                 const RenderWorld& snapshot,
                                 glm::vec3& centerOut, float& radiusOut)
{
	HE::AABB geometry;   // what the selection actually draws
	HE::AABB pivots;     // where its entities sit, drawn or not
	selectionBoxes(world, cm, sel, snapshot, geometry, pivots);

	if (!geometry.isValid() && !pivots.isValid()) return false;

	if (geometry.isValid())
	{
		centerOut = geometry.center();
		// Circumscribed sphere; the floor keeps a flat or degenerate mesh (a
		// ground plane, a single quad) from asking for a zero-distance framing.
		radiusOut = std::max(glm::length(geometry.extents()), 0.25f);
	}
	else
	{
		// Nothing here has a size, so the radius is a viewing distance rather
		// than a measurement: close enough to see the light, far enough to see
		// what it is lighting. A group of them still frames the whole spread.
		centerOut = pivots.center();
		radiusOut = glm::length(pivots.extents()) + 2.0f;
	}
	return true;
}

// Both of these are now EditorViewportNav's — the capture is a property of the
// WINDOW, not of the Scene window, and it is released from paths that draw no
// viewport at all. Kept here as the names the rest of the editor already calls.

// Identifies THIS viewport's fly-look capture. The editor calls the release
// below on every frame an asset tab is open ("the scene viewport won't run to
// release its own"), and an asset tab's viewport can be flying at that moment —
// so the release has to name whose capture it means.
static const char kSceneNavOwner = 0;
const void* navOwner() { return &kSceneNavOwner; }

// ── Right-click context menu ────────────────────────────────────────────────
// The verbs that belong to "this thing under the cursor": frame it, hide it,
// hide everything else, put it in a group, lock it, the clipboard, delete.
// Every scene edit goes through ViewportActions (headless, tested) or the same
// AppContext hooks the Edit menu and the Outliner use; this function only
// draws rows and takes the undo snapshot — ONE per operation, before it, the
// way the Outliner's context menu does.
//
// Opened by a right-click that did not turn into a fly-look (see the gesture
// in render()) or by the Menu key / Shift+F10 over the picture. As in the
// Outliner: a click on something outside the selection selects it first, a
// click on a selected thing keeps the set (the menu then acts on all of it),
// and a click on nothing leaves the selection alone — Show All and Paste are
// still worth a menu there.
namespace
{
	// The one-operation undo + prefab-override bookkeeping shared by every
	// row below: snapshot before, note every touched entity after.
	void noteEdited(AppContext& ctx, const std::vector<Entity>& touched)
	{
		if (!ctx.noteEntityEdited) return;
		for (const Entity e : touched) ctx.noteEntityEdited(e);
	}
	void snapshot(AppContext& ctx, const char* label)
	{
		if (ctx.undoSys) ctx.undoSys->snapshotNow(label);
	}

	// ── The stats overlay (Show ▸ Stats) ─────────────────────────────────────
	// The frame's counters in the viewport's top-right corner, painted into the
	// draw list like the name tags: frame rate and time, draw calls, triangles,
	// visible/total objects, and — only where the backend measures them — the
	// occlusion culler's take, the GPU time and the VRAM in use. A line that
	// the backend cannot fill is left out rather than shown as 0 or -1, which
	// would read as a measurement.
	//
	// Every number is the LAST rendered frame's, straight from the renderer
	// (IRenderer::GetFrameGpuStats — the same struct the profiler and the
	// headless dump read), so this and the profiler can never disagree.
	std::string thousands(uint32_t v)
	{
		std::string s = std::to_string(v);
		for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
		return s;
	}
	void drawStatsOverlay(AppContext& ctx, const ImVec2& rectMin, const ImVec2& rectMax)
	{
		if (!ctx.renderer) return;
		const IRenderer::FrameGpuStats st = ctx.renderer->GetFrameGpuStats();
		const ImGuiIO& io = ImGui::GetIO();

		std::vector<std::string> lines;
		char buf[96];
		std::snprintf(buf, sizeof(buf), "%.0f FPS   %.2f ms", io.Framerate,
		              io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
		lines.emplace_back(buf);
		lines.push_back("Draws      " + thousands(st.drawCalls));
		lines.push_back("Triangles  " + thousands(st.triangles));
		lines.push_back("Objects    " + thousands(st.visibleObjects) + " / " + thousands(st.totalObjects));
		if (st.occlusionCulled > 0)
			lines.push_back("Occluded   " + thousands(st.occlusionCulled));
		if (st.gpuFrameMs >= 0.0)
		{
			std::snprintf(buf, sizeof(buf), "GPU        %.2f ms", st.gpuFrameMs);
			lines.emplace_back(buf);
		}
		if (st.vramBudgetMB > 0.0)
		{
			std::snprintf(buf, sizeof(buf), "VRAM       %.0f / %.0f MB", st.vramUsedMB, st.vramBudgetMB);
			lines.emplace_back(buf);
		}

		// ── The session, while there is one (plan §8.5) ──────────────────────
		// Role, players, round trip and loss, under the frame's numbers and only
		// when a session is running — a multiplayer game's first question is
		// "am I the host and is the link healthy", and this is the corner
		// somebody is already looking at. Ping and loss come off the UDP
		// transport, so they are absent (not 0) when there is no socket
		// underneath: an in-process pair has no round trip to report, and a 0
		// there would read as a perfect one.
		if (ctx.netSessionStatus)
		{
			const int st2 = ctx.netSessionStatus();
			if (st2 != 0)
			{
				static const char* kRole[] = { "Idle", "Host", "Connecting", "Client", "Failed" };
				lines.push_back(std::string("Session    ") + kRole[(st2 >= 0 && st2 < 5) ? st2 : 0]);
				if (ctx.netPlayerCount)
				{
					std::snprintf(buf, sizeof(buf), "Players    %d", ctx.netPlayerCount());
					lines.emplace_back(buf);
				}
				const float ping = ctx.netPingMs ? ctx.netPingMs() : 0.0f;
				if (ping > 0.0f)
				{
					std::snprintf(buf, sizeof(buf), "Ping       %.0f ms", ping);
					lines.emplace_back(buf);
				}
				const float loss = ctx.netLossPercent ? ctx.netLossPercent() : 0.0f;
				if (loss > 0.0f)
				{
					std::snprintf(buf, sizeof(buf), "Loss       %.1f %%", loss);
					lines.emplace_back(buf);
				}
				// The replicated variables, and the three that cost the most
				// (plan §6.2): this is where "my variable changes every frame"
				// stops being folklore. Only when there is something to show —
				// a session whose entities declare none needs no empty heading.
				const int props = ctx.netPropertyCount ? ctx.netPropertyCount() : 0;
				if (props > 0)
				{
					std::snprintf(buf, sizeof(buf), "Properties %d entities", props);
					lines.emplace_back(buf);
					if (ctx.netCostliestProperties)
						for (const std::string& row : ctx.netCostliestProperties())
							lines.push_back(row);
				}
			}
		}

		// Sized to the widest line; a translucent card so it stays readable
		// over a bright sky without hiding much of the scene.
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float pad = 6.0f, lineH = ImGui::GetTextLineHeight();
		float w = 0.0f;
		for (const std::string& l : lines) w = std::max(w, ImGui::CalcTextSize(l.c_str()).x);
		const ImVec2 size(w + pad * 2.0f, lineH * static_cast<float>(lines.size()) + pad * 2.0f);
		const ImVec2 p0(rectMax.x - size.x - 8.0f, rectMin.y + 8.0f);
		const ImVec2 p1(p0.x + size.x, p0.y + size.y);
		if (p0.x < rectMin.x || p1.y > rectMax.y) return;   // too small a pane to hold it
		dl->AddRectFilled(p0, p1, IM_COL32(12, 11, 10, 190), 4.0f);
		dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 30), 4.0f);
		float y = p0.y + pad;
		for (size_t i = 0; i < lines.size(); ++i)
		{
			// The first line is the one people glance at; it gets the accent.
			const ImU32 col = i == 0 ? ImGui::GetColorU32(HE::Ed::Theme::AccentBright)
			                         : IM_COL32(225, 222, 215, 255);
			dl->AddText(ImVec2(p0.x + pad, y), col, lines[i].c_str());
			y += lineH;
		}
	}
}

// The actions behind the menu, also bound to keys in render(): one place for
// "what does Hide do", whichever way it was asked for. External linkage
// (declared in the header) since the main bar's Entity menu became a third
// door onto them; nothing about them changed for that.
void hideSelected(AppContext& ctx)
{
	if (!ctx.world || ctx.isPlaying || ctx.selection.empty()) return;
	snapshot(ctx, "Hide Selected");
	noteEdited(ctx, ViewportActions::hideSelected(*ctx.world, ctx.selection));
}
void isolateSelected(AppContext& ctx)
{
	if (!ctx.world || ctx.isPlaying || ctx.selection.empty()) return;
	snapshot(ctx, "Isolate Selected");
	noteEdited(ctx, ViewportActions::isolateSelected(*ctx.world, ctx.selection));
}
void showAll(AppContext& ctx)
{
	if (!ctx.world || ctx.isPlaying) return;
	snapshot(ctx, "Show All");
	noteEdited(ctx, ViewportActions::showAll(*ctx.world));
}
// Group and Ungroup rewrite the local transform of what they move (the world
// pose is kept, the local is what changes), so those are the edited entities
// — the roots going in, the children coming out.
void groupSelected(AppContext& ctx)
{
	if (!ctx.world || ctx.isPlaying || ctx.selection.empty()) return;
	const std::vector<Entity> roots = ctx.selection.roots(ctx.world->registry());
	snapshot(ctx, "Group Selected");
	if (ViewportActions::groupSelected(*ctx.world, ctx.selection) != entt::null)
		noteEdited(ctx, roots);
}
void ungroupSelected(AppContext& ctx)
{
	if (!ctx.world || ctx.isPlaying) return;
	snapshot(ctx, "Ungroup Selected");
	noteEdited(ctx, ViewportActions::ungroupSelected(*ctx.world, ctx.selection));
}
// The scene as something to land on: a triangle-exact ray probe over this
// frame's extract, minus every entity in `exclude` (the selection's own
// subtrees — a thing cannot rest on itself) and minus the editor's icon
// billboards, which are in the extract so a click can select a lamp but are
// not surfaces anyone means when they say "the ground".
static HE::ScenePick::ObjectFilter surfaceFilter(const std::unordered_set<uint32_t>& exclude)
{
	return [&exclude](const RenderObject& obj)
	{
		return exclude.find(obj.entityId) == exclude.end()
		    && !HE::isEditorIconMaterial(obj.materialAssetId);
	};
}
static bool probeSurface(AppContext& ctx, const RenderWorld& snapshotWorld,
                         const glm::vec3& origin, const glm::vec3& dir,
                         const std::unordered_set<uint32_t>& exclude,
                         glm::vec3& outPoint, glm::vec3* outNormal = nullptr)
{
	if (!ctx.contentManager) return false;
	const HE::ScenePick::SurfaceHit hit = HE::ScenePick::raycast(
		snapshotWorld, meshLookup(*ctx.contentManager), origin, dir, surfaceFilter(exclude));
	if (!hit.hit) return false;
	outPoint = hit.point;
	if (outNormal) *outNormal = hit.normal;
	return true;
}
// Snap to Ground (End): every selection root dropped onto what is beneath it.
// The geometry question is ViewportActions' (headless, tested); this only
// hands it the scene probe and the selection's boxes, and takes the snapshot.
static void snapSelectionToGround(AppContext& ctx, const RenderWorld& snapshotWorld)
{
	if (!ctx.world || ctx.isPlaying || ctx.selection.empty() || !ctx.contentManager) return;
	const ViewportActions::SurfaceProbe probe =
		[&](const glm::vec3& origin, const glm::vec3& dir,
		    const std::unordered_set<uint32_t>& exclude, glm::vec3& out)
		{ return probeSurface(ctx, snapshotWorld, origin, dir, exclude, out); };
	const ViewportActions::SubtreeBounds bounds =
		[&](Entity root, HE::AABB& box)
		{
			HE::AABB pivots;
			selectionBoxes(*ctx.world, ctx.contentManager, root, snapshotWorld, box, pivots);
			return box.isValid();
		};
	snapshot(ctx, "Snap to Ground");
	noteEdited(ctx, ViewportActions::snapToGround(*ctx.world, ctx.selection, probe, bounds));
}

static void focusSelected(AppContext& ctx, const RenderWorld& snapshotWorld)
{
	if (!ctx.world || !ctx.editorCamera) return;
	const Entity primary = ctx.selection.primary();
	if (primary == entt::null || !ctx.world->registry().valid(primary)) return;
	glm::vec3 center(0.0f);
	float     radius = 0.0f;
	if (selectionFocusSphere(*ctx.world, ctx.contentManager, primary, snapshotWorld, center, radius))
		ctx.editorCamera->focusOn(center, radius);
}

// The scene extract this panel drew its last frame from — declared at file
// scope (rather than as the static inside render() it used to be) because the
// secondary viewports frame the selection against it too: they have no extract
// of their own to measure (their picture is drawn inside RenderWorldPreview),
// and the selection's boxes are the same boxes from any direction.
static RenderExtractor s_extractor;
static RenderWorld     s_sceneSnapshot;

bool focusSelection(AppContext& ctx, EditorCamera& cam)
{
	if (!ctx.world) return false;
	const Entity primary = ctx.selection.primary();
	if (primary == entt::null || !ctx.world->registry().valid(primary)) return false;
	glm::vec3 center(0.0f);
	float     radius = 0.0f;
	if (!selectionFocusSphere(*ctx.world, ctx.contentManager, primary, s_sceneSnapshot, center, radius))
		return false;
	cam.focusOn(center, radius);
	return true;
}

bool selectionBox(AppContext& ctx, HE::AABB& out)
{
	if (!ctx.world) return false;
	const Entity primary = ctx.selection.primary();
	if (primary == entt::null || !ctx.world->registry().valid(primary)) return false;
	HE::AABB geometry, pivots;
	selectionBoxes(*ctx.world, ctx.contentManager, primary, s_sceneSnapshot, geometry, pivots);
	if (geometry.isValid()) { out = geometry; return true; }
	if (!pivots.isValid()) return false;
	// Nothing here draws (a light, an empty group): a small box around where
	// it sits, so the outline still says "there".
	out = pivots;
	out.expand(pivots.min - glm::vec3(0.5f));
	out.expand(pivots.max + glm::vec3(0.5f));
	return true;
}

// ── The context menu's verbs, as the main bar's Entity menu calls them ──────
// Thin wrappers over the file-static actions above and the Scene window's
// own extract: the main menu has no snapshot of its own to measure against,
// and it must not need one.
EntityActionState entityActionState(AppContext& ctx)
{
	EntityActionState st;
	if (!ctx.world) return st;
	auto& reg = ctx.world->registry();
	const Entity primary = ctx.selection.primary();
	const bool   hasSel  = !ctx.selection.empty();
	const bool   editable = !ctx.isPlaying;
	st.canFocus  = ctx.editorCamera && primary != entt::null && reg.valid(primary);
	st.canEdit   = editable && hasSel;
	st.anyHidden = editable && ViewportActions::anyHidden(*ctx.world);
	for (const Entity e : ctx.selection.entities())
		if (reg.valid(e) && !ctx.world->isBuiltin(e)) { st.groupable = true; break; }
	st.groupable  = editable && st.groupable;
	st.canUngroup = editable && ViewportActions::canUngroup(*ctx.world, ctx.selection);
	st.primaryLocked = primary != entt::null && reg.valid(primary)
	                && reg.all_of<EditorLockComponent>(primary);
	return st;
}
// (Hide / Isolate / Show All / Group / Ungroup are the functions above.)
void focusSelected(AppContext& ctx)          { focusSelected(ctx, s_sceneSnapshot); }
void snapSelectionToGround(AppContext& ctx)  { snapSelectionToGround(ctx, s_sceneSnapshot); }
void toggleLockSelected(AppContext& ctx)
{
	if (!ctx.world || ctx.isPlaying || ctx.selection.empty()) return;
	auto& reg = ctx.world->registry();
	const Entity primary = ctx.selection.primary();
	const bool primaryLocked = primary != entt::null && reg.valid(primary)
	                        && reg.all_of<EditorLockComponent>(primary);
	snapshot(ctx, primaryLocked ? "Unlock Entity" : "Lock Entity");
	for (const Entity e : ctx.selection.entities())
	{
		if (!reg.valid(e) || e == ctx.world->rootEntity()) continue;
		if (primaryLocked) reg.remove<EditorLockComponent>(e);
		else               reg.emplace_or_replace<EditorLockComponent>(e);
	}
}

HE::ViewMode viewMode()                 { return s_tb.viewMode; }
void         setViewMode(HE::ViewMode m) { s_tb.viewMode = m; }

// View presets on the numeric keypad (Blender's layout, the one people arrive
// with): 7 Top, 1 Front, 3 Right, Ctrl flips each to its opposite, 5 toggles
// the lens. Keypad only — the plain digits are the bookmarks, and a laptop
// without a keypad has the toolbar's view cell for the same thing.
void presetKeys(EditorCamera& cam)
{
	using VP = EditorCamera::ViewPreset;
	const bool flip = ImGui::GetIO().KeyCtrl;
	if (ImGui::IsKeyPressed(ImGuiKey_Keypad7, false))
		cam.applyPreset(flip ? VP::Bottom : VP::Top);
	else if (ImGui::IsKeyPressed(ImGuiKey_Keypad1, false))
		cam.applyPreset(flip ? VP::Back : VP::Front);
	else if (ImGui::IsKeyPressed(ImGuiKey_Keypad3, false))
		cam.applyPreset(flip ? VP::Left : VP::Right);
	else if (ImGui::IsKeyPressed(ImGuiKey_Keypad5, false))
		cam.setOrthographic(!cam.orthographic());
}

// Camera bookmarks on the digit row: Ctrl+<digit> remembers the camera's
// pose, <digit> puts it back. The caller has already checked that the
// viewport is hovered, no text field wants the keys and Alt is up.
void bookmarkKeys(EditorCamera& cam)
{
	static const ImGuiKey kDigits[CameraBookmarks::kSlots] = {
		ImGuiKey_0, ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4,
		ImGuiKey_5, ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9,
	};
	CameraBookmarks::Set& marks = CameraBookmarks::editorSet();
	const bool store = ImGui::GetIO().KeyCtrl;
	for (int i = 0; i < CameraBookmarks::kSlots; ++i)
	{
		if (!ImGui::IsKeyPressed(kDigits[i], false)) continue;
		if (store) marks.store(i, cam);
		else       marks.recall(i, cam);
		break;
	}
}

static void drawContextMenu(AppContext& ctx, const RenderWorld& snapshotWorld)
{
	// Every row is looked up as "Viewport Menu/<its label>".
	HE::Ed::Help::Scope helpScope("Viewport Menu");
	auto& reg = ctx.world->registry();
	const bool editable  = !ctx.isPlaying;
	const bool hasSel    = !ctx.selection.empty();
	const bool canFocus  = ctx.editorCamera && ctx.selection.primary() != entt::null
	                    && reg.valid(ctx.selection.primary());

	if (EditorWidgets::menuItem("Focus Selected", EditorShortcuts::label("viewport.focus").c_str(), false, canFocus))
		focusSelected(ctx, snapshotWorld);
	if (EditorWidgets::menuItem("Snap to Ground", EditorShortcuts::label("viewport.snapToGround").c_str(), false, editable && hasSel))
		snapSelectionToGround(ctx, snapshotWorld);

	ImGui::Separator();
	if (EditorWidgets::menuItem("Hide Selected", EditorShortcuts::label("viewport.hide").c_str(), false, editable && hasSel))
		hideSelected(ctx);
	if (EditorWidgets::menuItem("Isolate Selected", EditorShortcuts::label("viewport.isolate").c_str(), false, editable && hasSel))
		isolateSelected(ctx);
	if (EditorWidgets::menuItem("Show All", EditorShortcuts::label("viewport.showAll").c_str(), false,
	                            editable && ViewportActions::anyHidden(*ctx.world)))
		showAll(ctx);

	ImGui::Separator();
	// Group needs something that is not a built-in; the sun cannot be grouped.
	bool groupable = false;
	for (const Entity e : ctx.selection.entities())
		if (reg.valid(e) && !ctx.world->isBuiltin(e)) { groupable = true; break; }
	if (EditorWidgets::menuItem("Group", EditorShortcuts::label("viewport.group").c_str(), false, editable && groupable))
		groupSelected(ctx);
	if (EditorWidgets::menuItem("Ungroup", EditorShortcuts::label("viewport.ungroup").c_str(), false,
	                            editable && ViewportActions::canUngroup(*ctx.world, ctx.selection)))
		ungroupSelected(ctx);

	// Lock / unlock, verb decided by the primary the way the Outliner's row
	// decides it: a locked primary offers Unlock, the rest of the set follows.
	{
		const Entity primary = ctx.selection.primary();
		const bool primaryLocked = primary != entt::null && reg.valid(primary)
		                        && reg.all_of<EditorLockComponent>(primary);
		if (EditorWidgets::menuItem(primaryLocked ? "Unlock" : "Lock", nullptr, false,
		                            editable && hasSel))
		{
			snapshot(ctx, primaryLocked ? "Unlock Entity" : "Lock Entity");
			for (const Entity e : ctx.selection.entities())
			{
				if (!reg.valid(e) || e == ctx.world->rootEntity()) continue;
				if (primaryLocked) reg.remove<EditorLockComponent>(e);
				else               reg.emplace_or_replace<EditorLockComponent>(e);
			}
		}
	}

	// The SAME hooks the Edit menu, the keyboard and the Outliner use; they
	// act on the selection, which opening this menu just settled.
	ImGui::Separator();
	if (EditorWidgets::menuItem("Duplicate", EditorShortcuts::label("entity.duplicate").c_str(), false, editable && hasSel) && ctx.duplicateEntity)
		ctx.duplicateEntity();
	if (EditorWidgets::menuItem("Copy", EditorShortcuts::label("entity.copy").c_str(), false, editable && hasSel) && ctx.copyEntity)
		ctx.copyEntity();
	if (EditorWidgets::menuItem("Cut", EditorShortcuts::label("entity.cut").c_str(), false, editable && hasSel) && ctx.cutEntity)
		ctx.cutEntity();
	if (EditorWidgets::menuItem("Paste", EditorShortcuts::label("entity.paste").c_str(), false, editable && ctx.entityClipboardFull)
	    && ctx.pasteEntity)
		ctx.pasteEntity();

	ImGui::Separator();
	if (EditorWidgets::dangerMenuItem("Delete", editable && hasSel) && ctx.deleteEntity)
		ctx.deleteEntity();
}

void releaseViewportLookCapture(SDL_Window* win)
{
	EditorViewportNav::releaseLookCaptureFor(navOwner(), win);
}

void enforceViewportLookCaptureInvariant(SDL_Window* win)
{
	EditorViewportNav::enforceCaptureInvariant(win);
}

void renderSizePx(int& outW, int& outH) { outW = s_viewportPxW; outH = s_viewportPxH; }

void render(AppContext& ctx, float dt)
{
	// ── Scene viewport (offscreen render target as dockable window) ─────────
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		// NoMove is essential: the viewport content is a plain ImGui::Image (an
		// id-less item), so a click-drag on it would otherwise be treated as a
		// click on empty window space and start an ImGui window/dock move. That
		// move fights the ImGuizmo drag for the same mouse button, so the gizmo
		// never manipulates the object (translate/rotate/scale all dead). The
		// docked Scene window is still relocated via its tab, so NoMove costs
		// nothing here.
		ImGui::Begin("Scene", nullptr,
		             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
		             ImGuiWindowFlags_NoMove);
		ImGui::PopStyleVar();

		// ── Toolbar (always at top of Scene, works docked or floating) ────────
		// Zones, wells and the centred transport live in ViewportToolbar; it
		// leaves the cursor on the first row below the strip.
		//
		// An application project has none of what the bar edits: no gizmo, no
		// snapping, no camera speed, no play transport (docs/he-apps-plan.md E2).
		// The whole strip goes rather than being greyed out — a row of disabled
		// controls is a promise that they mean something here.
		if (!ctx.appLivePreview)
			ViewportToolbar::render(ctx, s_tb);

		// The view mode goes to the renderer every frame, like the camera: it is
		// this panel's state, and the backend keeps whatever it was last told.
		// An application preview has no scene to shade and shows Lit. The
		// headless HE_DUMP_VIEWMODE / HE_DUMP_GBUFFER overrides win here too,
		// for the same reason HE_DUMP_RENDERPATH does in EditorApplication's
		// per-frame push: a push that ran every frame would otherwise flip a
		// capture back to the toolbar's value between setup and the frame.
		if (ctx.renderer)
			ctx.renderer->SetViewMode(HE::Ed::viewModeOverrideFromEnv(
				ctx.appLivePreview ? HE::ViewMode::Lit : s_tb.viewMode));

		ImVec2 avail = ImGui::GetContentRegionAvail();

		// HE_VIEWPORT_RESIZE_STRESS=1 oscillates the viewport size every frame
		// to stress-test render-target recreation — a crash here means a
		// texture-lifetime bug in the backend (retired textures must outlive
		// the ImGui draw list that references them).
		static const bool kResizeStress = std::getenv("HE_VIEWPORT_RESIZE_STRESS") != nullptr;
		if (kResizeStress)
		{
			static int s_stressFrame = 0;
			++s_stressFrame;
			avail.x = std::max(64.0f, avail.x - static_cast<float>((s_stressFrame % 13) * 16));
			avail.y = std::max(64.0f, avail.y - static_cast<float>((s_stressFrame %  7) * 16));
		}

		if (ctx.renderer && avail.x >= 1.0f && avail.y >= 1.0f)
		{
			// Render at framebuffer resolution (HiDPI aware)
			const ImVec2 fbScale = ImGui::GetIO().DisplayFramebufferScale;
			s_viewportPxW = static_cast<int>(avail.x * fbScale.x);
			s_viewportPxH = static_cast<int>(avail.y * fbScale.y);
			ctx.renderer->SetViewportSize(
				static_cast<uint32_t>(s_viewportPxW),
				static_cast<uint32_t>(s_viewportPxH));

			if (void* tex = ctx.renderer->GetViewportTexture())
			{
				// OpenGL FBO textures have a bottom-left origin — flip vertically
				const bool flipY = (ctx.backend == HE::RendererBackend::OpenGL);
				ImGui::Image(reinterpret_cast<ImTextureID>(tex), avail,
				             flipY ? ImVec2(0, 1) : ImVec2(0, 0),
				             flipY ? ImVec2(1, 0) : ImVec2(1, 1));

				// Drag a mesh (or any asset) from the Content Browser onto the
				// viewport to spawn it. The drop target must bind to the Image
				// item, but placing it needs this frame's camera matrices (built
				// by the extract below), so just RECORD the drop here and process
				// it after the extract.
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
					{
						s_viewportDropPath.assign(static_cast<const char*>(p->Data));
						s_viewportDropMouse = ImGui::GetMousePos();
					}
					ImGui::EndDragDropTarget();
				}

				const ImVec2 rectMin = ImGui::GetItemRectMin();
				const ImVec2 rectMax = ImGui::GetItemRectMax();
				const bool viewportHovered = ImGui::IsItemHovered();
				ImGuiIO& io = ImGui::GetIO();

				// Camera + object snapshot, identical to what the backend
				// renders with (extractor recomputes world matrices). The
				// editor camera overrides any scene camera so the gizmo and
				// picking ray match exactly what is on screen.
				//
				// Declared ahead of the camera block because the F key measures
				// the selection out of it, and the camera has to be settled
				// BEFORE the extract runs (the extract is what the gizmo, the
				// picking ray and the drop probe all read this frame). F therefore
				// frames against last frame's boxes, which is exactly as accurate:
				// nothing resizes between two frames of holding a key down.
				// (s_extractor / s_sceneSnapshot, file scope above.)

				// ── Editor camera: drive from viewport input ────────────────
				// In play mode the game's scene camera takes over, so the
				// override is cleared and editor navigation is disabled.
				bool navigating = false;
				SDL_Window* sdlWin = ctx.window ? ctx.window->GetNativeWindow() : nullptr;
				// Drop fly-look capture: warp the cursor back to the press point BEFORE
				// leaving relative mode (SDL applies the warp as the post-relative
				// position, landing it exactly where the look-drag began). Shared with the
				// tab-switch safety release (releaseViewportLookCapture, file scope).
				auto endLookCapture = [&]() { releaseViewportLookCapture(sdlWin); };
				// An application's preview is always live, so its pointer is fed
				// every frame rather than only during play (ctx.appLivePreview).
				if (ctx.editorCamera && (ctx.isPlaying || ctx.appLivePreview))
				{
					endLookCapture();
					ctx.renderer->SetEditorCamera(EditorCameraOverride{}); // active=false

					// Feed the in-game UI pointer: mouse relative to the viewport
					// image, scaled to render-target pixels (the space the UI pass
					// and UISystem hit-tests operate in).
					if (ctx.reportPlayUIPointer)
					{
						const float mx = (io.MousePos.x - rectMin.x) * fbScale.x;
						const float my = (io.MousePos.y - rectMin.y) * fbScale.y;
						ctx.reportPlayUIPointer(mx, my,
							static_cast<float>(s_viewportPxW),
							static_cast<float>(s_viewportPxH),
							ImGui::IsMouseDown(ImGuiMouseButton_Left),
							viewportHovered,
							viewportHovered ? io.MouseWheel : 0.0f,
							ImGui::IsMouseDown(ImGuiMouseButton_Right),
							viewportHovered &&
								ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
					}
					// Where the image IS, for the drags that are not the mouse.
					// The corner is measured against the panel's own platform
					// viewport rather than the desktop: a file drop arrives in
					// window coordinates, and a panel torn out into a window of
					// its own would otherwise be offset by wherever that window
					// happens to sit on the screen.
					if (ctx.reportPlayUIRect)
					{
						const ImGuiViewport* vp = ImGui::GetWindowViewport();
						ctx.reportPlayUIRect(rectMin.x - vp->Pos.x, rectMin.y - vp->Pos.y,
							fbScale.x, fbScale.y,
							static_cast<unsigned>(reinterpret_cast<intptr_t>(vp->PlatformHandle)));
					}
				}
				else if (ctx.editorCamera)
				{
					EditorCamera& cam = *ctx.editorCamera;
					const bool imageHovered = ImGui::IsItemHovered();

					EditorCamera::Input cin;
					navigating = EditorViewportNav::gather(ctx, navOwner(), imageHovered,
					                                      dt, avail.y, cin);

					// ── Right-click → context menu ──────────────────────────
					// The right button is the fly-look, and gather() takes the
					// cursor on the press edge — so a CLICK is a press that was
					// released before the look moved: the same button, decided
					// at release by how far it travelled, the way the left
					// button decides click-vs-frame above. The press is ImGui's
					// click edge — the same one gather() engages on, and one
					// ImGui's event queue never drops however short the click.
					// The release is the PHYSICAL SDL button going up, as
					// gather() reads it: mid-look ImGui's mouse state is zeroed
					// by the NoMouse flag, so the release edge never reaches
					// IsMouseReleased. The travel is the look's own relative-
					// motion delta (in relative mode the absolute position does
					// not move at all). The press position is remembered from
					// the press frame, where ImGui's is still good; by the
					// release the capture has warped the cursor back there and
					// io.MousePos is a frame stale.
					//
					// Not on a trackpad: there the tap IS the fly toggle, and
					// the menu is on the Menu key / Shift+F10 instead (below).
					{
						static bool   s_rmbArmed  = false;
						static float  s_rmbTravel = 0.0f;
						static ImVec2 s_rmbPressAt{};
						constexpr float kClickTravel = 4.0f;   // px of look before it is a drag
						if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
						{
							s_rmbArmed   = imageHovered && !io.KeyAlt && !io.WantTextInput
							            && !EditorInput::trackpadPointer(ctx);
							s_rmbTravel  = 0.0f;
							s_rmbPressAt = ImGui::GetMousePos();
						}
						if (s_rmbArmed)
						{
							if (cin.look)
								s_rmbTravel += std::abs(cin.mouseDelta.x) + std::abs(cin.mouseDelta.y);
							// A flight with a still mouse — RMB held, W pressed,
							// or the wheel dollying — is a look too, not a click.
							if (cin.moveAxis != glm::vec3(0.0f) || cin.wheel != 0.0f)
								s_rmbArmed = false;
							const bool physRmb =
								(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) != 0;
							if (!physRmb)
							{
								if (s_rmbTravel < kClickTravel)
								{
									s_contextMenuRequest = true;
									s_contextMenuAt      = s_rmbPressAt;
								}
								s_rmbArmed = false;
							}
						}
					}
					// The keyboard way in, for a trackpad or for a hand that is
					// already on the keys: at the cursor, over the picture.
					if (imageHovered && !io.WantTextInput && !navigating &&
					    (ImGui::IsKeyPressed(ImGuiKey_Menu, false) ||
					     (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F10, false))))
					{
						s_contextMenuRequest = true;
						s_contextMenuAt      = ImGui::GetMousePos();
					}
					// The menu's verbs on keys, the ones its rows print. Alt+H
					// is safe beside Alt+LMB orbit — a key press is not a drag.
					// The chords come from EditorShortcuts (Preferences ▸
					// Shortcuts), which also holds the not-while-typing guard.
					if (imageHovered && !navigating && !ctx.isPlaying)
					{
						if (EditorShortcuts::pressed("viewport.showAll"))  showAll(ctx);
						if (EditorShortcuts::pressed("viewport.isolate"))  isolateSelected(ctx);
						if (EditorShortcuts::pressed("viewport.hide"))     hideSelected(ctx);
						if (EditorShortcuts::pressed("viewport.group"))    groupSelected(ctx);
						if (EditorShortcuts::pressed("viewport.ungroup"))  ungroupSelected(ctx);
						// End drops the selection onto the ground — Unreal's key
						// for it, and one nothing else in the viewport uses.
						if (EditorShortcuts::pressed("viewport.snapToGround"))
							snapSelectionToGround(ctx, s_sceneSnapshot);
					}
					// Focus on selection (F) — frame the selected entity and
					// everything parented under it (see selectionFocusSphere).
					if (imageHovered && !navigating &&
					    EditorShortcuts::pressed("viewport.focus") &&
					    ctx.world && ctx.selection.primary() != entt::null &&
					    ctx.world->registry().valid(ctx.selection.primary()))
					{
						glm::vec3 center(0.0f);
						float     radius = 0.0f;
						if (selectionFocusSphere(*ctx.world, ctx.contentManager,
						                         ctx.selection.primary(), s_sceneSnapshot,
						                         center, radius))
							cam.focusOn(center, radius);
					}
					// View presets on the numeric keypad (Blender's layout, the
					// one people arrive with): 7 Top, 1 Front, 3 Right, Ctrl
					// flips each to its opposite, 5 toggles the lens. Keypad
					// only — the plain digits are free for whatever a tool
					// binds, and a laptop without a keypad has the toolbar's
					// view cell for the same thing.
					if (imageHovered && !io.WantTextInput && !navigating)
						presetKeys(cam);
					// Camera bookmarks on the digit row (Unreal's layout): Ctrl
					// stores this view, the bare digit jumps back. Not with Alt
					// held — Alt+2/3/4 are the view modes below.
					if (imageHovered && !io.WantTextInput && !navigating && !io.KeyAlt)
						bookmarkKeys(cam);

					cam.update(cin);
					// Push to the backend so this frame's render uses it. The
					// icon switch rides on the override (see EditorCameraOverride)
					// — set here AND on the pick extract below, or the picker and
					// the picture disagree about whether the lamp symbol exists.
					EditorCameraOverride ov = cam.makeOverride();
					ov.editorIcons = s_showFlags.editorIcons;
					ctx.renderer->SetEditorCamera(ov);
				}

				// View mode on Alt+2 / 3 / 4 (Unreal's layout: Wireframe, Unlit,
				// Lit). Outside the camera branch on purpose — it is a renderer
				// flag, so it works while the scene plays too. Alt alone is the
				// orbit modifier, which a digit press does not disturb.
				if (viewportHovered && !io.WantTextInput && io.KeyAlt && !navigating
				    && !ctx.appLivePreview)
				{
					if (ImGui::IsKeyPressed(ImGuiKey_2, false))      s_tb.viewMode = HE::ViewMode::Wireframe;
					else if (ImGui::IsKeyPressed(ImGuiKey_3, false)) s_tb.viewMode = HE::ViewMode::Unlit;
					else if (ImGui::IsKeyPressed(ImGuiKey_4, false)) s_tb.viewMode = HE::ViewMode::Lit;
				}

				EditorCameraOverride camOverride =
					(ctx.editorCamera && !ctx.isPlaying) ? ctx.editorCamera->makeOverride()
					                                     : EditorCameraOverride{};
				camOverride.editorIcons = s_showFlags.editorIcons;
				if (ctx.world)
					s_extractor.extract(*ctx.world, s_sceneSnapshot, avail.x / avail.y,
					                    camOverride.active ? &camOverride : nullptr);

				// ── Spawn a mesh or prefab dropped onto the viewport ────────
				// A collision probe decides where: the drop ray is traced against
				// the scene's actual geometry and the asset lands on the nearest
				// surface under the cursor (terrain, floor, another mesh), resting
				// on it rather than intersecting it. With nothing under the cursor
				// the ground plane (Y=0) takes over, and if the ray points away
				// from that too (looking up), a fixed distance in front of the
				// camera. Everything else is ignored.
				if (!s_viewportDropPath.empty())
				{
					if (ctx.world && ctx.contentManager)
					{
						const std::string rel = ctx.contentManager->toContentRelativePath(s_viewportDropPath);
						const HE::UUID id = rel.empty() ? HE::UUID{} : ctx.contentManager->loadAsset(rel);
						const StaticMeshAsset* mesh = (id != HE::UUID{}) ? ctx.contentManager->getStaticMesh(id) : nullptr;
						// Only asked when the mesh lookup came up empty — one UUID is
						// never both, and the SlotMap's stored-id check makes the
						// cross-type probe safe rather than an alias.
						const PrefabAsset* prefab = (!mesh && id != HE::UUID{}) ? ctx.contentManager->getPrefab(id) : nullptr;
						if (mesh || prefab)
						{
							glm::vec3 spawnPos(0.0f);
							bool placed = false;
							const glm::mat4 invVP = glm::inverse(
								s_sceneSnapshot.camera.projection * s_sceneSnapshot.camera.view);
							const float du = (s_viewportDropMouse.x - rectMin.x) / std::max(rectMax.x - rectMin.x, 1.0f);
							const float dv = (s_viewportDropMouse.y - rectMin.y) / std::max(rectMax.y - rectMin.y, 1.0f);
							glm::vec4 pNear = invVP * glm::vec4(2.0f*du-1.0f, 1.0f-2.0f*dv, -1.0f, 1.0f);
							glm::vec4 pFar  = invVP * glm::vec4(2.0f*du-1.0f, 1.0f-2.0f*dv,  1.0f, 1.0f);
							if (std::abs(pNear.w) > 1e-6f && std::abs(pFar.w) > 1e-6f)
							{
								pNear /= pNear.w; pFar /= pFar.w;
								const glm::vec3 ro(pNear);
								const glm::vec3 rd = glm::normalize(glm::vec3(pFar) - glm::vec3(pNear));
								const HE::ScenePick::SurfaceHit surface = HE::ScenePick::raycast(
									s_sceneSnapshot, meshLookup(*ctx.contentManager), ro, rd);
								if (surface.hit) { spawnPos = surface.point; placed = true; }
								else if (std::abs(rd.y) > 1e-5f)
								{
									const float t = -ro.y / rd.y;
									if (t > 0.0f) { spawnPos = ro + t * rd; placed = true; }
								}
							}
							// Rest the mesh ON the surface: its own bounds decide how
							// far its origin sits above the contact point, so a model
							// whose pivot is at its centre doesn't sink in halfway.
							// A prefab has no single mesh to measure — its subtree can
							// hold many, or none — so its root simply lands on the
							// contact point and keeps whatever offset it was authored with.
							if (placed && mesh)
								if (const HE::AABB* box = meshBounds(*ctx.contentManager, id))
									spawnPos.y -= box->min.y;
							if (!placed && ctx.editorCamera)
							{
								// EditorCamera::forward() is private — derive it from the
								// public yaw/pitch (same convention as HE_DUMP_MATERIALTEST).
								const float cp = std::cos(ctx.editorCamera->pitch()), sp = std::sin(ctx.editorCamera->pitch());
								const float cy = std::cos(ctx.editorCamera->yaw()),   sy = std::sin(ctx.editorCamera->yaw());
								spawnPos = ctx.editorCamera->position() + glm::vec3(cp*sy, sp, -cp*cy) * 8.0f;
							}

							if (ctx.undoSys) ctx.undoSys->snapshotNow("Place Asset");
							if (mesh)
							{
								// Copied out BEFORE the material load below: the asset stores
								// are vector-backed SlotMaps, so registering another asset can
								// reallocate them and dangle `mesh`.
								const std::string meshName = mesh->name;
								const std::string matRel   = mesh->materialPath;

								Entity e = ctx.world->createEntity(meshName);
								TransformComponent tc; tc.position = spawnPos;
								ctx.world->addComponent(e, tc);
								ctx.world->addComponent(e, MeshComponent{ .meshAssetId = id });
								// …and the material the mesh names (chunk MREF), exactly as the
								// Content Browser's "Add to Scene" does. Without it the draw
								// carries no material at all — RenderExtractor takes matId only
								// from this component — so the generated PBR shader, the normal
								// and ORM maps and the alpha-mask cutout an import writes are
								// all inert and only the base-colour texture survives. Dropping
								// and right-clicking the same asset must not give two results,
								// and dropping is the gesture people reach for first.
								if (!matRel.empty())
								{
									const HE::UUID matId = ctx.contentManager->loadAsset(matRel);
									if (matId != HE::UUID{})
										ctx.world->addComponent(e, MaterialComponent{ matId });
									else
										HE_LOG_WARN(Editor, "%s",
											("Editor: '" + meshName + "' names material '" + matRel
											 + "', which did not load — spawned without it").c_str());
								}
								ctx.world->markHierarchyDirty();
								ctx.selection.set(e); // select the freshly spawned mesh
								HE_LOG_INFO(Editor, "%s",
									("Editor: spawned '" + meshName + "' into the scene via drag-drop").c_str());
							}
							else
							{
								// preserveIds stays false: the blob carries the entity
								// uuids of the subtree it was captured from, and reusing
								// them would make two drops of the same prefab claim one
								// identity (see SceneSerializer::instantiatePrefab).
								SceneSerializer ser;
								std::vector<PrefabInstanceComponent::Binding> bindings;
								const Entity root = ser.instantiatePrefab(*ctx.world, prefab->data,
								                                          entt::null, /*preserveIds=*/false,
								                                          &bindings);
								if (root != entt::null)
								{
									// Only the position is overwritten — the prefab's own
									// rotation and scale are part of what was saved, and
									// resetting them would silently un-author it.
									if (auto* t = ctx.world->registry().try_get<TransformComponent>(root))
										t->position = spawnPos;
									else
									{
										TransformComponent tc; tc.position = spawnPos;
										ctx.world->addComponent(root, tc);
									}
									// Where it came from (PrefabInstanceComponent). Written
									// HERE and not inside instantiatePrefab, which also
									// serves paste, duplicate and a peer's create —
									// none of those is a prefab placement, and stamping
									// a link there would make three quarters of the
									// links in a scene lies. Without this line the
									// prefabs a HUMAN drops are the ones prefab_instances
									// cannot see. The bindings come from the same call
									// that minted the entities: which template record
									// became which entity is known nowhere else.
									PrefabInstanceComponent inst;
									inst.asset    = id;
									inst.bindings = std::move(bindings);
									ctx.world->registry().emplace_or_replace<PrefabInstanceComponent>(
										root, std::move(inst));
									ctx.world->markHierarchyDirty();
									ctx.selection.set(root);
									HE_LOG_INFO(Editor, "%s",
										("Editor: instantiated prefab '" + prefab->name + "' into the scene via drag-drop").c_str());
								}
								else
									HE_LOG_ERROR(Editor, "%s",
										("Editor: prefab '" + prefab->name + "' could not be instantiated — its payload is not a readable subtree").c_str());
							}
						}
						else
							HE_LOG_INFO(Editor, "%s",
								"Editor: dropped asset is not a static mesh or prefab — nothing spawned");
					}
					s_viewportDropPath.clear();
				}


				// ── Gizmo on the selection ──────────────────────────────────
				// Suppressed in Landscape mode: there LMB belongs to the sculpt
				// brush, and a stray gizmo drag would silently move/scale the
				// terrain — which then breaks the brush's world↔grid mapping.
				bool gizmoActive = false;
				if (ctx.editorConfig.mode != EditorMode::Landscape && ctx.world)
				{
					// W/E/R switch operation while the viewport is hovered (but not
					// while flying — W/A/S/D drive the camera then). The toolbar's
					// Move/Rotate/Scale cells set the same shared state.
					EditorTransformGizmo::handleOperationKeys(
						s_tb, ImGui::IsWindowHovered(), navigating);
					// The gizmo itself is shared with the class editor's viewport —
					// see EditorTransformGizmo for why a second copy would be a bug.
					// Suppressed while the camera is being driven so Alt+LMB orbit
					// and RMB fly-look don't fight the manipulator for the button.
					// The whole selection, roots only: a child whose parent is
					// selected too moves through the parent (see
					// EditorSelection::roots), and one entity is the plain
					// single-object gizmo it always was. Minus anything locked
					// in the Outliner: such an entity can be selected there and
					// edited in Details, but the gizmo does not take hold of it
					// — with nothing else selected there is no gizmo at all.
					std::vector<Entity> movable = ctx.selection.roots(ctx.world->registry());
					{
						auto& reg = ctx.world->registry();
						movable.erase(std::remove_if(movable.begin(), movable.end(),
						                             [&reg](Entity e) { return reg.all_of<EditorLockComponent>(e); }),
						              movable.end());
					}
					// Surface / vertex snapping: what a move drag lands on, asked
					// of this frame's extract. Only built when the mode calls for
					// it — the probe walks the scene, and the grid needs nothing.
					EditorTransformGizmo::SnapProbe probe;
					if (s_tb.probeSnapActive() && ctx.contentManager)
					{
						probe = [&](ViewportToolbar::State::SnapMode mode,
						            const ImVec2& screen, glm::vec3& out) -> bool
						{
							auto& reg = ctx.world->registry();
							std::unordered_set<uint32_t> exclude;
							for (const Entity r : movable) collectSubtree(reg, r, exclude);
							const glm::mat4 viewProj = s_sceneSnapshot.camera.projection
							                         * s_sceneSnapshot.camera.view;
							const glm::vec2 rmin(rectMin.x, rectMin.y);
							const glm::vec2 rsize(rectMax.x - rectMin.x, rectMax.y - rectMin.y);
							const glm::vec2 at(screen.x, screen.y);
							if (mode == ViewportToolbar::State::SnapMode::Vertex)
							{
								// Terrain is left out: a landscape has vertices
								// every metre and no corner anyone means to hit.
								const HE::ScenePick::ObjectFilter base = surfaceFilter(exclude);
								const HE::ScenePick::VertexHit hit = HE::ScenePick::nearestVertex(
									s_sceneSnapshot, meshLookup(*ctx.contentManager), viewProj,
									rmin, rsize, at, s_tb.snapVertexRadiusPx,
									[&](const RenderObject& obj)
									{
										if (!base(obj)) return false;
										const Entity e = static_cast<Entity>(obj.entityId);
										return !reg.valid(e) ||
										       !reg.any_of<TerrainComponent, TerrainChunkComponent>(e);
									});
								if (!hit.hit) return false;
								out = hit.point;
								return true;
							}
							glm::vec3 ro, rd;
							if (!PreviewPick::screenRay(viewProj, rmin, rsize, at, ro, rd)) return false;
							glm::vec3 normal;
							if (!probeSurface(ctx, s_sceneSnapshot, ro, rd, exclude, out, &normal)) return false;
							// Rest the object ON the surface: lift the pivot by its
							// height above the bottom of what it draws, along the
							// surface's normal. One object only — a group's pivot is
							// a centroid, and its members have no shared bottom.
							if (s_tb.snapSurfaceRest && movable.size() == 1)
							{
								HE::AABB geometry, pivots;
								selectionBoxes(*ctx.world, ctx.contentManager, movable.front(),
								               s_sceneSnapshot, geometry, pivots);
								const float lift = HE::worldPositionOf(*ctx.world, movable.front()).y
								                 - geometry.min.y;
								if (geometry.isValid() && lift > 0.0f) out += normal * lift;
							}
							return true;
						};
					}
					gizmoActive = EditorTransformGizmo::manipulate(
						*ctx.world, movable,
						s_sceneSnapshot.camera.view, s_sceneSnapshot.camera.projection,
						rectMin, rectMax, s_tb,
						/*enabled=*/!navigating && !io.KeyAlt, ctx.undoSys, nullptr, probe);
				}

				// ── Picking and the rubber band ─────────────────────────────
				// One left press on the picture arms both: on release it is a
				// CLICK if the mouse never moved past ImGui's drag threshold,
				// and selects what was under the press; otherwise it was a
				// FRAME, and selects everything inside it. Deciding at release
				// rather than at press is what keeps a Ctrl-drag from toggling
				// the object under the cursor first and framing second.
				//
				// Disabled in Landscape mode so a brush stroke can't deselect /
				// reselect entities and pop the gizmo back up mid-sculpt, and
				// never while the gizmo, the camera or Alt (orbit) has the button.
				static bool   s_pressArmed = false;  // LMB went down on the image with nothing else claiming it
				static bool   s_frameLive  = false;  // …and has since moved far enough to be a frame
				static ImVec2 s_pressPos{};
				const bool pickable = ctx.editorConfig.mode != EditorMode::Landscape &&
				                      ctx.world && !gizmoActive && !navigating && !io.KeyAlt;
				if (pickable && ImGui::IsItemClicked(ImGuiMouseButton_Left))
				{
					s_pressArmed = true;
					s_frameLive  = false;
					s_pressPos   = ImGui::GetMousePos();
				}
				// Whatever took the button mid-gesture — RMB fly-look, Alt orbit, a
				// mode switch — cancels the gesture rather than finishing it.
				if (s_pressArmed && (ctx.editorConfig.mode == EditorMode::Landscape ||
				                     !ctx.world || navigating || io.KeyAlt))
				{
					s_pressArmed = false;
					s_frameLive  = false;
				}

				// A click position → the entity under it, mesh before terrain
				// (the rule lives in ViewportPick). Boxes come from the mesh
				// assets through the cache above; an entity whose asset is not
				// readable measures as the fallback cube. Lights, cameras and
				// audio sources are in the snapshot as their icon quads, with the
				// entity id on the quad, so a click on the symbol selects them
				// like any mesh; ViewportPick asserts that headless.
				auto pickAt = [&](const ImVec2& mouse) -> Entity
				{
					ContentManager* cm = ctx.contentManager;
					const ViewportPick::BoxLookup boxes = [cm](const HE::UUID& meshId) -> const HE::AABB*
					{
						return cm ? meshBounds(*cm, meshId) : nullptr;
					};
					return ViewportPick::pickAtScreen(
						s_sceneSnapshot, ctx.world->registry(), boxes,
						s_sceneSnapshot.camera.projection * s_sceneSnapshot.camera.view,
						{ rectMin.x, rectMin.y }, { rectMax.x - rectMin.x, rectMax.y - rectMin.y },
						{ mouse.x, mouse.y });
				};

				// Everything the frame between `a` and `b` (screen positions)
				// encloses — see EditorMarquee for the rule. Meshes only, never
				// terrain: a landscape is hundreds of chunk entities, and "the
				// ground" is not what anyone frames on purpose; a click still
				// selects it. Lights, cameras and audio sources are on screen
				// as their icon quads (RenderExtractor's editor icons), so they
				// frame like any mesh; a bare empty draws nothing and stays out.
				auto entitiesInFrame = [&](const ImVec2& a, const ImVec2& b) -> std::vector<Entity>
				{
					const float w = std::max(rectMax.x - rectMin.x, 1.0f);
					const float h = std::max(rectMax.y - rectMin.y, 1.0f);
					const EditorMarquee::Rect frame = EditorMarquee::Rect::fromCorners(
						{ (a.x - rectMin.x) / w, (a.y - rectMin.y) / h },
						{ (b.x - rectMin.x) / w, (b.y - rectMin.y) / h });
					const glm::mat4 viewProj =
						s_sceneSnapshot.camera.projection * s_sceneSnapshot.camera.view;

					auto& reg = ctx.world->registry();
					std::vector<Entity> found;
					std::unordered_set<uint32_t> seen; // an entity draws one object per material slot
					auto consider = [&](uint32_t entityId, const HE::UUID& meshId, const glm::mat4& model)
					{
						const Entity e = static_cast<Entity>(entityId);
						if (!reg.valid(e) || !seen.insert(entityId).second) return;
						if (reg.any_of<TerrainChunkComponent, TerrainComponent>(e)) return;
						// Locked in the Outliner: not framed, as it is not clicked.
						if (reg.all_of<EditorLockComponent>(e)) return;
						const HE::AABB* box = (meshId != HE::UUID{} && ctx.contentManager)
						                    ? meshBounds(*ctx.contentManager, meshId) : nullptr;
						if (EditorMarquee::encloses(viewProj, frame, box ? *box : ViewportPick::fallbackBox(), model))
							found.push_back(e);
					};
					for (const RenderObject& obj : s_sceneSnapshot.objects)
						consider(obj.entityId, obj.meshAssetId, obj.transform);
					for (const SkinnedRenderObject& obj : s_sceneSnapshot.skinnedObjects)
						consider(obj.entityId, obj.meshAssetId, obj.transform);
					return found;
				};

				if (s_pressArmed)
				{
					const ImVec2 mouse = ImGui::GetMousePos();
					if (!s_frameLive && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
						s_frameLive = true;

					if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
					{
						if (s_frameLive)
						{
							// The frame itself, clipped to the picture: a drag that
							// leaves the window keeps selecting, but the band must
							// not be painted over the panels next door.
							const ImVec2 a(std::clamp(std::min(s_pressPos.x, mouse.x), rectMin.x, rectMax.x),
							               std::clamp(std::min(s_pressPos.y, mouse.y), rectMin.y, rectMax.y));
							const ImVec2 b(std::clamp(std::max(s_pressPos.x, mouse.x), rectMin.x, rectMax.x),
							               std::clamp(std::max(s_pressPos.y, mouse.y), rectMin.y, rectMax.y));
							ImDrawList* dl = ImGui::GetWindowDrawList();
							// The selection highlight's own colour, so the band and
							// what it is about to select read as one thing.
							const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
							dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.18f)));
							dl->AddRect(a, b, ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.90f)), 0.0f, 0, 1.5f);
						}
					}
					else
					{
						// Released: decide. Ctrl/Cmd (io.KeyCtrl is the platform's
						// multi-select key, see OutlinerPanel) and Shift keep what
						// is selected — a click toggles the hit in and out of the
						// set, a frame adds its contents; a plain gesture replaces.
						// A plain click on nothing deselects; with the modifier
						// held it leaves the selection alone.
						const bool keep = io.KeyCtrl || io.KeyShift;
						if (s_frameLive)
						{
							const std::vector<Entity> inside = entitiesInFrame(s_pressPos, mouse);
							if (keep) ctx.selection.addMany(inside);
							else      ctx.selection.setMany(inside);
						}
						else
						{
							const Entity hit = pickAt(s_pressPos);
							if (keep) { if (hit != entt::null) ctx.selection.toggle(hit); }
							else      ctx.selection.set(hit);
						}
						s_pressArmed = false;
						s_frameLive  = false;
					}
				}

				// ── Context menu ────────────────────────────────────────────
				// Raised in the camera block; settled here because this is
				// where pickAt lives. The Outliner's rule for what the menu is
				// about: a hit outside the selection becomes the selection, a
				// hit inside keeps it, a miss leaves it alone. Not in Landscape
				// mode (a right-click there is a brush gesture waiting to
				// happen) and never over a gizmo drag. A request raised while
				// the nav latch is still set (a click so short that ImGui
				// reports the press and the release on consecutive frames)
				// waits one frame rather than being thrown away.
				if (s_contextMenuRequest && !navigating)
				{
					s_contextMenuRequest = false;
					if (pickable)
					{
						const Entity hit = pickAt(s_contextMenuAt);
						if (hit != entt::null && !ctx.selection.contains(hit))
							ctx.selection.set(hit);
						ImGui::OpenPopup("##vpContextMenu");
					}
				}
				// Placed by hand: on the frame the look capture let go the
				// cursor was just warped and ImGui's mouse position is stale,
				// so the popup would otherwise open wherever the cursor was
				// before the press. (ImGui clears the pending position itself
				// when the popup is not open.)
				ImGui::SetNextWindowPos(s_contextMenuAt, ImGuiCond_Appearing);
				if (ImGui::BeginPopup("##vpContextMenu"))
				{
					if (ctx.world) drawContextMenu(ctx, s_sceneSnapshot);
					ImGui::EndPopup();
				}

				// ── Landscape brush cursor + sculpt ────────────────────────
				// Brush state and the whole sculpt/paint stroke live in TerrainTools.cpp,
				// together with the Landscape tool panel that shares that state.
				TerrainTools::sculptInViewport(ctx, s_sceneSnapshot, rectMin, rectMax,
					navigating, viewportHovered, dt,
					[](const HE::UUID& meshId) { s_aabbCache.erase(meshId); });

				// ── Collaboration name tags ────────────────────────────────
				// Last, so they sit over the gizmo and the brush cursor: this is
				// the layer that has to stay findable, and the camera matrices it
				// projects with are the ones the frame was just rendered from.
				// Same switch as their depth-tested rings in the debug block.
				if (s_showFlags.collaborators)
					CollabPresenceBar::DrawViewportMarkers(
						ctx, s_sceneSnapshot.camera.view, s_sceneSnapshot.camera.projection,
						rectMin.x, rectMin.y, rectMax.x, rectMax.y);
				// The MCP clients' cameras get the same treatment on the same
				// switch: a tag with the connection number over the frame,
				// beside their frustum in the debug block. No session gate —
				// a client's camera exists without a collaboration session.
				if (s_showFlags.collaborators && ctx.mcpCameras)
					HE::Ed::McpCameraGizmos::drawViewportLabels(
						*ctx.mcpCameras, s_sceneSnapshot.camera.view,
						s_sceneSnapshot.camera.projection,
						rectMin.x, rectMin.y, rectMax.x, rectMax.y);
				// The counters, over everything: a diagnostic has to stay
				// readable whatever the scene is doing underneath it.
				if (s_showFlags.stats)
					drawStatsOverlay(ctx, rectMin, rectMax);
			}
			else
			{
				// The one and only piece of window text the Scene window submits,
				// and the whole reason this file needs a wrap at all: everything
				// else here — the toolbar strip, the gizmo, the landscape brush
				// ring, the collaborators' name tags — is painted straight into
				// the draw list at coordinates it computed itself, which no wrap
				// position can reach or disturb. This line is a sentence, it is
				// the only thing on screen when a backend cannot present the
				// viewport, and in a narrowly docked Scene panel it would
				// otherwise be cut off mid-word with no hint that it continued.
				// Scoped tightly rather than pushed right after Begin() because
				// the guard has to release before ImGui::End() closes the window
				// it was pushed on.
				EditorWidgets::WrapText wrap;
				ImGui::TextDisabled("  Viewport not available on this backend yet.");
			}
		}
		ImGui::End();
	}
}

#else  // !HE_IMGUI_ENABLED

void releaseViewportLookCapture(SDL_Window*)          {}
void enforceViewportLookCaptureInvariant(SDL_Window*) {}
void renderSizePx(int& outW, int& outH)               { outW = 0; outH = 0; }
void render(AppContext&, float)                       {}

#endif // HE_IMGUI_ENABLED

} // namespace ViewportPanel
