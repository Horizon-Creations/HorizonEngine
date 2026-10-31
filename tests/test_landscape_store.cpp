// HE::LandscapeTile / LandscapeSource / LandscapeStore (Thema 326, step 1): the tile format round-trips
// bit-exact and refuses damaged input, a pack file serves what was written, and the store keeps the tiles
// around its anchors and nothing else: no tile twice, never over its byte budget, nothing loaded for a
// place the anchor has already left.
#include "doctest.h"
#include <HorizonScene/LandscapeSource.h>
#include <HorizonScene/LandscapeStore.h>
#include <HorizonScene/LandscapeTile.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <set>
#include <thread>

using namespace HE;

namespace
{
// Small tiles keep the tests quick: 512 m tiles with 33 x 33 heights and 16 x 16 weights of 4 channels.
LandscapeGeometry smallGeometry()
{
	LandscapeGeometry g;
	g.tileM = 512.0f;
	g.hCells = 32;
	g.wTexels = 16;
	g.channels = 4;
	return g;
}

ProceduralLandscapeParams smallWorld(int tiles = 196)
{
	ProceduralLandscapeParams p;
	p.geometry = smallGeometry();
	p.tilesX = p.tilesZ = tiles;
	return p;
}

std::filesystem::path tempFile(const char* name)
{
	return std::filesystem::temp_directory_path() / (std::string("he_landscape_") + name + "_" + std::to_string(std::random_device{}()) + ".bin");
}

bool settleStore(LandscapeStore& store, const std::vector<LandscapeAnchor>& anchors, int timeoutMs = 20000)
{
	const auto t0 = std::chrono::steady_clock::now();
	for (;;)
	{
		store.update(anchors, 5.0);
		if (store.settled()) return true;
		if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > timeoutMs) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

// Tiles within `radius` of (x, z) for a world of tiles x tiles, the way the store counts them.
size_t tilesWithin(double x, double z, double radius, int tiles, double tileM)
{
	size_t n = 0;
	for (int tz = 0; tz < tiles; ++tz)
		for (int tx = 0; tx < tiles; ++tx)
		{
			const double dx = std::max({ tx * tileM - x, 0.0, x - (tx + 1) * tileM });
			const double dz = std::max({ tz * tileM - z, 0.0, z - (tz + 1) * tileM });
			if (std::sqrt(dx * dx + dz * dz) <= radius) ++n;
		}
	return n;
}

// A source that holds every load until the gate opens, and can fail chosen tiles.
class GatedSource final : public LandscapeTileSource
{
public:
	explicit GatedSource(ProceduralLandscapeParams p) : m_inner(p) {}
	const LandscapeGeometry& geometry() const override { return m_inner.geometry(); }
	int  tilesX() const override { return m_inner.tilesX(); }
	int  tilesZ() const override { return m_inner.tilesZ(); }
	bool load(LandscapeTileKey key, int maxLevels, LandscapeTile& out) const override
	{
		++started;
		while (!open.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if (failingPairs.count({ key.x, key.z })) { ++failedLoads; return false; }
		return m_inner.load(key, maxLevels, out);
	}
	mutable std::atomic<int> started{ 0 };
	mutable std::atomic<int> failedLoads{ 0 };
	std::atomic<bool>        open{ false };
	std::set<std::pair<int, int>> failingPairs;

private:
	ProceduralLandscapeSource m_inner;
};
} // namespace

// ── geometry ─────────────────────────────────────────────────────────────────
TEST_CASE("landscape geometry: the 512 m / 2 m / 4 m default and its pyramids")
{
	LandscapeGeometry g;
	CHECK(g.valid());
	CHECK(g.heightLevels() == 7);   // 257 down to the 128 m raster: 257 129 65 33 17 9 5
	CHECK(g.weightLevels() == 6);   // 128 down to 4 texels of 128 m
	CHECK(g.heightDim(0) == 257);
	CHECK(g.heightDim(6) == 5);
	CHECK(g.heightBytes(1) == 257u * 257u * 2u);
	CHECK(g.weightBytes(1) == 128u * 128u * 12u);
	CHECK(g.decodedBytes() > g.decodedBytes(1));

	LandscapeGeometry bad = g;
	bad.hCells = 100;               // not a power of two
	CHECK_FALSE(bad.valid());
	bad = g; bad.channels = 0;
	CHECK_FALSE(bad.valid());
	bad = g; bad.tileM = -1.0f;
	CHECK_FALSE(bad.valid());
	bad = g; bad.hCells = 2;        // no level at the 128 m raster
	CHECK_FALSE(bad.valid());
}

// ── tile format ──────────────────────────────────────────────────────────────
TEST_CASE("landscape tile: every backend round-trips bit-exact, with and without the predictor")
{
	for (int rough : { 0, 3 })
	{
		ProceduralLandscapeParams p = smallWorld();
		p.roughLsb = rough;
		const ProceduralLandscapeSource s(p);
		const LandscapeTile t = s.generate({ 17, 90 });
		REQUIRE(t.heights.size() == static_cast<size_t>(t.geometry.heightLevels()));
		for (auto backend : { LandscapeCodec::Backend::Store, LandscapeCodec::Backend::Lz4, LandscapeCodec::Backend::Zstd })
		{
			if (!LandscapeCodec::available(backend)) continue;
			for (bool predictor : { false, true })
			{
				LandscapeCodec c;
				c.backend = backend;
				c.predictor = predictor;
				const auto blob = encodeLandscapeTile(t, c);
				REQUIRE_FALSE(blob.empty());
				LandscapeTile back;
				std::string err;
				REQUIRE_MESSAGE(decodeLandscapeTile(blob.data(), blob.size(), back, -1, &err), err);
				CHECK(back == t);
			}
		}
	}
}

TEST_CASE("landscape tile: the default 100 km geometry round-trips and the predictor shrinks it")
{
	ProceduralLandscapeParams p;   // 512 m, 257 x 257 heights, 12 weight channels
	const ProceduralLandscapeSource s(p);
	const LandscapeTile t = s.generate({ 100, 100 });
	LandscapeCodec stored; stored.backend = LandscapeCodec::Backend::Store; stored.predictor = false;
	const auto rawBlob = encodeLandscapeTile(t, stored);
	CHECK(rawBlob.size() >= t.bytes());
	const auto best = encodeLandscapeTile(t);
	LandscapeTile back;
	REQUIRE(decodeLandscapeTile(best.data(), best.size(), back));
	CHECK(back == t);
	if (LandscapeCodec::available(LandscapeCodec::Backend::Zstd) || LandscapeCodec::available(LandscapeCodec::Backend::Lz4))
		CHECK(best.size() < rawBlob.size() / 2);
}

TEST_CASE("landscape tile: decoding only the first levels gives the prefix of the full decode")
{
	const LandscapeTile t = ProceduralLandscapeSource(smallWorld()).generate({ 3, 4 });
	const auto blob = encodeLandscapeTile(t);
	LandscapeTile one;
	REQUIRE(decodeLandscapeTile(blob.data(), blob.size(), one, 1));
	CHECK(one.heights.size() == 1);
	CHECK(one.weights.size() == 1);
	CHECK(one.heights[0] == t.heights[0]);
	CHECK(one.weights[0] == t.weights[0]);
	CHECK(one.bytes() == t.geometry.decodedBytes(1));
	LandscapeGeometry g;
	REQUIRE(peekLandscapeTile(blob.data(), blob.size(), g));
	CHECK(g == t.geometry);
}

TEST_CASE("landscape tile: format version 1 is fixed byte for byte")
{
	LandscapeGeometry g;
	g.tileM = 128.0f; g.hCells = 2; g.wTexels = 2; g.channels = 1;
	REQUIRE(g.valid());
	REQUIRE(g.heightLevels() == 2);
	REQUIRE(g.weightLevels() == 2);
	LandscapeTile t;
	t.geometry = g;
	t.heights = { { 100, 200, 300, 400, 500, 600, 700, 800, 900 } };
	t.weights = { { 10, 20, 30, 40 } };
	t.buildMips();

	const std::vector<uint8_t> golden = {
	    0x48, 0x4C, 0x54, 0x31, 0x01, 0x00, 0x00, 0x02, 0x02, 0x01, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x43,   // header
	    0x12, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00,                                                         // heights level 0
	    0x64, 0x00, 0xC8, 0x00, 0x2C, 0x01, 0x90, 0x01, 0xF4, 0x01, 0x58, 0x02, 0xBC, 0x02, 0x20, 0x03, 0x84, 0x03,
	    0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,                                                         // heights level 1
	    0x64, 0x00, 0x2C, 0x01, 0xBC, 0x02, 0x84, 0x03,
	    0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0A, 0x14, 0x1E, 0x28,                                 // weights level 0
	    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x19,                                                   // weights level 1
	};
	LandscapeCodec stored; stored.backend = LandscapeCodec::Backend::Store; stored.predictor = false;
	CHECK(encodeLandscapeTile(t, stored) == golden);

	LandscapeTile back;
	std::string err;
	REQUIRE_MESSAGE(decodeLandscapeTile(golden.data(), golden.size(), back, -1, &err), err);
	CHECK(back == t);
}

TEST_CASE("landscape tile: damaged input is refused, never read past the end")
{
	const LandscapeTile t = ProceduralLandscapeSource(smallWorld()).generate({ 1, 1 });
	for (auto backend : { LandscapeCodec::Backend::Store, LandscapeCodec::Backend::Lz4, LandscapeCodec::Backend::Zstd })
	{
		if (!LandscapeCodec::available(backend)) continue;
		LandscapeCodec c; c.backend = backend;
		const auto blob = encodeLandscapeTile(t, c);
		LandscapeTile out;
		// every truncation
		for (size_t n = 0; n < blob.size(); ++n)
			CHECK_FALSE(decodeLandscapeTile(blob.data(), n, out));
		// the blob itself still decodes
		CHECK(decodeLandscapeTile(blob.data(), blob.size(), out));

		auto bad = blob;
		bad[0] ^= 0xFF;                  // magic
		CHECK_FALSE(decodeLandscapeTile(bad.data(), bad.size(), out));
		bad = blob; bad[4] = 2;          // a version this reader does not know
		CHECK_FALSE(decodeLandscapeTile(bad.data(), bad.size(), out));
		bad = blob; bad[5] = 9;          // backend
		CHECK_FALSE(decodeLandscapeTile(bad.data(), bad.size(), out));
		bad = blob; bad[10] = 0;         // hCells = 0
		bad[11] = 0;
		CHECK_FALSE(decodeLandscapeTile(bad.data(), bad.size(), out));
		bad = blob; bad[kLandscapeTileHeaderBytes] ^= 0x7F;      // first stream's raw size
		CHECK_FALSE(decodeLandscapeTile(bad.data(), bad.size(), out));
		bad = blob;                                              // first stream's stored size runs past the blob
		bad[kLandscapeTileHeaderBytes + 4] = 0xFF; bad[kLandscapeTileHeaderBytes + 5] = 0xFF;
		bad[kLandscapeTileHeaderBytes + 6] = 0xFF; bad[kLandscapeTileHeaderBytes + 7] = 0x7F;
		CHECK_FALSE(decodeLandscapeTile(bad.data(), bad.size(), out));
	}
	// random garbage behind a valid header must not crash (compressed backends reject it)
	auto blob = encodeLandscapeTile(t);
	std::mt19937 rng(5);
	for (int round = 0; round < 50; ++round)
	{
		auto bad = blob;
		for (int i = 0; i < 8; ++i) bad[kLandscapeTileHeaderBytes + 8 + rng() % (bad.size() - kLandscapeTileHeaderBytes - 8)] = static_cast<uint8_t>(rng());
		LandscapeTile out;
		(void)decodeLandscapeTile(bad.data(), bad.size(), out);
	}
}

// ── sources ──────────────────────────────────────────────────────────────────
TEST_CASE("procedural landscape source: deterministic, and neighbouring tiles share their edge")
{
	const ProceduralLandscapeSource s(smallWorld());
	const LandscapeTile a = s.generate({ 10, 20 }), a2 = s.generate({ 10, 20 }), b = s.generate({ 11, 20 });
	CHECK(a == a2);
	const int n = a.geometry.heightDim(0);
	for (int z = 0; z < n; ++z)   // last column of a = first column of b
		CHECK(a.heights[0][static_cast<size_t>(z) * n + (n - 1)] == b.heights[0][static_cast<size_t>(z) * n]);
	// every weight texel sums to 255
	const int C = a.geometry.channels;
	for (size_t i = 0; i + C <= a.weights[0].size(); i += C)
	{
		int sum = 0;
		for (int c = 0; c < C; ++c) sum += a.weights[0][i + c];
		CHECK(sum == 255);
	}
	LandscapeTile out;
	CHECK_FALSE(s.load({ -1, 0 }, -1, out));
	CHECK_FALSE(s.load({ 0, 196 }, -1, out));
	CHECK(s.load({ 0, 195 }, 1, out));
	CHECK(out.heights.size() == 1);
}

TEST_CASE("landscape pack: written in any order, every tile reads back; damaged packs are refused")
{
	const ProceduralLandscapeSource s(smallWorld(6));
	const auto path = tempFile("pack");
	{
		LandscapePackWriter w;
		REQUIRE(w.begin(path.string(), s.geometry(), 6, 5));
		std::vector<LandscapeTileKey> keys;
		for (int z = 0; z < 5; ++z)
			for (int x = 0; x < 6; ++x)
				if (!(x == 2 && z == 3)) keys.push_back({ x, z });   // one tile left out
		std::shuffle(keys.begin(), keys.end(), std::mt19937(3));
		for (const auto& k : keys) REQUIRE(w.add(k, encodeLandscapeTile(s.generate(k))));
		CHECK_FALSE(w.add(keys[0], encodeLandscapeTile(s.generate(keys[0]))));         // each tile once
		CHECK_FALSE(w.add({ 6, 0 }, encodeLandscapeTile(s.generate({ 0, 0 }))));       // outside the world
		LandscapeGeometry other = s.geometry();
		other.channels = 8;
		ProceduralLandscapeParams op = smallWorld(6);
		op.geometry = other;
		CHECK_FALSE(w.add({ 2, 3 }, encodeLandscapeTile(ProceduralLandscapeSource(op).generate({ 2, 3 }))));   // not the pack's geometry
		REQUIRE(w.finish());
		CHECK(w.tilesWritten() == 29);
	}
	std::string err;
	auto pack = LandscapePackSource::open(path.string(), &err);
	REQUIRE_MESSAGE(pack, err);
	CHECK(pack->tilesX() == 6);
	CHECK(pack->tilesZ() == 5);
	CHECK(pack->geometry() == s.geometry());
	for (int z = 0; z < 5; ++z)
		for (int x = 0; x < 6; ++x)
		{
			LandscapeTile t;
			if (x == 2 && z == 3)
			{
				CHECK_FALSE(pack->has({ x, z }));
				CHECK_FALSE(pack->load({ x, z }, -1, t));
				continue;
			}
			REQUIRE(pack->load({ x, z }, -1, t));
			CHECK(t == s.generate({ x, z }));
		}
	LandscapeTile t;
	CHECK_FALSE(pack->load({ 6, 0 }, -1, t));

	// a pack cut inside its index, and one with a wrong magic
	const auto cut = tempFile("cut");
	std::filesystem::copy_file(path, cut);
	std::filesystem::resize_file(cut, 100);
	CHECK_FALSE(LandscapePackSource::open(cut.string(), &err));
	{
		std::fstream f(cut, std::ios::in | std::ios::out | std::ios::binary);
		f.seekp(0);
		f.write("XXXX", 4);
	}
	CHECK_FALSE(LandscapePackSource::open(cut.string(), &err));
	CHECK_FALSE(LandscapePackSource::open((path.string() + ".missing"), &err));
	pack.reset();   // Windows cannot delete a file that is still open
	std::filesystem::remove(cut);
	std::filesystem::remove(path);
}

// ── store ────────────────────────────────────────────────────────────────────
TEST_CASE("landscape store: loads the tiles around an anchor, each exactly once, and they are the source's tiles")
{
	auto src = std::make_shared<ProceduralLandscapeSource>(smallWorld(40));
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 1500.0f;
	LandscapeStore store(src, cfg);
	const std::vector<LandscapeAnchor> anchors = { { glm::dvec3(10240.0, 0.0, 10240.0) } };
	REQUIRE(settleStore(store, anchors));

	const size_t expected = tilesWithin(10240.0, 10240.0, 1500.0, 40, 512.0);
	CHECK(store.stats().resident == expected);
	CHECK(store.stats().loadsStarted == expected);          // no tile loaded twice
	CHECK(store.stats().loadsFinished == expected);
	CHECK(store.stats().peakBytes <= store.config().byteBudget);
	for (const auto& k : store.residentKeys())
	{
		const auto t = store.tile(k);
		REQUIRE(t);
		CHECK(*t == src->generate(k));
	}
	// idle frames do not start anything
	for (int i = 0; i < 20; ++i) store.update(anchors, 1.0);
	CHECK(store.stats().loadsStarted == expected);
	CHECK(store.stats().plans == 1);                         // the anchor did not move: no second plan
}

TEST_CASE("landscape store: walking 100 km keeps the resident set inside the kept ring, under the budget, without duplicates")
{
	auto src = std::make_shared<ProceduralLandscapeSource>(smallWorld(196));
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 2500.0f;
	cfg.byteBudget = 200 * src->geometry().decodedBytes();   // room for the ring (~85 tiles) and then some
	LandscapeStore store(src, cfg);

	size_t rounds = 0;
	for (double d = 1000.0; d <= 99000.0; d += 150.0)
	{
		const std::vector<LandscapeAnchor> anchors = { { glm::dvec3(d, 0.0, d) } };
		store.update(anchors, 1.0);
		++rounds;
		const auto keys = store.residentKeys();
		const std::set<std::pair<int, int>> unique = [&] { std::set<std::pair<int, int>> s; for (auto& k : keys) s.insert({ k.x, k.z }); return s; }();
		REQUIRE(unique.size() == keys.size());                                  // no tile twice
		for (const auto& k : keys) REQUIRE(store.inKeptRing(k));                // nothing outside the kept ring
		REQUIRE(store.stats().residentBytes <= cfg.byteBudget);
		REQUIRE(store.stats().loading + store.stats().resident <= store.stats().maxTiles);
		if (rounds % 8 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const std::vector<LandscapeAnchor> last = { { glm::dvec3(99000.0, 0.0, 99000.0) } };
	REQUIRE(settleStore(store, last));
	const LandscapeStoreStats& s = store.stats();
	CHECK(s.peakBytes <= cfg.byteBudget);
	CHECK(s.resident == store.residentKeys().size());
	CHECK(s.loadsStarted == s.loadsFinished + s.loadsFailed + s.loadsCancelled + s.loadsDiscarded);   // nothing leaked
	CHECK(s.loading == 0);
	CHECK(s.evictions > 0);
	// the main thread never sat in update() for long (CI timing is noisy: only a loose bound)
	MESSAGE("walk: " << rounds << " updates, main thread avg " << s.avgUpdateMs << " ms, max " << s.maxUpdateMs << " ms, "
	        << s.loadsStarted << " loads, " << s.loadsCancelled << " cancelled, " << s.loadsDiscarded << " discarded, "
	        << s.evictions << " evicted, " << s.plans << " plans");
	CHECK(s.maxUpdateMs < 100.0);
}

TEST_CASE("landscape store: a budget smaller than the ring shrinks the radius, never the budget")
{
	auto src = std::make_shared<ProceduralLandscapeSource>(smallWorld(40));
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 4000.0f;
	cfg.byteBudget = 10 * src->geometry().decodedBytes();
	LandscapeStore store(src, cfg);
	CHECK(store.stats().maxTiles == 10);
	const std::vector<LandscapeAnchor> anchors = { { glm::dvec3(10240.0, 0.0, 10240.0) } };
	for (int i = 0; i < 200; ++i)
	{
		store.update(anchors, 1.0);
		REQUIRE(store.stats().loading + store.stats().resident <= 10);
		REQUIRE(store.stats().residentBytes <= cfg.byteBudget);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(settleStore(store, anchors));
	CHECK(store.stats().budgetClamped);
	CHECK(store.stats().resident == 10);
	// the ten are the nearest ones
	double farthest = 0.0;
	for (const auto& k : store.residentKeys())
		farthest = std::max(farthest, std::hypot((k.x + 0.5) * 512.0 - 10240.0, (k.z + 0.5) * 512.0 - 10240.0));
	CHECK(farthest < 1200.0);
}

TEST_CASE("landscape store: an anchor that jumps away cancels the loads that have not started and drops the ones that finish late")
{
	ThreadPool pool(2);
	auto src = std::make_shared<GatedSource>(smallWorld(196));
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 2500.0f;
	cfg.maxInFlight = 64;
	{
		LandscapeStore store(src, cfg, &pool);
		const std::vector<LandscapeAnchor> at = { { glm::dvec3(10240.0, 0.0, 10240.0) } };
		const std::vector<LandscapeAnchor> far = { { glm::dvec3(60000.0, 0.0, 60000.0) } };
		store.update(at, 1.0);
		// wait until a worker is blocked inside load() (the pool may run fewer background jobs than it has threads)
		for (int i = 0; i < 2000 && src->started.load() < 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		REQUIRE(src->started.load() >= 1);
		const size_t scheduled = store.stats().loadsStarted;
		REQUIRE(scheduled > 4);

		store.update(far, 1.0);              // 50 km away: the old neighbourhood is out of the kept ring
		src->open = true;
		REQUIRE(settleStore(store, far));

		CHECK(store.stats().resident == tilesWithin(60000.0, 60000.0, 2500.0, 196, 512.0));
		for (const auto& k : store.residentKeys())
			CHECK(std::hypot((k.x + 0.5) * 512.0 - 60000.0, (k.z + 0.5) * 512.0 - 60000.0) < 5000.0);   // none of the old ring
		CHECK(store.stats().loadsCancelled > 0);       // queued loads never ran
		CHECK(store.stats().loadsDiscarded >= 1);      // the one(s) that were running were thrown away
		CHECK(static_cast<size_t>(src->started.load()) < scheduled + store.stats().resident);   // not all of the old ring was read
		const LandscapeStoreStats& s = store.stats();
		CHECK(s.loadsStarted == s.loadsFinished + s.loadsFailed + s.loadsCancelled + s.loadsDiscarded + s.loading);
	}
	src->open = true;
}

TEST_CASE("landscape store: a tile the source cannot give fails once and is tried again only after it left the kept ring")
{
	ThreadPool pool(2);
	auto src = std::make_shared<GatedSource>(smallWorld(40));
	src->open = true;
	src->failingPairs.insert({ 20, 20 });
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 1000.0f;
	LandscapeStore store(src, cfg, &pool);
	const std::vector<LandscapeAnchor> near = { { glm::dvec3(10752.0, 0.0, 10752.0) } };   // inside tile 21,21's corner; 20,20 is wanted
	REQUIRE(settleStore(store, near));
	CHECK(store.stats().failed == 1);
	CHECK(store.stats().loadsFailed == 1);
	CHECK_FALSE(store.isResident({ 20, 20 }));
	for (int i = 0; i < 50; ++i) store.update(near, 1.0);
	CHECK(src->failedLoads.load() == 1);                 // not retried every frame

	const std::vector<LandscapeAnchor> away = { { glm::dvec3(30000.0, 0.0, 30000.0) } };
	REQUIRE(settleStore(store, away));
	CHECK(store.stats().failed == 0);                    // forgotten with the ring
	REQUIRE(settleStore(store, near));
	CHECK(src->failedLoads.load() == 2);                 // asked again on the way back
}

TEST_CASE("landscape store: two anchors keep both neighbourhoods, a scale stretches one radius")
{
	auto src = std::make_shared<ProceduralLandscapeSource>(smallWorld(60));
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 800.0f;
	LandscapeStore store(src, cfg);
	LandscapeAnchor a{ glm::dvec3(5000.0, 0.0, 5000.0) }, b{ glm::dvec3(20000.0, 0.0, 20000.0) };
	REQUIRE(settleStore(store, { a, b }));
	const size_t two = store.stats().resident;
	CHECK(two == tilesWithin(5000.0, 5000.0, 800.0, 60, 512.0) + tilesWithin(20000.0, 20000.0, 800.0, 60, 512.0));
	b.radiusScale = 2.0f;
	REQUIRE(settleStore(store, { a, b }));
	CHECK(store.stats().resident > two);
	CHECK(store.stats().resident == tilesWithin(5000.0, 5000.0, 800.0, 60, 512.0) + tilesWithin(20000.0, 20000.0, 1600.0, 60, 512.0));
	// no anchor: everything goes
	store.update({}, 1.0);
	CHECK(store.stats().resident == 0);
}

TEST_CASE("landscape store: from a pack file, like from the procedural source")
{
	const ProceduralLandscapeSource s(smallWorld(12));
	const auto path = tempFile("storepack");
	{
		LandscapePackWriter w;
		REQUIRE(w.begin(path.string(), s.geometry(), 12, 12));
		for (int z = 0; z < 12; ++z)
			for (int x = 0; x < 12; ++x) REQUIRE(w.add({ x, z }, encodeLandscapeTile(s.generate({ x, z }))));
		REQUIRE(w.finish());
	}
	auto pack = LandscapePackSource::open(path.string());
	REQUIRE(pack);
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 1200.0f;
	{
	LandscapeStore store(pack, cfg);
	const std::vector<LandscapeAnchor> anchors = { { glm::dvec3(3000.0, 0.0, 3000.0) } };
	REQUIRE(settleStore(store, anchors));
	CHECK(store.stats().resident == tilesWithin(3000.0, 3000.0, 1200.0, 12, 512.0));
	for (const auto& k : store.residentKeys()) CHECK(*store.tile(k) == s.generate(k));
	}   // the store, its tiles and the pack have to let go of the file before it can be deleted (Windows)
	CHECK(pack.use_count() == 1);   // no job, queued or finished, still holds the source
	pack.reset();
	std::filesystem::remove(path);
}

// ── coarse world (pack version 2) ────────────────────────────────────────────
namespace
{
// Writes `source`'s whole world into a pack at `path` in the given order of keys.
bool writePack(const ProceduralLandscapeSource& source, const std::filesystem::path& path, std::vector<LandscapeTileKey> keys,
               int firstCoarseLevel, bool withSample)
{
	LandscapePackWriter w;
	if (!w.begin(path.string(), source.geometry(), source.tilesX(), source.tilesZ(), firstCoarseLevel)) return false;
	const LandscapeCoarseLayout layout = LandscapeCoarseLayout::forGeometry(source.geometry(), firstCoarseLevel);
	for (const auto& k : keys)
	{
		const LandscapeTile t = source.generate(k);
		const LandscapeCoarseSample sample = landscapeCoarseSampleOf(t, layout);
		if (!w.add(k, encodeLandscapeTile(t), withSample ? &sample : nullptr)) return false;
	}
	return w.finish();
}

std::vector<LandscapeTileKey> allKeys(int tx, int tz)
{
	std::vector<LandscapeTileKey> k;
	for (int z = 0; z < tz; ++z)
		for (int x = 0; x < tx; ++x) k.push_back({ x, z });
	return k;
}
} // namespace

TEST_CASE("landscape coarse world: the 32 m grid is the tiles' own samples, seam to the tile levels included")
{
	ProceduralLandscapeParams p;       // the real geometry: 512 m tiles, 256 cells, 12 channels
	p.tilesX = 3;
	p.tilesZ = 2;
	const ProceduralLandscapeSource source(p);
	const auto path = tempFile("coarse");
	REQUIRE(writePack(source, path, allKeys(3, 2), 4, true));

	std::string err;
	auto pack = LandscapePackSource::open(path.string(), &err);
	REQUIRE_MESSAGE(pack, err);
	CHECK(pack->version() == 2);
	const LandscapeCoarseWorld* c = pack->coarse();
	REQUIRE(c);
	CHECK(c->heightLevel == 4);
	CHECK(c->cellsPerTile == 16);
	CHECK(c->cellM(0) == doctest::Approx(32.0f));
	REQUIRE(c->heights.size() >= 5);                           // 32 m, 64 m, 128 m, 256 m ... down to a few samples
	CHECK(c->heights[0].w == 3 * 16 + 1);
	CHECK(c->heights[0].h == 2 * 16 + 1);
	CHECK(c->heights[1].w == 25);
	CHECK(c->heights[2].w == 13);
	CHECK(c->heights[3].w == 7);
	CHECK(c->heights.back().w >= 2);
	CHECK(c->heights.back().h >= 2);
	for (size_t i = 1; i < c->heights.size(); ++i) CHECK(c->cellM(static_cast<int>(i)) == doctest::Approx(c->cellM(static_cast<int>(i) - 1) * 2.0f));
	CHECK(c->weightsW == 3 * 4);                               // 4 x 4 texels of 128 m per tile
	CHECK(c->weightsH == 2 * 4);
	CHECK(c->channels == 12);

	// The grid is exactly tile level 0 at stride 16 (2 m x 16 = 32 m): at every tile corner and edge too.
	for (int tz = 0; tz < 2; ++tz)
		for (int tx = 0; tx < 3; ++tx)
		{
			const LandscapeTile t = source.generate({ tx, tz });
			const int n0 = t.geometry.heightDim(0);
			for (int j = 0; j <= 16; ++j)
				for (int i = 0; i <= 16; ++i)
					REQUIRE(c->heights[0].at(tx * 16 + i, tz * 16 + j) == t.heights[0][static_cast<size_t>(j * 16) * n0 + i * 16]);
			// and the tile's own mip 4 (what a tile-level patch next to it would read)
			for (int j = 0; j <= 16; ++j)
				for (int i = 0; i <= 16; ++i) REQUIRE(c->heights[0].at(tx * 16 + i, tz * 16 + j) == t.heights[4][static_cast<size_t>(j) * 17 + i]);
			// weights: the tile's coarsest mip
			for (int j = 0; j < 4; ++j)
				for (int i = 0; i < 4; ++i)
					for (int ch = 0; ch < 12; ++ch)
						REQUIRE(c->weights[(static_cast<size_t>(tz * 4 + j) * c->weightsW + tx * 4 + i) * 12 + ch] == t.weights[5][(static_cast<size_t>(j) * 4 + i) * 12 + ch]);
		}
	// the coarser grids are point samples of the 32 m one
	for (int z = 0; z < c->heights[1].h; ++z)
		for (int x = 0; x < c->heights[1].w; ++x) REQUIRE(c->heights[1].at(x, z) == c->heights[0].at(2 * x, 2 * z));
	// the clamped read at the world's edge
	CHECK(c->heights[0].at(-5, -5) == c->heights[0].at(0, 0));
	CHECK(c->heights[0].at(10000, 10000) == c->heights[0].at(c->heights[0].w - 1, c->heights[0].h - 1));
	pack.reset();   // Windows cannot delete a file that is still open
	std::filesystem::remove(path);
}

TEST_CASE("landscape coarse world: the pack's coarse world equals the procedural source's, in any write order, with or without the sample")
{
	const ProceduralLandscapeSource source(smallWorld(5));
	const auto a = tempFile("coarseA"), b = tempFile("coarseB");
	auto keys = allKeys(5, 5);
	REQUIRE(writePack(source, a, keys, 4, true));
	std::shuffle(keys.begin(), keys.end(), std::mt19937(9));
	REQUIRE(writePack(source, b, keys, 4, false));     // the writer decodes each blob itself
	auto pa = LandscapePackSource::open(a.string());
	auto pb = LandscapePackSource::open(b.string());
	REQUIRE(pa);
	REQUIRE(pb);
	const LandscapeCoarseWorld* ref = source.coarse();
	REQUIRE(ref);
	for (const auto* c : { pa->coarse(), pb->coarse() })
	{
		REQUIRE(c);
		REQUIRE(c->heights.size() == ref->heights.size());
		for (size_t i = 0; i < ref->heights.size(); ++i)
		{
			CHECK(c->heights[i].w == ref->heights[i].w);
			CHECK(c->heights[i].v == ref->heights[i].v);
		}
		CHECK(c->weights == ref->weights);
	}
	pa.reset();
	pb.reset();
	std::filesystem::remove(a);
	std::filesystem::remove(b);
}

TEST_CASE("landscape coarse world: a pack without one, a version 1 pack, and a cut-off coarse section")
{
	const ProceduralLandscapeSource source(smallWorld(4));
	// no coarse world requested
	const auto none = tempFile("nocoarse");
	REQUIRE(writePack(source, none, allKeys(4, 4), -1, true));
	auto packNone = LandscapePackSource::open(none.string());
	REQUIRE(packNone);
	CHECK(packNone->coarse() == nullptr);
	LandscapeTile t;
	CHECK(packNone->load({ 2, 2 }, -1, t));
	packNone.reset();
	std::filesystem::remove(none);

	// a version 1 file, made by hand: 32-byte header, index, blobs, nothing else
	const auto v1 = tempFile("v1");
	{
		const LandscapeGeometry g = source.geometry();
		const size_t n = 16;
		std::vector<std::vector<uint8_t>> blobs;
		for (const auto& k : allKeys(4, 4)) blobs.push_back(encodeLandscapeTile(source.generate(k)));
		std::vector<uint8_t> head(32 + n * 12, 0);
		const auto put32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) head[o + i] = static_cast<uint8_t>(v >> (8 * i)); };
		put32(0, kLandscapePackMagic);
		put32(4, 1);
		put32(8, 4);
		put32(12, 4);
		uint32_t bits;
		std::memcpy(&bits, &g.tileM, 4);
		put32(16, bits);
		head[20] = static_cast<uint8_t>(g.hCells); head[21] = static_cast<uint8_t>(g.hCells >> 8);
		head[22] = static_cast<uint8_t>(g.wTexels); head[23] = static_cast<uint8_t>(g.wTexels >> 8);
		head[24] = g.channels;
		uint64_t pos = head.size();
		for (size_t i = 0; i < n; ++i)
		{
			for (int b = 0; b < 8; ++b) head[32 + i * 12 + b] = static_cast<uint8_t>(pos >> (8 * b));
			put32(32 + i * 12 + 8, static_cast<uint32_t>(blobs[i].size()));
			pos += blobs[i].size();
		}
		std::ofstream f(v1, std::ios::binary);
		f.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
		for (const auto& bl : blobs) f.write(reinterpret_cast<const char*>(bl.data()), static_cast<std::streamsize>(bl.size()));
	}
	std::string err;
	auto packV1 = LandscapePackSource::open(v1.string(), &err);
	REQUIRE_MESSAGE(packV1, err);
	CHECK(packV1->version() == 1);
	CHECK(packV1->coarse() == nullptr);
	for (const auto& k : allKeys(4, 4))
	{
		LandscapeTile loaded;
		REQUIRE(packV1->load(k, -1, loaded));
		CHECK(loaded == source.generate(k));
	}
	packV1.reset();
	std::filesystem::remove(v1);

	// a coarse section cut short
	const auto good = tempFile("coarsegood");
	REQUIRE(writePack(source, good, allKeys(4, 4), 4, true));
	const uint64_t size = std::filesystem::file_size(good);
	const auto cut = tempFile("coarsecut");
	std::filesystem::copy_file(good, cut);
	std::filesystem::resize_file(cut, size - 30);
	CHECK_FALSE(LandscapePackSource::open(cut.string(), &err));
	std::filesystem::remove(cut);
	std::filesystem::remove(good);
}

TEST_CASE("landscape store: destroying it waits for the loads that are running, so nothing holds the source (or its file) afterwards")
{
	ThreadPool pool(2);
	auto src = std::make_shared<GatedSource>(smallWorld(40));
	LandscapeStoreConfig cfg;
	cfg.loadRadius = 1500.0f;
	auto store = std::make_unique<LandscapeStore>(src, cfg, &pool);
	store->update({ { glm::dvec3(10240.0, 0.0, 10240.0) } }, 1.0);
	for (int i = 0; i < 2000 && src->started.load() < 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	REQUIRE(src->started.load() >= 1);                    // a load is running now, blocked inside the source

	// the source lets go a little later, from another thread: until then the destructor has to wait
	std::thread opener([&] { std::this_thread::sleep_for(std::chrono::milliseconds(80)); src->open = true; });
	const auto t0 = std::chrono::steady_clock::now();
	store.reset();
	const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	opener.join();
	CHECK(waited >= 40);                                  // it did wait for the running load
	CHECK(src.use_count() == 1);                          // and no job holds the source any more

	// clear() does the same: the loads that run finish, the ones that wait never start
	ThreadPool pool2(2);
	auto src2 = std::make_shared<GatedSource>(smallWorld(40));
	LandscapeStore store2(src2, cfg, &pool2);
	store2.update({ { glm::dvec3(10240.0, 0.0, 10240.0) } }, 1.0);
	for (int i = 0; i < 2000 && src2->started.load() < 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	std::thread opener2([&] { std::this_thread::sleep_for(std::chrono::milliseconds(60)); src2->open = true; });
	store2.clear();
	opener2.join();
	CHECK(src2.use_count() == 2);                         // the store itself still has the source; no job does
	CHECK(store2.stats().resident == 0);
	CHECK(store2.residentKeys().empty());
}
