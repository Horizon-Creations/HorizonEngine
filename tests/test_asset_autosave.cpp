#include "doctest.h"
#include "TestFsUtil.h"
#include "AssetAutosave.h"
#include <ContentManager/ContentManager.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// The asset tabs' autosave: when a recovery copy is written, that a copy goes
// the moment its file stops being dirty, what an earlier run's copies become
// at the next start, and what Restore does to the file (and keeps of it). The
// clock is handed in; the panels are stood in for by writer lambdas, which is
// all AssetAutosave ever sees of them. The last cases write a real asset
// through ContentManager::writeAssetTo, the path every ContentManager-backed
// panel's copy takes.

namespace fs = std::filesystem;
using HE::Ed::AssetAutosave;
using HE::Ed::AssetSnapshotSource;

namespace
{
	struct Project
	{
		fs::path root;
		std::string dir;
		Project(const char* tag)
		{
			root = fs::temp_directory_path() / ("he_test_asset_autosave_" + std::string(tag));
			he_test::removeAllQuiet(root);
			fs::create_directories(root / "Content");
			dir = AssetAutosave::recoveryDirForProject((root / "Game.hproject").string());
		}
		~Project() { he_test::removeAllQuiet(root); }

		std::string file(const char* rel, const char* body = "on disk")
		{
			const fs::path p = root / rel;
			fs::create_directories(p.parent_path());
			std::ofstream(p, std::ios::binary | std::ios::trunc) << body;
			return p.string();
		}
	};

