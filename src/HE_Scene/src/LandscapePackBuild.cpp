#include "HorizonScene/LandscapePackBuild.h"

#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/TerrainHeightmap.h"
#include "HorizonScene/TerrainMeshGenerator.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>

namespace fs = std::filesystem;

namespace HE
{

namespace
{
using Clock = std::chrono::steady_clock;

struct MadeTile
{
	std::vector<uint8_t>  blob;
	LandscapeCoarseSample coarse;
	bool                  ok = false;
};

// Tiles are made on plain threads in batches and appended in key order. The packer only ever sees finished blobs.
LandscapeBuildResult buildPack(const std::string& path, const LandscapeGeometry& g, int tilesX, int tilesZ, float heightMinM, float heightScaleM,
                               const LandscapeBuildOptions& opt, const std::function<LandscapeTile(LandscapeTileKey)>& makeTile)
{
	LandscapeBuildResult r;
	r.tilesX = tilesX;
	r.tilesZ = tilesZ;
	r.heightMinM = heightMinM;
	r.heightScaleM = heightScaleM;
	if (path.empty()) { r.error = "No pack file given."; return r; }
	if (!g.valid()) { r.error = "The tile geometry is not valid (powers of two, 1..16 channels)."; return r; }
	if (tilesX <= 0 || tilesZ <= 0) { r.error = "The world has no tiles."; return r; }
	const auto t0 = Clock::now();

	std::error_code ec;
	const fs::path target(path);
	if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
	// Written beside the target and moved over it at the end: a runtime that has the old pack open keeps reading it, and a cancelled
	// or failed build leaves the old file alone.
	const std::string tmp = path + ".part";
	const size_t total = static_cast<size_t>(tilesX) * static_cast<size_t>(tilesZ);
	const LandscapeCoarseLayout layout = LandscapeCoarseLayout::forGeometry(g, 4);

	bool failed = false;
	{
		LandscapePackWriter pack;
		if (!pack.begin(tmp, g, tilesX, tilesZ, 4, heightMinM, heightScaleM)) { r.error = "Cannot write \"" + tmp + "\"."; return r; }
		unsigned threads = opt.threads ? opt.threads : std::max(1u, std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1u);
		threads = std::min(threads, 64u);
		const size_t batch = static_cast<size_t>(threads) * 2;
		for (size_t first = 0; first < total && !failed && !r.cancelled; first += batch)
		{
			const size_t last = std::min(total, first + batch);
			std::vector<std::future<MadeTile>> jobs;
			jobs.reserve(last - first);
			for (size_t i = first; i < last; ++i)
			{
				const LandscapeTileKey key{ static_cast<int>(i % static_cast<size_t>(tilesX)), static_cast<int>(i / static_cast<size_t>(tilesX)) };
				jobs.push_back(std::async(std::launch::async, [&makeTile, &opt, &layout, key]() {
					MadeTile m;
					const LandscapeTile t = makeTile(key);
					m.blob = encodeLandscapeTile(t, opt.codec);
					m.coarse = landscapeCoarseSampleOf(t, layout);
					m.ok = !m.blob.empty();
					return m;
				}));
			}
			for (size_t i = first; i < last; ++i)
			{
				MadeTile m = jobs[i - first].get();   // always join every job of the batch, even after a failure
				if (failed) continue;
				const LandscapeTileKey key{ static_cast<int>(i % static_cast<size_t>(tilesX)), static_cast<int>(i / static_cast<size_t>(tilesX)) };
				if (!m.ok || !pack.add(key, m.blob, &m.coarse)) { failed = true; r.error = "Cannot add tile (" + std::to_string(key.x) + ", " + std::to_string(key.z) + ") to the pack."; }
			}
			if (!failed && opt.progress && !opt.progress(pack.tilesWritten(), total)) r.cancelled = true;
		}
		if (!failed && !r.cancelled && !pack.finish()) { failed = true; r.error = "Cannot finish \"" + tmp + "\" (disk full?)."; }
		r.tilesWritten = pack.tilesWritten();
	}
	if (failed || r.cancelled)
	{
		fs::remove(tmp, ec);
		return r;
	}
	fs::rename(tmp, target, ec);
	if (ec)
	{
		fs::remove(target, ec);   // Windows cannot rename over a file; the old pack is replaced by the new one
		fs::rename(tmp, target, ec);
	}
	if (ec) { r.error = "Cannot replace \"" + path + "\": " + ec.message(); fs::remove(tmp, ec); return r; }
	r.packBytes = static_cast<uint64_t>(fs::file_size(target, ec));
	r.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
	r.ok = true;
	return r;
}

uint16_t quantize(double metres, float minM, float scaleM)
{
	const double v = (metres - static_cast<double>(minM)) / static_cast<double>(scaleM) * 65535.0;
	return static_cast<uint16_t>(std::clamp(std::llround(v), 0ll, 65535ll));
}

// Bilinear read of a square-ish grid at fractional (x, z) in samples, clamped to the grid.
template <class T>
double bilinear(const std::vector<T>& v, int w, int h, double x, double z)
{
	x = std::clamp(x, 0.0, static_cast<double>(w - 1));
	z = std::clamp(z, 0.0, static_cast<double>(h - 1));
	const int x0 = static_cast<int>(std::floor(x)), z0 = static_cast<int>(std::floor(z));
	const int x1 = std::min(x0 + 1, w - 1), z1 = std::min(z0 + 1, h - 1);
	const double fx = x - x0, fz = z - z0;
	const auto at = [&](int xx, int zz) { return static_cast<double>(v[static_cast<size_t>(zz) * w + xx]); };
	return (at(x0, z0) * (1 - fx) + at(x1, z0) * fx) * (1 - fz) + (at(x0, z1) * (1 - fx) + at(x1, z1) * fx) * fz;
}

// Rounds a row of weights to bytes that add up to 255 (the biggest takes the rounding error), like the engine's paint pages.
void normaliseTo255(const double* in, int channels, uint8_t* out)
{
	double sum = 0.0;
	for (int k = 0; k < channels; ++k) sum += in[k];
	if (sum <= 1e-9) { std::fill(out, out + channels, uint8_t(0)); out[0] = 255; return; }
	int acc = 0, best = 0;
	for (int k = 0; k < channels; ++k)
	{
		out[k] = static_cast<uint8_t>(std::clamp(std::llround(255.0 * in[k] / sum), 0ll, 255ll));
		acc += out[k];
		if (out[k] > out[best]) best = k;
	}
	out[best] = static_cast<uint8_t>(std::clamp(out[best] + 255 - acc, 0, 255));
}

// Layer weights from the shape of the heights: 0 = flat ground, 1 = steep rock, 2 = high ground. Used by the heightmap import, which has pictures
// of heights and nothing else; the user repaints from there. The numbers are the engine's usual slope/height split, not a measurement.
void autoWeightsFor(LandscapeTile& t, float heightMinM, float heightScaleM)
{
	const LandscapeGeometry& g = t.geometry;
	const int W = g.wTexels, C = g.channels, n = g.heightDim(0);
	const int step = std::max(1, static_cast<int>(g.hCells) / std::max(1, static_cast<int>(g.wTexels)));
	const double rasterH = static_cast<double>(g.tileM) / g.hCells;
	t.weights.assign(1, std::vector<uint8_t>(static_cast<size_t>(W) * W * C, 0));
	const auto& H = t.heights[0];
	for (int z = 0; z < W; ++z)
		for (int x = 0; x < W; ++x)
		{
			const int hx = std::min(x * step, n - 1), hz = std::min(z * step, n - 1);
			const int hx1 = std::min(hx + 1, n - 1), hz1 = std::min(hz + 1, n - 1);
			const double scale = static_cast<double>(heightScaleM) / 65535.0;
			const double dx = (H[static_cast<size_t>(hz) * n + hx1] - H[static_cast<size_t>(hz) * n + hx]) * scale;
			const double dz = (H[static_cast<size_t>(hz1) * n + hx] - H[static_cast<size_t>(hz) * n + hx]) * scale;
			const double slope = std::min(1.0, std::sqrt(dx * dx + dz * dz) / rasterH);        // 1 = 45 degrees and more
			const double high = std::clamp((H[static_cast<size_t>(hz) * n + hx] / 65535.0 - 0.6) / 0.25, 0.0, 1.0);
			double s[16] = {};
			s[0] = (1.0 - slope) * (1.0 - high);
			if (C > 1) s[1] = slope;
			if (C > 2) s[2] = (1.0 - slope) * high;
			normaliseTo255(s, C, &t.weights[0][(static_cast<size_t>(z) * W + x) * C]);
		}
	(void)heightMinM;
}

std::string lowerExt(const std::string& path)
{
	std::string e = fs::path(path).extension().string();
	if (!e.empty() && e[0] == '.') e.erase(0, 1);
	for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return e;
}
} // namespace

std::string LandscapeBuildResult::summary() const
{
	char b[256];
	if (cancelled) return "Cancelled.";
	if (!ok) return error.empty() ? "Failed." : error;
	std::snprintf(b, sizeof b, "%zu tiles (%d x %d), %.1f MB, %.1f s; heights %.1f m + %.1f m.", tilesWritten, tilesX, tilesZ,
	              static_cast<double>(packBytes) / 1048576.0, seconds, static_cast<double>(heightMinM), static_cast<double>(heightScaleM));
	return b;
}

// ── Procedural ───────────────────────────────────────────────────────────────
LandscapeBuildResult buildProceduralPack(const std::string& path, const ProceduralLandscapeParams& params, const LandscapeBuildOptions& options)
{
	const ProceduralLandscapeSource source(params);
	float mn = 0.0f, sc = params.heightScale;
	source.heightRange(mn, sc);
	return buildPack(path, params.geometry, params.tilesX, params.tilesZ, mn, sc, options, [&source](LandscapeTileKey k) { return source.generate(k); });
}

// ── Migration ────────────────────────────────────────────────────────────────
LandscapeBuildResult migrateTerrainToPack(const TerrainComponent& tc, const std::string& path, const LandscapeMigrateParams& params,
                                          const LandscapeBuildOptions& options)
{
	LandscapeBuildResult bad;
	if (!(tc.sizeX > 0.0f) || !(tc.sizeZ > 0.0f)) { bad.error = "The landscape has no area."; return bad; }
	const LandscapeGeometry g = params.geometry;
	if (!g.valid()) { bad.error = "The tile geometry is not valid."; return bad; }

	const std::vector<float> field = computeTerrainHeightField(tc);
	const int res = static_cast<int>(std::lround(std::sqrt(static_cast<double>(field.size()))));
	if (res < 2 || static_cast<size_t>(res) * res != field.size()) { bad.error = "The landscape has no height field."; return bad; }

	float lo = field[0], hi = field[0];
	for (float v : field) { lo = std::min(lo, v); hi = std::max(hi, v); }
	float minM = params.heightMinM, scaleM = params.heightScaleM;
	if (scaleM <= 0.0f)
	{
		minM = std::floor(lo);
		scaleM = std::max(1.0f, std::ceil(hi - minM));
	}

	// Weights: the three pages are 4 channels each; a missing page is zero, no paint at all is layer 0.
	const int wres = static_cast<int>(tc.weightRes);
	const size_t pageBytes = static_cast<size_t>(wres) * wres * 4;
	const std::vector<uint8_t>* pages[3] = { &tc.layerWeights, &tc.layerWeights2, &tc.layerWeights3 };
	bool painted = false;
	for (int p = 0; p < 3; ++p)
		if (pages[p]->size() == pageBytes && wres > 0) painted = true;

	const int tilesX = std::max(1, static_cast<int>(std::ceil(static_cast<double>(tc.sizeX) / g.tileM - 1e-6)));
	const int tilesZ = std::max(1, static_cast<int>(std::ceil(static_cast<double>(tc.sizeZ) / g.tileM - 1e-6)));
	const double yOff = static_cast<double>(params.yOffsetM);

	const auto make = [&](LandscapeTileKey key) {
		LandscapeTile t;
		t.geometry = g;
		const int n = g.heightDim(0), W = g.wTexels, C = g.channels;
		const double rasterH = static_cast<double>(g.tileM) / g.hCells, rasterW = static_cast<double>(g.tileM) / g.wTexels;
		const double x0 = key.x * static_cast<double>(g.tileM) - params.originX, z0 = key.z * static_cast<double>(g.tileM) - params.originZ;
		std::vector<uint16_t> h0(static_cast<size_t>(n) * n);
		for (int z = 0; z < n; ++z)
			for (int x = 0; x < n; ++x)
			{
				const double u = (x0 + x * rasterH) / tc.sizeX * (res - 1), v = (z0 + z * rasterH) / tc.sizeZ * (res - 1);
				h0[static_cast<size_t>(z) * n + x] = quantize(bilinear(field, res, res, u, v) + yOff, minM, scaleM);
			}
		t.heights.push_back(std::move(h0));
		std::vector<uint8_t> w0(static_cast<size_t>(W) * W * C, 0);
		for (int z = 0; z < W; ++z)
			for (int x = 0; x < W; ++x)
			{
				uint8_t* out = &w0[(static_cast<size_t>(z) * W + x) * C];
				if (!painted) { out[0] = 255; continue; }
				// A terrain texel t covers [t, t+1) / wres of the size, so its centre is at t + 0.5; a tile texel's centre is the same kind of point.
				const double u = (x0 + (x + 0.5) * rasterW) / tc.sizeX * wres - 0.5, v = (z0 + (z + 0.5) * rasterW) / tc.sizeZ * wres - 0.5;
				double s[16] = {};
				for (int k = 0; k < std::min(C, 12); ++k)
				{
					const std::vector<uint8_t>& page = *pages[k / 4];
					if (page.size() != pageBytes) continue;
					const double cx = std::clamp(u, 0.0, static_cast<double>(wres - 1)), cz = std::clamp(v, 0.0, static_cast<double>(wres - 1));
					const int ix = static_cast<int>(std::floor(cx)), iz = static_cast<int>(std::floor(cz));
					const int ix1 = std::min(ix + 1, wres - 1), iz1 = std::min(iz + 1, wres - 1);
					const double fx = cx - ix, fz = cz - iz;
					const auto at = [&](int xx, int zz) { return static_cast<double>(page[(static_cast<size_t>(zz) * wres + xx) * 4 + static_cast<size_t>(k % 4)]); };
					s[k] = (at(ix, iz) * (1 - fx) + at(ix1, iz) * fx) * (1 - fz) + (at(ix, iz1) * (1 - fx) + at(ix1, iz1) * fx) * fz;
				}
				normaliseTo255(s, C, out);
			}
		t.weights.push_back(std::move(w0));
		t.buildMips();
		return t;
	};
	return buildPack(path, g, tilesX, tilesZ, minM, scaleM, options, make);
}

// ── Height map import ────────────────────────────────────────────────────────
LandscapeHeightmapFiles singleHeightmapFile(const std::string& path)
{
	LandscapeHeightmapFiles f;
	f.files = { path };
	f.filesX = f.filesZ = 1;
	return f;
}

LandscapeHeightmapFiles findHeightmapTileFiles(const std::string& dir)
{
	LandscapeHeightmapFiles f;
	std::error_code ec;
	if (!fs::is_directory(dir, ec)) { f.error = "\"" + dir + "\" is not a folder."; return f; }
	std::map<std::string, std::map<std::pair<int, int>, std::string>> sets;   // name -> (x, y) -> file
	for (const auto& e : fs::directory_iterator(dir, ec))
	{
		if (!e.is_regular_file()) continue;
		const std::string ext = lowerExt(e.path().string());
		if (ext != "png" && ext != "r16" && ext != "raw" && ext != "pgm") continue;
		const std::string stem = e.path().stem().string();
		// name_x<N>_y<M>: the last two underscore-separated tokens.
		const size_t ay = stem.rfind("_y");
		if (ay == std::string::npos) continue;
		const size_t ax = stem.rfind("_x", ay);
		if (ax == std::string::npos) continue;
		const std::string xs = stem.substr(ax + 2, ay - ax - 2), ys = stem.substr(ay + 2);
		if (xs.empty() || ys.empty() || xs.size() > 6 || ys.size() > 6 || !std::all_of(xs.begin(), xs.end(), ::isdigit) || !std::all_of(ys.begin(), ys.end(), ::isdigit)) continue;
		sets[stem.substr(0, ax)][{ std::stoi(xs), std::stoi(ys) }] = e.path().string();
	}
	// One tile set per import: the name with the most files (the first in the alphabet on a tie), so a stray file of another set cannot win.
	std::map<std::pair<int, int>, std::string> found;
	for (const auto& kv : sets)
		if (kv.second.size() > found.size()) found = kv.second;
	if (found.empty()) { f.error = "No files named <name>_x<N>_y<M>.png|r16|raw|pgm in \"" + dir + "\"."; return f; }
	int maxX = 0, maxY = 0;
	for (const auto& kv : found) { maxX = std::max(maxX, kv.first.first); maxY = std::max(maxY, kv.first.second); }
	f.filesX = maxX + 1;
	f.filesZ = maxY + 1;
	f.files.assign(static_cast<size_t>(f.filesX) * f.filesZ, std::string());
	for (const auto& kv : found) f.files[static_cast<size_t>(kv.first.second) * f.filesX + kv.first.first] = kv.second;
	return f;
}

namespace
{
// The files of a grid, read when a sample needs them and kept while they are near. Thread safe: tiles are made on several threads.
class HeightmapGrid
{
public:
	HeightmapGrid(const LandscapeHeightmapFiles& f, bool flipY) : m_f(f), m_flipY(flipY) {}

