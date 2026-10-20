// Thema 190, Schritt 1: GI shadow rays at a landscape chunk seam, on the CPU.
//
// Two neighbouring chunks of one rolling landscape, built by the engine's own
// generateTerrainChunkMesh / ...Tessellated (skirts included), each put into
// the engine's own GiBvh (the BLAS the SW GI kernels traverse; the HW paths
// get the same triangles). From points on the RENDERED surface next to the
// seam it shoots the ray gi_shadow.comp shoots: origin = P + N * 0.05,
// tMin 0.02, toward the sun. A ray that is blocked with the skirts in the
// BVH but free without them is a shadow that only the skirts cast.
//
// Build (from the repo root, no engine build needed; nlohmann is only pulled in
// by Assets.h, any configured build tree's _deps has it):
//   clang++ -std=c++20 -O2 -Isrc/HE_Scene/include -Isrc/HE_Core/include \
//     -Isrc/HE_Rendering/include -Isrc/HE_Rendering/glm \
//     -I<build>/_deps/nlohmann_json-src/include \
//     scripts/gi-seam-repro/seam_probe.cpp src/HE_Scene/src/TerrainMeshGenerator.cpp \
//     src/HE_Rendering/src/GiBvh.cpp -Wl,-dead_strip -o /tmp/seam_probe && /tmp/seam_probe
//
// Result on 13.10.2026 (docs/gi-landscape-tile-seams-ursache-2026-10-13.md):
// same LOD 0 skirt-only hits and no step; LOD0|LOD1 and LOD0|LOD2 add
// +1.5 % / +7 % surface-blocked rays next to the seam, step pixels 15 / 35 %.
// Schritt 2 (stitchTerrainChunkEdges on the finer side): LOD0|LOD1, |LOD2,
// |LOD3 and tess x4|LOD0 all 0 step rays and 0 skirt-only hits; surface-
// blocked 29 362 / 25 568 / 30 362 / 32 053 against 31 639 for same LOD (the
// coarser side has less relief of its own to shadow with).
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonRendering/GiBvh.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
constexpr uint32_t kRes  = 129;      // 128 cells
constexpr float    kSize = 128.0f;   // 1 m cells, 2x2 chunks of 64 m (64 cells at LOD0)

float terrainH(float x, float z)       // rolling ground, a few metres of relief
{
	return 3.0f * std::sin(x * 0.21f + 0.7f) * std::cos(z * 0.17f)
	     + 1.5f * std::sin(z * 0.43f + x * 0.11f)
	     + 0.6f * std::sin(x * 0.9f + z * 0.7f);
}

struct Chunk
{
	HE::GiBvh full, surf;     // with skirts / surface triangles only
	glm::vec3 centre{0.0f};   // chunk entity position (the instance transform)
	uint32_t  N = 0;
};

Chunk makeChunk(const StaticMeshAsset& m, float cx, float cz, uint32_t N)
{
	Chunk c;
	c.centre = glm::vec3(cx, 0.0f, cz);
	c.N      = N;
	const size_t nv = m.vertices.size() / 3;
	c.full = HE::buildGiBvh(m.vertices.data(), nv, 3, m.indices.data(), m.indices.size());
	const size_t surfIdx = static_cast<size_t>(N - 1) * (N - 1) * 6;   // grid tris come first
	c.surf = HE::buildGiBvh(m.vertices.data(), nv, 3, m.indices.data(), surfIdx);
	return c;
}

bool anyHit(const HE::GiBvh& b, const glm::vec3& centre, glm::vec3 o, glm::vec3 d, float tMin, float tMax)
{
	float t; uint32_t tri;
	return HE::giBvhIntersect(b, o - centre, d, tMin, tMax, true, t, tri);
}

