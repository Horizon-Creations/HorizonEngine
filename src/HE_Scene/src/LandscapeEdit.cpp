#include "HorizonScene/LandscapeEdit.h"
#include "HorizonScene/LandscapeRuntime.h"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace HE
{

namespace
{
void put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
uint32_t get32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24; }
constexpr uint32_t kEditsMagic = 0x31454c48u;   // 'HLE1'
constexpr uint32_t kEditsVersion = 1;
constexpr uint32_t kEditsWaterTag = 0x54574c48u;   // 'HLWT': the optional water table after the tile records

uint64_t fnv(const std::vector<uint8_t>& bytes)
{
	uint64_t h = 1469598103934665603ull;
	for (const uint8_t b : bytes) h = (h ^ b) * 1099511628211ull;
	return h ? h : 1;
}
} // namespace

// ── the overlay ──────────────────────────────────────────────────────────────

LandscapeEditSource::LandscapeEditSource(std::shared_ptr<const LandscapeTileSource> base) : m_base(std::move(base)) {}

bool LandscapeEditSource::load(LandscapeTileKey key, int maxLevels, LandscapeTile& out) const
{
	LandscapeTilePtr tile;
	std::shared_ptr<const StampSet> set;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		const auto it = m_tiles.find({ key.x, key.z });
		if (it != m_tiles.end()) tile = it->second;
		if (m_stampSet && m_stampSet->tiles.count({ key.x, key.z })) set = m_stampSet;
	}
	if (!tile && !set) return m_base->load(key, maxLevels, out);
	if (tile) out = *tile;
	else if (!m_base->load(key, -1, out)) return false;   // every level: the stamp goes onto level 0 and the mips are made again
	if (set) applyStamps(*set, key, out);
	if (maxLevels >= 0)
	{
		out.heights.resize(std::min<size_t>(out.heights.size(), static_cast<size_t>(maxLevels)));
		out.weights.resize(std::min<size_t>(out.weights.size(), static_cast<size_t>(maxLevels)));
	}
	return true;
}

void LandscapeEditSource::set(const std::vector<std::pair<LandscapeTileKey, LandscapeTilePtr>>& tiles, bool patchCoarse)
{
	if (tiles.empty()) return;
	m_dirty = true;
	if (!patchCoarse)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (const auto& [key, tile] : tiles)
		{
			if (tile) m_tiles[{ key.x, key.z }] = tile;
			else m_tiles.erase({ key.x, key.z });
		}
		return;
	}
	std::vector<std::pair<LandscapeTileKey, LandscapeTilePtr>> patch;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (const auto& [key, tile] : tiles)
		{
			if (tile) m_tiles[{ key.x, key.z }] = tile;
			else m_tiles.erase({ key.x, key.z });
		}
	}
	for (const auto& [key, tile] : tiles)
	{
		if (tile) { patch.emplace_back(key, shown(key, tile)); continue; }
		// The edit is gone: the base's tile is what the coarse world shows again.
		auto back = std::make_shared<LandscapeTile>();
		if (m_base->load(key, -1, *back)) patch.emplace_back(key, shown(key, std::move(back)));
	}
	if (!m_coarse.valid())
		if (const LandscapeCoarseWorld* c = m_base->coarse()) m_coarse = *c;
	landscapeCoarsePatch(m_coarse, patch);
	m_dirty = true;
}

std::shared_ptr<const LandscapeCoarseWorld> LandscapeEditSource::coarseWindow(LandscapeTileKey origin, int tx, int tz) const
{
	if (m_coarse.valid() || m_base->coarse()) return LandscapeTileSource::coarseWindow(origin, tx, tz);   // a crop of coarse(), patched or not
	std::shared_ptr<const LandscapeCoarseWorld> window = m_base->coarseWindow(origin, tx, tz);
	if (!window) return nullptr;
	// The edited and the stamped tiles inside the window, as they are shown, with keys relative to its corner.
	std::vector<std::pair<LandscapeTileKey, LandscapeTilePtr>> inside;
	std::vector<LandscapeTileKey> keys = editedKeys();
	for (const LandscapeTileKey& k : stampedKeys())
		if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
	for (const LandscapeTileKey& k : keys)
		if (k.x >= origin.x && k.z >= origin.z && k.x < origin.x + window->tilesX && k.z < origin.z + window->tilesZ)
		{
			// An edited tile is in memory (copied only where a stamp reaches it); a stamped-only one is read from the base.
			LandscapeTilePtr tile = shown(k, edited(k));
			if (!tile)
			{
				auto read = std::make_shared<LandscapeTile>();
				if (load(k, -1, *read)) tile = std::move(read);
			}
			if (tile) inside.emplace_back(LandscapeTileKey{ k.x - origin.x, k.z - origin.z }, std::move(tile));
		}
	if (inside.empty()) return window;
	auto patched = std::make_shared<LandscapeCoarseWorld>(*window);
	landscapeCoarsePatch(*patched, inside);
	return patched;
}

