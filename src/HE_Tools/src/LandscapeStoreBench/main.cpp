// landscape_store_bench: measurement base for the world-scale landscape (docs/landscape-world-scale-plan-2026-10-28.md,
// docs/landscape-world-scale-measurements-2026-10-31.md). Pure data store, no rendering: a procedural 100 km world is cut
// into tiles, every tile is encoded, written, read back and decoded, and each stage is timed.
//
// Since step 1 (Thema 326) it measures the ENGINE's code: tiles come from ProceduralLandscapeSource, the container is
// HE::encodeLandscapeTile / decodeLandscapeTile (LandscapeTile.h), the pack is LandscapePackWriter's. Only the disk
// probes and the cold-cache reads are the tool's own.
//
//   landscape_store_bench [--dir D] [--tiles-x N] [--tiles-z N] [--full] [--tile-m 512] [--raster-h 2] [--raster-w 4]
//                         [--weight-pages 3] [--rough LSB] [--threads N] [--sample N] [--disk-mb N]
//                         [--zstd-level 3] [--primary store|lz4|lz4+pred|zstd|zstd+pred] [--keep]
//
// Everything is std C++ plus POSIX pread where there is one; the cold-cache reads need fcntl/posix_fadvise, so on
// Windows they fall back to warm reads and the output says so.
#include <HorizonScene/LandscapeSource.h>
#include <HorizonScene/LandscapeTile.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using Clock  = std::chrono::steady_clock;
using namespace HE;

namespace
{
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

struct Config
{
	std::string dir         = "landscape_bench_out";
	int         tilesX      = 8;
	int         tilesZ      = 0; // 0 = whole world length
	float       tileM       = 512.0f;
	float       worldM      = 100000.0f;
	float       rasterH     = 2.0f;
	float       rasterW     = 4.0f;
	int         pages       = 3; // kTerrainWeightPages on release/0.8.0 (the plan counted 2)
	int         rough       = 0; // +-LSB white noise on the heights (real DEMs are not noise-free)
	unsigned    threads     = 0;
	int         sample      = 48;
	int         diskMb      = 2048;
	bool        keep        = false;
	std::string primary;
};

struct Named
{
	const char*    name;
	LandscapeCodec codec;
};

LandscapeCodec codecOf(LandscapeCodec::Backend b, bool predictor, int zstdLevel)
{
	LandscapeCodec c;
	c.backend = b;
	c.predictor = predictor;
	c.zstdLevel = zstdLevel;
	return c;
}

// Stored bytes of the height streams and of the weight streams of one container (walks the stream table that
// LandscapeTile.h documents; a tool, not the reader).
void splitStreams(const std::vector<uint8_t>& blob, int hLevels, int wLevels, size_t& heights, size_t& weights)
{
	heights = weights = 0;
	size_t pos = kLandscapeTileHeaderBytes;
	for (int i = 0; i < hLevels + wLevels && pos + 8 <= blob.size(); ++i)
	{
		const uint32_t stored = blob[pos + 4] | (blob[pos + 5] << 8) | (blob[pos + 6] << 16) | (uint32_t(blob[pos + 7]) << 24);
		(i < hLevels ? heights : weights) += 8 + stored;
		pos += 8 + stored;
	}
}

struct Stat
{
	std::vector<double> v;
	void   add(double x) { v.push_back(x); }
	double mean() const { return v.empty() ? 0 : std::accumulate(v.begin(), v.end(), 0.0) / v.size(); }
	double pct(double p) const
	{
		if (v.empty()) return 0;
		std::vector<double> s = v;
		std::sort(s.begin(), s.end());
		return s[std::min(s.size() - 1, static_cast<size_t>(p * s.size()))];
	}
};

// ── file io ───────────────────────────────────────────────────────────────────
class Reader
{
public:
	Reader(const std::string& path, bool cold) : m_cold(cold)
	{
#ifndef _WIN32
		m_fd = ::open(path.c_str(), O_RDONLY);
	#ifdef __APPLE__
		if (m_fd >= 0 && cold) ::fcntl(m_fd, F_NOCACHE, 1);
	#elif defined(POSIX_FADV_DONTNEED)
		if (m_fd >= 0 && cold) ::posix_fadvise(m_fd, 0, 0, POSIX_FADV_DONTNEED);
	#endif
#else
		m_in.open(path, std::ios::binary);
#endif
	}
	~Reader()
	{
#ifndef _WIN32
		if (m_fd >= 0) ::close(m_fd);
#endif
	}
	bool read(uint64_t off, uint8_t* dst, size_t n)
	{
#ifndef _WIN32
	#if defined(POSIX_FADV_DONTNEED) && !defined(__APPLE__)
		if (m_cold) ::posix_fadvise(m_fd, static_cast<off_t>(off), static_cast<off_t>(n), POSIX_FADV_DONTNEED);
	#endif
		return m_fd >= 0 && ::pread(m_fd, dst, n, static_cast<off_t>(off)) == static_cast<ssize_t>(n);
#else
		m_in.clear(); m_in.seekg(static_cast<std::streamoff>(off));
		return static_cast<bool>(m_in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n)));
#endif
	}
