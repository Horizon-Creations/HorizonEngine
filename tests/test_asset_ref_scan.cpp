#include "doctest.h"
#include "TestFsUtil.h"
#include <ContentManager/AssetRefScan.h>
#include <ContentManager/HAsset.h>
#include <Types/Enums.h>
#include <Types/UUID.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
//  HE::AssetRefs::findReferrers — "what still points at this?"
//
//  Everything here is synthesized by hand with HAsset::Writer rather than saved
//  through ContentManager, because the scan reads the RAW chunk bytes: the point
//  of most of these cases is which byte shape a reference is stored in (a
//  length-prefixed string, a JSON blob nested inside one, a whole JSON chunk, a
//  [hi,lo] pair under `components`), and a typed save would hide that behind the
//  serializer's choices.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	struct TempContentDir
	{
		fs::path path;   // …/<name>/Content — the project root is its parent
		explicit TempContentDir(const char* name)
		{
			path = fs::temp_directory_path() / name / "Content";
			he_test::removeAllQuiet(path.parent_path());
			fs::create_directories(path);
		}
		~TempContentDir() { he_test::removeAllQuiet(path.parent_path()); }

		std::string projectRoot() const { return path.parent_path().string(); }
	};

	// Distinctive ids: `hi` is what the scan turns into a decimal needle, so a
	// value that could also appear as an entity id (111, 222…) in the same JSON
	// would make a negative case pass without ever being parsed.
	const HE::UUID kTexId  { 7412589630741258963ull, 1597534862159753486ull };
	const HE::UUID kMatId  { 8523697410852369741ull, 2648153972648153972ull };
	// HE::UUID::generate() leaves the top bit of `hi` random, so more than half of
	// all real asset ids do not fit in an int64 — the JSON matcher has to read them
	// as UNSIGNED or it silently answers "nothing references this" for them.
	const HE::UUID kBigId  { 18446744073709551557ull, 17293822569102704639ull };

	struct Chunk
	{
		uint32_t             id;
		std::vector<uint8_t> data;
	};

	// META field order mirrors ContentManager::buildMetaChunk — and this chunk is
	// exactly the one the scan must NOT read, since it holds the asset's own path.
	std::vector<uint8_t> metaChunk(HE::AssetType type, const HE::UUID& id,
	                               const std::string& name, const std::string& relPath)
	{
		std::vector<uint8_t> buf;
		HAsset::Writer::appendPOD(buf, static_cast<uint16_t>(type));
		HAsset::Writer::appendPOD(buf, id.hi);
		HAsset::Writer::appendPOD(buf, id.lo);
		HAsset::Writer::appendString(buf, name);
		HAsset::Writer::appendString(buf, relPath);
		return buf;
	}

	void writeAsset(const fs::path& contentRoot, const std::string& relPath,
	                HE::AssetType type, const HE::UUID& id,
	                const std::vector<Chunk>& extra = {})
	{
		const fs::path abs = contentRoot / relPath;
		fs::create_directories(abs.parent_path());
		HAsset::Writer w;
		const std::vector<uint8_t> meta =
			metaChunk(type, id, fs::path(relPath).stem().string(), relPath);
		w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
		for (const Chunk& c : extra) w.addChunk(c.id, c.data.data(), c.data.size());
		REQUIRE(w.write(abs.string(), static_cast<uint16_t>(type)));
	}

	// The flat MTRL shape: a shader path, then the texture list — every value a
	// length-prefixed string, which is the only thing the binary walk can see.
	std::vector<uint8_t> materialChunk(const std::vector<std::string>& texturePaths)
	{
		std::vector<uint8_t> buf;
		HAsset::Writer::appendString(buf, "Shaders/Lit.hasset");
		HAsset::Writer::appendVec(buf, texturePaths);
		return buf;
	}

	// The awkward shape: a whole JSON document stored as ONE length-prefixed
	// string inside a binary chunk (a material's node graph inside MTRL). The
	// reference has no length prefix of its own in there — it can only be found by
	// recognising the enclosing document.
	std::vector<uint8_t> materialGraphChunk(const std::string& graphJson)
	{
		std::vector<uint8_t> buf;
		HAsset::Writer::appendString(buf, "Shaders/Lit.hasset");
		HAsset::Writer::appendString(buf, graphJson);
		return buf;
	}

	std::vector<uint8_t> rawChunk(const std::string& text)
	{
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	void writeTextFile(const fs::path& abs, const std::string& text)
	{
		fs::create_directories(abs.parent_path());
		std::ofstream f(abs, std::ios::binary | std::ios::trunc);
		REQUIRE(f.is_open());
		f.write(text.data(), static_cast<std::streamsize>(text.size()));
	}

	// Proof that a negative case is not vacuous: the bytes really are there, so
	// the scan had every opportunity to (wrongly) report the file.
	bool fileContains(const fs::path& file, const std::string& needle)
	{
		std::ifstream f(file, std::ios::binary);
		const std::string bytes((std::istreambuf_iterator<char>(f)),
		                        std::istreambuf_iterator<char>());
		return bytes.find(needle) != std::string::npos;
	}

	const HE::AssetRefs::Referrer* findRef(const HE::AssetRefs::ScanResult& r,
	                                       const std::string& displayPath)
	{
		for (const HE::AssetRefs::Referrer& x : r.referrers)
			if (x.displayPath == displayPath) return &x;
		return nullptr;
	}

	std::string uuidPair(const HE::UUID& id)
	{
		return "[" + std::to_string(id.hi) + "," + std::to_string(id.lo) + "]";
	}

	HE::AssetRefs::ScanRequest requestFor(const TempContentDir& dir)
	{
		HE::AssetRefs::ScanRequest req;
		req.contentRoot    = dir.path.string();
		req.projectRoot    = dir.projectRoot();
		req.contentDirName = "Content";
		return req;
	}
}

