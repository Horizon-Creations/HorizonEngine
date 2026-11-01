// LandscapeFoliageLayer through the EngineContent path, checked without a server
// (Thema 327, step 2). Three halves, each the real code or a line-for-line mirror
// of the one place the real code is not callable without SFTP:
//  * PUBLISH: the scan in EngineContentPublish.cpp (publishEngineContentBlocking,
//    the `recursive_directory_iterator` loop) is inline in a function that opens
//    an SFTP session first, so it is mirrored here: a regular file whose
//    extension is ".hasset", UUID from its META chunk, FNV-1a over the bytes, path
//    relative to the root. Nothing in it names an asset type or a folder, which is
//    what this proves for the new type in a new folder.
//  * MANIFEST: the real serializeManifest / parseManifest.
//  * PULL: the editor's registration loop (EditorApplication.cpp, m_sftpManifestReady)
//    mirrored with the download swapped for a file copy, driving the real
//    ContentManager::registerRemoteAsset -> loadAssetAsync -> pollAsyncResults.
// The asset is a SCRATCH set in a temp tree: the real EditorDeps/EngineContent
// is not touched (its default sets wait for the human's UUID-block answer).
#include "doctest.h"
#include "TestFsUtil.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>
#include <ContentSync/EngineContentManifest.h>
#include <Hpak/HpakFormat.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
struct TempDir
{
	fs::path path;
	explicit TempDir(const char* name)
	{
		path = fs::temp_directory_path() / name;
		he_test::removeAllQuiet(path);
		fs::create_directories(path);
	}
	~TempDir() { he_test::removeAllQuiet(path); }
};

// EngineContentPublish.cpp readAssetUuid, as test_engine_weather_sounds.cpp mirrors it.
bool publishScanUuid(const std::vector<uint8_t>& bytes, HE::UUID& out)
{
	HAsset::Reader reader;
	if (!reader.openData(bytes)) return false;
	if (reader.header().version < 2) return false;
	const auto* meta = reader.findChunk(HAsset::CHUNK_META);
	if (!meta) return false;
	size_t off = sizeof(uint16_t);
	return HAsset::Reader::readPOD(meta->data, off, out.hi) && HAsset::Reader::readPOD(meta->data, off, out.lo);
}

// The scan loop of publishEngineContentBlocking.
HE::Cs::EngineContentManifest publishScan(const fs::path& root)
{
	HE::Cs::EngineContentManifest local;
	std::error_code ec;
	for (const auto& p : fs::recursive_directory_iterator(
	         root, fs::directory_options::skip_permission_denied, ec))
	{
		std::error_code fileEc;
		if (!p.is_regular_file(fileEc) || p.path().extension() != ".hasset") continue;
		std::ifstream f(p.path(), std::ios::binary);
		if (!f) continue;
		std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		HE::UUID id;
		if (!publishScanUuid(bytes, id) || id == HE::UUID{}) continue;
		HE::Cs::EngineContentManifestEntry e;
		e.relativePath = fs::relative(p.path(), root).generic_string();
		e.uuid         = id;
		e.contentHash  = Hpak::fnv1a64Bytes(bytes.data(), bytes.size());
		e.size         = bytes.size();
		local.entries.push_back(std::move(e));
	}
	return local;
}

