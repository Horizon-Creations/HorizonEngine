#include "doctest.h"

#include "McpToolRegistry.h"
#include "ProjectManager.h"

#include <string>
#include <vector>

// ─── Starting a build from outside the editor ────────────────────────────────
// The export itself is a worker thread packing gigabytes, and no test starts
// one. What these tools DECIDE, though, is decidable here, and it is the half
// that goes wrong quietly:
//
//   • which profile a run is made from, and that an override a client sent
//     reaches THIS RUN and never the stored profile. That one is the reason
//     `startFromProfile` takes a value instead of a name: if an override were
//     written back, "just build me a Linux copy once" would become what the
//     human's next Export Project does, and nobody would connect the two.
//   • that an unknown target platform is REFUSED rather than silently exported
//     as Host. `exportPlatformFromName` maps everything it does not know to
//     Host, so a typo is a full host export into a folder named after a
//     platform it is not.
//   • that neither tool starts anything into a Build window the other kind is
//     holding — one window, one run, which is the whole reason Kind exists.
//   • that the status tool returns the TAIL of the log and says so. A toolchain
//     prints thousands of lines, and a tool that returned them all would be a
//     tool nobody can call twice.
//
// The two start hooks are played by lambdas that record what they were handed.
// That is the honest boundary: the panels own the worker, and what this file
// can assert is what the panels are ASKED for.

using HE::Ed::McpBuildHooks;
using HE::Ed::McpBuildLogLine;
using HE::Ed::McpBuildStatus;
using HE::Ed::McpBuildStep;
using HE::Ed::McpTool;
using HE::Ed::McpToolRegistry;
using HE::Ed::ToolResult;
using nlohmann::json;

namespace {

std::string codeOf(const ToolResult& r)
{
	return r.isError ? r.errorCode : std::string("<ok>");
}

struct Fixture
{
	ProjectData     proj;
	McpToolRegistry registry;

	// What the hooks saw.
	bool           haveProject   = true;
	bool           exportStarted = false;
	ExportProfile  started;                 // the profile handed to the panel
	std::string    exportError;             // non-empty = the panel refuses
	bool           gameLogicAvail = false;
	bool           gameLogicStarted = false;
	bool           busy = false;
	McpBuildStatus status;

	Fixture()
	{
		proj.name = "Demo";
		proj.path = "/tmp/he_test_build/Demo.heproj";
		proj.exportProfiles = defaultExportProfiles();
		REQUIRE(proj.exportProfiles.size() >= 2);
		proj.activeExportProfile = proj.exportProfiles[1].name;   // "Shipping"

		McpBuildHooks h;
		h.project = [this]() -> ProjectData* { return haveProject ? &proj : nullptr; };
		h.startExport = [this](const ExportProfile& p, std::string& err) {
			if (!exportError.empty()) { err = exportError; return false; }
			started = p;
			exportStarted = true;
			return true;
		};
		h.gameLogicAvailable  = [this] { return gameLogicAvail; };
		h.startGameLogicBuild = [this](std::string& err) {
			(void)err;
			gameLogicStarted = true;
			return true;
		};
		h.buildRunning = [this] { return busy; };
		h.buildStatus  = [this] { return status; };
		HE::Ed::registerBuildTools(registry, std::move(h));
	}

	ToolResult call(const std::string& name, const json& args = json::object())
	{
		const McpTool* t = registry.find(name);
		REQUIRE_MESSAGE(t != nullptr, "no such tool registered: " << name);
		return t->handler(args);
	}
};

} // namespace

TEST_CASE("build tools: the three tools register with legal names and schemas")
{
	Fixture f;
	for (const char* n : { "project_package", "project_build", "project_build_status" })
	{
		const McpTool* t = f.registry.find(n);
		REQUIRE_MESSAGE(t != nullptr, "missing tool: " << n);
		CHECK(McpToolRegistry::enforceNameRule(t->name));
		CHECK(t->inputSchema.is_object());
		CHECK(t->inputSchema["type"] == "object");
		CHECK_FALSE(t->description.empty());
	}
	// Starting a build is written to the console; reading the window is not.
	CHECK(f.registry.find("project_package")->mutates);
	CHECK(f.registry.find("project_build")->mutates);
	CHECK_FALSE(f.registry.find("project_build_status")->mutates);
}

TEST_CASE("project_package: no project is a named refusal, not a crash")
{
	Fixture f;
	f.haveProject = false;
	const ToolResult r = f.call("project_package");
	CHECK(codeOf(r) == "no_project");
	CHECK_FALSE(f.exportStarted);
}

TEST_CASE("project_package: without a name it runs the project's ACTIVE profile")
{
	Fixture f;
	const ToolResult r = f.call("project_package");
	REQUIRE(codeOf(r) == "<ok>");
	CHECK(f.exportStarted);
	CHECK(f.started.name == f.proj.activeExportProfile);
	CHECK(r.content["started"] == true);
	CHECK(r.content["profile"] == f.proj.activeExportProfile);
	// Deliberately no verdict: at this moment nothing about the outcome exists.
	CHECK_FALSE(r.content.contains("success"));
}