	std::string slurp(const fs::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	// A panel's dirty file, whose "unsaved state" is `payload`.
	AssetSnapshotSource source(const std::string& file, const std::string& payload, int* calls = nullptr)
	{
		return { file, [payload, calls](const std::string& dest) {
			if (calls) ++*calls;
			std::ofstream out(dest, std::ios::binary | std::ios::trunc);
			out << payload;
			return static_cast<bool>(out);
		} };
	}

	std::size_t filesIn(const std::string& dir)
	{
		std::error_code ec;
		if (!fs::is_directory(dir, ec)) return 0;
		std::size_t n = 0;
		for (const auto& e : fs::directory_iterator(dir, ec)) { (void)e; ++n; }
		return n;
	}
}

TEST_CASE("AssetAutosave: the folder sits beside the scene's snapshot")
{
	const std::string d = AssetAutosave::recoveryDirForProject("/p/Game/Game.hproject");
	CHECK(fs::path(d) == fs::path("/p/Game/Saved/Autosave/Assets"));
	CHECK(fs::path(AssetAutosave::projectRootFor("/p/Game/Game.hproject")) == fs::path("/p/Game"));
	CHECK(AssetAutosave::recoveryDirForProject("").empty());
}

TEST_CASE("AssetAutosave: a copy is written one interval after the first tick, not before")
{
	Project p("interval");
	const std::string f = p.file("Content/Hero.hasset");
	AssetAutosave a;
	a.configure(p.dir, p.root.string());
	a.setIntervalMs(10'000);
	int calls = 0;
	const std::vector<AssetSnapshotSource> dirty{ source(f, "edited", &calls) };

	CHECK(a.update(1'000, dirty) == 0);      // arms the timer
	CHECK(a.update(10'999, dirty) == 0);     // 9.999 s later: not yet
	CHECK(calls == 0);
	CHECK(a.update(11'000, dirty) == 1);
	CHECK(calls == 1);
	CHECK(a.liveCount() == 1);

	const std::string key = a.keyFor(f);
	REQUIRE_FALSE(key.empty());
	CHECK(slurp(fs::path(a.liveDir()) / (key + ".snap")) == "edited");
	CHECK(fs::exists(fs::path(a.liveDir()) / (key + ".json")));
	CHECK_FALSE(fs::exists(fs::path(a.liveDir()) / (key + ".snap.tmp")));
	// The file itself is the user's: untouched.
	CHECK(slurp(f) == "on disk");

	// Still dirty: rewritten every interval, with the current state.
	const std::vector<AssetSnapshotSource> later{ source(f, "edited more", &calls) };
	CHECK(a.update(15'000, later) == 0);
	CHECK(a.update(21'000, later) == 1);
	CHECK(slurp(fs::path(a.liveDir()) / (key + ".snap")) == "edited more");
}

TEST_CASE("AssetAutosave: a file that stops being dirty loses its copy on the very next tick")
{
	Project p("prune");
	const std::string f1 = p.file("Content/A.hasset");
	const std::string f2 = p.file("Content/B.hasset");
	AssetAutosave a;
	a.configure(p.dir, p.root.string());
	a.setIntervalMs(10'000);
	a.update(0, {});
	REQUIRE(a.update(10'000, { source(f1, "a"), source(f2, "b") }) == 2);
	REQUIRE(filesIn(a.liveDir()) == 4);

	// A was saved (its panel no longer reports it); the interval has NOT passed.
	a.update(10'016, { source(f2, "b") });
	CHECK(a.liveCount() == 1);
	CHECK_FALSE(fs::exists(fs::path(a.liveDir()) / (a.keyFor(f1) + ".snap")));
	CHECK_FALSE(fs::exists(fs::path(a.liveDir()) / (a.keyFor(f1) + ".json")));
	CHECK(fs::exists(fs::path(a.liveDir()) / (a.keyFor(f2) + ".snap")));

	// Everything clean: nothing left to offer after a crash.
	a.update(10'032, {});
	CHECK(a.liveCount() == 0);
	CHECK(filesIn(a.liveDir()) == 0);
}

TEST_CASE("AssetAutosave: switched off, nothing is written but stale copies still go")
{
	Project p("disabled");
	const std::string f = p.file("Content/A.hasset");
	AssetAutosave a;
	a.configure(p.dir, p.root.string());
	a.setIntervalMs(10'000);
	a.update(0, {});
	REQUIRE(a.update(10'000, { source(f, "a") }) == 1);
	a.setEnabled(false);
	CHECK(a.update(40'000, { source(f, "a2") }) == 0);
	CHECK(slurp(fs::path(a.liveDir()) / (a.keyFor(f) + ".snap")) == "a");
	a.update(40'016, {});
	CHECK(a.liveCount() == 0);
}

TEST_CASE("AssetAutosave: a file outside the project is not copied")
{
	Project p("outside");
	const fs::path elsewhere = fs::temp_directory_path() / "he_test_asset_autosave_elsewhere.hasset";
	std::ofstream(elsewhere) << "x";
	AssetAutosave a;
	a.configure(p.dir, p.root.string());
	a.setIntervalMs(10'000);
	a.update(0, {});
	int calls = 0;
	CHECK(a.update(10'000, { source(elsewhere.string(), "y", &calls) }) == 0);
	CHECK(calls == 0);
	CHECK(a.keyFor(elsewhere.string()).empty());
	he_test::removeQuiet(elsewhere);
}

TEST_CASE("AssetAutosave: a writer that fails leaves the previous copy whole")
{
	Project p("writerfail");
	const std::string f = p.file("Content/A.hasset");
	AssetAutosave a;
	a.configure(p.dir, p.root.string());
	a.setIntervalMs(10'000);
	a.update(0, {});
	REQUIRE(a.update(10'000, { source(f, "good") }) == 1);
	AssetSnapshotSource bad{ f, [](const std::string& dest) {
		std::ofstream(dest) << "half";
		return false;
	} };
	CHECK(a.update(20'000, { bad }) == 0);
	CHECK(slurp(fs::path(a.liveDir()) / (a.keyFor(f) + ".snap")) == "good");
	CHECK_FALSE(fs::exists(fs::path(a.liveDir()) / (a.keyFor(f) + ".snap.tmp")));
}

TEST_CASE("AssetAutosave: an earlier run's copies become pending at the next start")
{
	Project p("promote");
	const std::string f1 = p.file("Content/Mat/Rock.hasset");
	const std::string f2 = p.file("Source/Player.h");
	{
		AssetAutosave crashed;
		crashed.configure(p.dir, p.root.string());
		crashed.setIntervalMs(10'000);
		crashed.update(0, {});
		REQUIRE(crashed.update(10'000, { source(f1, "rock*"), source(f2, "player*") }) == 2);
		// ...and the process dies here: no clear().
	}
	// A write the crash cut in half: a temp and a copy without a manifest.
	std::ofstream(fs::path(p.dir) / "Live" / "deadbeef_X.snap.tmp") << "half";
	std::ofstream(fs::path(p.dir) / "Live" / "deadbeef_Y.snap") << "orphan";

	AssetAutosave next;
	next.configure(p.dir, p.root.string());
	CHECK(next.promoteStale() == 2);
	CHECK(filesIn(next.liveDir()) == 0);   // orphans gone, live empty for this run

	const auto offers = next.pending();
	REQUIRE(offers.size() == 2);
	// Sorted by path; project-relative, so the offer survives a moved project.
	CHECK(offers[0].relativePath == "Content/Mat/Rock.hasset");
	CHECK(offers[1].relativePath == "Source/Player.h");
	CHECK(fs::path(offers[0].targetPath) == fs::path(f1).lexically_normal());
	CHECK(offers[0].savedAtUnix > 0);
	CHECK_FALSE(offers[0].changedSince);
	CHECK_FALSE(offers[0].targetMissing);
	CHECK(slurp(offers[1].snapshotPath) == "player*");

	// This run's first write does not touch the pending copies.
	next.setIntervalMs(10'000);
	next.update(0, {});
	next.update(10'000, { source(f1, "rock again") });
	CHECK(slurp(next.pending()[0].snapshotPath) == "rock*");
}

TEST_CASE("AssetAutosave: a file changed or deleted after its copy is flagged")
{
	Project p("stale");
	const std::string f1 = p.file("Content/A.hasset");
	const std::string f2 = p.file("Content/B.hasset");
	{
		AssetAutosave crashed;
		crashed.configure(p.dir, p.root.string());
		crashed.setIntervalMs(10'000);
		crashed.update(0, {});
		REQUIRE(crashed.update(10'000, { source(f1, "a*"), source(f2, "b*") }) == 2);
	}
	// A was written again after the copy (a pull, another tool); B was deleted.
	fs::last_write_time(f1, fs::last_write_time(f1) + std::chrono::seconds(5));
	he_test::removeQuiet(f2);

	AssetAutosave next;
	next.configure(p.dir, p.root.string());
	REQUIRE(next.promoteStale() == 2);
	const auto offers = next.pending();
	REQUIRE(offers.size() == 2);
	CHECK(offers[0].changedSince);
	CHECK_FALSE(offers[0].targetMissing);
	CHECK(offers[1].targetMissing);
}

TEST_CASE("AssetAutosave: Restore keeps the replaced file, writes the copy, drops the offer")
{
	Project p("restore");
	const std::string f = p.file("Content/Hero.hasset", "saved version");
	{
		AssetAutosave crashed;
		crashed.configure(p.dir, p.root.string());
		crashed.setIntervalMs(10'000);
		crashed.update(0, {});
		REQUIRE(crashed.update(10'000, { source(f, "unsaved version") }) == 1);
	}
	AssetAutosave next;
	next.configure(p.dir, p.root.string());
	REQUIRE(next.promoteStale() == 1);
	const std::string key = next.pending()[0].key;

	std::string error;
	const auto written = next.restorePending(key, &error);
	REQUIRE(written.has_value());
	CHECK(error.empty());
	CHECK(fs::path(*written) == fs::path(f).lexically_normal());
	CHECK(slurp(f) == "unsaved version");
	CHECK(next.pending().empty());

	// The version it replaced is in Replaced/, whole.
	REQUIRE(filesIn(next.replacedDir()) == 1);
	for (const auto& e : fs::directory_iterator(next.replacedDir()))
	{
		CHECK(slurp(e.path()) == "saved version");
		CHECK(e.path().filename().string().find("Hero.hasset") != std::string::npos);
	}
	// No temp left beside the asset.
	CHECK(filesIn((p.root / "Content").string()) == 1);

	// A second restore of the same key has nothing to restore.
	CHECK_FALSE(next.restorePending(key, &error).has_value());
	CHECK_FALSE(error.empty());
}

TEST_CASE("AssetAutosave: Restore of a deleted file creates it; Delete Copy touches no file")
{
	Project p("restore_missing");
	const std::string f1 = p.file("Content/Gone.hasset");
	const std::string f2 = p.file("Content/Kept.hasset", "kept");
	{
		AssetAutosave crashed;
		crashed.configure(p.dir, p.root.string());
		crashed.setIntervalMs(10'000);
		crashed.update(0, {});
		REQUIRE(crashed.update(10'000, { source(f1, "gone*"), source(f2, "kept*") }) == 2);
	}
	he_test::removeQuiet(f1);
	AssetAutosave next;
	next.configure(p.dir, p.root.string());
	REQUIRE(next.promoteStale() == 2);
	const auto offers = next.pending();

	REQUIRE(next.restorePending(offers[0].key).has_value());
	CHECK(slurp(f1) == "gone*");
	CHECK(filesIn(next.replacedDir()) == 0);    // nothing on disk to keep

	next.discardPending(offers[1].key);
	CHECK(next.pending().empty());
	CHECK(slurp(f2) == "kept");
}

TEST_CASE("AssetAutosave: a manifest pointing out of the project is never written to")
{
	Project p("escape");
	const fs::path pend = fs::path(p.dir) / "Pending";
	fs::create_directories(pend);
	std::ofstream(pend / "0000000000000001_evil.snap") << "payload";
	std::ofstream(pend / "0000000000000001_evil.json")
		<< R"({"file":"../outside.txt","savedAt":1,"targetExisted":false,"targetMtime":0})";

	AssetAutosave a;
	a.configure(p.dir, p.root.string());
	const auto offers = a.pending();
	REQUIRE(offers.size() == 1);
	CHECK(offers[0].targetPath.empty());
	std::string error;
	CHECK_FALSE(a.restorePending(offers[0].key, &error).has_value());
	CHECK_FALSE(error.empty());
	CHECK_FALSE(fs::exists(p.root.parent_path() / "outside.txt"));
	// And a key that climbs is not one of ours.
	CHECK_FALSE(a.restorePending("../x").has_value());
}

TEST_CASE("AssetAutosave: clear() removes this run's copies (clean exit)")
{
	Project p("clear");
	const std::string f = p.file("Content/A.hasset");
	AssetAutosave a;
	a.configure(p.dir, p.root.string());
	a.setIntervalMs(10'000);
	a.update(0, {});
	REQUIRE(a.update(10'000, { source(f, "a") }) == 1);
	a.clear();
	CHECK(a.liveCount() == 0);
	CHECK(filesIn(a.liveDir()) == 0);
	AssetAutosave next;
	next.configure(p.dir, p.root.string());
	CHECK(next.promoteStale() == 0);
}

TEST_CASE("ContentManager::writeAssetTo writes saveAsset's bytes elsewhere and publishes nothing")
{
	Project p("writeto");
	ContentManager cm((p.root / "Content").string());
	int published = 0;
	cm.setOnAssetSaved([&published](const std::string&, const std::string&) { ++published; });

	ThemeAsset t;
	t.type = HE::AssetType::Theme;
	t.name = "Dark";
	t.path = "Dark.hasset";
	t.json = R"({"roles":{"accent":"#ff0000"}})";
	REQUIRE(cm.saveAsset(t));
	CHECK(published == 1);
	const HE::UUID id = t.id;

	// The copy: same identity, the edited payload, not in Content/, no publish.
	ThemeAsset copy = t;
	copy.json = R"({"roles":{"accent":"#00ff00"}})";
	const fs::path dest = p.root / "Saved" / "Autosave" / "Assets" / "Live" / "x.snap";
	REQUIRE(cm.writeAssetTo(copy, dest.string()));
	CHECK(published == 1);
	CHECK(fs::exists(dest));

	// Put back where the asset lives (what Restore does), it loads as that asset.
	fs::copy_file(dest, p.root / "Content" / "Dark.hasset", fs::copy_options::overwrite_existing);
	ContentManager fresh((p.root / "Content").string());
	const HE::UUID loaded = fresh.loadAsset("Dark.hasset");
	CHECK(loaded == id);
	const ThemeAsset* back = fresh.getTheme(loaded);
	REQUIRE(back != nullptr);
	CHECK(back->json == copy.json);
}