private:
	bool m_cold;
#ifndef _WIN32
	int m_fd = -1;
#else
	std::ifstream m_in;
#endif
};

// Writes without leaving the data in the page cache (so a later "cold" read really goes to the disk): macOS F_NOCACHE,
// Linux fsync + POSIX_FADV_DONTNEED at close. Elsewhere an ordinary write.
class Writer
{
public:
	explicit Writer(const std::string& path, bool bypass)
	{
#ifndef _WIN32
		m_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	#ifdef __APPLE__
		if (m_fd >= 0 && bypass) ::fcntl(m_fd, F_NOCACHE, 1);
	#endif
		m_bypass = bypass;
#else
		(void)bypass;
		m_out.open(path, std::ios::binary);
#endif
	}
	~Writer()
	{
#ifndef _WIN32
		if (m_fd >= 0)
		{
	#if defined(POSIX_FADV_DONTNEED) && !defined(__APPLE__)
			if (m_bypass) { ::fsync(m_fd); ::posix_fadvise(m_fd, 0, 0, POSIX_FADV_DONTNEED); }
	#endif
			::close(m_fd);
		}
#endif
	}
	void write(const uint8_t* p, size_t n)
	{
#ifndef _WIN32
		while (n && m_fd >= 0)
		{
			const ssize_t w = ::write(m_fd, p, n);
			if (w <= 0) return;
			p += w; n -= static_cast<size_t>(w);
		}
#else
		m_out.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
#endif
	}
private:
#ifndef _WIN32
	int  m_fd     = -1;
	bool m_bypass = false;
#else
	std::ofstream m_out;
#endif
};
constexpr bool kColdSupported =
#ifdef _WIN32
	false;
#else
	true;
#endif

void parallelFor(unsigned threads, size_t count, const std::function<void(size_t, unsigned)>& fn)
{
	std::atomic<size_t> next{0};
	std::vector<std::thread> ts;
	for (unsigned t = 0; t < threads; ++t)
		ts.emplace_back([&, t] { for (size_t i; (i = next.fetch_add(1)) < count;) fn(i, t); });
	for (auto& t : ts) t.join();
}
} // namespace

