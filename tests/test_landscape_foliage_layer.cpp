#include "doctest.h"

#include "AssetStubWriter.h"      // writeAssetStub — what the Content Browser makes
#include "TestFsUtil.h"

#include <ContentManager/AssetRefScan.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>
#include <LandscapeFoliage/LandscapeFoliageLayerJson.h>

#include <filesystem>
#include <string>
#include <vector>

// ── LandscapeFoliageLayer: the asset type (Thema 327, step 2) ────────────────
// A rule set, not a spawner: these tests pin the type, its JSON form, the
// load/save path through ContentManager and the validation. Nothing here scatters
// foliage — applying a set is a later step.

namespace fs = std::filesystem;

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
	};

	// Full 64-bit halves on purpose: a writer that squeezes ids through double
	// loses the low bits of both.
	const HE::UUID kRockMesh { 0x4B0123456789ABCDull, 0xFEDCBA9876543210ull };
	const HE::UUID kRockLod  { 0x4B0123456789ABCEull, 0xFEDCBA9876543211ull };
	const HE::UUID kTuftMesh { 0x00000000000004B1ull, 0x0000000000000001ull };
	const HE::UUID kMoss     { 0x7000000000000000ull, 0x8000000000000000ull };

	LandscapeFoliageLayerAsset sample()
	{
		LandscapeFoliageLayerAsset s;
		s.description  = "Forest floor";
		s.densityScale = 0.75f;

		LandscapeFoliageEntry rock;
		rock.name            = "Mossy Rock";
		rock.meshAssetId     = kRockMesh;
		rock.materialAssetId = kMoss;
		rock.lods            = { { kRockLod, kMoss, 25.0f, 0.5f }, { kRockLod, {}, 60.0f, 0.25f } };
		rock.paintLayer      = 2;
		rock.slopeMin        = 0.1f;
		rock.slopeMax        = 0.9f;
		rock.heightMin       = -5.0f;
		rock.heightMax       = 120.5f;
		rock.density         = 0.03f;
		rock.minScale        = 0.5f;
		rock.maxScale        = 2.5f;
		rock.drawDistance    = 150.0f;
		rock.bucketSize      = 64.0f;
		rock.castsShadow     = false;
		rock.contributesAO   = false;
		rock.shadowDistance  = 40.0f;
		rock.lodHysteresis   = 4.0f;
		s.entries.push_back(rock);

		LandscapeFoliageEntry tuft;   // everything else at its default
		tuft.name         = "Tuft";
		tuft.meshAssetId  = kTuftMesh;
		s.entries.push_back(tuft);
		return s;
	}

	void checkSame(const LandscapeFoliageLayerAsset& a, const LandscapeFoliageLayerAsset& b)
	{
		CHECK(a.description  == b.description);
		CHECK(a.densityScale == b.densityScale);
		REQUIRE(a.entries.size() == b.entries.size());
		for (size_t i = 0; i < a.entries.size(); ++i)
		{
			CAPTURE(i);
			const auto& x = a.entries[i];
			const auto& y = b.entries[i];
			CHECK(x.name            == y.name);
			CHECK(x.meshAssetId     == y.meshAssetId);
			CHECK(x.materialAssetId == y.materialAssetId);
			CHECK(x.paintLayer      == y.paintLayer);
			CHECK(x.slopeMin        == y.slopeMin);
			CHECK(x.slopeMax        == y.slopeMax);
			CHECK(x.heightMin       == y.heightMin);
			CHECK(x.heightMax       == y.heightMax);
			CHECK(x.density         == y.density);
			CHECK(x.minScale        == y.minScale);
			CHECK(x.maxScale        == y.maxScale);
			CHECK(x.drawDistance    == y.drawDistance);
			CHECK(x.bucketSize      == y.bucketSize);
			CHECK(x.castsShadow     == y.castsShadow);
			CHECK(x.contributesAO   == y.contributesAO);
			CHECK(x.shadowDistance  == y.shadowDistance);
			CHECK(x.lodHysteresis   == y.lodHysteresis);
			REQUIRE(x.lods.size() == y.lods.size());
			for (size_t k = 0; k < x.lods.size(); ++k)
			{
				CHECK(x.lods[k].meshAssetId     == y.lods[k].meshAssetId);
				CHECK(x.lods[k].materialAssetId == y.lods[k].materialAssetId);
				CHECK(x.lods[k].distance        == y.lods[k].distance);
				CHECK(x.lods[k].density         == y.lods[k].density);
			}
		}
	}

	// A .hasset by hand, so a test controls exactly which chunks exist.
	void writeLfolFile(const fs::path& contentRoot, const std::string& relPath,
	                   const HE::UUID& id, const std::string* payload)
	{
		const auto type = static_cast<uint16_t>(HE::AssetType::LandscapeFoliageLayer);
		std::vector<uint8_t> meta;
		HAsset::Writer::appendPOD(meta, type);
		HAsset::Writer::appendPOD(meta, id.hi);
		HAsset::Writer::appendPOD(meta, id.lo);
		HAsset::Writer::appendString(meta, fs::path(relPath).stem().string());
		HAsset::Writer::appendString(meta, relPath);

		fs::create_directories((contentRoot / relPath).parent_path());
		HAsset::Writer w;
		w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
		if (payload) w.addChunk(HAsset::CHUNK_LFOL, payload->data(), payload->size());
		REQUIRE(w.write((contentRoot / relPath).string(), type));
	}

	// A document with `entriesJson` between the brackets.
	std::string doc(const std::string& entriesJson, const std::string& version = "1")
	{
		return "{\"version\":" + version + ",\"entries\":[" + entriesJson + "]}";
	}
}

