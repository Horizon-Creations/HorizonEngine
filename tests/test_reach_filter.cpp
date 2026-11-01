// "Pack only what the game reaches" (Hpak/ReachFilter.h, PackSettings::onlyReferenced).
//
// What the tests pin: reachability is TRANSITIVE FROM A ROOT. A mesh that names a material is
// no reason to keep either when nothing reachable names the mesh; a material both an unreached
// and a reached mesh name stays (the reached one needs it). Every spelling of a reference is
// found (path, UUID as raw bytes either endianness, UUID as JSON decimal pair and object),
// scenes are nodes of the same graph, and the engine's by-type and implicit roots survive.

#include "doctest.h"
#include "TestFsUtil.h"
#include <Hpak/HpakWriter.h>
#include <Hpak/HpakFormat.h>
#include <Hpak/ReachFilter.h>
#include <Hpak/PackManifest.h>
#include <Hpak/ProjectExporter.h>
#include <Hpak/HpakReader.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace
{

std::vector<uint8_t> bytesOf(const std::string& s) { return { s.begin(), s.end() }; }

std::string jsonPair(const HE::UUID& id)
{
	return "[" + std::to_string(id.hi) + ", " + std::to_string(id.lo) + "]";
}
std::string jsonObject(const HE::UUID& id)
{
	return "{\"hi\":" + std::to_string(id.hi) + ",\"lo\":" + std::to_string(id.lo) + "}";
}
std::vector<uint8_t> beBytes(const HE::UUID& id)
{
	std::vector<uint8_t> b(16);
	for (int i = 0; i < 8; ++i) { b[i] = uint8_t(id.hi >> (56 - 8 * i)); b[8 + i] = uint8_t(id.lo >> (56 - 8 * i)); }
	return b;
}

struct Project
{
	std::filesystem::path dir;
	HE::UUID texA, matA, meshA, texB, matB, meshB, meshC, texD, matD;

	explicit Project(const char* tag)
	{
		dir = std::filesystem::temp_directory_path() / (std::string("he_reach_") + tag);
		he_test::removeAllQuiet(dir);
		std::filesystem::create_directories(dir);
		ContentManager cm(dir.string());
		auto tex = [&](const char* name, HE::UUID& id) {
			TextureAsset t; t.type = HE::AssetType::Texture; t.name = name; t.path = std::string(name) + ".hasset";
			t.width = 2; t.height = 2; t.channels = 4; t.data = std::vector<uint8_t>(16, 0x55);
			REQUIRE(cm.saveAsset(t)); id = t.id;
		};
		auto mat = [&](const char* name, const char* texPath, HE::UUID& id) {
			MaterialAsset m; m.type = HE::AssetType::Material; m.name = name; m.path = std::string(name) + ".hasset";
			if (texPath) m.texturePaths = { texPath };
			REQUIRE(cm.saveAsset(m)); id = m.id;
		};
		auto mesh = [&](const char* name, const char* matPath, HE::UUID& id) {
			StaticMeshAsset s; s.type = HE::AssetType::StaticMesh; s.name = name; s.path = std::string(name) + ".hasset";
			s.materialPath = matPath; s.vertices = { 0,0,0, 1,0,0, 0,1,0 }; s.indices = { 0,1,2 };
			REQUIRE(cm.saveAsset(s)); id = s.id;
		};
		tex("texA", texA);  mat("matA", "texA.hasset", matA);  mesh("meshA", "matA.hasset", meshA);
		tex("texB", texB);  mat("matB", "texB.hasset", matB);  mesh("meshB", "matB.hasset", meshB);
		mesh("meshC", "matA.hasset", meshC);                   // an unreached mesh on a reached material
		tex("texD", texD);  mat("matD", "texD.hasset", matD);  // D is named by nothing but what a test adds
	}
	~Project() { he_test::removeAllQuiet(dir); }
};

std::set<std::string> pack(const Project& p, Hpak::PackSettings s, HpakWriter* out = nullptr)
{
	s.codec = Hpak::Codec::Store;
	HpakWriter local;
	HpakWriter& w = out ? *out : local;
	w.addDirectory(p.dir, s);
	std::set<std::string> names;
	for (const auto& [path, id] : w.packedPaths()) names.insert(path);
	return names;
}

} // namespace

