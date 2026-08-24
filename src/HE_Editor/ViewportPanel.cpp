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
							viewportHovered,
							viewportHovered ? io.MouseWheel : 0.0f);
					}
				}
				else if (ctx.editorCamera)
				{
					EditorCamera& cam = *ctx.editorCamera;
					const bool imageHovered = ImGui::IsItemHovered();

					EditorCamera::Input cin;
					navigating = EditorViewportNav::gather(ctx, navOwner(), imageHovered,
					                                      dt, avail.y, cin);
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
