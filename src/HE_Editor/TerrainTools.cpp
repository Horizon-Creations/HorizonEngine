#include "TerrainTools.h"
#include "EditorToolbar.h"   // shared toolbar strip

#include <cstdio>
#include "EditorApplication.h"           // AppContext
#include "EditorWidgets.h"               // shared Content-Browser asset drop slot
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/TerrainPaint.h>   // landscape layer brush
#include <HorizonRendering/RenderWorld.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Types/Enums.h>
#include <Diagnostics/Logger.h>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace TerrainTools
{

// Landscape sculpt tool state (shared between the panel and viewport)
enum class TerrainTool { Raise, Lower, Smooth, Flatten, Ramp, Roughen };
static TerrainTool s_terrainTool     = TerrainTool::Raise;
// Landscape PAINT mode: the same brush writes layer weights instead of heights.
// The paintable layers come from the assigned material's Landscape Layer Blend
// node (MaterialAsset::graphLayerNames) — the material defines what a layer is.
static bool        s_landscapePaint  = false;
static int         s_paintLayer      = 0;
static float       s_brushRadius     = 10.0f;  // inner full-strength radius (m)
static float       s_falloffRadius   = 5.0f;   // transition width — strength falls linearly to 0
static float       s_brushStrength   = 5.0f;
static bool        s_brushWasDown    = false;   // tracks LMB edge for undo
// Stroke-scoped state, captured on the LMB-down edge (see the sculpt block):
static float       s_flattenTarget   = 0.0f;    // Flatten: height to pull toward
static glm::vec3   s_rampStartWS{};             // Ramp: world-space start of the gradient
static float       s_rampStartH      = 0.0f;    // Ramp: terrain height at the start point
static bool        s_rampValid       = false;   // Ramp: a start point was captured this stroke

void sculptInViewport(AppContext& ctx, const RenderWorld& sceneSnapshot,
                      const ImVec2& rectMin, const ImVec2& rectMax,
                      bool navigating, bool viewportHovered, float dt,
                      const std::function<void(const HE::UUID&)>& invalidateMeshAabb)
{
#ifdef HE_IMGUI_ENABLED
	// Alias of the enclosing viewport scope this block was lifted out of.
	ImGuiIO& io = ImGui::GetIO();

	// ── Landscape brush cursor + sculpt ────────────────────────
	if (ctx.editorConfig.mode == EditorMode::Landscape && ctx.world)
	{
		auto& terrainReg = ctx.world->registry();
		auto  tvw        = terrainReg.view<TerrainComponent>();
		if (!tvw.empty())
		{
			Entity terrainEnt = tvw.front();
			auto&  tc         = terrainReg.get<TerrainComponent>(terrainEnt);

			// Terrain world translation. The brush works in world XZ but
			// the height grid is local, so every world↔grid conversion has
			// to subtract / add this offset (recovers a terrain that an
			// earlier stray gizmo drag may have displaced).
			glm::vec3 terrainWorldPos(0.0f);
			if (const auto* xf = terrainReg.try_get<TransformComponent>(terrainEnt))
				terrainWorldPos = xf->position;
			const float terrainWorldY = terrainWorldPos.y;

			// ── Terrain height sampler (bilinear, local space) ────────
			// Returns the sculpted height at world XZ; 0 if no sculpt data.
			const uint32_t tcRes   = std::clamp(tc.resolution, 2u, 1024u);
			const float    tcHalfX = tc.sizeX * 0.5f;
			const float    tcHalfZ = tc.sizeZ * 0.5f;
			const float    tcStepX = tc.sizeX / static_cast<float>(tcRes - 1);
			const float    tcStepZ = tc.sizeZ / static_cast<float>(tcRes - 1);

			auto sampleH = [&](float wx, float wz) -> float
			{
				const float lx = wx - terrainWorldPos.x;  // world → local XZ
				const float lz = wz - terrainWorldPos.z;
				// No baked heights yet (e.g. a freshly-loaded seeded
				// terrain): sample the generated surface so the cursor
				// still tracks fBm bumps. Brushes bake on first stroke.
				if (tc.sculptHeights.empty())
					return terrainHeightAt(tc, lx, lz);
				const float gx = std::clamp((lx + tcHalfX) / tcStepX,
				                            0.0f, static_cast<float>(tcRes - 1));
				const float gz = std::clamp((lz + tcHalfZ) / tcStepZ,
				                            0.0f, static_cast<float>(tcRes - 1));
				const int xi0 = static_cast<int>(gx);
				const int zi0 = static_cast<int>(gz);
				const int xi1 = std::min(xi0 + 1, static_cast<int>(tcRes) - 1);
				const int zi1 = std::min(zi0 + 1, static_cast<int>(tcRes) - 1);
				const float fx = gx - static_cast<float>(xi0);
				const float fz = gz - static_cast<float>(zi0);
				const float h00 = tc.sculptHeights[zi0 * tcRes + xi0];
				const float h10 = tc.sculptHeights[zi0 * tcRes + xi1];
				const float h01 = tc.sculptHeights[zi1 * tcRes + xi0];
				const float h11 = tc.sculptHeights[zi1 * tcRes + xi1];
				return h00*(1-fx)*(1-fz) + h10*fx*(1-fz)
				     + h01*(1-fx)*fz     + h11*fx*fz;
			};

			// ── Unproject mouse → refined terrain surface hit ─────────
			const ImVec2 mouse = ImGui::GetMousePos();
			const bool mouseInViewport =
				mouse.x >= rectMin.x && mouse.x <= rectMax.x &&
				mouse.y >= rectMin.y && mouse.y <= rectMax.y;

			bool      hasHit = false;
			glm::vec3 hitWS{};

			const glm::mat4 VP    = sceneSnapshot.camera.projection
			                      * sceneSnapshot.camera.view;
			const glm::mat4 invVP = glm::inverse(VP);

			if (mouseInViewport)
			{
				const float u = (mouse.x - rectMin.x) / (rectMax.x - rectMin.x);
				const float v = (mouse.y - rectMin.y) / (rectMax.y - rectMin.y);
				const glm::vec4 ndcNear(2.0f*u-1.0f, 1.0f-2.0f*v, -1.0f, 1.0f);
				const glm::vec4 ndcFar (ndcNear.x, ndcNear.y,       1.0f, 1.0f);
				glm::vec4 pNear = invVP * ndcNear; pNear /= pNear.w;
				glm::vec4 pFar  = invVP * ndcFar;  pFar  /= pFar.w;
				const glm::vec3 rayOrigin(pNear);
				const glm::vec3 rayDir = glm::normalize(glm::vec3(pFar) - glm::vec3(pNear));
				const float denom = rayDir.y;
				if (std::abs(denom) > 1e-5f)
				{
					// Start at the flat base plane (Y = terrainWorldY), then
					// fixed-point iterate t so the hit lands on the actual
					// sculpted surface Y = terrainWorldY + h(p.xz). One step
					// suffices for gentle slopes; a few more keep the cursor
					// glued to the surface on steep, heavily-sculpted terrain.
					float t = (terrainWorldY - rayOrigin.y) / denom;
					if (t > 0.0f)
					{
						glm::vec3 p = rayOrigin + t * rayDir;
						for (int it = 0; it < 8; ++it)
						{
							const float surfaceY = terrainWorldY + sampleH(p.x, p.z);
							const float tn = (surfaceY - rayOrigin.y) / denom;
							if (tn <= 0.0f) break;
							const bool converged = std::abs(tn - t) < 1e-3f;
							t = tn;
							p = rayOrigin + t * rayDir;
							if (converged) break;
						}
						if (t > 0.0f)
						{
							hitWS  = p;
							hasHit = true;
						}
					}
				}
			}

			// ── Draw brush circles on the terrain surface ─────────────
			if (hasHit && !navigating)
			{
				ImDrawList* dl    = ImGui::GetWindowDrawList();
				const float viewW = rectMax.x - rectMin.x;
				const float viewH = rectMax.y - rectMin.y;

				// Project a world XZ point at its actual terrain height → screen
				auto projectPt = [&](float wx, float wz, ImVec2& outPt) -> bool
				{
					const float wy = terrainWorldY + sampleH(wx, wz);
					glm::vec4 clip = VP * glm::vec4(wx, wy, wz, 1.0f);
					if (clip.w <= 0.0f) return false;
					clip /= clip.w;
					if (clip.z < -1.0f || clip.z > 1.0f) return false;
					outPt = ImVec2(
						rectMin.x + (clip.x * 0.5f + 0.5f) * viewW,
						rectMin.y + (0.5f   - clip.y * 0.5f) * viewH);
					return true;
				};

				constexpr int   kSeg = 48;
				constexpr float kPi2 = 6.28318530f;
				const float totalR   = s_brushRadius + s_falloffRadius;

				for (int ci = 0; ci < 2; ++ci)
				{
					const float r     = (ci == 0) ? s_brushRadius : totalR;
					const ImU32 col   = (ci == 0) ? IM_COL32(255,255,255,210)
					                              : IM_COL32(180,180,180,120);
					const float thick = (ci == 0) ? 1.5f : 1.0f;
					if (r < 0.01f) continue;

					ImVec2 prev{}; bool prevValid = false;
					for (int i = 0; i <= kSeg; ++i)
					{
						const float a = kPi2 * i / kSeg;
						ImVec2 cur{}; bool curValid = projectPt(
							hitWS.x + r * std::cos(a),
							hitWS.z + r * std::sin(a), cur);
						if (prevValid && curValid)
							dl->AddLine(prev, cur, col, thick);
						prev = cur; prevValid = curValid;
					}
				}

				// Ramp guide: a line from the stroke start to the cursor so
				// the gradient direction is visible while dragging.
				if (s_terrainTool == TerrainTool::Ramp && s_rampValid &&
				    ImGui::IsMouseDown(ImGuiMouseButton_Left))
				{
					ImVec2 ps{}, pe{};
					if (projectPt(s_rampStartWS.x, s_rampStartWS.z, ps) &&
					    projectPt(hitWS.x, hitWS.z, pe))
						dl->AddLine(ps, pe, IM_COL32(120,200,255,230), 2.0f);
				}
			}

			// ── Apply brush on LMB drag ───────────────────────────────
			const bool lmbDown =
				ImGui::IsMouseDown(ImGuiMouseButton_Left) && !io.KeyAlt
				&& (viewportHovered || s_brushWasDown);

			// ── Paint mode: layer weights, not heights ────────────
			// Shares the brush shape/radius/falloff and the terrain hit
			// with sculpting; only the target data differs. The sculpt
			// blocks below all hang off `sculptDown`, which paint mode
			// forces false — so a paint stroke can never move geometry.
			if (s_landscapePaint)
			{
				if (lmbDown && !s_brushWasDown && ctx.undoSys)
					ctx.undoSys->snapshotNow();   // one undo entry per stroke
				if (lmbDown && hasHit)
				{
					// World → terrain-local (the brush works in world XZ).
					const float lx = hitWS.x - terrainWorldPos.x;
					const float lz = hitWS.z - terrainWorldPos.z;
					// Per-second like the sculpt tools, but as a 0..1 blend
					// factor: the shared 0.1..50 strength slider maps onto a
					// usable paint rate here.
					const float amount = std::clamp(
						s_brushStrength * static_cast<float>(dt) * 0.16f, 0.0f, 1.0f);
					TerrainPaint::paint(tc, lx, lz, s_paintLayer,
					                    s_brushRadius, s_falloffRadius, amount);
				}
			}
			// Sculpting is suppressed while painting.
			const bool sculptDown = lmbDown && !s_landscapePaint;

			if (sculptDown && !s_brushWasDown)
			{
				if (ctx.undoSys) ctx.undoSys->snapshotNow();
				// Lazy-init sculptHeights from current terrain geometry
				if (tc.sculptHeights.empty())
				{
					const size_t nVerts = static_cast<size_t>(tcRes) * tcRes;
					const StaticMeshAsset tmp = generateTerrainMesh(tc);
					tc.sculptHeights.resize(nVerts);
					for (size_t vi = 0; vi < nVerts; ++vi)
						tc.sculptHeights[vi] = tmp.vertices[vi * 3 + 1];
				}
				// Capture stroke-scoped targets (sculptHeights is now
				// populated, so sampleH returns the real local height).
				if (hasHit)
				{
					s_flattenTarget = sampleH(hitWS.x, hitWS.z);
					s_rampStartWS   = hitWS;
					s_rampStartH    = s_flattenTarget;
					s_rampValid     = true;
				}
				else
					s_rampValid = false;
			}
			s_brushWasDown = lmbDown;

			if (sculptDown && hasHit && !tc.sculptHeights.empty())
			{
				const float totalR  = s_brushRadius + s_falloffRadius;
				const float totalR2 = totalR * totalR;
				const float delta   = s_brushStrength * static_cast<float>(dt);
				bool anyChange = false;

				auto brushWeight = [&](float dist2) -> float
				{
					if (dist2 >= totalR2) return 0.0f;
					const float dist = std::sqrt(dist2);
					if (dist <= s_brushRadius) return 1.0f;
					if (s_falloffRadius < 0.001f) return 0.0f;
					return 1.0f - (dist - s_brushRadius) / s_falloffRadius;
				};

				if (s_terrainTool == TerrainTool::Smooth)
				{
					std::vector<float> smoothed = tc.sculptHeights;
					for (uint32_t zi = 0; zi < tcRes; ++zi)
					{
						for (uint32_t xi = 0; xi < tcRes; ++xi)
						{
							const float wx = terrainWorldPos.x - tcHalfX + static_cast<float>(xi) * tcStepX;
							const float wz = terrainWorldPos.z - tcHalfZ + static_cast<float>(zi) * tcStepZ;
							const float d2 = (hitWS.x-wx)*(hitWS.x-wx)
							               + (hitWS.z-wz)*(hitWS.z-wz);
							const float w  = brushWeight(d2);
							if (w <= 0.0f) continue;
							float sum = 0.0f; int cnt = 0;
							for (int dzi = -1; dzi <= 1; ++dzi)
								for (int dxi = -1; dxi <= 1; ++dxi)
								{
									const int ni = static_cast<int>(zi) + dzi;
									const int nj = static_cast<int>(xi) + dxi;
									if (ni >= 0 && ni < static_cast<int>(tcRes) &&
									    nj >= 0 && nj < static_cast<int>(tcRes))
									{ sum += tc.sculptHeights[ni*tcRes+nj]; ++cnt; }
								}
							const float avg = cnt > 0 ? sum/cnt
							                          : tc.sculptHeights[zi*tcRes+xi];
							// Blend factor: 10× the raise/lower rate so smooth
							// is visually comparable in speed.
							const float blend = std::min(w * delta * 10.0f, w);
							smoothed[zi*tcRes+xi] = tc.sculptHeights[zi*tcRes+xi]
							    + blend * (avg - tc.sculptHeights[zi*tcRes+xi]);
							anyChange = true;
						}
					}
					if (anyChange) tc.sculptHeights = std::move(smoothed);
				}
				else if (s_terrainTool == TerrainTool::Flatten)
				{
					// Pull heights toward the height sampled where the
					// stroke began — levels bumps without a fixed plane.
					for (uint32_t zi = 0; zi < tcRes; ++zi)
					for (uint32_t xi = 0; xi < tcRes; ++xi)
					{
						const float wx = terrainWorldPos.x - tcHalfX + static_cast<float>(xi) * tcStepX;
						const float wz = terrainWorldPos.z - tcHalfZ + static_cast<float>(zi) * tcStepZ;
						const float d2 = (hitWS.x-wx)*(hitWS.x-wx) + (hitWS.z-wz)*(hitWS.z-wz);
						const float w  = brushWeight(d2);
						if (w <= 0.0f) continue;
						const float blend = std::min(w * delta * 6.0f, w);
						float& h = tc.sculptHeights[zi*tcRes+xi];
						h += blend * (s_flattenTarget - h);
						anyChange = true;
					}
				}
				else if (s_terrainTool == TerrainTool::Ramp)
				{
					// Linear height gradient from the stroke start to the
					// cursor, inside a corridor the width of the brush.
					const glm::vec2 a(s_rampStartWS.x, s_rampStartWS.z);
					const glm::vec2 b(hitWS.x, hitWS.z);
					const glm::vec2 ab = b - a;
					const float L2 = glm::dot(ab, ab);
					if (s_rampValid && L2 > 1e-3f)
					{
						const float endH = sampleH(hitWS.x, hitWS.z);
						for (uint32_t zi = 0; zi < tcRes; ++zi)
						for (uint32_t xi = 0; xi < tcRes; ++xi)
						{
							const float wx = terrainWorldPos.x - tcHalfX + static_cast<float>(xi) * tcStepX;
							const float wz = terrainWorldPos.z - tcHalfZ + static_cast<float>(zi) * tcStepZ;
							const glm::vec2 p(wx, wz);
							const float t = std::clamp(glm::dot(p - a, ab) / L2, 0.0f, 1.0f);
							const glm::vec2 d = p - (a + t * ab);
							const float w = brushWeight(glm::dot(d, d)); // dist² to ramp line
							if (w <= 0.0f) continue;
							const float target = s_rampStartH + (endH - s_rampStartH) * t;
							const float blend  = std::min(w * delta * 6.0f, w);
							float& h = tc.sculptHeights[zi*tcRes+xi];
							h += blend * (target - h);
							anyChange = true;
						}
					}
				}
				else if (s_terrainTool == TerrainTool::Roughen)
				{
					// Stable per-vertex hash → consistent bumps, no shimmer.
					auto vhash = [](uint32_t xi, uint32_t zi) -> float
					{
						uint32_t n = xi * 73856093u ^ zi * 19349663u;
						n = (n ^ 61u) ^ (n >> 16u); n += n << 3u;
						n ^= n >> 4u; n *= 0x27D4EB2Du; n ^= n >> 15u;
						return static_cast<float>(n & 0x00FFFFFFu)
						     / static_cast<float>(0x01000000u) * 2.0f - 1.0f;
					};
					for (uint32_t zi = 0; zi < tcRes; ++zi)
					for (uint32_t xi = 0; xi < tcRes; ++xi)
					{
						const float wx = terrainWorldPos.x - tcHalfX + static_cast<float>(xi) * tcStepX;
						const float wz = terrainWorldPos.z - tcHalfZ + static_cast<float>(zi) * tcStepZ;
						const float d2 = (hitWS.x-wx)*(hitWS.x-wx) + (hitWS.z-wz)*(hitWS.z-wz);
						const float w  = brushWeight(d2);
						if (w <= 0.0f) continue;
						tc.sculptHeights[zi*tcRes+xi] += vhash(xi, zi) * w * delta;
						anyChange = true;
					}
				}
				else
				{
					const float sign = (s_terrainTool == TerrainTool::Raise) ? 1.0f : -1.0f;
					for (uint32_t zi = 0; zi < tcRes; ++zi)
					{
						for (uint32_t xi = 0; xi < tcRes; ++xi)
						{
							const float wx = terrainWorldPos.x - tcHalfX + static_cast<float>(xi) * tcStepX;
							const float wz = terrainWorldPos.z - tcHalfZ + static_cast<float>(zi) * tcStepZ;
							const float d2 = (hitWS.x-wx)*(hitWS.x-wx)
							               + (hitWS.z-wz)*(hitWS.z-wz);
							const float w  = brushWeight(d2);
							if (w <= 0.0f) continue;
							tc.sculptHeights[zi*tcRes+xi] += sign * w * delta;
							anyChange = true;
						}
					}
				}

				if (anyChange && ctx.contentManager)
				{
					if (const auto* mc = terrainReg.try_get<MeshComponent>(terrainEnt))
						invalidateMeshAabb(mc->meshAssetId);
					// Regenerate only the chunks the brush touched (terrain-local
					// XZ rect around the hit + brush extent), not all 64+ chunks —
					// otherwise interactive sculpting rebuilds the whole terrain each
					// frame. The first stroke still triggers a full build (the chunk
					// grid doesn't exist yet → TerrainSystem detects the grid change).
					const float r = s_brushRadius + s_falloffRadius;
					float mnX = hitWS.x - r, mxX = hitWS.x + r;
					float mnZ = hitWS.z - r, mxZ = hitWS.z + r;
					if (s_terrainTool == TerrainTool::Ramp && s_rampValid)
					{
						mnX = std::min(mnX, s_rampStartWS.x - r); mxX = std::max(mxX, s_rampStartWS.x + r);
						mnZ = std::min(mnZ, s_rampStartWS.z - r); mxZ = std::max(mxZ, s_rampStartWS.z + r);
					}
					tc.dirtyMinX = mnX - terrainWorldPos.x; tc.dirtyMaxX = mxX - terrainWorldPos.x;
					tc.dirtyMinZ = mnZ - terrainWorldPos.z; tc.dirtyMaxZ = mxZ - terrainWorldPos.z;
					tc.regionDirty = true;
					TerrainSystem::updateTerrains(*ctx.world, *ctx.contentManager,
					                              ctx.renderer);
				}
			}
		}
	}
#else
	(void)ctx; (void)sceneSnapshot; (void)rectMin; (void)rectMax;
	(void)navigating; (void)viewportHovered; (void)dt; (void)invalidateMeshAabb;
#endif // HE_IMGUI_ENABLED
}

// ── Landscape tool panel (body of the "Landscape###Quick Settings" window) ───
void renderPanel(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
    // ── Check for an existing terrain entity ─────────────────────────────
    auto& reg = ctx.world->registry();
    auto terrainView = reg.view<TerrainComponent>();
    const bool hasTerrain = !terrainView.empty();

    if (!hasTerrain)
    {
        // ── No terrain yet — show creation form ──────────────────────────
        // Parameters live on EditorConfig so the renderer can draw a 3D grid
        // preview of the terrain-to-be (see EditorApplication debug-draw).
        auto& np = ctx.editorConfig.newTerrain;
        ImGui::SeparatorText("Create Landscape");
        ImGui::TextDisabled("Green grid in the viewport previews the result");

        ImGui::DragFloat("Width (X)",    &np.sizeX,       1.0f,  1.0f, 10000.0f, "%.1f m");
        ImGui::DragFloat("Depth (Z)",    &np.sizeZ,       1.0f,  1.0f, 10000.0f, "%.1f m");
        ImGui::DragInt  ("Resolution",   &np.resolution,  1,     2,    512);
        ImGui::DragFloat("Height Scale", &np.heightScale, 0.5f,  0.0f, 1000.0f,  "%.1f m");
        ImGui::SeparatorText("Noise (seed 0 = flat)");
        ImGui::DragInt  ("Seed",         &np.seed,        1,     0,    0x7fffffff);
        ImGui::DragInt  ("Octaves",      &np.octaves,     1,     1,    8);
        ImGui::DragFloat("Frequency",    &np.frequency,   0.01f, 0.01f, 16.0f, "%.2f");
        ImGui::DragFloat("Lacunarity",   &np.lacunarity,  0.01f, 1.0f,  8.0f,  "%.2f");
        ImGui::DragFloat("Gain",         &np.gain,        0.01f, 0.0f,  1.0f,  "%.2f");

        ImGui::Spacing();
        const float btnW = 160.0f;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - btnW) * 0.5f
                             + ImGui::GetCursorPosX());
        if (ImGui::Button("Create Landscape", ImVec2(btnW, 0)))
        {
            if (ctx.undoSys) ctx.undoSys->snapshotNow();

            TerrainComponent tc;
            tc.sizeX      = np.sizeX;
            tc.sizeZ      = np.sizeZ;
            tc.resolution = static_cast<uint32_t>(std::clamp(np.resolution, 2, 1024));
            tc.heightScale= np.heightScale;
            tc.seed       = np.seed;
            tc.octaves    = np.octaves;
            tc.frequency  = np.frequency;
            tc.lacunarity = np.lacunarity;
            tc.gain       = np.gain;
            tc.dirty      = true;

            // Bake a seeded surface into editable per-vertex heights right
            // away. From here the terrain is a plain heightfield the brushes
            // edit directly — the seed/noise is a one-time creation input and
            // no longer feeds the mesh (sculptHeights overrides it). A flat
            // terrain (seed 0) is left empty and stays flat until first sculpt.
            if (tc.seed != 0)
            {
                const StaticMeshAsset gen = generateTerrainMesh(tc);
                const size_t nVerts = gen.vertices.size() / 3;
                tc.sculptHeights.resize(nVerts);
                for (size_t vi = 0; vi < nVerts; ++vi)
                    tc.sculptHeights[vi] = gen.vertices[vi * 3 + 1];
            }

            Entity e = ctx.world->createEntity("Terrain");
            ctx.world->addComponent(e, TransformComponent{});
            ctx.world->addComponent(e, tc);
            MaterialComponent mc;
            mc.materialAssetId = HE::kDefaultTerrainMaterialId;
            ctx.world->addComponent(e, mc);

            ctx.world->markHierarchyDirty();
            ctx.selectedEntity = e;
            HE_LOG_INFO(Editor, "%s", "Editor: created Terrain entity");
        }
    }
    else
    {
        // ── Landscape material ───────────────────────────────────────────
        // Right here, not only on the Terrain entity in the Outliner: the
        // material IS a landscape authoring tool (it defines the paint
        // layers), so it belongs in the Landscape panel. The Landscape's
        // MaterialComponent is what TerrainSystem propagates to the chunk
        // entities that actually render.
        {
            const Entity terrainEnt = terrainView.front();
            auto* tmat = reg.try_get<MaterialComponent>(terrainEnt);
            if (!tmat)
            {
                MaterialComponent mc; mc.materialAssetId = HE::kDefaultTerrainMaterialId;
                tmat = &reg.emplace_or_replace<MaterialComponent>(terrainEnt, mc);
            }
            const MaterialAsset* lmat =
                (tmat->materialAssetId == HE::UUID{} || !ctx.contentManager)
                    ? nullptr : ctx.contentManager->getMaterial(tmat->materialAssetId);
            const bool builtIn = lmat && lmat->path.rfind("mem://", 0) == 0;
            const std::string label = !lmat ? std::string("(none — drop a material here)")
                                            : (builtIn ? lmat->name + " (engine default)" : lmat->name);
            ImGui::SeparatorText("Material");
            // Full-width button + a "Reset to Engine Default" instead of a Clear,
            // so only the drop half is the shared widget.
            ImGui::Button((label + "##lsmat").c_str(), ImVec2(-1.0f, 0.0f));
            if (const EditorWidgets::AssetDrop drop =
                    EditorWidgets::acceptAssetDrop(ctx, HE::AssetType::Material, "material"))
            {
                if (ctx.undoSys) ctx.undoSys->snapshotNow();
                tmat->materialAssetId = drop.id;
                tmat->dirty = true;   // TerrainSystem pushes it to the chunks
                if (ctx.renderer) ctx.renderer->InvalidateMaterial(drop.id);
            }
            if (!builtIn && lmat)
            {
                if (ImGui::SmallButton("Reset to Engine Default##lsmat"))
                {
                    if (ctx.undoSys) ctx.undoSys->snapshotNow();
                    tmat->materialAssetId = HE::kDefaultTerrainMaterialId;
                    tmat->dirty = true;
                }
            }
            else
                ImGui::TextDisabled("Drag a material from the Content Browser.");
        }

        // ── Sculpt / Paint mode ──────────────────────────────────────────
        // Painting is only meaningful when the assigned material DECLARES
        // layers (a Landscape Layer Blend node): those names are the paintable
        // layers, in weightmap-channel order. The material is the source of
        // truth for what a layer means; the terrain only stores the weights.
        {
            const Entity terrainEnt2 = terrainView.front();
            const auto* tmat2 = reg.try_get<MaterialComponent>(terrainEnt2);
            const MaterialAsset* lmat2 = (!tmat2 || !ctx.contentManager) ? nullptr
                : ctx.contentManager->getMaterial(tmat2->materialAssetId);
            const std::vector<std::string> layers =
                lmat2 ? lmat2->graphLayerNames : std::vector<std::string>{};
            if (layers.empty()) s_landscapePaint = false;

            // Sculpt | Paint is one choice between two tools, so it is one well
            // of two cells — the same shape the Scene bar uses for View |
            // Landscape, and the reason a row of radio buttons never reads as
            // "pick one of these".
            {
                namespace T = EditorToolbar;
                T::Bar bar;
                bar.group();
                if (bar.item("##lsSculpt", T::iconBrush, "Sculpt", !s_landscapePaint, true,
                             "Raise, lower and smooth the ground"))
                {
                    s_landscapePaint = false;
                }
                if (bar.item("##lsPaint", T::iconLayers, "Paint", s_landscapePaint,
                             !layers.empty(),
                             layers.empty()
                                 ? "Painting needs a material with a Landscape Layer Blend node"
                                 : "Paint the material layers onto the ground"))
                {
                    s_landscapePaint = true;
                }
                bar.endGroup();
            }

            if (s_landscapePaint)
            {
                auto& ptc = reg.get<TerrainComponent>(terrainEnt2);
                ImGui::SeparatorText("Layer");
                s_paintLayer = std::clamp(s_paintLayer, 0, static_cast<int>(layers.size()) - 1);
                for (int i = 0; i < static_cast<int>(layers.size()); ++i)
                {
                    if (i % 2 != 0) ImGui::SameLine();
                    if (ImGui::RadioButton(layers[i].c_str(), s_paintLayer == i))
                        s_paintLayer = i;
                }
                ImGui::Spacing();
                ImGui::DragFloat("Radius##paint",   &s_brushRadius,   0.5f, 0.5f, 500.0f, "%.1f m");
                ImGui::DragFloat("Falloff##paint",  &s_falloffRadius, 0.5f, 0.0f, 500.0f, "%.1f m");
                ImGui::DragFloat("Strength##paint", &s_brushStrength, 0.1f, 0.1f,  50.0f, "%.2f");
                s_brushRadius   = std::max(0.5f, s_brushRadius);
                s_falloffRadius = std::max(0.0f, s_falloffRadius);

                // Changing the resolution would throw the paint away, so it is
                // locked once anything has been painted.
                int wres = static_cast<int>(ptc.weightRes);
                ImGui::Spacing();
                ImGui::BeginDisabled(!ptc.layerWeights.empty());
                if (ImGui::SliderInt("Weightmap##paint", &wres, 32, 2048))
                    ptc.weightRes = static_cast<uint32_t>(std::clamp(wres, 32, 2048));
                ImGui::EndDisabled();
                if (!ptc.layerWeights.empty())
                    ImGui::TextDisabled("Resolution is fixed once painted.");
                ImGui::TextDisabled("LMB drag in viewport to paint");
                ImGui::Spacing();
                if (ImGui::Button("Clear Paint") && !ptc.layerWeights.empty())
                {
                    if (ctx.undoSys) ctx.undoSys->snapshotNow();
                    ptc.layerWeights.clear();   // back to "everything is layer 0"
                    ptc.weightsDirty = true;
                }
            }
        }

        // ── Sculpt tools (hidden while painting) ─────────────────────────
        if (!s_landscapePaint)
        {
        // Six brushes, one armed. A well per row rather than radio buttons: the
        // armed tool is what the mouse will do in the viewport, and that deserves
        // the same "this is on" paint the gizmo tools get.
        {
            namespace T = EditorToolbar;
            struct Tool { const char* label; T::IconFn icon; const char* tip; };
            static const Tool kTools[] = {
                { "Raise",   T::iconArrowUp,   "Pull the ground up" },
                { "Lower",   T::iconArrowDown, "Push the ground down" },
                { "Smooth",  T::iconRefresh,   "Average out the neighbourhood" },
                { "Flatten", T::iconGrid,      "Level towards the first height touched" },
                { "Ramp",    T::iconFlip,      "Blend between two heights along the drag" },
                { "Roughen", T::iconSparkle,   "Add noise to the surface" },
            };
            const int toolIdx = static_cast<int>(s_terrainTool);
            for (int row = 0; row < 2; ++row)
            {
                T::Bar bar;
                bar.group();
                for (int i = row * 3; i < row * 3 + 3; ++i)
                {
                    char id[24];
                    std::snprintf(id, sizeof(id), "##lsTool%d", i);
                    if (bar.item(id, kTools[i].icon, kTools[i].label, toolIdx == i, true,
                                 kTools[i].tip))
                    {
                        s_terrainTool = static_cast<TerrainTool>(i);
                    }
                }
                bar.endGroup();
            }
        }

        ImGui::Spacing();
        ImGui::DragFloat("Radius##brush",   &s_brushRadius,   0.5f,  0.5f, 500.0f, "%.1f m");
        ImGui::DragFloat("Falloff##brush",  &s_falloffRadius, 0.5f,  0.0f, 500.0f, "%.1f m");
        ImGui::DragFloat("Strength##brush", &s_brushStrength, 0.1f,  0.1f,  50.0f, "%.2f");
        s_brushRadius   = std::max(0.5f, s_brushRadius);
        s_falloffRadius = std::max(0.0f, s_falloffRadius);

        ImGui::Spacing();
        // Per-tool hint — Ramp and Flatten read the point where the drag began.
        switch (s_terrainTool)
        {
        case TerrainTool::Ramp:
            ImGui::TextDisabled("Drag from one spot to another:");
            ImGui::TextDisabled("ramps between their heights");
            break;
        case TerrainTool::Flatten:
            ImGui::TextDisabled("Flattens toward the height");
            ImGui::TextDisabled("where the drag began");
            break;
        case TerrainTool::Roughen:
            ImGui::TextDisabled("Adds fixed-noise bumps under the brush");
            break;
        default:
            ImGui::TextDisabled("LMB drag in viewport to sculpt");
            break;
        }
        ImGui::TextDisabled("Ctrl+click a field to type a value");

        ImGui::Spacing();
        if (ImGui::Button("Reset Sculpting"))
        {
            Entity terrainEnt = terrainView.front();
            auto& tc = reg.get<TerrainComponent>(terrainEnt);
            if (ctx.undoSys) ctx.undoSys->snapshotNow();
            tc.sculptHeights.clear();
            tc.dirty = true;
        }
        } // end !s_landscapePaint (sculpt tools)
    }
#else
	(void)ctx;
#endif // HE_IMGUI_ENABLED
}

} // namespace TerrainTools