// ─── Path references ─────────────────────────────────────────────────────────

TEST_CASE("findReferrers names every asset that stores a texture's path")
{
	TempContentDir dir("he_test_refscan_paths");

	writeAsset(dir.path, "Tex/Rock.hasset", HE::AssetType::Texture, kTexId,
	           { { HAsset::CHUNK_PIXL, { 255, 255, 255, 255 } } });

	// 1. the flat path list inside MTRL (a length-prefixed string)
	writeAsset(dir.path, "Mat.hasset", HE::AssetType::Material, HE::UUID::generate(),
	           { { HAsset::CHUNK_MTRL, materialChunk({ "Tex/Rock.hasset" }) } });

	// 2. a JSON node graph stored as one length-prefixed string INSIDE that chunk
	writeAsset(dir.path, "GraphMat.hasset", HE::AssetType::Material, HE::UUID::generate(),
	           { { HAsset::CHUNK_MTRL, materialGraphChunk(
	                 R"({"nodes":[{"id":1,"type":"TextureSample","s":"Tex/Rock.hasset"}],"links":[]})") } });

	// 3. a chunk whose ENTIRE payload is JSON (widget logic graph)
	writeAsset(dir.path, "Hud.hasset", HE::AssetType::Widget, HE::UUID::generate(),
	           { { HAsset::CHUNK_UIWG, rawChunk(
	                 R"({"nodes":[{"id":1,"s":"Tex/Rock.hasset"}],"links":[]})") } });

	// 4. raw source text (no length prefix anywhere — matched as text)
	writeAsset(dir.path, "Spawner.hasset", HE::AssetType::Script, HE::UUID::generate(),
	           { { HAsset::CHUNK_SRC, rawChunk("function init()\n  load(\"Tex/Rock.hasset\")\nend\n") } });

	// 5. a loose script beside the assets
	writeTextFile(dir.path / "Scripts" / "Player.lua",
	              "local tex = horizon.loadAsset(\"Tex/Rock.hasset\")\n");

	// Negatives — each in its OWN file, because a file is one verdict.
	writeAsset(dir.path, "Unrelated.hasset", HE::AssetType::Material, HE::UUID::generate(),
	           { { HAsset::CHUNK_MTRL, materialChunk({ "Tex/Grass.hasset" }) } });
	writeAsset(dir.path, "NearMiss.hasset", HE::AssetType::Widget, HE::UUID::generate(),
	           { { HAsset::CHUNK_UIWG, rawChunk(R"({"nodes":[{"id":1,"s":"Tex/Rock.hasset.bak"}]})") } });
	writeTextFile(dir.path / "Scripts" / "Notes.lua", "-- backup lives at Tex/Rock.hasset.bak\n");

	HE::AssetRefs::ScanTargets targets;
	targets.paths.push_back("Tex/Rock.hasset");

	HE::AssetRefs::ScanRequest req = requestFor(dir);
	// What the Content Browser does: the file being deleted is not its own referrer.
	req.excludeFiles.push_back((dir.path / "Tex" / "Rock.hasset").string());

	const HE::AssetRefs::ScanResult res = HE::AssetRefs::findReferrers(targets, req);

	CHECK_FALSE(res.incomplete);
	CHECK_FALSE(res.truncated);
	CHECK(res.filesScanned > 0);
	CHECK(res.referrers.size() == 5);

	for (const char* hit : { "Mat.hasset", "GraphMat.hasset", "Hud.hasset",
	                         "Spawner.hasset", "Scripts/Player.lua" })
	{
		const HE::AssetRefs::Referrer* r = findRef(res, hit);
		REQUIRE_MESSAGE(r != nullptr, hit);
		// A stored path, not an id — the dialog says so, so lock it in.
		CHECK(r->kind == HE::AssetRefs::RefKind::Path);
		CHECK(fs::exists(r->absolutePath));
	}

	CHECK(findRef(res, "Unrelated.hasset")   == nullptr);
	CHECK(findRef(res, "NearMiss.hasset")    == nullptr);
	CHECK(findRef(res, "Scripts/Notes.lua")  == nullptr);
}

