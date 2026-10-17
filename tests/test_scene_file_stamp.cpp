#include "doctest.h"
#include "TestFsUtil.h"

#include "../src/HE_Editor/SceneFileStamp.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

// ─── Telling a pull from our own save ────────────────────────────────────────
// SceneFileStamp is what the editor keeps about the scene file it has open: size,
// time and a hash of the bytes. The open scene is only worth a "reload?" question
// when the BYTES on disk differ from what was loaded - not when git touched the file
// without changing it, and not when the editor wrote it itself.

namespace fs = std::filesystem;

namespace {
fs::path stampDir(const char* stem)
{
	static int n = 0;
	const fs::path p = fs::temp_directory_path() /
		("he_stamp_" + std::string(stem) + "_" +
		 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(n++));
	std::error_code ec;
	fs::create_directories(p, ec);
	return p;
}

void write(const fs::path& p, const std::string& text)
{
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	out << text;
}
}

TEST_CASE("scene file stamp: a rewrite with different bytes is a change, our own remember() settles it")
{
	const fs::path dir = stampDir("change");
	const fs::path scene = dir / "Level.hescene";
	write(scene, "{\"entities\":[1,2,3]}");

	SceneFileStamp stamp;
	CHECK_FALSE(stamp.watching());
	CHECK_FALSE(stamp.changed());
	stamp.remember(scene.string());
	REQUIRE(stamp.watching());
	CHECK_FALSE(stamp.changed());

	// What a pull does: the same file name, other content (same SIZE on purpose -
	// the cheap check alone must not be what decides).
	write(scene, "{\"entities\":[4,5,6]}");
	fs::last_write_time(scene, fs::file_time_type::clock::now() + std::chrono::seconds(5));
	CHECK(stamp.changed());
	CHECK(stamp.changed());               // asking does not move the stamp

	stamp.remember(scene.string());       // reloaded, or knowingly kept
	CHECK_FALSE(stamp.changed());

	he_test::removeAllQuiet(dir);
}

TEST_CASE("scene file stamp: touching a file without changing it is not a change")
{
	const fs::path dir = stampDir("touch");
	const fs::path scene = dir / "Level.hescene";
	write(scene, std::string(100000, 'x'));

	SceneFileStamp stamp;
	stamp.remember(scene.string());
	// git rewrites a file it checked out again, bytes identical: only the time moves.
	fs::last_write_time(scene, fs::file_time_type::clock::now() + std::chrono::seconds(30));
	CHECK_FALSE(stamp.changed());
	stamp.refreshTimes();
	CHECK_FALSE(stamp.changed());

	// And a real edit afterwards is still seen.
	write(scene, std::string(100000, 'y'));
	fs::last_write_time(scene, fs::file_time_type::clock::now() + std::chrono::seconds(60));
	CHECK(stamp.changed());
	he_test::removeAllQuiet(dir);
}

TEST_CASE("scene file stamp: a missing file or an empty path watches nothing")
{
	SceneFileStamp stamp;
	stamp.remember("");
	CHECK_FALSE(stamp.watching());
	CHECK_FALSE(stamp.changed());

	const fs::path dir = stampDir("gone");
	const fs::path scene = dir / "Level.hescene";
	write(scene, "a");
	stamp.remember(scene.string());
	REQUIRE(stamp.watching());
	fs::remove(scene);
	CHECK_FALSE(stamp.changed());         // gone: the editor's own Save recreates it, no question to ask
	stamp.remember((dir / "nope.hescene").string());
	CHECK_FALSE(stamp.watching());
	he_test::removeAllQuiet(dir);
}