LandscapeTilePtr LandscapeEditSource::edited(LandscapeTileKey key) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const auto it = m_tiles.find({ key.x, key.z });
	return it == m_tiles.end() ? nullptr : it->second;
}

std::vector<LandscapeTileKey> LandscapeEditSource::editedKeys() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	std::vector<LandscapeTileKey> keys;
	keys.reserve(m_tiles.size());
	for (const auto& [k, t] : m_tiles) keys.push_back({ k.first, k.second });
	return keys;
}

void LandscapeEditSource::setWaterTable(LandscapeWaterTable table)
{
	if (m_water) m_waterRetired.push_back(m_water);
	m_water = std::make_shared<const LandscapeWaterTable>(std::move(table));
	m_dirty = true;
}

// ── stamps ──────────────────────────────────────────────────────────────────

std::shared_ptr<const LandscapeEditSource::StampSet> LandscapeEditSource::stampSet() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_stampSet;
}

size_t LandscapeEditSource::stampCount() const { const auto s = stampSet(); return s ? s->stamps.size() : 0; }
bool   LandscapeEditSource::stamped(LandscapeTileKey key) const { const auto s = stampSet(); return s && s->tiles.count({ key.x, key.z }) != 0; }

std::vector<LandscapeTileKey> LandscapeEditSource::stampedKeys() const
{
	std::vector<LandscapeTileKey> keys;
	if (const auto s = stampSet())
		for (const auto& [k, v] : s->tiles) keys.push_back({ k.first, k.second });
	return keys;
}

std::vector<LandscapeTileKey> LandscapeEditSource::footprint(const LandscapeStamp& s) const
{
	std::vector<LandscapeTileKey> keys;
	if (!s.image || s.w < 1 || s.h < 1 || s.image->size() < static_cast<size_t>(s.w) * static_cast<size_t>(s.h) || s.sizeX <= 0.0f || s.sizeZ <= 0.0f || s.heightM == 0.0f)
		return keys;
	const LandscapeGeometry& g = geometry();
	const double tileM = g.tileM, cell = tileM / g.hCells;
	// The turned rectangle's box, a sample wider: a sample on a tile border belongs to both tiles.
	const double c = std::abs(std::cos(s.yaw)), sn = std::abs(std::sin(s.yaw));
	const double hx = (s.sizeX * c + s.sizeZ * sn) * 0.5 + cell, hz = (s.sizeX * sn + s.sizeZ * c) * 0.5 + cell;
	const int x0 = std::max(0, static_cast<int>(std::floor((s.x - hx) / tileM))), x1 = std::min(tilesX() - 1, static_cast<int>(std::floor((s.x + hx) / tileM)));
	const int z0 = std::max(0, static_cast<int>(std::floor((s.z - hz) / tileM))), z1 = std::min(tilesZ() - 1, static_cast<int>(std::floor((s.z + hz) / tileM)));
	for (int z = z0; z <= z1; ++z)
		for (int x = x0; x <= x1; ++x) keys.push_back({ x, z });
	return keys;
}

