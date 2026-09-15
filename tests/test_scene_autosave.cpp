#include "doctest.h"
#include "TestFsUtil.h"
#include "SceneAutosave.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/TransformComponent.h>

#include <filesystem>
#include <fstream>
#include <string>

// The solo autosave: when a recovery snapshot is written, how, and what becomes
// of one a crash left behind. The class takes the clock, the dirty flag and the
// undo revision as arguments, so every timing rule below runs on a fake clock;
// only the last case serialises a real world, to prove the file the writer is
// handed is one SceneSerializer can read back.

namespace fs = std::filesystem;
using HE::Ed::SceneAutosave;
using HE::Ed::RecoveryInfo;

namespace
{
	struct Sandbox
	{
		fs::path root;
		std::string dir;
		Sandbox(const char* tag)
		{
			root = fs::temp_directory_path() / ("he_test_autosave_" + std::string(tag));
			he_test::removeAllQuiet(root);
			fs::create_directories(root);
			dir = (root / "Saved" / "Autosave").string();
		}
		~Sandbox() { he_test::removeAllQuiet(root); }
	};

	// A writer that drops a recognisable payload where it is told to.
	SceneAutosave::Writer stubWriter(int& calls, const std::string& payload = "scene")
	{
		return [&calls, payload](const std::string& path)
		{
			++calls;
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			out << payload;
			return static_cast<bool>(out);
		};
	}

