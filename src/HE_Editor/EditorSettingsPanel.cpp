#include "EditorSettingsPanel.h"
#include "EditorApplication.h"           // AppContext, EditorConfig, EditorCamera
#include "ToolchainDialog.h"             // Preferences > Recheck forces the dialog open
#include "EditorWidgets.h"               // pinDialogToEditorWindow
#include <HorizonScene/HcCodegen.h>      // HE::hccg::ToolchainProbe (toolchain readout)
#include <Types/Enums.h>
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

// ─── Preferences window (Edit > Preferences) ────────────────────────────────
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
// One catalog of pinnable engine settings, rendered in two modes: Preferences
// shows every setting with a "pin" toggle; Quick Settings shows only the pinned
// ones. Favourites are a comma-separated list of stable keys in
// EditorConfig::QuickSettingsFavorites (persisted to config.json). (Scene
// environment settings are NOT here — those live on the World entity.)

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
void DrawEngineSettings(AppContext& ctx, SettingsMode mode)
{
	EditorConfig& cfg = ctx.editorConfig;
	const char* lastCat = nullptr;
	int shown = 0;
	auto row = [&](const char* key, const char* cat, auto&& widget)
	{
		const bool fav = isFavorite(cfg, key);
		if (mode == SettingsMode::QuickSettings && !fav) return;
		if (!lastCat || std::strcmp(lastCat, cat) != 0) { ImGui::SeparatorText(cat); lastCat = cat; }
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

	row("backend", "Renderer", [&]{
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
	row("renderpath", "Renderer", [&]{
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
	row("vsync", "Renderer", [&]{ if (ImGui::Checkbox("VSync", &ctx.vsync)) ApplyVSync(ctx); });
	row("maxfps", "Renderer", [&]{
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

	row("bloom", "Post-processing", [&]{
		ImGui::Checkbox("Bloom", &cfg.BloomEnabled);
		ImGui::BeginDisabled(!cfg.BloomEnabled);
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("Bloom Threshold", &cfg.BloomThreshold, 0.0f, 4.0f, "%.2f");
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("Bloom Intensity", &cfg.BloomIntensity, 0.0f, 2.0f, "%.2f");
		ImGui::EndDisabled();
	});
	row("ssao", "Post-processing", [&]{
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
	row("gpuparticles", "Renderer", [&]{
		const bool supported = ctx.renderer && ctx.renderer->GetCapabilities().supportsGpuParticles;
		ImGui::BeginDisabled(!supported);
		ImGui::Checkbox("GPU Weather Particles", &cfg.GpuParticles);
		ImGui::EndDisabled();
		if (!supported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Not available on this backend (needs OpenGL / transform feedback).");
		else if (supported)
			ImGui::TextDisabled("Simulate rain/snow on the GPU (transform feedback).");
	});
	row("ssr", "Renderer", [&]{
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
	row("gi", "Renderer", [&]{
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
#endif // HE_IMGUI_ENABLED

void DrawPreferencesWindow(AppContext& ctx, bool& open)
{
	// `open` is a one-shot request raised by the Edit menu / Ctrl+, shortcut.
	// We turn it into a *modal* popup rather than a plain window: a modal renders
	// above everything and ignores clicks outside its bounds, so it can no longer
	// be dismissed by clicking next to it nor slip behind the fullscreen dockspace
	// and become unreachable. Escape, the X, or the Close button dismiss it.
	if (open)
	{
		ImGui::OpenPopup("Preferences");
		open = false; // consume the request; the popup now owns its lifetime
	}

	ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_Appearing);
	// Pinned to the editor window: the settings catalog is long enough to grow
	// taller than the editor, and a protruding popup becomes its own OS window
	// that the window manager can bury behind us on the next focus change.
	EditorWidgets::pinDialogToEditorWindow();
	if (ImGui::BeginPopupModal("Preferences", nullptr, ImGuiWindowFlags_NoCollapse))
	{
		EditorConfig& cfg = ctx.editorConfig;

		ImGui::TextDisabled("Tick the pin on a setting to show it in Quick Settings.");
		ImGui::Spacing();

		// The full engine-settings catalog (each row has a pin toggle here).
		DrawEngineSettings(ctx, SettingsMode::Preferences);

		ImGui::Separator();
		ImGui::TextDisabled("C++ Toolchain");
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

		ImGui::Separator();
		ImGui::TextDisabled("Preferences are saved when the editor exits.");
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
		if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

} // namespace EditorSettingsPanel