void LandscapeEditSource::applyStamps(const StampSet& set, LandscapeTileKey key, LandscapeTile& tile) const
{
	const auto found = set.tiles.find({ key.x, key.z });
	if (found == set.tiles.end() || tile.heights.empty() || set.heightScaleM <= 0.0f) return;
	const LandscapeGeometry& g = tile.geometry;
	const int dim = g.heightDim(0);
	const double cell = static_cast<double>(g.tileM) / g.hCells, tx0 = key.x * static_cast<double>(g.tileM), tz0 = key.z * static_cast<double>(g.tileM);
	std::vector<uint16_t>& h = tile.heights[0];
	if (h.size() != static_cast<size_t>(dim) * static_cast<size_t>(dim)) return;
	const float k = 65535.0f / set.heightScaleM;
	bool any = false;
	for (const uint32_t index : found->second)
	{
		const LandscapeStamp& s = set.stamps[index];
		const std::vector<float>& img = *s.image;
		const double c = std::cos(s.yaw), sn = std::sin(s.yaw);
		for (int z = 0; z < dim; ++z)
			for (int x = 0; x < dim; ++x)
			{
				// Absolute position -> the stamp's own square (0..1 across it). The same for a sample two tiles share.
				const double dx = tx0 + x * cell - s.x, dz = tz0 + z * cell - s.z;
				const double u = (dx * c - dz * sn) / s.sizeX + 0.5, v = (dx * sn + dz * c) / s.sizeZ + 0.5;
				if (u < 0.0 || v < 0.0 || u > 1.0 || v > 1.0) continue;
				const double fx = u * (s.w - 1), fz = v * (s.h - 1);
				const int ix = std::min(static_cast<int>(fx), std::max(0, s.w - 2)), iz = std::min(static_cast<int>(fz), std::max(0, s.h - 2));
				const int jx = std::min(ix + 1, s.w - 1), jz = std::min(iz + 1, s.h - 1);
				const float ax = static_cast<float>(fx - ix), az = static_cast<float>(fz - iz);
				const auto at = [&](int px, int pz) { return img[static_cast<size_t>(pz) * static_cast<size_t>(s.w) + static_cast<size_t>(px)]; };
				const float value = (at(ix, iz) * (1 - ax) + at(jx, iz) * ax) * (1 - az) + (at(ix, jz) * (1 - ax) + at(jx, jz) * ax) * az;
				float fade = 1.0f;
				if (s.falloff > 0.0f)
				{
					const float edge = static_cast<float>(std::min(std::min(u, 1.0 - u), std::min(v, 1.0 - v)) * 2.0);   // 0 at the edge, 1 in the centre
					const float t = std::clamp(edge / s.falloff, 0.0f, 1.0f);
					fade = t * t * (3.0f - 2.0f * t);
				}
				const float add = value * fade * s.heightM * k;
				if (add == 0.0f) continue;
				uint16_t& out = h[static_cast<size_t>(z) * static_cast<size_t>(dim) + static_cast<size_t>(x)];
				out = static_cast<uint16_t>(std::lround(std::clamp(static_cast<float>(out) + add, 0.0f, 65535.0f)));
				any = true;
			}
	}
	if (any) tile.buildMips();
}

LandscapeTilePtr LandscapeEditSource::shown(LandscapeTileKey key, LandscapeTilePtr tile) const
{
	const auto set = stampSet();
	if (!tile || !set || !set->tiles.count({ key.x, key.z })) return tile;
	auto copy = std::make_shared<LandscapeTile>(*tile);
	applyStamps(*set, key, *copy);
	return copy;
}

std::vector<LandscapeTileKey> LandscapeEditSource::setStamps(std::vector<LandscapeStamp> stamps, float heightScaleM, bool patchCoarse)
{
	const auto old = stampSet();
	auto next = std::make_shared<StampSet>();
	float mn = 0.0f, scale = 0.0f;
	next->heightScaleM = (heightRange(mn, scale) && scale > 0.0f) ? scale : heightScaleM;
	next->stamps = std::move(stamps);
	std::map<std::pair<int, int>, bool> changed;
	for (uint32_t i = 0; i < next->stamps.size(); ++i)
	{
		const LandscapeStamp& s = next->stamps[i];
		const bool same = old && std::find(old->stamps.begin(), old->stamps.end(), s) != old->stamps.end();
		for (const LandscapeTileKey& key : footprint(s))
		{
			next->tiles[{ key.x, key.z }].push_back(i);
			if (!same) changed[{ key.x, key.z }] = true;
		}
	}
	if (old)
		for (const LandscapeStamp& s : old->stamps)
			if (std::find(next->stamps.begin(), next->stamps.end(), s) == next->stamps.end())
				for (const LandscapeTileKey& key : footprint(s)) changed[{ key.x, key.z }] = true;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_stampSet = next->stamps.empty() ? nullptr : std::move(next);
	}
	std::vector<LandscapeTileKey> keys;
	for (const auto& [k, yes] : changed) keys.push_back({ k.first, k.second });
	if (patchCoarse) patchCoarseFor(keys);
	return keys;
}

void LandscapeEditSource::patchCoarseFor(const std::vector<LandscapeTileKey>& keys)
{
	// The far levels: the coarse copy takes the tiles as they are shown now (made at the first change, like an edit's). About a
	// millisecond a tile (a decode, the stamps, the chain above grid 0), which is why a stamp that is being dragged waits with it.
	std::vector<std::pair<LandscapeTileKey, LandscapeTilePtr>> patch;
	for (const LandscapeTileKey& key : keys)
	{
		LandscapeTilePtr tile = shown(key, edited(key));
		if (!tile)
		{
			auto read = std::make_shared<LandscapeTile>();
			if (load(key, -1, *read)) tile = std::move(read);
		}
		if (tile) patch.emplace_back(key, std::move(tile));
	}
	if (patch.empty()) return;
	if (!m_coarse.valid())
		if (const LandscapeCoarseWorld* c = m_base->coarse()) m_coarse = *c;
	if (m_coarse.valid()) landscapeCoarsePatch(m_coarse, patch);
}

