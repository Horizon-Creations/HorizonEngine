#include "EditorSettingsPanel.h"
#include "EditorApplication.h"           // AppContext, EditorConfig, EditorCamera
#include "ToolchainDialog.h"             // Tools > C++ Toolchain > Recheck forces the dialog open
#include "GitController.h"               // Source Control pages
#include "GitMissingDialog.h"            // install remedies shared with the startup dialog
#include "EditorWidgets.h"             // Row:: label-above widgets + wrapped hint()
#include "EditorInput.h"               // pointer-device grammar (Auto/Mouse/Trackpad)
#include <HorizonScene/HcCodegen.h>      // HE::hccg::ToolchainProbe (toolchain readout)
#include <SourceControl/GitProbe.h>
#include <SourceControl/RepoStatus.h>
#include <Net/RouterProbe.h>
#include <Diagnostics/GlobalState.h>
#include <Types/Enums.h>
#include <algorithm>
#include <cfloat>
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

// Label-above-control rows live in EditorWidgets (the Details panel needs the
// same thing); these are the local spellings the catalog below reads with.
namespace {

using EditorWidgets::hint;
namespace Row = EditorWidgets::Row;

// Sub-controls of an enabled/disabled group read better indented under their
// checkbox, and the indent is what keeps "AO Radius" visibly subordinate to
// "AO" now that both start at the left margin.
struct SubGroup
{
	SubGroup(bool enabled) { ImGui::BeginDisabled(!enabled); ImGui::Indent(12.0f); }
	~SubGroup()            { ImGui::Unindent(12.0f); ImGui::EndDisabled(); }
};

} // namespace