int main(int argc, char** argv)
{
	Config c;
	int zstdLevel = 3;
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		const auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
		if (a == "--dir") c.dir = val();
		else if (a == "--tiles-x") c.tilesX = std::atoi(val().c_str());
		else if (a == "--tiles-z") c.tilesZ = std::atoi(val().c_str());
		else if (a == "--full") { c.tilesX = c.tilesZ = -1; }
		else if (a == "--tile-m") c.tileM = static_cast<float>(std::atof(val().c_str()));
		else if (a == "--raster-h") c.rasterH = static_cast<float>(std::atof(val().c_str()));
		else if (a == "--raster-w") c.rasterW = static_cast<float>(std::atof(val().c_str()));
		else if (a == "--weight-pages") c.pages = std::atoi(val().c_str());
		else if (a == "--rough") c.rough = std::atoi(val().c_str());
		else if (a == "--threads") c.threads = static_cast<unsigned>(std::atoi(val().c_str()));
		else if (a == "--sample") c.sample = std::atoi(val().c_str());
		else if (a == "--disk-mb") c.diskMb = std::atoi(val().c_str());
		else if (a == "--zstd-level") zstdLevel = std::atoi(val().c_str());
		else if (a == "--primary") c.primary = val();
		else if (a == "--keep") c.keep = true;
		else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
	}
	if (c.threads == 0) c.threads = std::max(1u, std::thread::hardware_concurrency());
	const int worldTiles = static_cast<int>(std::ceil(c.worldM / c.tileM));
	if (c.tilesX < 0) c.tilesX = worldTiles;
	if (c.tilesZ <= 0) c.tilesZ = worldTiles;
	c.tilesX = std::min(c.tilesX, worldTiles);
	c.tilesZ = std::min(c.tilesZ, worldTiles);

	ProceduralLandscapeParams params;
	params.geometry.tileM    = c.tileM;
	params.geometry.hCells   = static_cast<uint16_t>(std::lround(c.tileM / c.rasterH));
	params.geometry.wTexels  = static_cast<uint16_t>(std::lround(c.tileM / c.rasterW));
	params.geometry.channels = static_cast<uint8_t>(4 * c.pages);
	params.tilesX = params.tilesZ = worldTiles;
	params.roughLsb = c.rough;
	if (!params.geometry.valid()) { std::fprintf(stderr, "tile-m / raster / weight-pages give no valid tile geometry\n"); return 2; }
	const ProceduralLandscapeSource source(params);
	const LandscapeGeometry& g = params.geometry;
	const size_t nTiles = static_cast<size_t>(c.tilesX) * c.tilesZ;
	const size_t rawAll = g.decodedBytes(), rawL0 = g.decodedBytes(1);
	const auto keyOf = [&](size_t id) { return LandscapeTileKey{ static_cast<int>(id % c.tilesX), static_cast<int>(id / c.tilesX) }; };

	std::vector<Named> codecs = { { "store", codecOf(LandscapeCodec::Backend::Store, false, zstdLevel) } };
	if (LandscapeCodec::available(LandscapeCodec::Backend::Lz4))
	{
		codecs.push_back({ "lz4", codecOf(LandscapeCodec::Backend::Lz4, false, zstdLevel) });
		codecs.push_back({ "lz4+pred", codecOf(LandscapeCodec::Backend::Lz4, true, zstdLevel) });
	}
	if (LandscapeCodec::available(LandscapeCodec::Backend::Zstd))
	{
		codecs.push_back({ "zstd", codecOf(LandscapeCodec::Backend::Zstd, false, zstdLevel) });
		codecs.push_back({ "zstd+pred", codecOf(LandscapeCodec::Backend::Zstd, true, zstdLevel) });
	}
	codecs.push_back({ "store+pred", codecOf(LandscapeCodec::Backend::Store, true, zstdLevel) });
	Named primary = codecs.back();
	const char* wantPrimary = c.primary.empty() ? (LandscapeCodec::available(LandscapeCodec::Backend::Zstd) ? "zstd+pred" : "lz4+pred") : c.primary.c_str();
	for (const Named& cd : codecs) if (std::strcmp(cd.name, wantPrimary) == 0) primary = cd;

	std::printf("# landscape_store_bench\n\n");
	std::printf("- tile %.0f m, heights %.1f m raster (%dx%d, %d mip levels), weights %.1f m raster (%dx%d x %d ch = %d pages, %d levels)\n",
	            c.tileM, c.rasterH, g.heightDim(0), g.heightDim(0), g.heightLevels(), c.rasterW, g.weightDim(0), g.weightDim(0), g.channels, c.pages, g.weightLevels());
	std::printf("- sampling: %d x %d tiles of the %d x %d world (%.1f km x %.1f km of %.1f km), height noise +-%d LSB, %u threads, primary codec %s, zstd level %d\n",
	            c.tilesX, c.tilesZ, worldTiles, worldTiles, c.tilesX * c.tileM / 1000.0, c.tilesZ * c.tileM / 1000.0, worldTiles * c.tileM / 1000.0, c.rough, c.threads,
	            primary.name, zstdLevel);
	std::printf("- raw size per tile (whole pyramid): %.1f KB, level 0 only: %.1f KB\n\n", rawAll / 1024.0, rawL0 / 1024.0);

	std::error_code ec;
	fs::create_directories(c.dir, ec);
	std::vector<std::string> created;
	double primaryDecodeMs = 0; // single-thread, whole pyramid, primary codec (section A)

	// ── A. single-thread latency per codec over a spread sample ──────────────
	{
		const int ns = static_cast<int>(std::min<size_t>(c.sample, nTiles));
		Stat gen;
		std::vector<LandscapeTile> tiles;
		for (int i = 0; i < ns; ++i)
		{
			const auto t0 = Clock::now();
			tiles.push_back(source.generate(keyOf(nTiles * i / ns)));
			gen.add(msSince(t0));
		}
		std::printf("## A. Single thread, %d tiles spread over the sample\n\n", ns);
		std::printf("Procedural generation (fBm heights + weights + both pyramids): mean %.2f ms/tile, p99 %.2f ms\n\n", gen.mean(), gen.pct(0.99));
		std::printf("| codec | KB/tile | of which heights | weights | ratio | encode ms | decode ms (all levels) | decode p99 | decode ms (L0 only) | roundtrip |\n|---|---:|---:|---:|---:|---:|---:|---:|---:|---|\n");
		for (const Named& cd : codecs)
		{
			Stat enc, dec, dec0, size, hsize;
			bool ok = true;
			for (const LandscapeTile& t : tiles)
			{
				auto t0 = Clock::now();
				const auto blob = encodeLandscapeTile(t, cd.codec);
				enc.add(msSince(t0));
				size.add(static_cast<double>(blob.size()));
				size_t hb = 0, wb = 0;
				splitStreams(blob, g.heightLevels(), g.weightLevels(), hb, wb);
				hsize.add(static_cast<double>(hb));
				LandscapeTile d, d0;
				t0 = Clock::now();
				ok = decodeLandscapeTile(blob.data(), blob.size(), d) && ok;
				dec.add(msSince(t0));
				t0 = Clock::now();
				ok = decodeLandscapeTile(blob.data(), blob.size(), d0, 1) && ok;
				dec0.add(msSince(t0));
				ok = ok && d == t && !d0.heights.empty() && !d0.weights.empty() && d0.heights[0] == t.heights[0] && d0.weights[0] == t.weights[0];
			}
			if (std::strcmp(cd.name, primary.name) == 0) primaryDecodeMs = dec.mean();
			std::printf("| %s | %.1f | %.1f | %.1f | %.2f | %.2f | %.3f | %.3f | %.3f | %s |\n", cd.name, size.mean() / 1024.0, hsize.mean() / 1024.0,
			            (size.mean() - hsize.mean()) / 1024.0, rawAll / size.mean(), enc.mean(), dec.mean(), dec.pct(0.99), dec0.mean(), ok ? "ok" : "MISMATCH");
		}
		std::printf("\n");
	}

	// ── B. generate + encode + write the whole sample (parallel) ─────────────
	const std::string packPath = c.dir + "/world.pack";
	const std::string coldPath = c.dir + "/world.cold.pack";
	std::vector<std::string> tilePath(nTiles);
	std::vector<uint64_t> packOff(nTiles, 0);
	std::vector<uint32_t> packSize(nTiles, 0);
	double totalComp = 0;
	{
		std::vector<std::vector<uint8_t>> blobs(nTiles);
		std::vector<double> genMs(c.threads, 0);
		const auto t0 = Clock::now();
		parallelFor(c.threads, nTiles, [&](size_t id, unsigned th) {
			const auto s = Clock::now();
			blobs[id] = encodeLandscapeTile(source.generate(keyOf(id)), primary.codec);
			genMs[th] += msSince(s);
		});
		const double wall = msSince(t0);
		for (const auto& b : blobs) totalComp += static_cast<double>(b.size());

		// The pack is the engine's (LandscapePackWriter, index and all), written for the sample's own extent.
		const auto w0 = Clock::now();
		created.push_back(packPath);
		{
			LandscapePackWriter pack;
			if (!pack.begin(packPath, g, c.tilesX, c.tilesZ)) { std::fprintf(stderr, "cannot write %s\n", packPath.c_str()); return 1; }
			for (size_t i = 0; i < nTiles; ++i) pack.add(keyOf(i), blobs[i]);
			pack.finish();
		}
		const double packMs = msSince(w0);
		// Cold reads need a file the page cache does not hold: copy the pack around the cache and read the copy.
		created.push_back(coldPath);
		{
			Reader in(packPath, false);
			Writer out(coldPath, true);
			std::vector<uint8_t> buf(1 << 20);
			const uint64_t size = fs::file_size(packPath);
			for (uint64_t off = 0; off < size; off += buf.size())
			{
				const size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), size - off));
				in.read(off, buf.data(), n);
				out.write(buf.data(), n);
			}
		}
		fs::remove(packPath, ec);
		const auto pack = LandscapePackSource::open(coldPath);
		if (!pack) { std::fprintf(stderr, "cannot reopen the pack\n"); return 1; }
		for (size_t i = 0; i < nTiles; ++i) { packSize[i] = pack->storedSize(keyOf(i)); packOff[i] = pack->storedOffset(keyOf(i)); }

		const auto f0 = Clock::now();
		for (size_t i = 0; i < nTiles; ++i)
		{
			tilePath[i] = c.dir + "/t_" + std::to_string(i % c.tilesX) + "_" + std::to_string(i / c.tilesX) + ".hlt";
			Writer f(tilePath[i], true);
			created.push_back(tilePath[i]);
			f.write(blobs[i].data(), blobs[i].size());
		}
		const double fileMs = msSince(f0);
		std::printf("## B. Build the sample (%zu tiles, %u threads)\n\n", nTiles, c.threads);
		std::printf("- generate + encode (%s): wall %.1f s = %.1f tiles/s, %.1f ms per tile and thread\n", primary.name, wall / 1000.0, nTiles * 1000.0 / wall,
		            std::accumulate(genMs.begin(), genMs.end(), 0.0) / nTiles);
		std::printf("- on disk: %.1f MB total, %.1f KB/tile (ratio %.2f vs %.1f KB raw)\n", totalComp / 1048576.0, totalComp / nTiles / 1024.0,
		            rawAll * nTiles / totalComp, rawAll / 1024.0);
		std::printf("- write: one pack file (engine writer, with index; page cache, not a disk number) %.0f MB/s (%.2f s), %zu tile files (cache bypassed) %.0f MB/s (%.2f s)\n\n",
		            totalComp / 1048576.0 / (packMs / 1000.0), packMs / 1000.0, nTiles, totalComp / 1048576.0 / (fileMs / 1000.0), fileMs / 1000.0);
	}

	// ── C. read back + decode (random order, warm and cold, pack and file-per-tile) ──
	{
		std::vector<size_t> order(nTiles);
		std::iota(order.begin(), order.end(), 0);
		std::shuffle(order.begin(), order.end(), std::mt19937(7));
		std::printf("## C. Random-order read + decode of every tile (%s)\n\n", kColdSupported ? "cold = page cache bypassed, warm = after an untimed pass" : "warm only on this platform");
		std::printf("| source | cache | threads | tiles/s | MB/s (compressed) | read ms/tile | decode ms/tile | decode L0-only ms/tile |\n|---|---|---:|---:|---:|---:|---:|---:|\n");
		for (const char* sourceName : {"pack", "files"})
			for (int cold = kColdSupported ? 1 : 0; cold >= 0; --cold)
				for (unsigned th : {1u, 4u, c.threads})
				{
					if (th > c.threads) continue;
					const bool isPack = std::strcmp(sourceName, "pack") == 0;
					if (!cold) // untimed pass: the page cache holds the data
						for (size_t id = 0; id < nTiles; ++id)
						{
							std::vector<uint8_t> b(packSize[id]);
							Reader r(isPack ? coldPath : tilePath[id], false);
							r.read(isPack ? packOff[id] : 0, b.data(), b.size());
						}
					std::vector<double> readMs(th, 0), decMs(th, 0), dec0Ms(th, 0);
					std::atomic<double> bytes{0};
					const auto t0 = Clock::now();
					parallelFor(th, nTiles, [&](size_t k, unsigned t) {
						const size_t id = order[k];
						std::vector<uint8_t> buf(packSize[id]);
						auto s = Clock::now();
						Reader r(isPack ? coldPath : tilePath[id], cold != 0);
						r.read(isPack ? packOff[id] : 0, buf.data(), buf.size());
						readMs[t] += msSince(s);
						LandscapeTile tile;
						s = Clock::now();
						decodeLandscapeTile(buf.data(), buf.size(), tile);
						decMs[t] += msSince(s);
						s = Clock::now();
						decodeLandscapeTile(buf.data(), buf.size(), tile, 1);
						dec0Ms[t] += msSince(s);
						double b = bytes.load();
						while (!bytes.compare_exchange_weak(b, b + static_cast<double>(buf.size()))) {}
					});
					const double wall = msSince(t0) / 1000.0;
					std::printf("| %s | %s | %u | %.0f | %.0f | %.3f | %.3f | %.3f |\n", sourceName, cold ? "cold" : "warm", th, nTiles / wall, bytes.load() / 1048576.0 / wall,
					            std::accumulate(readMs.begin(), readMs.end(), 0.0) / nTiles, std::accumulate(decMs.begin(), decMs.end(), 0.0) / nTiles,
					            std::accumulate(dec0Ms.begin(), dec0Ms.end(), 0.0) / nTiles);
				}
		std::printf("\n");
	}

	// ── D. raw disk, same directory ───────────────────────────────────────────
	{
		const std::string path = c.dir + "/disk_probe.bin";
		const size_t block = 1 << 20, blocks = static_cast<size_t>(c.diskMb);
		std::vector<uint8_t> buf(block);
		std::mt19937 rng(1);
		for (auto& b : buf) b = static_cast<uint8_t>(rng());
		created.push_back(path);
		auto t0 = Clock::now();
		{
			Writer f(path, true);
			for (size_t i = 0; i < blocks; ++i) f.write(buf.data(), block);
		}
		const double wr = msSince(t0) / 1000.0;
		std::printf("## D. Raw disk throughput (%d MB probe file, %s)\n\n", c.diskMb, c.dir.c_str());
		std::printf("| test | MB/s |\n|---|---:|\n| sequential write 1 MiB blocks (no fsync: not a disk number) | %.0f |\n", c.diskMb / wr);
		for (int cold = kColdSupported ? 1 : 0; cold >= 0; --cold)
		{
			if (!cold)
			{
				Reader warm(path, false);
				for (size_t i = 0; i < blocks; ++i) warm.read(i * block, buf.data(), block);
			}
			Reader r(path, cold != 0);
			t0 = Clock::now();
			for (size_t i = 0; i < blocks; ++i) r.read(i * block, buf.data(), block);
			std::printf("| sequential read 1 MiB %s | %.0f |\n", cold ? "cold" : "warm", c.diskMb / (msSince(t0) / 1000.0));
			if (cold) // before any warm pass: a read that went through the cache would leave the probe resident
			{
				const size_t small = 128 * 1024, count = std::min<size_t>(4096, blocks * block / small);
				std::vector<size_t> idx(count);
				std::uniform_int_distribution<size_t> dist(0, blocks * block / small - 1);
				for (auto& i : idx) i = dist(rng);
				for (unsigned th : {1u, 4u, c.threads})
				{
					if (th > c.threads) continue;
					t0 = Clock::now();
					parallelFor(th, count, [&](size_t k, unsigned) {
						std::vector<uint8_t> b(small);
						Reader rr(path, true);
						rr.read(idx[k] * small, b.data(), small);
					});
					const double s = msSince(t0) / 1000.0;
					std::printf("| random read 128 KiB cold, %u thread(s) | %.0f (%.0f IOPS) |\n", th, count * small / 1048576.0 / s, count / s);
				}
			}
		}
		std::printf("\n");
	}

	// ── E. what the numbers mean for the full world ───────────────────────────
	{
		const double perTile  = totalComp / nTiles;
		const double allTiles = static_cast<double>(worldTiles) * worldTiles;
		std::printf("## E. Extrapolation to the %d x %d tile world (from the sample above)\n\n", worldTiles, worldTiles);
		std::printf("- tiles: %.0f; on disk with %s: %.2f GB; raw (all levels): %.2f GB; raw level 0 only: %.2f GB\n", allTiles, primary.name,
		            perTile * allTiles / 1e9, rawAll * allTiles / 1e9, rawL0 * allTiles / 1e9);
		std::printf("\n| radius | tiles | decoded in RAM / VRAM (all levels) | compressed | decode CPU for a full fill: 1 thread / all threads |\n|---|---:|---:|---:|---:|\n");
		for (double r : {2000.0, 4000.0, 8000.0, 12000.0})
		{
			const double tiles = 3.14159265 * r * r / (c.tileM * c.tileM);
			std::printf("| %.0f km | %.0f | %.0f MB | %.0f MB | %.2f s / %.2f s |\n", r / 1000.0, tiles, tiles * rawAll / 1048576.0, tiles * perTile / 1048576.0,
			            tiles * primaryDecodeMs / 1000.0, tiles * primaryDecodeMs / 1000.0 / c.threads);
		}
		std::printf("\nNew tiles per second while the camera moves (radius 8 km, a ring 2R wide enters): ");
		for (double v : {30.0, 100.0, 250.0}) std::printf("%.0f m/s -> %.1f tiles/s; ", v, 2 * 8000.0 * v / (c.tileM * c.tileM));
		std::printf("\n");
	}

	if (!c.keep)
	{
		for (const auto& p : created) fs::remove(p, ec);
		fs::remove(c.dir, ec); // only succeeds when empty: never removes anything this run did not create
	}
	return 0;
}
