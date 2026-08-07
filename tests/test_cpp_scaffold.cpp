#include "doctest.h"
#include "TestFsUtil.h"
#include "CppScaffoldTemplates.h"
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
	CHECK(HE::tools::cppIdentifier("StartupScene") == "StartupScene");
	CHECK(HE::tools::cppIdentifier("My Level")     == "My_Level");   // space → underscore
	CHECK(HE::tools::cppIdentifier("level-2.a")    == "level_2_a");  // punctuation → underscore
	CHECK(HE::tools::cppIdentifier("3rd")          == "_3rd");        // leading digit gets a prefix
	CHECK(HE::tools::cppIdentifier("")             == "Unnamed");     // empty → placeholder
	CHECK(HE::tools::cppIdentifier("_ok_")         == "_ok_");        // underscores preserved
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

// The scaffolding writers only choose a file name and drop a CppScaffold template
// on disk — the generated file must be that template VERBATIM. This pins the seam
// the template strings moved across (out of ProjectManager.cpp into
// CppScaffoldTemplates.cpp): they are the source a generated project compiles
// from, so a stray character produces a project that does not build.
TEST_CASE("scaffolded files are the CppScaffold templates verbatim")
{
	const auto root = fs::temp_directory_path() / "he_cpp_scaffold_seam";
	he_test::removeAllQuiet(root);
	fs::create_directories(root);

	REQUIRE(scaffoldCppProject(root.string(), "My Game", "StartupScene"));
	const auto src = root / "Source";

	CHECK(readFile(src / "GameLogicRuntime.h")   == CppScaffold::runtimeHeader());
	CHECK(readFile(src / "GameLogicRuntime.cpp") == CppScaffold::runtimeSource());
	CHECK(readFile(src / "GameInstance.h")       == CppScaffold::gameInstanceHeader());
	CHECK(readFile(src / "GameInstance.cpp")     == CppScaffold::gameInstanceSource());
	CHECK(readFile(src / "GameLogic.cpp")        == CppScaffold::gameLogicSource("StartupScene"));
	// The CMakeLists gets the identifier form of the project name, the README the
	// display name.
	CHECK(readFile(src / "CMakeLists.txt")       == CppScaffold::cmakeLists("My_Game"));
	CHECK(readFile(src / "README.md")            == CppScaffold::readme("My Game"));
	CHECK(readFile(src / "StartupSceneLevelScript.h")
	      == CppScaffold::levelScriptHeader("StartupSceneLevelScript", "StartupScene"));
	CHECK(readFile(src / "StartupSceneLevelScript.cpp")
	      == CppScaffold::levelScriptSource("StartupSceneLevelScript", "StartupScene"));

	he_test::removeAllQuiet(root);
}

