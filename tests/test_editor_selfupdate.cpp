// Help ▸ Check for Updates: the editor's half of the self-update (Thema 186
// Schritt 6). EditorSelfUpdate::Session is what the dialog drives; here it runs
// against a directory laid out like the update server, with the takeover's two
// calls (preflightTakeover / beginTakeover) replaced by recorders. What this
// pins down, on every platform the CI matrix builds:
//
//   * nothing is started on its own: check and download never begin the
//     takeover; only install() does, which the editor calls from its guarded
//     action, after the unsaved-changes prompt (EditorUI.cpp, GuardedAction::
//     InstallUpdate);
//   * the open project is what the new build is started with, and the launch
//     pick the new build runs (ProjectLaunchOpen::pickFromArguments) opens it;
//   * a takeover that does not start leaves the editor able to carry on and the
//     staged build intact.
//
// The real takeover (HorizonUpdater against a real child process) is
// test_selfupdate_apply.cpp; the real SFTP account is test_selfupdate.cpp.

#include "doctest.h"
#include "TestFsUtil.h"

#include "EditorSelfUpdate.h"
#include "ProjectLaunchOpen.h"

#include <SelfUpdate/SelfUpdateCheck.h>
#include <SelfUpdate/SelfUpdateEndpoint.h>
#include <SelfUpdate/SelfUpdateManifest.h>
#include <SelfUpdate/SelfUpdateTransport.h>

#include <Crypto/Sha256.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace HE::Su;
using EditorSelfUpdate::Phase;