TEST_CASE("project_package: an unknown profile lists the ones that exist")
{
	Fixture f;
	const ToolResult r = f.call("project_package", json{ { "profile", "Release" } });
	CHECK(codeOf(r) == "not_found");
	CHECK(r.errorMessage.find(f.proj.exportProfiles[0].name) != std::string::npos);
	CHECK(r.errorMessage.find(f.proj.exportProfiles[1].name) != std::string::npos);
	CHECK_FALSE(f.exportStarted);
}

TEST_CASE("project_package: overrides reach the RUN and never the stored profile")
{
	Fixture f;
	const std::string name = f.proj.exportProfiles[0].name;   // "Development"
	const ExportProfile before = f.proj.exportProfiles[0];

	const ToolResult r = f.call("project_package", json{
		{ "profile",            name },
		{ "targetPlatform",     "Linux" },
		{ "compress",           !before.compress },
		{ "encrypt",            !before.encrypt },
		{ "incremental",        !before.incremental },
		{ "compileHorizonCode", !before.compileHorizonCode },
		{ "hcStopOnFailure",    !before.hcStopOnFailure },
		{ "outputDir",          "/tmp/he_test_build/out" },
		{ "excludePatterns",    json::array({ "Debug/*", "*_test.hasset" }) },
	});
	REQUIRE(codeOf(r) == "<ok>");

	// …the run got them…
	CHECK(f.started.name            == name);
	CHECK(f.started.targetPlatform  == "Linux");
	CHECK(f.started.compress        == !before.compress);
	CHECK(f.started.encrypt         == !before.encrypt);
	CHECK(f.started.incremental     == !before.incremental);
	CHECK(f.started.compileHorizonCode == !before.compileHorizonCode);
	CHECK(f.started.hcStopOnFailure    == !before.hcStopOnFailure);
	CHECK(f.started.outputDir       == "/tmp/he_test_build/out");
	REQUIRE(f.started.excludePatterns.size() == 2);
	CHECK(f.started.excludePatterns[0] == "Debug/*");

	// …and the PROJECT did not. This is the whole point of the value copy.
	CHECK(f.proj.exportProfiles[0].targetPlatform    == before.targetPlatform);
	CHECK(f.proj.exportProfiles[0].compress          == before.compress);
	CHECK(f.proj.exportProfiles[0].encrypt           == before.encrypt);
	CHECK(f.proj.exportProfiles[0].outputDir         == before.outputDir);
	CHECK(f.proj.exportProfiles[0].excludePatterns   == before.excludePatterns);
	CHECK(f.proj.activeExportProfile == "Shipping");
}

TEST_CASE("project_package: a mistyped platform is refused, not exported as Host")
{
	Fixture f;
	// The trap this guards: exportPlatformFromName maps anything it does not
	// know to Host, so without the round-trip check "linx" would pack host
	// binaries into a folder the caller believes holds a Linux build.
	const ToolResult r = f.call("project_package", json{ { "targetPlatform", "linx" } });
	CHECK(codeOf(r) == "invalid_argument");
	CHECK_FALSE(f.exportStarted);

	// Spelling is not the point — the enum's own name is.
	Fixture g;
	const ToolResult ok = g.call("project_package", json{ { "targetPlatform", "windows" } });
	CHECK(codeOf(ok) == "<ok>");
	CHECK(g.started.targetPlatform == "Windows");
}

TEST_CASE("project_package: a project with no profiles says so")
{
	Fixture f;
	f.proj.exportProfiles.clear();
	const ToolResult r = f.call("project_package");
	CHECK(codeOf(r) == "not_found");
	CHECK_FALSE(f.exportStarted);
}

TEST_CASE("project_package: the panel's own refusal comes through verbatim")
{
	Fixture f;
	f.exportError = "no game runtime found";
	const ToolResult r = f.call("project_package");
	CHECK(codeOf(r) == "failed");
	CHECK(r.errorMessage.find("no game runtime found") != std::string::npos);
}

TEST_CASE("build tools: one window, one run — and the refusal names the kind")
{
	Fixture f;
	f.gameLogicAvail = true;
	f.busy = true;
	f.status.kind    = "gamelogic";
	f.status.running = true;

	const ToolResult a = f.call("project_package");
	CHECK(codeOf(a) == "busy");
	CHECK(a.errorMessage.find("game-logic") != std::string::npos);
	CHECK_FALSE(f.exportStarted);

	f.status.kind = "export";
	const ToolResult b = f.call("project_build");
	CHECK(codeOf(b) == "busy");
	CHECK(b.errorMessage.find("export") != std::string::npos);
	CHECK_FALSE(f.gameLogicStarted);
}

