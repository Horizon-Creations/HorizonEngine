// SelfUpdateApplyCore / HorizonUpdater, exercised as a real child process —
// not in-process calls, because the thing under test IS the process boundary:
// CreateProcess/posix_spawn, a directory rename, a health-watch timeout. Every
// case spawns the REAL HorizonUpdater executable (HE_TEST_UPDATER_EXE) against
// he_fake_editor (HE_TEST_FAKE_EDITOR_EXE) standing in for both "the editor
// being waited on" and "the build being relaunched".

#include "doctest.h"
#include "TestFsUtil.h"

#include <SelfUpdate/SelfUpdateApplyPlan.h>
#include <SelfUpdate/SelfUpdateProcess.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using namespace HE::Su;

namespace {

struct TempDir
{
	fs::path path;
	TempDir()
	{
		static int counter = 0;
		path = fs::temp_directory_path() /
		       ("he_su_apply_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
		        std::to_string(++counter));
		fs::create_directories(path);
	}
	~TempDir() { he_test::removeAllQuiet(path); }
	TempDir(const TempDir&) = delete;
	TempDir& operator=(const TempDir&) = delete;
};

#if defined(_WIN32)
const std::string kExeSuffix = ".exe";
#else
const std::string kExeSuffix = "";
#endif

void writeFile(const fs::path& p, const std::string& bytes)
{
	fs::create_directories(p.parent_path());
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string readFile(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Copies he_fake_editor to `root/mainExeName` — a real, independently
// executable copy per install root, exactly as a real build's main executable
// would sit there (and as Takeover.cpp lays the extracted payload out).
void layOutFakeInstall(const fs::path& root, const std::string& mainExeName, const std::string& configContents)
{
	fs::create_directories(root);
	std::error_code ec;
	fs::copy_file(pathFromUtf8(HE_TEST_FAKE_EDITOR_EXE), root / mainExeName, fs::copy_options::overwrite_existing, ec);
	REQUIRE(!ec);
#if !defined(_WIN32)
	fs::permissions(root / mainExeName,
	                 fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
	                 fs::perms::others_read | fs::perms::others_exec,
	                 fs::perm_options::add);
#endif
	if (!configContents.empty()) writeFile(root / "config.json", configContents);
}

// Reads the result file HorizonUpdater writes (ApplyResult, see
// SelfUpdateApplyCore.h) without linking that file's implementation into
// he_tests — it is only compiled into HorizonSelfUpdate and HorizonUpdater,
// and the point of this test is to watch the REAL updater's output, not to
// call back into engine code to interpret it.
struct ResultJson
{
	bool found = false;
	std::string status, error;
	std::int64_t newPid = 0;
};

ResultJson readResult(const fs::path& path)
{
	ResultJson r;
	std::ifstream f(path, std::ios::binary);
	if (!f) return r;
	nlohmann::json j;
	try { f >> j; } catch (...) { return r; }
	r.found  = true;
	r.status = j.value("status", std::string());
	r.error  = j.value("error", std::string());
	r.newPid = j.value("newPid", static_cast<std::int64_t>(0));
	return r;
}

long long markerPid(const fs::path& marker)
{
	std::ifstream f(marker, std::ios::binary);
	long long pid = 0;
	f >> pid;
	return pid;
}

std::vector<std::string> markerArgs(const fs::path& marker)
{
	std::ifstream f(marker, std::ios::binary);
	std::string line;
	std::getline(f, line);   // pid, discarded here
	std::vector<std::string> args;
	while (std::getline(f, line)) args.push_back(line);
	return args;
}

// Runs HorizonUpdater with `plan` to completion (it is a normal, short-lived
// process once its waitPid is satisfied) and returns its own exit code.
int runUpdaterSync(const ApplyPlan& plan, std::uint32_t timeoutMs = 20'000)
{
	SpawnOptions so;
	so.exe           = pathFromUtf8(HE_TEST_UPDATER_EXE);
	so.args          = plan.toArgs();
	so.waitForExitMs = timeoutMs;
	const SpawnResult r = spawnDetached(so);
	REQUIRE(r.ok);
	REQUIRE(r.exited);   // a real test bug, not a product one, if this times out
	return r.exitCode;
}

} // namespace

TEST_CASE("selfupdate apply: ApplyPlan command line round-trips")
{
	ApplyPlan p;
	p.waitPid        = 4242;
	p.waitTimeoutMs  = 9000;
	p.installRoot    = "C:/Program Files/Horizon Editor";
	p.payloadRoot    = "C:/temp/payload one";
	p.backupRoot     = "C:/temp/backup";
	p.mainExe        = "HorizonEditor.exe";
	p.preserve       = { "config.json", "imgui.ini" };
	p.fromVersion    = "0.7.0";
	p.toVersion      = "0.7.1";
	p.launcher       = "exec";
	p.relaunchArgs   = { "C:/projects/My Game/game.heproj" };
	p.healthMs       = 5000;
	p.renameRetryMs  = 1234;
	p.resultFile     = "C:/temp/result.json";
	p.logFile        = "C:/temp/log.txt";

	ApplyPlan parsed;
	std::string error;
	REQUIRE(ApplyPlan::fromArgs(p.toArgs(), parsed, error));
	CHECK(error.empty());
	CHECK(parsed.waitPid == p.waitPid);
	CHECK(parsed.waitTimeoutMs == p.waitTimeoutMs);
	CHECK(parsed.installRoot == p.installRoot);
	CHECK(parsed.payloadRoot == p.payloadRoot);
	CHECK(parsed.backupRoot == p.backupRoot);
	CHECK(parsed.mainExe == p.mainExe);
	CHECK(parsed.preserve == p.preserve);
	CHECK(parsed.fromVersion == p.fromVersion);
	CHECK(parsed.toVersion == p.toVersion);
	CHECK(parsed.launcher == p.launcher);
	CHECK(parsed.relaunchArgs == p.relaunchArgs);
	CHECK(parsed.healthMs == p.healthMs);
	CHECK(parsed.renameRetryMs == p.renameRetryMs);
	CHECK(parsed.resultFile == p.resultFile);
	CHECK(parsed.logFile == p.logFile);
}

TEST_CASE("selfupdate apply: a malformed command line is rejected, not guessed")
{
	ApplyPlan parsed;
	std::string error;
	CHECK_FALSE(ApplyPlan::fromArgs({}, parsed, error));
	CHECK_FALSE(ApplyPlan::fromArgs({ "not-apply" }, parsed, error));
	CHECK_FALSE(ApplyPlan::fromArgs({ "apply", "--wait-pid", "not-a-number" }, parsed, error));
	CHECK_FALSE(ApplyPlan::fromArgs({ "apply", "--install" }, parsed, error));   // flag with no value
	CHECK_FALSE(ApplyPlan::fromArgs({ "apply", "--unknown-flag", "x" }, parsed, error));
}

TEST_CASE("selfupdate apply: a 19-digit number up to int64_t::max is accepted, 20 digits is not")
{
	ApplyPlan parsed;
	std::string error;
	CHECK(ApplyPlan::fromArgs({ "apply", "--wait-pid", "9223372036854775807" }, parsed, error));
	CHECK(parsed.waitPid == 9223372036854775807LL);
	CHECK_FALSE(ApplyPlan::fromArgs({ "apply", "--wait-pid", "92233720368547758070" }, parsed, error));
}

TEST_CASE("selfupdate apply: happy swap preserves config, relaunches with the new build's args")
{
	TempDir dir;
	const std::string mainExe = "HorizonEditor" + kExeSuffix;

	layOutFakeInstall(dir.path / "install", mainExe, "OLD-CONFIG");
	layOutFakeInstall(dir.path / "payload", mainExe, "");   // the new build brings no config of its own

	const fs::path editorMarker = dir.path / "editor-exited.marker";
	const fs::path newMarker    = dir.path / "new-build.marker";

	// The "editor" we wait on: exits almost immediately on its own, as the real
	// editor does once Takeover.cpp has already spawned the updater.
	SpawnOptions editorOpts;
	editorOpts.exe  = dir.path / "install" / mainExe;
	editorOpts.args = { "--marker", editorMarker.string(), "--sleep-ms", "50" };
	const SpawnResult editorProc = spawnDetached(editorOpts);
	REQUIRE(editorProc.ok);

	ApplyPlan plan;
	plan.waitPid       = editorProc.pid;
	plan.waitTimeoutMs = 10'000;
	plan.installRoot   = (dir.path / "install").string();
	plan.payloadRoot   = (dir.path / "payload").string();
	plan.backupRoot    = (dir.path / "backup").string();
	plan.mainExe       = mainExe;
	plan.preserve      = { "config.json" };
	plan.fromVersion   = "0.7.0";
	plan.toVersion     = "0.7.1";
	plan.launcher      = "exec";
	plan.relaunchArgs  = { "--marker", newMarker.string(), "--sleep-ms", "3000", "--project", "C:/fake/project.heproj" };
	plan.healthMs      = 300;
	plan.renameRetryMs = 2000;
	plan.resultFile    = (dir.path / "result.json").string();
	plan.logFile       = (dir.path / "apply.log").string();

	CHECK(runUpdaterSync(plan) == 0);

	const ResultJson result = readResult(dir.path / "result.json");
	REQUIRE(result.found);
	CHECK(result.status == "applied");
	CHECK(result.error.empty());

	// config.json carried over from the old install, the swap itself happened
	// (install now holds what was payload), and the old install's slot is gone.
	CHECK(readFile(dir.path / "install" / "config.json") == "OLD-CONFIG");
	CHECK(fs::exists(dir.path / "backup" / mainExe));          // the old build, moved aside
	CHECK_FALSE(fs::exists(dir.path / "payload"));             // consumed by the rename into install

	REQUIRE(fs::exists(newMarker));
	// Everything in relaunchArgs the fake editor does not itself parse as a flag
	// is passed through unchanged — this is the project path reaching the new build.
	CHECK(markerArgs(newMarker) == std::vector<std::string>{ "--project", "C:/fake/project.heproj" });

	// Clean up the still-running relaunched fake editor.
	const long long newPid = markerPid(newMarker);
	waitForProcessExit(newPid, 5000);
}

TEST_CASE("selfupdate apply: the editor still running past the timeout aborts without touching anything")
{
	TempDir dir;
	const std::string mainExe = "HorizonEditor" + kExeSuffix;
	layOutFakeInstall(dir.path / "install", mainExe, "ORIGINAL");
	layOutFakeInstall(dir.path / "payload", mainExe, "");

	SpawnOptions editorOpts;
	editorOpts.exe  = dir.path / "install" / mainExe;
	editorOpts.args = { "--sleep-ms", "3000" };   // outlives the plan's short wait timeout
	const SpawnResult editorProc = spawnDetached(editorOpts);
	REQUIRE(editorProc.ok);

	ApplyPlan plan;
	plan.waitPid       = editorProc.pid;
	plan.waitTimeoutMs = 800;
	plan.installRoot   = (dir.path / "install").string();
	plan.payloadRoot   = (dir.path / "payload").string();
	plan.backupRoot    = (dir.path / "backup").string();
	plan.mainExe       = mainExe;
	plan.fromVersion   = "0.7.0";
	plan.toVersion     = "0.7.1";
	plan.launcher      = "exec";
	plan.healthMs      = 300;
	plan.renameRetryMs = 1000;
	plan.resultFile    = (dir.path / "result.json").string();
	plan.logFile       = (dir.path / "apply.log").string();

	CHECK(runUpdaterSync(plan) != 0);

	const ResultJson result = readResult(dir.path / "result.json");
	REQUIRE(result.found);
	CHECK(result.status == "aborted");
	CHECK(processExists(editorProc.pid));   // still the SAME process, nobody touched it

	CHECK(readFile(dir.path / "install" / "config.json") == "ORIGINAL");
	CHECK(fs::exists(dir.path / "payload"));      // untouched
	CHECK_FALSE(fs::exists(dir.path / "backup")); // no swap was even attempted

	waitForProcessExit(editorProc.pid, 5000);
}

TEST_CASE("selfupdate apply: a new build that dies on arrival rolls back and relaunches the old one")
{
	TempDir dir;
	const std::string mainExe = "HorizonEditor" + kExeSuffix;
	layOutFakeInstall(dir.path / "install", mainExe, "OLD-CONFIG");
	layOutFakeInstall(dir.path / "payload", mainExe, "");

	const fs::path editorMarker = dir.path / "editor-exited.marker";
	SpawnOptions editorOpts;
	editorOpts.exe  = dir.path / "install" / mainExe;
	editorOpts.args = { "--marker", editorMarker.string(), "--sleep-ms", "50" };
	const SpawnResult editorProc = spawnDetached(editorOpts);
	REQUIRE(editorProc.ok);

	ApplyPlan plan;
	plan.waitPid       = editorProc.pid;
	plan.waitTimeoutMs = 10'000;
	plan.installRoot   = (dir.path / "install").string();
	plan.payloadRoot   = (dir.path / "payload").string();
	plan.backupRoot    = (dir.path / "backup").string();
	plan.mainExe       = mainExe;
	plan.preserve      = { "config.json" };
	plan.fromVersion   = "0.7.0";
	plan.toVersion     = "0.7.1";
	plan.launcher      = "exec";
	// The NEW build "crashes": exits immediately with a non-zero code, well
	// inside the health window.
	plan.relaunchArgs  = { "--sleep-ms", "0", "--exit-code", "1" };
	plan.healthMs      = 2000;
	plan.renameRetryMs = 2000;
	plan.resultFile    = (dir.path / "result.json").string();
	plan.logFile       = (dir.path / "apply.log").string();

	CHECK(runUpdaterSync(plan) == 0);   // RolledBack is still a successful apply outcome

	const ResultJson result = readResult(dir.path / "result.json");
	REQUIRE(result.found);
	CHECK(result.status == "rolled-back");

	// install is back to the OLD build; the broken new build was moved aside to
	// where payload used to be.
	CHECK(readFile(dir.path / "install" / "config.json") == "OLD-CONFIG");
	CHECK(fs::exists(dir.path / "install" / mainExe));
	CHECK(fs::exists(dir.path / "payload" / mainExe));      // the broken build, parked
	CHECK_FALSE(fs::exists(dir.path / "backup"));           // consumed by the restore

	if (result.newPid != 0) waitForProcessExit(result.newPid, 5000);
}
