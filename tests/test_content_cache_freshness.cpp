// A cached EngineContent file the server has replaced is dropped, so the on-demand download fetches the
// current one (ContentSync/CacheFreshness.h). No server: the manifest is built by hand from the bytes.

#include "doctest.h"
#include "TestFsUtil.h"

#include <ContentSync/CacheFreshness.h>
#include <ContentSync/EngineContentManifest.h>
#include <Hpak/HpakFormat.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
namespace fs = std::filesystem;

void writeFile(const fs::path& p, const std::vector<uint8_t>& bytes)
{
	fs::create_directories(p.parent_path());
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

HE::Cs::EngineContentManifestEntry entryFor(const std::string& rel, const std::vector<uint8_t>& bytes)
{
	HE::Cs::EngineContentManifestEntry e;
	e.relativePath = rel;
	e.contentHash  = Hpak::fnv1a64Bytes(bytes.data(), bytes.size());
	e.size         = bytes.size();
	return e;
}

std::vector<uint8_t> pattern(size_t n, uint8_t seed)
{
	std::vector<uint8_t> v(n);
	for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed + i * 7);
	return v;
}
} // namespace

TEST_CASE("CacheFreshness: current files stay, replaced ones go, busy ones are left alone")
{
	const fs::path cache = fs::temp_directory_path() / "he_cache_fresh";
	he_test::removeAllQuiet(cache);

	const auto same  = pattern(5000, 1);
	const auto oldA  = pattern(6000, 2);                 // same name on the server, other size
	const auto oldB  = pattern(7000, 3);                 // same name and size, other bytes
	auto newB = oldB; newB[3500] ^= 0x55;
	const auto busy  = pattern(4000, 4);
	const auto raw   = pattern(100, 5);

	writeFile(cache / "Textures/same.hasset", same);
	writeFile(cache / "Textures/a.hasset", oldA);
	writeFile(cache / "Textures/b.hasset", oldB);
	writeFile(cache / "Textures/busy.hasset", busy);
	writeFile(cache / "Audio/raw.wav", raw);

	HE::Cs::EngineContentManifest m;
	m.entries.push_back(entryFor("Textures/same.hasset", same));
	m.entries.push_back(entryFor("Textures/a.hasset", pattern(9000, 2)));      // server: bigger
	m.entries.push_back(entryFor("Textures/b.hasset", newB));                  // server: same size, other bytes
	m.entries.push_back(entryFor("Textures/busy.hasset", pattern(4001, 4)));   // stale but downloading
	m.entries.push_back(entryFor("Textures/missing.hasset", same));            // not cached: nothing to do
	HE::Cs::EngineContentManifestEntry rawEntry; rawEntry.relativePath = "Audio/raw.wav";   // no hash
	m.entries.push_back(rawEntry);

	std::vector<std::string> log;
	const size_t removed = HE::Cs::discardStaleCachedFiles(
		cache, m, [](const std::string& rel) { return rel == "Textures/busy.hasset"; },
		[&](const std::string& l) { log.push_back(l); });
	CHECK(removed == 2);
	CHECK(log.size() == 2);
	CHECK(fs::exists(cache / "Textures/same.hasset"));
	CHECK_FALSE(fs::exists(cache / "Textures/a.hasset"));
	CHECK_FALSE(fs::exists(cache / "Textures/b.hasset"));
	CHECK(fs::exists(cache / "Textures/busy.hasset"));
	CHECK(fs::exists(cache / "Audio/raw.wav"));

	// A second pass (the busy file still downloading) changes nothing; the remembered hash of `same` is reused.
	CHECK(HE::Cs::discardStaleCachedFiles(cache, m, [](const std::string& rel) { return rel == "Textures/busy.hasset"; }) == 0);
	CHECK(fs::exists(cache / ".cache-hashes.json"));
	he_test::removeAllQuiet(cache);
}

TEST_CASE("CacheFreshness: a remembered hash does not outlive a replaced file")
{
	const fs::path cache = fs::temp_directory_path() / "he_cache_fresh_rec";
	he_test::removeAllQuiet(cache);

	const auto v1 = pattern(3000, 9);
	auto v2 = v1; v2[10] ^= 0x01;                                   // same size
	writeFile(cache / "x.hasset", v1);
	HE::Cs::recordCachedFile(cache, "x.hasset", Hpak::fnv1a64Bytes(v1.data(), v1.size()));

	HE::Cs::EngineContentManifest m;
	m.entries.push_back(entryFor("x.hasset", v1));
	CHECK(HE::Cs::discardStaleCachedFiles(cache, m) == 0);          // matches the record

	std::this_thread::sleep_for(std::chrono::milliseconds(50));     // a visibly newer mtime
	writeFile(cache / "x.hasset", v2);                              // replaced behind the record's back
	CHECK(HE::Cs::discardStaleCachedFiles(cache, m) == 1);          // size equal, mtime changed: hashed again
	CHECK_FALSE(fs::exists(cache / "x.hasset"));
	he_test::removeAllQuiet(cache);
}