TEST_CASE("project_build: a project with no native game logic is told why")
{
	Fixture f;
	f.gameLogicAvail = false;
	const ToolResult r = f.call("project_build");
	CHECK(codeOf(r) == "unavailable");
	// The message has to be usable: a HorizonCode project is not broken, it just
	// has nothing to compile — and the tool that DOES compile its graphs is the
	// other one.
	CHECK(r.errorMessage.find("project_package") != std::string::npos);
	CHECK_FALSE(f.gameLogicStarted);

	f.gameLogicAvail = true;
	const ToolResult ok = f.call("project_build");
	CHECK(codeOf(ok) == "<ok>");
	CHECK(f.gameLogicStarted);
	CHECK(ok.content["started"] == true);
}

TEST_CASE("project_build_status: nothing built yet is a state, not an error")
{
	Fixture f;
	const ToolResult r = f.call("project_build_status");
	REQUIRE(codeOf(r) == "<ok>");
	CHECK(r.content["hasRun"] == false);
	CHECK(r.content["running"] == false);
	CHECK(r.content["steps"].empty());
	// `success` is absent rather than false: a run that never happened did not
	// fail.
	CHECK_FALSE(r.content.contains("success"));
}

TEST_CASE("project_build_status: a finished run carries the verdict and the target")
{
	Fixture f;
	f.status.hasRun  = true;
	f.status.kind    = "export";
	f.status.steps   = {
		McpBuildStep{ "Set up",  "done",   1.0f, false, "" },
		McpBuildStep{ "Package", "failed", 0.4f, false, "128 / 340" },
	};
	f.status.currentStep = 1;
	f.status.finished = true;
	f.status.success  = false;
	f.status.message  = "Error: no game runtime found";
	f.status.executable = "/tmp/out/HorizonGame";
	f.status.runnableHere = true;
	f.status.interpretedHeadline = "2 of 16 classes ship interpreted";
	f.status.interpretedClasses  = { { "Enemy.hasset", "exec cycle at node 12" } };

	const ToolResult r = f.call("project_build_status");
	REQUIRE(codeOf(r) == "<ok>");
	CHECK(r.content["finished"] == true);
	CHECK(r.content["success"] == false);
	CHECK(r.content["message"] == "Error: no game runtime found");
	CHECK(r.content["executable"] == "/tmp/out/HorizonGame");
	CHECK(r.content["runnableHere"] == true);
	CHECK(r.content["steps"][1]["state"] == "failed");
	CHECK(r.content["steps"][1]["detail"] == "128 / 340");
	// The one thing a SUCCESSFUL export is quietly worth less for travels with
	// the verdict rather than being left in the log.
	REQUIRE(r.content.contains("interpreted"));
	CHECK(r.content["interpreted"]["classes"][0]["class"] == "Enemy.hasset");
}

TEST_CASE("project_build_status: the log is the TAIL, filtered, and says it was cut")
{
	Fixture f;
	f.status.hasRun = true;
	for (int i = 0; i < 500; ++i)
		f.status.log.push_back(McpBuildLogLine{ 1, 0, "line " + std::to_string(i) });
	f.status.log.push_back(McpBuildLogLine{ 1, 2, "error: undefined symbol" });
	f.status.log.push_back(McpBuildLogLine{ 0, 1, "warning: unused" });

	SUBCASE("default keeps the last hundred")
	{
		const ToolResult r = f.call("project_build_status");
		REQUIRE(codeOf(r) == "<ok>");
		CHECK(r.content["log"].size() == 100);
		CHECK(r.content["logLinesMatched"] == 502);
		CHECK(r.content["logTruncated"] == true);
		// The tail, so the thing that went wrong is in it.
		CHECK(r.content["log"].back()["text"] == "warning: unused");
	}
	SUBCASE("minSeverity 2 is the compiler's own message without the 500 that worked")
	{
		const ToolResult r = f.call("project_build_status", json{ { "minSeverity", 2 } });
		REQUIRE(codeOf(r) == "<ok>");
		REQUIRE(r.content["log"].size() == 1);
		CHECK(r.content["log"][0]["text"] == "error: undefined symbol");
		CHECK(r.content["logTruncated"] == false);
	}
	SUBCASE("one step only")
	{
		const ToolResult r = f.call("project_build_status",
		                            json{ { "step", 0 }, { "maxLines", 50 } });
		REQUIRE(codeOf(r) == "<ok>");
		REQUIRE(r.content["log"].size() == 1);
		CHECK(r.content["log"][0]["text"] == "warning: unused");
	}
	SUBCASE("log can be left out entirely")
	{
		const ToolResult r = f.call("project_build_status", json{ { "log", false } });
		REQUIRE(codeOf(r) == "<ok>");
		CHECK_FALSE(r.content.contains("log"));
	}
	SUBCASE("maxLines is clamped rather than obeyed into a megabyte")
	{
		const ToolResult r = f.call("project_build_status", json{ { "maxLines", 100000 } });
		REQUIRE(codeOf(r) == "<ok>");
		CHECK(r.content["log"].size() == 502);   // everything there is, capped at 2000
	}
}
