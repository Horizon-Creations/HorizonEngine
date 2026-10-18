// The editor's "Compile HorizonCode" export all the way through, with a real
// compiler (Thema 211 step 7).
//
// Generated code calls the engine directly (HorizonEngine/HorizonEngineApi.h),
// so the library it builds into has to find HorizonScene's headers and link
// HorizonScene — something the parity tests cannot see, because he_tests
// compiles the generated classes with the engine's own include paths. This one
// takes the export's path: generate() + generateCMakeLists(), the SDK resolved
// from a he_sdk_config.json exactly like the one the editor writes beside
// itself, buildDylib(), and CompiledClassTable to load the result. Then the
// loaded class runs against a real world next to the interpreter on a twin, and
// both have to leave the same variables and the same world behind.
//
// Skipped rather than failed without a toolchain, like test_gamelogic_build.cpp.
#include "doctest.h"
#include "TestFsUtil.h"
#include "fixtures.h"   // tests/fixtures/hcodegen — the shared graphs
#include <HorizonCode/HcCompiledLoader.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/HcCodegen.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace HorizonCode;

namespace
{
	void writeFile(const fs::path& p, const std::string& text)
	{
		std::ofstream f(p, std::ios::binary | std::ios::trunc);
		f << text;
	}

	std::string jsonPath(const fs::path& p)
	{
		std::string s = p.generic_string();
		std::string out;
		for (const char c : s) { if (c == '"' || c == '\\') out += '\\'; out += c; }
		return "\"" + out + "\"";
	}

	// One side of the comparison: a world with the instance's entity in it, and
	// a Runtime whose engine services build their Ctx from that world — the
	// interpreter's callApi through the registry's invoke, generated code's
	// direct calls through apiCtx, the one builder for both (as the game has).
	struct Side
	{
		HorizonWorld world;
		Runtime      rt;
		Entity       owner = entt::null;
		InstanceId   id = 0;

		Side()
		{
			owner = world.createEntity("Probe");
			world.registry().emplace<TransformComponent>(owner);
			Runtime::Services s;
			s.callApi = [this](InstanceId self, const std::string& apiId,
			                   const std::vector<Value>& args) -> std::vector<Value>
			{
				const HE::api::ApiFn* fn = HE::api::find(apiId);
				if (!fn) return {};
				HE::api::Ctx c = ctx(self);
				return fn->invoke(c, args);
			};
			s.apiCtx = [this](InstanceId self, HE::api::Ctx& out) { out = ctx(self); };
			rt.setServices(std::move(s));
		}
		HE::api::Ctx ctx(InstanceId self)
		{
			HE::api::Ctx c;
			c.world   = &world;
			c.runtime = &rt;
			c.self    = self;
			return c;
		}
		void own() { rt.setOwnedEntity(id, static_cast<uint32_t>(owner)); }
		glm::vec3 position(Entity e) { return world.registry().get<TransformComponent>(e).position; }
	};
}

