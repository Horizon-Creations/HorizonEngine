#include "EditorSettingsPanel.h"
#include "EditorApplication.h"           // AppContext, EditorConfig, EditorCamera
#include "ToolchainDialog.h"             // Tools > C++ Toolchain > Recheck forces the dialog open
#include "GitController.h"               // Source Control pages
#include "GitMissingDialog.h"            // install remedies shared with the startup dialog
#include <HorizonScene/HcCodegen.h>      // HE::hccg::ToolchainProbe (toolchain readout)
#include <SourceControl/GitProbe.h>
#include <SourceControl/RepoStatus.h>
#include <Diagnostics/GlobalState.h>
#include <Types/Enums.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

// Forward declaration — defined in EditorApplication.cpp
std::string getRHIName(HE::RendererBackend backend);

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace EditorSettingsPanel
{

// ─── Preferences tab (Edit > Preferences) ───────────────────────────────────
// Central place for editor settings. Values live in EditorConfig (a reference in
// AppContext) and are persisted to config.json on exit. Changes that need an
// immediate side effect (camera speed, vsync) are applied here on edit; the font
// scale is applied every frame from EditorConfig in render().

// VSync routing: OpenGL toggles the SDL swap interval on the Window, while
// Metal / Vulkan / D3D switch their present mode in the renderer. Call both so
// the toggle takes effect on every backend.
static void ApplyVSync(AppContext& ctx)
{
	// Route through Application::setVSync so the app's vsync state (which the profiler
	// capture saves/restores around F9) stays in sync with this editor toggle — else
	// a capture turns vsync back ON after stopping even though the user had it OFF.
	if (ctx.setVSync) { ctx.setVSync(ctx.vsync); return; }
	if (ctx.window)   ctx.window->SetVSync(ctx.vsync);
	if (ctx.renderer) ctx.renderer->SetVSync(ctx.vsync);
}

// ─── Engine-settings catalog + Quick-Settings favourites ────────────────────
// One catalog of pinnable engine settings. The Preferences pages render it one
// category at a time (with a "pin" toggle per row); Quick Settings renders only
// the pinned rows across all categories. Favourites are a comma-separated list
// of stable keys in EditorConfig::QuickSettingsFavorites (persisted to
// config.json). (Scene environment settings are NOT here — those live on the
// World entity.)

static bool isFavorite(const EditorConfig& cfg, const char* key)
{
	const std::string hay = "," + cfg.QuickSettingsFavorites + ",";
	return hay.find("," + std::string(key) + ",") != std::string::npos;
}
static void toggleFavorite(EditorConfig& cfg, const char* key)
{
	std::vector<std::string> keys;
	std::stringstream ss(cfg.QuickSettingsFavorites);
	std::string tok;
	bool had = false;
	while (std::getline(ss, tok, ','))
	{
		if (tok.empty()) continue;
		if (tok == key) { had = true; continue; } // drop it (toggle off)
		keys.push_back(tok);
	}
	if (!had) keys.push_back(key);              // add it (toggle on)
	cfg.QuickSettingsFavorites.clear();
	for (size_t i = 0; i < keys.size(); ++i)
		cfg.QuickSettingsFavorites += (i ? "," : "") + keys[i];
}

#ifdef HE_IMGUI_ENABLED
// Renders the engine-settings catalog. Each `row(key, category, widget)` is a
// logical setting group; `widget` draws its control(s).
void DrawEngineSettings(AppContext& ctx, SettingsMode mode, const char* categoryFilter)
{
	EditorConfig& cfg = ctx.editorConfig;
	const char* lastCat = nullptr;
	int shown = 0;
	auto row = [&](const char* key, const char* cat, auto&& widget)
	{
		if (categoryFilter && std::strcmp(cat, categoryFilter) != 0) return;
		const bool fav = isFavorite(cfg, key);
		if (mode == SettingsMode::QuickSettings && !fav) return;
		// With a category filter the page heading already names the category, so
		// the per-category separator is only drawn for the unfiltered catalog.
		if (!categoryFilter && (!lastCat || std::strcmp(lastCat, cat) != 0))
		{ ImGui::SeparatorText(cat); lastCat = cat; }
		if (mode == SettingsMode::Preferences)
		{
			ImGui::PushID(key);
			bool f = fav;
			if (ImGui::Checkbox("##pin", &f)) toggleFavorite(cfg, key);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(fav ? "Unpin from Quick Settings" : "Pin to Quick Settings");
			ImGui::PopID();
			ImGui::SameLine();
		}
		widget();
		++shown;
	};

	row("backend", "Display", [&]{
		ImGui::TextUnformatted("Backend");
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::BeginCombo("##backend", ctx.backendName.c_str()))
		{
			auto pick = [&](const char* label, HE::RendererBackend api){
				if (ImGui::Selectable(label)) { ctx.globalState->setSelectedRHI(api); ctx.backendName = getRHIName(api); }
			};
			pick("OpenGL", HE::RendererBackend::OpenGL);
#ifdef HE_VULKAN_ENABLED
			pick("Vulkan", HE::RendererBackend::Vulkan);
#endif
#ifdef _WIN32
			pick("DirectX11", HE::RendererBackend::D3D11);
			pick("DirectX12", HE::RendererBackend::D3D12);
#endif
#ifdef __APPLE__
			pick("Metal", HE::RendererBackend::Metal);
#endif
			ImGui::EndCombo();
		}
	});
	row("renderpath", "Display", [&]{
		const bool supported = ctx.renderer && ctx.renderer->GetCapabilities().supportsDeferredRendering;
		ImGui::TextUnformatted("Render Path");
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::BeginDisabled(!supported);
		const char* kPaths[] = { "Forward", "Deferred" };
		ImGui::Combo("##renderpath", &cfg.RenderPath, kPaths, IM_ARRAYSIZE(kPaths));
		ImGui::EndDisabled();
		if (!supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Deferred is available on Metal and OpenGL only.");
		else if (supported)
			ImGui::TextDisabled("Deferred: G-buffer + one lighting resolve per visible pixel.");
	});
	row("vsync", "Display", [&]{ if (ImGui::Checkbox("VSync", &ctx.vsync)) ApplyVSync(ctx); });
	row("maxfps", "Display", [&]{
		// VSync-off frame cap. 0 = unlimited (default — full FPS). A cap paces the loop so
		// the high-FPS mouse-look stays smooth and idle GPU load drops; ignored with VSync on.
		ImGui::BeginDisabled(ctx.vsync);
		int capped = static_cast<int>(cfg.MaxFps);
		ImGui::SetNextItemWidth(220.0f);
		if (ImGui::SliderInt("Max FPS (VSync off)", &capped, 0, 1000,
		                     capped <= 0 ? "Unlimited" : "%d FPS"))
		{
			cfg.MaxFps = static_cast<float>(capped < 0 ? 0 : capped);
			if (ctx.setMaxFps) ctx.setMaxFps(cfg.MaxFps);
		}
		ImGui::EndDisabled();
	});

	row("bloom", "Post-Processing", [&]{
		ImGui::Checkbox("Bloom", &cfg.BloomEnabled);
		ImGui::BeginDisabled(!cfg.BloomEnabled);
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("Bloom Threshold", &cfg.BloomThreshold, 0.0f, 4.0f, "%.2f");
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("Bloom Intensity", &cfg.BloomIntensity, 0.0f, 2.0f, "%.2f");
		ImGui::EndDisabled();
	});
	row("ssao", "Post-Processing", [&]{
		ImGui::Checkbox("AO", &cfg.SSAOEnabled);
		ImGui::BeginDisabled(!cfg.SSAOEnabled);
		ImGui::SetNextItemWidth(220.0f);
		// AO method: SSAO (kernel), HBAO (horizon bitmask), or GTAO (analytic arc).
		const char* kAOMethods[] = { "SSAO", "HBAO", "GTAO" };
		ImGui::Combo("AO Method", &cfg.SSAOMethod, kAOMethods, IM_ARRAYSIZE(kAOMethods));
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("AO Radius", &cfg.SSAORadius, 0.05f, 2.0f, "%.2f");
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("AO Intensity", &cfg.SSAOIntensity, 0.0f, 2.0f, "%.2f");
		ImGui::EndDisabled();
	});
	row("ssr", "Post-Processing", [&]{
		const bool supported = ctx.renderer && ctx.renderer->GetCapabilities().supportsScreenSpaceReflections;
		ImGui::BeginDisabled(!supported);
		ImGui::Checkbox("Screen-Space Reflections", &cfg.SSREnabled);
		ImGui::BeginDisabled(!cfg.SSREnabled);
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("SSR Intensity", &cfg.SSRIntensity, 0.0f, 1.0f, "%.2f");
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("SSR Max Roughness", &cfg.SSRMaxRoughness, 0.05f, 1.0f, "%.2f");
		ImGui::SetNextItemWidth(220.0f);
		// Low = 16 steps, raw trace; Med = 32 + blur; High = 64 + glossy
		// roughness lerp (wide second blur) — matches ssr-plan §7's tiers.
		const char* kSSRQuality[] = { "Low", "Medium", "High" };
		int ssrQ = std::clamp(cfg.SSRQuality, 0, 2);
		if (ImGui::Combo("SSR Quality", &ssrQ, kSSRQuality, 3))
			cfg.SSRQuality = ssrQ;
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		if (!supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Metal only, and only with Render Path = Deferred.");
		else if (supported)
			ImGui::TextDisabled("Metallic surfaces reflect the actual scene (deferred path).");
	});

	row("gi", "Global Illumination", [&]{
		const bool supported = ctx.renderer && ctx.renderer->GetCapabilities().supportsGlobalIllumination;
		ImGui::BeginDisabled(!supported);
		ImGui::Checkbox("Global Illumination (ray-traced, Metal)", &cfg.GlobalIlluminationEnabled);
		ImGui::BeginDisabled(!cfg.GlobalIlluminationEnabled);
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("GI Indirect Intensity", &cfg.GIIndirectIntensity, 0.0f, 3.0f, "%.2f");
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("GI Light Radius (deg)", &cfg.GILightRadius, 0.05f, 3.0f, "%.2f");
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		if (!supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Metal-only; needs a ray-tracing-capable GPU + macOS 12+.");
		else if (supported)
			ImGui::TextDisabled("Replaces CSM shadows + AO/ambient with ray-traced DDGI.");
	});
	row("girefl", "Global Illumination", [&]{
		const bool supported = ctx.renderer && ctx.renderer->GetCapabilities().supportsGIReflections;
		ImGui::BeginDisabled(!supported);
		ImGui::Checkbox("GI Reflections (ray-traced)", &cfg.GIReflectionsEnabled);
		ImGui::BeginDisabled(!cfg.GIReflectionsEnabled);
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("GI Refl Intensity", &cfg.GIReflIntensity, 0.0f, 1.0f, "%.2f");
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("GI Refl Max Roughness", &cfg.GIReflMaxRoughness, 0.05f, 1.0f, "%.2f");
		ImGui::SetNextItemWidth(220.0f);
		// Low = raw mirror trace; Med = + confidence-weighted blur; High =
		// + roughness-jittered cone rays with temporal accumulation (glossy).
		const char* kGIReflQuality[] = { "Low", "Medium", "High" };
		int grQ = std::clamp(cfg.GIReflQuality, 0, 2);
		if (ImGui::Combo("GI Refl Quality", &grQ, kGIReflQuality, 3))
			cfg.GIReflQuality = grQ;
		ImGui::SetNextItemWidth(220.0f);
		// Mirror-like surfaces seen IN a reflection reflect onward instead of
		// flattening to their base colour; each bounce costs one more trace
		// on the affected pixels only.
		ImGui::SliderInt("GI Refl Bounces", &cfg.GIReflBounces, 1, 4);
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		if (!supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Metal only, with Render Path = Deferred.");
		else if (supported)
			ImGui::TextDisabled("Scene rays fill SSR's off-screen gaps (hits lit by the GI probes).");
	});

	row("gpuparticles", "Effects", [&]{
		const bool supported = ctx.renderer && ctx.renderer->GetCapabilities().supportsGpuParticles;
		ImGui::BeginDisabled(!supported);
		ImGui::Checkbox("GPU Weather Particles", &cfg.GpuParticles);
		ImGui::EndDisabled();
		if (!supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Not available on this backend (needs OpenGL / transform feedback).");
		else if (supported)
			ImGui::TextDisabled("Simulate rain/snow on the GPU (transform feedback).");
	});

	row("camspeed", "Viewport", [&]{
		ImGui::SetNextItemWidth(220.0f);
		if (ImGui::SliderFloat("Camera Speed", &cfg.EditorCameraSpeed, 1.0f, 50.0f, "%.1f u/s") && ctx.editorCamera)
			ctx.editorCamera->setFlySpeed(cfg.EditorCameraSpeed);
	});

	row("fontscale", "Appearance", [&]{
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("UI Font Scale", &cfg.UiFontScale, 0.5f, 2.0f, "%.2fx");
	});

	row("cpucache", "Content Browser", [&]{ ImGui::Checkbox("Keep CPU Asset Cache", &cfg.KeepCPUAssets); });
	row("cbrefresh", "Content Browser", [&]{
		ImGui::SetNextItemWidth(120.0f);
		ImGui::InputInt("Refresh Interval (s)", &cfg.ContentBrowserRefreshRate, 0, 0);
		if (cfg.ContentBrowserRefreshRate < 0) cfg.ContentBrowserRefreshRate = 0;
	});

	if (mode == SettingsMode::QuickSettings && shown == 0)
		ImGui::TextDisabled("Pin engine settings in Preferences\n(Edit \xe2\x96\xb8 Preferences) to show them here.");
}

// ─── Preferences tab state ───────────────────────────────────────────────────

static Page s_page          = Page::Display;
static bool s_openRequested = false;
// Frame stamp of the last drawn Source Control page (see sourceControlPageActive).
static int  s_scPageFrame   = -1;

void requestOpen()          { s_openRequested = true; }
void requestOpen(Page page) { s_openRequested = true; s_page = page; }
bool takeOpenRequest()
{
	const bool r = s_openRequested;
	s_openRequested = false;
	return r;
}
bool sourceControlPageActive() { return s_scPageFrame == ImGui::GetFrameCount(); }

// ─── Source Control pages ────────────────────────────────────────────────────

namespace {

// GitHub setup inputs. The token buffer is wiped the moment it is handed off —
// it must not sit in static memory for the rest of the session.
char s_remoteUrl[512]  = "";
char s_ghRepoName[128] = "";
char s_ghToken[256]    = "";
bool s_ghPrivate       = true;
bool s_autoPushLoaded  = false;

// git identity form, pre-filled from whatever git already has so a user with
// only one of the two set does not have to retype the other.
char s_idName[128]   = {};
char s_idEmail[128]  = {};
bool s_idSeeded      = false;

void drawGitMessages(GitController* git)
{
	if (!git->lastError().empty())
	{
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
		ImGui::TextWrapped("%s", git->lastError().c_str());
		ImGui::PopStyleColor();
	}
	else if (!git->lastInfo().empty())
	{
		ImGui::Spacing();
		ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f), "%s", git->lastInfo().c_str());
	}
	if (git->busy()) { ImGui::Spacing(); ImGui::TextDisabled("Working…"); }
}

// Repository: init, remote / GitHub setup (token), auto-push. Commit, push,
// pull and the change list stay in the Source Control window (View menu) —
// this page is the one-time setup, that window is the daily driver.
void drawRepositoryPage(AppContext& ctx)
{
	s_scPageFrame = ImGui::GetFrameCount();

	GitController* git = ctx.git;
	if (!git)
	{
		ImGui::TextDisabled("Source control is unavailable in this build.");
		return;
	}
	if (!ctx.projectLoaded)
	{
		ImGui::TextWrapped("Open a project to configure its repository.");
		return;
	}
	if (ctx.gitProbe && !ctx.gitProbe->gitFound)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
		ImGui::TextWrapped("git was not found on this machine — see the Git Setup "
		                   "page for install instructions.");
		ImGui::PopStyleColor();
		if (ImGui::Button("Open Git Setup")) s_page = Page::GitSetup;
		return;
	}
	if (git->blockedByCollabSession())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.80f, 1.0f, 1.0f));
		ImGui::TextWrapped("You are a guest in a collaboration session. The host "
		                   "manages source control for this project — your changes "
		                   "reach the others through the session.");
		ImGui::PopStyleColor();
		return;
	}

	const HE::Sc::RepoStatus& st = git->status();

	if (!git->isRepo())
	{
		const bool lfs = ctx.gitProbe && ctx.gitProbe->lfsFound;
		ImGui::TextWrapped("This project is not in a git repository yet.");
		ImGui::Spacing();
		ImGui::TextWrapped("Initializing creates the repository with a generated "
		                   ".gitignore (engine output stays out) and .gitattributes "
		                   "(large binaries go through Git LFS).");
		if (!lfs)
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
			ImGui::TextWrapped("git-lfs was not found — the repository will work, "
			                   "but large assets will not be tracked through LFS "
			                   "until it is installed.");
			ImGui::PopStyleColor();
		}
		ImGui::Spacing();
		if (git->busy()) ImGui::BeginDisabled();
		if (ImGui::Button("Initialize Git repository", ImVec2(240.0f, 0.0f)))
			git->requestInit(lfs);
		if (git->busy()) ImGui::EndDisabled();
		drawGitMessages(git);
		return;
	}

	// ── Branch line ─────────────────────────────────────────────────────────
	if (st.detached)
		ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f), "Detached HEAD");
	else
		ImGui::Text("Branch: %s", st.branch.empty() ? "(unknown)" : st.branch.c_str());
	if (!st.initialCommit && !st.upstream.empty())
	{
		ImGui::SameLine();
		ImGui::TextDisabled("(upstream: %s)", st.upstream.c_str());
	}

	// ── Remote ───────────────────────────────────────────────────────────────
	ImGui::Spacing();
	ImGui::SeparatorText("Remote");
	if (git->remoteUrl().empty())
	{
		// Two ways in: let the editor create the repository on GitHub, or
		// paste the URL of one that already exists (any provider).
		ImGui::TextWrapped("Create the repository on GitHub directly:");

		if (s_ghRepoName[0] == '\0' && !git->projectRoot().empty())
		{
			// Default to the project folder's name — right nearly always,
			// editable when not.
			const std::string def = git->projectRoot().filename().string();
			std::snprintf(s_ghRepoName, sizeof(s_ghRepoName), "%s", def.c_str());
		}
		ImGui::SetNextItemWidth(180.0f);
		ImGui::InputText("Name##gh", s_ghRepoName, sizeof(s_ghRepoName));
		ImGui::SameLine();
		ImGui::Checkbox("Private", &s_ghPrivate);

		ImGui::SetNextItemWidth(-140.0f);
		ImGui::InputTextWithHint("##ghtoken", "Personal access token",
		                         s_ghToken, sizeof(s_ghToken),
		                         ImGuiInputTextFlags_Password);
		ImGui::SameLine();
		ImGui::BeginDisabled(git->busy() || s_ghToken[0] == '\0' ||
		                     s_ghRepoName[0] == '\0' || st.initialCommit);
		if (ImGui::Button("Create & push", ImVec2(130.0f, 0.0f)))
		{
			git->requestSetupGitHub(s_ghRepoName, s_ghPrivate, std::string(s_ghToken));
			// Wipe, not clear: the bytes must go, not just the length.
			std::fill(std::begin(s_ghToken), std::end(s_ghToken), '\0');
		}
		ImGui::EndDisabled();
		ImGui::TextDisabled("Token: github.com/settings/tokens — classic, 'repo' scope. "
		                    "It is handed to git's credential helper, stored nowhere else.");
		if (st.initialCommit)
			ImGui::TextDisabled("Make the first commit before setting up the remote.");

		ImGui::Spacing();
		ImGui::TextWrapped("Or paste an existing repository URL (GitHub, GitLab, "
		                   "Azure DevOps):");
		ImGui::SetNextItemWidth(-90.0f);
		ImGui::InputTextWithHint("##remoteurl", "https://github.com/you/project.git",
		                         s_remoteUrl, sizeof(s_remoteUrl));
		ImGui::SameLine();
		ImGui::BeginDisabled(git->busy() || s_remoteUrl[0] == '\0');
		if (ImGui::Button("Set##remote"))
		{
			git->requestSetRemote(s_remoteUrl);
			s_remoteUrl[0] = '\0';
		}
		ImGui::EndDisabled();
	}
	else
	{
		ImGui::TextDisabled("origin: %s", git->remoteUrl().c_str());

		// Auto-push: commit lands on the remote in the same action. The
		// preference survives restarts — per user, not per project file, so
		// nothing project-visible changes for collaborators.
		if (!s_autoPushLoaded)
		{
			s_autoPushLoaded = true;
			git->autoPushAfterCommit = GlobalState::getInstance()
				.getCustomConfigBool("GitAutoPushAfterCommit", false);
		}
		if (ImGui::Checkbox("Push automatically after each commit",
		                    &git->autoPushAfterCommit))
		{
			GlobalState::getInstance().setCustomConfigEntry(
				"GitAutoPushAfterCommit", git->autoPushAfterCommit);
		}
		ImGui::TextDisabled("Commit, push and pull live in View \xe2\x96\xb8 Source Control.");
	}

	drawGitMessages(git);
}

