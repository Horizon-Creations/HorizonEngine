#include "ViewportPanel.h"
#include <cstdint>
#include "EditorApplication.h"           // AppContext, EditorCamera, EditorUndo
#include "EditorInput.h"                 // pointer-device grammar (trackpad swipe vs mouse wheel)
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
#include <glm/gtc/type_ptr.hpp>
#include <Diagnostics/Logger.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <ImGuizmo.h>
#endif

namespace ViewportPanel
{

#ifdef HE_IMGUI_ENABLED

// Last viewport RENDER resolution in framebuffer pixels (HiDPI-aware), captured when
// the viewport panel is drawn and shown in the footer beside the FPS counter.
static int s_viewportPxW = 0;
static int s_viewportPxH = 0;

// RMB fly-look capture state for the Scene viewport (SDL relative-mouse mode). File-
// scope (not a viewport-local static) so the capture can be force-released from paths
// that DON'T draw the viewport — e.g. switching to a material/script tab mid-look via a
// keyboard shortcut. Otherwise relative mode + the ImGui NoMouse flag stay latched and
// the cursor is hidden/pinned with no way out but quitting.
static bool  s_rmbCaptured = false;
static float s_rmbStartX   = 0.f;
static float s_rmbStartY   = 0.f;
// Trackpad fly TOGGLE: on a pad, holding RMB for the whole flight means pressing
// with two fingers the whole time — so in trackpad mode a two-finger TAP
// (a right-click) toggles fly mode on/off instead, and no button is held while
// flying. File-scope for the same reason as s_rmbCaptured, and because the
// capture invariant below must know a toggled fly is not a leaked capture.
static bool  s_flyToggle   = false;

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

void releaseViewportLookCapture(SDL_Window* win)
{
	if (!s_rmbCaptured) return;
	ImGuiIO& io = ImGui::GetIO();
	if (win)
	{
		SDL_WarpMouseInWindow(win, s_rmbStartX, s_rmbStartY);
		SDL_SetWindowRelativeMouseMode(win, false);
	}
	SDL_ShowCursor();
	io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
	io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
	s_rmbCaptured = false;
	// Every teardown path runs through here, so the fly toggle can never outlive
	// its capture (tab switch, play mode, Esc — all of them land here).
	s_flyToggle   = false;
}

// Belt-and-suspenders invariant, run once per frame BEFORE any early-out: fly-look
// capture must never outlive a physically-held right mouse button. If the OS reports RMB
// is not down but we're still flagged as captured, force-release — this recovers from any
// path that latched the capture without releasing it (tab switch, focus change, a stale
// ImGui button state that spuriously (re)engaged look). Reads the PHYSICAL SDL button
// state, not ImGui's io.MouseDown (which NoMouse zeroes during a real look), so an actual
// fly-look is never cut short.
void enforceViewportLookCaptureInvariant(SDL_Window* win)
{
	if (!s_rmbCaptured) return;
	// A toggled (trackpad) fly holds NO button by design — the invariant below
	// only guards the held-RMB flavour of the capture.
	if (s_flyToggle) return;
	const bool rmbDown =
		(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) != 0;
	if (!rmbDown) releaseViewportLookCapture(win);
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
				if (ctx.editorCamera && ctx.isPlaying)
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
							viewportHovered);
					}
				}
				else if (ctx.editorCamera)
				{
					EditorCamera& cam = *ctx.editorCamera;
					const bool imageHovered = ImGui::IsItemHovered();
					const bool trackpad = EditorInput::trackpadPointer(ctx);

					// Trackpad fly toggle: a two-finger TAP (right-click) over the
					// viewport enters fly mode, a second tap — or Esc — leaves it.
					// While flying, look and WASDQE/Shift work with nothing held.
					// The edge is read from the PHYSICAL SDL button, not ImGui:
					// mid-fly the NoMouse flag zeroes ImGui's mouse state, so the
					// exit tap would be invisible to IsMouseClicked.
					const bool physRmb =
						(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) != 0;
					static bool s_prevPhysRmb = false;
					const bool physRmbTap = physRmb && !s_prevPhysRmb;
					s_prevPhysRmb = physRmb;
					if (trackpad)
					{
						if (physRmbTap)
						{
							if (s_flyToggle)          endLookCapture();   // clears the toggle too
							else if (imageHovered && !io.KeyAlt) s_flyToggle = true;
						}
						if (s_flyToggle && ImGui::IsKeyPressed(ImGuiKey_Escape))
							endLookCapture();
					}

					// In trackpad mode "RMB held" is replaced by the toggle; with a
					// mouse it stays the physically held button, exactly as before.
					const bool rmb    = trackpad ? s_flyToggle
					                             : ImGui::IsMouseDown(ImGuiMouseButton_Right);
					const bool mmb    = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
					const bool altLmb = io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left);
					const bool anyNav = rmb || mmb || altLmb;

					// Latch navigation so a drag keeps going if the cursor leaves the image.
					static bool s_navActive = false;
					if (imageHovered && anyNav) s_navActive = true;
					if (!anyNav)                s_navActive = false;
					navigating = s_navActive;

					// RMB fly-look capture: put the window into relative-mouse mode so we
					// read raw OS motion deltas (event.motion.xrel/yrel) instead of the
					// absolute cursor position. The previous approach sampled the absolute
					// position once per frame and warped the cursor back to the press point,
					// which discarded the sub-pixel remainder every frame. At high frame
					// rates the per-frame movement is tiny, so that cumulative loss (plus OS
					// pointer-acceleration) made looking feel sluggish and frame-rate
					// dependent. Relative mode delivers acceleration-free, frame-rate-
					// independent deltas with no warping and no display-edge collisions.
					//
					// Capture tracks the look predicate EXACTLY (rmb && !altLmb): Alt+LMB
					// is the orbit gesture, which needs a visible cursor and io.MouseDelta,
					// so engaging Alt mid-RMB must drop relative mode — otherwise orbit
					// freezes and the stale accumulator snaps the view when look resumes.
					if (sdlWin)
					{
						// Engage on a FRESH right-press over the viewport (click edge), never on
						// "RMB happens to be down" — otherwise arriving on the Scene tab with a
						// stale/held button state would capture the cursor without the user
						// starting a look here. In trackpad mode the fresh toggle IS the edge.
						const bool rmbClicked = trackpad
							? (s_flyToggle && !s_rmbCaptured)
							: ImGui::IsMouseClicked(ImGuiMouseButton_Right);
						if (rmbClicked && !altLmb && imageHovered && !s_rmbCaptured)
						{
							SDL_GetMouseState(&s_rmbStartX, &s_rmbStartY);
							SDL_SetWindowRelativeMouseMode(sdlWin, true);
							SDL_HideCursor(); // relative mode alone doesn't reliably hide the OS cursor (SDL3/macOS)
							// Discard any relative motion accumulated before capture so the
							// first look frame doesn't jump by a stale delta.
							SDL_GetRelativeMouseState(nullptr, nullptr);
							s_rmbCaptured = true;
						}
						else if ((!rmb || altLmb) && s_rmbCaptured)
						{
							endLookCapture();
						}
					}

					EditorCamera::Input cin;
					cin.dt             = dt;
					cin.viewportHeight = avail.y;
					if (navigating)
					{
						cin.orbit      = altLmb;
						cin.pan        = mmb && !altLmb;
						cin.look       = rmb && !altLmb;
						cin.mouseDelta = glm::vec2(io.MouseDelta.x, io.MouseDelta.y);
						if (cin.look)
						{
							// Relative mode keeps the OS cursor pinned, so this is the raw
							// frame motion delta — no warp, no absolute-position truncation.
							if (s_rmbCaptured && sdlWin)
							{
								float rx = 0.f, ry = 0.f;
								SDL_GetRelativeMouseState(&rx, &ry);
								cin.mouseDelta = glm::vec2(rx, ry);
								io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // don't let ImGui re-show it mid-look
								io.ConfigFlags |= ImGuiConfigFlags_NoMouse;            // block ImGui hover/click while free-looking
								SDL_HideCursor();
								ImGui::SetMouseCursor(ImGuiMouseCursor_None);
							}
							cin.fast = io.KeyShift;
							if (ImGui::IsKeyDown(ImGuiKey_D)) cin.moveAxis.x += 1.0f;
							if (ImGui::IsKeyDown(ImGuiKey_A)) cin.moveAxis.x -= 1.0f;
							if (ImGui::IsKeyDown(ImGuiKey_E)) cin.moveAxis.y += 1.0f;
							if (ImGui::IsKeyDown(ImGuiKey_Q)) cin.moveAxis.y -= 1.0f;
							if (ImGui::IsKeyDown(ImGuiKey_W)) cin.moveAxis.z += 1.0f;
							if (ImGui::IsKeyDown(ImGuiKey_S)) cin.moveAxis.z -= 1.0f;
						}
					}
					// Wheel zoom works on hover without holding a button — on every
					// pointer device. (A swipe-orbits variant was tried here and
					// reverted: it took the bare scroll away from the dolly, which
					// reads as "zoom is broken" on a pad. Trackpad navigation is the
					// fly TOGGLE above plus Alt+drag orbit; swipe-pan belongs to the
					// 2D canvases.)
					if (imageHovered) cin.wheel = io.MouseWheel;

					// Focus on selection (F) — frame the selected entity.
					if (imageHovered && !io.WantTextInput && !navigating &&
					    ImGui::IsKeyPressed(ImGuiKey_F) &&
					    ctx.world && ctx.selectedEntity != entt::null &&
					    ctx.world->registry().valid(ctx.selectedEntity))
					{
						if (auto* t = ctx.world->registry().try_get<TransformComponent>(ctx.selectedEntity))
						{
							const glm::vec3 center = glm::vec3(t->worldMatrix[3]);
							const float     radius = glm::length(t->scale) * 0.75f + 0.5f;
							cam.focusOn(center, radius);
						}
					}

					cam.update(cin);
					// Push to the backend so this frame's render uses it.
					ctx.renderer->SetEditorCamera(cam.makeOverride());

					// ── Self-diagnostic (throttled ~once/sec, only while it matters) ──
					// The field report is "WASD stopped working after joining a session
					// on Windows", and every link in the chain fails silently: hover,
					// the nav latch, relative-mouse capture, key delivery. Logging the
					// state of each while the user is actually trying makes the next
					// report name the broken link instead of the symptom.
					{
						const bool tryingToNavigate =
							ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
							ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_S);
						static int s_navDiagFrames = 0;
						if (tryingToNavigate && ++s_navDiagFrames >= 60)
						{
							s_navDiagFrames = 0;
							HE_LOG_INFO(Editor,
								"Edit-nav diagnostic: hovered=%d navigating=%d look=%d "
								"captured=%d relMode=%d W=%d rmb=%d wantKbd=%d wantTxt=%d "
								"focusWin=%s",
								static_cast<int>(imageHovered),
								static_cast<int>(navigating),
								static_cast<int>(cin.look),
								static_cast<int>(s_rmbCaptured),
								static_cast<int>(sdlWin && SDL_GetWindowRelativeMouseMode(sdlWin)),
								static_cast<int>(ImGui::IsKeyDown(ImGuiKey_W)),
								static_cast<int>(ImGui::IsMouseDown(ImGuiMouseButton_Right)),
								static_cast<int>(io.WantCaptureKeyboard),
								static_cast<int>(io.WantTextInput),
								SDL_GetKeyboardFocus() == sdlWin ? "main" : "OTHER");
						}
						if (!tryingToNavigate) s_navDiagFrames = 0;
					}
				}

				// Camera + object snapshot, identical to what the backend
				// renders with (extractor recomputes world matrices). The
				// editor camera overrides any scene camera so the gizmo and
				// picking ray match exactly what is on screen.
				static RenderExtractor s_extractor;
				static RenderWorld     s_sceneSnapshot;
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
								Entity e = ctx.world->createEntity(mesh->name);
								TransformComponent tc; tc.position = spawnPos;
								ctx.world->addComponent(e, tc);
								ctx.world->addComponent(e, MeshComponent{ .meshAssetId = id });
								ctx.world->markHierarchyDirty();
								ctx.selectedEntity = e; // select the freshly spawned mesh
								HE_LOG_INFO(Editor, "%s",
									("Editor: spawned '" + mesh->name + "' into the scene via drag-drop").c_str());
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


				// Suppress the gizmo while the camera is being driven so Alt+LMB
				// orbit and RMB fly-look don't fight the manipulator.
				ImGuizmo::Enable(!navigating && !io.KeyAlt);

				// ── Gizmo on the selected entity ────────────────────────────
				// Suppressed in Landscape mode: there LMB belongs to the sculpt
				// brush, and a stray gizmo drag would silently move/scale the
				// terrain — which then breaks the brush's world↔grid mapping.
				bool gizmoActive = false;
				if (ctx.editorConfig.mode != EditorMode::Landscape &&
				    ctx.world && ctx.selectedEntity != entt::null &&
				    ctx.world->registry().valid(ctx.selectedEntity))
				if (auto* t = ctx.world->registry().try_get<TransformComponent>(ctx.selectedEntity))
				{
					// W/E/R switch operation while the viewport is hovered
					// (but not while flying — W/A/S/D drive the camera then). The
					// toolbar's Move/Rotate/Scale cells set the same shared state.
					if (ImGui::IsWindowHovered() && !ImGui::GetIO().WantTextInput && !navigating)
					{
						if (ImGui::IsKeyPressed(ImGuiKey_W)) s_tb.op = ImGuizmo::TRANSLATE;
						if (ImGui::IsKeyPressed(ImGuiKey_E)) s_tb.op = ImGuizmo::ROTATE;
						if (ImGui::IsKeyPressed(ImGuiKey_R)) s_tb.op = ImGuizmo::SCALE;
					}

					ImGuizmo::SetOrthographic(false);
					ImGuizmo::SetDrawlist();
					ImGuizmo::SetRect(rectMin.x, rectMin.y,
					                  rectMax.x - rectMin.x, rectMax.y - rectMin.y);

					// Pre-state for undo — captured ONLY on the frame a gizmo drag is
					// about to begin (mouse pressed over the gizmo), NOT every frame.
					// capturePre() serializes the WHOLE world (expensive with terrain),
					// so the old per-frame call dropped the editor to ~15 ms the moment
					// anything was selected. IsOver()+MouseClicked fires once, just
					// before Manipulate first mutates the transform this frame.
					if (ctx.undoSys && !ImGuizmo::IsUsing() && ImGuizmo::IsOver()
					    && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
						ctx.undoSys->capturePre();

					// For rotation, optionally drop ImGuizmo's outer screen-space
					// ring (rotate about the view axis) — it's the confusing white
					// circle. Toggled from the toolbar's options popup.
					ImGuizmo::OPERATION effectiveOp = s_tb.op;
					if (s_tb.op == ImGuizmo::ROTATE && !s_tb.rotateScreenRing)
						effectiveOp = ImGuizmo::ROTATE_X | ImGuizmo::ROTATE_Y | ImGuizmo::ROTATE_Z;

					// While a drag is in progress the gizmo works on the matrix IT
					// produced last frame, NOT on the scene graph's freshly
					// recomposed worldMatrix. The round-trip
					//   TRS -> worldMatrix -> decompose -> TRS -> worldMatrix
					// is not an identity: DecomposeMatrixToComponents extracts an
					// Euler triple that is only *equivalent* to the authored one, so
					// each frame handed the gizmo a slightly different matrix and the
					// values visibly jittered mid-drag.
					static bool      s_gizmoWasUsing = false;
					static glm::mat4 s_gizmoWorld(1.0f);
					glm::mat4 world = s_gizmoWasUsing ? s_gizmoWorld : t->worldMatrix;
					// Snapping (toolbar): ImGuizmo quantises the drag to the
					// increment of whichever operation is armed, or moves freely
					// when activeSnap() hands back nullptr.
					ImGuizmo::Manipulate(
						&s_sceneSnapshot.camera.view[0][0],
						&s_sceneSnapshot.camera.projection[0][0],
						effectiveOp, s_tb.mode, &world[0][0],
						nullptr, s_tb.activeSnap());
					s_gizmoWorld = world;

					// Undo session: one entry per drag
					if (ctx.undoSys)
					{
						if (ImGuizmo::IsUsing() && !s_gizmoWasUsing) ctx.undoSys->stashPre();
						if (!ImGuizmo::IsUsing() && s_gizmoWasUsing) ctx.undoSys->commitPending();
					}
					s_gizmoWasUsing = ImGuizmo::IsUsing();

					gizmoActive = ImGuizmo::IsOver() || ImGuizmo::IsUsing();

					if (ImGuizmo::IsUsing())
					{
						// world → local: divide out the parent's world matrix
						glm::mat4 parentWorld(1.0f);
						if (auto* h = ctx.world->registry().try_get<HierarchyComponent>(ctx.selectedEntity);
						    h && h->parent != entt::null)
							if (auto* pt = ctx.world->registry().try_get<TransformComponent>(h->parent))
								parentWorld = pt->worldMatrix;
						glm::mat4 local = glm::inverse(parentWorld) * world;

						float pos[3], rot[3], scale[3];
						ImGuizmo::DecomposeMatrixToComponents(&local[0][0], pos, rot, scale);
						// Write back ONLY the channels this operation manipulates. A
						// scale drag used to overwrite rotation with the re-extracted
						// (equivalent but different) Euler triple and vice versa —
						// visible as a value that jumps the moment you touch an
						// unrelated handle.
						const unsigned opBits = static_cast<unsigned>(effectiveOp);
						if (opBits & static_cast<unsigned>(ImGuizmo::TRANSLATE))
							t->position = { pos[0], pos[1], pos[2] };
						if (opBits & static_cast<unsigned>(ImGuizmo::ROTATE))
							t->rotation = { rot[0], rot[1], rot[2] };
						if (opBits & (static_cast<unsigned>(ImGuizmo::SCALE) |
						              static_cast<unsigned>(ImGuizmo::SCALEU)))
							t->scale = { scale[0], scale[1], scale[2] };
						t->dirty = true;
					}
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
					// Entities without an asset use the built-in fallback cube's box.
					static const HE::AABB s_cubeBox = []{
						HE::AABB b; b.expand({-0.5f,-0.5f,-0.5f}); b.expand({0.5f,0.5f,0.5f}); return b;
					}();

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
						HE::AABB box = s_cubeBox;
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