uint64_t LandscapeEditSource::contentHash(LandscapeTileKey key, const std::function<uint64_t(LandscapeTileKey)>& baseHash) const
{
	if (stamped(key))
	{
		LandscapeTile t;
		if (load(key, -1, t)) return fnv(encodeLandscapeTile(t));
	}
	if (const LandscapeTilePtr t = edited(key)) return fnv(encodeLandscapeTile(*t));
	return baseHash ? baseHash(key) : 0;
}

bool LandscapeEditSource::save(const std::filesystem::path& file, std::string* error)
{
	const auto fail = [&](const char* why) { if (error) *error = why; return false; };
	std::map<std::pair<int, int>, LandscapeTilePtr> tiles;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		tiles = m_tiles;
	}
	std::error_code ec;
	if (tiles.empty())
	{
		std::filesystem::remove(file, ec);
		m_dirty = false;
		return true;
	}
	const std::filesystem::path tmp = file.string() + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out) return fail("cannot write beside the pack");
		std::vector<uint8_t> head;
		put32(head, kEditsMagic);
		put32(head, kEditsVersion);
		put32(head, static_cast<uint32_t>(tiles.size()));
		put32(head, static_cast<uint32_t>(m_worldId));
		put32(head, static_cast<uint32_t>(m_worldId >> 32));
		out.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
		for (const auto& [k, t] : tiles)
		{
			const std::vector<uint8_t> blob = encodeLandscapeTile(*t);
			std::vector<uint8_t> rec;
			put32(rec, static_cast<uint32_t>(k.first));
			put32(rec, static_cast<uint32_t>(k.second));
			put32(rec, static_cast<uint32_t>(blob.size()));
			out.write(reinterpret_cast<const char*>(rec.data()), static_cast<std::streamsize>(rec.size()));
			out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
		}
		// The water table, when a brush changed it: a tail after the tile records (a reader from before it stops at the last record).
		if (m_water)
		{
			const std::vector<uint8_t> blob = encodeLandscapeWaterTable(*m_water);
			std::vector<uint8_t> rec;
			put32(rec, kEditsWaterTag);
			put32(rec, static_cast<uint32_t>(blob.size()));
			out.write(reinterpret_cast<const char*>(rec.data()), static_cast<std::streamsize>(rec.size()));
			out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
		}
		out.flush();
		if (!out) { out.close(); std::filesystem::remove(tmp, ec); return fail("the edits could not be written"); }
	}
	std::filesystem::rename(tmp, file, ec);
	if (ec) { std::filesystem::remove(tmp, ec); return fail("cannot replace the edits file"); }
	m_dirty = false;
	return true;
}

size_t LandscapeEditSource::loadFile(const std::filesystem::path& file)
{
	std::ifstream in(file, std::ios::binary | std::ios::ate);
	if (!in) return 0;
	const std::streamoff size = in.tellg();
	if (size < 20) return 0;
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	in.seekg(0);
	in.read(reinterpret_cast<char*>(bytes.data()), size);
	if (!in || get32(bytes.data()) != kEditsMagic || get32(bytes.data() + 4) != kEditsVersion) return 0;
	const uint32_t count = get32(bytes.data() + 8);
	const uint64_t stamp = static_cast<uint64_t>(get32(bytes.data() + 12)) | static_cast<uint64_t>(get32(bytes.data() + 16)) << 32;
	if (stamp != 0 && m_worldId != 0 && stamp != m_worldId) return 0;   // edits of another world under the same file name
	std::vector<std::pair<LandscapeTileKey, LandscapeTilePtr>> tiles;
	size_t at = 20;
	uint32_t i = 0;
	for (; i < count; ++i)
	{
		if (bytes.size() - at < 12) break;
		const LandscapeTileKey key{ static_cast<int>(get32(&bytes[at])), static_cast<int>(get32(&bytes[at + 4])) };
		const uint32_t blob = get32(&bytes[at + 8]);
		at += 12;
		if (blob > bytes.size() - at) break;   // cut short: the records before it stand
		auto tile = std::make_shared<LandscapeTile>();
		if (!decodeLandscapeTile(&bytes[at], blob, *tile) || !(tile->geometry == geometry())) break;
		at += blob;
		if (key.x < 0 || key.z < 0 || key.x >= tilesX() || key.z >= tilesZ()) continue;
		tiles.emplace_back(key, std::move(tile));
	}
	// The water table a brush left, after the last record (only behind a file that is whole).
	if (i == count && bytes.size() - at >= 8 && get32(&bytes[at]) == kEditsWaterTag)
	{
		const uint32_t n = get32(&bytes[at + 4]);
		LandscapeWaterTable table;
		if (n <= bytes.size() - at - 8 && decodeLandscapeWaterTable(&bytes[at + 8], n, table)) setWaterTable(std::move(table));
	}
	set(tiles);
	m_dirty = false;
	return tiles.size();
}

