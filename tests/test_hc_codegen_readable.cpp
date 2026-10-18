// What the HorizonCode → C++ codegen writes has to read as the graph (Thema 211
// step 7): classes, members, functions, events and parameters under their own
// names, Engine Call nodes as direct HE::api calls through
// HorizonEngine/HorizonEngineApi.h, and the Runtime's by-name machinery in a
// file of its own. test_horizoncode_codegen.cpp proves the code does what the
// interpreter does; this file pins how it LOOKS.
//
// The golden files in fixtures/hcodegen_golden/ are the generator's output for
// three parity fixtures. After a deliberate change to the emitter, regenerate:
//   HE_UPDATE_HCGEN_GOLDEN=1 he_tests --source-file='*test_hc_codegen_readable.cpp'
// and read the diff: it is the review of what generated code now looks like.
#include "doctest.h"
#include "fixtures.h"   // tests/fixtures/hcodegen — the shared graphs
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/HcCodegen.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace
{
	const fs::path kGolden = HE_TEST_HCGEN_GOLDEN_DIR;

	// The fixtures, generated the way hc_codegen --fixtures does it.
	const std::map<std::string, std::string>& generated()
	{
		static const std::map<std::string, std::string> files = []
		{
			HE::hccg::Options opt;
			opt.engineVersion = "dev";
			opt.onFailure     = HE::hccg::OnFailure::Stop;
			const HE::hccg::Result r = HE::hccg::generate(hcfix::all(), opt);
			REQUIRE(r.ok);
			std::map<std::string, std::string> out;
			for (const auto& f : r.files) out[f.name] = f.contents;
			return out;
		}();
		return files;
	}

	std::string readText(const fs::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	bool isClassFile(const std::string& name)
	{
		return name.rfind("hcgen_", 0) == 0 && name.rfind("hcgen_type", 0) != 0 &&
		       name != "hcgen.h";
	}
}

TEST_CASE("codegen readable: three fixtures come out exactly as the golden files")
{
	const char* files[] = {
		"hcgen_AnimatorSync.cpp",       // direct engine calls, Ctx through a Scope
		"hcgen_EngineExecCached.cpp",   // exec rows, results into named RunState fields
		"hcgen_FunctionsBasic.cpp",     // functions under their own names and parameters
		"hcgen_FunctionsBasic.h",
	};
	const bool update = std::getenv("HE_UPDATE_HCGEN_GOLDEN") != nullptr;
	for (const char* name : files)
	{
		INFO("file ", name);
		REQUIRE(generated().count(name) == 1);
		const std::string& text = generated().at(name);
		if (update)
		{
			std::ofstream out(kGolden / name, std::ios::binary | std::ios::trunc);
			out << text;
			MESSAGE("rewrote " << (kGolden / name).string());
		}
		CHECK(text == readText(kGolden / name));
	}
}

TEST_CASE("codegen readable: no generated file uses the old cryptic forms")
{
	// What step 1 of Thema 211 listed as in the way: kind prefixes, node ids in
	// names, positional parameters, boxed argument vectors per engine call.
	const std::pair<const char*, const char*> banned[] = {
		{ R"(\bv_[A-Za-z])",                "a v_ member" },
		{ R"(\bl_[A-Za-z])",                "an l_ local" },
		{ R"(\bf_[A-Za-z])",                "an f_ function body" },
		{ R"(\bev_[A-Za-z])",               "an ev_<name>_<id> event body" },
		{ R"(\bia_[A-Za-z])",               "an ia_ input body" },
		{ R"(\bresume_[0-9])",              "a resume_<id> continuation" },
		{ R"(\bn[0-9]+_(o[0-9]+|state)\b)", "an n<id>_ field" },
		{ R"(\b[ar][0-9]+(_[0-9]+)?\b)",    "an a<id>/r<id> temporary" },
		// Positional lists always had a _0; an authored `r` made unique is r_2.
		{ R"(\b[pr]_0\b)",                  "a positional p_/r_ parameter" },
		{ R"(\bC_[A-Za-z])",                "a C_ class name" },
	};
	size_t classFiles = 0;
	for (const auto& [name, text] : generated())
	{
		if (!isClassFile(name)) continue;
		++classFiles;
		for (const auto& [pattern, what] : banned)
		{
			std::smatch m;
			const bool found = std::regex_search(text, m, std::regex(pattern));
			CHECK_MESSAGE(!found, name, " still has ", what, ": '", (found ? m.str() : ""), "'");
		}
	}
	CHECK(classFiles >= 3 * 49);   // .h + .cpp + .reflect.cpp per fixture class
}

TEST_CASE("codegen readable: Engine Call nodes are direct calls, the dispatch only where it must be")
{
	size_t direct = 0;
	for (const auto& [name, text] : generated())
	{
		if (!isClassFile(name)) continue;
		// Every remaining dispatch names a row that cannot be called directly.
		static const std::regex dispatch(R"re(hc::callApi\(m_ctx, "([A-Za-z0-9_.]+)")re");
		for (auto it = std::sregex_iterator(text.begin(), text.end(), dispatch);
		     it != std::sregex_iterator(); ++it)
			CHECK_MESSAGE(!HE::hccg::callsDirectly((*it)[1].str()), name, ": ", (*it)[1].str(),
			              " is dispatched although it has a direct form");
		for (size_t at = text.find("HE::api::"); at != std::string::npos;
		     at = text.find("HE::api::", at + 1))
			++direct;
		// The API header only where it is used, and then in the logic file.
		const bool calls = text.find("HE::api::") != std::string::npos;
		const bool includes = text.find("#include <HorizonEngine/HorizonEngineApi.h>") != std::string::npos;
		CHECK_MESSAGE(calls == includes, name);
		if (calls) CHECK_MESSAGE(name.find(".reflect.cpp") == std::string::npos, name);
	}
	CHECK(direct > 50);
}

TEST_CASE("codegen readable: the logic and the Runtime's by-name machinery are two files")
{
	const std::string& logic   = generated().at("hcgen_FunctionsBasic.cpp");
	const std::string& reflect = generated().at("hcgen_FunctionsBasic.reflect.cpp");
	for (const char* runtimeSide : { "::fireEvent(", "::callFunction(", "::slots()", "::classKey()",
	                                 "create_FunctionsBasic", "funcInfos()" })
	{
		CHECK_MESSAGE(logic.find(runtimeSide) == std::string::npos, runtimeSide);
		CHECK_MESSAGE(reflect.find(runtimeSide) != std::string::npos, runtimeSide);
	}
	// The graph's functions are defined in the logic file, under their names.
	CHECK(logic.find("void FunctionsBasic::Sum(RunState& rs, float a, float b, float& total)")
	      != std::string::npos);
	CHECK(reflect.find("void FunctionsBasic::Sum(") == std::string::npos);
}
