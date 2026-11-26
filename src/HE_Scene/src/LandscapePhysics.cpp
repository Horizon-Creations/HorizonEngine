#include "HorizonScene/LandscapePhysics.h"

#include "HorizonScene/PhysicsWorld.h"

#include <algorithm>
#include <cmath>

namespace HE
{
namespace
{
double distToSquare(double px, double pz, double x0, double z0, double x1, double z1)
{
	const double dx = std::max({ x0 - px, 0.0, px - x1 });
	const double dz = std::max({ z0 - pz, 0.0, pz - z1 });
	return std::sqrt(dx * dx + dz * dz);
}
} // namespace

void LandscapePhysics::update(PhysicsWorld& physics, const LandscapeStore& store, const std::vector<glm::dvec3>& anchors, const glm::dvec3& origin,
                              float heightMinM, float heightScaleM)
{
	const LandscapeTileSource& src = store.source();
	const LandscapeGeometry& g = src.geometry();
	const double tileM = g.tileM, keep = static_cast<double>(m_config.radius) * m_config.keepFactor;
	const int tx = src.tilesX(), tz = src.tilesZ();

	// distance of a tile to the nearest anchor
	const auto distOf = [&](int x, int z) {
		double d = 1e300;
		for (const glm::dvec3& a : anchors) d = std::min(d, distToSquare(a.x, a.z, x * tileM, z * tileM, (x + 1) * tileM, (z + 1) * tileM));
		return d;
	};

	// 1. ground that no anchor needs any more
	for (auto it = m_fields.begin(); it != m_fields.end();)
	{
		if (distOf(it->second.x, it->second.z) > keep)
		{
			physics.removeGroundField(it->first);
			it = m_fields.erase(it);
			++m_stats.removed;
		}
		else ++it;
	}

	// 2. ground that is wanted and not there: nearest first, within the budget
	std::vector<std::pair<double, LandscapeTileKey>> want;
	for (const glm::dvec3& a : anchors)
	{
		const int x0 = std::max(0, static_cast<int>(std::floor((a.x - m_config.radius) / tileM)));
		const int x1 = std::min(tx - 1, static_cast<int>(std::floor((a.x + m_config.radius) / tileM)));
		const int z0 = std::max(0, static_cast<int>(std::floor((a.z - m_config.radius) / tileM)));
		const int z1 = std::min(tz - 1, static_cast<int>(std::floor((a.z + m_config.radius) / tileM)));
		for (int z = z0; z <= z1; ++z)
			for (int x = x0; x <= x1; ++x)
			{
				const LandscapeTileKey k{ x, z };
				if (m_fields.count(pack(k))) continue;
				const double d = distOf(x, z);
				if (d <= m_config.radius) want.emplace_back(d, k);
			}
	}
	std::sort(want.begin(), want.end(), [](const auto& a, const auto& b) {
		if (a.first != b.first) return a.first < b.first;
		return a.second.z != b.second.z ? a.second.z < b.second.z : a.second.x < b.second.x;
	});
	want.erase(std::unique(want.begin(), want.end(), [](const auto& a, const auto& b) { return a.second == b.second; }), want.end());

	size_t built = 0;
	const int level = std::max(0, m_config.level);
	std::vector<float> heights;
	for (const auto& [d, key] : want)
	{
		if (built >= m_config.maxBuildsPerUpdate) { ++m_stats.deferred; continue; }
		const LandscapeStore::TilePtr tile = store.tile(key);
		if (!tile || level >= static_cast<int>(tile->heights.size())) { ++m_stats.deferred; continue; }   // not in memory yet
		const uint32_t n = static_cast<uint32_t>(g.heightDim(level));
		const std::vector<uint16_t>& h16 = tile->heights[static_cast<size_t>(level)];
		heights.resize(h16.size());
		const float k = heightScaleM / 65535.0f;
		for (size_t i = 0; i < h16.size(); ++i) heights[i] = static_cast<float>(h16[i]) * k;
		const float step = g.tileM / static_cast<float>(n - 1);
		const glm::vec3 corner(static_cast<float>(key.x * tileM - origin.x), static_cast<float>(heightMinM - origin.y),
		                       static_cast<float>(key.z * tileM - origin.z));
		if (physics.addGroundField(pack(key), heights.data(), n, corner, step, step, m_config.reportAs))
		{
			m_fields.emplace(pack(key), key);
			++m_stats.built;
			++built;
		}
		else ++m_stats.failed;
	}
	m_stats.fields = m_fields.size();
}

void LandscapePhysics::clear(PhysicsWorld& physics)
{
	for (const auto& [packed, key] : m_fields) physics.removeGroundField(packed);
	m_stats.removed += m_fields.size();
	m_fields.clear();
	m_stats.fields = 0;
}

bool LandscapePhysics::holdsAt(const glm::vec3& worldPosition, const glm::dvec3& origin, const LandscapeStore& store) const
{
	const LandscapeTileSource& src = store.source();
	const double tileM = src.geometry().tileM;
	const double ax = static_cast<double>(worldPosition.x) + origin.x, az = static_cast<double>(worldPosition.z) + origin.z;
	if (ax < 0.0 || az < 0.0 || ax >= src.tilesX() * tileM || az >= src.tilesZ() * tileM) return false;
	const LandscapeTileKey key{ static_cast<int>(std::floor(ax / tileM)), static_cast<int>(std::floor(az / tileM)) };
	return m_fields.count(pack(key)) == 0;
}

} // namespace HE