// Height of the RENDERED surface of a chunk at world (x,z): a vertical ray onto
// its surface triangles (that is what the G-buffer stores as position).
bool surfaceAt(const Chunk& c, float x, float z, float& y)
{
	float t; uint32_t tri;
	const glm::vec3 o(x - c.centre.x, 1000.0f, z - c.centre.z);
	if (!HE::giBvhIntersect(c.surf, o, glm::vec3(0, -1, 0), 0.0f, 1e6f, false, t, tri)) return false;
	y = 1000.0f - t;
	return true;
}

glm::vec3 fieldNormal(float x, float z)
{
	const float e = 1.0f;   // source-cell spacing, like sampleFieldNormal
	const float gx = (terrainH(x + e, z) - terrainH(x - e, z)) / (2 * e);
	const float gz = (terrainH(x, z + e) - terrainH(x, z - e)) / (2 * e);
	return glm::normalize(glm::vec3(-gx, 1.0f, -gz));
}

struct Count { int rays = 0, legit = 0, skirtOnly = 0; };

// Points on chunk `own` at distance d from the seam x = 0 (side = -1: x < 0).
Count probeSide(const Chunk& own, const Chunk& other, float side, const std::vector<glm::vec3>& suns)
{
	Count c;
	for (float d : { 0.02f, 0.05f, 0.1f, 0.2f, 0.35f })
		for (float z = -62.0f; z <= -2.0f; z += 0.05f)
		{
			const float x = side * d;
			float y;
			if (!surfaceAt(own, x, z, y)) continue;
			const glm::vec3 P(x, y, z), N = fieldNormal(x, z);
			for (const glm::vec3& L : suns)
			{
				if (glm::dot(N, L) <= 0.0f) continue;    // the kernel skips these too
				const glm::vec3 o = P + N * 0.05f;
				const bool full = anyHit(own.full, own.centre, o, L, 0.02f, 1e4f)
				               || anyHit(other.full, other.centre, o, L, 0.02f, 1e4f);
				const bool surf = anyHit(own.surf, own.centre, o, L, 0.02f, 1e4f)
				               || anyHit(other.surf, other.centre, o, L, 0.02f, 1e4f);
				++c.rays;
				if (surf) ++c.legit;
				else if (full) ++c.skirtOnly;
			}
		}
	return c;
}

// The pixels a camera sees IN the seam when the two edges disagree in height:
// the step between them is filled by the higher chunk's skirt, whose vertices
// copy the surface normal (up). Its ray starts inside the higher chunk.
Count probeStep(const Chunk& a, const Chunk& b, const std::vector<glm::vec3>& suns)
{
	Count c;
	for (float z = -62.0f; z <= -2.0f; z += 0.05f)
	{
		float ya, yb;
		if (!surfaceAt(a, -1e-4f, z, ya) || !surfaceAt(b, 1e-4f, z, yb)) continue;
		if (std::fabs(ya - yb) < 0.01f) continue;            // no visible step here
		const float yTop = std::max(ya, yb), yLow = std::min(ya, yb);
		const glm::vec3 N = fieldNormal(0.0f, z);
		for (float f : { 0.25f, 0.5f, 0.75f })
		{
			const glm::vec3 P(0.0f, yLow + (yTop - yLow) * f, z);   // on the skirt face
			for (const glm::vec3& L : suns)
			{
				if (glm::dot(N, L) <= 0.0f) continue;
				const glm::vec3 o = P + N * 0.05f;
				const bool hit = anyHit(a.full, a.centre, o, L, 0.02f, 1e4f)
				              || anyHit(b.full, b.centre, o, L, 0.02f, 1e4f);
				++c.rays;
				if (hit) ++c.skirtOnly;
			}
		}
	}
	return c;
}

std::vector<float> field()
{
	std::vector<float> h(static_cast<size_t>(kRes) * kRes);
	for (uint32_t j = 0; j < kRes; ++j)
		for (uint32_t i = 0; i < kRes; ++i)
		{
			const float x = -kSize * 0.5f + kSize * static_cast<float>(i) / (kRes - 1);
			const float z = -kSize * 0.5f + kSize * static_cast<float>(j) / (kRes - 1);
			h[static_cast<size_t>(j) * kRes + i] = terrainH(x, z);
		}
	return h;
}

