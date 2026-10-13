#include "doctest.h"

#include <ContentSync/EngineContentManifest.h>
#include <ContentSync/EngineContentSync.h>
#include <Diagnostics/GlobalState.h>
#include <Diagnostics/Log.h>

#include "CsRename.h" // HorizonContentSync's private src/, see tests/CMakeLists.txt

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

// Thema 187, Schritt 3: the regression test for the EngineContent manifest that
// Windows reported as "used by another process" with no second editor running
// (docs/engine-content-manifest-windows-lock-2026-10-13.md). The witness next door
// (test_engine_content_manifest_lock.cpp) pins down what the platforms do with a
// rename under an open fstream; this drives the REAL EngineContentSync code
// through refreshManifestWith, the refresh with only the SFTP download swapped out:
//  • two startup refreshes at once (the SFTP probe and the project preflight) —
//    the original conflict. Without m_refreshMutex this fails on every platform,
//    not only on Windows: there one's rename hits the other's open fstream, on
//    macOS/Linux one's remove() deletes the file the other just renamed in place.
//  • the leftovers of an editor killed mid-download, on the next start.
//  • a foreign process (Defender, the indexer) holding a file for a moment while
//    it is renamed: on Windows that is detail::renameReplacing's retry, which
//    nothing else in the suite runs.
//
// EngineContentSync is a process-wide singleton and writes the cached manifest
// into GlobalState::engineContentCacheDir(), resolved ONCE per process from HOME /
// XDG_CONFIG_HOME / APPDATA — on a developer machine that holds the real catalogue.
// ctest therefore starts this file with all three pointed into its scratch
// directory and says so in HE_TEST_ENGINE_CONTENT_HOME (tests/CMakeLists.txt).
// Without it (a bare he_tests run, where the real directory may already be
// resolved) the cases do nothing, and say so.

namespace fs = std::filesystem;

