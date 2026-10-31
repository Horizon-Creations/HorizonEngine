#include "HorizonScene/LandscapeQuadtree.h"

#include <algorithm>
#include <cmath>

namespace HE
{
namespace
{
struct Ctx
{
	const LandscapeQuadtreeConfig& cfg;
	glm::dvec3                     camera;
	const LandscapeFrustum*        frustum;
	const LandscapeTileResident*   resident;
	double                         worldX, worldZ;
	std::vector<LandscapePatch>&   out;
};

// 3D distance from the camera to the box [x0, x0 + s] x [minY, maxY] x [z0, z0 + s].
double boxDistance(const Ctx& c, double x0, double z0, double s)
{
	const double dx = std::max({ x0 - c.camera.x, 0.0, c.camera.x - (x0 + s) });
	const double dy = std::max({ static_cast<double>(c.cfg.minY) - c.camera.y, 0.0, c.camera.y - static_cast<double>(c.cfg.maxY) });
	const double dz = std::max({ z0 - c.camera.z, 0.0, c.camera.z - (z0 + s) });
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool boxInFrustum(const LandscapeFrustum& f, double x0, double z0, double s, double minY, double maxY)
{
	for (const glm::dvec4& p : f.planes)
	{
		// the corner furthest along the plane normal
		const double x = p.x >= 0.0 ? x0 + s : x0;
		const double y = p.y >= 0.0 ? maxY : minY;
		const double z = p.z >= 0.0 ? z0 + s : z0;
		if (p.x * x + p.y * y + p.z * z + p.w < 0.0) return false;
	}
	return true;
}

// Level <= tileLevels needs the store's tiles under the node; the coarse pyramid above is always there.
bool nodeAvailable(const Ctx& c, int level, int ix, int iz)
{
	if (level > c.cfg.tileLevels || !c.resident || !*c.resident) return true;
	const double s = c.cfg.patchSize(level), tileM = c.cfg.tileM;
	const int tx0 = static_cast<int>(std::floor(ix * s / tileM));
	const int tx1 = std::min(c.cfg.tilesX - 1, static_cast<int>(std::ceil((ix + 1) * s / tileM)) - 1);
	const int tz0 = static_cast<int>(std::floor(iz * s / tileM));
	const int tz1 = std::min(c.cfg.tilesZ - 1, static_cast<int>(std::ceil((iz + 1) * s / tileM)) - 1);
	for (int tz = tz0; tz <= tz1; ++tz)
		for (int tx = tx0; tx <= tx1; ++tx)
			if (!(*c.resident)(tx, tz)) return false;
	return true;
}

// Quadrants of the node whose corner lies inside the world.
uint8_t quadrantsInWorld(const Ctx& c, double x0, double z0, double s)
{
	uint8_t m = 0;
	for (int q = 0; q < 4; ++q)
	{
		const double qx = x0 + (q & 1) * s * 0.5, qz = z0 + (q >> 1) * s * 0.5;
		if (qx < c.worldX && qz < c.worldZ) m |= static_cast<uint8_t>(1 << q);
	}
	return m;
}

// True when the node is handled (drawn, culled, or nothing of it is in the world); false when it is out of its
// own range and the parent has to draw its area.
bool select(Ctx& c, int level, int ix, int iz)
{
	const double s = c.cfg.patchSize(level);
	const double x0 = ix * s, z0 = iz * s;
	if (x0 >= c.worldX || z0 >= c.worldZ) return true;

	const double d = boxDistance(c, x0, z0, s);
	if (d > c.cfg.rangeOf(level)) return false;
	if (c.frustum && c.frustum->valid && !boxInFrustum(*c.frustum, x0, z0, s, c.cfg.minY, c.cfg.maxY)) return true;

	const uint8_t inWorld = quadrantsInWorld(c, x0, z0, s);
	if (level == 0 || d > c.cfg.rangeOf(level - 1))
	{
		c.out.push_back({ level, ix, iz, inWorld, x0, z0, static_cast<float>(s) });
		return true;
	}
	uint8_t mask = 0;
	for (int q = 0; q < 4; ++q)
	{
		if (!(inWorld & (1 << q))) continue;
		const int cx = 2 * ix + (q & 1), cz = 2 * iz + (q >> 1);
		if (!nodeAvailable(c, level - 1, cx, cz) || !select(c, level - 1, cx, cz)) mask |= static_cast<uint8_t>(1 << q);
	}
	if (mask) c.out.push_back({ level, ix, iz, mask, x0, z0, static_cast<float>(s) });
	return true;
}
} // namespace

int LandscapeQuadtreeConfig::topLevel() const
{
	const double world = std::max(tilesX, tilesZ) * static_cast<double>(tileM);
	int l = 0;
	while (l < 24 && static_cast<double>(patchSize(l)) < world) ++l;
	return l;
}

LandscapeQuadtreeConfig LandscapeQuadtreeConfig::fromGeometry(const LandscapeGeometry& g, int tilesX_, int tilesZ_)
{
	LandscapeQuadtreeConfig c;
	c.tileM = g.tileM;
	c.cell0M = g.tileM / static_cast<float>(g.hCells);
	c.tilesX = tilesX_;
	c.tilesZ = tilesZ_;
	return c;
}

float LandscapeQuadtreeConfig::minLoadRadius() const
{
	return rangeOf(tileLevels) + 1.4143f * patchSize(tileLevels);
}

LandscapeFrustum LandscapeFrustum::fromViewProjection(const glm::dmat4& m, bool zeroToOne)
{
	const auto row = [&](int i) { return glm::dvec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
	LandscapeFrustum f;
	f.planes[0] = row(3) + row(0);                       // left
	f.planes[1] = row(3) - row(0);                       // right
	f.planes[2] = row(3) + row(1);                       // bottom
	f.planes[3] = row(3) - row(1);                       // top
	f.planes[4] = zeroToOne ? row(2) : row(3) + row(2);  // near
	f.planes[5] = row(3) - row(2);                       // far
	for (glm::dvec4& p : f.planes)
	{
		const double len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
		if (len > 0.0) p /= len;
	}
	f.valid = true;
	return f;
}

std::vector<LandscapePatch> selectLandscapePatches(const LandscapeQuadtreeConfig& cfg, const glm::dvec3& camera,
                                                   const LandscapeFrustum* frustum, const LandscapeTileResident& resident)
{
	std::vector<LandscapePatch> out;
	Ctx c{ cfg, camera, frustum, &resident, cfg.tilesX * static_cast<double>(cfg.tileM), cfg.tilesZ * static_cast<double>(cfg.tileM), out };
	const int top = cfg.topLevel();
	const double s = cfg.patchSize(top);
	const int nx = static_cast<int>(std::ceil(c.worldX / s)), nz = static_cast<int>(std::ceil(c.worldZ / s));
	for (int iz = 0; iz < nz; ++iz)
		for (int ix = 0; ix < nx; ++ix)
			select(c, top, ix, iz);
	return out;
}

float landscapeMorphDistance(const LandscapeQuadtreeConfig& cfg, const glm::dvec3& camera, double x, double z)
{
	const double dy = std::max({ static_cast<double>(cfg.minY) - camera.y, 0.0, camera.y - static_cast<double>(cfg.maxY) });
	return static_cast<float>(std::sqrt((x - camera.x) * (x - camera.x) + (z - camera.z) * (z - camera.z) + dy * dy));
}

float landscapeMorph(const LandscapeQuadtreeConfig& cfg, int level, float distance)
{
	const float lo = cfg.rangeOf(level - 1), hi = cfg.rangeOf(level);
	const float start = lo + (hi - lo) * cfg.morphStart;
	if (!(hi > start)) return 0.0f;
	return std::clamp((distance - start) / (hi - start), 0.0f, 1.0f);
}

} // namespace HE