namespace {

struct EsuTempDir
{
	fs::path path;
	EsuTempDir()
	{
		static int counter = 0;
		path = fs::temp_directory_path() /
		       ("he_editor_selfupdate_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
		        "_" + std::to_string(++counter));
		fs::create_directories(path);
	}
	~EsuTempDir() { he_test::removeAllQuiet(path); }
	EsuTempDir(const EsuTempDir&) = delete;
	EsuTempDir& operator=(const EsuTempDir&) = delete;
};

void esuWrite(const fs::path& p, const std::string& bytes)
{
	fs::create_directories(p.parent_path());
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string esuSha(const std::string& bytes)
{
	std::uint8_t digest[HE::Crypto::Sha256::kDigestSize];
	HE::Crypto::Sha256::digest(bytes.data(), bytes.size(), digest);
	return HE::Crypto::Sha256::toHex(digest);
}

const LocalBuild kRunning { "0.7.0", "linux-x64" };

// The update server as a directory, a staging directory, and a Backend wired to
// both, with the two takeover calls recorded instead of performed.
struct Rig
{
	EsuTempDir server, staging, scratch;
	std::string artifactBytes = std::string(256 * 1024, 'u');

	std::atomic<int> preflights{ 0 };
	std::atomic<int> begins{ 0 };
	TakeoverOptions  begunWith;
	fs::path         preflightStaging;   // recorded on the worker, checked on the test thread
	TakeoverCheck    preflightAnswer { true, {} };
	TakeoverResult   beginAnswer     { true, {} };

	Rig()
	{
		const std::string rel = "releases/9.9.9/HorizonEditor-linux-x64.tar.gz";
		esuWrite(server.path / "packages" / rel, artifactBytes);
		UpdateManifest m;
		m.version  = "9.9.9";
		m.codename = "Test";
		m.notes    = "A newer editor.";
		m.artifacts.push_back({ "linux-x64", rel, esuSha(artifactBytes), artifactBytes.size() });
		esuWrite(server.path / "packages" / "manifest.json", serializeUpdateManifest(m));
	}

	EditorSelfUpdate::Backend backend()
	{
		EditorSelfUpdate::Backend b;
		b.stagingRoot = staging.path;
		CheckOptions o;
		o.scratchDir = scratch.path;
		o.backoffMs  = 0;
		b.check = [this, o] {
			auto t = makeLocalDirTransport(server.path);
			return checkForUpdate(*t, "packages/manifest.json", kRunning, o);
		};
		b.download = [this, o](const CheckResult& c, const SuProgressFn& progress) {
			auto t = makeLocalDirTransport(server.path);
			return downloadUpdate(*t, c, staging.path, progress, o);
		};
		b.preflight = [this](const TakeoverOptions& opts) {
			preflightStaging = opts.stagingRoot;
			++preflights;
			return preflightAnswer;
		};
		b.begin = [this](const TakeoverOptions& opts) {
			++begins;
			begunWith = opts;
			return beginAnswer;
		};
		return b;
	}
};

void checkAndDownload(EditorSelfUpdate::Session& s)
{
	REQUIRE(s.startCheck());
	s.join();
	REQUIRE(s.snapshot().phase == Phase::Available);
	REQUIRE(s.startDownload());
	s.join();
}

} // namespace

TEST_CASE("editor self-update: the open project is what the new build reopens")
{
	EsuTempDir dir;
	const fs::path project = dir.path / "My Game" / "My Game.heproj";
	esuWrite(project, "{}");

	const std::vector<std::string> args = EditorSelfUpdate::relaunchArgsFor(true, project.string());
	REQUIRE(args.size() == 1);
	// The same pick EditorApplication runs on the relaunched build's arguments
	// (pickLaunchProject), which hands the project to ProjectPreflight.
	const ProjectLaunchOpen::LaunchPick pick = ProjectLaunchOpen::pickFromArguments(args, dir.path);
	CHECK(ProjectLaunchOpen::samePath(pick.project, project.string()));
	CHECK(pick.rejected.empty());

	// On the Project Hub there is no project to pass: the new build then opens
	// the last project, as on any other start.
	CHECK(EditorSelfUpdate::relaunchArgsFor(false, project.string()).empty());
	CHECK(EditorSelfUpdate::relaunchArgsFor(true, "").empty());
}

TEST_CASE("editor self-update: check and download never start the takeover on their own")
{
	Rig rig;
	EditorSelfUpdate::Session s(rig.backend());

	REQUIRE(s.startCheck());
	s.join();
	EditorSelfUpdate::Snapshot snap = s.snapshot();
	CHECK(snap.phase == Phase::Available);
	CHECK(snap.remoteVersion == "9.9.9");
	CHECK(snap.notes == "A newer editor.");
	CHECK(rig.begins == 0);

	// Nothing staged yet: install() refuses without calling anything.
	CHECK_FALSE(s.install({ "x.heproj" }));
	s.join();
	CHECK(rig.begins == 0);

	REQUIRE(s.startDownload());
	s.join();
	snap = s.snapshot();
	CHECK(snap.phase == Phase::Ready);
	CHECK(snap.canInstall());
	CHECK(snap.done == rig.artifactBytes.size());
	CHECK(rig.preflights == 1);
	CHECK(rig.preflightStaging == rig.staging.path);
	CHECK(rig.begins == 0);   // downloaded and verified, still nothing started

	// What the takeover step will trust: pending.json, re-verified.
	StagedUpdate staged;
	std::string err;
	REQUIRE(readPendingUpdate(rig.staging.path, staged, err));
	CHECK(verifyStagedUpdate(staged, err));
	CHECK(staged.version == "9.9.9");

	// The editor's guarded action: only now, and with the open project.
	REQUIRE(s.install({ "/projects/My Game/My Game.heproj" }));
	s.join();
	CHECK(rig.begins == 1);
	CHECK(rig.begunWith.stagingRoot == rig.staging.path);
	REQUIRE(rig.begunWith.relaunchArgs.size() == 1);
	CHECK(rig.begunWith.relaunchArgs[0] == "/projects/My Game/My Game.heproj");
	CHECK(s.snapshot().phase == Phase::Installed);   // the editor quits on this

	// A second press in the frame that quits does not start a second updater.
	CHECK_FALSE(s.install({}));
	s.join();
	CHECK(rig.begins == 1);
}

TEST_CASE("editor self-update: the takeover runs on the worker, not on the caller's thread")
{
	// SUP-101: beginTakeover re-hashes and unpacks the staged build, which takes
	// seconds to minutes. install() must hand it off and return at once, with
	// the phase at Installing until the takeover answers.
	Rig rig;
	std::mutex              gateMutex;
	std::condition_variable gateCv;
	bool                    release = false;
	std::thread::id         beganOn;
	EditorSelfUpdate::Backend b = rig.backend();
	b.begin = [&](const TakeoverOptions& opts) {
		beganOn = std::this_thread::get_id();
		++rig.begins;
		rig.begunWith = opts;
		std::unique_lock<std::mutex> lk(gateMutex);
		gateCv.wait(lk, [&] { return release; });
		return TakeoverResult{ true, {} };
	};
	EditorSelfUpdate::Session s(b);
	checkAndDownload(s);
	REQUIRE(s.snapshot().canInstall());

	REQUIRE(s.install({ "p.heproj" }));   // returns while the takeover is held
	EditorSelfUpdate::Snapshot snap = s.snapshot();
	CHECK(snap.phase == Phase::Installing);
	CHECK(snap.busy());
	CHECK_FALSE(snap.canInstall());
	CHECK_FALSE(s.install({ "p.heproj" }));   // no second takeover while one runs
	CHECK_FALSE(s.startCheck());

	{
		std::lock_guard<std::mutex> lk(gateMutex);
		release = true;
	}
	gateCv.notify_all();
	s.join();
	CHECK(beganOn != std::this_thread::get_id());
	CHECK(rig.begins == 1);
	CHECK(s.snapshot().phase == Phase::Installed);
}

TEST_CASE("editor self-update: a takeover that does not start leaves the editor as it was")
{
	Rig rig;
	rig.beginAnswer = { false, "could not start HorizonUpdater: simulated" };
	EditorSelfUpdate::Session s(rig.backend());
	checkAndDownload(s);
	REQUIRE(s.snapshot().canInstall());

	REQUIRE(s.install({ "p.heproj" }));   // started; the outcome lands in the snapshot
	s.join();
	CHECK(rig.begins == 1);

	// The caller must not quit (EditorUI only quits on ok), the dialog gets the
	// reason, and the verified build is still staged for a later attempt.
	const EditorSelfUpdate::Snapshot snap = s.snapshot();
	CHECK(snap.phase == Phase::Ready);
	CHECK_FALSE(snap.canInstall());
	CHECK(snap.installBlocker.find("simulated") != std::string::npos);
	StagedUpdate staged;
	std::string err;
	REQUIRE(readPendingUpdate(rig.staging.path, staged, err));
	CHECK(verifyStagedUpdate(staged, err));

	// No retry from the same session state: a press would only fail the same way.
	CHECK_FALSE(s.install({ "p.heproj" }));
	CHECK(rig.begins == 1);
}

TEST_CASE("editor self-update: Restart is not offered when the takeover cannot run")
{
	Rig rig;
	rig.preflightAnswer = { false, "cannot update in place: not writable" };
	EditorSelfUpdate::Session s(rig.backend());
	checkAndDownload(s);

	const EditorSelfUpdate::Snapshot snap = s.snapshot();
	CHECK(snap.phase == Phase::Ready);
	CHECK_FALSE(snap.canInstall());
	CHECK(snap.installBlocker == "cannot update in place: not writable");
	CHECK_FALSE(s.install({}));
	CHECK(rig.begins == 0);
}

TEST_CASE("editor self-update: a damaged download stops before anything is offered")
{
	Rig rig;
	EditorSelfUpdate::Session s(rig.backend());
	REQUIRE(s.startCheck());
	s.join();
	REQUIRE(s.snapshot().phase == Phase::Available);

	// The file on the server changes between check and download.
	esuWrite(rig.server.path / "packages" / "releases" / "9.9.9" / "HorizonEditor-linux-x64.tar.gz",
	         std::string(rig.artifactBytes.size(), 'X'));
	REQUIRE(s.startDownload());
	s.join();

	const EditorSelfUpdate::Snapshot snap = s.snapshot();
	CHECK(snap.phase == Phase::Failed);
	CHECK_FALSE(snap.message.empty());
	CHECK_FALSE(snap.canInstall());
	CHECK(rig.preflights == 0);
	CHECK(rig.begins == 0);
	StagedUpdate staged;
	std::string err;
	CHECK_FALSE(readPendingUpdate(rig.staging.path, staged, err));
}

TEST_CASE("editor self-update: no account, no network, nothing to download")
{
	EditorSelfUpdate::Backend b;
	b.check = [] {
		const SelfUpdateConfig config = loadSelfUpdateConfig([](const char*) -> const char* { return nullptr; },
		                                                     HE::Cs::SftpEndpoint{});   // not this build's compiled-in one
		return checkForUpdate(config, kRunning);
	};
	int begins = 0;
	b.begin = [&](const TakeoverOptions&) { ++begins; return TakeoverResult{ true, {} }; };
	EditorSelfUpdate::Session s(b);
	REQUIRE(s.startCheck());
	s.join();
	const EditorSelfUpdate::Snapshot snap = s.snapshot();
	CHECK(snap.phase == Phase::NotConfigured);
	CHECK(snap.message.find("HE_SELFUPDATE") != std::string::npos);
	CHECK_FALSE(s.startDownload());
	CHECK_FALSE(s.install({}));
	CHECK(begins == 0);
}