TEST_CASE("An asset's own META path is not a reference to itself")
{
	TempContentDir dir("he_test_refscan_selfmeta");

	writeAsset(dir.path, "Tex/Rock.hasset", HE::AssetType::Texture, kTexId,
	           { { HAsset::CHUNK_PIXL, { 1, 2, 3, 4 } } });
	writeAsset(dir.path, "Tex/Lonely.hasset", HE::AssetType::Texture, HE::UUID::generate(),
	           { { HAsset::CHUNK_PIXL, { 5, 6, 7, 8 } } });
	writeAsset(dir.path, "Mat.hasset", HE::AssetType::Material, HE::UUID::generate(),
	           { { HAsset::CHUNK_MTRL, materialChunk({ "Tex/Rock.hasset" }) } });

	// The asset really does spell its own path on disk (that is what the packer's
	// path→UUID map is built from), so the scan is looking straight at it.
	CHECK(fileContains(dir.path / "Tex" / "Rock.hasset", "Tex/Rock.hasset"));

	// NO excludeFiles on purpose: the CHUNK_META skip is what has to carry this,
	// not the caller's exclusion list. Without it every asset reports itself.
	HE::AssetRefs::ScanTargets targets;
	targets.paths.push_back("Tex/Rock.hasset");
	const HE::AssetRefs::ScanResult res =
		HE::AssetRefs::findReferrers(targets, requestFor(dir));

	CHECK(res.referrers.size() == 1);
	CHECK(findRef(res, "Mat.hasset")      != nullptr);
	CHECK(findRef(res, "Tex/Rock.hasset") == nullptr);

	// And an asset nothing points at reports nothing at all — its own META being
	// the only place in the tree its path appears.
	HE::AssetRefs::ScanTargets lonely;
	lonely.paths.push_back("Tex/Lonely.hasset");
	const HE::AssetRefs::ScanResult none =
		HE::AssetRefs::findReferrers(lonely, requestFor(dir));
	CHECK(none.referrers.empty());
	CHECK_FALSE(none.incomplete);
}

TEST_CASE("A path that is only a PREFIX of a stored reference is not a reference")
{
	TempContentDir dir("he_test_refscan_substring");

	writeAsset(dir.path, "Tex/Rock.hasset", HE::AssetType::Texture, kTexId,
	           { { HAsset::CHUNK_PIXL, { 9, 9, 9, 9 } } });
	writeAsset(dir.path, "Mat.hasset", HE::AssetType::Material, HE::UUID::generate(),
	           { { HAsset::CHUNK_MTRL, materialChunk({ "Tex/Rock.hasset" }) } });
	writeAsset(dir.path, "Hud.hasset", HE::AssetType::Widget, HE::UUID::generate(),
	           { { HAsset::CHUNK_UIWG, rawChunk(R"({"nodes":[{"id":1,"s":"Tex/Rock.hasset"}]})") } });

	// Sanity: the fixture really does hold the reference in both encodings.
	{
		HE::AssetRefs::ScanTargets whole;
		whole.paths.push_back("Tex/Rock.hasset");
		const HE::AssetRefs::ScanResult res =
			HE::AssetRefs::findReferrers(whole, requestFor(dir));
		CHECK(res.referrers.size() == 2);
	}

	// "Tex/Rock" is a different asset from "Tex/Rock.hasset" — the length prefix
	// (binary) and the whole-value compare (JSON) are what keep them apart.
	{
		HE::AssetRefs::ScanTargets partial;
		partial.paths.push_back("Tex/Rock");
		const HE::AssetRefs::ScanResult res =
			HE::AssetRefs::findReferrers(partial, requestFor(dir));
		CHECK(res.referrers.empty());
	}

	// Nor does the FOLDER form of the same string: a folder re-roots only what
	// continues with '/', and "Tex/Rock.hasset" continues with '.'.
	{
		HE::AssetRefs::ScanTargets folder;
		folder.pathPrefixes.push_back("Tex/Rock");
		const HE::AssetRefs::ScanResult res =
			HE::AssetRefs::findReferrers(folder, requestFor(dir));
		CHECK(res.referrers.empty());
	}
}