TEST_CASE("Reach: a mesh in the startup scene keeps its material and texture, an island is left out")
{
	Project p("island");
	Hpak::PackSettings s;
	s.onlyReferenced = true;
	s.referenceRoots.push_back(bytesOf("{\"entities\":[{\"mesh\":" + jsonPair(p.meshA) + "}]}"));
	const auto kept = pack(p, s);
	CHECK(kept.count("meshA.hasset")); CHECK(kept.count("matA.hasset")); CHECK(kept.count("texA.hasset"));
	// B names a material and a texture, but nothing reachable names B.
	CHECK_FALSE(kept.count("meshB.hasset")); CHECK_FALSE(kept.count("matB.hasset")); CHECK_FALSE(kept.count("texB.hasset"));
	// C names a reached material: that is no reason to keep C.
	CHECK_FALSE(kept.count("meshC.hasset"));
	CHECK_FALSE(kept.count("matD.hasset")); CHECK_FALSE(kept.count("texD.hasset"));

	// Off: everything, as before.
	Hpak::PackSettings off;
	CHECK(pack(p, off).size() == 9);
}

TEST_CASE("Reach: every spelling of a reference is found")
{
	Project p("spellings");
	auto keptWith = [&](std::vector<uint8_t> root) {
		Hpak::PackSettings s; s.onlyReferenced = true; s.referenceRoots.push_back(std::move(root));
		return pack(p, s);
	};
	// JSON object {"hi":…,"lo":…}
	CHECK(keptWith(bytesOf("x " + jsonObject(p.meshB) + " y")).count("meshB.hasset"));
	// raw big-endian (a CBOR scene or prefab), in the middle of other bytes
	{
		std::vector<uint8_t> blob = { 0x9f, 0x1b };
		const auto be = beBytes(p.meshB); blob.insert(blob.end(), be.begin(), be.end());
		blob.push_back(0xff);
		CHECK(keptWith(blob).count("meshB.hasset"));
	}
	// CBOR array of shortest-form ints: a random id is 1b + 8 bytes each, an engine id is tiny
	{
		std::vector<uint8_t> blob = { 0x82, 0x1b };
		for (int i = 0; i < 8; ++i) blob.push_back(uint8_t(p.meshB.hi >> (56 - 8 * i)));
		blob.push_back(0x1b);
		for (int i = 0; i < 8; ++i) blob.push_back(uint8_t(p.meshB.lo >> (56 - 8 * i)));
		CHECK(keptWith(blob).count("meshB.hasset"));
	}
	// raw little-endian (a chunk)
	{
		std::vector<uint8_t> blob(3, 0);
		uint8_t le[16]; std::memcpy(le, &p.meshB.hi, 8); std::memcpy(le + 8, &p.meshB.lo, 8);
		blob.insert(blob.end(), le, le + 16);
		CHECK(keptWith(blob).count("meshB.hasset"));
	}
	// a path, with and without the Content/ prefix, behind a stray length byte
	CHECK(keptWith(bytesOf(std::string("\x0e") + "meshB.hasset")).count("meshB.hasset"));
	CHECK(keptWith(bytesOf("{\"mesh\":\"Content/meshB.hasset\"}")).count("meshB.hasset"));
	// and a path that names nothing here keeps nothing
	CHECK(keptWith(bytesOf("{\"mesh\":\"Content/other.hasset\"}")).empty());
}