namespace
{
// Every refresh in here writes the cache dir of THIS process, so each case first
// checks it is the isolated one. Returns false (and the case returns) outside ctest.
bool cacheDirIsIsolated()
{
	const char* home = std::getenv("HE_TEST_ENGINE_CONTENT_HOME");
	if (!home || !*home)
	{
		MESSAGE("not run: needs the isolated HOME/APPDATA that ctest sets "
		        "(HE_TEST_ENGINE_CONTENT_HOME) — it would write the real EngineContent cache");
		return false;
	}
	std::error_code ec;
	const fs::path cache = fs::weakly_canonical(GlobalState::engineContentCacheDir(), ec);
	const fs::path root  = fs::weakly_canonical(fs::path(home), ec);
	const fs::path rel   = cache.lexically_relative(root);
	REQUIRE_MESSAGE((!rel.empty() && *rel.begin() != ".."),
	                "EngineContent cache dir " << cache.string() << " is outside the isolated "
	                << root.string() << " — refusing to overwrite a real catalogue");
	return true;
}

std::string readText(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeText(const fs::path& p, const std::string& text)
{
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	out << text;
}

// What sftpGetFile does with the bytes (SftpClient.cpp): an ofstream on
// "<local>.partial", truncated, written in pieces while the network delivers,
// closed, then renamed onto <local>. The pause keeps the file open long enough
// for a second refresh to run into it.
HE::Cs::SftpResult fetchLikeSftp(const std::string& localPath, const std::string& json,
                                 std::chrono::milliseconds pause = std::chrono::milliseconds(3))
{
	const fs::path partial = localPath + ".partial";
	{
		std::ofstream out(partial, std::ios::binary | std::ios::trunc);
		if (!out) return { false, "could not create " + partial.filename().string() };
		const size_t half = json.size() / 2;
		out.write(json.data(), static_cast<std::streamsize>(half));
		out.flush();
		// Not sleep_for: a Mac on battery coalesces a 3 ms sleep into ~90 ms,
		// which made 200 refreshes take 18 s instead of under one.
		const auto until = std::chrono::steady_clock::now() + pause;
		while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
		out.write(json.data() + half, static_cast<std::streamsize>(json.size() - half));
	}
	// The real finalize, retry included: Defender looking at the fresh .partial on
	// a Windows runner must not fail the stand-in where sftpGetFile would not fail.
	std::error_code ec;
	HE::Cs::detail::renameReplacing(partial, localPath, ec);
	if (ec) return { false, "rename: " + std::to_string(ec.value()) + " " + ec.message() };
	return { true, {} };
}

std::string manifestJson(std::initializer_list<const char*> paths)
{
	HE::Cs::EngineContentManifest m;
	for (const char* p : paths)
	{
		HE::Cs::EngineContentManifestEntry e;
		e.relativePath = p;
		m.entries.push_back(std::move(e));
	}
	return HE::Cs::serializeManifest(m);
}

// Not EngineContentManifest::findByPath: the struct is not HE_CS_API, so its
// out-of-line members are not exported from the Windows DLL.
bool hasPath(const HE::Cs::EngineContentManifest& m, const char* path)
{
	return std::any_of(m.entries.begin(), m.entries.end(),
	                   [path](const HE::Cs::EngineContentManifestEntry& e) { return e.relativePath == path; });
}

// What the next start would read: the cached manifest.json, parsed. (The
// singleton's loadCachedManifest() cannot show it any more once a refresh made
// the manifest live in this process.)
HE::Cs::EngineContentManifest cachedOnDisk()
{
	HE::Cs::EngineContentManifest m;
	CHECK(HE::Cs::parseManifest(readText(GlobalState::engineContentCacheDir() / "manifest.json"), m));
	return m;
}

size_t retryWarnings()
{
	const auto lines = HE::Log::recentLines();
	return static_cast<size_t>(std::count_if(lines.begin(), lines.end(), [](const std::string& l)
	                                         { return l.find("Renaming '") != std::string::npos &&
	                                                  l.find(" retry ") != std::string::npos; }));
}

// Holds `path` open from another thread, the way a scanner looks at a freshly
// written file: open() returns once the handle is held, it lets go after `hold`.
struct ForeignHolder
{
	std::thread             t;
	std::mutex              mu;
	std::condition_variable cv;
	bool                    opened = false;

	void open(const fs::path& path, std::chrono::milliseconds hold)
	{
		t = std::thread([this, path, hold]
		{
			std::ifstream in(path, std::ios::binary);
			{
				std::lock_guard<std::mutex> lock(mu);
				opened = in.is_open();
			}
			cv.notify_all();
			std::this_thread::sleep_for(hold);
		});
		std::unique_lock<std::mutex> lock(mu);
		cv.wait_for(lock, std::chrono::seconds(5), [this] { return opened; });
		REQUIRE(opened);
	}
	~ForeignHolder() { if (t.joinable()) t.join(); }
};
} // namespace

TEST_CASE("EngineContent manifest: the two startup refreshes no longer collide")
{
	// The startup in miniature: EditorApplication's SFTP probe worker and
	// ProjectPreflight both call refreshManifestBlocking() at the same moment, and
	// both download through the same ".manifest.json.tmp(.partial)". Each round
	// lets the two threads go at once. With the refreshes serialized no fetch ever
	// overlaps another and both succeed; with them overlapping (the bug) some
	// rename/remove fails within the first few rounds — measured: every round.
	if (!cacheDirIsIsolated()) return;
	auto& sync = HE::Cs::EngineContentSync::instance();
	const std::string json = manifestJson({ "Textures/Race.png", "Meshes/Race.fbx" });

	std::atomic<int> inFetch{ 0 }, maxInFetch{ 0 }, failures{ 0 };
	std::string firstError;
	std::mutex errorMu;
	auto fetch = [&](const std::string& localPath)
	{
		const int now = ++inFetch;
		int prev = maxInFetch.load();
		while (now > prev && !maxInFetch.compare_exchange_weak(prev, now)) {}
		HE::Cs::SftpResult r = fetchLikeSftp(localPath, json);
		--inFetch;
		return r;
	};

	constexpr int kRounds = 100;
	for (int round = 0; round < kRounds; ++round)
	{
		std::atomic<int> ready{ 0 };
		auto worker = [&]
		{
			++ready;
			while (ready.load() < 2) std::this_thread::yield();
			if (!sync.refreshManifestWith(fetch))
			{
				++failures;
				std::lock_guard<std::mutex> lock(errorMu);
				if (firstError.empty()) firstError = sync.lastManifestError();
			}
		};
		std::thread probe(worker), preflight(worker);
		probe.join();
		preflight.join();
	}

	CHECK_MESSAGE(failures.load() == 0, failures.load() << " of " << 2 * kRounds
	              << " refreshes failed, first: " << firstError);
	CHECK_MESSAGE(maxInFetch.load() == 1, "two manifest downloads ran at once through the shared temp path");
	CHECK(sync.lastManifestError().empty());
	const auto live = sync.manifest();
	CHECK(live.entries.size() == 2);
	CHECK(hasPath(live, "Textures/Race.png"));
	// The loser of the race used to hear "no cached copy on this machine" here and
	// skip merging the remote assets into the Engine tree.
	CHECK(sync.loadCachedManifest());
	const auto cached = cachedOnDisk();
	CHECK(cached.entries.size() == 2);
	CHECK(hasPath(cached, "Meshes/Race.fbx"));
	CHECK_FALSE(fs::exists(GlobalState::engineContentCacheDir() / ".manifest.json.tmp"));
}

TEST_CASE("EngineContent manifest: a start after a killed editor finds its leftovers and refreshes anyway")
{
	// There is no local content-server process (the SFTP host is at the hoster);
	// what a crash leaves behind on this machine is the EDITOR's half-done work,
	// killed mid-download or mid-cache-write. The next start must not trip over
	// any of it — on Windows a killed process's handles are closed by the kernel,
	// so these are ordinary files by then, just garbage.
	if (!cacheDirIsIsolated()) return;
	auto& sync = HE::Cs::EngineContentSync::instance();
	const fs::path cache = GlobalState::engineContentCacheDir();
	writeText(cache / ".manifest.json.tmp.partial", "{\"entries\":[{\"pa");
	writeText(cache / ".manifest.json.tmp", "{\"entries\":[{\"path\":\"Old/Half");
	writeText(cache / "manifest.json.tmp", "{\"entr");

	const std::string json = manifestJson({ "Audio/AfterCrash.wav" });
	REQUIRE(sync.refreshManifestWith([&](const std::string& p) { return fetchLikeSftp(p, json); }));
	CHECK(sync.lastManifestError().empty());
	CHECK(hasPath(sync.manifest(), "Audio/AfterCrash.wav"));
	CHECK(hasPath(cachedOnDisk(), "Audio/AfterCrash.wav"));
	CHECK_FALSE(fs::exists(cache / ".manifest.json.tmp.partial"));
	CHECK_FALSE(fs::exists(cache / ".manifest.json.tmp"));
	CHECK_FALSE(fs::exists(cache / "manifest.json.tmp"));

	SUBCASE("and with the server unreachable keeps what it has")
	{
		CHECK_FALSE(sync.refreshManifestWith([](const std::string&)
			{ return HE::Cs::SftpResult{ false, "connection timed out" }; }));
		CHECK(sync.lastManifestError() == "connection timed out");
		CHECK(hasPath(sync.manifest(), "Audio/AfterCrash.wav"));
		CHECK(sync.loadCachedManifest());
		CHECK(hasPath(cachedOnDisk(), "Audio/AfterCrash.wav"));
	}
}

TEST_CASE("EngineContent manifest: a file held open for a moment by someone else")
{
	// Defender and the search indexer open a freshly closed file to look at it. On
	// Windows a rename from or onto it then fails with 32/5 until they let go, and
	// detail::renameReplacing retries ~20..320 ms, logging each retry. On
	// macOS/Linux nothing is held against a rename: no retry, no warning.
	if (!cacheDirIsIsolated()) return;
	auto& sync = HE::Cs::EngineContentSync::instance();
	const fs::path cache = GlobalState::engineContentCacheDir();
	const auto hold = std::chrono::milliseconds(60);

	SUBCASE("renameReplacing: the source is held")
	{
		const fs::path from = cache / "held_source.partial";
		const fs::path to   = cache / "held_source";
		writeText(from, "new");
		const size_t warnsBefore = retryWarnings();
		std::error_code ec;
		{
			ForeignHolder scanner;
			scanner.open(from, hold);
			HE::Cs::detail::renameReplacing(from, to, ec);
		}
		CHECK_MESSAGE(!ec, "rename still failed: " << ec.value() << " " << ec.message());
		CHECK(readText(to) == "new");
#ifdef _WIN32
		CHECK_MESSAGE(retryWarnings() > warnsBefore, "the rename went through at once — the holder never held");
#else
		CHECK(retryWarnings() == warnsBefore);
#endif
		std::error_code rm;
		fs::remove(to, rm);
	}

	SUBCASE("refresh: the cached manifest.json is held while the new one is written over it")
	{
		// writeCachedManifest renames manifest.json.tmp onto manifest.json. Without
		// the retry a Windows refresh still succeeded but kept YESTERDAY's catalogue
		// on disk, and the next offline start showed that one.
		REQUIRE(sync.refreshManifestWith([](const std::string& p)
			{ return fetchLikeSftp(p, manifestJson({ "Before/Held.png" })); }));
		const size_t warnsBefore = retryWarnings();
		{
			ForeignHolder indexer;
			indexer.open(cache / "manifest.json", hold);
			REQUIRE(sync.refreshManifestWith([](const std::string& p)
				{ return fetchLikeSftp(p, manifestJson({ "After/Held.png" })); }));
		}
		const auto cached = cachedOnDisk();
		CHECK(hasPath(cached, "After/Held.png"));
		CHECK_FALSE(hasPath(cached, "Before/Held.png"));
#ifdef _WIN32
		CHECK_MESSAGE(retryWarnings() > warnsBefore, "the cache write went through at once — the holder never held");
#else
		CHECK(retryWarnings() == warnsBefore);
#endif
	}
}