// ── region terrain -> tiles ──────────────────────────────────────────────────

bool landscapeWriteRegionTerrain(const TerrainComponent& tc, const LandscapeGeometry& g, LandscapeTileKey origin, int n,
                                 const LandscapeTileTerrainParams& params, const std::function<LandscapeTilePtr(LandscapeTileKey)>& current,
                                 std::vector<std::pair<LandscapeTileKey, LandscapeTilePtr>>& out, const std::vector<uint8_t>* foliageDensity)
{
	out.clear();
	const uint32_t cells = g.hCells, res = static_cast<uint32_t>(n) * cells + 1, dim = cells + 1;
	if (n < 1 || tc.resolution != res || tc.sculptHeights.size() != static_cast<size_t>(res) * res || params.heightScaleM <= 0.0f) return false;
	const uint32_t wt = static_cast<uint32_t>(g.weightDim(0)), wr = static_cast<uint32_t>(n) * wt;
	const int C = g.channels;
	const std::vector<uint8_t>* pages[3] = { &tc.layerWeights, &tc.layerWeights2, &tc.layerWeights3 };
	const bool haveWeights = tc.weightRes == wr;
	const float k = 65535.0f / params.heightScaleM;

	for (int tj = 0; tj < n; ++tj)
		for (int ti = 0; ti < n; ++ti)
		{
			const LandscapeTileKey key{ origin.x + ti, origin.z + tj };
			const LandscapeTilePtr was = current(key);
			if (!was || was->heights.empty() || was->weights.empty() || !(was->geometry == g)) return false;
			auto tile = std::make_shared<LandscapeTile>(*was);   // water rasters and anything else a tile carries stay
			std::vector<uint16_t>& h = tile->heights[0];
			for (uint32_t z = 0; z < dim; ++z)
				for (uint32_t x = 0; x < dim; ++x)
				{
					const float m = tc.sculptHeights[(static_cast<size_t>(tj) * cells + z) * res + static_cast<size_t>(ti) * cells + x];
					h[static_cast<size_t>(z) * dim + x] = static_cast<uint16_t>(std::lround(std::clamp((m - params.heightMinM) * k, 0.0f, 65535.0f)));
				}
			if (haveWeights)
			{
				std::vector<uint8_t>& w = tile->weights[0];
				for (uint32_t z = 0; z < wt; ++z)
					for (uint32_t x = 0; x < wt; ++x)
					{
						const size_t src = (static_cast<size_t>(tj) * wt + z) * wr + static_cast<size_t>(ti) * wt + x;
						for (int c = 0; c < C && c < 12; ++c)
						{
							const std::vector<uint8_t>& page = *pages[c / 4];
							if (page.size() == static_cast<size_t>(wr) * wr * 4)
								w[(static_cast<size_t>(z) * wt + x) * static_cast<size_t>(C) + static_cast<size_t>(c)] = page[src * 4 + static_cast<size_t>(c % 4)];
						}
					}
			}
			// Water: the region's field (LandscapeRegionTerrain has one at the tiles' raster once the world has a water table or a
			// brush made one) back into the tile's rasters; a tile without a wet cell holds none. A terrain without that field
			// leaves the tile's water as it is.
			if (tc.water.res == wr && tc.water.coverage.size() == static_cast<size_t>(wr) * wr && tc.water.owner.size() == tc.water.coverage.size())
			{
				std::vector<uint8_t> cov(static_cast<size_t>(wt) * wt);
				std::vector<uint16_t> own(static_cast<size_t>(wt) * wt);
				bool wet = false;
				for (uint32_t z = 0; z < wt; ++z)
					for (uint32_t x = 0; x < wt; ++x)
					{
						const size_t src = (static_cast<size_t>(tj) * wt + z) * wr + static_cast<size_t>(ti) * wt + x;
						cov[static_cast<size_t>(z) * wt + x] = tc.water.coverage[src];
						own[static_cast<size_t>(z) * wt + x] = tc.water.owner[src];
						wet = wet || tc.water.coverage[src] != 0;
					}
				// A tile that carried rasters keeps them even when they are all dry now (a world with a sea has them on every tile):
				// an untouched region must come back byte for byte.
				if (wet || was->hasWater()) { tile->waterCoverage = std::move(cov); tile->waterOwner = std::move(own); }
				else                        { tile->waterCoverage.clear(); tile->waterOwner.clear(); }
			}
			// The painted foliage density: a tile that is 255 everywhere holds none (what an unpainted tile is).
			if (foliageDensity && foliageDensity->size() == static_cast<size_t>(wr) * wr)
			{
				std::vector<uint8_t> f(static_cast<size_t>(wt) * wt);
				bool painted = false;
				for (uint32_t z = 0; z < wt; ++z)
					for (uint32_t x = 0; x < wt; ++x)
					{
						const uint8_t v = (*foliageDensity)[(static_cast<size_t>(tj) * wt + z) * wr + static_cast<size_t>(ti) * wt + x];
						f[static_cast<size_t>(z) * wt + x] = v;
						painted = painted || v != 255;
					}
				if (painted) tile->foliageDensity = std::move(f);
				else tile->foliageDensity.clear();
			}
			if (tile->heights[0] == was->heights[0] && tile->weights[0] == was->weights[0] && tile->foliageDensity == was->foliageDensity
			    && tile->waterCoverage == was->waterCoverage && tile->waterOwner == was->waterOwner)
				continue;
			tile->buildMips();
			out.emplace_back(key, std::move(tile));
		}
	return true;
}