TEST_CASE("landscape foliage layer: the type is numbered after Sequence, named, creatable and synced")
{
	// Numbers are written into every file header: appended, never inserted.
	CHECK(static_cast<int>(HE::AssetType::LandscapeFoliageLayer)
	      == static_cast<int>(HE::AssetType::Sequence) + 1);
	CHECK(std::string(HE::assetTypeName(HE::AssetType::LandscapeFoliageLayer)) == "LandscapeFoliageLayer");
	CHECK(HE::Ed::assetTypeFromName("LandscapeFoliageLayer") == HE::AssetType::LandscapeFoliageLayer);
	CHECK(HE::Ed::isCreatableAssetType(HE::AssetType::LandscapeFoliageLayer));
	CHECK(HE::isCollabSyncableAssetType(HE::AssetType::LandscapeFoliageLayer));
	CHECK(HAsset::k_version == 2);   // a new chunk of a new type is no layout change
}

TEST_CASE("landscape foliage layer: every field survives the JSON form, and a second save is the same bytes")
{
	const LandscapeFoliageLayerAsset original = sample();
	const std::string json = HE::landscapeFoliageLayerToJson(original);

	LandscapeFoliageLayerAsset back;
	std::string why;
	REQUIRE_MESSAGE(HE::landscapeFoliageLayerFromJson(json, back, &why), why);
	checkSame(original, back);
	CHECK(HE::landscapeFoliageLayerToJson(back) == json);
	CHECK(HE::validateLandscapeFoliageLayer(back).empty());
}

TEST_CASE("landscape foliage layer: an unreadable document is refused whole and leaves the target alone")
{
	LandscapeFoliageLayerAsset out;
	out.description = "keep me";
	std::string why;

	CHECK_FALSE(HE::landscapeFoliageLayerFromJson("", out, &why));
	CHECK_FALSE(why.empty());
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson("[1,2]", out));
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson("{\"version\":1,\"entries\":[", out));
	// version: missing, wrong type, a build from the future, or one we never wrote
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson("{\"entries\":[]}", out));
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson("{\"version\":\"1\"}", out));
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson(doc("", "2"), out));
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson(doc("", "0"), out));
	// entries: not an array, an element that is not an object
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson("{\"version\":1,\"entries\":{}}", out));
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson(doc("3"), out));
	// a field of the wrong JSON type
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson(doc("{\"density\":\"lots\"}"), out));
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson(doc("{\"castsShadow\":1}"), out));
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson(doc("{\"paintLayer\":1.5}"), out));
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson(doc("{\"name\":7}"), out));
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson("{\"version\":1,\"densityScale\":\"x\"}", out));
	// asset ids: a path, a half id, a negative half, a string half
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson(doc("{\"meshAssetId\":\"Meshes/Rock.hasset\"}"), out));
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson(doc("{\"meshAssetId\":{\"hi\":1}}"), out));
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson(doc("{\"meshAssetId\":{\"hi\":-1,\"lo\":2}}"), out));
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson(doc("{\"meshAssetId\":{\"hi\":\"1\",\"lo\":2}}"), out));
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson(
		doc("{\"lods\":[{\"materialAssetId\":{\"hi\":1}}]}"), out));
	// lods: not an array, too many
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson(doc("{\"lods\":3}"), out));
	std::string eight = "{\"lods\":[";
	for (int i = 0; i < 8; ++i) eight += std::string(i ? "," : "") + "{}";
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson(doc(eight + "]}"), out));

	CHECK(out.description == "keep me");
	CHECK(out.entries.empty());
}