// Git Setup: install status (git, git-lfs), identity, credential helper —
// the same facts the startup "Source Control Not Ready" dialog reports, as a
// permanent page with a Recheck.
void drawGitSetupPage(AppContext& ctx)
{
	s_scPageFrame = ImGui::GetFrameCount();

	if (!ctx.gitProbe)
	{
		ImGui::TextDisabled("Checking git installation…");
		return;
	}
	const HE::Sc::GitProbe& p = *ctx.gitProbe;

	// ── git ──────────────────────────────────────────────────────────────────
	if (!p.gitFound)
	{
		ImGui::BulletText("git was not found.");
		ImGui::Indent();
		GitMissingDialog::drawGitInstallRemedy();
		ImGui::Unindent();
		ImGui::Spacing();
	}
	else
	{
		ImGui::BulletText("git %s found%s.",
		                  p.gitVersion.empty() ? "(unknown version)" : p.gitVersion.c_str(),
		                  p.gitPath.empty() ? "" : (" at " + p.gitPath.string()).c_str());
	}

	// ── git-lfs ──────────────────────────────────────────────────────────────
	if (p.gitFound && !p.lfsFound)
	{
		ImGui::BulletText("Git LFS was not found.");
		ImGui::Indent();
		ImGui::TextWrapped(
			"Git LFS stores large binary files — meshes, textures, audio. Without "
			"it those would be committed straight into the repository, which most "
			"hosts reject above 100 MB per file and which makes every clone "
			"download the entire history of every asset.");
		GitMissingDialog::drawLfsInstallRemedy();
		ImGui::Unindent();
		ImGui::Spacing();
	}
	else if (p.lfsFound)
	{
		ImGui::BulletText("Git LFS %s found.", p.lfsVersion.c_str());
	}

	// ── identity ─────────────────────────────────────────────────────────────
	if (p.gitFound)
	{
		if (!s_idSeeded)
		{
			s_idSeeded = true;
			std::snprintf(s_idName,  sizeof(s_idName),  "%s", p.userName.c_str());
			std::snprintf(s_idEmail, sizeof(s_idEmail), "%s", p.userEmail.c_str());
		}
		if (p.identityConfigured)
		{
			ImGui::BulletText("Identity: %s <%s>", p.userName.c_str(), p.userEmail.c_str());
		}
		else
		{
			ImGui::BulletText("Your name and email are not set in git.");
			ImGui::Indent();
			ImGui::TextWrapped("git records who made each change and refuses to commit "
			                   "without them. This is set once, for all your projects.");
			ImGui::Spacing();

			const bool applying = ctx.gitIdentityApplying;
			if (applying) ImGui::BeginDisabled();
			ImGui::SetNextItemWidth(260.0f);
			ImGui::InputTextWithHint("##scname",  "Your Name",       s_idName,  sizeof(s_idName));
			ImGui::SetNextItemWidth(260.0f);
			ImGui::InputTextWithHint("##scemail", "you@example.com", s_idEmail, sizeof(s_idEmail));

			const bool usable = s_idName[0] != '\0' && s_idEmail[0] != '\0';
			if (!usable) ImGui::BeginDisabled();
			if (ImGui::Button("Save Identity") && ctx.setGitIdentity)
				ctx.setGitIdentity(s_idName, s_idEmail);
			if (!usable) ImGui::EndDisabled();
			if (applying) ImGui::EndDisabled();
			if (applying) { ImGui::SameLine(); ImGui::TextDisabled("Saving…"); }
			ImGui::Unindent();
			ImGui::Spacing();
		}
	}

	// ── credential helper ────────────────────────────────────────────────────
	if (p.gitFound && p.credentialHelper.empty())
	{
		ImGui::BulletText("No credential helper is configured.");
		ImGui::Indent();
		ImGui::TextDisabled("Only needed to push. Source control will offer to set "
		                    "this up when you sign in.");
		ImGui::Unindent();
	}

	ImGui::Spacing();
	ImGui::Separator();
	if (ImGui::Button("Recheck##git") && ctx.recheckGit) ctx.recheckGit();
}