	std::string slurp(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	RecoveryInfo infoFor(const char* scene)
	{
		RecoveryInfo i;
		i.scenePath   = scene;
		i.projectPath = "/tmp/Proj/Proj.hproject";
		return i;
	}
}

TEST_CASE("SceneAutosave: the recovery folder sits beside the thumbnail cache, outside Content")
{
	CHECK(SceneAutosave::recoveryDirForProject("") == "");
	const std::string d = SceneAutosave::recoveryDirForProject("/a/b/Proj/Proj.hproject");
	CHECK(fs::path(d) == fs::path("/a/b/Proj/Saved/Autosave"));
	// Live and pending never share a name — the first must never clobber the second.
	CHECK(SceneAutosave::livePath(d) != SceneAutosave::pendingPath(d));
	CHECK(SceneAutosave::liveManifestPath(d) != SceneAutosave::pendingManifestPath(d));
}

TEST_CASE("SceneAutosave: nothing is written while the scene is clean, and nothing before the interval")
{
	Sandbox sb("clean");
	SceneAutosave a;
	a.configure(sb.dir);
	a.setIntervalMs(10'000);
	int calls = 0;
	auto w = stubWriter(calls);

	// The first tick only arms the clock.
	CHECK_FALSE(a.update(1'000, true, 1, infoFor("A.hescene"), w));
	// Dirty, but not yet one interval after arming.
	CHECK_FALSE(a.update(5'000, true, 1, infoFor("A.hescene"), w));
	// Interval reached, but clean: nothing to lose, nothing to write.
	CHECK_FALSE(a.update(20'000, false, 1, infoFor("A.hescene"), w));
	CHECK(calls == 0);
	CHECK_FALSE(fs::exists(sb.dir));   // the folder is only created for a write
}

TEST_CASE("SceneAutosave: a dirty scene is snapshotted once per interval and only when its revision moved")
{
	Sandbox sb("interval");
	SceneAutosave a;
	a.configure(sb.dir);
	a.setIntervalMs(10'000);
	int calls = 0;
	auto w = stubWriter(calls);

	a.update(0, true, 1, infoFor("A.hescene"), w);              // arm
	CHECK(a.update(10'000, true, 1, infoFor("A.hescene"), w));  // first snapshot
	CHECK(calls == 1);
	CHECK(a.hasLiveSnapshot());
	CHECK(a.lastSnapshotRevision() == 1);

	// Same revision, another interval later: the world has not changed, so the
	// file is not re-serialised.
	CHECK_FALSE(a.update(15'000, true, 1, infoFor("A.hescene"), w));
	CHECK(calls == 1);

	// New revision, but inside the interval since the last WRITE: waits.
	CHECK_FALSE(a.update(16'000, true, 2, infoFor("A.hescene"), w));
	CHECK(calls == 1);

	// New revision and the interval is up: writes again.
	CHECK(a.update(30'000, true, 2, infoFor("A.hescene"), w));
	CHECK(calls == 2);
	CHECK(a.lastSnapshotRevision() == 2);

	// The manifest is what the next start reads: it names the scene and the revision.
	auto m = SceneAutosave::readManifest(SceneAutosave::liveManifestPath(sb.dir));
	REQUIRE(m.has_value());
	CHECK(m->scenePath   == "A.hescene");
	CHECK(m->projectPath == "/tmp/Proj/Proj.hproject");
	CHECK(m->revision    == 2);
	CHECK(m->savedAtUnix > 0);
	// No temp file lingers after a successful write.
	CHECK_FALSE(fs::exists(fs::path(sb.dir) / "autosave.hescene.tmp"));
}

TEST_CASE("SceneAutosave: disabled means no file, whatever the clock says")
{
	Sandbox sb("disabled");
	SceneAutosave a;
	a.configure(sb.dir);
	a.setIntervalMs(10'000);
	a.setEnabled(false);
	int calls = 0;
	auto w = stubWriter(calls);
	a.update(0, true, 1, infoFor("A.hescene"), w);
	CHECK_FALSE(a.update(60'000, true, 5, infoFor("A.hescene"), w));
	CHECK(calls == 0);
	CHECK_FALSE(a.hasLiveSnapshot());

	// Interval floor: below ten seconds the serialisation is the hitch.
	a.setIntervalMs(1);
	CHECK(a.intervalMs() == 10'000);
}

TEST_CASE("SceneAutosave: a failed write leaves the previous snapshot and its manifest whole")
{
	Sandbox sb("failed");
	SceneAutosave a;
	a.configure(sb.dir);
	a.setIntervalMs(10'000);
	int good = 0;
	auto ok = stubWriter(good, "first");
	a.update(0, true, 1, infoFor("A.hescene"), ok);
	REQUIRE(a.update(10'000, true, 1, infoFor("A.hescene"), ok));

	// The writer that dies half-way: it wrote something, then said no.
	int bad = 0;
	SceneAutosave::Writer failing = [&bad](const std::string& path)
	{
		++bad;
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << "half";
		return false;
	};
	CHECK_FALSE(a.update(20'000, true, 2, infoFor("A.hescene"), failing));
	CHECK(bad == 1);
	// The live file is still the complete first snapshot, its manifest still
	// says revision 1, and the half file is gone.
	CHECK(slurp(SceneAutosave::livePath(sb.dir)) == "first");
	auto m = SceneAutosave::readManifest(SceneAutosave::liveManifestPath(sb.dir));
	REQUIRE(m.has_value());
	CHECK(m->revision == 1);
	CHECK_FALSE(fs::exists(fs::path(sb.dir) / "autosave.hescene.tmp"));

	// And a failure is not retried every frame: the next try is an interval away.
	CHECK_FALSE(a.update(20'016, true, 2, infoFor("A.hescene"), failing));
	CHECK(bad == 1);
	CHECK(a.update(30'000, true, 2, infoFor("A.hescene"), ok));
	CHECK(slurp(SceneAutosave::livePath(sb.dir)) == "first");   // same payload, new revision
	CHECK(SceneAutosave::readManifest(SceneAutosave::liveManifestPath(sb.dir))->revision == 2);
}

TEST_CASE("SceneAutosave: clear() removes the live snapshot and re-arms the revision check")
{
	Sandbox sb("clear");
	SceneAutosave a;
	a.configure(sb.dir);
	a.setIntervalMs(10'000);
	int calls = 0;
	auto w = stubWriter(calls);
	a.update(0, true, 1, infoFor("A.hescene"), w);
	REQUIRE(a.update(10'000, true, 1, infoFor("A.hescene"), w));

	a.clear();   // what a real save does
	CHECK_FALSE(a.hasLiveSnapshot());
	CHECK_FALSE(fs::exists(SceneAutosave::livePath(sb.dir)));
	CHECK_FALSE(fs::exists(SceneAutosave::liveManifestPath(sb.dir)));

	// The revision the last snapshot had is forgotten too: after the save the
	// user undoes back to it, the scene is dirty again, and it must be written.
	CHECK(a.update(20'000, true, 1, infoFor("A.hescene"), w));
	CHECK(calls == 2);
}

TEST_CASE("SceneAutosave: what a crash left behind is set aside at the next start, not overwritten")
{
	Sandbox sb("stale");
	// Session 1 writes a snapshot and "crashes" (no clear()).
	{
		SceneAutosave a;
		a.configure(sb.dir);
		a.setIntervalMs(10'000);
		int calls = 0;
		auto w = stubWriter(calls, "crashed-work");
		a.update(0, true, 7, infoFor("A.hescene"), w);
		REQUIRE(a.update(10'000, true, 7, infoFor("A.hescene"), w));
		// A temp from a write the crash interrupted, for good measure.
		std::ofstream(fs::path(sb.dir) / "autosave.hescene.tmp") << "torn";
	}
	// Session 2 opens the project.
	SceneAutosave b;
	b.configure(sb.dir);
	CHECK_FALSE(b.pending().has_value());   // not until promoteStale ran
	auto found = b.promoteStale();
	REQUIRE(found.has_value());
	CHECK(found->scenePath    == "A.hescene");
	CHECK(found->revision     == 7);
	CHECK(found->snapshotPath == SceneAutosave::pendingPath(sb.dir));
	CHECK(slurp(found->snapshotPath) == "crashed-work");
	CHECK_FALSE(b.hasLiveSnapshot());
	CHECK_FALSE(fs::exists(fs::path(sb.dir) / "autosave.hescene.tmp"));

	// Session 2's own autosaves land on the LIVE name and leave the offer alone.
	int calls = 0;
	auto w = stubWriter(calls, "new-work");
	b.setIntervalMs(10'000);
	b.update(0, true, 1, infoFor("B.hescene"), w);
	REQUIRE(b.update(10'000, true, 1, infoFor("B.hescene"), w));
	CHECK(slurp(SceneAutosave::pendingPath(sb.dir)) == "crashed-work");
	CHECK(b.pending()->scenePath == "A.hescene");
	CHECK(slurp(SceneAutosave::livePath(sb.dir)) == "new-work");

	// A second promote replaces the older offer with the newer one.
	auto again = b.promoteStale();
	REQUIRE(again.has_value());
	CHECK(again->scenePath == "B.hescene");
	CHECK(slurp(SceneAutosave::pendingPath(sb.dir)) == "new-work");

	// Saying no drops it, and only it.
	b.discardPending();
	CHECK_FALSE(b.pending().has_value());
	CHECK_FALSE(fs::exists(SceneAutosave::pendingPath(sb.dir)));
}

TEST_CASE("SceneAutosave: a snapshot without its manifest is an interrupted write, not an offer")
{
	Sandbox sb("orphan");
	fs::create_directories(sb.dir);
	std::ofstream(SceneAutosave::livePath(sb.dir)) << "torn";   // scene only, no manifest
	SceneAutosave a;
	a.configure(sb.dir);
	CHECK_FALSE(a.promoteStale().has_value());
	CHECK_FALSE(fs::exists(SceneAutosave::livePath(sb.dir)));
	CHECK_FALSE(fs::exists(SceneAutosave::pendingPath(sb.dir)));

	// An unreadable manifest is reported as nothing rather than as garbage.
	std::ofstream(SceneAutosave::pendingPath(sb.dir)) << "x";
	std::ofstream(SceneAutosave::pendingManifestPath(sb.dir)) << "{ not json";
	CHECK_FALSE(a.pending().has_value());
}

TEST_CASE("SceneAutosave: the file the writer produces is a scene the serializer reads back")
{
	Sandbox sb("world");
	HorizonWorld world;
	Entity e = world.createEntity("Recovered");
	TransformComponent t;
	t.position = { 1.0f, 2.0f, 3.0f };
	world.addComponent(e, t);

	SceneAutosave a;
	a.configure(sb.dir);
	a.setIntervalMs(10'000);
	SceneAutosave::Writer w = [&world](const std::string& path)
	{
		SceneSerializer s;
		return s.save(world, path, SerializeFormat::JSON);
	};
	a.update(0, true, 1, infoFor(""), w);   // a never-saved scene: empty path is legal
	REQUIRE(a.update(10'000, true, 1, infoFor(""), w));
	CHECK(SceneAutosave::readManifest(SceneAutosave::liveManifestPath(sb.dir))->scenePath.empty());

	HorizonWorld back;
	SceneSerializer s;
	REQUIRE(s.load(back, SceneAutosave::livePath(sb.dir), SerializeFormat::JSON));
	bool found = false;
	for (auto [ent, n, t] : back.registry().view<NameComponent, TransformComponent>().each())
	{
		if (n.name != "Recovered") continue;
		found = true;
		CHECK(back.registry().get<TransformComponent>(ent).position.x == doctest::Approx(1.0f));
		CHECK(back.registry().get<TransformComponent>(ent).position.z == doctest::Approx(3.0f));
	}
	CHECK(found);
}