TEST_CASE("landscape foliage layer: at most 32 entries, defaults fill what a document leaves out")
{
	std::string many;
	for (size_t i = 0; i < kLandscapeFoliageMaxEntries; ++i) many += std::string(i ? "," : "") + "{}";
	LandscapeFoliageLayerAsset out;
	REQUIRE(HE::landscapeFoliageLayerFromJson(doc(many), out));
	CHECK(out.entries.size() == kLandscapeFoliageMaxEntries);

	LandscapeFoliageLayerAsset over;
	std::string why;
	CHECK_FALSE(HE::landscapeFoliageLayerFromJson(doc(many + ",{}"), over, &why));
	CHECK(why.find("33 entries") != std::string::npos);

	// An entry that says nothing is a default FoliageLayer; unknown keys are ignored.
	LandscapeFoliageLayerAsset d;
	REQUIRE(HE::landscapeFoliageLayerFromJson(
		"{\"version\":1,\"fromTheFuture\":[1],\"entries\":[{\"name\":\"x\",\"newKnob\":true}]}", d));
	REQUIRE(d.entries.size() == 1);
	const LandscapeFoliageEntry def;
	CHECK(d.densityScale == 1.0f);
	CHECK(d.entries[0].paintLayer == -1);
	CHECK(d.entries[0].density == def.density);
	CHECK(d.entries[0].slopeMax == 1.0f);
	CHECK(d.entries[0].heightMax == 10000.0f);
	CHECK(d.entries[0].castsShadow);
	CHECK(d.entries[0].meshAssetId == HE::UUID{});
}

TEST_CASE("landscape foliage layer: validation names the entry and what is wrong, and still lets the set load")
{
	LandscapeFoliageLayerAsset s = sample();
	CHECK(HE::validateLandscapeFoliageLayer(s).empty());

	s.entries[0].slopeMin = 0.95f;                 // above slopeMax 0.9
	s.entries[0].paintLayer = 8;                   // no such channel
	s.entries[1].meshAssetId = {};                 // nothing to grow
	s.entries[1].heightMin = 10.0f; s.entries[1].heightMax = -10.0f;
	s.entries[1].density = -1.0f;
	s.entries[1].minScale = 3.0f;                  // above maxScale 1.2
	s.entries[1].lods = { { kRockLod, {}, 40.0f, 0.5f }, { kRockLod, {}, 30.0f, 2.0f } };
	s.densityScale = -1.0f;

	const std::vector<std::string> problems = HE::validateLandscapeFoliageLayer(s);
	auto has = [&](const std::string& needle)
	{
		for (const std::string& p : problems) if (p.find(needle) != std::string::npos) return true;
		return false;
	};
	CHECK(has("densityScale"));
	CHECK(has("entry 0 'Mossy Rock': slope range"));
	CHECK(has("entry 0 'Mossy Rock': paintLayer 8"));
	CHECK(has("entry 1 'Tuft': no mesh"));
	CHECK(has("entry 1 'Tuft': heightMin"));
	CHECK(has("entry 1 'Tuft': density is negative"));
	CHECK(has("entry 1 'Tuft': scale range"));
	CHECK(has("entry 1 'Tuft': lod 1: distance"));
	CHECK(has("entry 1 'Tuft': lod 1: density"));

	// Readable, so it parses: validation is a report, not a gate on loading.
	LandscapeFoliageLayerAsset back;
	CHECK(HE::landscapeFoliageLayerFromJson(HE::landscapeFoliageLayerToJson(s), back));
	CHECK(HE::validateLandscapeFoliageLayer(back).size() == problems.size());
}

TEST_CASE("landscape foliage layer: the meshes and materials a set names are listed once each")
{
	std::vector<HE::UUID> refs;
	HE::landscapeFoliageLayerAssetRefs(sample(), refs);
	// rock mesh, moss, rock lod mesh, tuft mesh (moss appears three times)
	REQUIRE(refs.size() == 4);
	CHECK(refs[0] == kRockMesh);
	CHECK(refs[1] == kMoss);
	CHECK(refs[2] == kRockLod);
	CHECK(refs[3] == kTuftMesh);
}

TEST_CASE("landscape foliage layer: a stub reads as an empty set, and an edit survives save and reload")
{
	TempContentDir dir("he_test_lfol_roundtrip");
	{
		const fs::path abs = dir.path / "LF_Test.hasset";
		REQUIRE(HE::Ed::writeAssetStub(abs.string(), "LF_Test.hasset", "LF_Test",
		                               HE::AssetType::LandscapeFoliageLayer));
		ContentManager cm(dir.path.string());
		const HE::UUID id = cm.loadAsset("LF_Test.hasset");
		REQUIRE(id != HE::UUID{});
		CHECK(cm.assetType(id) == HE::AssetType::LandscapeFoliageLayer);
		LandscapeFoliageLayerAsset* set = cm.getLandscapeFoliageLayerMutable(id);
		REQUIRE(set != nullptr);
		CHECK(set->entries.empty());
		CHECK(set->densityScale == 1.0f);

		const LandscapeFoliageLayerAsset authored = sample();
		set->description  = authored.description;
		set->densityScale = authored.densityScale;
		set->entries      = authored.entries;
		REQUIRE(cm.saveAsset(*set));
	}
	{
		ContentManager cm(dir.path.string());
		const LandscapeFoliageLayerAsset* set = cm.getLandscapeFoliageLayer(cm.loadAsset("LF_Test.hasset"));
		REQUIRE(set != nullptr);
		checkSame(sample(), *set);
	}
}