TEST_CASE("A folder query answers what breaks OUTSIDE the folder")
{
	TempContentDir dir("he_test_refscan_folder");

	writeAsset(dir.path, "Tex/Rock.hasset", HE::AssetType::Texture, kTexId,
	           { { HAsset::CHUNK_PIXL, { 1, 1, 1, 1 } } });
	// Inside the folder: goes away together with the target, so it is not breakage.
	writeAsset(dir.path, "Tex/Inner.hasset", HE::AssetType::Material, HE::UUID::generate(),
	           { { HAsset::CHUNK_MTRL, materialChunk({ "Tex/Rock.hasset" }) } });
	// Outside: this is the answer the dialog exists for.
	writeAsset(dir.path, "Mat.hasset", HE::AssetType::Material, HE::UUID::generate(),
	           { { HAsset::CHUNK_MTRL, materialChunk({ "Tex/Rock.hasset" }) } });
	// A folder whose name merely STARTS with the queried one.
	writeAsset(dir.path, "TexOther/Neighbour.hasset", HE::AssetType::Material, HE::UUID::generate(),
	           { { HAsset::CHUNK_MTRL, materialChunk({ "TexOther/Rock.hasset" }) } });

	HE::AssetRefs::ScanTargets targets;
	targets.pathPrefixes.push_back("Tex");

	HE::AssetRefs::ScanRequest req = requestFor(dir);
	req.excludeUnder.push_back((dir.path / "Tex").string());

	const HE::AssetRefs::ScanResult res = HE::AssetRefs::findReferrers(targets, req);

	CHECK(res.referrers.size() == 1);
	CHECK(findRef(res, "Mat.hasset")                 != nullptr);
	CHECK(findRef(res, "Tex/Inner.hasset")           == nullptr);
	CHECK(findRef(res, "TexOther/Neighbour.hasset")  == nullptr);
}

// ─── UUID references ─────────────────────────────────────────────────────────

TEST_CASE("A scene component's asset id is a reference; an entity id is not")
{
	TempContentDir dir("he_test_refscan_sceneuuid");

	writeAsset(dir.path, "Tex/Rock.hasset", HE::AssetType::Texture, kTexId,
	           { { HAsset::CHUNK_PIXL, { 4, 3, 2, 1 } } });

	// The reference: a component field addressing the asset by id.
	writeTextFile(dir.path / "Level.hescene",
		R"({"version":1,"entities":[{"uuid":[111,222],"name":"Cube","parent":null,"children":[],)"
		R"("components":{"transform":{"position":[0,0,0]},"mesh":{"asset":)" + uuidPair(kTexId) +
		R"(,"visible":true}}}]})");

	// The SAME [hi,lo] pair spelled at ENTITY level: identity, parent link, child
	// link. Using the same pair is what forces the file to be parsed at all — a
	// different id would be gated out by the needle scan and this would pass
	// without ever exercising the `components` guard.
	writeTextFile(dir.path / "Entities.hescene",
		R"({"version":1,"entities":[)"
		R"({"uuid":)" + uuidPair(kTexId) + R"(,"name":"Root","parent":null,"children":[)" +
		uuidPair(kTexId) + R"(],"components":{"transform":{"position":[0,0,0]}}},)"
		R"({"uuid":[333,444],"name":"Child","parent":)" + uuidPair(kTexId) +
		R"(,"children":[],"components":{}}]})");

	// An unset component reference serialises as [0,0], not as nothing.
	writeTextFile(dir.path / "Unset.hescene",
		R"({"version":1,"entities":[{"uuid":[555,666],"name":"Empty","parent":null,"children":[],)"
		R"("components":{"mesh":{"asset":[0,0]}}}]})");

	// The entity-level scene really does carry the id, so it is parsed rather than
	// skipped by the needle gate — otherwise the guard below is never exercised.
	CHECK(fileContains(dir.path / "Entities.hescene", std::to_string(kTexId.hi)));

	{
		HE::AssetRefs::ScanTargets targets;
		targets.uuids.push_back(kTexId);
		const HE::AssetRefs::ScanResult res =
			HE::AssetRefs::findReferrers(targets, requestFor(dir));

		CHECK(res.referrers.size() == 1);
		const HE::AssetRefs::Referrer* r = findRef(res, "Level.hescene");
		REQUIRE(r != nullptr);
		CHECK(r->kind == HE::AssetRefs::RefKind::Uuid);
		CHECK(findRef(res, "Entities.hescene") == nullptr);
		CHECK(findRef(res, "Unset.hescene")    == nullptr);
	}

	// A null id is ignored rather than matching every unset reference in the tree.
	{
		HE::AssetRefs::ScanTargets targets;
		targets.uuids.push_back(HE::UUID{});
		const HE::AssetRefs::ScanResult res =
			HE::AssetRefs::findReferrers(targets, requestFor(dir));
		CHECK(res.referrers.empty());
	}

	// An id past INT64_MAX — the common half of the id space, and the shape a
	// signed read would drop on the floor.
	{
		writeTextFile(dir.path / "Big.hescene",
			R"({"version":1,"entities":[{"uuid":[777,888],"name":"Huge","parent":null,"children":[],)"
			R"("components":{"mesh":{"asset":)" + uuidPair(kBigId) + R"(}}}]})");
		// The graph-chunk spelling of the same id: an object, not a pair.
		writeAsset(dir.path, "Anim.hasset", HE::AssetType::AnimatorStateMachine, HE::UUID::generate(),
		           { { HAsset::CHUNK_ASMG, rawChunk(
		                 R"({"states":[{"name":"Idle","clip":{"hi":)" + std::to_string(kBigId.hi) +
		                 R"(,"lo":)" + std::to_string(kBigId.lo) + R"(}}],"transitions":[]})") } });

		HE::AssetRefs::ScanTargets targets;
		targets.uuids.push_back(kBigId);
		const HE::AssetRefs::ScanResult res =
			HE::AssetRefs::findReferrers(targets, requestFor(dir));

		CHECK(res.referrers.size() == 2);
		const HE::AssetRefs::Referrer* scene = findRef(res, "Big.hescene");
		REQUIRE(scene != nullptr);
		CHECK(scene->kind == HE::AssetRefs::RefKind::Uuid);
		const HE::AssetRefs::Referrer* graph = findRef(res, "Anim.hasset");
		REQUIRE(graph != nullptr);
		CHECK(graph->kind == HE::AssetRefs::RefKind::Uuid);
	}
}