// ─── Toolchain page (Tools) ──────────────────────────────────────────────────

void drawToolchainPage(AppContext& ctx)
{
	ImGui::TextWrapped("cmake and a C++ compiler are needed for HorizonCode C++ "
	                   "export codegen and C++ GameLogic projects.");
	ImGui::Spacing();
	if (ctx.toolchainProbe)
	{
		const HE::hccg::ToolchainProbe& p = *ctx.toolchainProbe;
		if (p.cmakeFound && p.compilerFound)
			ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "OK — cmake %s, %s",
				p.cmakeVersion.c_str(), p.compilerId.c_str());
		else
			ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
				"Not found — needed for HorizonCode C++ export codegen and C++ GameLogic projects.");
	}
	else
	{
		ImGui::TextDisabled("Checking...");
	}
	if (ImGui::Button("Recheck##toolchain") && ctx.recheckToolchain)
	{
		ctx.recheckToolchain();
		ToolchainDialog::requestShow();
	}
}

// ─── Navigation + page plumbing ──────────────────────────────────────────────

struct NavItem  { Page page; const char* label; };
struct NavGroup { const char* label; const NavItem* items; int count; };

constexpr NavItem kGeneralItems[] = {
	{ Page::Appearance,     "Appearance" },
	{ Page::Viewport,       "Viewport" },
	{ Page::ContentBrowser, "Content Browser" },
};
constexpr NavItem kRenderingItems[] = {
	{ Page::Display,            "Display" },
	{ Page::PostProcessing,     "Post-Processing" },
	{ Page::GlobalIllumination, "Global Illumination" },
	{ Page::Effects,            "Effects" },
};
constexpr NavItem kSourceControlItems[] = {
	{ Page::Repository, "Repository" },
	{ Page::GitSetup,   "Git Setup" },
};
constexpr NavItem kToolsItems[] = {
	{ Page::Toolchain, "C++ Toolchain" },
};
constexpr NavGroup kNavGroups[] = {
	{ "General",        kGeneralItems,       IM_ARRAYSIZE(kGeneralItems) },
	{ "Rendering",      kRenderingItems,     IM_ARRAYSIZE(kRenderingItems) },
	{ "Source Control", kSourceControlItems, IM_ARRAYSIZE(kSourceControlItems) },
	{ "Tools",          kToolsItems,         IM_ARRAYSIZE(kToolsItems) },
};