std::vector<uint8_t> readBytes(const fs::path& p)
{
	std::ifstream f(p, std::ios::binary);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct FakeDownload
{
	int                       calls = 0;
	std::function<void(bool)> done;
	fs::path                  from, to;
	void arrive()
	{
		fs::create_directories(to.parent_path());
		fs::copy_file(from, to, fs::copy_options::overwrite_existing);
		auto d = std::move(done);
		done = nullptr;
		if (d) d(true);
	}
};

template<class Pred>
bool pumpUntil(ContentManager& cm, Pred pred)
{
	for (int i = 0; i < 400 && !pred(); ++i)
	{
		cm.pollAsyncResults(16);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return pred();
}

// Not EngineContentManifest::findByUuid/findByPath (test_engine_content_manifest_refresh.cpp's
// note): the struct is not HE_CS_API, so its out-of-line members are not exported from the
// Windows DLL — a direct call links on this platform's compiler but fails LNK2019 at link time.
bool hasUuid(const HE::Cs::EngineContentManifest& m, HE::UUID id)
{
	return std::any_of(m.entries.begin(), m.entries.end(),
	                   [id](const HE::Cs::EngineContentManifestEntry& e) { return e.uuid == id; });
}
bool hasPath(const HE::Cs::EngineContentManifest& m, const std::string& path)
{
	return std::any_of(m.entries.begin(), m.entries.end(),
	                   [&path](const HE::Cs::EngineContentManifestEntry& e) { return e.relativePath == path; });
}
}

TEST_CASE("landscape foliage layer sync: a set in EngineContent/LandscapeFoliage is published and pulled")
{
	TempDir work("he_test_lfol_sync");   // server-side EngineContent, as EditorDeps/EngineContent
	const fs::path serverRoot = work.path / "EngineContent";
	const std::string rel     = "LandscapeFoliage/LF_Scratch.hasset";

	// Author the scratch set the way the engine would write it, then add the
	// things a scan must skip: a raw file, a file that is not an asset at all.
	HE::UUID id;
	{
		ContentManager author(serverRoot.string());
		LandscapeFoliageLayerAsset set;
		set.type = HE::AssetType::LandscapeFoliageLayer;
		set.name = "LF_Scratch";
		set.path = rel;
		set.id   = HE::UUID::generate();
		set.description = "scratch";
		LandscapeFoliageEntry e;
		e.name = "Rock"; e.meshAssetId = { 0x100, 1 }; e.paintLayer = 1; e.slopeMin = 0.3f;
		set.entries.push_back(e);
		REQUIRE(author.saveAsset(set));
		id = set.id;
	}
	fs::create_directories(serverRoot / "LandscapeFoliage");
	{ std::ofstream(serverRoot / "LandscapeFoliage" / "notes.txt") << "not an asset"; }
	{ std::ofstream(serverRoot / "LandscapeFoliage" / "Truncated.hasset") << "HAST"; }

	// PUBLISH: exactly one entry, with the file's UUID, size and byte hash.
	const HE::Cs::EngineContentManifest local = publishScan(serverRoot);
	REQUIRE(local.entries.size() == 1);
	const auto& entry = local.entries[0];
	CHECK(entry.relativePath == rel);
	CHECK(entry.uuid == id);
	const std::vector<uint8_t> bytes = readBytes(serverRoot / rel);
	CHECK(entry.size == bytes.size());
	CHECK(entry.contentHash == Hpak::fnv1a64Bytes(bytes.data(), bytes.size()));
	MESSAGE("manifest entry: " << entry.relativePath << " uuid " << std::hex << entry.uuid.hi << ":"
	        << entry.uuid.lo << " hash " << entry.contentHash << std::dec << " size " << entry.size);

	// MANIFEST: the real (de)serialiser keeps all of it.
	HE::Cs::EngineContentManifest manifest;
	REQUIRE(HE::Cs::parseManifest(HE::Cs::serializeManifest(local), manifest));
	REQUIRE(manifest.entries.size() == 1);
	CHECK(hasUuid(manifest, id));
	CHECK(hasPath(manifest, rel));
	CHECK(manifest.entries[0].contentHash == entry.contentHash);

	// PULL: the editor registers every manifest entry that has a UUID under
	// "Engine/<path>"; the first request for the UUID asks for the download.
	TempDir proj("he_test_lfol_sync_project"), cache("he_test_lfol_sync_cache");
	ContentManager cm(proj.path.string());
	cm.setEngineContentRoot(cache.path.string());
	FakeDownload dl;
	dl.from = serverRoot / rel;
	dl.to   = cache.path / rel;
	for (const auto& e : manifest.entries)
	{
		if (e.uuid == HE::UUID{}) continue;                 // raw entries have nothing to register
		cm.registerRemoteAsset(e.uuid, "Engine/" + e.relativePath,
			[&dl](std::function<void(bool)> done) { ++dl.calls; dl.done = std::move(done); });
	}

	HE::UUID loaded;
	bool     called = false;
	cm.loadAssetAsync(id, [&](HE::UUID got) { loaded = got; called = true; });
	REQUIRE(dl.calls == 1);                                 // the pull fired on first request
	CHECK(cm.getLandscapeFoliageLayer(id) == nullptr);      // nothing on disk yet
	dl.arrive();
	REQUIRE(pumpUntil(cm, [&] { return called; }));
	CHECK(loaded == id);
	CHECK(cm.assetType(id) == HE::AssetType::LandscapeFoliageLayer);
	const LandscapeFoliageLayerAsset* got = cm.getLandscapeFoliageLayer(id);
	REQUIRE(got != nullptr);
	CHECK(got->description == "scratch");
	REQUIRE(got->entries.size() == 1);
	CHECK(got->entries[0].meshAssetId == HE::UUID{ 0x100, 1 });
	CHECK(got->entries[0].paintLayer == 1);
	CHECK(got->entries[0].slopeMin == 0.3f);
}

TEST_CASE("landscape foliage layer sync: an edit changes the content hash, so a publish re-uploads it")
{
	TempDir work("he_test_lfol_sync_hash");
	const std::string rel = "LandscapeFoliage/LF_Scratch.hasset";
	ContentManager cm(work.path.string());
	LandscapeFoliageLayerAsset set;
	set.type = HE::AssetType::LandscapeFoliageLayer;
	set.name = "LF_Scratch"; set.path = rel; set.id = HE::UUID::generate();
	REQUIRE(cm.saveAsset(set));
	const auto before = publishScan(work.path);
	REQUIRE(before.entries.size() == 1);

	// Same bytes on a second save: unchanged (the diff would skip the upload) ...
	REQUIRE(cm.saveAsset(set));
	CHECK(publishScan(work.path).entries[0].contentHash == before.entries[0].contentHash);
	// ... and a changed rule is a different file.
	LandscapeFoliageEntry e; e.name = "Tuft"; e.meshAssetId = { 0x101, 1 };
	set.entries.push_back(e);
	REQUIRE(cm.saveAsset(set));
	CHECK(publishScan(work.path).entries[0].contentHash != before.entries[0].contentHash);
}