TEST_CASE("A scene names another scene inside its level script")
{
	TempContentDir dir("he_test_refscan_levelscript");

	writeTextFile(dir.path / "Levels" / "Other.hescene",
	              R"({"version":1,"entities":[]})");
	writeTextFile(dir.path / "Main.hescene",
		R"({"version":1,"entities":[],"levelScript":{"nextId":2,)"
		R"("nodes":[{"id":1,"type":"OpenLevel","s":"Levels/Other.hescene"}],"links":[],"variables":[]}})");

	HE::AssetRefs::ScanTargets targets;
	targets.paths.push_back("Levels/Other.hescene");

	HE::AssetRefs::ScanRequest req = requestFor(dir);
	req.excludeFiles.push_back((dir.path / "Levels" / "Other.hescene").string());

	const HE::AssetRefs::ScanResult res = HE::AssetRefs::findReferrers(targets, req);

	CHECK(res.referrers.size() == 1);
	const HE::AssetRefs::Referrer* r = findRef(res, "Main.hescene");
	REQUIRE(r != nullptr);
	CHECK(r->kind == HE::AssetRefs::RefKind::Path);
}

// ─── The project-relative form and the project root ──────────────────────────

TEST_CASE("The project-relative form matches only when the content directory is named")
{
	TempContentDir dir("he_test_refscan_projrel");

	writeTextFile(dir.path / "Level.hescene", R"({"version":1,"entities":[]})");
	// Scene references are spelled project-relative ("Content/Level.hescene").
	writeAsset(dir.path, "Logic.hasset", HE::AssetType::HorizonCodeClass, HE::UUID::generate(),
	           { { HAsset::CHUNK_HCGR, rawChunk(
	                 R"({"nextId":2,"nodes":[{"id":1,"type":"OpenLevel","s":"Content/Level.hescene"}],)"
	                 R"("links":[],"variables":[]})") } });

	HE::AssetRefs::ScanTargets targets;
	targets.paths.push_back("Level.hescene");

	{
		HE::AssetRefs::ScanRequest req = requestFor(dir);   // contentDirName = "Content"
		const HE::AssetRefs::ScanResult res = HE::AssetRefs::findReferrers(targets, req);
		CHECK(res.referrers.size() == 1);
		CHECK(findRef(res, "Logic.hasset") != nullptr);
	}
	{
		HE::AssetRefs::ScanRequest req = requestFor(dir);
		req.contentDirName.clear();   // the extra rule form is not generated
		const HE::AssetRefs::ScanResult res = HE::AssetRefs::findReferrers(targets, req);
		CHECK(res.referrers.empty());
	}
}

TEST_CASE("The project manifest's startup scene is a reference")
{
	TempContentDir dir("he_test_refscan_heproj");

	writeTextFile(dir.path / "StartupScene.hescene", R"({"version":1,"entities":[]})");
	writeTextFile(fs::path(dir.projectRoot()) / "Demo.heproj",
		R"({"name":"Demo","engineVersion":"1.0","startupScene":"Content/StartupScene.hescene",)"
		R"("scriptLanguage":"HorizonCode"})");
	// A project-root GameInstance graph: beside Content/, so the tree walk never
	// sees it — the manifest pass is the only thing that does.
	writeTextFile(fs::path(dir.projectRoot()) / "GameInstance.hcode",
		R"({"nextId":2,"nodes":[{"id":1,"type":"OpenLevel","s":"Content/StartupScene.hescene"}],)"
		R"("links":[],"variables":[]})");

	HE::AssetRefs::ScanTargets targets;
	targets.paths.push_back("StartupScene.hescene");

	{
		HE::AssetRefs::ScanRequest req = requestFor(dir);
		req.excludeFiles.push_back((dir.path / "StartupScene.hescene").string());
		const HE::AssetRefs::ScanResult res = HE::AssetRefs::findReferrers(targets, req);

		CHECK(res.referrers.size() == 2);
		const HE::AssetRefs::Referrer* proj = findRef(res, "Demo.heproj");
		REQUIRE(proj != nullptr);
		CHECK(proj->kind == HE::AssetRefs::RefKind::Path);
		// Outside the content root, so the display name falls back to the
		// project-relative spelling rather than a bare filename by accident.
		CHECK(findRef(res, "GameInstance.hcode") != nullptr);
	}

	// Without a project root there is nothing to walk beside Content/.
	{
		HE::AssetRefs::ScanRequest req = requestFor(dir);
		req.projectRoot.clear();
		req.excludeFiles.push_back((dir.path / "StartupScene.hescene").string());
		const HE::AssetRefs::ScanResult res = HE::AssetRefs::findReferrers(targets, req);
		CHECK(res.referrers.empty());
	}
}