TEST_CASE("Reach: scenes are nodes: one nothing loads is left out, one a reached scene loads stays")
{
	Project p("scenes");
	Hpak::PackSettings s;
	s.onlyReferenced = true;
	s.referenceRoots.push_back(bytesOf("{\"levelScript\":\"loads Content/Level2.hescene\"}"));
	s.reachScenes.push_back({ "Content/Level2.hescene", bytesOf("{\"mesh\":" + jsonPair(p.meshB) + "}") });
	s.reachScenes.push_back({ "Content/Level3.hescene", bytesOf("{\"mesh\":" + jsonPair(p.meshA) + "}") });
	HpakWriter w;
	const auto kept = pack(p, s, &w);
	REQUIRE(w.unreachableScenes().size() == 1);
	CHECK(w.unreachableScenes()[0] == "Content/Level3.hescene");
	CHECK(kept.count("meshB.hasset"));             // Level2 is loaded, it places B
	CHECK_FALSE(kept.count("meshA.hasset"));       // only Level3 places A, and nothing loads Level3
	CHECK(w.unreferencedCount() == static_cast<int>(9 - kept.size()));
	CHECK(w.unreferencedBytes() > 0);
}

TEST_CASE("Reach: roots by kind, engine folder, and always-include pattern survive")
{
	Project p("roots");
	{
		ContentManager cm(p.dir.string());
		InputActionAsset a; a.type = HE::AssetType::InputAction; a.name = "Jump"; a.path = "Input/Jump.hasset"; a.json = "{}";
		REQUIRE(cm.saveAsset(a));
		MaterialAsset m; m.type = HE::AssetType::Material; m.name = "keep_dynamic"; m.path = "keep_dynamic.hasset";
		REQUIRE(cm.saveAsset(m));
	}
	std::filesystem::path eng = std::filesystem::temp_directory_path() / "he_reach_roots_engine";
	he_test::removeAllQuiet(eng);
	std::filesystem::create_directories(eng);
	{
		ContentManager cm(eng.string());
		MaterialAsset w; w.type = HE::AssetType::Material; w.name = "Rain"; w.path = "Audio/Weather/Rain.hasset";
		REQUIRE(cm.saveAsset(w));
		MaterialAsset o; o.type = HE::AssetType::Material; o.name = "Other"; o.path = "Textures/Other.hasset";
		REQUIRE(cm.saveAsset(o));
	}
	Hpak::PackSettings s;
	s.codec = Hpak::Codec::Store;
	s.onlyReferenced = true;
	s.alwaysIncludePatterns = { "keep_*.hasset" };
	HpakWriter w;
	w.addDirectories({ { p.dir, {} }, { eng, "Engine/" } }, s);
	std::set<std::string> kept;
	for (const auto& [path, id] : w.packedPaths()) kept.insert(path);
	CHECK(kept.count("Input/Jump.hasset"));                    // found by type, nothing names it
	CHECK(kept.count("keep_dynamic.hasset"));                  // the author's pattern
	CHECK(kept.count("Engine/Audio/Weather/Rain.hasset"));     // the weather system loads it itself
	CHECK_FALSE(kept.count("Engine/Textures/Other.hasset"));   // an engine asset nothing names
	CHECK_FALSE(kept.count("meshA.hasset"));
	he_test::removeAllQuiet(eng);
}

TEST_CASE("Reach: the pack fingerprint tells filtered and unfiltered paks apart")
{
	Hpak::PackSettings a, b;
	b.onlyReferenced = true;
	CHECK(HE::packSettingsFingerprint(a) != HE::packSettingsFingerprint(b));
}

