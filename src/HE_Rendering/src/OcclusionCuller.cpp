#include "HorizonRendering/OcclusionCuller.h"
#include "HorizonRendering/RenderSorter.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Diagnostics/Profiler.h>
#include <JobSystem/JobSystem.h>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace
{
	// Clip-space w below which a vertex counts as "at or behind the eye". Depth
	// is 1/w, so w must stay clear of zero; anything this close is clipped.
	constexpr float kNearW = 1e-4f;
	// Relative slack the buffer has to beat an occludee's nearest corner by.
	// Guards the occluder against culling ITSELF out of float rounding: its own
	// surface is never farther than its own bounding box, but the two numbers
	// come out of different arithmetic.
	constexpr float kDepthEps = 1e-3f;
	constexpr int   kTile     = 8;

	struct Candidate
	{
		uint32_t index;
		float    area;      // projected screen fraction (bigger = better occluder)
		uint32_t triangles;
	};

	// Index range of one rasterized piece of a mesh.
	struct Range { uint32_t offset, count; };

	// The verdict for ONE material as the draw loops resolve it: a null id is
	// the built-in opaque program (the mesh's own material only lends its
	// texture, whose alpha the built-in shader ignores). An unresolved id could
	// be anything, so it is not an occluder.
	bool materialOpaque(const ContentManager& cm, const HE::UUID& id)
	{
		if (id == HE::UUID{}) return true;
		const MaterialAsset* ma = cm.getMaterial(id);
		if (!ma) return false;
		if (ma->blendMode != 0) return false;                 // Masked discards, Translucent blends
		if (ma->opacity < RenderSorter::kOpaqueOpacityThreshold) return false;
		if (!ma->customShaderVertGlsl.empty()) return false;  // WPO moves the surface on the GPU
		return true;
	}

	// The opaque index ranges of an object's mesh, honouring the section split
	// the extractor produced (a MaterialComponent override replaces every slot,
	// so the object then has no sections and draws whole with the override).
	void opaqueRanges(const RenderObject& obj, const StaticMeshAsset& mesh,
	                  const ContentManager& cm, std::vector<Range>& out)
	{
		out.clear();
		if (obj.sections.empty())
		{
			if (materialOpaque(cm, obj.materialAssetId))
				out.push_back({ 0u, static_cast<uint32_t>(mesh.indices.size()) });
			return;
		}
		for (const RenderSection& sec : obj.sections)
			if (materialOpaque(cm, sec.materialAssetId))
				out.push_back({ sec.indexOffset, sec.indexCount });
	}

	// Position pointer + stride for the two layouts a StaticMeshAsset can carry
	// (the same two BuildGIBlas reads). Null when the mesh has no usable data.
	const float* positions(const StaticMeshAsset& mesh, size_t& stride, size_t& count)
	{
		if (mesh.cooked && !mesh.interleaved.empty())
		{
			stride = 8; count = mesh.vertexCount;
			return mesh.interleaved.size() >= count * 8 ? mesh.interleaved.data() : nullptr;
		}
		if (!mesh.vertices.empty())
		{
			stride = 3; count = mesh.vertices.size() / 3;
			return mesh.vertices.data();
		}
		return nullptr;
	}

	// Screen-space rect of a set of clip-space points, in pixels. Returns false
	// when any point is at/behind the near plane (then no rect is trustworthy).
	struct ScreenRect { float minX, minY, maxX, maxY; };
	bool projectRect(const glm::vec4* clip, int n, int W, int H, ScreenRect& r, float& invWmax)
	{
		r = { 1e30f, 1e30f, -1e30f, -1e30f };
		invWmax = 0.0f;
		for (int i = 0; i < n; ++i)
		{
			const glm::vec4& c = clip[i];
			if (!(c.w > kNearW)) return false;
			const float iw = 1.0f / c.w;
			const float sx = (c.x * iw * 0.5f + 0.5f) * static_cast<float>(W);
			const float sy = (0.5f - c.y * iw * 0.5f) * static_cast<float>(H); // row 0 = top
			r.minX = std::min(r.minX, sx); r.maxX = std::max(r.maxX, sx);
			r.minY = std::min(r.minY, sy); r.maxY = std::max(r.maxY, sy);
			invWmax = std::max(invWmax, iw);
		}
		return true;
	}

	void boxCorners(const HE::AABB& b, const glm::mat4& m, glm::vec4 out[8])
	{
		int k = 0;
		for (int i = 0; i < 8; ++i)
		{
			const glm::vec3 p((i & 1) ? b.max.x : b.min.x,
			                  (i & 2) ? b.max.y : b.min.y,
			                  (i & 4) ? b.max.z : b.min.z);
			out[k++] = m * glm::vec4(p, 1.0f);
		}
	}
} // namespace