TEST_CASE("landscape foliage layer: a file without the chunk is empty, one with a bad chunk does not load")
{
	TempContentDir dir("he_test_lfol_oldfiles");

	const HE::UUID oldId = HE::UUID::generate();
	// What a stub, or a file from before the chunk existed, looks like.
	writeLfolFile(dir.path, "Old.hasset", oldId, nullptr);
	// A hand edit gone wrong, a future version and an over-long list: each must be a
	// FAILED load, never an empty set a later save would write over the author's file.
	const std::string broken  = "{\"version\":1,\"entries\":[";
	const std::string future  = doc("", "2");
	std::string many;
	for (size_t i = 0; i <= kLandscapeFoliageMaxEntries; ++i) many += std::string(i ? "," : "") + "{}";
	const std::string tooMany = doc(many);
	const std::string badId   = doc("{\"meshAssetId\":\"Meshes/Rock.hasset\"}");
	writeLfolFile(dir.path, "Broken.hasset",  HE::UUID::generate(), &broken);
	writeLfolFile(dir.path, "Future.hasset",  HE::UUID::generate(), &future);
	writeLfolFile(dir.path, "TooMany.hasset", HE::UUID::generate(), &tooMany);
	writeLfolFile(dir.path, "BadId.hasset",   HE::UUID::generate(), &badId);
	// A readable but wrong set loads (validation is a report): the author fixes it.
	const std::string wrong = doc("{\"name\":\"w\",\"slopeMin\":0.8,\"slopeMax\":0.2}");
	writeLfolFile(dir.path, "Wrong.hasset", HE::UUID::generate(), &wrong);

	ContentManager cm(dir.path.string());
	const LandscapeFoliageLayerAsset* old = cm.getLandscapeFoliageLayer(cm.loadAsset("Old.hasset"));
	REQUIRE(old != nullptr);
	CHECK(old->entries.empty());
	CHECK(old->densityScale == 1.0f);

	CHECK(cm.loadAsset("Broken.hasset")  == HE::UUID{});
	CHECK(cm.loadAsset("Future.hasset")  == HE::UUID{});
	CHECK(cm.loadAsset("TooMany.hasset") == HE::UUID{});
	CHECK(cm.loadAsset("BadId.hasset")   == HE::UUID{});
	CHECK_FALSE(cm.isLoaded("Broken.hasset"));

	const LandscapeFoliageLayerAsset* wrongSet = cm.getLandscapeFoliageLayer(cm.loadAsset("Wrong.hasset"));
	REQUIRE(wrongSet != nullptr);
	CHECK_FALSE(HE::validateLandscapeFoliageLayer(*wrongSet).empty());
}

TEST_CASE("landscape foliage layer: an asset a set names is found by the reference scan")
{
	TempContentDir dir("he_test_lfol_refscan");
	const std::string payload = HE::landscapeFoliageLayerToJson(sample());
	writeLfolFile(dir.path, "Foliage/LF_Forest.hasset", HE::UUID::generate(), &payload);
	const std::string quiet = HE::landscapeFoliageLayerToJson(LandscapeFoliageLayerAsset{});
	writeLfolFile(dir.path, "Foliage/LF_Empty.hasset", HE::UUID::generate(), &quiet);

	HE::AssetRefs::ScanRequest req;
	req.contentRoot    = dir.path.string();
	req.projectRoot    = dir.path.parent_path().string();
	req.contentDirName = "Content";

	// The low bits matter: a scan that read the ids as doubles would match a neighbour.
	for (const HE::UUID& target : { kRockMesh, kRockLod, kTuftMesh, kMoss })
	{
		HE::AssetRefs::ScanTargets targets;
		targets.uuids.push_back(target);
		const HE::AssetRefs::ScanResult res = HE::AssetRefs::findReferrers(targets, req);
		CHECK_FALSE(res.incomplete);
		REQUIRE(res.referrers.size() == 1);
		CHECK(res.referrers[0].displayPath == "Foliage/LF_Forest.hasset");
		CHECK(res.referrers[0].kind == HE::AssetRefs::RefKind::Uuid);
	}
}
