#include "HorizonScene/LandscapeFarBake.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace HE
{

namespace
{
constexpr uint64_t kFnvBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint64_t fnv(uint64_t h, const void* data, size_t n)
{
	const uint8_t* p = static_cast<const uint8_t*>(data);
	for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * kFnvPrime;
	return h;
}
template <typename T> uint64_t fnvValue(uint64_t h, const T& v) { return fnv(h, &v, sizeof(v)); }

int log2Of(int v) { int l = 0; while ((1 << l) < v) ++l; return l; }
} // namespace

uint64_t landscapeFarPaintHash(const LandscapeFarPaint& paint)
{
	uint64_t h = kFnvBasis;
	for (const glm::vec3& c : paint.layerColor) h = fnv(h, &c.x, sizeof(float) * 3);
	for (const float a : paint.layerAo) h = fnvValue(h, a);
	return h;
}

LandscapeFarTileHash landscapePackTileHash(const LandscapePackSource& pack)
{
	// shortcut: hashes every stored byte of the world on every export (3.85 GB for 100 km, seconds on an NVMe), keep a per-tile hash in the
	// pack index when that shows up in an export's time.
	return [&pack](LandscapeTileKey key) { return pack.storedHash(key); };
}

LandscapeFarPainter landscapeFarWeightPainter(const LandscapeFarPaint& paint)
{
	return [paint](const LandscapeTileSource& source, const LandscapeFarLayout& layout, int rx, int rz, std::vector<uint8_t>& rgba) {
		const LandscapeGeometry& g = source.geometry();
		const int tpt = layout.texelsPerTile, side = layout.regionSide();
		const int level = log2Of(g.wTexels / tpt);          // the weight mip with tpt texels a side: the 2 x 2 means are the tile's own
		const int channels = std::min<int>(g.channels, 12);
		rgba.assign(static_cast<size_t>(side) * static_cast<size_t>(side) * 4, 0);
		LandscapeTile tile;
		for (int tz = 0; tz < layout.regionTiles; ++tz)
			for (int tx = 0; tx < layout.regionTiles; ++tx)
			{
				const LandscapeTileKey key{ rx * layout.regionTiles + tx, rz * layout.regionTiles + tz };
				if (key.x >= layout.tilesX || key.z >= layout.tilesZ) continue;
				// A tile the world does not have stays black: nothing is drawn there either. One that loads without the level asked for
				// (a layout finer or coarser than the tile's pyramid) is a region that cannot be painted, not a black one.
				if (!source.load(key, level + 1, tile)) continue;
				if (static_cast<int>(tile.weights.size()) <= level) return false;
				const std::vector<uint8_t>& w = tile.weights[static_cast<size_t>(level)];
				if (w.size() < static_cast<size_t>(tpt) * static_cast<size_t>(tpt) * g.channels) return false;
				for (int z = 0; z < tpt; ++z)
					for (int x = 0; x < tpt; ++x)
					{
						const uint8_t* t = &w[(static_cast<size_t>(z) * static_cast<size_t>(tpt) + static_cast<size_t>(x)) * g.channels];
						// kLandscapeFS: albedo = sum of weight x colour, ao = 1 minus what each layer takes away.
						glm::vec3 albedo(0.0f);
						float ao = 1.0f;
						for (int c = 0; c < channels; ++c)
						{
							const float wt = static_cast<float>(t[c]) / 255.0f;
							albedo += wt * paint.layerColor[static_cast<size_t>(c)];
							ao += wt * (paint.layerAo[static_cast<size_t>(c)] - 1.0f);
						}
						uint8_t* d = &rgba[(static_cast<size_t>(tz * tpt + z) * static_cast<size_t>(side) + static_cast<size_t>(tx * tpt + x)) * 4];
						d[0] = landscapeFarEncodeSrgb(albedo.r);
						d[1] = landscapeFarEncodeSrgb(albedo.g);
						d[2] = landscapeFarEncodeSrgb(albedo.b);
						d[3] = static_cast<uint8_t>(std::lround(std::clamp(ao, 0.0f, 1.0f) * 255.0f));
					}
			}
		return true;
	};
}

LandscapeFarLayout planLandscapeFar(const LandscapeGeometry& geometry, int tilesX, int tilesZ, uint64_t budgetBytes, int maxSide, int regionTiles)
{
	LandscapeFarLayout l{ tilesX, tilesZ, std::max(1, regionTiles), std::max<int>(1, geometry.wTexels) };
	// The mip chain adds a third. Halve until the image fits the budget and the largest texture.
	// What is ever in memory and in one texture is a render window's part of the image (LandscapeRuntimeConfig::renderWindowTiles), not
	// the world's: a world larger than the window gets the resolution of a world the window's size.
	const uint64_t wx = static_cast<uint64_t>(std::min(tilesX, kLandscapePackWholeCoarseMaxTiles)), wz = static_cast<uint64_t>(std::min(tilesZ, kLandscapePackWholeCoarseMaxTiles));
	const auto bytes = [&] { return wx * wz * static_cast<uint64_t>(l.texelsPerTile) * static_cast<uint64_t>(l.texelsPerTile) * 4ull * 4ull / 3ull; };
	const auto side = [&] { return static_cast<int>(std::max(wx, wz)) * l.texelsPerTile; };
	// Not below the coarsest weight mip a tile stores: that is the last level a painter can read.
	const int floorTexels = std::max(1, geometry.weightDim(std::max(0, geometry.weightLevels() - 1)));
	while (l.texelsPerTile > floorTexels && (bytes() > budgetBytes || side() > maxSide)) l.texelsPerTile /= 2;
	return l;
}

void bakeLandscapeFar(const LandscapeTileSource& source, const LandscapeFarTileHash& tileHash, const LandscapeFarBakeOptions& options,
                      const LandscapeFarTable* previous, LandscapeFarTable& out, LandscapeFarBakeStats& stats)
{
	stats = LandscapeFarBakeStats{};
	out = LandscapeFarTable{};
	out.layout = planLandscapeFar(source.geometry(), source.tilesX(), source.tilesZ(), options.budgetBytes, options.maxSide, options.regionTiles);
	if (!out.layout.valid() || !options.painter) return;
	const LandscapeFarLayout& l = out.layout;
	const int rxN = l.regionsX(), rzN = l.regionsZ();
	out.regions.resize(static_cast<size_t>(rxN) * static_cast<size_t>(rzN));
	stats.regions = rxN * rzN;
	const bool usePrevious = previous && previous->layout == l && previous->regions.size() == out.regions.size();
	const size_t imageBytes = static_cast<size_t>(l.regionSide()) * static_cast<size_t>(l.regionSide()) * 4;

	int done = 0;
	for (int rz = 0; rz < rzN && !stats.cancelled; ++rz)
		for (int rx = 0; rx < rxN; ++rx)
		{
			const size_t i = static_cast<size_t>(rz) * static_cast<size_t>(rxN) + static_cast<size_t>(rx);
			uint64_t h = fnvValue(kFnvBasis, kLandscapeFarVersion);
			h = fnvValue(h, options.painterHash);
			h = fnvValue(h, l.texelsPerTile);
			h = fnvValue(h, l.regionTiles);
			for (int tz = 0; tz < l.regionTiles; ++tz)
				for (int tx = 0; tx < l.regionTiles; ++tx)
				{
					const LandscapeTileKey key{ rx * l.regionTiles + tx, rz * l.regionTiles + tz };
					if (key.x < l.tilesX && key.z < l.tilesZ) h = fnvValue(h, tileHash ? tileHash(key) : 0);
				}
			LandscapeFarRegion& r = out.regions[i];
			if (usePrevious && previous->regions[i].hash == h && previous->regions[i].baked())
			{
				r = previous->regions[i];
				++stats.reused;
			}
			else if (options.painter(source, l, rx, rz, r.rgba) && r.rgba.size() == imageBytes)
			{
				r.hash = h;
				out.compact(i);   // kept stored from here on: the table of a large world is then its file's size, not its pixels'
				++stats.baked;
			}
			else
			{
				r = LandscapeFarRegion{};
				++stats.failed;
			}
			if (options.progress && !options.progress(++done, stats.regions)) { stats.cancelled = true; break; }
		}
}

} // namespace HE