	// Reads the first usable file to learn the picture size; false with an error when none can be read or the sizes differ.
	bool prepare(std::string& error)
	{
		for (size_t i = 0; i < m_f.files.size(); ++i)
		{
			if (m_f.files[i].empty()) continue;
			const auto img = load(static_cast<int>(i % static_cast<size_t>(m_f.filesX)), static_cast<int>(i / static_cast<size_t>(m_f.filesX)), error);
			if (!img) return false;
			m_w = static_cast<int>(img->w);
			m_h = static_cast<int>(img->h);
			return true;
		}
		error = "The tile set has no files.";
		return false;
	}
	int width() const { return m_w * m_f.filesX; }
	int height() const { return m_h * m_f.filesZ; }
	bool failed() const { return m_failed; }
	std::string failure() const { std::lock_guard<std::mutex> lk(m_mutex); return m_failure; }

	// Grid pixel (gx, gz), clamped; a hole reads 0.
	uint16_t pixel(int gx, int gz)
	{
		gx = std::clamp(gx, 0, width() - 1);
		gz = std::clamp(gz, 0, height() - 1);
		int fx = gx / m_w, fz = gz / m_h;
		const int px = gx % m_w, pz = gz % m_h;
		if (m_flipY) fz = m_f.filesZ - 1 - fz;
		std::string err;
		const auto img = load(fx, fz, err);
		if (!img) return 0;
		return img->v[static_cast<size_t>(pz) * img->w + px];
	}
	double sample(double x, double z)
	{
		x = std::clamp(x, 0.0, static_cast<double>(width() - 1));
		z = std::clamp(z, 0.0, static_cast<double>(height() - 1));
		const int x0 = static_cast<int>(std::floor(x)), z0 = static_cast<int>(std::floor(z));
		const double fx = x - x0, fz = z - z0;
		return (pixel(x0, z0) * (1 - fx) + pixel(x0 + 1, z0) * fx) * (1 - fz) + (pixel(x0, z0 + 1) * (1 - fx) + pixel(x0 + 1, z0 + 1) * fx) * fz;
	}

private:
	struct Image { std::vector<uint16_t> v; uint32_t w = 0, h = 0; };
	std::shared_ptr<const Image> load(int fx, int fz, std::string& error)
	{
		if (fx < 0 || fz < 0 || fx >= m_f.filesX || fz >= m_f.filesZ) return nullptr;
		const std::string& path = m_f.files[static_cast<size_t>(fz) * m_f.filesX + fx];
		if (path.empty()) return nullptr;
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			const auto it = m_cache.find(path);
			if (it != m_cache.end()) { it->second.second = ++m_tick; return it->second.first; }
		}
		auto img = std::make_shared<Image>();   // decoded outside the lock: other threads read their own files meanwhile
		std::string why;
		if (!TerrainHeightmap::readGrey16(path, img->v, img->w, img->h, why))
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_failed = true;
			if (m_failure.empty()) m_failure = why;
			error = why;
			return nullptr;
		}
		if (m_w > 0 && (static_cast<int>(img->w) != m_w || static_cast<int>(img->h) != m_h))
		{
			std::lock_guard<std::mutex> lk(m_mutex);
			m_failed = true;
			if (m_failure.empty()) m_failure = "\"" + path + "\" is " + std::to_string(img->w) + " x " + std::to_string(img->h) + " pixels, the others " + std::to_string(m_w) + " x " + std::to_string(m_h) + ".";
			error = m_failure;
			return nullptr;
		}
		std::lock_guard<std::mutex> lk(m_mutex);
		m_cache[path] = { img, ++m_tick };
		while (m_cache.size() > 12)   // least recently used goes: a row of tiles touches a few files at a time
		{
			auto victim = m_cache.begin();
			for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
				if (it->second.second < victim->second.second) victim = it;
			m_cache.erase(victim);
		}
		return img;
	}