// ── bake into the pack ───────────────────────────────────────────────────────

bool landscapePackBakeEdits(const std::string& packPath, const LandscapeEditSource& edits, std::string* error)
{
	const auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
	if (edits.editedKeys().empty() && edits.stampedKeys().empty()) return true;
	const std::string tmp = packPath + ".bake.tmp";
	std::error_code ec;
	{
		std::string err;
		const auto pack = LandscapePackSource::open(packPath, &err);
		if (!pack) return fail("not a landscape pack: " + err);
		if (!(pack->geometry() == edits.geometry()) || pack->tilesX() != edits.tilesX() || pack->tilesZ() != edits.tilesZ())
			return fail("the edits are of another world");
		float minM = 0.0f, scaleM = 0.0f;
		pack->heightRange(minM, scaleM);
		LandscapePackWriter w;
		if (!w.begin(tmp, pack->geometry(), pack->tilesX(), pack->tilesZ(), pack->coarseHeightLevel(), minM, scaleM))
			return fail("cannot write beside the pack");
		w.setCoarseRegions(pack->coarseRegionTiles());   // laid out as the pack is: whole, or in regions of the same size
		if (edits.water()) w.setWaterTable(*edits.water());   // the pack's, or the one a water brush changed
		if (pack->hlod()) w.setHlodTable(*pack->hlod());
		if (pack->farImage()) w.setFarTable(*pack->farImage());
		std::vector<uint8_t> blob;
		for (int z = 0; z < pack->tilesZ(); ++z)
			for (int x = 0; x < pack->tilesX(); ++x)
			{
				const LandscapeTileKey key{ x, z };
				// An edited or stamped tile as the overlay shows it (LandscapeEditSource::load lays the stamps over).
				if (edits.edited(key) || (edits.stamped(key) && pack->has(key)))
				{
					LandscapeTile t;
					if (!edits.load(key, -1, t)) { std::filesystem::remove(tmp, ec); return fail("an edited tile cannot be read"); }
					blob = encodeLandscapeTile(t);
				}
				else if (!pack->has(key)) continue;
				else if (!pack->readStored(key, blob)) { std::filesystem::remove(tmp, ec); return fail("a tile of the pack cannot be read"); }
				if (!w.add(key, blob)) { std::filesystem::remove(tmp, ec); return fail("a tile could not be written"); }
			}
		if (!w.finish()) { std::filesystem::remove(tmp, ec); return fail("the pack could not be finished"); }
	}   // both files closed: the pack is renamed over (Windows)
	std::filesystem::rename(tmp, packPath, ec);
	if (ec) { std::filesystem::remove(tmp, ec); return fail("cannot replace the pack"); }
	return true;
}

// ── the session ──────────────────────────────────────────────────────────────

LandscapeEditSession::LandscapeEditSession(LandscapeRuntime& runtime, LandscapeEditSource& edits) : m_runtime(runtime), m_edits(edits)
{
	if (!m_edits.heightRange(m_params.heightMinM, m_params.heightScaleM) || m_params.heightScaleM <= 0.0f)
	{
		m_params.heightMinM = runtime.config().heightMinM;
		m_params.heightScaleM = runtime.config().heightScaleM;
	}
}

