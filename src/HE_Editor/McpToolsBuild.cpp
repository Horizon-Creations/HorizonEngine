#include "McpToolCommon.h"
#include "McpToolRegistry.h"

#include "ProjectManager.h"            // ProjectData, ExportProfile
#include <Hpak/ProjectExporter.h>      // exportPlatformName / exportPlatformFromName

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace HE::Ed
{
namespace
{
using json = nlohmann::json;

// ─── Refusals ────────────────────────────────────────────────────────────────
// The same wire vocabulary the other families use, plus two of this family's
// own: `busy` (the one Build window is taken) and `unavailable` (the menu row
// would be greyed out for a human too).
ToolResult failNoProject()
{
	return ToolResult::fail("no_project",
		"no project is open — the editor starts without one, and nothing can be "
		"built until File > Open Project or File > New Project has run");
}

std::string lowered(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

// ─── The Build window, as JSON ───────────────────────────────────────────────
json stepsToJson(const std::vector<McpBuildStep>& steps)
{
	json out = json::array();
	for (const McpBuildStep& s : steps)
		out.push_back(json{
			{ "name",          s.name },
			{ "state",         s.state },
			// The progress number is meaningless while a step is indeterminate —
			// a cmake configure phase genuinely has no length — so it is reported
			// with the flag that says so rather than as a plausible 0.0.
			{ "progress",      s.progress },
			{ "indeterminate", s.indeterminate },
			{ "detail",        s.detail },
		});
	return out;
}

json statusToJson(const McpBuildStatus& st, bool withLog)
{
	json j{
		{ "hasRun",      st.hasRun },
		{ "running",     st.running },
		{ "kind",        st.kind },
		{ "steps",       stepsToJson(st.steps) },
		{ "currentStep", st.currentStep },
		{ "activity",    st.activity },
		{ "finished",    st.finished },
	};
	// `success` and `message` only mean something once a run has ENDED. Reporting
	// success:false while a build is happily running would read as a failure.
	if (st.finished)
	{
		j["success"] = st.success;
		j["message"] = st.message;
		if (!st.executable.empty())
		{
			j["executable"]   = st.executable;
			j["runnableHere"] = st.runnableHere;
		}
	}
	if (!st.interpretedHeadline.empty() || !st.interpretedClasses.empty())
	{
		json cls = json::array();
		for (const auto& [label, reason] : st.interpretedClasses)
			cls.push_back(json{ { "class", label }, { "reason", reason } });
		j["interpreted"] = json{
			{ "headline", st.interpretedHeadline },
			{ "classes",  std::move(cls) },
		};
	}
	if (withLog)
	{
		json lines = json::array();
		for (const McpBuildLogLine& l : st.log)
			lines.push_back(json{
				{ "step",     l.step },
				{ "severity", l.severity },
				{ "text",     l.text },
			});
		j["log"] = std::move(lines);
	}
	return j;
}

// ─── Reading the Build window before starting something into it ──────────────
// One window, one run at a time (BuildProgressDialog::Kind exists for exactly
// this). The refusal names the kind that holds it, because "busy" without that
// leaves a client polling the wrong tool.
ToolResult failBusy(const McpBuildHooks& h)
{
	std::string kind = "another build";
	if (h.buildStatus)
	{
		const McpBuildStatus st = h.buildStatus();
		if (st.kind == "export")    kind = "a project export";
		if (st.kind == "gamelogic") kind = "a game-logic build";
	}
	return ToolResult::fail("busy",
		kind + " is already running in the Build window — poll "
		"project_build_status until `running` is false");
}

// ─── project_package ─────────────────────────────────────────────────────────
void addPackage(McpToolRegistry& registry, const McpBuildHooks& h)
{
	McpTool t;
	t.name = "project_package";
	t.description =
		"Start Build > Export Project: pack this project into a runnable build "
		"(the .hpak plus a game runtime). Returns as soon as the run STARTED — "
		"the verdict, the steps and the log arrive through project_build_status. "
		"Every argument besides `profile` overrides that profile's stored value "
		"FOR THIS RUN ONLY; nothing is written back to the project.";
	t.mutates = true;
	t.inputSchema = objectSchema(json{
		{ "profile", stringProp(
			"Name of the export profile to run. Omitted = the project's active "
			"profile. settings_get with scope 'project' lists them.") },
		{ "targetPlatform", json{
			{ "type", "string" },
			{ "enum", json::array({ "Host", "Windows", "macOS", "Linux" }) },
			{ "description",
			  "Host = this machine (runtime from ../Game). The three named ones "
			  "need a prebuilt runtime bundle in ../GameRuntimes/<name>." } } },
		{ "outputDir", stringProp(
			"Absolute output directory. Omitted = the profile's, which defaults "
			"to <ProjectRoot>/Export/<profile name>.") },
		{ "startupScene", stringProp(
			"Project-relative path of the .hescene to boot into. Omitted = the "
			"profile's choice, which itself falls back to the scene open in the "
			"editor. Ignored for an application project, which ships no scene.") },
		{ "compress", json{ { "type", "boolean" },
			{ "description", "Compress the pak (zstd)." } } },
		{ "encrypt", json{ { "type", "boolean" },
			{ "description", "Encrypt the pak (AES)." } } },
		{ "enableModSupport", json{ { "type", "boolean" },
			{ "description", "Let the shipped game load loose files over the pak." } } },
		{ "incremental", json{ { "type", "boolean" },
			{ "description",
			  "Reuse unchanged assets from the previous export. Falls back to a "
			  "full pack by itself when the manifest does not match." } } },
		{ "appBundle", json{ { "type", "boolean" },
			{ "description",
			  "macOS: emit a .app bundle. Ignored when the target does not "
			  "produce macOS binaries." } } },
		{ "compileHorizonCode", json{ { "type", "boolean" },
			{ "description",
			  "Translate HorizonCode graphs to native C++ in the packaged build. "
			  "Needs cmake and a C++ toolchain, and only runs for a Host target." } } },
		{ "hcStopOnFailure", json{ { "type", "boolean" },
			{ "description",
			  "What a graph that cannot be compiled means: false ships it "
			  "interpreted (compiling is then an optimisation and never a gate), "
			  "true fails the export instead." } } },
		{ "excludePatterns", json{
			{ "type", "array" },
			{ "items", json{ { "type", "string" } } },
			{ "description",
			  "Content-relative globs kept OUT of the pak, e.g. 'Debug/*'. "
			  "Replaces the profile's list for this run." } } },
	}, {});

	McpBuildHooks hooks = h;
	t.handler = [hooks](const json& args) -> ToolResult
	{
		ProjectData* proj = hooks.project ? hooks.project() : nullptr;
		if (!proj) return failNoProject();
		if (!hooks.startExport)
			return ToolResult::fail("unavailable", "this editor cannot start an export");
		if (hooks.buildRunning && hooks.buildRunning()) return failBusy(hooks);

		if (proj->exportProfiles.empty())
			return ToolResult::fail("not_found",
				"this project has no export profiles — open Build > Export Project "
				"once, which seeds the two defaults");

		// Which profile. An unknown name lists what there is: a client that
		// guessed "Release" should read "Development, Shipping" rather than
		// guess again.
		const std::string want = strArg(args, "profile");
		const ExportProfile* found = nullptr;
		if (want.empty())
		{
			for (const ExportProfile& p : proj->exportProfiles)
				if (p.name == proj->activeExportProfile) { found = &p; break; }
			if (!found) found = &proj->exportProfiles.front();
		}
		else
		{
			for (const ExportProfile& p : proj->exportProfiles)
				if (p.name == want) { found = &p; break; }
			if (!found)
			{
				std::string names;
				for (const ExportProfile& p : proj->exportProfiles)
				{
					if (!names.empty()) names += ", ";
					names += p.name;
				}
				return ToolResult::fail("not_found",
					"no export profile named '" + want + "' — this project has: " + names);
			}
		}

		// The copy every override lands in. The stored profile is never touched:
		// a one-off "build me a Linux copy" must not become what the human's next
		// Export Project does.
		ExportProfile p = *found;

		if (hasArg(args, "targetPlatform"))
		{
			const std::string raw = strArg(args, "targetPlatform");
			// Matched against the enum's OWN names rather than handed to
			// exportPlatformFromName, which maps everything it does not recognise
			// to Host: a mistyped "linx" would otherwise pack host binaries into a
			// folder the caller believes holds a Linux build, and nothing would
			// say so. Case-insensitive, because "linux" is not a typo — but the
			// value stored is the canonical spelling, which is the only one
			// exportPlatformFromName will recognise on the other side.
			// Global scope, spelled out: this file lives in namespace HE::Ed, and
			// an unqualified name would be looked up in HE first.
			std::string canon;
			for (const ::ExportPlatform e : { ::ExportPlatform::Host,
			                                  ::ExportPlatform::Windows,
			                                  ::ExportPlatform::MacOS,
			                                  ::ExportPlatform::Linux })
				if (lowered(::exportPlatformName(e)) == lowered(raw))
				{ canon = ::exportPlatformName(e); break; }
			if (canon.empty())
				return ToolResult::fail("invalid_argument",
					"unknown targetPlatform '" + raw + "' — expected Host, Windows, "
					"macOS or Linux");
			p.targetPlatform = canon;
		}
		if (hasArg(args, "outputDir"))          p.outputDir          = strArg(args, "outputDir");
		if (hasArg(args, "startupScene"))       p.startupScene       = strArg(args, "startupScene");
		if (hasArg(args, "compress"))           p.compress           = boolArg(args, "compress", p.compress);
		if (hasArg(args, "encrypt"))            p.encrypt            = boolArg(args, "encrypt", p.encrypt);
		if (hasArg(args, "enableModSupport"))   p.enableModSupport   = boolArg(args, "enableModSupport", p.enableModSupport);
		if (hasArg(args, "incremental"))        p.incremental        = boolArg(args, "incremental", p.incremental);
		if (hasArg(args, "appBundle"))          p.appBundle          = boolArg(args, "appBundle", p.appBundle);
		if (hasArg(args, "compileHorizonCode")) p.compileHorizonCode = boolArg(args, "compileHorizonCode", p.compileHorizonCode);
		if (hasArg(args, "hcStopOnFailure"))    p.hcStopOnFailure    = boolArg(args, "hcStopOnFailure", p.hcStopOnFailure);
		if (args.contains("excludePatterns") && args["excludePatterns"].is_array())
		{
			p.excludePatterns.clear();
			for (const json& e : args["excludePatterns"])
				if (e.is_string()) p.excludePatterns.push_back(e.get<std::string>());
		}

		std::string err;
		if (!hooks.startExport(p, err))
			return ToolResult::fail("failed",
				err.empty() ? std::string("the editor refused to start the export") : err);

		// Deliberately no verdict here: at this moment the worker has been handed
		// the job and nothing about its outcome exists yet.
		return ToolResult::ok(json{
			{ "started",        true },
			{ "profile",        p.name },
			{ "targetPlatform", p.targetPlatform },
			{ "outputDir",      p.outputDir },
			{ "compileHorizonCode", p.compileHorizonCode },
			{ "note", "poll project_build_status — `running` goes false and "
			          "`finished` true when the export is over" },
		});
	};
	registry.add(std::move(t));
}

// ─── project_build ───────────────────────────────────────────────────────────
void addGameLogicBuild(McpToolRegistry& registry, const McpBuildHooks& h)
{
	McpTool t;
	t.name = "project_build";
	t.description =
		"Start Build > Build and Reload Game Logic: compile this C++ project's "
		"native GameLogic module and, if a preview is running, swap it in without "
		"ending the session. Returns as soon as the compile STARTED; the verdict "
		"arrives through project_build_status. Only a C++ project with a "
		"Source/CMakeLists.txt has this — for every other scripting language the "
		"menu row is greyed out and so is this tool.";
	t.mutates = true;
	t.inputSchema = objectSchema(json::object(), {});

	McpBuildHooks hooks = h;
	t.handler = [hooks](const json&) -> ToolResult
	{
		ProjectData* proj = hooks.project ? hooks.project() : nullptr;
		if (!proj) return failNoProject();
		if (!hooks.gameLogicAvailable || !hooks.gameLogicAvailable())
			return ToolResult::fail("unavailable",
				"this project has no native game logic to build — it needs to be a "
				"C++ project with a Source/CMakeLists.txt. HorizonCode, Lua and "
				"Python projects are interpreted and need no build step; "
				"project_package compiles HorizonCode when asked to.");
		if (!hooks.startGameLogicBuild)
			return ToolResult::fail("unavailable", "this editor cannot start a build");
		if (hooks.buildRunning && hooks.buildRunning()) return failBusy(hooks);

		std::string err;
		if (!hooks.startGameLogicBuild(err))
			return ToolResult::fail("failed",
				err.empty() ? std::string("the editor refused to start the build") : err);

		return ToolResult::ok(json{
			{ "started", true },
			{ "note", "poll project_build_status — the compile runs on a worker, "
			          "and the module swap happens on the frame after it ends" },
		});
	};
	registry.add(std::move(t));
}

// ─── project_build_status ────────────────────────────────────────────────────
void addStatus(McpToolRegistry& registry, const McpBuildHooks& h)
{
	McpTool t;
	t.name = "project_build_status";
	t.description =
		"What the Build window shows for the last run started by project_package "
		"or project_build: the steps and their state, what the running one is "
		"doing, and once it is over the verdict, the executable it produced and "
		"the HorizonCode classes that had to ship interpreted. The log is the "
		"TAIL — a toolchain prints thousands of lines and this interface will not "
		"carry them all.";
	t.mutates = false;
	t.inputSchema = objectSchema(json{
		{ "log", json{ { "type", "boolean" },
			{ "description", "Include log lines at all (default true)." } } },
		{ "maxLines", json{ { "type", "integer" },
			{ "description",
			  "How many log lines from the END of the run (default 100, max 2000)." } } },
		{ "minSeverity", json{ { "type", "integer" },
			{ "description",
			  "0 = everything (default), 1 = warnings and errors, 2 = errors only. "
			  "Start at 2 when a build failed: that is the compiler's own message "
			  "without the thousand lines that succeeded." } } },
		{ "step", json{ { "type", "integer" },
			{ "description",
			  "Only this step's log (index into `steps`). Omitted = every step." } } },
	}, {});

	McpBuildHooks hooks = h;
	t.handler = [hooks](const json& args) -> ToolResult
	{
		if (!hooks.buildStatus)
			return ToolResult::fail("unavailable", "this editor has no build window");

		McpBuildStatus st = hooks.buildStatus();

		const bool withLog  = boolArg(args, "log", true);
		int maxLines        = std::clamp(intArg(args, "maxLines", 100), 1, 2000);
		const int minSev    = std::clamp(intArg(args, "minSeverity", 0), 0, 2);
		const bool oneStep  = hasArg(args, "step");
		const int  wantStep = intArg(args, "step", -1);

		std::size_t total = st.log.size();
		if (withLog)
		{
			std::vector<McpBuildLogLine> kept;
			for (const McpBuildLogLine& l : st.log)
			{
				if (l.severity < minSev) continue;
				if (oneStep && l.step != wantStep) continue;
				kept.push_back(l);
			}
			total = kept.size();
			// The TAIL, not the head: what went wrong is at the end of a build log.
			if (static_cast<int>(kept.size()) > maxLines)
				kept.erase(kept.begin(),
				           kept.end() - static_cast<std::ptrdiff_t>(maxLines));
			st.log = std::move(kept);
		}
		else
			st.log.clear();

		json j = statusToJson(st, withLog);
		if (withLog)
		{
			j["logLinesMatched"]  = static_cast<std::uint64_t>(total);
			j["logLinesReturned"] = static_cast<std::uint64_t>(st.log.size());
			j["logTruncated"]     = st.log.size() < total;
		}
		return ToolResult::ok(std::move(j));
	};
	registry.add(std::move(t));
}

} // namespace

void registerBuildTools(McpToolRegistry& registry, McpBuildHooks hooks)
{
	addPackage(registry, hooks);
	addGameLogicBuild(registry, hooks);
	addStatus(registry, hooks);
}

} // namespace HE::Ed
