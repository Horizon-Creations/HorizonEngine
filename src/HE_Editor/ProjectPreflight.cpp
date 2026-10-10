#include "ProjectPreflight.h"

#include "EditorApplication.h"   // AppContext
#include "EditorHelp.h"          // "Project Hub/<label>" scope for the buttons
#include "EditorUI.h"            // endProjectSession
#include "EditorWidgets.h"       // pinDialogToEditorWindow, button, primaryButton

#include <ContentManager/ContentManager.h>
#include <ContentManager/EngineDependencies.h>
#include <Diagnostics/GlobalState.h>
#include <Diagnostics/Logger.h>

#ifdef HE_HAVE_LIBSSH2
#include <ContentSync/EngineContentSync.h>
#include <ContentSync/SftpCredentials.h>
#endif

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace ProjectPreflight
{
namespace
{
namespace fs = std::filesystem;
namespace Deps = HE::EngineDeps;

enum class Phase { Idle, Running, Review };

struct Job
{
	std::string       projectFile;
	std::thread       worker;
	std::atomic<bool> cancel{ false };
	std::atomic<bool> finished{ false };

	std::mutex  m;                 // guards everything below
	std::string stage;
	std::string detail;
	std::size_t done = 0, total = 0;

	Deps::Result result;
	bool         serverConfigured = false;
	bool         serverReached    = false;   // the manifest came from the server, not the cache
	bool         haveCatalogue    = false;
	std::string  serverDetail;
};

Phase                s_phase = Phase::Idle;
std::unique_ptr<Job> s_job;
std::string          s_startup;         // handed over by requestAtStartup, taken by render()

bool skipped()
{
	const char* v = std::getenv("HE_SKIP_PROJECT_PREFLIGHT");
	return v && v[0] && v[0] != '0';
}

// The one place a project is actually loaded once it is cleared to open — the same
// bookkeeping every open site used to carry its own copy of.
void openNow(AppContext& ctx, const std::string& projectFile)
{
	const bool switching = ctx.projectLoaded;
	// The old session ends BEFORE the new project loads (see EditorUI's openProjectAt:
	// tearing its tabs down against a ContentManager that already points elsewhere
	// makes every path they hold resolve to nothing or to the wrong asset).
	if (switching) EditorUI::endProjectSession(ctx);
	if (ctx.projectManager->loadProject(projectFile))
	{
		ctx.globalState->addKnownProject(projectFile);
		ctx.globalState->writeConfig();
		ctx.contentRefreshPending = true;
		ctx.projectLoaded = true;
		ctx.hubOpenError.clear();
	}
	else
	{
		if (switching) ctx.projectLoaded = false;   // the old one is gone
		ctx.hubOpenError = "Failed to load project file.";
	}
}

void setStage(Job& j, const std::string& stage, const std::string& detail = {}, std::size_t done = 0, std::size_t total = 0)
{
	std::lock_guard<std::mutex> lock(j.m);
	j.stage = stage; j.detail = detail; j.done = done; j.total = total;
}

// The worker: sign in, read the catalogue, walk the project's references, download.
void run(Job& j, std::string engineRoot, std::string cacheRoot)
{
	Deps::Options o;
	o.projectFile        = j.projectFile;
	o.projectContentRoot = (fs::path(j.projectFile).parent_path() / "Content").string();
	o.engineRoot         = std::move(engineRoot);
	o.cacheRoot          = std::move(cacheRoot);
	o.cancelled          = [&j] { return j.cancel.load(std::memory_order_acquire); };
	o.progress           = [&j](const Deps::Progress& p) { setStage(j, p.stage, p.detail, p.done, p.total); };

#ifdef HE_HAVE_LIBSSH2
	j.serverConfigured = HE::Cs::engineContentEndpoint().configured();
	if (j.serverConfigured)
	{
		setStage(j, "Connecting to the EngineContent server");
		auto& sync = HE::Cs::EngineContentSync::instance();
		// Signing in IS fetching the catalogue: the same connection retry the editor's own
		// probe uses, and the error text it keeps (already scrubbed of the password).
		j.serverReached = sync.refreshManifestBlocking();
		if (!j.serverReached) j.serverDetail = sync.lastManifestError();
		// No server: the catalogue the last good connection wrote beside the downloads —
		// what is already downloaded still counts, only the rest cannot be fetched.
		bool have = j.serverReached || sync.loadCachedManifest();
		if (have)
		{
			for (const auto& e : sync.manifest().entries)
				o.catalogue.push_back({ e.relativePath, e.uuid });
			o.catalogueKnown = true;
			j.haveCatalogue  = true;
		}
		if (j.serverReached)
			o.fetch = [](const std::string& path, std::function<void(bool)> done)
			{
				HE::Cs::EngineContentSync::instance().enqueueDownload(
					path, HE::UUID{}, HE::Cs::DownloadTrigger::Passive, std::move(done));
			};
	}
#endif
	if (j.cancel.load()) { std::lock_guard<std::mutex> l(j.m); j.result.cancelled = true; j.finished.store(true); return; }

	Deps::Result r = Deps::resolve(o);
	{
		std::lock_guard<std::mutex> lock(j.m);
		j.result = std::move(r);
	}
	j.finished.store(true, std::memory_order_release);
}

void reset()
{
	if (s_job && s_job->worker.joinable()) s_job->worker.join();
	s_job.reset();
	s_phase = Phase::Idle;
}

const char* reasonText(Deps::MissingReason r)
{
	switch (r)
	{
	case Deps::MissingReason::NotOnServer:    return "not on the server";
	case Deps::MissingReason::NoCatalogue:    return "no server catalogue available";
	case Deps::MissingReason::DownloadFailed: return "the download failed";
	case Deps::MissingReason::NoFetcher:      return "this build cannot download";
	}
	return "";
}
} // namespace

void request(AppContext& ctx, const std::string& projectFile)
{
	if (s_phase != Phase::Idle) return;
	ctx.hubOpenError.clear();
	if (skipped() || projectFile.empty() || !ctx.contentManager)
	{
		openNow(ctx, projectFile);
		return;
	}
	s_job = std::make_unique<Job>();
	s_job->projectFile = projectFile;
	s_phase = Phase::Running;
	std::string engineRoot = ctx.contentManager->engineContentRoot();
	std::string cacheRoot  = GlobalState::engineContentCacheDir().string();
	HE_LOG_INFO(Editor, "Project preflight: checking %s", projectFile.c_str());
	Job* j = s_job.get();
	j->worker = std::thread([j, engineRoot = std::move(engineRoot), cacheRoot = std::move(cacheRoot)]() mutable
	{
		run(*j, std::move(engineRoot), std::move(cacheRoot));
	});
}

void requestAtStartup(const std::string& projectFile) { s_startup = projectFile; }

bool busy() { return s_phase != Phase::Idle; }

void shutdown()
{
	if (s_job)
	{
		s_job->cancel.store(true);
		if (s_job->worker.joinable()) s_job->worker.join();
	}
	s_job.reset();
	s_phase = Phase::Idle;
}

void render(AppContext& ctx)
{
	if (s_phase == Phase::Idle && !s_startup.empty())
	{
		const std::string path = std::move(s_startup);
		s_startup.clear();
		request(ctx, path);
	}
	if (s_phase == Phase::Idle || !s_job) return;

#ifdef HE_IMGUI_ENABLED
	Job& j = *s_job;
	const std::string name = fs::path(j.projectFile).stem().string();
	HE::Ed::Help::Scope helpScope("Project Hub");

	// ── The worker is done: open the project, drop the check, or ask ─────────
	if (s_phase == Phase::Running && j.finished.load(std::memory_order_acquire))
	{
		if (j.worker.joinable()) j.worker.join();
		if (j.result.cancelled)
		{
			HE_LOG_INFO(Editor, "Project preflight: cancelled (%s)", j.projectFile.c_str());
			reset();
			return;
		}
		HE_LOG_INFO(Editor,
			"Project preflight: %zu engine asset(s) referenced, %zu already here, %zu downloaded, %zu missing",
			j.result.referenced, j.result.alreadyHere, j.result.downloaded, j.result.missing.size());
		if (j.result.ok())
		{
			const std::string path = j.projectFile;
			reset();
			openNow(ctx, path);
			return;
		}
		s_phase = Phase::Review;
		ImGui::OpenPopup("##project_preflight_review");
	}

	// ── Progress ─────────────────────────────────────────────────────────────
	if (s_phase == Phase::Running)
	{
		if (!ImGui::IsPopupOpen("##project_preflight_progress"))
			ImGui::OpenPopup("##project_preflight_progress");
		EditorWidgets::pinDialogToEditorWindow(ImVec2(520.0f, 0.0f));
		if (ImGui::BeginPopupModal("##project_preflight_progress", nullptr,
		        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
		{
			std::string stage, detail;
			std::size_t done = 0, total = 0;
			{
				std::lock_guard<std::mutex> lock(j.m);
				stage = j.stage; detail = j.detail; done = j.done; total = j.total;
			}
			ImGui::Text("Opening %s", name.c_str());
			ImGui::Separator();
			ImGui::Spacing();
			ImGui::TextUnformatted(stage.empty() ? "Starting" : stage.c_str());
			float fraction = -1.0f * static_cast<float>(ImGui::GetTime());   // indeterminate
			std::string overlay;
#ifdef HE_HAVE_LIBSSH2
			if (stage == "Downloading" && total > 0)
			{
				// The sync queue knows the file in flight and how far it is: a progress bar that
				// only stepped per finished file sits at zero for a single large texture array.
				const HE::Cs::DownloadQueueStatus st = HE::Cs::EngineContentSync::instance().status();
				if (st.active && !st.currentRelativePath.empty()) detail = st.currentRelativePath;
				double files = static_cast<double>(done);
				if (st.active && st.currentBytesTotal > 0)
					files += static_cast<double>(st.currentBytesDone) / static_cast<double>(st.currentBytesTotal);
				fraction = std::clamp(static_cast<float>(files / static_cast<double>(total)), 0.0f, 1.0f);
				overlay  = std::to_string(done) + " / " + std::to_string(total);
			}
#endif
			ImGui::ProgressBar(fraction, ImVec2(480.0f, 0.0f), overlay.empty() ? nullptr : overlay.c_str());
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextUnformatted(detail.empty() ? " " : detail.c_str());
			ImGui::PopStyleColor();
			ImGui::Spacing();
			ImGui::BeginDisabled(j.cancel.load());
			if (EditorWidgets::button("Cancel", ImVec2(110.0f, 0.0f)))
				j.cancel.store(true);
			ImGui::EndDisabled();
			ImGui::EndPopup();
		}
		return;
	}

	// ── The question ─────────────────────────────────────────────────────────
	enum class Answer { None, OpenAnyway, CloseProject, CloseEditor } answer = Answer::None;
	EditorWidgets::pinDialogToEditorWindow(ImVec2(620.0f, 0.0f));
	if (ImGui::BeginPopupModal("##project_preflight_review", nullptr,
	        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
	{
		const auto& miss = j.result.missing;
		ImGui::Text("Some engine content could not be loaded for \"%s\"", name.c_str());
		ImGui::Separator();
		ImGui::Spacing();
		{
			EditorWidgets::WrapText wrap(590.0f);
			ImGui::TextWrapped("%zu of the %zu engine assets this project uses %s not available. "
			                   "Materials and meshes that use them will look wrong — grey surfaces, missing "
			                   "textures — until the files can be downloaded.",
			                   miss.size(), j.result.referenced, miss.size() == 1 ? "is" : "are");
			if (j.serverConfigured && !j.serverReached)
			{
				ImGui::Spacing();
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.35f, 1.0f));
				ImGui::TextWrapped("The EngineContent server could not be reached%s%s.",
				                   j.serverDetail.empty() ? "" : ": ", j.serverDetail.c_str());
				ImGui::PopStyleColor();
			}
			else if (!j.serverConfigured)
			{
				ImGui::Spacing();
				ImGui::TextDisabled("This build has no EngineContent server configured.");
			}
		}
		ImGui::Spacing();
		ImGui::BeginChild("##preflight_missing", ImVec2(590.0f, 180.0f), ImGuiChildFlags_Borders);
		constexpr size_t kShown = 40;
		for (size_t i = 0; i < miss.size() && i < kShown; ++i)
		{
			ImGui::TextUnformatted(miss[i].path.c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("— %s%s%s", reasonText(miss[i].reason),
			                    miss[i].neededBy.empty() ? "" : ", used by ", miss[i].neededBy.c_str());
		}
		if (miss.size() > kShown)
			ImGui::TextDisabled("… and %zu more", miss.size() - kShown);
		ImGui::EndChild();
		ImGui::Spacing();
		ImGui::TextDisabled("Open it anyway, and the editor keeps trying to download them whenever something needs them.");
		ImGui::Spacing();
		if (EditorWidgets::primaryButton("Open Anyway", ImVec2(150.0f, 0.0f))) answer = Answer::OpenAnyway;
		ImGui::SameLine();
		if (EditorWidgets::button("Close Project", ImVec2(150.0f, 0.0f))) answer = Answer::CloseProject;
		ImGui::SameLine();
		if (EditorWidgets::button("Close Editor", ImVec2(150.0f, 0.0f))) answer = Answer::CloseEditor;
		if (answer != Answer::None) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	switch (answer)
	{
	case Answer::OpenAnyway:
	{
		const std::string path = j.projectFile;
		reset();
		openNow(ctx, path);
		break;
	}
	case Answer::CloseProject:
		HE_LOG_INFO(Editor, "Project preflight: %s not opened (the user closed it)", j.projectFile.c_str());
		reset();
		break;
	case Answer::CloseEditor:
		reset();
		if (ctx.quit) ctx.quit();
		break;
	case Answer::None: break;
	}
#else
	(void)ctx;
#endif
}

} // namespace ProjectPreflight