// The pieces every generated tree needs to compile and to be found at runtime.
TEST_CASE("CppScaffold templates carry the load-bearing declarations")
{
	// The registry macro + the two functions it drives — without them a level
	// script never registers and the engine finds no script for its scene.
	const std::string rt = CppScaffold::runtimeHeader();
	CHECK(rt.find("#define REGISTER_LEVEL_SCRIPT") != std::string::npos);
	CHECK(rt.find("void registerLevelScript(")     != std::string::npos);
	CHECK(rt.find("std::unique_ptr<LevelScript> createLevelScript(") != std::string::npos);
	CHECK(CppScaffold::runtimeSource().find("std::unique_ptr<LevelScript> createLevelScript(")
	      != std::string::npos);

	// A gameplay class stub is a matching header/source pair.
	CHECK(CppScaffold::gameplayClassHeader("Enemy").find("class Enemy")        != std::string::npos);
	CHECK(CppScaffold::gameplayClassSource("Enemy").find("#include \"Enemy.h\"") != std::string::npos);
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

// ── Generated C++ types header (CppTypesHeaderGen) ───────────────────────────

#include "CppTypesHeaderGen.h"
#include <Types/TypeRegistry.h>

TEST_CASE("CppTypesHeaderGen: enums, structs, defaults and dependency order")
{
	auto& reg = HE::TypeRegistry::instance();
	HE::EnumDef weapon;
	weapon.name = "Weapon"; weapon.assetPath = "Content/G/Weapon.hasset";
	weapon.entries = { { "Sword", 0 }, { "Bow", 7 } };
	reg.registerEnum(weapon);

	// Outer embeds Inner — Inner must be emitted first regardless of name order
	// ("Inner" < "Outer" alphabetically anyway, so flip: name them against it).
	HE::StructDef inner;
	inner.name = "ZInner"; inner.assetPath = "Content/G/ZInner.hasset";
	{
		HE::StructField sp; sp.name = "speed"; sp.type = HorizonCode::PinType::Float;
		sp.defaultValue = HorizonCode::Value::ofFloat(3.5f);
		inner.fields = { sp };
	}
	reg.registerStruct(inner);
	HE::StructDef outer;
	outer.name = "AOuter"; outer.assetPath = "Content/G/AOuter.hasset";
	{
		HE::StructField hp; hp.name = "hp"; hp.type = HorizonCode::PinType::Float;
		hp.defaultValue = HorizonCode::Value::ofFloat(100.0f);
		HE::StructField ttl; ttl.name = "title"; ttl.type = HorizonCode::PinType::String;
		ttl.defaultValue = HorizonCode::Value::ofString("Rookie \"R1\"");
		HE::StructField w; w.name = "weapon"; w.type = HorizonCode::PinType::Enum;
		w.typeName = weapon.assetPath; w.defaultValue.s = "Bow";
		HE::StructField in; in.name = "inner"; in.type = HorizonCode::PinType::Struct;
		in.typeName = inner.assetPath;
		HE::StructField tags; tags.name = "tags"; tags.type = HorizonCode::PinType::String;
		tags.isArray = true;
		HE::StructField bad; bad.name = "2 bad name!"; bad.type = HorizonCode::PinType::Int;
		outer.fields = { hp, ttl, w, in, tags, bad };
	}
	reg.registerStruct(outer);

	const std::string h = HE::generateCppTypesHeader();
	CHECK(h.find("enum class Weapon : int") != std::string::npos);
	CHECK(h.find("Bow = 7,") != std::string::npos);
	CHECK(h.find("struct AOuter") != std::string::npos);
	CHECK(h.find("float hp = 100.0f;") != std::string::npos);
	CHECK(h.find("std::string title = \"Rookie \\\"R1\\\"\";") != std::string::npos);
	CHECK(h.find("Weapon weapon = Weapon::Bow;") != std::string::npos);
	CHECK(h.find("ZInner inner;") != std::string::npos);
	CHECK(h.find("std::vector<std::string> tags;") != std::string::npos);
	CHECK(h.find("int _2_bad_name_ = 0;") != std::string::npos);   // sanitized + commented
	CHECK(h.find("\"2 bad name!\"") != std::string::npos);
	// Dependency order: ZInner's definition precedes AOuter's.
	CHECK(h.find("struct ZInner") < h.find("struct AOuter"));

	// writeCppTypesHeader: writes once, then skips identical bytes.
	const fs::path dir = fs::temp_directory_path() / "he_typegen_test";
	fs::remove_all(dir);
	fs::create_directories(dir);
	CHECK(HE::writeCppTypesHeader(dir));
	const fs::path file = dir / "Source" / "Generated" / "GameTypes.h";
	REQUIRE(fs::exists(file));
	const auto t0 = fs::last_write_time(file);
	CHECK(HE::writeCppTypesHeader(dir));           // unchanged → no rewrite
	CHECK(fs::last_write_time(file) == t0);
	fs::remove_all(dir);

	reg.removeType(weapon.assetPath);
	reg.removeType(inner.assetPath);
	reg.removeType(outer.assetPath);
}