// ─── Limits and the id-of-a-file helper ──────────────────────────────────────

TEST_CASE("maxReferrers truncates the list and says the answer is a sample")
{
	TempContentDir dir("he_test_refscan_truncate");

	writeAsset(dir.path, "Tex/Rock.hasset", HE::AssetType::Texture, kTexId,
	           { { HAsset::CHUNK_PIXL, { 0, 0, 0, 0 } } });
	for (const char* name : { "MatA.hasset", "MatB.hasset", "MatC.hasset" })
		writeAsset(dir.path, name, HE::AssetType::Material, HE::UUID::generate(),
		           { { HAsset::CHUNK_MTRL, materialChunk({ "Tex/Rock.hasset" }) } });

	HE::AssetRefs::ScanTargets targets;
	targets.paths.push_back("Tex/Rock.hasset");

	{
		HE::AssetRefs::ScanRequest req = requestFor(dir);
		req.maxReferrers = 2;
		const HE::AssetRefs::ScanResult res = HE::AssetRefs::findReferrers(targets, req);
		// WHICH two is directory-order dependent; that there are exactly two and
		// that the caller is told more exist is the contract.
		CHECK(res.referrers.size() == 2);
		CHECK(res.truncated);
	}
	{
		HE::AssetRefs::ScanRequest req = requestFor(dir);
		req.maxReferrers = 10;
		const HE::AssetRefs::ScanResult res = HE::AssetRefs::findReferrers(targets, req);
		CHECK(res.referrers.size() == 3);
		CHECK_FALSE(res.truncated);
	}
}

TEST_CASE("assetUuidOfFile round-trips the META id and refuses everything else")
{
	TempContentDir dir("he_test_refscan_uuidoffile");

	writeAsset(dir.path, "Tex/Rock.hasset", HE::AssetType::Texture, kTexId,
	           { { HAsset::CHUNK_PIXL, { 7, 7, 7, 7 } } });
	CHECK(HE::AssetRefs::assetUuidOfFile((dir.path / "Tex" / "Rock.hasset").string()) == kTexId);

	// The real call site (containedAssetUuids) hands it .hescene files too, and a
	// saved scene is JSON, not an .hasset: a null id, not garbage.
	writeTextFile(dir.path / "Level.hescene", R"({"version":1,"entities":[]})");
	CHECK(HE::AssetRefs::assetUuidOfFile((dir.path / "Level.hescene").string()) == HE::UUID{});

	writeTextFile(dir.path / "notes.txt", "not an asset");
	CHECK(HE::AssetRefs::assetUuidOfFile((dir.path / "notes.txt").string()) == HE::UUID{});

	// Truncated below the 32-byte file header.
	writeTextFile(dir.path / "stub.hasset", "HAST");
	CHECK(HE::AssetRefs::assetUuidOfFile((dir.path / "stub.hasset").string()) == HE::UUID{});

	CHECK(HE::AssetRefs::assetUuidOfFile((dir.path / "missing.hasset").string()) == HE::UUID{});
}

// ─── A documented limitation ─────────────────────────────────────────────────

TEST_CASE("LIMITATION: a pack-time baked POD uuid in a binary chunk is invisible")
{
	TempContentDir dir("he_test_refscan_bakeduuid");

	writeAsset(dir.path, "Mat.hasset", HE::AssetType::Material, kMatId,
	           { { HAsset::CHUNK_MTRL, materialChunk({}) } });

	// A COOKED mesh: its material reference has been baked into MRFU as a raw
	// 16-byte HE::UUID and the MREF path is gone.
	std::vector<uint8_t> mrfu;
	HAsset::Writer::appendPOD(mrfu, kMatId.hi);
	HAsset::Writer::appendPOD(mrfu, kMatId.lo);
	writeAsset(dir.path, "Cube.hasset", HE::AssetType::StaticMesh, HE::UUID::generate(),
	           { { HAsset::CHUNK_MRFU, mrfu } });

	// The same id in its JSON spelling, to prove the target itself is findable.
	writeTextFile(dir.path / "Level.hescene",
		R"({"version":1,"entities":[{"uuid":[111,222],"name":"Cube","parent":null,"children":[],)"
		R"("components":{"mesh":{"material":)" + uuidPair(kMatId) + R"(}}}]})");

	HE::AssetRefs::ScanTargets targets;
	targets.uuids.push_back(kMatId);
	const HE::AssetRefs::ScanResult res =
		HE::AssetRefs::findReferrers(targets, requestFor(dir));

	CHECK(findRef(res, "Level.hescene") != nullptr);

	// The gate needle for a uuid is the DECIMAL text of its `hi` (compile() in
	// AssetRefScan.cpp), which the 16 raw bytes do not contain — so a baked
	// reference reports nothing. Harmless today: baking happens at pack time, and
	// loose editor assets always keep the path form alongside. It would stop being
	// harmless the moment cooked assets live in the content tree.
	CHECK(findRef(res, "Cube.hasset") == nullptr);
	CHECK(fileContains(dir.path / "Cube.hasset", std::string(1, static_cast<char>(kMatId.hi & 0xFF))));
	CHECK_FALSE(fileContains(dir.path / "Cube.hasset", std::to_string(kMatId.hi)));
}

