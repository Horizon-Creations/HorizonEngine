#include "doctest.h"
#include "TestFsUtil.h"
#include "ProjectManager.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace
{
std::string readFile(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("cppIdentifier: sanitises names into valid C++ identifiers")
{
	CHECK(cppIdentifier("StartupScene") == "StartupScene");
	CHECK(cppIdentifier("My Level")     == "My_Level");   // space → underscore
	CHECK(cppIdentifier("level-2.a")    == "level_2_a");  // punctuation → underscore
	CHECK(cppIdentifier("3rd")          == "_3rd");        // leading digit gets a prefix
	CHECK(cppIdentifier("")             == "Unnamed");     // empty → placeholder
	CHECK(cppIdentifier("_ok_")         == "_ok_");        // underscores preserved
}

TEST_CASE("scaffoldCppProject: lays down a compilable Source/ tree")
{
	const auto root = fs::temp_directory_path() / "he_cpp_scaffold";
	he_test::removeAllQuiet(root);
	fs::create_directories(root);

	REQUIRE(scaffoldCppProject(root.string(), "My Game", "StartupScene"));

	const auto src = root / "Source";
	// The fixed runtime + entry-point files.
	CHECK(fs::exists(src / "GameLogicRuntime.h"));
	CHECK(fs::exists(src / "GameLogicRuntime.cpp"));
	CHECK(fs::exists(src / "GameInstance.h"));
	CHECK(fs::exists(src / "GameInstance.cpp"));
	CHECK(fs::exists(src / "GameLogic.cpp"));
	CHECK(fs::exists(src / "CMakeLists.txt"));
	CHECK(fs::exists(src / "README.md"));
	// A level script for the startup scene.
	CHECK(fs::exists(src / "StartupSceneLevelScript.h"));
	CHECK(fs::exists(src / "StartupSceneLevelScript.cpp"));

	// GameLogic bakes the startup scene name and exports the loader's factories.
	const std::string gl = readFile(src / "GameLogic.cpp");
	CHECK(gl.find("kStartupScene = \"StartupScene\"") != std::string::npos);
	CHECK(gl.find("HE_CreateGameLogic")  != std::string::npos);
	CHECK(gl.find("HE_DestroyGameLogic") != std::string::npos);

	// The library builds under the fixed name the engine loads (no lib prefix).
	const std::string cm = readFile(src / "CMakeLists.txt");
	CHECK(cm.find("add_library(GameLogic SHARED") != std::string::npos);
	CHECK(cm.find("PREFIX \"\"") != std::string::npos);

	he_test::removeAllQuiet(root);
}

TEST_CASE("writeCppLevelScript: registers under the real scene name, class is an identifier")
{
	const auto root = fs::temp_directory_path() / "he_cpp_level";
	he_test::removeAllQuiet(root);
	fs::create_directories(root / "Source");

	// A scene name with a space: the class name is sanitised, the registration key
	// keeps the original name so the runtime looks it up by the scene's real name.
	REQUIRE(writeCppLevelScript(root.string(), "Arena 2"));
	const auto hdr = root / "Source" / "Arena_2LevelScript.h";
	const auto cpp = root / "Source" / "Arena_2LevelScript.cpp";
	CHECK(fs::exists(hdr));
	CHECK(fs::exists(cpp));

	const std::string body = readFile(cpp);
	CHECK(body.find("REGISTER_LEVEL_SCRIPT(\"Arena 2\", Arena_2LevelScript)") != std::string::npos);
	CHECK(body.find("onLevelLoaded")   != std::string::npos);
	CHECK(body.find("onLevelUnloaded") != std::string::npos);

	// Idempotent: a second call over the same scene leaves the files untouched.
	CHECK(writeCppLevelScript(root.string(), "Arena 2"));

	he_test::removeAllQuiet(root);
}

TEST_CASE("writeCppClass: auto-uniquifies when a class of that name exists")
{
	const auto root = fs::temp_directory_path() / "he_cpp_class";
	he_test::removeAllQuiet(root);
	fs::create_directories(root / "Source");

	std::string first, second;
	REQUIRE(writeCppClass(root.string(), "Enemy", &first));
	REQUIRE(writeCppClass(root.string(), "Enemy", &second));

	CHECK(fs::path(first).filename()  == "Enemy.h");
	CHECK(fs::path(second).filename() == "Enemy1.h"); // second one is suffixed
	CHECK(fs::exists(root / "Source" / "Enemy.cpp"));
	CHECK(fs::exists(root / "Source" / "Enemy1.cpp"));

	he_test::removeAllQuiet(root);
}
