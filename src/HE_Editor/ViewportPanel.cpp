#include "ViewportPanel.h"
#include <HorizonScene/Components/MaterialComponent.h> // a spawned mesh follows its MREF material
#include <cstdint>
#include "EditorApplication.h"           // AppContext, EditorCamera, EditorUndo
#include "EditorInput.h"                 // pointer-device grammar (trackpad swipe vs mouse wheel)
#include "EditorViewportNav.h"           // shared orbit/pan/fly gesture grammar + look capture
#include "EditorTransformGizmo.h"        // shared move/rotate/scale gizmo
#include "TerrainTools.h"                // Landscape brush cursor + sculpt stroke
#include "CollabPresenceBar.h"           // name tags for the other people in the session
#include "ViewportToolbar.h"             // the strip along the top of the Scene window
#include "EditorWidgets.h"               // WrapText — text wraps at the pane edge, never runs off it
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

// ── Ground grid ─────────────────────────────────────────────────────────────
// Outside the ImGui guard on purpose: this is pure geometry pushed into a debug
// line buffer, and keeping the toggle next to the only code that reads it is
// what stops a second copy of "is the grid on" from appearing.
static bool s_groundGrid = true;

bool groundGridEnabled()        { return s_groundGrid; }
void setGroundGridEnabled(bool on) { s_groundGrid = on; }