// Engine-settings pages map onto one catalog category each; the rest have
// bespoke bodies. Null category = bespoke page.
const char* catalogCategory(Page p)
{
	switch (p)
	{
	case Page::Appearance:         return "Appearance";
	case Page::Viewport:           return "Viewport";
	case Page::ContentBrowser:     return "Content Browser";
	case Page::Display:            return "Display";
	case Page::PostProcessing:     return "Post-Processing";
	case Page::GlobalIllumination: return "Global Illumination";
	case Page::Effects:            return "Effects";
	default:                       return nullptr;
	}
}

const char* pageTitle(Page p)
{
	for (const NavGroup& g : kNavGroups)
		for (int i = 0; i < g.count; ++i)
			if (g.items[i].page == p) return g.items[i].label;
	return "";
}

} // namespace

// ─── The Preferences tab ─────────────────────────────────────────────────────

void render(AppContext& ctx, const ImVec2& pos, const ImVec2& size)
{
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::Begin("##PreferencesTab", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

	// ── Left: category navigation ────────────────────────────────────────────
	ImGui::BeginChild("##prefnav", ImVec2(200.0f, 0.0f), ImGuiChildFlags_None,
	                  ImGuiWindowFlags_NoScrollbar);
	for (const NavGroup& group : kNavGroups)
	{
		if (&group != &kNavGroups[0]) ImGui::Spacing();
		if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
		ImGui::TextDisabled("%s", group.label);
		if (ctx.fontSubheading) ImGui::PopFont();
		for (int i = 0; i < group.count; ++i)
		{
			const NavItem& item = group.items[i];
			ImGui::Indent(8.0f);
			if (ImGui::Selectable(item.label, s_page == item.page))
				s_page = item.page;
			ImGui::Unindent(8.0f);
		}
	}
	ImGui::EndChild();

	ImGui::SameLine();

	// ── Right: the selected page ─────────────────────────────────────────────
	ImGui::BeginChild("##prefcontent", ImVec2(0.0f, 0.0f));

	if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
	ImGui::TextUnformatted(pageTitle(s_page));
	if (ctx.fontHeading) ImGui::PopFont();
	ImGui::Separator();
	ImGui::Spacing();

	const char* category = catalogCategory(s_page);

	// Body scrolls on its own so the footer row below stays put.
	const float footerH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
	ImGui::BeginChild("##prefbody", ImVec2(0.0f, -footerH));
	if (category)
	{
		ImGui::TextDisabled("Tick the pin on a setting to show it in Quick Settings.");
		ImGui::Spacing();
		DrawEngineSettings(ctx, SettingsMode::Preferences, category);
	}
	else if (s_page == Page::Repository) drawRepositoryPage(ctx);
	else if (s_page == Page::GitSetup)   drawGitSetupPage(ctx);
	else if (s_page == Page::Toolchain)  drawToolchainPage(ctx);
	ImGui::EndChild();

	// ── Footer ───────────────────────────────────────────────────────────────
	ImGui::Separator();
	if (category)
	{
		EditorConfig& cfg = ctx.editorConfig;
		if (ImGui::Button("Restore Defaults"))
		{
			cfg.UiFontScale       = 1.0f;
			cfg.EditorCameraSpeed = 6.0f;
			cfg.KeepCPUAssets     = false;
			cfg.ContentBrowserRefreshRate = 60;
			cfg.BloomEnabled      = true;
			cfg.BloomThreshold    = 1.0f;
			cfg.BloomIntensity    = 0.6f;
			cfg.SSAOEnabled       = true;
			cfg.SSAORadius        = 0.5f;
			cfg.SSAOIntensity     = 1.0f;
			cfg.SSAOMethod        = 0;
			cfg.GpuParticles      = true;
			if (ctx.editorCamera) ctx.editorCamera->setFlySpeed(cfg.EditorCameraSpeed);
		}
		ImGui::SameLine();
	}
	ImGui::TextDisabled("Preferences are saved when the editor exits.");

	ImGui::EndChild();
	ImGui::End();
}
#endif // HE_IMGUI_ENABLED

} // namespace EditorSettingsPanel
