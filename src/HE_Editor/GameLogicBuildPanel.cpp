#include "GameLogicBuildPanel.h"
#include "BuildProgressDialog.h"
#include "EditorApplication.h"      // AppContext
#include "NotificationStore.h"
#include <Diagnostics/Logger.h>
#include <HorizonScene/HcCodegen.h>

#include <atomic>
#include <mutex>
#include <thread>

#include <SDL3/SDL_filesystem.h>

namespace GameLogicBuildPanel
{
namespace
{
namespace Build = BuildProgressDialog::Build;

// ── The run ─────────────────────────────────────────────────────────────────
// One at a time — the Build window holds one run, and two cmake processes in the
// same build directory would fight over the same cache anyway.
std::thread           s_thread;
std::atomic<bool>     s_running{false};
// Raised by the worker when it is done and nothing else; the UI thread picks it
// up in render(), joins, and only THEN touches the world. See the header for why
// the reload cannot live on the worker.
std::atomic<bool>     s_finished{false};
std::mutex            s_resultMutex;
bool                  s_ok = false;
std::string           s_message;
std::filesystem::path s_artifact;

// Asked for in render() (menu action or the window's "Build Again"), started at
// the end of it — a run must not begin inside the popup it is about to reopen.
bool                  s_startRequest = false;

std::filesystem::path editorBaseDir()
{
	const char* base = SDL_GetBasePath();
	return base ? std::filesystem::path(base) : std::filesystem::path{};
}

// Severity read off the toolchain's own words, so a compiler diagnostic is red
// in the log view without anything having to parse the compiler properly.
int severityOf(const std::string& line)
{
	if (line.find("error") != std::string::npos)   return 2;
	if (line.find("warning") != std::string::npos) return 1;
	return 0;
}

} // namespace

bool available(const AppContext& ctx)
{
	if (!ctx.projectManager || !ctx.projectLoaded) return false;
	const auto& proj = ctx.projectManager->currentProject();
	if (proj.scriptLanguage != ProjectScriptLanguage::Cpp) return false;
	std::error_code ec;
	return std::filesystem::exists(
		HE::hccg::gameLogicSourceDir(proj.path) / "CMakeLists.txt", ec);
}

bool running() { return s_running.load(); }

void joinPendingBuild()
{
	if (s_thread.joinable()) s_thread.join();
}

void start(AppContext& ctx)
{
	if (s_running.load()) return;
	// The Build window belongs to one run. An export in flight keeps it, and
	// starting a compile that reported into a window showing someone else's
	// steps would be worse than saying no.
	if (Build::running())
	{
		HE::Ed::notify(HE::Ed::NoteLevel::Warning,
			"A build is already running",
			"Wait for the Build window to finish before building the game logic.");
		return;
	}
	if (!available(ctx))
	{
		HE::Ed::notify(HE::Ed::NoteLevel::Warning,
			"No C++ game logic in this project",
			"Build and Reload compiles a C++ project's Source/ folder. This project "
			"scripts its gameplay in another language.");
		return;
	}

	const std::string projectFile = ctx.projectManager->currentProject().path;
	const HE::hccg::SdkInfo sdk  = HE::hccg::resolveSdk(editorBaseDir());
	const std::filesystem::path engineRoot = HE::hccg::engineRootFromSdk(sdk);
	if (engineRoot.empty())
	{
		HE::Ed::notify(HE::Ed::NoteLevel::Problem,
			"Cannot build the game logic",
			"The engine headers this editor would compile against could not be "
			"located (no he_sdk_config.json with a src/HE_Core/include entry). "
			"Build Source/ by hand with -DHORIZON_ENGINE_DIR=<engine root>.");
		return;
	}

	// Two rings: the compile, then the swap into the running session. The second
	// one is a single step that either happened or did not, so it never fills
	// gradually — but it is a step, because "compiled" and "running" are
	// different answers and the window has to be able to say which one it got to.
	const HE::hccg::DylibBuildSpec spec = HE::hccg::gameLogicSpec(projectFile, engineRoot);

	Build::begin({ "Compile", "Reload" }, BuildProgressDialog::Kind::GameLogic);
	BuildProgressDialog::requestOpen();
	Build::stepBegin(0);
	Build::setActivity("Compiling " + spec.sourceDir.filename().string() + "\xe2\x80\xa6");
	Build::log(0, "cmake -S " + spec.sourceDir.string() + " -B " + spec.buildDir.string());
	Build::log(0, "HORIZON_ENGINE_DIR=" + engineRoot.string());

	{
		std::lock_guard<std::mutex> lk(s_resultMutex);
		s_ok = false;
		s_message.clear();
		s_artifact.clear();
	}
	s_finished.store(false);
	s_running.store(true);
	if (s_thread.joinable()) s_thread.join();   // the previous run, already done
	s_thread = std::thread([spec]
	{
		const HE::hccg::BuildOutcome out = HE::hccg::buildDylib(spec,
			[](const std::string& line)
			{
				Build::log(severityOf(line), line);
				if (const auto p = BuildProgressDialog::toolchainProgress(line))
					Build::stepProgress(*p);
			});

		{
			std::lock_guard<std::mutex> lk(s_resultMutex);
			s_ok       = out.ok;
			s_message  = out.message;
			s_artifact = out.artifact;
		}
		// Nothing else. finish() belongs to the UI thread, which still has the
		// reload to run and the two outcomes to say in one sentence.
		s_running.store(false);
		s_finished.store(true);
	});
}

void render(AppContext& ctx)
{
	// The Build window's buttons, for OUR runs only — the export panel does the
	// same for its own and the two never see each other's.
	if (BuildProgressDialog::runKind() == BuildProgressDialog::Kind::GameLogic)
	{
		switch (BuildProgressDialog::takeAction())
		{
		case BuildProgressDialog::Action::Rebuild:     s_startRequest = true; break;
		// Only an export has settings; the button is not drawn for our runs.
		case BuildProgressDialog::Action::BackToSetup:                        break;
		case BuildProgressDialog::Action::None:                               break;
		}
	}

	if (s_finished.exchange(false))
	{
		if (s_thread.joinable()) s_thread.join();

		bool ok = false;
		std::string message;
		std::filesystem::path artifact;
		{
			std::lock_guard<std::mutex> lk(s_resultMutex);
			ok = s_ok; message = s_message; artifact = s_artifact;
		}

		if (!ok)
		{
			Build::log(2, message);
			Build::finish(false, message.empty() ? "Build failed" : message);
			HE::Ed::notify(HE::Ed::NoteLevel::Problem, "Game logic build failed", message);
			return;
		}

		Build::log(0, "Built " + artifact.filename().string());
		Build::stepBegin(1);
		Build::stepProgress(1.0f);

		// The swap, on the UI thread. Everything it does — onStop, dlopen,
		// re-inject, onStart — runs against the live world.
		const bool swapped = ctx.reloadGameLogic && ctx.reloadGameLogic();
		const std::string done = swapped
			? "Built and reloaded " + artifact.filename().string()
			  + " — the play session kept running."
			: "Built " + artifact.filename().string()
			  + " — it loads the next time you press Play.";
		Build::log(0, done);
		Build::finish(true, done);
		HE::Ed::notify(HE::Ed::NoteLevel::Info, "Game logic built", done);
	}

	if (s_startRequest && !s_running.load())
	{
		s_startRequest = false;
		start(ctx);
	}
}

} // namespace GameLogicBuildPanel