LandscapeTilePtr LandscapeEditSession::read(LandscapeTileKey key) const
{
	if (key.x < 0 || key.z < 0 || key.x >= m_edits.tilesX() || key.z >= m_edits.tilesZ()) return nullptr;
	if (const auto it = m_tiles.find({ key.x, key.z }); it != m_tiles.end()) return it->second;
	LandscapeTilePtr t = m_edits.edited(key);
	if (!t)
	{
		// The store's tile may hold fewer levels than a tile has (LandscapeStoreConfig): an edit needs all of level 0, so read the source.
		auto tile = std::make_shared<LandscapeTile>();
		if (!m_edits.base().load(key, -1, *tile)) return nullptr;
		t = std::move(tile);
	}
	m_tiles[{ key.x, key.z }] = t;
	return t;
}

bool LandscapeEditSession::focus(double absX, double absZ, double radiusM)
{
	const LandscapeGeometry& g = m_edits.geometry();
	const double tileM = g.tileM, sample = tileM / g.hCells;
	const double reach = radiusM + sample;   // a sample to spare: the brush never writes the region's outer border, which tiles outside share
	// Still inside the region the session holds: nothing to do.
	if (m_n > 0)
	{
		const double x0 = m_region.origin.x * tileM, z0 = m_region.origin.z * tileM, side = m_n * tileM;
		const bool fitsX = (absX - reach >= x0 || m_region.origin.x == 0) && (absX + reach <= x0 + side || m_region.origin.x + m_n == m_edits.tilesX());
		const bool fitsZ = (absZ - reach >= z0 || m_region.origin.z == 0) && (absZ + reach <= z0 + side || m_region.origin.z + m_n == m_edits.tilesZ());
		if (fitsX && fitsZ && absX >= x0 && absX <= x0 + side && absZ >= z0 && absZ <= z0 + side) return true;
	}
	commit();
	m_n = 0;
	m_tiles.clear();
	if (absX < 0.0 || absZ < 0.0 || absX > m_edits.tilesX() * tileM || absZ > m_edits.tilesZ() * tileM) return false;
	// The smallest of 1, 2, 4 tiles whose span around the brush fits; the world's own edge needs no spare sample.
	for (const int n : { 1, 2, 4 })
	{
		if (n > m_edits.tilesX() || n > m_edits.tilesZ()) break;
		const auto originOf = [&](double c, int tiles) {
			const int lo = static_cast<int>(std::floor((c - reach) / tileM));
			return std::clamp(lo, 0, tiles - n);
		};
		const LandscapeTileKey origin{ originOf(absX, m_edits.tilesX()), originOf(absZ, m_edits.tilesZ()) };
		const auto fits = [&](double c, int o, int tiles) {
			return (c - reach >= o * tileM || o == 0) && (c + reach <= (o + n) * tileM || o + n == tiles);
		};
		if (!fits(absX, origin.x, m_edits.tilesX()) || !fits(absZ, origin.z, m_edits.tilesZ())) continue;
		LandscapeRegionTerrain region;
		// With the world's water (an empty table where it has none), so the water brush finds the region's lakes and a free id.
		static const LandscapeWaterTable kNoWater;
		const LandscapeWaterTable* waterTable = m_edits.water() ? m_edits.water() : &kNoWater;
		if (!landscapeRegionTerrain([this](LandscapeTileKey k) { return read(k); }, origin, n, m_params, waterTable, region)) return false;
		m_region = std::move(region);
		m_n = n;
		return true;
	}
	return false;
}

bool LandscapeEditSession::heightAt(double absX, double absZ, float& outM) const
{
	if (m_n == 0) return false;
	const TerrainComponent& tc = m_region.terrain;
	const double side = tc.sizeX, cell = side / (tc.resolution - 1);
	const double fx = (absX - m_region.centre.x + side * 0.5) / cell, fz = (absZ - m_region.centre.z + side * 0.5) / cell;
	if (fx < 0.0 || fz < 0.0 || fx > tc.resolution - 1 || fz > tc.resolution - 1) return false;
	const uint32_t x0 = std::min(static_cast<uint32_t>(fx), tc.resolution - 2), z0 = std::min(static_cast<uint32_t>(fz), tc.resolution - 2);
	const float tx = static_cast<float>(fx - x0), tz = static_cast<float>(fz - z0);
	const auto h = [&](uint32_t x, uint32_t z) { return tc.sculptHeights[static_cast<size_t>(z) * tc.resolution + x]; };
	outM = (h(x0, z0) * (1 - tx) + h(x0 + 1, z0) * tx) * (1 - tz) + (h(x0, z0 + 1) * (1 - tx) + h(x0 + 1, z0 + 1) * tx) * tz;
	return true;
}

void LandscapeEditSession::beginStroke()
{
	if (m_inStroke) endStroke();
	m_inStroke = true;
	m_stroke.clear();
}

