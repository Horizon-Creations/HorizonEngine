#pragma once

// ─── Client integrity, light and honest (docs/anti-cheat-plan.md §3.5) ────────
// Every participant hashes ITS OWN program once at startup: SHA-256 of the
// executable and of every engine library beside it, and for every mounted pak
// the archive's tocHash (HpakReader::tocHash(), already computed at mount — a
// 2 GB pak does not need its content read again). The result is a MANIFEST: a
// sorted list of (kind, file name, hash).
//
// At join the guest sends its manifest and the HOST compares it against its
// own. In the listen-server model host and guest run the same packaged build,
// so the lists have to be equal; no signed manifest and no key are needed for
// that, and a key the host does not hold could not check anything anyway. Each
// difference becomes an IntegrityMismatch observation with the file name as
// detail (GameReplication + AntiCheatService, HE_Scene).
//
// What this is worth: it catches casual edits — a Lua script changed inside
// the pak, an asset value set with a hex editor, a debug dylib dropped in. An
// attacker who patches the executable also patches the code that sends the
// list. The server path (§3.3, §3.4) is the defence against that; this file
// promises nothing more than the plan does.
//
// Packaging note, held here as well as in the plan: on macOS `codesign`
// rewrites the Mach-O bytes of exe and dylibs AFTER the build, on Windows
// Authenticode rewrites the PE. Nothing here relies on a pre-signing state:
// both sides hash the signed bytes they actually run, and a manifest written at
// export time would have to be the LAST packaging step. Dev builds (loose
// project files, editor play-in-editor) start no probe at all; the check is
// off there and says so once in the log.
//
// Threading: probe() is synchronous and does the file I/O — call it from the
// job system (IntegrityProbe::start does exactly that through globalPool), not
// from the frame thread. The inputs are collected on the main thread BEFORE the
// job is submitted, because HpakReader and ContentManager's mount list are
// main-thread state; the job only ever touches its own copy.

#include <Types/Defines.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <vector>

namespace HE::Integrity {

enum class FileKind : std::uint8_t
{
	Executable = 0,
	Library    = 1,   // engine dylib / .so / .dll beside the executable
	Pak        = 2,   // .hpak, identified by its TOC hash
};
HE_API const char* fileKindName(FileKind kind);

struct Entry
{
	FileKind    kind = FileKind::Executable;
	std::string name;   // file name only — paths differ per machine, names do not
	std::string hash;   // lowercase hex: 64 chars SHA-256, 16 chars tocHash
};

struct Manifest
{
	// Sorted by (kind, name); compare() relies on the order.
	std::vector<Entry> entries;
	bool empty() const { return entries.empty(); }
};

struct PakInfo
{
	std::string   name;      // file name of the archive
	std::uint64_t tocHash = 0;
};

// What the probe hashes. Collected by the application, which knows where its
// files live (on macOS the exe sits in Contents/MacOS while SDL_GetBasePath()
// answers Contents/Resources, so there are usually TWO library directories).
struct Inputs
{
	std::filesystem::path              executable;    // empty = not hashed
	std::vector<std::filesystem::path> libraryDirs;   // scanned flat, not recursively
	std::vector<PakInfo>               paks;
};

// Synchronous. Files that cannot be read are left out of the manifest rather
// than failing the whole probe: a missing entry then shows up as a difference
// at join, which is the honest outcome.
HE_API Manifest probe(const Inputs& in);

// Where the running executable is, or empty when the platform will not say.
HE_API std::filesystem::path currentExecutablePath();

// ── Comparison (host side) ──
enum class Mismatch : std::uint8_t
{
	HashDiffers,      // both have the file, bytes differ
	MissingOnGuest,   // host has it, guest does not
	ExtraOnGuest,     // guest has a file the host does not (the debug dylib case)
};

struct Difference
{
	Mismatch    what = Mismatch::HashDiffers;
	FileKind    kind = FileKind::Executable;
	std::string name;
};

HE_API std::vector<Difference> compare(const Manifest& host, const Manifest& guest);
// One line for a log or an observation detail, e.g. "pak Game.hpak differs".
HE_API std::string describe(const Difference& d);

// ── The process-wide probe ──
// GameApplication starts it after the paks are mounted; GameReplication reads
// it when a connection appears. One per process, because there is one program
// to hash.
class HE_API IntegrityProbe
{
public:
	enum class State : std::uint8_t
	{
		Idle,      // never started: a dev build, or nothing to hash — check OFF
		Running,   // job in flight; a join waits for it
		Ready,     // manifest() is valid
	};

	static IntegrityProbe& instance();

	// Submit the hashing to the global thread pool. A second call while one is
	// running is ignored; a call after Ready re-runs (paks mounted later).
	void start(Inputs in);
	// Same, on the calling thread. Tests, and callers that already sit on a
	// worker.
	void runNow(const Inputs& in);
	// Adopt a manifest directly (tests; a manifest read from elsewhere).
	void setManifest(Manifest m);
	// Back to Idle. Blocks until a running job has finished.
	void reset();

	State state() const { return m_state.load(std::memory_order_acquire); }
	bool  ready() const { return state() == State::Ready; }
	// A copy: the caller keeps it for the session, the probe may re-run.
	Manifest manifest() const;

	IntegrityProbe(const IntegrityProbe&)            = delete;
	IntegrityProbe& operator=(const IntegrityProbe&) = delete;

private:
	IntegrityProbe();
	~IntegrityProbe();

	void publish(Manifest m);
	void waitForJob();

	mutable std::mutex   m_mutex;
	Manifest             m_manifest;
	std::future<void>    m_job;       // the pool task, so reset() can wait for it
	std::atomic<State>   m_state { State::Idle };
};

} // namespace HE::Integrity
