#include "doctest.h"
#include "TestFsUtil.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

// Thema 187, Schritt 1: the EngineContent manifest "used by another process" on
// Windows, with no second editor running (docs/engine-content-manifest-windows-lock-2026-10-13.md).
//
// At startup the editor fetches the manifest TWICE at the same time: the SFTP probe
// worker (EditorApplication::startSftpProbe) and the project preflight
// (ProjectPreflight::run) both call EngineContentSync::refreshManifestBlocking(), and
// both download into the same fixed temp path, ".manifest.json.tmp", through
// sftpGetFile()'s ".partial" + rename. The process holding the file is the editor
// itself. These cases replay the two places the threads can meet, deterministically,
// with a second std::fstream standing in for the other thread, and pin down what each
// platform does there:
//  • Windows: MSVC's fstreams open with _SH_DENYNO (FILE_SHARE_READ | FILE_SHARE_WRITE,
//    never FILE_SHARE_DELETE), so std::filesystem::rename (MoveFileExW) refuses: the
//    source still open is ERROR_SHARING_VIOLATION (32, "The process cannot access the
//    file because it is being used by another process"), the destination still open
//    is ERROR_ACCESS_DENIED (5).
//  • macOS/Linux: rename() swaps the directory entry under any open descriptor, so
//    the same interleaving goes through silently — why only Windows ever saw it.
// This is the witness for the mechanism, not the fix: the fix (Schritt 2) runs the
// refreshes one after another (EngineContentSync::m_refreshMutex), so they never hold
// the shared path at the same time, and the regression test (Schritt 3) drives the
// real EngineContentSync code through refreshManifestWith.

namespace fs = std::filesystem;

namespace
{
struct ScratchDir
{
	fs::path root;
	explicit ScratchDir(const char* tag)
	{
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		root = fs::temp_directory_path() /
		       ("he_test_manifest_lock_" + std::string(tag) + "_" + std::to_string(stamp));
		fs::create_directories(root);
	}
	~ScratchDir() { he_test::removeAllQuiet(root); }
};

void writeText(const fs::path& p, const std::string& text)
{
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	out << text;
}

std::string readText(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string describe(const std::error_code& ec)
{
	return ec ? std::to_string(ec.value()) + " (" + ec.message() + ")" : std::string("no error");
}
} // namespace

TEST_CASE("manifest lock: renaming .partial while the other refresh still writes it")
{
	// Thread A finished its download and finalizes .partial -> .manifest.json.tmp
	// (SftpClient.cpp, sftpGetFile) while thread B, which truncated the same .partial
	// a moment later, is still inside its libssh2_sftp_read loop with the ofstream open.
	ScratchDir dir("partial");
	const fs::path tmp     = dir.root / ".manifest.json.tmp";
	const fs::path partial = dir.root / ".manifest.json.tmp.partial";

	writeText(partial, "{\"entries\":[]}");
	std::error_code ec;
	{
		std::ofstream otherThread(partial, std::ios::binary | std::ios::app);
		REQUIRE(otherThread.is_open());
		fs::rename(partial, tmp, ec);
	}
#ifdef _WIN32
	CHECK_MESSAGE(ec.value() == 32, "expected ERROR_SHARING_VIOLATION, got " << describe(ec));
	CHECK(fs::exists(partial));
	CHECK_FALSE(fs::exists(tmp));
#else
	CHECK_MESSAGE(!ec, "POSIX rename refused with an open writer: " << describe(ec));
	CHECK(fs::exists(tmp));
#endif
}

TEST_CASE("manifest lock: renaming onto .manifest.json.tmp while the other refresh reads it")
{
	// Thread B already finalized and is reading .manifest.json.tmp through its ifstream
	// (EngineContentSync.cpp, refreshManifestBlocking) when thread A's download lands on
	// the same name.
	ScratchDir dir("tmp");
	const fs::path tmp     = dir.root / ".manifest.json.tmp";
	const fs::path partial = dir.root / ".manifest.json.tmp.partial";

	writeText(tmp, "{\"entries\":[\"B\"]}");
	writeText(partial, "{\"entries\":[\"A\"]}");
	std::error_code ec;
	{
		std::ifstream otherThread(tmp, std::ios::binary);
		REQUIRE(otherThread.is_open());
		fs::rename(partial, tmp, ec);
	}
#ifdef _WIN32
	CHECK_MESSAGE(ec.value() == 5, "expected ERROR_ACCESS_DENIED, got " << describe(ec));
	CHECK(readText(tmp) == "{\"entries\":[\"B\"]}");
#else
	CHECK_MESSAGE(!ec, "POSIX rename refused with an open reader: " << describe(ec));
	CHECK(readText(tmp) == "{\"entries\":[\"A\"]}");
#endif
}