void LandscapeEditSession::apply(const std::vector<std::pair<LandscapeTileKey, LandscapeTilePtr>>& tiles, bool finish)
{
	if (tiles.empty()) return;
	// The coarse copy waits for the end of the stroke: patching it rebuilds the whole chain above grid 0.
	m_edits.set(tiles, finish);
	for (const auto& [key, tile] : tiles)
	{
		if (tile) m_tiles[{ key.x, key.z }] = tile;
		else m_tiles.erase({ key.x, key.z });
	}
	// In place where the tile is resident (the atlas uploads it again into its slot); a removed edit, or `finish`, goes through the
	// drop-and-reload path, which is also what makes ground bodies, foliage, navigation and water build again.
	std::vector<std::pair<LandscapeTileKey, LandscapeTilePtr>> inPlace;
	std::vector<LandscapeTileKey> reload;
	for (const auto& [key, tile] : tiles)
	{
		if (tile && !finish) inPlace.emplace_back(key, m_edits.shown(key, tile));   // what is drawn has the stamps over it
		else reload.push_back(key);
	}
	m_runtime.tilesReplaced(inPlace);
	m_runtime.tileChanged(reload);
	if (finish) m_runtime.coarseChanged();
}

size_t LandscapeEditSession::commit()
{
	if (m_n == 0) return 0;
	std::vector<std::pair<LandscapeTileKey, LandscapeTilePtr>> changed;
	if (!landscapeWriteRegionTerrain(m_region.terrain, m_edits.geometry(), m_region.origin, m_n, m_params,
	                                 [this](LandscapeTileKey k) { return read(k); }, changed, &m_region.foliageDensity) || changed.empty())
		return 0;
	// A body the water brush made or moved goes into the overlay's water table (bodies are never taken out: one without a wet
	// cell left is a table row nobody reads).
	{
		const LandscapeWaterTable* now = m_edits.water();
		LandscapeWaterTable table = now ? *now : LandscapeWaterTable{};
		bool differs = false;
		for (const water::Body& b : m_region.terrain.water.bodies)
		{
			auto it = std::find_if(table.bodies.begin(), table.bodies.end(), [&](const LandscapeWaterBody& w) { return w.id == b.id; });
			if (it == table.bodies.end()) { table.bodies.push_back({}); it = table.bodies.end() - 1; it->id = b.id; it->level = b.level - 1.0f; }
			if (it->level == b.level) continue;
			it->level = b.level;
			it->physics = b.physics;
			it->underwaterFog = b.underwaterFog;
			it->underwaterCaustics = b.underwaterCaustics;
			it->tessellation = b.tessellation;
			differs = true;
		}
		if (differs) m_edits.setWaterTable(std::move(table));
	}
	// The undo step keeps what each tile was BEFORE its first change in this stroke.
	const bool own = !m_inStroke;
	if (own) m_stroke.clear();
	for (const auto& [key, tile] : changed)
	{
		auto it = std::find_if(m_stroke.begin(), m_stroke.end(), [&](const Change& c) { return c.key == key; });
		if (it == m_stroke.end()) m_stroke.push_back({ key, m_edits.edited(key), tile });
		else it->after = tile;
	}
	apply(changed, false);
	if (own) { m_inStroke = true; endStroke(); }
	return changed.size();
}

void LandscapeEditSession::endStroke()
{
	if (!m_inStroke) return;
	commit();
	m_inStroke = false;
	if (m_stroke.empty()) return;
	std::vector<std::pair<LandscapeTileKey, LandscapeTilePtr>> last;
	for (const Change& c : m_stroke) last.emplace_back(c.key, c.after);
	apply(last, true);
	m_steps.resize(m_undoAt);
	m_steps.push_back(std::move(m_stroke));
	if (m_steps.size() > kMaxSteps) m_steps.erase(m_steps.begin());
	m_undoAt = m_steps.size();
	m_stroke.clear();
}

bool LandscapeEditSession::undo()
{
	if (m_inStroke) endStroke();
	if (!canUndo()) return false;
	const Step& step = m_steps[--m_undoAt];
	std::vector<std::pair<LandscapeTileKey, LandscapeTilePtr>> tiles;
	for (const Change& c : step) tiles.emplace_back(c.key, c.before);
	apply(tiles, true);
	m_n = 0;   // the region terrain shows what was undone: assembled again at the next focus()
	return true;
}

bool LandscapeEditSession::redo()
{
	if (m_inStroke) endStroke();
	if (!canRedo()) return false;
	const Step& step = m_steps[m_undoAt++];
	std::vector<std::pair<LandscapeTileKey, LandscapeTilePtr>> tiles;
	for (const Change& c : step) tiles.emplace_back(c.key, c.after);
	apply(tiles, true);
	m_n = 0;
	return true;
}

} // namespace HE