TEST_CASE("Codegen export build: classes with Engine Calls build against the SDK, load, and match the interpreter")
{
	const fs::path engineRoot = HE_TEST_ENGINE_ROOT;
	REQUIRE(!engineRoot.empty());
	const HE::hccg::ToolchainProbe probe = HE::hccg::probeToolchain();
	if (!probe.cmakeFound || !probe.compilerFound)
	{
		MESSAGE("no C++ toolchain on this machine (cmake: " << probe.cmakeFound
		        << ", compiler: " << probe.compilerFound
		        << ") — the export build cannot be exercised here");
		return;
	}

	// The SDK the way a development editor finds it: the JSON beside the
	// editor binary, nothing staged, no override.
	std::string savedEnv;
	const bool hadEnv = std::getenv("HE_HCGEN_SDK") != nullptr;
	if (hadEnv) savedEnv = std::getenv("HE_HCGEN_SDK");
#if defined(_WIN32)
	_putenv_s("HE_HCGEN_SDK", "");
#else
	unsetenv("HE_HCGEN_SDK");
#endif
	const fs::path root = fs::temp_directory_path() / "he_hc_codegen_export_build";
	he_test::removeAllQuiet(root);
	const fs::path editorDir = root / "editor";
	const fs::path genDir    = root / "gen";
	fs::create_directories(editorDir);
	fs::create_directories(genDir);
	writeFile(editorDir / "he_sdk_config.json",
	          "{\n  \"includeDirs\": [" + jsonPath(engineRoot / "src" / "HE_Core" / "include") + ", " +
	          jsonPath(engineRoot / "src" / "HE_Scene" / "include") + ", " +
	          jsonPath(HE_TEST_GLM_DIR) + "],\n  \"libDir\": " + jsonPath(HE_TEST_CORE_LIB_DIR) +
	          ",\n  \"libDirs\": [" + jsonPath(HE_TEST_SCENE_LIB_DIR) + "]\n}\n");
	const HE::hccg::SdkInfo sdk = HE::hccg::resolveSdk(editorDir);
	REQUIRE(sdk.valid());
	REQUIRE(sdk.extraLibDirs.size() == 1);

	// What the export writes (ExportDialogPanel): the classes, their manifest,
	// and the CMake project around them.
	HE::hccg::Options opt;
	opt.engineVersion = "export-build-test";
	opt.onFailure     = HE::hccg::OnFailure::Stop;
	const HE::hccg::Result r = HE::hccg::generate({ hcfix::fxEngineWorld(), hcfix::fxAnimatorSync() }, opt);
	REQUIRE(r.ok);
	std::vector<std::string> cppFiles;
	bool direct = false;
	for (const auto& f : r.files)
	{
		writeFile(genDir / f.name, f.contents);
		if (fs::path(f.name).extension() == ".cpp") cppFiles.push_back(f.name);
		direct = direct || f.contents.find("#include <HorizonEngine/HorizonEngineApi.h>") != std::string::npos;
	}
	REQUIRE(direct);   // the point: code that needs HorizonScene
	writeFile(genDir / "CMakeLists.txt", HE::hccg::generateCMakeLists(opt, cppFiles));

	std::string lastError;
	const HE::hccg::BuildOutcome out = HE::hccg::buildDylib(genDir, sdk,
		[&lastError](const std::string& line)
		{
			if (line.find("rror") != std::string::npos) lastError = line;
		});
	INFO("build message: " << out.message << "  |  last error line: " << lastError);
	REQUIRE(out.ok);

	HorizonCode::CompiledClassTable table;
	REQUIRE(table.load(out.artifact, opt.engineVersion));
	CHECK(table.find("fix/animator_sync") != nullptr);
	const CompiledClassEntry* entry = table.find("fix/engine_world");
	REQUIRE(entry != nullptr);

	{
		Side interp, comp;
		interp.id = interp.rt.add(hcfix::fxEngineWorld().graph);
		comp.id = comp.rt.addCompiled(CompiledPtr(entry->create(), CompiledDeleter{ entry->destroy }));
		REQUIRE(interp.id != 0);
		REQUIRE(comp.id != 0);
		interp.own();
		comp.own();
		interp.rt.fireEvent(interp.id, "Act");
		comp.rt.fireEvent(comp.id, "Act");

		// The variables: what the reads saw after the writes.
		const auto vi = interp.rt.variablesSnapshot(interp.id);
		const auto vc = comp.rt.variablesSnapshot(comp.id);
		REQUIRE(vi.size() == vc.size());
		CHECK(vc.at("pos").v3 == glm::vec3(1.0f, 2.0f, 3.0f));      // a real world answered
		CHECK(vc.at("scale").v3 == glm::vec3(2.0f, 2.0f, 2.0f));
		CHECK(vc.at("name").s == "Probe");
		CHECK(vc.at("spawned").i != 0);
		CHECK(vc.at("found").i == vc.at("spawned").i);
		for (const auto& [name, v] : vi)
		{
			INFO("variable " << name);
			REQUIRE(vc.count(name) == 1);
			const Value& c = vc.at(name);
			CHECK(c.type == v.type);
			CHECK(c.i == v.i);
			CHECK(c.f == v.f);
			CHECK(c.s == v.s);
			CHECK(c.v3 == v.v3);
		}
		// The worlds: the same writes landed in both.
		CHECK(comp.position(comp.owner) == interp.position(interp.owner));
		// entity.spawn makes a bare entity (no TransformComponent), so the
		// fixture's setPosition on it lands nowhere — on both sides alike.
		const Entity spawned = static_cast<Entity>(static_cast<uint32_t>(vc.at("spawned").i));
		REQUIRE(comp.world.registry().valid(spawned));
		REQUIRE(interp.world.registry().valid(spawned));
		const auto* tc = comp.world.registry().try_get<TransformComponent>(spawned);
		const auto* ti = interp.world.registry().try_get<TransformComponent>(spawned);
		CHECK((tc != nullptr) == (ti != nullptr));
		if (tc && ti) CHECK(tc->position == ti->position);
	}

#if defined(_WIN32)
	_putenv_s("HE_HCGEN_SDK", hadEnv ? savedEnv.c_str() : "");
#else
	if (hadEnv) setenv("HE_HCGEN_SDK", savedEnv.c_str(), 1);
#endif
	he_test::removeAllQuiet(root);
}