// Renders the engine-settings catalog. Each `row(key, category, widget)` is a
// logical setting group; `widget` draws its control(s).
void DrawEngineSettings(AppContext& ctx, SettingsMode mode, const char* categoryFilter)
{
	// Wrapped for the whole catalog, and pushed here rather than in render(): the
	// wrap position lives on the window, this runs inside two different ones (the
	// Preferences body and the docked Quick Settings panel), and a docked panel is
	// as narrow as the user drags it. Everything below is prose — the hints are
	// paragraphs, the "not available on this backend" lines are sentences — and an
	// unwrapped line is not shortened, it is cut off at the panel's edge with no
	// mark to say so, which is the same defect as a sideways scrollbar minus the
	// scrollbar. A page function is the right scope because it opens no window of
	// its own: the pop lands on the window the push came from even when a page
	// returns early. 0.0f rather than an absolute column so the pin table below
	// keeps working — ImGui resolves it per table cell, so a setting wraps at its
	// own column's edge and not at the window's, which would run it under the pin.
	EditorWidgets::WrapText wrap;

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

		if (mode == SettingsMode::QuickSettings)
		{
			widget();
		}
		else
		{
			// A two-column row: the setting on the left, its pin toggle in a fixed
			// column on the right. The pin used to be a bare checkbox squeezed in
			// front of the widget, which put two unlabelled checkboxes side by side
			// on every boolean setting — indistinguishable, and neither said what it
			// did. A table also handles a multi-line setting properly: the pin stays
			// at the row's top right instead of drifting down after the last slider.
			ImGui::PushID(key);
			if (ImGui::BeginTable("##row", 2, ImGuiTableFlags_SizingFixedFit))
			{
				ImGui::TableSetupColumn("##setting", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("##pin",     ImGuiTableColumnFlags_WidthFixed, 72.0f);
				ImGui::TableNextRow();

				ImGui::TableSetColumnIndex(0);
				widget();

				ImGui::TableSetColumnIndex(1);
				// Labelled, so it is obvious what the control is for, and coloured
				// when active so a pinned setting is visible at a glance.
				if (fav) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
				if (ImGui::SmallButton(fav ? "\xe2\x98\x85 Pinned" : "\xe2\x98\x86 Pin"))
					toggleFavorite(cfg, key);
				if (fav) ImGui::PopStyleColor();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(fav ? "Remove from the Quick Settings panel"
					                      : "Show this setting in the Quick Settings panel");
				ImGui::EndTable();
			}
			ImGui::PopID();
		}
		// Rows are multi-line now (label above control, indented sub-controls), so
		// without a gap two neighbouring settings read as one block.
		ImGui::Spacing();
		++shown;
	};

	row("backend", "Display", [&]{
		ImGui::TextUnformatted("Backend");
		ImGui::SetNextItemWidth(-FLT_MIN);
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
		ImGui::BeginDisabled(!supported);
		const char* kPaths[] = { "Forward", "Deferred" };
		Row::combo("Render Path", &cfg.RenderPath, kPaths, IM_ARRAYSIZE(kPaths));
		ImGui::EndDisabled();
		if (!supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Deferred is available on Metal and OpenGL only.");
		else if (supported)
			hint("Deferred: G-buffer + one lighting resolve per visible pixel.");
	});
	row("vsync", "Display", [&]{ if (ImGui::Checkbox("VSync", &ctx.vsync)) ApplyVSync(ctx); });
	row("maxfps", "Display", [&]{
		// VSync-off frame cap. 0 = unlimited (default — full FPS). A cap paces the loop so
		// the high-FPS mouse-look stays smooth and idle GPU load drops; ignored with VSync on.
		ImGui::BeginDisabled(ctx.vsync);
		int capped = static_cast<int>(cfg.MaxFps);
		if (Row::sliderInt("Max FPS (VSync off)", &capped, 0, 1000,
		                 capped <= 0 ? "Unlimited" : "%d FPS"))
		{
			cfg.MaxFps = static_cast<float>(capped < 0 ? 0 : capped);
			if (ctx.setMaxFps) ctx.setMaxFps(cfg.MaxFps);
		}
		ImGui::EndDisabled();
	});

	row("bloom", "Post-Processing", [&]{
		ImGui::Checkbox("Bloom", &cfg.BloomEnabled);
		SubGroup sub(cfg.BloomEnabled);
		Row::sliderFloat("Bloom Threshold", &cfg.BloomThreshold, 0.0f, 4.0f, "%.2f");
		Row::sliderFloat("Bloom Intensity", &cfg.BloomIntensity, 0.0f, 2.0f, "%.2f");
	});
	row("ssao", "Post-Processing", [&]{
		ImGui::Checkbox("AO", &cfg.SSAOEnabled);
		SubGroup sub(cfg.SSAOEnabled);
		// AO method: SSAO (kernel), HBAO (horizon bitmask), or GTAO (analytic arc).
		const char* kAOMethods[] = { "SSAO", "HBAO", "GTAO" };
		Row::combo("AO Method", &cfg.SSAOMethod, kAOMethods, IM_ARRAYSIZE(kAOMethods));
		Row::sliderFloat("AO Radius", &cfg.SSAORadius, 0.05f, 2.0f, "%.2f");
		Row::sliderFloat("AO Intensity", &cfg.SSAOIntensity, 0.0f, 2.0f, "%.2f");
	});
	row("ssr", "Post-Processing", [&]{
		const bool supported = ctx.renderer && ctx.renderer->GetCapabilities().supportsScreenSpaceReflections;
		ImGui::BeginDisabled(!supported);
		ImGui::Checkbox("Screen-Space Reflections", &cfg.SSREnabled);
		const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
		{
			SubGroup sub(cfg.SSREnabled);
			Row::sliderFloat("SSR Intensity", &cfg.SSRIntensity, 0.0f, 1.0f, "%.2f");
			Row::sliderFloat("SSR Max Roughness", &cfg.SSRMaxRoughness, 0.05f, 1.0f, "%.2f");
			// Low = 16 steps, raw trace; Med = 32 + blur; High = 64 + glossy
			// roughness lerp (wide second blur) — matches ssr-plan §7's tiers.
			const char* kSSRQuality[] = { "Low", "Medium", "High" };
			int ssrQ = std::clamp(cfg.SSRQuality, 0, 2);
			if (Row::combo("SSR Quality", &ssrQ, kSSRQuality, 3))
				cfg.SSRQuality = ssrQ;
		}
		ImGui::EndDisabled();
		if (!supported && hovered)
			ImGui::SetTooltip("Metal only, and only with Render Path = Deferred.");
		else if (supported)
			hint("Metallic surfaces reflect the actual scene (deferred path).");
	});

	row("gi", "Global Illumination", [&]{
		const bool supported = ctx.renderer && ctx.renderer->GetCapabilities().supportsGlobalIllumination;
		ImGui::BeginDisabled(!supported);
		ImGui::Checkbox("Global Illumination (ray-traced, Metal)", &cfg.GlobalIlluminationEnabled);
		const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
		{
			SubGroup sub(cfg.GlobalIlluminationEnabled);
			Row::sliderFloat("GI Indirect Intensity", &cfg.GIIndirectIntensity, 0.0f, 3.0f, "%.2f");
			Row::sliderFloat("GI Light Radius (deg)", &cfg.GILightRadius, 0.05f, 3.0f, "%.2f");
		}
		ImGui::EndDisabled();
		if (!supported && hovered)
			ImGui::SetTooltip("Metal-only; needs a ray-tracing-capable GPU + macOS 12+.");
		else if (supported)
			hint("Replaces CSM shadows + AO/ambient with ray-traced DDGI.");
	});
	row("girefl", "Global Illumination", [&]{
		const bool supported = ctx.renderer && ctx.renderer->GetCapabilities().supportsGIReflections;
		ImGui::BeginDisabled(!supported);
		ImGui::Checkbox("GI Reflections (ray-traced)", &cfg.GIReflectionsEnabled);
		const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
		{
			SubGroup sub(cfg.GIReflectionsEnabled);
			Row::sliderFloat("GI Refl Intensity", &cfg.GIReflIntensity, 0.0f, 1.0f, "%.2f");
			Row::sliderFloat("GI Refl Max Roughness", &cfg.GIReflMaxRoughness, 0.05f, 1.0f, "%.2f");
			// The tier means RAYS per pixel and the RESOLUTION they are traced
			// at — Low 1 ray at quarter res, Medium 2 at half, High 4 at full.
			// Blur is not a tier feature, it is what stands in for the pixels a
			// lower resolution did not trace, so it SHRINKS as the tier rises
			// (4 / 2 / 1 texels). Mirror-like surfaces trace one ray at every
			// tier — their lobe is narrower than a pixel — so the ray cost only
			// grows where the reflection is actually glossy; the resolution
			// cost applies everywhere, which is what makes High expensive.
			const char* kGIReflQuality[] = { "Low (1 ray, 1/4 res)",
			                                 "Medium (2 rays, 1/2 res)",
			                                 "High (4 rays, full res)" };
			int grQ = std::clamp(cfg.GIReflQuality, 0, 2);
			if (Row::combo("GI Refl Quality", &grQ, kGIReflQuality, 3))
				cfg.GIReflQuality = grQ;
			// Mirror-like surfaces seen IN a reflection reflect onward instead of
			// flattening to their base colour; each bounce costs one more trace
			// on the affected pixels only. Labelled: the OpenGL kernel traces a
			// single segment and ignores this, so on Windows/Linux the slider
			// would otherwise drag with no effect on the image.
			Row::sliderInt("GI Refl Bounces (Metal)", &cfg.GIReflBounces, 1, 4);
			// The blur stands in for what the trace did NOT sample (skipped
			// pixels at a lower resolution, the unsampled part of the glossy
			// lobe), so it narrows as the tier rises. Off shows the raw trace at
			// the tier's resolution and ray count — the direct way to tell
			// whether a soft reflection is the blur or something else.
			ImGui::Checkbox("GI Refl Blur", &cfg.GIReflBlur);
		}
		ImGui::EndDisabled();
		if (!supported && hovered)
			ImGui::SetTooltip("Needs Metal (Render Path = Deferred) or an OpenGL 4.3 context.");
		else if (supported)
			hint("Real scene rays instead of the sky cubemap (hits lit by the GI probes).");
	});

	row("gpuparticles", "Effects", [&]{
		const bool supported = ctx.renderer && ctx.renderer->GetCapabilities().supportsGpuParticles;
		ImGui::BeginDisabled(!supported);
		ImGui::Checkbox("GPU Weather Particles", &cfg.GpuParticles);
		ImGui::EndDisabled();
		if (!supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Not available on this backend (needs OpenGL / transform feedback).");
		else if (supported)
			hint("Simulate rain/snow on the GPU (transform feedback).");
	});

	row("landiscovery", "Collaboration", [&]{
		ImGui::Checkbox("Find Sessions on the Local Network", &cfg.CollabLanDiscovery);
		hint("Hosts announce a session on the local network and guests see it in "
		     "a list, so nobody has to exchange an address or a session ID. This "
		     "is also the only route that works when the router will not forward "
		     "a port. The join code is never announced — a guest still needs it "
		     "from the host. Turn this off on a network you would rather not be "
		     "visible on.");
	});

	row("collabsynclarge", "Collaboration", [&]{
		// Locked while a session is live, and this is the reason: everyone in the
		// session joined under THIS value — a guest is asked about it before the
		// join and is refused if it disagrees. Flipping it afterwards would leave
		// one peer sending (or expecting) files the others never agreed to carry,
		// and the whole point of the setting is that nobody ends up pulling
		// hundreds of megabytes they said no to. Peers quietly holding different
		// copies of the same project is the failure this prevents.
		// active(), not inSession(): a guest that is still CONNECTING has already
		// put its answer in the join request, so the window in which this may
		// change closes at connect, not when the snapshot lands.
		const bool locked = ctx.collab && ctx.collab->largeAssetSyncLocked();
		ImGui::BeginDisabled(locked);
		ImGui::Checkbox("Sync Large Assets (Meshes, Textures, Audio)", &cfg.CollabSyncLargeAssets);
		// Read the hover state off the checkbox itself, before anything else can
		// become the "last item" — a tooltip that only appears on the enabled
		// control would never be seen, since it exists to explain the disabled one.
		const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
		ImGui::EndDisabled();
		if (locked && hovered)
			ImGui::SetTooltip("Cannot be changed while a session is running.");
		hint("Carries the big imported media over the session as well — meshes, "
		     "textures, audio, fonts, the files source control normally carries. "
		     "Those are measured in hundreds of megabytes, so a session with this "
		     "on can use considerably more data than the scenes and scripts it "
		     "otherwise syncs; leave it off on a metered or slow connection. It is "
		     "the HOST's setting for the session: a guest is asked to match it "
		     "before joining and cannot join without agreeing, so nobody is put on "
		     "the big downloads without being asked first.");
		if (locked)
			hint("Locked while a session is running — the others joined under the "
			     "current setting, and changing it now would leave the peers "
			     "holding different files. Leave the session to change it.");
	});

	row("collabmaxasset", "Collaboration", [&]{
		// NOT locked during a session, unlike the checkbox above it. That one is
		// a promise made to the other peers at the join; this is a bound on what
		// this machine will read, hold and hand out, and a bound you cannot
		// tighten while the thing you are worried about is happening would be a
		// strange kind of guard.
		Row::inputInt("Largest Asset to Transfer (MB)", &cfg.CollabMaxAssetMB);
		// Clamped after the edit, the way the refresh interval next door is —
		// and clamped to the CONTROLLER's constants, not to a second pair of
		// numbers here. A panel that lets you type 1024 while every refusal
		// quotes 512 is worse than one that refuses the keystroke.
		cfg.CollabMaxAssetMB = std::clamp(cfg.CollabMaxAssetMB,
		                                  CollabController::kMinAssetMB,
		                                  CollabController::kMaxAssetMB);
		hint("The largest single file a session will carry. Raising it is not "
		     "free: a file that travels is held WHOLE in memory on both machines "
		     "— read into one buffer here, queued a second time in the outgoing "
		     "connection buffer, and assembled into one more buffer at the far "
		     "end before it hits disk. It also goes out ahead of everything the "
		     "session still has to say, so a big file on a slow connection makes "
		     "editing together feel frozen until it is through. And this number "
		     "is the only thing standing between another peer and an allocation "
		     "of that size on this machine: an announced transfer larger than "
		     "this is refused before a byte of it is kept.");
		hint("Lowering it does not shrink anything — assets over the limit are "
		     "refused whole, never truncated, so they simply never arrive and "
		     "everyone else keeps the older file. When this editor is the one "
		     "refusing to send, a notification says which file and how big, so it "
		     "can go through source control instead. Each peer applies its own "
		     "number, though: when the far end refuses because THAT editor is set "
		     "lower, the person there is told and you are not — your save looked "
		     "as though it went through.");
	});

	row("camspeed", "Viewport", [&]{
		if (Row::sliderFloat("Camera Speed", &cfg.EditorCameraSpeed, 1.0f, 50.0f, "%.1f u/s")
		    && ctx.editorCamera)
			ctx.editorCamera->setFlySpeed(cfg.EditorCameraSpeed);
	});

	row("pointerinput", "Viewport", [&]{
		// The Auto item SAYS what it resolved to, so "why does scroll orbit?"
		// answers itself right here instead of needing a trip to a manual.
		const char* items[] = {
			EditorInput::detectTrackpad() ? "Auto (trackpad detected)" : "Auto (mouse detected)",
			"Mouse", "Trackpad" };
		cfg.PointerInput = std::clamp(cfg.PointerInput, 0, 2);
		Row::combo("Pointer Device", &cfg.PointerInput, items, IM_ARRAYSIZE(items));
		if (EditorInput::resolveTrackpad(cfg.PointerInput, EditorInput::detectTrackpad()))
			hint("Trackpad grammar: two-finger swipe pans graphs and 2D views "
			     "(Cmd/Ctrl+scroll zooms them); scroll zooms 3D previews; a "
			     "two-finger tap over the viewport toggles fly mode (WASDQE, "
			     "tap again or Esc to exit).");
		else
			hint("Mouse grammar: wheel zooms, right/middle-drag pans, "
			     "RMB-hold flies the viewport.");
	});

	row("fontscale", "Appearance", [&]{
		Row::sliderFloat("UI Font Scale", &cfg.UiFontScale, 0.5f, 2.0f, "%.2fx");
	});

	row("cpucache", "Content Browser", [&]{ ImGui::Checkbox("Keep CPU Asset Cache", &cfg.KeepCPUAssets); });
	row("cbrefresh", "Content Browser", [&]{
		Row::inputInt("Refresh Interval (s)", &cfg.ContentBrowserRefreshRate);
		if (cfg.ContentBrowserRefreshRate < 0) cfg.ContentBrowserRefreshRate = 0;
	});

	if (mode == SettingsMode::QuickSettings && shown == 0)
		hint("Nothing pinned yet. Open Edit \xe2\x96\xb8 Preferences and press \xe2\x98\x86 Pin "
		     "on the settings you want here.");
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

// Standalone "store a token for this remote" form (separate buffers from the
// GitHub-create form above — the two are different flows and clearing one must
// not disturb the other).
char s_credHost[128]  = "";
char s_credUser[128]  = "";
char s_credToken[256] = "";
// The remote URL the host/user fields were seeded from, so a changed remote
// re-seeds them but the user's own edits survive a redraw.
std::string s_credSeededFrom;

// Host of a git remote URL: https://host/…, ssh://git@host/…, git@host:path.
std::string hostFromRemote(const std::string& url)
{
	std::string s = url;
	if (const auto scheme = s.find("://"); scheme != std::string::npos)
		s = s.substr(scheme + 3);
	if (const auto at = s.find('@'); at != std::string::npos)
		s = s.substr(at + 1);
	if (const auto cut = s.find_first_of("/:"); cut != std::string::npos)
		s = s.substr(0, cut);
	return s;
}

// An SSH remote authenticates with a key, not a token — storing one would do
// nothing at all, so the form says so instead of pretending to work.
bool isSshRemote(const std::string& url)
{
	if (url.rfind("ssh://", 0) == 0) return true;
	// scp-style (git@host:path) — an '@' before any '/' and no scheme.
	const auto at    = url.find('@');
	const auto slash = url.find('/');
	return url.find("://") == std::string::npos && at != std::string::npos &&
	       (slash == std::string::npos || at < slash);
}

// The username the host expects next to a personal access token. GitHub wants
// x-access-token, GitLab oauth2; everything else (Azure DevOps, Gitea, self-
// hosted) accepts any non-empty name, so the account name is the safe default.
const char* tokenUserFor(const std::string& host)
{
	if (host.find("github") != std::string::npos) return "x-access-token";
	if (host.find("gitlab") != std::string::npos) return "oauth2";
	return "";
}

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
	// Every explanation on this page is a sentence, and one of the things it
	// prints is a remote URL that nobody typed by hand ("origin: https://…").
	EditorWidgets::WrapText wrap;

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
		if (EditorWidgets::primaryButton("Create & push", ImVec2(130.0f, 0.0f)))
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
		//
		// Loading it (and the fetch schedule) is SourceControlPanel's job now:
		// this page is one of three places that need the values in the
		// controller, and three copies of the same lazy load is two too many.
		// The footer status runs every frame with a project open, so by the time
		// anyone gets here it has already happened.
		if (ImGui::Checkbox("Push automatically after each commit",
		                    &git->autoPushAfterCommit))
		{
			GlobalState::getInstance().setCustomConfigEntry(
				"GitAutoPushAfterCommit", git->autoPushAfterCommit);
		}

		// ── Background fetch ─────────────────────────────────────────────────
		// Deliberately separate from auto-push, and worded as what it does rather
		// than as "fetch": the reason to want it is that "3 behind" stops being
		// whatever was true when the project was opened. Nothing on disk moves,
		// which is exactly why this one is safe to automate and pulling is not.
		if (ImGui::Checkbox("Check the remote for new commits periodically",
		                    &git->autoFetch))
		{
			GlobalState::getInstance().setCustomConfigEntry("GitAutoFetch", git->autoFetch);
		}
		ImGui::TextDisabled("Runs git fetch in the background. Your files, your branch and "
		                    "your commits are untouched — only the \"ahead / behind\" "
		                    "counters become current.");

		ImGui::BeginDisabled(!git->autoFetch);
		ImGui::Indent();
		// Presets rather than a free number: the useful range is narrow, and the
		// one value nobody should be able to type is a small one.
		struct Interval { const char* label; int minutes; };
		static const Interval kIntervals[] = {
			{ "Every 5 minutes",  5  },
			{ "Every 15 minutes", 15 },
			{ "Every 30 minutes", 30 },
			{ "Every hour",       60 },
		};
		int current = std::max(GitController::kMinFetchMinutes, git->autoFetchMinutes);
		const char* preview = "Every 15 minutes";
		for (const Interval& i : kIntervals)
			if (i.minutes == current) preview = i.label;

		ImGui::SetNextItemWidth(200.0f);
		if (ImGui::BeginCombo("##autofetchinterval", preview))
		{
			for (const Interval& i : kIntervals)
			{
				if (ImGui::Selectable(i.label, i.minutes == current))
				{
					git->autoFetchMinutes = i.minutes;
					GlobalState::getInstance().setCustomConfigEntry("GitAutoFetchMinutes",
					                                                i.minutes);
				}
			}
			ImGui::EndCombo();
		}
		ImGui::Unindent();
		ImGui::EndDisabled();

		ImGui::Spacing();
		ImGui::TextDisabled("Commit, push and pull live in View \xe2\x96\xb8 Source Control.");
	}

	// ── Access token ─────────────────────────────────────────────────────────
	// Available whatever route the remote took: the GitHub-create flow above
	// stores its token on the way through, but a pasted URL, a cloned project
	// or an expired token all end up here, with no repository being created.
	ImGui::Spacing();
	ImGui::SeparatorText("Access token");

	const std::string& remote = git->remoteUrl();
	if (remote.empty())
	{
		ImGui::TextDisabled("Configure a remote first — a token is stored per host.");
	}
	else if (isSshRemote(remote))
	{
		ImGui::TextWrapped("origin uses SSH, which authenticates with your SSH key. "
		                   "Access tokens apply to https:// remotes only.");
	}
	else
	{
		if (s_credSeededFrom != remote)
		{
			s_credSeededFrom = remote;
			const std::string host = hostFromRemote(remote);
			std::snprintf(s_credHost, sizeof(s_credHost), "%s", host.c_str());
			std::snprintf(s_credUser, sizeof(s_credUser), "%s", tokenUserFor(host));
		}

		ImGui::TextWrapped("Store an access token for pushing and pulling. It goes "
		                   "straight to git's credential helper (the system keychain) "
		                   "and is never written to a project or engine file.");
		ImGui::Spacing();

		ImGui::TextUnformatted("Host");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputText("##credhost", s_credHost, sizeof(s_credHost));

		ImGui::TextUnformatted("Username");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##creduser", "your account name",
		                         s_credUser, sizeof(s_credUser));

		ImGui::TextUnformatted("Token");
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##credtoken", "Personal access token",
		                         s_credToken, sizeof(s_credToken),
		                         ImGuiInputTextFlags_Password);

		ImGui::Spacing();
		ImGui::BeginDisabled(git->busy() || s_credHost[0] == '\0' ||
		                     s_credUser[0] == '\0' || s_credToken[0] == '\0');
		if (EditorWidgets::primaryButton("Save token", ImVec2(140.0f, 0.0f)))
		{
			git->requestStoreCredential(s_credHost, s_credUser, std::string(s_credToken));
			// Wipe, not clear: the bytes must go, not just the length.
			std::fill(std::begin(s_credToken), std::end(s_credToken), '\0');
		}
		ImGui::EndDisabled();
		ImGui::TextDisabled("GitHub: github.com/settings/tokens — classic, 'repo' scope.");
	}

	drawGitMessages(git);
}