void appendGroundGrid(const EditorCamera& cam, bool playing, DebugDrawBuffer& out)
{
	if (playing || !s_groundGrid) return;

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

// What an entity without a readable mesh asset is measured as: the built-in
// fallback cube's own local box. Shared by picking and by the F-key framing, so
// clicking a thing and focusing it never disagree about how big it is.
static const HE::AABB s_fallbackBox = []{
	HE::AABB b; b.expand({ -0.5f, -0.5f, -0.5f }); b.expand({ 0.5f, 0.5f, 0.5f }); return b;
}();

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
static bool selectionFocusSphere(HorizonWorld& world, ContentManager* cm, Entity sel,
                                 const RenderWorld& snapshot,
                                 glm::vec3& centerOut, float& radiusOut)
{
	auto& reg = world.registry();
	if (!reg.valid(sel)) return false;

	std::unordered_set<uint32_t> subtree;
	collectSubtree(reg, sel, subtree);

	HE::AABB geometry;   // what the selection actually draws
	HE::AABB pivots;     // where its entities sit, drawn or not

	auto expandFromObject = [&](const RenderObject& obj)
	{
		if (subtree.find(obj.entityId) == subtree.end()) return;
		const HE::AABB* local = (cm && obj.meshAssetId != HE::UUID{})
		                      ? meshBounds(*cm, obj.meshAssetId) : nullptr;
		geometry.expand((local ? *local : s_fallbackBox).transformed(obj.transform));
	};
	for (const RenderObject& obj : snapshot.objects)               expandFromObject(obj);
	for (const SkinnedRenderObject& obj : snapshot.skinnedObjects) expandFromObject(obj);

	for (const uint32_t raw : subtree)
		if (const auto* t = reg.try_get<TransformComponent>(static_cast<Entity>(raw)))
			pivots.expand(glm::vec3(t->worldMatrix[3]));

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
				static RenderExtractor s_extractor;
				static RenderWorld     s_sceneSnapshot;

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
					// Focus on selection (F) — frame the selected entity and
					// everything parented under it (see selectionFocusSphere).
					if (imageHovered && !io.WantTextInput && !navigating &&
					    ImGui::IsKeyPressed(ImGuiKey_F) &&
					    ctx.world && ctx.selectedEntity != entt::null &&
					    ctx.world->registry().valid(ctx.selectedEntity))
					{
						glm::vec3 center(0.0f);
						float     radius = 0.0f;
						if (selectionFocusSphere(*ctx.world, ctx.contentManager,
						                         ctx.selectedEntity, s_sceneSnapshot,
						                         center, radius))
							cam.focusOn(center, radius);
					}

					cam.update(cin);
					// Push to the backend so this frame's render uses it.
					ctx.renderer->SetEditorCamera(cam.makeOverride());
				}

				const EditorCameraOverride camOverride =
					(ctx.editorCamera && !ctx.isPlaying) ? ctx.editorCamera->makeOverride()
					                                     : EditorCameraOverride{};
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

							if (ctx.undoSys) ctx.undoSys->snapshotNow();
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
								ctx.selectedEntity = e; // select the freshly spawned mesh
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
								const Entity root = ser.instantiatePrefab(*ctx.world, prefab->data);
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
									ctx.world->markHierarchyDirty();
									ctx.selectedEntity = root;
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


				// ── Gizmo on the selected entity ────────────────────────────
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
					gizmoActive = EditorTransformGizmo::manipulate(
						*ctx.world, ctx.selectedEntity,
						s_sceneSnapshot.camera.view, s_sceneSnapshot.camera.projection,
						rectMin, rectMax, s_tb,
						/*enabled=*/!navigating && !io.KeyAlt, ctx.undoSys);
				}
				// ── Picking: click in the viewport selects the hit entity ──
				// Disabled in Landscape mode so a brush stroke can't deselect /
				// reselect entities and pop the gizmo back up mid-sculpt.
				if (ctx.editorConfig.mode != EditorMode::Landscape &&
				    ctx.world && !gizmoActive && !navigating && !io.KeyAlt &&
				    ImGui::IsItemClicked(ImGuiMouseButton_Left))
				{
					const ImVec2 mouse = ImGui::GetMousePos();
					const float  u = (mouse.x - rectMin.x) / (rectMax.x - rectMin.x);
					const float  v = (mouse.y - rectMin.y) / (rectMax.y - rectMin.y);

					// Unproject the click to a world-space ray
					const glm::mat4 invVP = glm::inverse(
						s_sceneSnapshot.camera.projection * s_sceneSnapshot.camera.view);
					const glm::vec4 ndcNear(2.0f * u - 1.0f, 1.0f - 2.0f * v, -1.0f, 1.0f);
					const glm::vec4 ndcFar (ndcNear.x, ndcNear.y, 1.0f, 1.0f);
					glm::vec4 pNear = invVP * ndcNear; pNear /= pNear.w;
					glm::vec4 pFar  = invVP * ndcFar;  pFar  /= pFar.w;
					const glm::vec3 rayOrigin(pNear);
					const glm::vec3 rayDir(glm::vec3(pFar) - glm::vec3(pNear));

					// Local-space AABBs per mesh asset, cached (file-scope).
					// Entities without an asset use the built-in fallback cube's
					// box — s_fallbackBox, the same one the F-key framing measures.

					// Real mesh entities take priority over terrain: a terrain
					// chunk's bounding box is huge and loose (its near face sits
					// closer to the camera than a small mesh resting on the
					// surface), so an AABB pick would otherwise select the ground
					// under every object. Track the two categories separately and
					// only fall back to terrain when nothing else is under the
					// cursor — and then select the owning Landscape entity, never
					// a raw auto-generated chunk.
					auto& reg = ctx.world->registry();
					Entity meshHit    = entt::null; float meshDist    = std::numeric_limits<float>::max();
					Entity terrainHit = entt::null; float terrainDist = std::numeric_limits<float>::max();
					for (const RenderObject& obj : s_sceneSnapshot.objects)
					{
						HE::AABB box = s_fallbackBox;
						if (obj.meshAssetId != HE::UUID{} && ctx.contentManager)
							if (const HE::AABB* b = meshBounds(*ctx.contentManager, obj.meshAssetId))
								box = *b;

						// Ray → object space (exact test for rotated objects)
						const glm::mat4 invModel = glm::inverse(obj.transform);
						const glm::vec3 o = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
						const glm::vec3 d = glm::vec3(invModel * glm::vec4(rayDir,    0.0f));

						float t = 0.0f;
						if (!box.intersectRay(o, d, t)) continue;

						const Entity e = static_cast<Entity>(obj.entityId);
						Entity terrainOwner = entt::null;
						if (reg.valid(e))
						{
							if (auto* cc = reg.try_get<TerrainChunkComponent>(e)) terrainOwner = cc->terrain;
							else if (reg.all_of<TerrainComponent>(e))            terrainOwner = e;
						}

						if (terrainOwner != entt::null)
						{
							if (t < terrainDist) { terrainDist = t; terrainHit = terrainOwner; }
						}
						else if (t < meshDist)
						{
							meshDist = t; meshHit = e;
						}
					}
					// miss = deselect
					ctx.selectedEntity = (meshHit != entt::null) ? meshHit : terrainHit;
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
				CollabPresenceBar::DrawViewportMarkers(
					ctx, s_sceneSnapshot.camera.view, s_sceneSnapshot.camera.projection,
					rectMin.x, rectMin.y, rectMax.x, rectMax.y);
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