	const LandscapeHeightmapFiles& m_f;
	bool   m_flipY;
	int    m_w = 0, m_h = 0;
	mutable std::mutex m_mutex;
	std::map<std::string, std::pair<std::shared_ptr<const Image>, uint64_t>> m_cache;
	uint64_t m_tick = 0;
	bool   m_failed = false;
	std::string m_failure;
};
} // namespace

LandscapeBuildResult importHeightmapToPack(const LandscapeHeightmapFiles& files, const std::string& path, const LandscapeHeightmapImportParams& params,
                                           const LandscapeBuildOptions& options)
{
	LandscapeBuildResult bad;
	if (!files.error.empty()) { bad.error = files.error; return bad; }
	if (files.filesX <= 0 || files.filesZ <= 0 || files.files.size() != static_cast<size_t>(files.filesX) * files.filesZ) { bad.error = "The tile set is empty."; return bad; }
	if (!(params.metersPerPixel > 0.0f)) { bad.error = "Metres per pixel must be positive."; return bad; }
	if (!(params.heightScaleM > 0.0f)) { bad.error = "The height range must be positive."; return bad; }
	const LandscapeGeometry g = params.geometry;
	if (!g.valid()) { bad.error = "The tile geometry is not valid."; return bad; }

	HeightmapGrid grid(files, params.flipY);
	std::string err;
	if (!grid.prepare(err)) { bad.error = err; return bad; }

	const double mpp = params.metersPerPixel;
	const double spanX = (grid.width() - 1) * mpp, spanZ = (grid.height() - 1) * mpp;
	const int tilesX = params.tilesX > 0 ? params.tilesX : std::max(1, static_cast<int>(std::ceil(spanX / g.tileM - 1e-6)));
	const int tilesZ = params.tilesZ > 0 ? params.tilesZ : std::max(1, static_cast<int>(std::ceil(spanZ / g.tileM - 1e-6)));

	const auto make = [&](LandscapeTileKey key) {
		LandscapeTile t;
		t.geometry = g;
		const int n = g.heightDim(0), W = g.wTexels, C = g.channels;
		const double rasterH = static_cast<double>(g.tileM) / g.hCells;
		std::vector<uint16_t> h0(static_cast<size_t>(n) * n);
		for (int z = 0; z < n; ++z)
			for (int x = 0; x < n; ++x)
			{
				const double wx = key.x * static_cast<double>(g.tileM) + x * rasterH, wz = key.z * static_cast<double>(g.tileM) + z * rasterH;
				h0[static_cast<size_t>(z) * n + x] = static_cast<uint16_t>(std::clamp(std::llround(grid.sample(wx / mpp, wz / mpp)), 0ll, 65535ll));
			}
		t.heights.push_back(std::move(h0));
		if (params.autoWeights)
			autoWeightsFor(t, params.heightMinM, params.heightScaleM);
		else
		{
			std::vector<uint8_t> w0(static_cast<size_t>(W) * W * C, 0);
			for (size_t i = 0; i < static_cast<size_t>(W) * W; ++i) w0[i * C] = 255;
			t.weights.push_back(std::move(w0));
		}
		t.buildMips();
		return t;
	};
	LandscapeBuildResult r = buildPack(path, g, tilesX, tilesZ, params.heightMinM, params.heightScaleM, options, make);
	if (r.ok && grid.failed())
	{
		// A file that would not read in the middle of the build is an error, not a hole in the ground.
		std::error_code ec;
		fs::remove(path, ec);
		r.ok = false;
		r.error = grid.failure();
	}
	return r;
}

// ── Job ──────────────────────────────────────────────────────────────────────
LandscapeBuildJob::~LandscapeBuildJob()
{
	m_cancel = true;
	if (m_thread.joinable()) m_thread.join();
}

bool LandscapeBuildJob::start(std::string label, Work work)
{
	if (m_running) return false;
	if (m_thread.joinable()) m_thread.join();
	m_label = std::move(label);
	m_cancel = false;
	m_finished = false;
	m_done = 0;
	m_total = 0;
	m_result = {};
	m_running = true;
	m_thread = std::thread([this, work = std::move(work)]() {
		LandscapeBuildOptions opt;
		opt.progress = [this](size_t done, size_t total) {
			m_done = done;
			m_total = total;
			return !m_cancel.load();
		};
		LandscapeBuildResult r = work(opt);
		m_result = std::move(r);
		m_finished = true;
		m_running = false;
	});
	return true;
}

bool LandscapeBuildJob::poll(LandscapeBuildResult& out)
{
	if (!m_finished.exchange(false)) return false;
	if (m_thread.joinable()) m_thread.join();
	out = std::move(m_result);
	return true;
}

} // namespace HE