// ─── What the first review pass got wrong ────────────────────────────────────
// Each case below pins a behaviour the scan shipped INCORRECTLY in its first
// version. They are the cheapest possible insurance: every one of the three was
// a plausible-looking implementation that a refactor could plausibly restore.

TEST_CASE("a folder target matches what is under it, never the bare folder name")
{
	TempContentDir dir("he_test_refscan_foldername");

	// The trap: a folder called "Player" and a scene with an entity of the same
	// name. The first version compared stored values against the folder path with
	// an EXACT match as well as a prefix one, so every scene holding an entity,
	// variable or label spelled like the folder was reported as referencing the
	// entire subtree — on the dialog that deletes it.
	writeAsset(dir.path, "Player/Skin.hasset", HE::AssetType::Texture, kTexId);
	writeTextFile(dir.path / "Level.hescene",
		R"({"version":1,"entities":[{"uuid":[111,222],"name":"Player","parent":null,)"
		R"("children":[],"components":{"transform":{"position":[0,0,0]}}}]})");
	// A real reference into the folder, so the case is not vacuously empty.
	writeAsset(dir.path, "Mats/Skin.hasset", HE::AssetType::Material, kMatId,
	           { { HAsset::CHUNK_MTRL, materialChunk({ "Player/Skin.hasset" }) } });

	HE::AssetRefs::ScanTargets targets;
	targets.pathPrefixes.push_back("Player");
	const HE::AssetRefs::ScanResult res =
		HE::AssetRefs::findReferrers(targets, requestFor(dir));

	CHECK(findRef(res, "Mats/Skin.hasset") != nullptr);
	CHECK(findRef(res, "Level.hescene") == nullptr);
	// Not vacuous: the scene really does spell the folder's name.
	CHECK(fileContains(dir.path / "Level.hescene", "\"Player\""));
	CHECK(res.referrers.size() == 1);
}

TEST_CASE("a candidate that cannot be read is reported as unchecked, never as unreferenced")
{
	TempContentDir dir("he_test_refscan_unreadable");

	// A file that mentions the target but is not a valid .hasset — truncated by a
	// crash, or caught mid-write. The first version answered "holds no reference"
	// for it, with `incomplete` left false, so the dialog stated flatly that
	// nothing referenced an asset it had never managed to parse.
	writeAsset(dir.path, "Mats/Good.hasset", HE::AssetType::Material, kMatId,
	           { { HAsset::CHUNK_MTRL, materialChunk({ "Tex/Rock.hasset" }) } });

	std::vector<uint8_t> truncated;
	{
		HAsset::Writer w;
		const std::vector<uint8_t> mtrl = materialChunk({ "Tex/Rock.hasset" });
		w.addChunk(HAsset::CHUNK_MTRL, mtrl.data(), mtrl.size());
		// A trailing chunk to cut into, so the reference text itself SURVIVES the
		// truncation — otherwise the file no longer mentions the target at all and
		// the case would pass for the wrong reason (nothing to check, rather than
		// something that could not be checked).
		const std::vector<uint8_t> filler(64, 0x5A);
		w.addChunk(HAsset::CHUNK_PIXL, filler.data(), filler.size());
		truncated = w.toBytes(static_cast<uint16_t>(HE::AssetType::Material));
	}
	REQUIRE(truncated.size() > 96);
	truncated.resize(truncated.size() - 32);   // the last chunk now overruns the file
	{
		fs::create_directories(dir.path / "Mats");
		std::ofstream f(dir.path / "Mats" / "Torn.hasset", std::ios::binary | std::ios::trunc);
		REQUIRE(f.is_open());
		f.write(reinterpret_cast<const char*>(truncated.data()),
		        static_cast<std::streamsize>(truncated.size()));
	}

	HE::AssetRefs::ScanTargets targets;
	targets.paths.push_back("Tex/Rock.hasset");
	const HE::AssetRefs::ScanResult res =
		HE::AssetRefs::findReferrers(targets, requestFor(dir));

	CHECK(findRef(res, "Mats/Good.hasset") != nullptr);
	// The whole point: the answer admits it is a lower bound.
	CHECK(res.incomplete);
	CHECK(fileContains(dir.path / "Mats" / "Torn.hasset", "Tex/Rock.hasset"));
}