TEST_CASE("Reach: the exporter roots the filter at the startup scene, leaves out a scene nothing loads")
{
	Project p("exporter");
	const auto out = std::filesystem::temp_directory_path() / "he_reach_exporter_out";
	he_test::removeAllQuiet(out);

	const std::vector<uint8_t> startup = bytesOf("{\"entities\":[{\"mesh\":" + jsonPair(p.meshA) + "}]}");
	const std::vector<uint8_t> level2  = bytesOf("{\"entities\":[{\"mesh\":" + jsonPair(p.meshB) + "}]}");
	std::vector<std::pair<std::string, std::vector<uint8_t>>> scenes = {
		{ "Content/Startup.hescene", startup },     // the startup scene's own entry stays
		{ "Content/Level2.hescene", level2 },       // nothing loads it
	};
	std::string notice;
	ExportSettings settings;
	settings.compress = false;
	settings.onlyReferenced = true;
	settings.onNotice = [&](const std::string& m) { notice = m; };
	const auto res = ProjectExporter::exportProject(p.dir, "Reach", "", out, settings, startup, scenes);
	REQUIRE(res.success);

	HpakReader reader;
	REQUIRE(reader.open((out / "Reach.hpak").string()));
	CHECK(reader.hasEntry(p.meshA)); CHECK(reader.hasEntry(p.matA)); CHECK(reader.hasEntry(p.texA));
	CHECK_FALSE(reader.hasEntry(p.meshB)); CHECK_FALSE(reader.hasEntry(p.matB)); CHECK_FALSE(reader.hasEntry(p.meshC));
	CHECK(reader.hasEntry(sceneUuidForPath("Content/Startup.hescene")));
	CHECK_FALSE(reader.hasEntry(sceneUuidForPath("Content/Level2.hescene")));
	CHECK(notice.find("asset(s)") != std::string::npos);

	// The same export with the filter off packs the whole folder and both scenes.
	const auto out2 = std::filesystem::temp_directory_path() / "he_reach_exporter_out2";
	he_test::removeAllQuiet(out2);
	settings.onlyReferenced = false;
	REQUIRE(ProjectExporter::exportProject(p.dir, "Reach", "", out2, settings, startup, scenes).success);
	HpakReader all;
	REQUIRE(all.open((out2 / "Reach.hpak").string()));
	CHECK(all.hasEntry(p.meshB));
	CHECK(all.hasEntry(sceneUuidForPath("Content/Level2.hescene")));
	he_test::removeAllQuiet(out); he_test::removeAllQuiet(out2);
}

TEST_CASE("Reach: an engine asset with a small id is found in a CBOR scene")
{
	// {0x412, 1} (the Mountains landscape material) is 82 19 04 12 01 in a CBOR scene, which
	// no 16-byte pattern contains. Found on a real project: the three landscape arrays,
	// 344 MB of the pak, were left out until this form was scanned.
	std::filesystem::path dir = std::filesystem::temp_directory_path() / "he_reach_cbor_small";
	he_test::removeAllQuiet(dir);
	std::filesystem::create_directories(dir);
	HE::UUID small{ 0x412, 1 }, tiny{ 0x2A, 0x7 }, mid{ 0x123456, 0x9 };
	{
		ContentManager cm(dir.string());
		for (const HE::UUID& id : { small, tiny, mid })
		{
			MaterialAsset m; m.type = HE::AssetType::Material; m.id = id;
			m.name = "m" + std::to_string(id.hi); m.path = m.name + ".hasset";
			REQUIRE(cm.saveAsset(m));
		}
	}
	Hpak::PackSettings s; s.codec = Hpak::Codec::Store; s.onlyReferenced = true;
	// 0x412 = 19 04 12, 0x2A = 18 2A, 0x123456 = 1a 00 12 34 56, lo 1 = 01, 7 = 07, 9 = 09
	s.referenceRoots.push_back({ 0x9f, 0x82, 0x19, 0x04, 0x12, 0x01, 0x82, 0x18, 0x2A, 0x07,
	                             0x82, 0x1a, 0x00, 0x12, 0x34, 0x56, 0x09, 0xff });
	HpakWriter w;
	w.addDirectory(dir, s);
	CHECK(w.packedPaths().count("m1042.hasset"));
	CHECK(w.packedPaths().count("m42.hasset"));
	CHECK(w.packedPaths().count("m1193046.hasset"));
	he_test::removeAllQuiet(dir);
}