// Chunk (cx, 0) of the 2x2 grid at `verts` per side, or the tessellated level.
// stitchCells > 0 (Schritt 2): its right edge (the seam) stitched onto a
// neighbour of that many cells, as TerrainSystem::stitchSeams does.
Chunk build(const std::vector<float>& h, int cx, uint32_t verts, bool tess, uint32_t stitchCells = 0)
{
	const float u0 = 0.5f * cx, u1 = u0 + 0.5f;
	StaticMeshAsset m = tess
		? generateTerrainChunkMeshTessellated(h, kRes, kSize, kSize, u0, 0.0f, u1, 0.5f,
		                                      verts, 1.0f, TerrainDisplacementMap{})
		: generateTerrainChunkMesh(h, kRes, kSize, kSize, u0, 0.0f, u1, 0.5f, verts, 1.0f);
	if (stitchCells)
	{
		const StaticMeshAsset lod0 = generateTerrainChunkMesh(h, kRes, kSize, kSize, u0, 0.0f, u1, 0.5f, 65, 1.0f);
		TerrainChunkStitch st;
		st.edgeCells[1] = stitchCells;
		m = stitchTerrainChunkEdges(m, verts - 1, lod0, 64, st);
	}
	return makeChunk(m, -32.0f + 64.0f * cx, -32.0f, verts);
}
} // namespace

int main()
{
	const std::vector<float> h = field();
	std::vector<glm::vec3> suns;
	for (float elev : { 20.0f, 35.0f, 55.0f })
		for (float az = 0.0f; az < 360.0f; az += 30.0f)
		{
			const float e = glm::radians(elev), a = glm::radians(az);
			suns.push_back(glm::vec3(std::cos(e) * std::cos(a), std::sin(e), std::cos(e) * std::sin(a)));
		}

	struct Case { const char* name; uint32_t vA; bool tA; uint32_t vB; bool tB; uint32_t stitch = 0; };
	const Case cases[] = {
		{ "same LOD (LOD0 | LOD0)       ", 65, false, 65, false },
		{ "LOD0 | LOD1                  ", 65, false, 33, false },
		{ "LOD0 | LOD2                  ", 65, false, 17, false },
		{ "tessellated x2 | LOD0        ", 129, true, 65, false },
		{ "tessellated x4 | LOD0        ", 257, true, 65, false },
		{ "LOD0 | LOD1, stitched        ", 65, false, 33, false, 32 },
		{ "LOD0 | LOD2, stitched        ", 65, false, 17, false, 16 },
		{ "LOD0 | LOD3, stitched        ", 65, false,  9, false,  8 },
		{ "tessellated x4 | LOD0, stitch", 257, true, 65, false, 64 },
	};
	std::printf("rays from the rendered surface within 0.35 m of the seam, %zu sun directions\n", suns.size());
	std::printf("%-30s %10s %10s %12s %10s | step pixels: %8s %10s\n",
	            "case", "rays", "terrain", "SKIRT ONLY", "share", "rays", "shadowed");
	for (const Case& c : cases)
	{
		const Chunk a = build(h, 0, c.vA, c.tA, c.stitch);
		const Chunk b = build(h, 1, c.vB, c.tB);
		const Count ca = probeSide(a, b, -1.0f, suns);
		const Count cb = probeSide(b, a, +1.0f, suns);
		const Count st = probeStep(a, b, suns);
		const int rays = ca.rays + cb.rays, legit = ca.legit + cb.legit, sk = ca.skirtOnly + cb.skirtOnly;
		std::printf("%-30s %10d %10d %12d %9.2f%% | %20d %9.1f%%\n", c.name, rays, legit, sk,
		            100.0 * sk / std::max(rays, 1), st.rays, 100.0 * st.skirtOnly / std::max(st.rays, 1));
	}
	return 0;
}