TEST_CASE("a cancelled scan stops early and says so")
{
	TempContentDir dir("he_test_refscan_cancel");

	for (int i = 0; i < 12; ++i)
		writeAsset(dir.path, "Mats/M" + std::to_string(i) + ".hasset",
		           HE::AssetType::Material, HE::UUID::generate(),
		           { { HAsset::CHUNK_MTRL, materialChunk({ "Tex/Rock.hasset" }) } });

	HE::AssetRefs::ScanTargets targets;
	targets.paths.push_back("Tex/Rock.hasset");

	SUBCASE("cancelled from the first poll")
	{
		HE::AssetRefs::ScanRequest req = requestFor(dir);
		req.isCancelled = []{ return true; };
		const HE::AssetRefs::ScanResult res = HE::AssetRefs::findReferrers(targets, req);
		CHECK(res.referrers.empty());
		// An empty list from a cancelled walk must never read as "nothing
		// references it" — that is the one sentence a delete dialog must earn.
		CHECK(res.incomplete);
	}
	SUBCASE("not cancelled — the same tree answers in full")
	{
		const HE::AssetRefs::ScanResult res =
			HE::AssetRefs::findReferrers(targets, requestFor(dir));
		CHECK(res.referrers.size() == 12);
		CHECK_FALSE(res.incomplete);
	}
}

TEST_CASE("assetUuidOfFile separates 'has no id' from 'could not be read'")
{
	TempContentDir dir("he_test_refscan_uuid_tristate");

	// Both answers are a null UUID, and the difference decides whether a folder
	// delete quietly stops looking for a target. A scene addresses meshes and
	// materials by id ALONE, so a dropped id turns a scene full of live
	// references into "Nothing else references it".
	writeAsset(dir.path, "Mesh.hasset", HE::AssetType::StaticMesh, kTexId);
	writeTextFile(dir.path / "Level.hescene", R"({"version":1,"entities":[]})");

	SUBCASE("a real asset yields its id and reports no trouble")
	{
		bool unreadable = true;   // must be cleared, not merely left alone
		CHECK(HE::AssetRefs::assetUuidOfFile((dir.path / "Mesh.hasset").string(), &unreadable) == kTexId);
		CHECK_FALSE(unreadable);
	}
	SUBCASE("a file that legitimately carries no id is not a failure")
	{
		// The folder collector feeds .hescene files in on purpose; answering
		// "unreadable" for every scene would make every folder delete claim it
		// could not be checked, which trains the user to ignore the warning.
		bool unreadable = false;
		CHECK(HE::AssetRefs::assetUuidOfFile((dir.path / "Level.hescene").string(), &unreadable) == HE::UUID{});
		CHECK_FALSE(unreadable);
	}
	SUBCASE("a file that cannot be read says so")
	{
		bool unreadable = false;
		CHECK(HE::AssetRefs::assetUuidOfFile((dir.path / "Nope.hasset").string(), &unreadable) == HE::UUID{});
		CHECK(unreadable);

		// Truncated mid-header: an .hasset by its magic, unusable in fact.
		writeTextFile(dir.path / "Torn.hasset", "HAST\x02\x00");
		unreadable = false;
		CHECK(HE::AssetRefs::assetUuidOfFile((dir.path / "Torn.hasset").string(), &unreadable) == HE::UUID{});
		CHECK(unreadable);
	}
	SUBCASE("a corrupt chunk size is rejected instead of allocated")
	{
		// A bit flip in a chunk header used to be handed straight to
		// std::vector's constructor: the allocation throws out of the worker, and
		// the dialog spins on "Checking what references it" forever because the
		// result is never published.
		std::vector<uint8_t> bytes;
		{
			std::ifstream f(dir.path / "Mesh.hasset", std::ios::binary);
			bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		}
		REQUIRE(bytes.size() > sizeof(HAsset::FileHeader) + sizeof(HAsset::ChunkHeader));
		const std::uint64_t huge = 0x0000400000000000ull;   // 64 TB, plausible as a bit flip
		std::memcpy(bytes.data() + sizeof(HAsset::FileHeader) + sizeof(uint32_t), &huge, sizeof(huge));
		{
			std::ofstream f(dir.path / "Bomb.hasset", std::ios::binary | std::ios::trunc);
			REQUIRE(f.is_open());
			f.write(reinterpret_cast<const char*>(bytes.data()),
			        static_cast<std::streamsize>(bytes.size()));
		}

		bool unreadable = false;
		CHECK(HE::AssetRefs::assetUuidOfFile((dir.path / "Bomb.hasset").string(), &unreadable) == HE::UUID{});
		CHECK(unreadable);
	}
}