uint32_t OcclusionCuller::refine(const RenderWorld& world, const ContentManager* cm,
                                 std::vector<uint8_t>& visible)
{
	m_stats = Stats{};
	if (!m_settings.enabled || !cm) return 0;
	const size_t count = world.objects.size();
	if (count == 0 || visible.size() != count) return 0;
	HE_PROFILE_SCOPE_N("OcclusionCuller::refine");

	const glm::mat4& proj = world.camera.projection;
	// Orthographic (w ≡ 1) has no 1/w depth to interpolate — the buffer would
	// be flat. Not a case the runtime produces today; skip rather than guess.
	if (std::abs(proj[2][3]) < 1e-6f) return 0;
	const glm::mat4 viewProj = proj * world.camera.view;

	// ── Buffer size from the camera aspect ─────────────────────────────────
	const float aspect = (std::abs(proj[0][0]) > 1e-9f) ? std::abs(proj[1][1] / proj[0][0]) : 1.0f;
	const int W = std::clamp(m_settings.bufferWidth, 16, 2048);
	const int H = std::clamp(static_cast<int>(std::lround(static_cast<float>(W) / std::max(aspect, 1e-3f))), 16, 2048);
	m_width = W; m_height = H;
	m_depth.assign(static_cast<size_t>(W) * H, 0.0f);
	m_tilesX = (W + kTile - 1) / kTile;
	m_tilesY = (H + kTile - 1) / kTile;
	const float screenArea = static_cast<float>(W) * static_cast<float>(H);

	// ── Occluder selection ─────────────────────────────────────────────────
	// Serial: material/mesh lookups go through the ContentManager, whose
	// read-side thread safety is not something this code wants to depend on.
	std::vector<Candidate> candidates;
	std::vector<Range>     ranges;
	const int maxTris = std::max(1, m_settings.maxOccluderTriangles);
	for (size_t i = 0; i < count; ++i)
	{
		if (!visible[i]) continue;
		const RenderObject& obj = world.objects[i];
		if (!obj.worldBounds.isValid() || !obj.contributesAO) continue;
		if (obj.instanceTint.a < RenderSorter::kOpaqueOpacityThreshold) continue;

		// Screen-area gate FIRST — pure math on the bounds — so the thousands
		// of small things a scene has (foliage, props, far chunks) never reach
		// the ContentManager lookups below.
		glm::vec4 corners[8];
		boxCorners(obj.worldBounds, viewProj, corners);
		ScreenRect rect; float invWmax;
		float area;
		if (projectRect(corners, 8, W, H, rect, invWmax))
		{
			const float w = std::max(0.0f, std::min(rect.maxX, static_cast<float>(W)) - std::max(rect.minX, 0.0f));
			const float h = std::max(0.0f, std::min(rect.maxY, static_cast<float>(H)) - std::max(rect.minY, 0.0f));
			area = (w * h) / screenArea;
		}
		else
			area = 1.0f; // straddles the near plane: right in front of the camera, as big as it gets
		if (area < m_settings.minOccluderScreenArea) continue;

		const StaticMeshAsset* mesh = cm->getStaticMesh(obj.meshAssetId);
		if (!mesh || mesh->indices.empty()) continue;
		size_t stride = 0, vcount = 0;
		if (!positions(*mesh, stride, vcount)) continue;
		opaqueRanges(obj, *mesh, *cm, ranges);
		uint32_t tris = 0;
		for (const Range& r : ranges) tris += r.count / 3;
		if (tris == 0 || tris > static_cast<uint32_t>(maxTris)) continue;
		candidates.push_back({ static_cast<uint32_t>(i), area, tris });
	}
	if (candidates.empty()) return 0;
	std::sort(candidates.begin(), candidates.end(),
	          [](const Candidate& a, const Candidate& b) { return a.area > b.area; });
	if (candidates.size() > static_cast<size_t>(std::max(1, m_settings.maxOccluders)))
		candidates.resize(static_cast<size_t>(std::max(1, m_settings.maxOccluders)));

	// ── Rasterize the occluders ────────────────────────────────────────────
	// Depth is 1/w, larger = nearer; the buffer keeps the nearest surface per
	// pixel, and every triangle writes its FARTHEST 1/w over the pixel footprint
	// (centre value minus half the |gradient| in x and y — 1/w is affine on
	// screen, so that is the exact minimum over the pixel square).
	const float fW = static_cast<float>(W), fH = static_cast<float>(H);
	auto rasterTriangle = [&](glm::vec4 c0, glm::vec4 c1, glm::vec4 c2)
	{
		// To screen. (x/w, y/w) in [-1,1] → pixels, y flipped so row 0 is the top.
		const float iw0 = 1.0f / c0.w, iw1 = 1.0f / c1.w, iw2 = 1.0f / c2.w;
		const float x0 = (c0.x * iw0 * 0.5f + 0.5f) * fW, y0 = (0.5f - c0.y * iw0 * 0.5f) * fH;
		const float x1 = (c1.x * iw1 * 0.5f + 0.5f) * fW, y1 = (0.5f - c1.y * iw1 * 0.5f) * fH;
		const float x2 = (c2.x * iw2 * 0.5f + 0.5f) * fW, y2 = (0.5f - c2.y * iw2 * 0.5f) * fH;
		// Edge function, evaluated in a CANONICAL vertex order (the lexicographically
		// smaller endpoint first, result negated when the caller's order was the
		// other way round). Two triangles sharing an edge then compute bit-identical
		// values for it, so a pixel centre on the seam passes both inclusive tests
		// and never falls through the crack: with the raw per-triangle formula the
		// two evaluations round differently, and a one-pixel hole along every
		// mesh diagonal lets whatever sits behind the occluder show through.
		auto edge = [](float ax, float ay, float bx, float by, float px, float py) {
			const bool swap = (ax > bx) || (ax == bx && ay > by);
			if (swap) { std::swap(ax, bx); std::swap(ay, by); }
			const float v = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
			return swap ? -v : v;
		};
		// Signed double area — from the same function, so the orientation sign
		// agrees with the edge tests; either winding rasterizes (back faces
		// occlude too).
		const float det = edge(x0, y0, x1, y1, x2, y2);
		if (std::abs(det) < 1e-12f) return;
		const float invDet = 1.0f / det;
		// Affine plane of 1/w over the screen: d = A*x + B*y + C.
		const float A = ((iw1 - iw0) * (y2 - y0) - (iw2 - iw0) * (y1 - y0)) * invDet;
		const float B = ((iw2 - iw0) * (x1 - x0) - (iw1 - iw0) * (x2 - x0)) * invDet;
		const float C = iw0 - A * x0 - B * y0;
		const float slack = 0.5f * (std::abs(A) + std::abs(B));

		int minX = static_cast<int>(std::floor(std::min({ x0, x1, x2 })));
		int maxX = static_cast<int>(std::ceil (std::max({ x0, x1, x2 })));
		int minY = static_cast<int>(std::floor(std::min({ y0, y1, y2 })));
		int maxY = static_cast<int>(std::ceil (std::max({ y0, y1, y2 })));
		minX = std::max(minX, 0); minY = std::max(minY, 0);
		maxX = std::min(maxX, W - 1); maxY = std::min(maxY, H - 1);
		if (minX > maxX || minY > maxY) return;

		// "Inside" is ≥ 0 on all three edges once the orientation sign is applied.
		const float s = det > 0.0f ? 1.0f : -1.0f;
		for (int py = minY; py <= maxY; ++py)
		{
			const float cy = static_cast<float>(py) + 0.5f;
			float* row = &m_depth[static_cast<size_t>(py) * W];
			for (int px = minX; px <= maxX; ++px)
			{
				const float cx = static_cast<float>(px) + 0.5f;
				if (s * edge(x0, y0, x1, y1, cx, cy) < 0.0f) continue;
				if (s * edge(x1, y1, x2, y2, cx, cy) < 0.0f) continue;
				if (s * edge(x2, y2, x0, y0, cx, cy) < 0.0f) continue;
				const float d = A * cx + B * cy + C - slack;
				if (d <= 0.0f) continue; // farthest point of the footprint is at/behind infinity
				if (d > row[px]) row[px] = d;
			}
		}
	};

	// Clip a triangle against w = kNearW (Sutherland–Hodgman on one plane) and
	// hand the pieces to the rasterizer. A vertex behind the eye would flip
	// under the perspective divide.
	auto emitTriangle = [&](const glm::vec4& c0, const glm::vec4& c1, const glm::vec4& c2)
	{
		const bool in0 = c0.w > kNearW, in1 = c1.w > kNearW, in2 = c2.w > kNearW;
		if (in0 && in1 && in2) { rasterTriangle(c0, c1, c2); return; }
		if (!in0 && !in1 && !in2) return;
		const glm::vec4 src[3] = { c0, c1, c2 };
		glm::vec4 poly[4]; int n = 0;
		for (int i = 0; i < 3; ++i)
		{
			const glm::vec4& a = src[i];
			const glm::vec4& b = src[(i + 1) % 3];
			const bool ia = a.w > kNearW, ib = b.w > kNearW;
			if (ia) poly[n++] = a;
			if (ia != ib)
			{
				const float t = (kNearW - a.w) / (b.w - a.w);
				poly[n++] = a + (b - a) * t;
			}
		}
		for (int i = 1; i + 1 < n; ++i) rasterTriangle(poly[0], poly[i], poly[i + 1]);
	};

	for (const Candidate& cand : candidates)
	{
		const RenderObject&    obj  = world.objects[cand.index];
		const StaticMeshAsset* mesh = cm->getStaticMesh(obj.meshAssetId);
		if (!mesh) continue; // cannot happen between the two loops (no loads), but stay safe
		size_t stride = 0, vcount = 0;
		const float* pos = positions(*mesh, stride, vcount);
		if (!pos) continue;
		opaqueRanges(obj, *mesh, *cm, ranges);

		const glm::mat4 mvp = viewProj * obj.transform;
		m_clipScratch.resize(vcount);
		for (size_t v = 0; v < vcount; ++v)
			m_clipScratch[v] = mvp * glm::vec4(pos[v * stride], pos[v * stride + 1], pos[v * stride + 2], 1.0f);

		const size_t indexCount = mesh->indices.size();
		for (const Range& r : ranges)
		{
			const size_t end = std::min<size_t>(indexCount, static_cast<size_t>(r.offset) + r.count);
			for (size_t k = r.offset; k + 2 < end; k += 3)
			{
				const uint32_t i0 = mesh->indices[k], i1 = mesh->indices[k + 1], i2 = mesh->indices[k + 2];
				if (i0 >= vcount || i1 >= vcount || i2 >= vcount) continue;
				emitTriangle(m_clipScratch[i0], m_clipScratch[i1], m_clipScratch[i2]);
			}
		}
		++m_stats.occluders;
		m_stats.occluderTriangles += cand.triangles;
	}

	// ── Tile summaries for the occludee test ───────────────────────────────
	m_tileMin.assign(static_cast<size_t>(m_tilesX) * m_tilesY,  1e30f);
	m_tileMax.assign(static_cast<size_t>(m_tilesX) * m_tilesY, -1e30f);
	for (int y = 0; y < H; ++y)
	{
		const float* row = &m_depth[static_cast<size_t>(y) * W];
		float* tmin = &m_tileMin[static_cast<size_t>(y / kTile) * m_tilesX];
		float* tmax = &m_tileMax[static_cast<size_t>(y / kTile) * m_tilesX];
		for (int x = 0; x < W; ++x)
		{
			const int t = x / kTile;
			tmin[t] = std::min(tmin[t], row[x]);
			tmax[t] = std::max(tmax[t], row[x]);
		}
	}

	// ── Occludee test ──────────────────────────────────────────────────────
	// Hidden only when the object's NEAREST corner is farther than the buffer
	// at EVERY pixel of its (dilated) screen rect. Reads only; parallel.
	std::atomic<uint32_t> culled{ 0 }, tested{ 0 };
	parallel_for(count, [&](size_t i) {
		if (!visible[i]) return;
		const RenderObject& obj = world.objects[i];
		if (!obj.worldBounds.isValid()) return; // unknown bounds: always keep (as the frustum cull does)
		tested.fetch_add(1, std::memory_order_relaxed);

		glm::vec4 corners[8];
		boxCorners(obj.worldBounds, viewProj, corners);
		ScreenRect rect; float invWmax;
		if (!projectRect(corners, 8, W, H, rect, invWmax)) return; // touches the near plane: keep
		// Dilate by one pixel: the rasterizer samples pixel centres, so the
		// buffer's coverage can be up to half a pixel short at an occluder's edge.
		int minX = static_cast<int>(std::floor(rect.minX)) - 1;
		int maxX = static_cast<int>(std::ceil (rect.maxX)) + 1;
		int minY = static_cast<int>(std::floor(rect.minY)) - 1;
		int maxY = static_cast<int>(std::ceil (rect.maxY)) + 1;
		// Off-screen (by the box) yet frustum-visible: the frustum test is the
		// conservative one — keep.
		if (maxX < 0 || maxY < 0 || minX >= W || minY >= H) return;
		minX = std::max(minX, 0); minY = std::max(minY, 0);
		maxX = std::min(maxX, W - 1); maxY = std::min(maxY, H - 1);

		const float threshold = invWmax * (1.0f + kDepthEps);
		const int tx0 = minX / kTile, tx1 = maxX / kTile, ty0 = minY / kTile, ty1 = maxY / kTile;
		for (int ty = ty0; ty <= ty1; ++ty)
			for (int tx = tx0; tx <= tx1; ++tx)
			{
				const size_t t = static_cast<size_t>(ty) * m_tilesX + tx;
				if (m_tileMax[t] <= threshold) return;     // some pixel here cannot occlude → visible
				if (m_tileMin[t] >  threshold) continue;   // every pixel here occludes → next tile
				const int px0 = std::max(minX, tx * kTile), px1 = std::min(maxX, tx * kTile + kTile - 1);
				const int py0 = std::max(minY, ty * kTile), py1 = std::min(maxY, ty * kTile + kTile - 1);
				for (int py = py0; py <= py1; ++py)
				{
					const float* row = &m_depth[static_cast<size_t>(py) * W];
					for (int px = px0; px <= px1; ++px)
						if (row[px] <= threshold) return;  // visible through this pixel
				}
			}
		visible[i] = 0u;
		culled.fetch_add(1, std::memory_order_relaxed);
	}, "OcclusionCull");

	m_stats.tested = tested.load();
	m_stats.culled = culled.load();
	return m_stats.culled;
}