// Git Setup: install status (git, git-lfs), identity, credential helper —
// the same facts the startup "Source Control Not Ready" dialog reports, as a
// permanent page with a Recheck.
void drawGitSetupPage(AppContext& ctx)
{
	// The bullets here interpolate an absolute path ("git 2.39.5 found at
	// /usr/bin/git") and an identity, neither of which has a known length.
	EditorWidgets::WrapText wrap;

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
		// Bullet() + TextWrapped rather than BulletText: ImGui draws a bullet's
		// text in one unwrapped run whatever wrap position is pushed, and this
		// line ends in an absolute path — "/usr/local/Cellar/git/2.39.5/bin/git"
		// is longer than the page is wide, and the part that gets cut off is the
		// part that answers "which git is it using?". The two render at the same
		// offsets, so only the wrapping changes. The bullets around this one carry
		// fixed sentences that fit, and stay as they are.
		ImGui::Bullet();
		ImGui::TextWrapped("git %s found%s.",
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
			// Same reason as the git bullet above: a name and an address, neither
			// of which this panel gets to choose the length of.
			ImGui::Bullet();
			ImGui::TextWrapped("Identity: %s <%s>", p.userName.c_str(), p.userEmail.c_str());
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
			if (EditorWidgets::primaryButton("Save Identity") && ctx.setGitIdentity)
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

// ─── Status page (Tools) ─────────────────────────────────────────────────────
// Every external thing the editor depends on, in one table, from the three
// background probes that already run at startup. Read-only by construction:
// nothing here installs, configures or changes anything, so it is safe to open
// at any time — the per-item remedies live on the pages that own them.

enum class StatusLevel { Ok, Warn, Missing, Checking };

ImVec4 statusColor(StatusLevel s)
{
	switch (s)
	{
	case StatusLevel::Ok:       return ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
	case StatusLevel::Warn:     return ImVec4(1.00f, 0.78f, 0.35f, 1.0f);
	case StatusLevel::Missing:  return ImVec4(1.00f, 0.50f, 0.45f, 1.0f);
	case StatusLevel::Checking: break;
	}
	return ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
}

const char* statusMark(StatusLevel s)
{
	switch (s)
	{
	case StatusLevel::Ok:       return "OK";
	case StatusLevel::Warn:     return "!";
	case StatusLevel::Missing:  return "X";
	case StatusLevel::Checking: break;
	}
	return "...";
}

// One table row: name, coloured verdict, and the detail that makes the verdict
// actionable ("git 2.39.5 at /usr/bin/git" beats a green tick with no facts).
void statusRow(const char* name, StatusLevel level, const std::string& detail,
               Page jumpTo, bool canJump)
{
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::TextUnformatted(name);

	ImGui::TableSetColumnIndex(1);
	ImGui::TextColored(statusColor(level), "%s", statusMark(level));

	ImGui::TableSetColumnIndex(2);
	if (detail.empty()) ImGui::TextDisabled("—");
	else                ImGui::TextWrapped("%s", detail.c_str());

	ImGui::TableSetColumnIndex(3);
	// A jump only where there is somewhere useful to go — otherwise the column
	// fills with buttons that all lead to the same page saying the same thing.
	if (canJump && level != StatusLevel::Ok && level != StatusLevel::Checking)
	{
		ImGui::PushID(name);
		if (ImGui::SmallButton("Fix")) s_page = jumpTo;
		ImGui::PopID();
	}
}

void drawStatusPage(AppContext& ctx)
{
	// Safe over the table below because ImGui resolves a 0.0f wrap position
	// against the current cell's column, not the window — so a Detail cell wraps
	// inside its own column and the fixed Tool/State/Fix columns are unaffected.
	// The router probe log at the foot needs it most: that is raw multi-line
	// output from a device on the network, and its longest line is nobody's
	// business but the router's.
	EditorWidgets::WrapText wrap;

	ImGui::TextWrapped("Everything the editor needs from outside itself. Checked in "
	                   "the background at startup; nothing here changes any setting.");
	ImGui::Spacing();

	constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
	                                   ImGuiTableFlags_SizingStretchProp;
	if (ImGui::BeginTable("##statustable", 4, kFlags))
	{
		ImGui::TableSetupColumn("Tool",   ImGuiTableColumnFlags_WidthFixed, 150.0f);
		ImGui::TableSetupColumn("State",  ImGuiTableColumnFlags_WidthFixed, 34.0f);
		ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("##fix",  ImGuiTableColumnFlags_WidthFixed, 44.0f);

		// ── Source control ───────────────────────────────────────────────────
		if (!ctx.gitProbe)
		{
			statusRow("git",              StatusLevel::Checking, "", Page::GitSetup, false);
			statusRow("Git LFS",          StatusLevel::Checking, "", Page::GitSetup, false);
			statusRow("git identity",     StatusLevel::Checking, "", Page::GitSetup, false);
			statusRow("Credential helper",StatusLevel::Checking, "", Page::GitSetup, false);
		}
		else
		{
			const HE::Sc::GitProbe& g = *ctx.gitProbe;
			statusRow("git", g.gitFound ? StatusLevel::Ok : StatusLevel::Missing,
			          g.gitFound
			              ? (g.gitVersion.empty() ? std::string("found") : g.gitVersion) +
			                (g.gitPath.empty() ? "" : " at " + g.gitPath.string())
			              : std::string("not found — source control is unavailable"),
			          Page::GitSetup, true);

			// A warning, not an error: the repository works without LFS, it just
			// commits big binaries the wrong way.
			statusRow("Git LFS", g.lfsFound ? StatusLevel::Ok : StatusLevel::Warn,
			          g.lfsFound ? g.lfsVersion
			                     : std::string("not found — large assets would be committed "
			                                   "directly into the repository"),
			          Page::GitSetup, true);

			statusRow("git identity",
			          g.identityConfigured ? StatusLevel::Ok : StatusLevel::Warn,
			          g.identityConfigured ? g.userName + " <" + g.userEmail + ">"
			                               : std::string("user.name / user.email are not set — "
			                                             "commits will fail"),
			          Page::GitSetup, true);

			statusRow("Credential helper",
			          g.credentialHelper.empty() ? StatusLevel::Warn : StatusLevel::Ok,
			          g.credentialHelper.empty()
			              ? std::string("none configured — needed to push without retyping a token")
			              : g.credentialHelper,
			          Page::Repository, true);
		}

		// ── C++ toolchain ────────────────────────────────────────────────────
		if (!ctx.toolchainProbe)
		{
			statusRow("cmake",        StatusLevel::Checking, "", Page::Toolchain, false);
			statusRow("C++ compiler", StatusLevel::Checking, "", Page::Toolchain, false);
		}
		else
		{
			const HE::hccg::ToolchainProbe& t = *ctx.toolchainProbe;
			statusRow("cmake", t.cmakeFound ? StatusLevel::Ok : StatusLevel::Missing,
			          t.cmakeFound ? t.cmakeVersion
			                       : std::string("not found — needed for C++ export codegen "
			                                     "and C++ projects"),
			          Page::Toolchain, true);
			statusRow("C++ compiler", t.compilerFound ? StatusLevel::Ok : StatusLevel::Missing,
			          t.compilerFound ? t.compilerId : std::string("not found"),
			          Page::Toolchain, true);
		}

		// ── Collaboration ────────────────────────────────────────────────────
		if (!ctx.routerProbe)
		{
			statusRow("Local network",     StatusLevel::Checking, "", Page::Status, false);
			statusRow("Router / UPnP",     StatusLevel::Checking, "", Page::Status, false);
			statusRow("IPv6",              StatusLevel::Checking, "", Page::Status, false);
			statusRow("Session directory", StatusLevel::Checking, "", Page::Status, false);
		}
		else
		{
			const HE::Net::RouterProbe& r = *ctx.routerProbe;

			statusRow("Local network",
			          r.localNetworkBlocked ? StatusLevel::Missing : StatusLevel::Ok,
			          r.localNetworkBlocked
			              ? std::string("this application may not talk to the local network — "
			                            "on macOS, allow it under Privacy & Security > Local Network")
			              : (r.gatewayV4.empty() ? std::string("no default gateway")
			                                     : "gateway " + r.gatewayV4),
			          Page::Status, false);

			// Amber rather than red: without forwarding you can still JOIN a
			// session, and a global IPv6 address makes hosting work anyway.
			const bool forward = r.portForwardingAvailable();
			std::string routerDetail;
			if (forward)
			{
				routerDetail = r.upnpFound ? ("UPnP: " + r.upnpService) : std::string("NAT-PMP");
				if (r.upnpFound && r.natPmpFound) routerDetail += " + NAT-PMP";
				if (!r.externalIp.empty())        routerDetail += ", WAN " + r.externalIp;
			}
			else
			{
				routerDetail = "the router did not answer UPnP or NAT-PMP — hosting needs a "
				               "port forwarded by hand, or the feature enabled on the router";
			}
			statusRow("Router / UPnP", forward ? StatusLevel::Ok : StatusLevel::Warn,
			          routerDetail, Page::Status, false);

			if (r.carrierNat)
			{
				statusRow("Carrier NAT", StatusLevel::Warn,
				          "the router's own WAN address is private (CGNAT) — a port forward "
				          "on it cannot be reached from the internet",
				          Page::Status, false);
			}

			statusRow("IPv6", r.globalIPv6.empty() ? StatusLevel::Warn : StatusLevel::Ok,
			          r.globalIPv6.empty()
			              ? std::string("no global address — hosting depends on IPv4 forwarding")
			              : r.globalIPv6 + (r.upnpV6Firewall ? " (router offers pinholes)" : ""),
			          Page::Status, false);

			statusRow("Session directory",
			          r.httpsAvailable ? StatusLevel::Ok : StatusLevel::Missing,
			          r.httpsAvailable
			              ? std::string("HTTPS available — joining by session ID works")
			              : std::string("this build has no HTTPS backend — sessions can only be "
			                            "joined by address"),
			          Page::Status, false);
		}

		ImGui::EndTable();
	}

	// ── Footer: recheck + the collaboration caveat ───────────────────────────
	ImGui::Spacing();
	ImGui::Separator();
	const bool checking = !ctx.gitProbe || !ctx.toolchainProbe || !ctx.routerProbe;
	ImGui::BeginDisabled(checking);
	if (ImGui::Button("Recheck all"))
	{
		if (ctx.recheckGit)       ctx.recheckGit();
		if (ctx.recheckToolchain) ctx.recheckToolchain();
		if (ctx.recheckRouter)    ctx.recheckRouter();
	}
	ImGui::EndDisabled();
	if (checking) { ImGui::SameLine(); ImGui::TextDisabled("Checking…"); }

	// Said plainly so a green row is not over-read: the only proof that an
	// inbound connection arrives is one arriving, which needs a live session.
	ImGui::Spacing();
	ImGui::TextWrapped("The router check is read-only — it asks what the router supports "
	                   "and never creates a port mapping. Whether an incoming connection "
	                   "really arrives is confirmed only once a session is open "
	                   "(View \xe2\x96\xb8 Collaboration).");

	if (ctx.routerProbe && !ctx.routerProbe->detail.empty())
	{
		ImGui::Spacing();
		if (ImGui::TreeNode("Router probe log"))
		{
			ImGui::TextUnformatted(ctx.routerProbe->detail.c_str());
			ImGui::TreePop();
		}
	}
}

// ─── Toolchain page (Tools) ──────────────────────────────────────────────────

void drawToolchainPage(AppContext& ctx)
{
	// The "Not found" line is one long sentence and the "OK" line carries a
	// compiler id string that the toolchain probe read off the machine.
	EditorWidgets::WrapText wrap;

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
constexpr NavItem kCollaborationItems[] = {
	{ Page::CollabGeneral, "Sessions" },
};
constexpr NavItem kSourceControlItems[] = {
	{ Page::Repository, "Repository" },
	{ Page::GitSetup,   "Git Setup" },
};
constexpr NavItem kToolsItems[] = {
	{ Page::Status,    "Status" },
	{ Page::Toolchain, "C++ Toolchain" },
};
constexpr NavGroup kNavGroups[] = {
	{ "General",        kGeneralItems,       IM_ARRAYSIZE(kGeneralItems) },
	{ "Rendering",      kRenderingItems,     IM_ARRAYSIZE(kRenderingItems) },
	{ "Collaboration",  kCollaborationItems, IM_ARRAYSIZE(kCollaborationItems) },
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
	case Page::CollabGeneral:      return "Collaboration";
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
	// Its own shade, taller rows, and the active page marked with an accent bar
	// at the rail's edge — the way every settings sidebar this decade says
	// "you are here". The Selectable's own blue fill comes from the theme.
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.09f, 0.095f, 1.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 12.0f));
	ImGui::BeginChild("##prefnav", ImVec2(210.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding,
	                  ImGuiWindowFlags_NoScrollbar);
	ImGui::PopStyleVar();
	for (const NavGroup& group : kNavGroups)
	{
		if (&group != &kNavGroups[0]) { ImGui::Spacing(); ImGui::Spacing(); }
		if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
		ImGui::TextDisabled("%s", group.label);
		if (ctx.fontSubheading) ImGui::PopFont();
		ImGui::Spacing();
		for (int i = 0; i < group.count; ++i)
		{
			const NavItem& item = group.items[i];
			const bool active = s_page == item.page;
			ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
			ImGui::Indent(6.0f);
			if (ImGui::Selectable(item.label, active, 0,
			                      ImVec2(0.0f, ImGui::GetFrameHeight())))
				s_page = item.page;
			if (active)
			{
				const ImVec2 mn = ImGui::GetItemRectMin();
				const ImVec2 mx = ImGui::GetItemRectMax();
				ImGui::GetWindowDrawList()->AddRectFilled(
					ImVec2(mn.x - 6.0f, mn.y + 3.0f), ImVec2(mn.x - 3.0f, mx.y - 3.0f),
					IM_COL32(72, 130, 205, 255), 2.0f);
			}
			ImGui::Unindent(6.0f);
			ImGui::PopStyleVar();
		}
	}
	ImGui::EndChild();
	ImGui::PopStyleColor();

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
		EditorWidgets::hint("Press \xe2\x98\x86 Pin on a setting to keep it in the Quick Settings "
		                    "panel next to the viewport.");
		ImGui::Spacing();
		DrawEngineSettings(ctx, SettingsMode::Preferences, category);
	}
	else if (s_page == Page::Repository) drawRepositoryPage(ctx);
	else if (s_page == Page::GitSetup)   drawGitSetupPage(ctx);
	else if (s_page == Page::Status)     drawStatusPage(ctx);
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
			cfg.CollabLanDiscovery = true;
			// Restore Defaults is the one path that can move CollabSyncLargeAssets
			// without touching its (disabled) checkbox, so it has to honour the same
			// lock: a running session would silently switch to a rule its peers did
			// not join under, which is exactly the split-project state the checkbox
			// is greyed out to avoid. Outside a session it resets like everything
			// else — off, because the big media is opt-in.
			if (!(ctx.collab && ctx.collab->largeAssetSyncLocked())) cfg.CollabSyncLargeAssets = false;
			// The ceiling has no such lock — it is a local bound, not a rule the
			// peers joined under — so it resets unconditionally, to the same 64 MB
			// the session config carries as its own default (Config::maxAssetBytes,
			// which is the asset ceiling now — the join snapshot keeps its own).
			cfg.CollabMaxAssetMB  = 64;
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
