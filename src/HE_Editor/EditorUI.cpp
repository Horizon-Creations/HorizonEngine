#include "EditorUI.h"
#include "EditorApplication.h"
#include "ScriptEditorPanel.h"
#include "MaterialEditorPanel.h"
#include "UIEditorPanel.h"
#include "LevelScriptPanel.h"
#include "GameInstancePanel.h"
#include "HorizonCodeClassPanel.h"
#include "HorizonVersion.h"
#ifdef __APPLE__
#include "MacMenuBar.h"   // native system menu bar (replaces the ImGui menu row)
#endif
#include <Hpak/ProjectExporter.h>
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/LODSystem.h>
#include <HorizonScene/NavigationSystem.h>
#include <Scripting/ScriptEngine.h>
#include <ContentManager/HAsset.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <material/MaterialShaderLibrary.h>
#include <MaterialGraph/MaterialGraph.h> // HE::MatParamKind for the entity param editor
#include <UIWidget/UIWidgetTree.h>       // starter tree for freshly created UI widgets
#include <HorizonCode/HorizonCode.h>     // starter graph for freshly created HorizonCode classes
#include <Types/Enums.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <Math/AABB.h>
#include <glm/gtc/type_ptr.hpp>
#include "MeshImporter.h"
#include "TextureImporter.h"
#include "MaterialImporter.h"
#include "AudioImporter.h"
#include "FontImporter.h"

#ifdef _WIN32
#include <windows.h>  // must come before any header that pulls in rpcdce.h
#endif

#include <Diagnostics/Logger.h>
#include <Diagnostics/EngineProfiler.h>
#include <SDL3/SDL.h>
#include <filesystem>
#include <string>
#include <sstream>
#include <vector>
#include <cstring>
#include <array>
#include <unordered_map>
#include <cmath>
#include <atomic>
#include <mutex>
#include <thread>

// Forward declaration — defined in EditorApplication.cpp
std::string getRHIName(HE::RendererBackend backend);

namespace
{
	// Last viewport RENDER resolution in framebuffer pixels (HiDPI-aware), captured when
	// the viewport panel is drawn and shown in the footer beside the FPS counter.
	int s_viewportPxW = 0;
	int s_viewportPxH = 0;

	// RMB fly-look capture state for the Scene viewport (SDL relative-mouse mode). File-
	// scope (not a viewport-local static) so the capture can be force-released from paths
	// that DON'T draw the viewport — e.g. switching to a material/script tab mid-look via a
	// keyboard shortcut. Otherwise relative mode + the ImGui NoMouse flag stay latched and
	// the cursor is hidden/pinned with no way out but quitting.
	bool  s_rmbCaptured = false;
	float s_rmbStartX   = 0.f;
	float s_rmbStartY   = 0.f;

	// Drop any active fly-look capture: warp the cursor back to where the look-drag began,
	// leave relative mode, re-show the OS cursor, and hand mouse control back to ImGui.
	// Safe to call every frame — a no-op unless a capture is actually active.
	void releaseViewportLookCapture(SDL_Window* win)
	{
		if (!s_rmbCaptured) return;
		ImGuiIO& io = ImGui::GetIO();
		if (win)
		{
			SDL_WarpMouseInWindow(win, s_rmbStartX, s_rmbStartY);
			SDL_SetWindowRelativeMouseMode(win, false);
		}
		SDL_ShowCursor();
		io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
		io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
		s_rmbCaptured = false;
	}

	// Belt-and-suspenders invariant, run once per frame BEFORE any early-out: fly-look
	// capture must never outlive a physically-held right mouse button. If the OS reports RMB
	// is not down but we're still flagged as captured, force-release — this recovers from any
	// path that latched the capture without releasing it (tab switch, focus change, a stale
	// ImGui button state that spuriously (re)engaged look). Reads the PHYSICAL SDL button
	// state, not ImGui's io.MouseDown (which NoMouse zeroes during a real look), so an actual
	// fly-look is never cut short.
	void enforceViewportLookCaptureInvariant(SDL_Window* win)
	{
		if (!s_rmbCaptured) return;
		const bool rmbDown =
			(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) != 0;
		if (!rmbDown) releaseViewportLookCapture(win);
	}

	// The async SDL file slot (pendingFileReady/Result) is shared across project
	// and scene operations; this records which one is currently in flight so the
	// single result handler can dispatch correctly.
	enum class PendingFileOp { OpenProject, OpenScene, SaveScene, ImportAsset, AddSceneAdditive };

	// A destructive action that would discard the current scene. When requested
	// while the scene is dirty it is stashed and a "Save changes?" modal is shown;
	// the action runs once the user resolves the modal.
	enum class GuardedAction {
		None, NewScene, OpenSceneDialog, OpenScenePath,
		OpenProjectDialog, CloseProject, Quit,
	};
}

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <imgui_internal.h>   // DockBuilder* for the default dock layout
#include <misc/cpp/imgui_stdlib.h> // InputText overloads for std::string
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>
#ifdef _WIN32
#include <imgui_impl_dx11.h>
#include <imgui_impl_dx12.h>
#include <d3d11.h>
#include <d3d12.h>
#include <shobjidl.h>
#endif
#ifdef HE_IMGUI_VULKAN_ENABLED
#include <vulkan/vulkan.h>
#include <imgui_impl_vulkan.h>
#endif
#ifdef HE_IMGUI_METAL_ENABLED
#include "ImGuiMetalBridge.h"
#include <Backends/Metal/MetalRenderer.h>
#endif
#include <ImGuizmo.h>

// Builds the editor's default dock layout into the given dockspace node. Only
// called when the imgui.ini did not already provide a layout (DockBuilderGetNode
// == nullptr), so a saved arrangement always wins. Windows the ini doesn't place
// fall into this default instead of floating loose. Mirrors the panel layout in
// the reference screenshots: thin toolbar floats on top; Quick Settings left,
// World Outliner + Details stacked right, Content Browser bottom, Scene centre.
// Set by View > Reset Layout; consumed by the dockspace block in RenderEditor()
// to force a rebuild of the default layout even when a layout is already loaded.
static bool s_resetLayoutRequested = false;


// Toggled by Edit > Preferences (Ctrl+,); drives the Preferences window.
static bool s_showPreferences = false;

// Toggled by View > Performance Profiler; drives the profiler panel.
static bool s_showProfiler = false;

// (Level Script + Game Instance open as editor tabs, not toggled windows.)

// Build > Export Project modal state. Editable fields mirror the selected
// ExportProfile (persisted in the .heproj); the export itself runs on a worker
// thread so packing/compression never freezes the UI.
static bool   s_showExportModal   = false;
static int    s_exportProfileIdx  = 0;             // index into exportProfiles
static std::string s_exportOutputDir;
static bool   s_exportCompress    = true;
static bool   s_exportEncrypt     = false;
static bool   s_exportModSupport  = false;
static std::string s_exportExcludes;               // one glob pattern per line
static bool   s_exportIncremental = true;
static bool   s_exportAppBundle   = false;         // macOS .app bundle
static bool   s_exportCompileHC   = false;         // compile HorizonCode → C++ (not implemented yet)
static std::string s_exportPlatform = "Host";      // exportPlatformName() value
static uint32_t s_exportShaderBackends = (1u << 4) | (1u << 0); // Metal|OpenGL bitmask of 1u<<RendererBackend

// True when the selected target produces macOS binaries the editor can bundle +
// sign — i.e. this editor runs on macOS and targets Host or macOS. Building a
// signed .app requires codesign, so it is a macOS-host-only feature.
static bool exportAppBundleApplicable(const std::string& platform)
{
#ifdef __APPLE__
    return platform == "Host" || platform == "macOS";
#else
    (void)platform; return false;
#endif
}
static std::string              s_exportStartupScene;  // project-relative; "" = current scene
static std::vector<std::string> s_exportSceneChoices;  // .hescene files found on modal open
static std::string s_exportNewProfileName;
// Runtime-bundle lookup cache for the modal's live display: findRuntimeBundle
// stats a couple dozen paths — fine once, wasteful every frame. Keyed by the
// selected platform; cleared when the modal opens so a freshly built runtime
// shows up on reopen.
static std::string           s_exportBundleKey;
static std::filesystem::path s_exportBundleCache;
// Worker-thread state: the callback writes the atomics + the mutex-guarded
// strings; the UI thread only reads them and joins once running flips false.
static std::atomic<bool> s_exportRunning{false};
static std::atomic<int>  s_exportDone{0};
static std::atomic<int>  s_exportTotal{0};
static std::mutex        s_exportMutex;            // guards the two strings below
static std::string       s_exportCurrentFile;
static std::string       s_exportResultShared;
static std::string       s_exportResult;           // UI-side copy (main thread only)
static std::thread       s_exportThread;

void EditorUI::joinPendingExport()
{
	// Called on editor shutdown: an export in flight must finish before the
	// process tears down statics (a joinable std::thread dying = std::terminate).
	if (s_exportThread.joinable()) s_exportThread.join();
}

// One exclude pattern per line; blank lines and surrounding whitespace dropped.
static std::vector<std::string> parseExcludeLines(const char* buf)
{
	std::vector<std::string> out;
	std::string line;
	for (const char* c = buf;; ++c)
	{
		if (*c == '\n' || *c == '\r' || *c == '\0')
		{
			const auto a = line.find_first_not_of(" \t");
			if (a != std::string::npos)
			{
				const auto b = line.find_last_not_of(" \t");
				out.push_back(line.substr(a, b - a + 1));
			}
			line.clear();
			if (*c == '\0') break;
		}
		else line += *c;
	}
	return out;
}

// ─── Precompiled material shaders (cook-time) ───────────────────────────────
// The exporter (in HE_Core) cannot link the shader cross-compiler, so it calls
// back into the editor with a material's canonical fragment GLSL + a bitmask of
// target backends (1u << HE::RendererBackend). We cross-compile the standard
// vertex + this fragment for each requested backend and return the PSHD blob the
// runtime decodes into MaterialAsset::precompiledShaders. Empty result → the
// exporter simply omits the chunk and the shipped game cross-compiles at load.
static std::vector<uint8_t> CompileMaterialShaderVariants(const std::string& fragGlsl,
                                                          const std::string& vertBody,
                                                          uint32_t backends)
{
	using LB = HE::MaterialShaderLibrary::Backend;
	if (fragGlsl.empty() || backends == 0) return {};

	// RendererBackend value → cross-compiler backend. D3D11/D3D12 share HLSL.
	auto mapBackend = [](HE::RendererBackend rb, LB& out) -> bool {
		switch (rb) {
			case HE::RendererBackend::OpenGL: out = LB::GLSL410; return true;
			case HE::RendererBackend::Vulkan: out = LB::SpirV;   return true;
			case HE::RendererBackend::D3D11:
			case HE::RendererBackend::D3D12:  out = LB::HLSL;     return true;
			case HE::RendererBackend::Metal:  out = LB::Metal;    return true;
		}
		return false;
	};

	// SPIR-V words → a byte string (the variant stores backend-native text OR, for
	// Vulkan, the raw SPIR-V bytes in the same string field; runtime reinterprets).
	auto spirvToBytes = [](const std::vector<uint32_t>& words) {
		std::string s;
		s.resize(words.size() * sizeof(uint32_t));
		if (!words.empty()) std::memcpy(s.data(), words.data(), s.size());
		return s;
	};

	HE::MaterialShaderLibrary lib;
	const uint64_t hash = std::hash<std::string>{}(fragGlsl);

	std::vector<MaterialShaderVariant> variants;
	for (uint8_t v = 0; v <= static_cast<uint8_t>(HE::RendererBackend::Metal); ++v)
	{
		if ((backends & (1u << v)) == 0) continue;
		LB lb;
		if (!mapBackend(static_cast<HE::RendererBackend>(v), lb)) continue;

		// WPO materials bake their graph-generated vertex; everything else the shared one.
		const auto& vert = vertBody.empty()
			? lib.standardVertex(lb)
			: lib.customVertex(std::hash<std::string>{}(vertBody), vertBody, lb);
		const auto& frag = lib.fragment(hash, fragGlsl, lb);
		if (!vert.ok || !frag.ok)
		{
			Logger::Log(Logger::LogLevel::Warning,
			            ("Export: material shader precompile failed for backend "
			             + std::to_string(static_cast<int>(v)) + " — "
			             + vert.log + " " + frag.log).c_str());
			continue; // skip this backend; runtime falls back to cross-compile
		}

		MaterialShaderVariant var;
		var.backend  = v;
		var.vertex   = (lb == LB::SpirV) ? spirvToBytes(vert.spirv) : vert.source;
		var.fragment = (lb == LB::SpirV) ? spirvToBytes(frag.spirv) : frag.source;
		variants.push_back(std::move(var));
	}

	if (variants.empty()) return {};
	return HE::encodeMaterialShaderVariants(variants);
}

// Fill the export dialog fields from a profile. An empty profile outputDir
// resolves to <projectRoot>/Export/<profile name>. All string fields are
// std::string — no fixed buffers, so nothing can silently truncate (a cut-off
// exclude pattern would broaden the glob and change the pak contents).
static void exportProfileToDialog(const ExportProfile& p, const std::filesystem::path& projectRoot)
{
	s_exportOutputDir = p.outputDir.empty()
		? (projectRoot / "Export" / p.name).string()
		: p.outputDir;
	s_exportCompress     = p.compress;
	s_exportEncrypt      = p.encrypt;
	s_exportModSupport   = p.enableModSupport;
	s_exportStartupScene = p.startupScene;
	s_exportIncremental  = p.incremental;
	s_exportAppBundle    = p.appBundle;
	s_exportCompileHC    = p.compileHorizonCode;
	// Canonicalize via the enum round-trip: a hand-edited value like "windows"
	// falls back to Host — showing "Host" in the combo makes that fallback
	// visible BEFORE exporting host binaries somewhere unexpected.
	s_exportPlatform     = exportPlatformName(exportPlatformFromName(p.targetPlatform));
	s_exportShaderBackends = p.shaderBackends;
	s_exportExcludes.clear();
	for (const auto& pat : p.excludePatterns) { s_exportExcludes += pat; s_exportExcludes += '\n'; }
}

// Read the dialog fields back into a profile (the name stays as-is).
static void exportDialogToProfile(ExportProfile& p)
{
	p.outputDir        = s_exportOutputDir;
	p.compress         = s_exportCompress;
	p.encrypt          = s_exportEncrypt;
	p.enableModSupport = s_exportModSupport;
	p.startupScene     = s_exportStartupScene;
	p.excludePatterns  = parseExcludeLines(s_exportExcludes.c_str());
	p.incremental      = s_exportIncremental;
	p.targetPlatform   = s_exportPlatform;
	p.appBundle        = s_exportAppBundle;
	p.shaderBackends   = s_exportShaderBackends;
	p.compileHorizonCode = s_exportCompileHC;
}

// Active manipulation tool, shared by the viewport toolbar buttons and the
// W/E/R shortcuts and consumed by the gizmo (Move / Rotate / Scale).
static ImGuizmo::OPERATION s_gizmoOp   = ImGuizmo::TRANSLATE;
// Gizmo orientation: LOCAL (object axes) or WORLD (axis-aligned). Toolbar toggle.
static ImGuizmo::MODE      s_gizmoMode = ImGuizmo::LOCAL;
// Show ImGuizmo's outer "screen-space" rotation ring (rotate about the view axis).
// Off by default — its viewport-relative behaviour is confusing.
static bool                s_rotateScreen = false;

// Requested by fast local content edits (create/rename) that want the file list
// updated this frame without the heavyweight "##ContentRefresh" progress modal.
// Consumed at the top of render(), outside the content-folder shared lock.
static bool s_quietContentRefresh = false;

// Landscape sculpt tool state (shared between the panel and viewport)
enum class TerrainTool { Raise, Lower, Smooth, Flatten, Ramp, Roughen };
static TerrainTool s_terrainTool     = TerrainTool::Raise;
static float       s_brushRadius     = 10.0f;  // inner full-strength radius (m)
static float       s_falloffRadius   = 5.0f;   // transition width — strength falls linearly to 0
static float       s_brushStrength   = 5.0f;
static bool        s_brushWasDown    = false;   // tracks LMB edge for undo
// Stroke-scoped state, captured on the LMB-down edge (see the sculpt block):
static float       s_flattenTarget   = 0.0f;    // Flatten: height to pull toward
static glm::vec3   s_rampStartWS{};             // Ramp: world-space start of the gradient
static float       s_rampStartH      = 0.0f;    // Ramp: terrain height at the start point
static bool        s_rampValid       = false;   // Ramp: a start point was captured this stroke

// Picking + sculpt AABB cache (keyed by mesh asset UUID)
static std::unordered_map<HE::UUID, HE::AABB> s_aabbCache;

static void BuildDefaultDockLayout(ImGuiID dockspaceId, const ImVec2& size)
{
	ImGui::DockBuilderRemoveNode(dockspaceId);
	ImGui::DockBuilderAddNode(dockspaceId,
		ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
	ImGui::DockBuilderSetNodeSize(dockspaceId, size);

	// Fractions are relative to the node being split (they compound). Tuned to the
	// reference layout: Quick Settings ~18% left, World Outliner/Details ~21%
	// right (split 50/50), Content Browser ~33% of the centre column's height.
	ImGuiID dockMain  = dockspaceId;
	ImGuiID dockLeft  = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left,  0.18f, nullptr, &dockMain);
	ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.26f, nullptr, &dockMain);
	ImGuiID dockDown  = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down,  0.33f, nullptr, &dockMain);
	ImGuiID dockRightBottom =
		ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Down, 0.50f, nullptr, &dockRight);

	ImGui::DockBuilderDockWindow("Quick Settings", dockLeft);
	ImGui::DockBuilderDockWindow("World Outliner", dockRight);
	ImGui::DockBuilderDockWindow("Details",        dockRightBottom);
	ImGui::DockBuilderDockWindow("Content Browser", dockDown);
	ImGui::DockBuilderDockWindow("Scene",          dockMain);
	ImGui::DockBuilderFinish(dockspaceId);
}

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
enum class SettingsMode { Preferences, QuickSettings };

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
static void DrawEngineSettings(AppContext& ctx, SettingsMode mode)
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
			auto pick = [&](const char* label, HE::GraphicsAPI api){
				if (ImGui::Selectable(label)) { ctx.globalState->setSelectedRHI(api); ctx.backendName = getRHIName(api); }
			};
			pick("OpenGL", HE::GraphicsAPI::OpenGL);
#ifdef HE_VULKAN_ENABLED
			pick("Vulkan", HE::GraphicsAPI::Vulkan);
#endif
#ifdef _WIN32
			pick("DirectX11", HE::GraphicsAPI::D3D11);
			pick("DirectX12", HE::GraphicsAPI::D3D12);
#endif
#ifdef __APPLE__
			pick("Metal", HE::GraphicsAPI::Metal);
#endif
			ImGui::EndCombo();
		}
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

static void DrawPreferencesWindow(AppContext& ctx, bool& open)
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
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
	                        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	if (ImGui::BeginPopupModal("Preferences", nullptr, ImGuiWindowFlags_NoCollapse))
	{
		EditorConfig& cfg = ctx.editorConfig;

		ImGui::TextDisabled("Tick the pin on a setting to show it in Quick Settings.");
		ImGui::Spacing();

		// The full engine-settings catalog (each row has a pin toggle here).
		DrawEngineSettings(ctx, SettingsMode::Preferences);

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
#endif // HE_IMGUI_ENABLED

// ─── render ───────────────────────────────────────────────────────────────────
void EditorUI::render(AppContext& ctx, float dt)
{
#ifdef HE_IMGUI_ENABLED
    if (!ctx.imguiReady) return;

    ImGuiIO& io = ImGui::GetIO();

    // Begin new ImGui frame — platform backend is backend-specific
    switch (ctx.backend)
    {
    case RendererFactory::Backend::OpenGL:
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        break;
#ifdef _WIN32
    case RendererFactory::Backend::D3D11:
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        break;
    case RendererFactory::Backend::D3D12:
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        {
            io.DisplaySize = ImVec2(static_cast<float>(ctx.window->GetWidth()),
                static_cast<float>(ctx.window->GetHeight()));
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        }
        break;
#endif
#ifdef HE_IMGUI_VULKAN_ENABLED
    case RendererFactory::Backend::Vulkan:
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        break;
#endif
#ifdef HE_IMGUI_METAL_ENABLED
    case RendererFactory::Backend::Metal:
        if (auto* mtl = static_cast<MetalRenderer*>(ctx.renderer))
            ImGuiMetalBridge::NewFrame(mtl->GetFramePassDescriptor());
        ImGui_ImplSDL3_NewFrame();
        break;
#endif
    default:
        break;
    }

    ImGui::NewFrame();
    ImGuizmo::BeginFrame();

    // Apply the user's UI font scale preference (clamped to a sane range).
    ImGui::GetStyle().FontScaleMain = std::clamp(ctx.editorConfig.UiFontScale, 0.5f, 3.0f);

    // ── Content-Refresh Popup ─────────────────────────────────────────────────
    if (ctx.contentRefreshPending || ctx.contentRefreshDone)
    {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(360, 100), ImGuiCond_Always);
        if (ImGui::BeginPopupModal("##ContentRefresh", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        {
            float textY = (100.0f - ImGui::GetTextLineHeightWithSpacing() * 2.0f) * 0.5f;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textY);
            ImGui::SetCursorPosX((360.0f - ImGui::CalcTextSize("Projektdaten werden aktualisiert...").x) * 0.5f);
            ImGui::TextUnformatted("Projektdaten werden aktualisiert...");

            if (ctx.contentRefreshPending)
            {
                ctx.globalState->refreshContentFolder();
                ctx.contentRefreshPending = false;
                ctx.contentRefreshDone    = true;
            }
            else
            {
                ImGui::CloseCurrentPopup();
                ctx.contentRefreshDone = false;
                ctx.projectLoaded      = true;
            }
            ImGui::EndPopup();
        }
        else
        {
            ImGui::OpenPopup("##ContentRefresh");
        }
    }

    // ── Silent content refresh (create/rename) ───────────────────────────────
    // Runs here, before RenderEditor acquires the content-folder shared lock, so
    // refreshContentFolder()'s unique_lock can't deadlock against it.
    if (s_quietContentRefresh && ctx.globalState)
    {
        ctx.globalState->refreshContentFolder();
        s_quietContentRefresh = false;
    }

    // ── Route to either the Project Hub or the full Editor UI ─────────────────
    if (ctx.projectLoaded)
        RenderEditor(ctx, dt);
    else
        RenderProjectHub(ctx);

    ImGui::Render();

    // ── Multi-viewport / platform windows ─────────────────────────────────────
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        SDL_Window*   backupWin = SDL_GL_GetCurrentWindow();
        SDL_GLContext backupCtx = SDL_GL_GetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        if (backupWin && backupCtx)
            SDL_GL_MakeCurrent(backupWin, backupCtx);
    }
#endif // HE_IMGUI_ENABLED
}

// ─── Performance Profiler window (View > Performance Profiler) ──────────────────
// Three tabs over the runtime EngineProfiler:
//   Overview     — always-live HUD: FPS / CPU / GPU / counters + a frame-time graph.
//   Capture      — benchmark capture (F9 → JSON dump), single-frame capture, toggles.
//   Frame Detail — the full per-pass GPU + per-scope CPU breakdown of one frame
//                  (the single-frame capture if taken, else the last captured frame).
// Reads the profiler singleton directly. Sets liveEnabled() so the app feeds the HUD.
#ifdef HE_IMGUI_ENABLED
// Full breakdown of one captured frame: counters, GPU passes, CPU scope tree.
static void DrawFrameDetail(const ProfFrameRecord& f)
{
    const double fps = f.deltaMs > 0.0 ? 1000.0 / f.deltaMs : 0.0;
    ImGui::Text("Frame %llu", static_cast<unsigned long long>(f.index));
    ImGui::Text("CPU %.3f ms", f.cpuFrameMs);
    ImGui::SameLine(160); ImGui::Text("frame %.3f ms (%.0f FPS)", f.deltaMs, fps);
    if (f.gpuFrameMs >= 0.0)
    {
        ImGui::Text("GPU %.3f ms", f.gpuFrameMs);
        if (f.gpuTimingMode && f.gpuTimingMode[0])
        { ImGui::SameLine(160); ImGui::TextDisabled("mode: %s", f.gpuTimingMode); }
    }
    else
        ImGui::TextDisabled("GPU n/a on this backend");

    const ProfRenderStats& s = f.stats;
    ImGui::Text("draws %u  ·  tris %u  ·  objects %u/%u visible",
                s.drawCalls, s.triangles, s.visibleObjects, s.totalObjects);
    if (s.vramBudgetMB > 0.0)
        ImGui::Text("VRAM %.0f / %.0f MB", s.vramUsedMB, s.vramBudgetMB);

    // ── GPU passes ──────────────────────────────────────────────────────────
    if (!f.gpuPasses.empty())
    {
        const std::string gpuMode = f.gpuTimingMode ? f.gpuTimingMode : "";
        // "detailed" (Metal, serialized cmd-buffer/pass) and "gl-timer" (GL timer
        // queries) are both exclusive + additive per-pass, so the sum is meaningful;
        // "counter" spans overlap on TBDR and must NOT be summed.
        const bool detailed = gpuMode == "detailed" || gpuMode == "gl-timer";
        const double gref = f.gpuFrameMs > 0.0 ? f.gpuFrameMs : 1.0;
        ImGui::Separator();
        ImGui::TextUnformatted(detailed ? "GPU passes (exclusive, additive)"
                                        : "GPU passes (per-encoder spans — see caveat)");
        double sumExact = 0.0; bool anyExact = false;
        for (const ProfGpuPass& gp : f.gpuPasses)
        {
            const char* nm = gp.name ? gp.name : "?";
            if (gp.approx)
            {
                ImGui::TextDisabled("    ~ %s", nm);
                ImGui::SameLine(210); ImGui::TextDisabled("%7.3f ms", gp.ms);
            }
            else
            {
                sumExact += gp.ms; anyExact = true;
                ImGui::Text("%s", nm);
                ImGui::SameLine(210); ImGui::Text("%7.3f ms", gp.ms);
            }
            ImGui::SameLine(300);
            ImGui::ProgressBar(static_cast<float>(gp.ms / gref), ImVec2(-1, 0), "");
        }
        if (anyExact && f.gpuFrameMs > 0.0)
        {
            if (!detailed && sumExact > f.gpuFrameMs * 1.05)
                ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.2f, 1.0f),
                    "\xCE\xA3 spans %.2f ms = %.1fx GPU frame — spans OVERLAP, not exclusive; do not sum.",
                    sumExact, sumExact / f.gpuFrameMs);
            else
            {
                ImGui::Text("\xCE\xA3 passes %.3f ms", sumExact);
                ImGui::SameLine(220);
                ImGui::TextDisabled("untimed %.3f ms", f.gpuFrameMs - sumExact);
            }
        }
        if (!detailed)
            ImGui::TextDisabled("Per-encoder spans overlap on TBDR — enable 'Detailed GPU' for exclusive per-pass.");
        else if (gpuMode == "detailed")
            ImGui::TextDisabled("Note: the FIRST pass (Shadow) absorbs GPU queue/present latency in a single\n"
                                "serialized frame — it can read high here. Trust the Overview median, not one frame.");
        else // gl-timer: exact, exclusive per-pass GPU time — no serialization caveat.
            ImGui::TextDisabled("GL timer queries: exact per-pass GPU time; \xCE\xA3 passes + untimed = GPU frame.");
    }

    // ── CPU scopes (nested) ─────────────────────────────────────────────────
    if (!f.scopes.empty())
    {
        const double ref = f.cpuFrameMs > 0.0 ? f.cpuFrameMs : 1.0;
        ImGui::Separator();
        ImGui::TextUnformatted("CPU scopes");
        for (const ProfScopeSample& sc : f.scopes)
        {
            std::string label(static_cast<size_t>(sc.depth) * 2, ' ');
            label += sc.name ? sc.name : "?";
            ImGui::Text("%s", label.c_str());
            ImGui::SameLine(210); ImGui::Text("%7.3f ms", sc.ms);
            ImGui::SameLine(300);
            ImGui::ProgressBar(static_cast<float>(sc.ms / ref), ImVec2(-1, 0), "");
        }
    }
}
#endif // HE_IMGUI_ENABLED

static void DrawProfilerWindow(AppContext& ctx, bool& open)
{
#ifdef HE_IMGUI_ENABLED
    EngineProfiler& prof = EngineProfiler::instance();
    if (!open) { prof.setLiveEnabled(false); return; }

    ImGui::SetNextWindowSize(ImVec2(480, 600), ImGuiCond_FirstUseEver);
    const bool visible = ImGui::Begin("Performance Profiler", &open);
    // Feed the live HUD only while the window is actually visible (not collapsed).
    prof.setLiveEnabled(visible);
    if (!visible) { ImGui::End(); return; }

    if (ImGui::BeginTabBar("##profTabs"))
    {
        // ── Overview: live HUD + frame-time graph ───────────────────────────
        if (ImGui::BeginTabItem("Overview"))
        {
            const std::vector<ProfLiveFrame> live = prof.liveSnapshot();
            if (live.empty())
                ImGui::TextDisabled("Collecting live data…");
            else
            {
                const ProfLiveFrame& cur = live.back();
                const double fps = cur.deltaMs > 0.0 ? 1000.0 / cur.deltaMs : 0.0;
                ImGui::Text("%.1f FPS", fps);
                ImGui::SameLine(120); ImGui::Text("frame %.2f ms", cur.deltaMs);
                ImGui::Text("CPU %.2f ms", cur.cpuFrameMs);
                ImGui::SameLine(120);
                if (cur.gpuFrameMs >= 0.0) ImGui::Text("GPU %.2f ms", cur.gpuFrameMs);
                else                       ImGui::TextDisabled("GPU n/a");
                ImGui::Text("draws %u  ·  tris %u  ·  objects %u/%u",
                            cur.draws, cur.triangles, cur.visible, cur.total);

                // Frame-time graph over the live window (+ avg/max overlay).
                std::vector<float> ftimes; ftimes.reserve(live.size());
                float mx = 0.0f; double sum = 0.0;
                for (const ProfLiveFrame& lf : live)
                {
                    const float v = static_cast<float>(lf.deltaMs);
                    ftimes.push_back(v); sum += v; if (v > mx) mx = v;
                }
                const float avg = ftimes.empty() ? 0.0f : static_cast<float>(sum / ftimes.size());
                char overlay[64];
                std::snprintf(overlay, sizeof(overlay), "avg %.2f  max %.2f ms", avg, mx);
                ImGui::Separator();
                ImGui::TextUnformatted("Frame time (ms)");
                ImGui::PlotLines("##ft", ftimes.data(), static_cast<int>(ftimes.size()),
                                 0, overlay, 0.0f, mx > 0.0f ? mx * 1.1f : 1.0f, ImVec2(-1, 80));

                // GPU-time graph (only if available).
                bool anyGpu = false;
                std::vector<float> gtimes; gtimes.reserve(live.size());
                float gmx = 0.0f;
                for (const ProfLiveFrame& lf : live)
                {
                    const float v = lf.gpuFrameMs >= 0.0 ? static_cast<float>(lf.gpuFrameMs) : 0.0f;
                    if (lf.gpuFrameMs >= 0.0) anyGpu = true;
                    gtimes.push_back(v); if (v > gmx) gmx = v;
                }
                if (anyGpu)
                {
                    ImGui::TextUnformatted("GPU time (ms, whole frame)");
                    ImGui::PlotLines("##gt", gtimes.data(), static_cast<int>(gtimes.size()),
                                     0, nullptr, 0.0f, gmx > 0.0f ? gmx * 1.1f : 1.0f, ImVec2(-1, 60));
                }
            }
            ImGui::EndTabItem();
        }

        // ── Capture controls ────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Capture"))
        {
            const bool recording = prof.isRecordingOrPending();
            if (recording)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
                if (ImGui::Button("Stop & Dump  (F9)", ImVec2(-1, 0)) && ctx.toggleProfilerCapture)
                    ctx.toggleProfilerCapture();
                ImGui::PopStyleColor();
                ImGui::TextDisabled("Recording — %zu frames (vsync off)", prof.recordedFrames());
            }
            else
            {
                if (ImGui::Button("Start Benchmark Capture  (F9)", ImVec2(-1, 0)) && ctx.toggleProfilerCapture)
                    ctx.toggleProfilerCapture();
                ImGui::TextDisabled("Benchmark = vsync-off multi-frame capture → JSON dump.");

                // Single-frame capture: one frame in full detail (forces detailed GPU),
                // shown in the Frame Detail tab. No dump.
                if (ImGui::Button("Capture Single Frame", ImVec2(-1, 0)))
                    prof.requestSingleFrameCapture();
                ImGui::TextDisabled("One frame (CPU scopes + counters + GPU) → 'Frame Detail'. Fast unless");
                ImGui::TextDisabled("'Detailed GPU' is ticked (then exclusive per-pass, but that frame is slow).");
            }

            ImGui::Separator();
            // Detailed GPU capture (serialized per-pass). Also auto-forced for single frames.
            bool detailed = prof.detailedGpuCapture();
            if (ImGui::Checkbox("Detailed GPU pass timing (serializes GPU — capture only)", &detailed))
                prof.setDetailedGpuCapture(detailed);
            ImGui::TextDisabled("On = exclusive per-pass GPU (ranking/upper bound). FPS during capture is meaningless.");

            // Debug: tint lit fragments by shadow-cascade index (red/green/blue) to
            // verify the CSM split placement (cascade 0 should hug the camera).
            static bool s_dbgCascades = false;
            if (ImGui::Checkbox("Debug: shadow cascades (cascade-index tint)", &s_dbgCascades))
                if (ctx.renderer) ctx.renderer->SetShadowDebug(s_dbgCascades);

            ImGui::Separator();
            if (ImGui::Button("Dump Now"))
            {
                std::string p = prof.dumpNow();
                if (!p.empty()) Logger::Log(Logger::LogLevel::Info, ("Profiler dump: " + p).c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Dumps Folder"))
            {
                std::string dir = !prof.dumpsDir().empty()
                                ? prof.dumpsDir()
                                : (ctx.globalState ? ctx.globalState->getDumpsDir() : std::string());
                if (!dir.empty()) SDL_OpenURL(("file://" + dir).c_str());
            }
            {
                std::string dir = !prof.dumpsDir().empty()
                                ? prof.dumpsDir()
                                : (ctx.globalState ? ctx.globalState->getDumpsDir() : std::string("(starts on first capture)"));
                ImGui::TextDisabled("%s", dir.c_str());
            }
            ImGui::EndTabItem();
        }

        // ── Frame Detail: single-frame capture, else last captured frame ────
        if (ImGui::BeginTabItem("Frame Detail"))
        {
            const ProfFrameRecord* single = prof.singleFrame();
            const ProfFrameRecord* last   = prof.lastFrame();
            const ProfFrameRecord* f      = single ? single : last;
            if (single) ImGui::TextDisabled("Source: single-frame capture");
            else if (last) ImGui::TextDisabled("Source: last benchmark frame");
            ImGui::Separator();
            if (f) DrawFrameDetail(*f);
            else   ImGui::TextDisabled("No frame yet — use 'Capture Single Frame' or run a benchmark.");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
#else
    (void)ctx; (void)open;
#endif
}

// ─── Project Hub ──────────────────────────────────────────────────────────────
void EditorUI::RenderProjectHub(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(vp->Pos,  ImGuiCond_Always);
    ImGui::SetNextWindowSize(vp->Size, ImGuiCond_Always);
    ImGui::SetNextWindowViewport(vp->ID);

    const ImGuiWindowFlags kHubFlags =
        ImGuiWindowFlags_NoTitleBar   |
        ImGuiWindowFlags_NoResize     |
        ImGuiWindowFlags_NoMove       |
        ImGuiWindowFlags_NoScrollbar  |
        ImGuiWindowFlags_NoCollapse   |
        ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##ProjectHub", nullptr, kHubFlags);
    ImGui::PopStyleVar(3);

    // ── Header bar ────────────────────────────────────────────────────────────
    const float headerH = 56.0f;
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.12f, 1.0f));
    ImGui::BeginChild("##HubHeader", ImVec2(vp->Size.x, headerH), false,
        ImGuiWindowFlags_NoScrollbar);

    if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
    const char* title = "Horizon Engine " HE_VERSION_STRING " \"" HE_VERSION_CODENAME "\"  —  Project Hub";

    const float logoDisplayH = headerH - 16.0f;
    if (ctx.logoTexture && ctx.logoW > 0 && ctx.logoH > 0)
    {
        const float logoDisplayW = logoDisplayH * (static_cast<float>(ctx.logoW) / static_cast<float>(ctx.logoH));
        ImGui::SetCursorPos(ImVec2(12.0f, (headerH - logoDisplayH) * 0.5f));
        ImGui::Image(ctx.logoTexture, ImVec2(logoDisplayW, logoDisplayH));
        ImGui::SameLine();
        ImGui::SetCursorPosY((headerH - ImGui::GetTextLineHeight()) * 0.5f);
    }
    else
    {
        ImGui::SetCursorPos(ImVec2(20.0f, (headerH - ImGui::GetTextLineHeight()) * 0.5f));
    }
    ImGui::Text("%s", title);
    if (ctx.fontHeading) ImGui::PopFont();

    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ── Three panels ──────────────────────────────────────────────────────────
    const float bodyY   = headerH + 1.0f;
    const float bodyH   = vp->Size.y - bodyY;
    const float panelW  = vp->Size.x / 3.0f;
    const float padding = 16.0f;

    static const std::array<const char*, 4> kPresetNames = {
        "Empty Project",
        "Game",
        "Simulation",
        "Tool",
    };
    static const std::array<const char*, 4> kPresetDesc = {
        "Only the basic folder skeleton, no extra content.",
        "Assets, Scenes and Scripts folders + a sample scene file.",
        "Assets, Scenes and Data folders.",
        "Assets and Source folders.",
    };

    // ════════════════════════════════════════════════════════════════════
    // PANEL 1 — Create Project
    // ════════════════════════════════════════════════════════════════════
    ImGui::SetCursorPos(ImVec2(0.0f, bodyY));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.13f, 0.15f, 1.0f));
    ImGui::BeginChild("##PanelCreate", ImVec2(panelW - 1.0f, bodyH), false,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(padding, padding));
    if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
    ImGui::Text("Create Project");
    if (ctx.fontSubheading) ImGui::PopFont();
    ImGui::SetCursorPosX(padding);
    ImGui::Separator();
    ImGui::Spacing();

    if (ctx.fontBody) ImGui::PushFont(ctx.fontBody);

    ImGui::SetCursorPosX(padding);
    ImGui::Text("Template");
    ImGui::SetCursorPosX(padding);
    ImGui::PushItemWidth(panelW - padding * 2.0f);
    ImGui::ListBox("##Presets", &ctx.hubSelectedPreset,
        kPresetNames.data(), static_cast<int>(kPresetNames.size()), 4);
    ImGui::PopItemWidth();

    ImGui::SetCursorPosX(padding);
    ImGui::TextDisabled("%s", kPresetDesc[ctx.hubSelectedPreset]);

    ImGui::Spacing();
    ImGui::SetCursorPosX(padding);
    ImGui::Text("Project Name");
    ImGui::SetCursorPosX(padding);
    ImGui::PushItemWidth(panelW - padding * 2.0f);
    ImGui::InputText("##ProjName", ctx.hubProjectName, ctx.hubProjectNameSize);
    ImGui::PopItemWidth();

    ImGui::Spacing();
    ImGui::SetCursorPosX(padding);
    ImGui::Text("Project Directory");
    ImGui::SetCursorPosX(padding);
    ImGui::PushItemWidth(panelW - padding * 2.0f - 70.0f);
    ImGui::InputText("##ProjDir", ctx.hubProjectDir, ctx.hubProjectDirSize);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Browse##dir"))
    {
#ifdef _WIN32
        {
            IFileOpenDialog* pDlg = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg))))
            {
                DWORD dwOpts = 0;
                pDlg->GetOptions(&dwOpts);
                pDlg->SetOptions(dwOpts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);

                HWND hwnd = nullptr;
                if (ctx.window)
                {
                    hwnd = static_cast<HWND>(SDL_GetPointerProperty(
                        SDL_GetWindowProperties(ctx.window->GetNativeWindow()),
                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
                }

                if (SUCCEEDED(pDlg->Show(hwnd)))
                {
                    IShellItem* pItem = nullptr;
                    if (SUCCEEDED(pDlg->GetResult(&pItem)))
                    {
                        PWSTR pPath = nullptr;
                        if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pPath)))
                        {
                            int len = WideCharToMultiByte(CP_UTF8, 0, pPath, -1,
                                nullptr, 0, nullptr, nullptr);
                            if (len > 0 && len <= (int)ctx.hubProjectDirSize)
                            {
                                WideCharToMultiByte(CP_UTF8, 0, pPath, -1,
                                    ctx.hubProjectDir, ctx.hubProjectDirSize,
                                    nullptr, nullptr);
                            }
                            CoTaskMemFree(pPath);
                        }
                        pItem->Release();
                    }
                }
                pDlg->Release();
            }
        }
#else
        SDL_ShowOpenFolderDialog(
            [](void* userdata, const char* const* filelist, int /*filter*/)
            {
                auto* b = static_cast<SDLDialogBridge*>(userdata);
                if (filelist && filelist[0])
                {
                    *b->pendingDirResult = filelist[0];
                    *b->pendingDirReady  = true;
                }
            },
            ctx.dialogBridge,
            ctx.window ? ctx.window->GetNativeWindow() : nullptr,
            nullptr,
            false);
#endif
    }

    if (ctx.pendingDirReady)
    {
        strncpy(ctx.hubProjectDir, ctx.pendingDirResult.c_str(),
                ctx.hubProjectDirSize - 1);
        ctx.hubProjectDir[ctx.hubProjectDirSize - 1] = '\0';
        ctx.pendingDirReady  = false;
        ctx.pendingDirResult.clear();
    }

    ImGui::Spacing();
    if (!ctx.hubCreateError.empty())
    {
        ImGui::SetCursorPosX(padding);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", ctx.hubCreateError.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    ImGui::SetCursorPosX(padding);
    const float btnW = panelW - padding * 2.0f;
    if (ImGui::Button("Create##create", ImVec2(btnW, 36.0f)))
    {
        ctx.hubCreateError.clear();
        std::string name = ctx.hubProjectName;
        std::string dir  = ctx.hubProjectDir;
        if (name.empty())
        {
            ctx.hubCreateError = "Please enter a project name.";
        }
        else if (dir.empty())
        {
            ctx.hubCreateError = "Please select a project directory.";
        }
        else
        {
            std::filesystem::path projRoot = std::filesystem::path(dir) / name;
            bool ok = ctx.projectManager->createNewProject(
                projRoot.string(), name,
                static_cast<ProjectPreset>(ctx.hubSelectedPreset));
            if (ok)
            {
                const std::string& heprojPath = ctx.projectManager->currentProject().path;
                ctx.globalState->addKnownProject(heprojPath);
                ctx.globalState->writeConfig();
                ctx.contentRefreshPending = true;
                ctx.projectLoaded = true;
            }
            else
            {
                ctx.hubCreateError = "Failed to create project. Check path/permissions.";
            }
        }
    }

    if (ctx.fontBody) ImGui::PopFont();
    ImGui::EndChild();

    // ════════════════════════════════════════════════════════════════════
    // PANEL 2 — Known Projects
    // ════════════════════════════════════════════════════════════════════
    ImGui::SetCursorPos(ImVec2(panelW + 1.0f, bodyY));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.11f, 0.13f, 1.0f));
    ImGui::BeginChild("##PanelKnown", ImVec2(panelW - 2.0f, bodyH), false,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(padding, padding));
    if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
    ImGui::Text("Recent Projects");
    if (ctx.fontSubheading) ImGui::PopFont();
    ImGui::SetCursorPosX(padding);
    ImGui::Separator();
    ImGui::Spacing();

    if (ctx.fontBody) ImGui::PushFont(ctx.fontBody);

    const auto& known = ctx.globalState->getKnownProjects();
    if (known.empty())
    {
        ImGui::SetCursorPosX(padding);
        ImGui::TextDisabled("No recent projects.");
    }
    else
    {
        const float listAreaH = bodyH - 90.0f;
        ImGui::SetCursorPosX(padding);
        ImGui::BeginChild("##KnownList",
            ImVec2(panelW - padding * 2.0f, listAreaH), true);

        for (int i = 0; i < static_cast<int>(known.size()); ++i)
        {
            std::filesystem::path p(known[i]);
            std::string name = p.stem().string();
            if (name.empty()) name = known[i];
            std::string label = name + "\n" + known[i];
            bool exists = std::filesystem::exists(p);

            ImGui::PushID(i);

            if (!exists)
            {
                ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.4f, 0.15f, 0.15f, 0.6f));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.5f, 0.18f, 0.18f, 0.8f));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.45f,0.16f, 0.16f, 1.0f));
            }

            ImGui::Selectable(label.c_str(), false,
                ImGuiSelectableFlags_None, ImVec2(0, 40.0f));

            if (!exists) ImGui::PopStyleColor(4);

            if (ImGui::IsItemClicked() && exists && ctx.projectManager->loadProject(known[i]))
            {
                ctx.globalState->addKnownProject(known[i]);
                ctx.globalState->writeConfig();
                ctx.contentRefreshPending = true;
                ctx.projectLoaded = true;
            }

            if (ImGui::BeginPopupContextItem("##KnownCtx"))
            {
                if (ImGui::MenuItem("Aus Liste entfernen"))
                {
                    ctx.hubRemoveIndex     = i;
                    ctx.hubRemoveRequested = true;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ImGui::Separator();
        }

        if (ctx.hubRemoveRequested)
        {
            ctx.hubRemoveRequested = false;
            ImGui::OpenPopup("##ConfirmRemove");
        }
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("##ConfirmRemove", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            if (ctx.hubRemoveIndex >= 0 && ctx.hubRemoveIndex < static_cast<int>(known.size()))
            {
                std::filesystem::path rp(known[ctx.hubRemoveIndex]);
                ImGui::Text("Projekt aus der Liste entfernen?");
                ImGui::Spacing();
                ImGui::TextDisabled("%s", rp.stem().string().c_str());
                ImGui::TextDisabled("%s", known[ctx.hubRemoveIndex].c_str());
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                if (ImGui::Button("Entfernen", ImVec2(120, 0)))
                {
                    ctx.globalState->removeKnownProject(known[ctx.hubRemoveIndex]);
                    ctx.globalState->writeConfig();
                    ctx.hubRemoveIndex = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Abbrechen", ImVec2(120, 0)))
                {
                    ctx.hubRemoveIndex = -1;
                    ImGui::CloseCurrentPopup();
                }
            }
            else
            {
                ctx.hubRemoveIndex = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::EndChild();
    }

    if (ctx.fontBody) ImGui::PopFont();
    ImGui::EndChild();

    // ════════════════════════════════════════════════════════════════════
    // PANEL 3 — Open Project
    // ════════════════════════════════════════════════════════════════════
    ImGui::SetCursorPos(ImVec2(panelW * 2.0f + 1.0f, bodyY));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.13f, 0.15f, 1.0f));
    ImGui::BeginChild("##PanelOpen", ImVec2(panelW, bodyH), false,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(padding, padding));
    if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
    ImGui::Text("Open Project");
    if (ctx.fontSubheading) ImGui::PopFont();
    ImGui::SetCursorPosX(padding);
    ImGui::Separator();
    ImGui::Spacing();

    if (ctx.fontBody) ImGui::PushFont(ctx.fontBody);

    ImGui::SetCursorPosX(padding);
    ImGui::TextWrapped(
        "Select an existing HorizonEngine project file (.heproj) "
        "to open it in the editor.");
    ImGui::Spacing();

    if (!ctx.hubOpenError.empty())
    {
        ImGui::SetCursorPosX(padding);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", ctx.hubOpenError.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    ImGui::SetCursorPosX(padding);
    if (ImGui::Button("Browse .heproj...", ImVec2(panelW - padding * 2.0f, 36.0f)))
    {
        ctx.hubOpenError.clear();
        SDL_DialogFileFilter filters[] = {
            { "HorizonEngine Project", "heproj" },
        };
        SDL_ShowOpenFileDialog(
            [](void* userdata, const char* const* filelist, int /*filter*/)
            {
                auto* b = static_cast<SDLDialogBridge*>(userdata);
                if (filelist && filelist[0])
                {
                    *b->pendingFileResult = filelist[0];
                    *b->pendingFileReady  = true;
                }
            },
            ctx.dialogBridge,
            ctx.window ? ctx.window->GetNativeWindow() : nullptr,
            filters, 1,
            nullptr,
            false);
    }

    if (ctx.pendingFileReady)
    {
        ctx.pendingFileReady = false;
        std::string chosen = ctx.pendingFileResult;
        ctx.pendingFileResult.clear();
        if (ctx.projectManager->loadProject(chosen))
        {
            ctx.globalState->addKnownProject(chosen);
            ctx.globalState->writeConfig();
            ctx.contentRefreshPending = true;
            ctx.projectLoaded = true;
        }
        else
        {
            ctx.hubOpenError = "Failed to load project file.";
        }
    }

    if (ctx.fontBody) ImGui::PopFont();
    ImGui::EndChild();

    ImGui::End();
#endif // HE_IMGUI_ENABLED
}

// Starter template for a freshly created script, by language (0 = Lua, 1 = Python).
static const char* scriptStarterTemplate(int lang)
{
	static const char* kLua =
		"local M = {}\n\n"
		"function M.onStart(self)\nend\n\n"
		"function M.onUpdate(self, dt)\nend\n\n"
		"return M\n";
	static const char* kPy =
		"import horizon\n\n"
		"class NewScript(horizon.Behavior):\n"
		"    def on_start(self):\n        pass\n\n"
		"    def on_update(self, dt):\n        pass\n";
	return (lang == 1) ? kPy : kLua;
}

// Rewrite a just-created script stub's language byte (SLNG) + starter (SRC), keeping
// every other chunk (META → the UUID). Used when the user flips the language in the
// name-on-create dialog, before they've opened/edited the file.
static void rewriteScriptStubLanguage(const std::string& path, int lang)
{
	HAsset::Reader r;
	if (!r.open(path)) return;
	const uint16_t type = r.assetType();
	HAsset::Writer w;
	for (const auto& c : r.chunks())
		if (c.id != HAsset::CHUNK_SRC && c.id != HAsset::CHUNK_SLNG)
			w.addChunk(c.id, c.data.data(), c.data.size());
	const char* starter = scriptStarterTemplate(lang);
	w.addChunk(HAsset::CHUNK_SRC, starter, std::char_traits<char>::length(starter));
	const uint8_t lb = static_cast<uint8_t>(lang);
	w.addChunk(HAsset::CHUNK_SLNG, &lb, 1);
	w.write(path, type);
}

// ─── Full Editor UI ───────────────────────────────────────────────────────────
void EditorUI::RenderEditor(AppContext& ctx, float dt)
{
#ifdef HE_IMGUI_ENABLED
	// Runs every frame regardless of which tab/panel is active (before any early-out):
	// guarantees the RMB fly-look capture can never stay stuck once the button is released.
	enforceViewportLookCaptureInvariant(ctx.window ? ctx.window->GetNativeWindow() : nullptr);

	// ── Scene-file dialog helpers ──────────────────────────────────────────
	static PendingFileOp s_pendingFileOp = PendingFileOp::OpenProject;

	// ── Unsaved-changes guard state ─────────────────────────────────────────
	// A destructive action requested while the scene is dirty is stashed here and
	// the "Unsaved Changes" modal is raised; the action runs when the user picks
	// Save (after the save completes) or Don't Save. s_guardSaveThenAct bridges the
	// async Save-As dialog: it tells the file-result handler to run s_guardAction
	// once the freshly chosen path has been written.
	static GuardedAction s_guardAction      = GuardedAction::None;
	static std::string   s_guardArg;            // path payload for OpenScenePath
	static bool          s_openUnsavedModal = false;
	static bool          s_guardSaveThenAct = false;

	// One-shot request to programmatically select a top-level tab (set by a
	// Content-Browser double-click). -1 = none. The tab bar applies SetSelected for
	// exactly one frame and then clears it, so it never fights the user's own tab
	// clicks — applying SetSelected every frame would (and did) do both.
	static int           s_tabSelectRequest = -1;

	auto sceneDialogDir = [&]() -> std::string
	{
		if (!ctx.projectManager) return {};
		std::filesystem::path p = ctx.projectManager->currentProject().path;
		if (std::filesystem::is_regular_file(p)) p = p.parent_path();
		const std::filesystem::path content = p / "Content";
		return std::filesystem::exists(content) ? content.string() : p.string();
	};
	auto fileDialogCb = [](void* userdata, const char* const* filelist, int)
	{
		auto* b = static_cast<SDLDialogBridge*>(userdata);
		if (filelist && filelist[0]) { *b->pendingFileResult = filelist[0]; *b->pendingFileReady = true; }
	};
	auto triggerOpenScene = [&]()
	{
		s_pendingFileOp = PendingFileOp::OpenScene;
		SDL_DialogFileFilter filters[] = { { "Horizon Scene", "hescene" } };
		const std::string dir = sceneDialogDir();
		SDL_ShowOpenFileDialog(fileDialogCb, ctx.dialogBridge,
			ctx.window ? ctx.window->GetNativeWindow() : nullptr,
			filters, 1, dir.empty() ? nullptr : dir.c_str(), false);
	};
	auto triggerAddSceneAdditive = [&]()
	{
		s_pendingFileOp = PendingFileOp::AddSceneAdditive;
		SDL_DialogFileFilter filters[] = { { "Horizon Scene", "hescene" } };
		const std::string dir = sceneDialogDir();
		SDL_ShowOpenFileDialog(fileDialogCb, ctx.dialogBridge,
			ctx.window ? ctx.window->GetNativeWindow() : nullptr,
			filters, 1, dir.empty() ? nullptr : dir.c_str(), false);
	};
	auto triggerSaveSceneAs = [&]()
	{
		s_guardSaveThenAct = false; // a manual Save-As is not part of a guard flow
		s_pendingFileOp = PendingFileOp::SaveScene;
		SDL_DialogFileFilter filters[] = { { "Horizon Scene", "hescene" } };
		const std::string dir = sceneDialogDir();
		SDL_ShowSaveFileDialog(fileDialogCb, ctx.dialogBridge,
			ctx.window ? ctx.window->GetNativeWindow() : nullptr,
			filters, 1, dir.empty() ? nullptr : dir.c_str());
	};
	auto doSaveScene = [&]()
	{
		if (ctx.currentScenePath.empty()) triggerSaveSceneAs();
		else if (ctx.saveSceneToPath)     ctx.saveSceneToPath(ctx.currentScenePath);
	};
	auto triggerOpenProject = [&]()
	{
		ctx.hubOpenError.clear();
		s_pendingFileOp = PendingFileOp::OpenProject;
		SDL_DialogFileFilter filters[] = { { "HorizonEngine Project", "heproj" } };
		SDL_ShowOpenFileDialog(fileDialogCb, ctx.dialogBridge,
			ctx.window ? ctx.window->GetNativeWindow() : nullptr,
			filters, 1, nullptr, false);
	};
	auto triggerImportAsset = [&]()
	{
		if (!ctx.projectLoaded || !ctx.contentManager) return;
		s_pendingFileOp = PendingFileOp::ImportAsset;
		SDL_DialogFileFilter filters[] = {
			{ "All Supported Assets", "gltf;glb;png;jpg;jpeg;tga;bmp;hdr;wav;hmat" },
			{ "3D Models",            "gltf;glb" },
			{ "Textures",             "png;jpg;jpeg;tga;bmp;hdr" },
			{ "Audio",                "wav" },
			{ "Materials",            "hmat" },
		};
		const std::string root = ctx.contentManager->contentRoot();
		SDL_ShowOpenFileDialog(fileDialogCb, ctx.dialogBridge,
			ctx.window ? ctx.window->GetNativeWindow() : nullptr,
			filters, 5,
			root.empty() ? nullptr : root.c_str(),
			false);
	};
	auto doCloseProject = [&]()
	{
		ctx.projectManager->closeProject();
		ctx.globalState->setLastProjectPath("");
		ctx.globalState->writeConfig();
		ctx.projectLoaded = false;
	};

	// ── Unsaved-changes guard ───────────────────────────────────────────────
	// runGuardedAction performs a stashed destructive action; requestGuarded gates
	// it behind the save-prompt when the scene is dirty (else runs it immediately).
	auto runGuardedAction = [&](GuardedAction a, const std::string& arg)
	{
		switch (a)
		{
		case GuardedAction::NewScene:          if (ctx.newScene) ctx.newScene();   break;
		case GuardedAction::OpenSceneDialog:   triggerOpenScene();                 break;
		case GuardedAction::OpenScenePath:     if (ctx.openScene) ctx.openScene(arg); break;
		case GuardedAction::OpenProjectDialog: triggerOpenProject();               break;
		case GuardedAction::CloseProject:      doCloseProject();                   break;
		case GuardedAction::Quit:              if (ctx.quit) ctx.quit();           break;
		case GuardedAction::None:                                                  break;
		}
	};
	auto requestGuarded = [&](GuardedAction a, const std::string& arg = std::string{})
	{
		if (!ctx.sceneDirty) { runGuardedAction(a, arg); return; }
		s_guardAction      = a;
		s_guardArg         = arg;
		s_guardSaveThenAct = false;
		s_openUnsavedModal = true;
	};

	// An OS-level close (window X / Cmd+Q) that EditorApplication vetoed because of
	// unsaved changes surfaces here as a guarded Quit request.
	if (ctx.exitRequested)
	{
		ctx.exitRequested = false;
		requestGuarded(GuardedAction::Quit);
	}

	// ── Menu actions shared by the ImGui menu bar and the macOS native menu ────
	// Open (or focus) the Level Script / Game Instance as editor tabs.
	auto openVirtualTab = [&](const char* label, const char* path)
	{
		auto it = std::find_if(ctx.tabs.begin(), ctx.tabs.end(),
			[&](const AppContext::EditorTab& t){ return t.assetPath == path; });
		if (it == ctx.tabs.end())
		{ ctx.tabs.push_back({ label, path, true, true }); ctx.activeTab = (int)ctx.tabs.size() - 1; }
		else ctx.activeTab = (int)std::distance(ctx.tabs.begin(), it);
		s_tabSelectRequest = ctx.activeTab;
	};
	auto openExportDialog = [&]()
	{
		if (!ctx.projectManager) return;
		auto& proj = ctx.projectManager->currentProject();
		const std::filesystem::path projectRoot =
			std::filesystem::path(proj.path).parent_path();

		// Select the last-used profile and mirror it into the dialog fields.
		s_exportProfileIdx = 0;
		for (int i = 0; i < static_cast<int>(proj.exportProfiles.size()); ++i)
			if (proj.exportProfiles[i].name == proj.activeExportProfile)
			{ s_exportProfileIdx = i; break; }
		if (!proj.exportProfiles.empty())
			exportProfileToDialog(proj.exportProfiles[s_exportProfileIdx], projectRoot);

		// Offer every .hescene in the project as a startup-scene choice.
		// Manual increment(ec): the range-for's operator++ throws on
		// unreadable subdirectories.
		s_exportSceneChoices.clear();
		std::error_code ec;
		std::filesystem::recursive_directory_iterator it(
			projectRoot, std::filesystem::directory_options::skip_permission_denied, ec);
		const std::filesystem::recursive_directory_iterator end;
		while (!ec && it != end)
		{
			const bool regular = it->is_regular_file(ec);
			if (!ec && regular && it->path().extension() == ".hescene")
				s_exportSceneChoices.push_back(
					it->path().lexically_relative(projectRoot).generic_string());
			ec.clear();
			it.increment(ec);
		}

		s_exportResult.clear();
		s_exportBundleKey.clear(); // re-stat the runtime bundle on open
		s_showExportModal = true;
	};
	auto beginNewProject = [&]()
	{
		ctx.hubProjectName[0] = '\0';
		ctx.hubProjectDir[0]  = '\0';
		ctx.hubSelectedPreset = 0;
		ctx.hubCreateError.clear();
	};

	// On macOS the menu lives in the system menu bar (next to the Apple symbol)
	// like any Mac app, and the in-window ImGui menu row is dropped entirely.
	bool nativeMenu = false;
	bool openNewProjectPopup = false;
#ifdef __APPLE__
	MacMenuBar::install();   // idempotent; needs NSApp, which SDL created long ago
	nativeMenu = MacMenuBar::available();
	if (nativeMenu)
	{
		MacMenuBar::setProjectLoaded(ctx.projectLoaded);
		using MC = MacMenuBar::Cmd;
		for (MC c; (c = MacMenuBar::take()) != MC::None; )
		{
			switch (c)
			{
			case MC::NewProject:      beginNewProject(); openNewProjectPopup = true;         break;
			case MC::OpenProject:     requestGuarded(GuardedAction::OpenProjectDialog);      break;
			case MC::CloseProject:    requestGuarded(GuardedAction::CloseProject);           break;
			case MC::NewScene:        requestGuarded(GuardedAction::NewScene);               break;
			case MC::OpenScene:       requestGuarded(GuardedAction::OpenSceneDialog);        break;
			case MC::AddSceneAdditive:triggerAddSceneAdditive();                             break;
			case MC::SaveScene:       doSaveScene();                                         break;
			case MC::SaveSceneAs:     triggerSaveSceneAs();                                  break;
			case MC::Quit:            requestGuarded(GuardedAction::Quit);                   break;
			case MC::Preferences:     s_showPreferences = true;                              break;
			case MC::ResetLayout:     s_resetLayoutRequested = true;                         break;
			case MC::ToggleProfiler:  s_showProfiler = !s_showProfiler;                      break;
			case MC::OpenLevelScript:
				if (ctx.projectLoaded) openVirtualTab("Level Script", LevelScriptPanel::kTabPath);
				break;
			case MC::OpenGameInstance:
				if (ctx.projectLoaded) openVirtualTab("Game Instance", GameInstancePanel::kTabPath);
				break;
			case MC::ImportAsset:     triggerImportAsset();                                  break;
			case MC::ExportProject:   if (ctx.projectLoaded) openExportDialog();             break;
			default: break;
			}
		}
	}
#endif

	if (!nativeMenu)
	{
	ImGui::PushFont(ctx.fontSubheading);
	ImGui::BeginMainMenuBar();
	if (ImGui::BeginMenu("File"))
	{
		if (ImGui::MenuItem("New Project", "Ctrl+N"))
		{
			beginNewProject();
			openNewProjectPopup = true;
		}
        if (ImGui::MenuItem("Open Project", "Ctrl+O"))
            requestGuarded(GuardedAction::OpenProjectDialog);
		if (ImGui::MenuItem("Close Project", "Ctrl+W"))
			requestGuarded(GuardedAction::CloseProject);
        ImGui::Separator();
        if (ImGui::MenuItem("New Scene"))            requestGuarded(GuardedAction::NewScene);
        if (ImGui::MenuItem("Open Scene..."))        requestGuarded(GuardedAction::OpenSceneDialog);
        if (ImGui::MenuItem("Add Scene Additive...")) triggerAddSceneAdditive();
        if (ImGui::MenuItem("Save Scene", "Ctrl+S")) doSaveScene();
        if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S")) triggerSaveSceneAs();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4"))
            requestGuarded(GuardedAction::Quit);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit"))
    {
        if (ImGui::MenuItem("Undo", "Ctrl+Z")) {}
        if (ImGui::MenuItem("Redo", "Ctrl+Y")) {}
        ImGui::Separator();
        if (ImGui::MenuItem("Cut",   "Ctrl+X")) {}
        if (ImGui::MenuItem("Copy",  "Ctrl+C")) {}
        if (ImGui::MenuItem("Paste", "Ctrl+V")) {}
        ImGui::Separator();
		if (ImGui::MenuItem("Preferences", "Ctrl+,")) s_showPreferences = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View"))
    {
        if (ImGui::MenuItem("Toggle Fullscreen", "F11")) {}
        if (ImGui::MenuItem("Reset Layout")) { s_resetLayoutRequested = true; }
        if (ImGui::MenuItem("Performance Profiler", nullptr, s_showProfiler)) s_showProfiler = !s_showProfiler;
        if (ImGui::MenuItem("Level Script", nullptr, false, ctx.projectLoaded))
            openVirtualTab("Level Script", LevelScriptPanel::kTabPath);
        if (ImGui::MenuItem("Game Instance", nullptr, false, ctx.projectLoaded))
            openVirtualTab("Game Instance", GameInstancePanel::kTabPath);
        ImGui::EndMenu();
    }
	if (ImGui::BeginMenu("Assets"))
	{
		if (ImGui::MenuItem("Import Asset...", nullptr, false, ctx.projectLoaded))
			triggerImportAsset();
		if (ImGui::MenuItem("Refresh Assets")) {}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Build", ctx.projectLoaded))
	{
		if (ImGui::MenuItem("Export Project..."))
			openExportDialog();
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Help"))
	{
		if (ImGui::MenuItem("Documentation")) {}
		if (ImGui::MenuItem("About")) {}
		ImGui::EndMenu();
	}
    ImGui::EndMainMenuBar();
    ImGui::PopFont();
	}

    if (openNewProjectPopup)
        ImGui::OpenPopup("##NewProjectPopup");

    // ── Export Project modal ────────────────────────────────────────────────
    if (s_showExportModal)
    {
        ImGui::OpenPopup("Export Project##build");
        s_showExportModal = false;
    }
    {
        // Reap a finished export worker + fetch its result. This runs outside the
        // modal so a completed export is joined even if the popup was closed.
        if (!s_exportRunning.load() && s_exportThread.joinable())
        {
            s_exportThread.join();
            std::lock_guard<std::mutex> lk(s_exportMutex);
            if (!s_exportResultShared.empty())
                s_exportResult = s_exportResultShared;
        }

        // The modal is the UI lock while the worker packs — but ImGui force-closes
        // it when another same-level popup opens (e.g. the Unsaved-Changes modal
        // from a vetoed OS quit). If that popup is dismissed, re-open the export
        // modal so the editor cannot mutate Content/ under the worker's reads.
        if (s_exportRunning.load()
            && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            ImGui::OpenPopup("Export Project##build");

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Always);
        if (ImGui::BeginPopupModal("Export Project##build", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        {
            ProjectManager* pm = ctx.projectManager;
            const std::filesystem::path projectRoot = pm
                ? std::filesystem::path(pm->currentProject().path).parent_path()
                : std::filesystem::path{};
            const bool running = s_exportRunning.load();

            if (running) ImGui::BeginDisabled();

            // ── Profile row: dropdown + Save Profile + Save As <name> ─────────
            if (pm && !pm->currentProject().exportProfiles.empty())
            {
                auto& proj = pm->currentProject();
                if (s_exportProfileIdx >= static_cast<int>(proj.exportProfiles.size()))
                    s_exportProfileIdx = 0;

                ImGui::Text("Profile:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(180.0f);
                if (ImGui::BeginCombo("##exportProfile",
                        proj.exportProfiles[s_exportProfileIdx].name.c_str()))
                {
                    for (int i = 0; i < static_cast<int>(proj.exportProfiles.size()); ++i)
                    {
                        const bool sel = (i == s_exportProfileIdx);
                        if (ImGui::Selectable(proj.exportProfiles[i].name.c_str(), sel) && !sel)
                        {
                            s_exportProfileIdx = i;
                            exportProfileToDialog(proj.exportProfiles[i], projectRoot);
                            proj.activeExportProfile = proj.exportProfiles[i].name;
                            pm->saveProject(proj.path);
                            s_exportResult.clear();
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("Save Profile"))
                {
                    exportDialogToProfile(proj.exportProfiles[s_exportProfileIdx]);
                    proj.activeExportProfile = proj.exportProfiles[s_exportProfileIdx].name;
                    pm->saveProject(proj.path);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Store the fields below into the selected profile\n(persisted in the .heproj manifest).");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120.0f);
                ImGui::InputTextWithHint("##newProfileName", "new profile",
                                         &s_exportNewProfileName);
                ImGui::SameLine();
                const bool canAdd = !s_exportNewProfileName.empty();
                if (!canAdd) ImGui::BeginDisabled();
                if (ImGui::Button("Save As"))
                {
                    // Same name = overwrite that profile, otherwise append a new one.
                    int idx = -1;
                    for (int i = 0; i < static_cast<int>(proj.exportProfiles.size()); ++i)
                        if (proj.exportProfiles[i].name == s_exportNewProfileName) { idx = i; break; }
                    if (idx < 0)
                    {
                        ExportProfile np;
                        np.name = s_exportNewProfileName;
                        proj.exportProfiles.push_back(std::move(np));
                        idx = static_cast<int>(proj.exportProfiles.size()) - 1;
                    }
                    exportDialogToProfile(proj.exportProfiles[idx]);
                    s_exportProfileIdx       = idx;
                    proj.activeExportProfile = proj.exportProfiles[idx].name;
                    pm->saveProject(proj.path);
                    s_exportNewProfileName.clear();
                }
                if (!canAdd) ImGui::EndDisabled();
                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            }

            // ── Editable fields (mirror of the selected profile) ──────────────
            ImGui::Text("Output Directory:");
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputText("##exportDir", &s_exportOutputDir);

            ImGui::Text("Startup Scene:");
            ImGui::SetNextItemWidth(-1.0f);
            const char* scenePreview = s_exportStartupScene.empty()
                ? "(currently open scene)" : s_exportStartupScene.c_str();
            if (ImGui::BeginCombo("##exportScene", scenePreview))
            {
                if (ImGui::Selectable("(currently open scene)", s_exportStartupScene.empty()))
                    s_exportStartupScene.clear();
                for (const auto& sc : s_exportSceneChoices)
                    if (ImGui::Selectable(sc.c_str(), sc == s_exportStartupScene))
                        s_exportStartupScene = sc;
                ImGui::EndCombo();
            }

            ImGui::Text("Target Platform:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::BeginCombo("##exportPlatform", s_exportPlatform.c_str()))
            {
                for (const char* name : { "Host", "Windows", "macOS", "Linux" })
                    if (ImGui::Selectable(name, s_exportPlatform == name))
                        s_exportPlatform = name;
                ImGui::EndCombo();
            }
            {
                // Live feedback: show the runtime bundle this export would ship,
                // or a warning when none is found (data-only exports can't run).
                const std::filesystem::path base =
                    SDL_GetBasePath() ? std::filesystem::path(SDL_GetBasePath())
                                      : std::filesystem::path{};
                const ExportPlatform plat = exportPlatformFromName(s_exportPlatform);
                if (s_exportBundleKey != s_exportPlatform)
                {
                    s_exportBundleKey   = s_exportPlatform;
                    s_exportBundleCache = findRuntimeBundle(base, plat);
                }
                const auto& bundle = s_exportBundleCache;
                if (!bundle.empty())
                    ImGui::TextDisabled("Game runtime: %s",
                                        bundle.lexically_normal().string().c_str());
                else
                    ImGui::TextColored(ImVec4(1.f, 0.55f, 0.2f, 1.f),
                        plat == ExportPlatform::Host
                            ? "No game runtime found — build the HorizonGame target first."
                            : "No %s runtime bundle — place one at %s.",
                        s_exportPlatform.c_str(),
                        resolveRuntimeDir(base, plat).lexically_normal().string().c_str());
                if (plat != ExportPlatform::Host)
                    ImGui::TextDisabled("Output goes to a %s/ sub-folder.", s_exportPlatform.c_str());
            }

            ImGui::Spacing();
            ImGui::Checkbox("Compress assets",       &s_exportCompress);
            ImGui::Checkbox("Encrypt assets",        &s_exportEncrypt);
            if (s_exportEncrypt)
                ImGui::TextDisabled("Note: encryption key management is the project's responsibility.");
            ImGui::Checkbox("Enable mod support",    &s_exportModSupport);
            if (s_exportModSupport)
                ImGui::TextDisabled("The game mounts every .hpak in a Mods/ folder next to the executable.");
            ImGui::Checkbox("Incremental packing",   &s_exportIncremental);
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Reuse unchanged assets from the previous export at the same output\n"
                                  "directory instead of re-compressing them (via a .manifest sidecar).\n"
                                  "Falls back to a full pack automatically when settings changed.");

            ImGui::Checkbox("Compile HorizonCode",   &s_exportCompileHC);
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Translate HorizonCode graphs to native C++ in the packaged build.\n"
                                  "NOT IMPLEMENTED YET — with this off (or until codegen ships) the\n"
                                  "game bundles the HorizonCode interpreter and runs the graph assets\n"
                                  "interpreted, exactly like in the editor.");
            if (s_exportCompileHC)
                ImGui::TextDisabled("Codegen is not implemented yet; this export still ships the interpreter.");

            if (exportAppBundleApplicable(s_exportPlatform))
            {
                ImGui::Checkbox("macOS .app bundle", &s_exportAppBundle);
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Emit a signed <project>.app instead of a flat folder:\n"
                                      "executable + libraries in Contents/MacOS, pak + config in\n"
                                      "Contents/Resources, generated Info.plist, ad-hoc codesigned.");
            }

            ImGui::Spacing();
            ImGui::Text("Exclude Patterns:");
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("One glob per line, matched against Content-relative paths.\n"
                                  "*  matches any characters (including /)\n"
                                  "?  matches exactly one character\n"
                                  "Examples: Debug/*   *_test.hasset   Scenes/Playground*");
            ImGui::InputTextMultiline("##exportExcludes", &s_exportExcludes,
                                      ImVec2(-1.0f, ImGui::GetTextLineHeight() * 3.5f));

            // ── Precompiled material shaders ──────────────────────────────────
            ImGui::Spacing();
            ImGui::Text("Precompiled Material Shaders:");
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Cross-compile every node-graph material into the pak for the\n"
                                  "selected backends, so the shipped game never cross-compiles at\n"
                                  "load. Ship only the backend(s) the target actually runs.\n"
                                  "None selected \xe2\x86\x92 the game compiles shaders on first use.");
            {
                struct Bk { const char* label; HE::RendererBackend rb; };
                static const Bk kBackends[] = {
                    { "Metal",  HE::RendererBackend::Metal  },
                    { "OpenGL", HE::RendererBackend::OpenGL },
                    { "Vulkan", HE::RendererBackend::Vulkan },
                    { "D3D11",  HE::RendererBackend::D3D11  },
                    { "D3D12",  HE::RendererBackend::D3D12  },
                };
                int col = 0;
                for (const Bk& b : kBackends)
                {
                    const uint32_t bit = 1u << static_cast<uint32_t>(b.rb);
                    bool on = (s_exportShaderBackends & bit) != 0;
                    if (ImGui::Checkbox(b.label, &on))
                    {
                        if (on) s_exportShaderBackends |=  bit;
                        else    s_exportShaderBackends &= ~bit;
                    }
                    if (++col < 3) ImGui::SameLine();
                    else           col = 0;
                }
            }

            if (running) ImGui::EndDisabled();

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            // ── Progress (while running) / result (after) ─────────────────────
            if (running)
            {
                const int done  = s_exportDone.load();
                const int total = s_exportTotal.load();
                std::string current;
                {
                    std::lock_guard<std::mutex> lk(s_exportMutex);
                    current = s_exportCurrentFile;
                }
                char overlay[64];
                std::snprintf(overlay, sizeof(overlay), "%d / %d", done, total);
                ImGui::ProgressBar(total > 0 ? static_cast<float>(done) / static_cast<float>(total)
                                             : 0.0f,
                                   ImVec2(-1.0f, 0.0f), overlay);
                ImGui::TextDisabled("%s", current.empty() ? "Packing..." : current.c_str());
                ImGui::Spacing();
            }
            else if (!s_exportResult.empty())
            {
                const bool ok = s_exportResult.rfind("OK:", 0) == 0;
                if (ok) ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.f), "%s", s_exportResult.c_str());
                else    ImGui::TextColored(ImVec4(1.f,  0.3f, 0.3f, 1.f), "%s", s_exportResult.c_str());
                ImGui::Spacing();
            }

            const bool canExport = !s_exportOutputDir.empty()
                                && ctx.contentManager && !running;
            if (!canExport) ImGui::BeginDisabled();
            if (ImGui::Button("Export", ImVec2(110, 0)))
            {
                // HorizonCode→C++ codegen is a planned feature (docs/horizoncode-
                // cpp-codegen-plan.md); the profile toggle is honored loudly, not
                // silently: the export proceeds interpreter-based either way.
                if (s_exportCompileHC)
                    Logger::Log(Logger::LogLevel::Warning,
                        "Export: 'Compile HorizonCode' is enabled but codegen is not "
                        "implemented yet — shipping the interpreter + HorizonCode assets.");

                // Resolve the startup scene: the profile's project-relative choice,
                // or the scene currently open in the editor.
                std::string scenePath = ctx.currentScenePath;
                if (!s_exportStartupScene.empty() && !projectRoot.empty())
                    scenePath = (projectRoot / s_exportStartupScene).string();
                std::string sceneName;
                if (!scenePath.empty())
                    sceneName = std::filesystem::path(scenePath).filename().string();

                // Serialize the SAVED startup scene to binary (CBOR) on the main
                // thread — loaded fresh from disk rather than the live editor
                // world, which may hold unsaved or play-mode mutations. A scene
                // that no longer loads (deleted/renamed since the profile was
                // saved) must FAIL the export: shipping a project.hcfg that names
                // a scene that is neither packed nor copied gives a game that
                // cannot boot, reported as success.
                std::vector<uint8_t> sceneBinary;
                bool sceneOk = true;
                if (!scenePath.empty())
                {
                    HorizonWorld sceneWorld;
                    SceneSerializer ser;
                    if (ser.load(sceneWorld, scenePath, SerializeFormat::JSON))
                        ser.saveToMemory(sceneWorld, sceneBinary);
                    else
                        sceneOk = false;
                }

                // Serialize EVERY project scene (incl. the startup one, so the
                // game can transition back to it): packed under path-derived
                // UUIDs + listed in the packed scene index, they make
                // scene.load("<path>") and scene.available() work in the shipped
                // game. A scene that fails to load is skipped with a note rather
                // than failing the whole export — only the STARTUP scene is
                // boot-critical.
                std::vector<std::pair<std::string, std::vector<uint8_t>>> extraScenes;
                {
                    const std::filesystem::path projectRoot2 =
                        std::filesystem::path(ctx.projectManager->currentProject().path).parent_path();
                    std::error_code ec2;
                    std::filesystem::recursive_directory_iterator sit(
                        projectRoot2, std::filesystem::directory_options::skip_permission_denied, ec2);
                    const std::filesystem::recursive_directory_iterator send;
                    while (!ec2 && sit != send)
                    {
                        const bool regular = sit->is_regular_file(ec2);
                        if (!ec2 && regular && sit->path().extension() == ".hescene")
                        {
                            HorizonWorld w2;
                            SceneSerializer ser2;
                            std::vector<uint8_t> bytes;
                            if (ser2.load(w2, sit->path(), SerializeFormat::JSON) &&
                                ser2.saveToMemory(w2, bytes))
                                extraScenes.emplace_back(
                                    sit->path().lexically_relative(projectRoot2).generic_string(),
                                    std::move(bytes));
                            else
                                Logger::Log(Logger::LogLevel::Warning,
                                    ("Export: skipping unreadable scene " + sit->path().string()).c_str());
                        }
                        ec2.clear();
                        sit.increment(ec2);
                    }
                }

                // Resolve the target platform: a COMPLETE runtime bundle (found
                // via findRuntimeBundle, which also handles running the editor
                // from a build tree) + per-platform output sub-folder. An export
                // without a game executable is just data — fail up front.
                const ExportPlatform platform = exportPlatformFromName(s_exportPlatform);
                std::filesystem::path effOutDir = s_exportOutputDir;
                if (platform != ExportPlatform::Host)
                    effOutDir /= exportPlatformName(platform);
                const std::filesystem::path base =
                    SDL_GetBasePath() ? std::filesystem::path(SDL_GetBasePath())
                                      : std::filesystem::path{};
                const std::filesystem::path runtimeDir = findRuntimeBundle(base, platform);

                if (!sceneOk)
                {
                    s_exportResult = "Error: startup scene could not be loaded: " + scenePath;
                }
                else if (runtimeDir.empty())
                {
                    s_exportResult = platform == ExportPlatform::Host
                        ? std::string("Error: no game runtime found — build the HorizonGame "
                                      "target, then export again.")
                        : "Error: no " + s_exportPlatform + " runtime bundle at "
                          + resolveRuntimeDir(base, platform).lexically_normal().string()
                          + " — build the game runtime on " + s_exportPlatform
                          + " and place it there.";
                }
                else
                {
                ExportSettings es;
                es.compress         = s_exportCompress;
                es.encrypt          = s_exportEncrypt;
                es.enableModSupport = s_exportModSupport;
                es.excludePatterns  = parseExcludeLines(s_exportExcludes.c_str());
                es.incremental      = s_exportIncremental;
                es.appBundle        = s_exportAppBundle && exportAppBundleApplicable(s_exportPlatform);
                // ASTC textures for Apple Metal targets (Host/macOS on a Mac) —
                // shares the .app-bundle predicate incidentally (both are "Apple
                // target"), not by definition. Caveat: on an Intel-Mac *target*
                // the GPU can't sample ASTC, so those meshes fall back to white
                // (see MetalRenderer astcOk guard). Fine for Apple-Silicon; a
                // dedicated toggle can split this if Intel distribution matters.
                es.astcTextures     = exportAppBundleApplicable(s_exportPlatform);
                es.gameRuntimeDir   = runtimeDir;
                // Precompile node-graph material shaders into the pak for the
                // selected backends (0 → runtime cross-compiles as before).
                es.shaderBackends        = s_exportShaderBackends;
                es.compileShaderVariants = &CompileMaterialShaderVariants;
                // Worker → UI progress: atomics + a mutex-guarded filename.
                es.progress = [](int done, int total, const std::string& current)
                {
                    s_exportDone.store(done);
                    s_exportTotal.store(total);
                    std::lock_guard<std::mutex> lk(s_exportMutex);
                    s_exportCurrentFile = current;
                };

                const std::string projName = pm ? pm->currentProject().name : "Game";
                const std::string contentDir = ctx.contentManager->contentRoot();
                const std::string outDir     = effOutDir.string();

                s_exportDone.store(0);
                s_exportTotal.store(0);
                {
                    std::lock_guard<std::mutex> lk(s_exportMutex);
                    s_exportCurrentFile.clear();
                    s_exportResultShared.clear();
                }
                s_exportResult.clear();
                s_exportRunning.store(true);
                if (s_exportThread.joinable()) s_exportThread.join(); // defensive; reaped above
                s_exportThread = std::thread([es, contentDir, projName, sceneName,
                                              outDir, sceneBinary, extraScenes]()
                {
                    // An exception escaping a std::thread is std::terminate — and
                    // exportProject touches the filesystem (unreadable dirs,
                    // disk-full) and allocates compression buffers (bad_alloc).
                    std::string msg;
                    try
                    {
                        const auto res = ProjectExporter::exportProject(
                            contentDir, projName, sceneName,
                            std::filesystem::path(outDir), es, sceneBinary, extraScenes);
                        msg = res.success
                            ? "OK: " + std::to_string(res.assetsPacked)
                              + " asset(s) packed ("
                              + std::to_string(res.assetsReused) + " reused), "
                              + std::to_string(res.binaryFilesCopied)
                              + " binary file(s) → " + outDir
                            : "Error: " + res.errorMessage;
                        if (res.success && es.appBundle)
                            msg += " — " + projName + ".app bundle";
                        if (res.success && es.encrypt)
                            msg += res.keyEmbedded
                                ? " — key embedded in the game binary"
                                : " — key in project.hcfg (runtime has no key block)";
                    }
                    catch (const std::exception& e)
                    {
                        msg = std::string("Error: export failed: ") + e.what();
                    }
                    catch (...)
                    {
                        msg = "Error: export failed with an unknown error";
                    }
                    {
                        std::lock_guard<std::mutex> lk(s_exportMutex);
                        s_exportResultShared = std::move(msg);
                    }
                    s_exportRunning.store(false); // last: UI may join right after
                });
                }
            }
            if (!canExport) ImGui::EndDisabled();
            ImGui::SameLine();
            if (running) ImGui::BeginDisabled();
            if (ImGui::Button("Close", ImVec2(80, 0)))
                ImGui::CloseCurrentPopup();
            if (running) ImGui::EndDisabled();
            ImGui::EndPopup();
        }
    }

    // ── Unsaved-changes modal ───────────────────────────────────────────────
    // Raised by requestGuarded() when a scene-discarding action is attempted with
    // a dirty scene. Save → write (Save-As if untitled) then run the action;
    // Don't Save → run it straight away; Cancel → abandon it.
    if (s_openUnsavedModal)
    {
        ImGui::OpenPopup("Unsaved Changes##scene");
        s_openUnsavedModal = false;
    }
    {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Unsaved Changes##scene", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        {
            const std::string sceneName = ctx.currentScenePath.empty()
                ? std::string("Untitled")
                : std::filesystem::path(ctx.currentScenePath).stem().string();
            ImGui::Text("Save changes to \"%s\" before continuing?", sceneName.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("Your unsaved changes will be lost otherwise.");
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

            // Snapshot the stashed action — the buttons may clear it.
            const GuardedAction action = s_guardAction;
            const std::string   arg    = s_guardArg;

            if (ImGui::Button("Save", ImVec2(110, 0)))
            {
                const bool hadPath = !ctx.currentScenePath.empty();
                doSaveScene(); // synchronous if a path exists, else async Save-As
                if (hadPath)
                {
                    runGuardedAction(action, arg);
                    s_guardAction = GuardedAction::None;
                }
                else
                {
                    // Save-As dialog is in flight; the file-result handler runs
                    // the action once a path has been chosen and written.
                    s_guardSaveThenAct = true;
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Don't Save", ImVec2(110, 0)))
            {
                runGuardedAction(action, arg);
                s_guardAction = GuardedAction::None;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(110, 0)) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                s_guardAction      = GuardedAction::None;
                s_guardSaveThenAct = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    if (ctx.pendingFileReady)
    {
        ctx.pendingFileReady = false;
        std::string chosen = ctx.pendingFileResult;
        ctx.pendingFileResult.clear();

        if (s_pendingFileOp == PendingFileOp::OpenScene)
        {
            if (!chosen.empty() && ctx.openScene) ctx.openScene(chosen);
        }
        else if (s_pendingFileOp == PendingFileOp::AddSceneAdditive)
        {
            if (!chosen.empty() && ctx.openSceneAdditive) ctx.openSceneAdditive(chosen);
        }
        else if (s_pendingFileOp == PendingFileOp::SaveScene)
        {
            if (!chosen.empty() && ctx.saveSceneToPath)
            {
                std::filesystem::path p(chosen);
                if (p.extension() != ".hescene") p += ".hescene";
                ctx.saveSceneToPath(p.string());
                // If this Save-As was the guard's "Save" choice, run the deferred
                // action now that the scene is on disk.
                if (s_guardSaveThenAct)
                {
                    runGuardedAction(s_guardAction, s_guardArg);
                    s_guardAction = GuardedAction::None;
                }
            }
            s_guardSaveThenAct = false;
        }
        else if (s_pendingFileOp == PendingFileOp::ImportAsset)
        {
            if (!chosen.empty() && ctx.contentManager)
            {
                const std::filesystem::path srcPath(chosen);
                std::string ext = srcPath.extension().string();
                for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));

                const bool isMeshSrc    = (ext == ".gltf" || ext == ".glb");
                const bool isTextureSrc = (ext == ".png"  || ext == ".jpg" || ext == ".jpeg" ||
                                           ext == ".tga"  || ext == ".bmp" || ext == ".hdr");
                const bool isAudioSrc   = (ext == ".wav");
                const bool isMatSrc     = (ext == ".hmat");
                const bool isFontSrc    = (ext == ".ttf" || ext == ".otf");

                const std::filesystem::path root(ctx.contentManager->contentRoot());
                bool ok = false;
                if      (isMeshSrc)    ok = MeshImporter::import(srcPath, root)     != nullptr;
                else if (isTextureSrc) ok = TextureImporter::import(srcPath, root)  != nullptr;
                else if (isAudioSrc)   ok = AudioImporter::import(srcPath, root)    != nullptr;
                else if (isMatSrc)     ok = MaterialImporter::import(srcPath, root) != nullptr;
                else if (isFontSrc)    ok = FontImporter::import(srcPath, root)     != nullptr;

                if (!ok)
                    Logger::Log(Logger::LogLevel::Error,
                        ("Editor: import failed for " + srcPath.string()).c_str());
                ctx.contentRefreshPending = true;
            }
        }
        else // OpenProject
        {
            if (ctx.projectManager->loadProject(chosen))
            {
                ctx.globalState->addKnownProject(chosen);
                ctx.globalState->writeConfig();
                ctx.contentRefreshPending = true;
                ctx.projectLoaded = true;
            }
            else
            {
                ctx.hubOpenError = "Failed to load project file.";
                ImGui::OpenPopup("##EditorOpenError");
            }
        }
        s_pendingFileOp = PendingFileOp::OpenProject; // reset to default
    }

    // ── Scene shortcuts: Cmd/Ctrl+S save, Shift+Cmd/Ctrl+S save as ─────────
    {
        const ImGuiIO& kio = ImGui::GetIO();
        const bool mod = kio.KeyCtrl || kio.KeySuper;
        if (mod && !kio.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_S, false))
        {
            if (kio.KeyShift) triggerSaveSceneAs();
            else              doSaveScene();
        }
        // Ctrl/Cmd+, opens Preferences (matches the Edit menu shortcut label).
        if (mod && !kio.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Comma, false))
            s_showPreferences = true;
    }

    if (!ctx.hubOpenError.empty())
    {
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("##EditorOpenError", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextUnformatted(ctx.hubOpenError.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (ImGui::Button("OK", ImVec2(120, 0)))
            {
                ctx.hubOpenError.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // ── New Project Popup ─────────────────────────────────────────────────────
    {
        static const std::array<const char*, 4> kPresetNames = {
            "Empty Project", "Game", "Simulation", "Tool",
        };
        static const std::array<const char*, 4> kPresetDesc = {
            "Only the basic folder skeleton, no extra content.",
            "Assets, Scenes and Scripts folders + a sample scene file.",
            "Assets, Scenes and Data folders.",
            "Assets and Source folders.",
        };

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 16.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(8.0f, 8.0f));
        if (ImGui::BeginPopupModal("##NewProjectPopup", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            ImGui::PopStyleVar(2);

            if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
            ImGui::TextUnformatted("New Project");
            if (ctx.fontSubheading) ImGui::PopFont();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Project Name");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##npName", ctx.hubProjectName, ctx.hubProjectNameSize);

            ImGui::Spacing();
            ImGui::Text("Project Directory");
            ImGui::SetNextItemWidth(-70.0f);
            ImGui::InputText("##npDir", ctx.hubProjectDir, ctx.hubProjectDirSize);
            ImGui::SameLine();
            if (ImGui::Button("Browse##npBrowse", ImVec2(62.0f, 0)))
            {
#ifdef _WIN32
                IFileOpenDialog* pDlg = nullptr;
                if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                    CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg))))
                {
                    DWORD dwOpts = 0;
                    pDlg->GetOptions(&dwOpts);
                    pDlg->SetOptions(dwOpts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                    HWND hwnd = nullptr;
                    if (ctx.window)
                        hwnd = static_cast<HWND>(SDL_GetPointerProperty(
                            SDL_GetWindowProperties(ctx.window->GetNativeWindow()),
                            SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
                    if (SUCCEEDED(pDlg->Show(hwnd)))
                    {
                        IShellItem* pItem = nullptr;
                        if (SUCCEEDED(pDlg->GetResult(&pItem)))
                        {
                            PWSTR pPath = nullptr;
                            if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pPath)))
                            {
                                int len = WideCharToMultiByte(CP_UTF8, 0, pPath, -1,
                                    nullptr, 0, nullptr, nullptr);
                                if (len > 0 && len <= ctx.hubProjectDirSize)
                                    WideCharToMultiByte(CP_UTF8, 0, pPath, -1,
                                        ctx.hubProjectDir, ctx.hubProjectDirSize, nullptr, nullptr);
                                CoTaskMemFree(pPath);
                            }
                            pItem->Release();
                        }
                    }
                    pDlg->Release();
                }
#else
                SDL_ShowOpenFolderDialog(
                    [](void* userdata, const char* const* filelist, int)
                    {
                        auto* b = static_cast<SDLDialogBridge*>(userdata);
                        if (filelist && filelist[0])
                        {
                            *b->pendingDirResult = filelist[0];
                            *b->pendingDirReady  = true;
                        }
                    },
                    ctx.dialogBridge,
                    ctx.window ? ctx.window->GetNativeWindow() : nullptr,
                    nullptr, false);
#endif
            }

            if (ctx.pendingDirReady)
            {
                strncpy(ctx.hubProjectDir, ctx.pendingDirResult.c_str(), ctx.hubProjectDirSize - 1);
                ctx.hubProjectDir[ctx.hubProjectDirSize - 1] = '\0';
                ctx.pendingDirReady  = false;
                ctx.pendingDirResult.clear();
            }

            ImGui::Spacing();
            ImGui::Text("Template");
            ImGui::SetNextItemWidth(-1);
            ImGui::ListBox("##npPresets", &ctx.hubSelectedPreset,
                kPresetNames.data(), static_cast<int>(kPresetNames.size()), 4);
            ImGui::TextDisabled("%s", kPresetDesc[ctx.hubSelectedPreset]);

            ImGui::Spacing();
            if (!ctx.hubCreateError.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::TextWrapped("%s", ctx.hubCreateError.c_str());
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }

            ImGui::Separator();
            ImGui::Spacing();
            const float btnW = (480.0f - 16.0f * 2 - 8.0f) * 0.5f;
            if (ImGui::Button("Create", ImVec2(btnW, 0)))
            {
                ctx.hubCreateError.clear();
                std::string name = ctx.hubProjectName;
                std::string dir  = ctx.hubProjectDir;
                if (name.empty())
                    ctx.hubCreateError = "Please enter a project name.";
                else if (dir.empty())
                    ctx.hubCreateError = "Please select a project directory.";
                else
                {
                    std::filesystem::path projRoot = std::filesystem::path(dir) / name;
                    bool ok = ctx.projectManager->createNewProject(
                        projRoot.string(), name,
                        static_cast<ProjectPreset>(ctx.hubSelectedPreset));
                    if (ok)
                    {
                        const std::string& heprojPath = ctx.projectManager->currentProject().path;
                        ctx.globalState->addKnownProject(heprojPath);
                        ctx.globalState->writeConfig();
                        ctx.contentRefreshPending = true;
                        ImGui::CloseCurrentPopup();
                    }
                    else
                    {
                        ctx.hubCreateError = "Failed to create project. Check path/permissions.";
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(btnW, 0)))
            {
                ctx.hubCreateError.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        else
        {
            ImGui::PopStyleVar(2);
        }
    }

    static constexpr float kFooterH  = 24.0f;
    static constexpr float kTabBarH  = 28.0f;

    // ── Footer bar ────────────────────────────────────────────────────────────
    // Must be rendered BEFORE the DockSpace window so ImGui processes it first
    // and docked windows cannot overlap it.
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - kFooterH), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, kFooterH), ImGuiCond_Always);
        ImGui::SetNextWindowViewport(vp->ID);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(8.0f, 4.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.13f, 0.13f, 0.15f, 1.0f));

        ImGui::Begin("##EditorFooter", nullptr,
            ImGuiWindowFlags_NoTitleBar         |
            ImGuiWindowFlags_NoResize           |
            ImGuiWindowFlags_NoMove             |
            ImGuiWindowFlags_NoScrollbar        |
            ImGuiWindowFlags_NoSavedSettings    |
            ImGuiWindowFlags_NoDocking          |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav              |
            ImGuiWindowFlags_NoDecoration);

        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();

		if (ctx.fontBody) ImGui::PushFont(ctx.fontBody);

		// Left — Undo / Redo buttons
		{
			constexpr float btnSize = 16.0f;
			ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1,1,1,0.10f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1,1,1,0.20f));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 0.0f));

			const bool canUndo = ctx.undoSys && ctx.undoSys->canUndo();
			const bool canRedo = ctx.undoSys && ctx.undoSys->canRedo();

			ImGui::BeginDisabled(!canUndo);
			bool doUndo;
			if (ctx.toolbarIcons.undo)
				doUndo = ImGui::ImageButton("##footerUndo", ctx.toolbarIcons.undo, ImVec2(btnSize, btnSize));
			else
				doUndo = ImGui::Button("Undo");
			ImGui::EndDisabled();

			ImGui::SameLine(0.0f, 4.0f);

			ImGui::BeginDisabled(!canRedo);
			bool doRedo;
			if (ctx.toolbarIcons.redo)
				doRedo = ImGui::ImageButton("##footerRedo", ctx.toolbarIcons.redo, ImVec2(btnSize, btnSize));
			else
				doRedo = ImGui::Button("Redo");
			ImGui::EndDisabled();

			// Keyboard shortcuts: Cmd/Ctrl+Z, Shift+Cmd/Ctrl+Z (or Ctrl+Y)
			const ImGuiIO& kio = ImGui::GetIO();
			const bool mod = kio.KeyCtrl || kio.KeySuper;
			if (!kio.WantTextInput && mod)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_Z, false))
					(kio.KeyShift ? doRedo : doUndo) = true;
				if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
					doRedo = true;
			}

			if (doUndo && canUndo && ctx.undo) ctx.undo();
			if (doRedo && canRedo && ctx.redo) ctx.redo();

			ImGui::PopStyleVar();
			ImGui::PopStyleColor(3);
		}

		// Right — render resolution + FPS (drawn before SameLine so GetWindowWidth() is stable).
		// The resolution is the actual viewport framebuffer size the scene renders at.
		std::string fpsText = "FPS: " + std::to_string(static_cast<int>(ctx.smoothFps));
		if (s_viewportPxW > 0 && s_viewportPxH > 0)
			fpsText = std::to_string(s_viewportPxW) + "x" + std::to_string(s_viewportPxH)
			        + "   " + fpsText;
		const float fpsW = ImGui::CalcTextSize(fpsText.c_str()).x;
		ImGui::SameLine(ImGui::GetWindowWidth() - fpsW - ImGui::GetStyle().WindowPadding.x);
		ImGui::Text("%s", fpsText.c_str());

		// Middle — status
		const std::string statusText = "Ready";
		const float       statusW    = ImGui::CalcTextSize(statusText.c_str()).x;
		ImGui::SameLine((ImGui::GetWindowWidth() - statusW) * 0.5f);
		ImGui::TextDisabled("%s", statusText.c_str());

        if (ctx.fontBody) ImGui::PopFont();

        ImGui::End();
    }

    // ── Editor TabBar (below menu bar, above DockSpace) ────────────────────────
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, kTabBarH), ImGuiCond_Always);
        ImGui::SetNextWindowViewport(vp->ID);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(4.0f, 2.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_TabRounding,      2.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg,   ImVec4(0.11f, 0.11f, 0.13f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Tab,        ImVec4(0.16f, 0.16f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.28f, 0.28f, 0.36f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_TabActive,  ImVec4(0.22f, 0.22f, 0.30f, 1.0f));

        ImGui::Begin("##EditorTabBar", nullptr,
            ImGuiWindowFlags_NoTitleBar         |
            ImGuiWindowFlags_NoResize           |
            ImGuiWindowFlags_NoMove             |
            ImGuiWindowFlags_NoScrollbar        |
            ImGuiWindowFlags_NoSavedSettings    |
            ImGuiWindowFlags_NoDocking          |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav              |
            ImGuiWindowFlags_NoDecoration);

        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(4);

        // ── Tab state (owned by AppContext / EditorApplication) ───────────
        auto& s_tabs      = ctx.tabs;
        auto& s_activeTab = ctx.activeTab;

        // Open request from inside an editor panel (e.g. double-clicking a Material
        // Function node) — same find-or-push flow as the Content Browser double-click.
        if (const std::string req = MaterialEditorPanel::takeOpenRequest(); !req.empty())
        {
            auto it = std::find_if(s_tabs.begin(), s_tabs.end(),
                [&](const AppContext::EditorTab& t){ return t.assetPath == req; });
            if (it == s_tabs.end())
            {
                s_tabs.push_back({ std::filesystem::path(req).stem().string(), req, true, true });
                s_activeTab = static_cast<int>(s_tabs.size()) - 1;
            }
            else
                s_activeTab = static_cast<int>(std::distance(s_tabs.begin(), it));
            s_tabSelectRequest = s_activeTab;
        }

        if (ctx.fontBody) ImGui::PushFont(ctx.fontBody);

        if (ImGui::BeginTabBar("##MainTabBar",
            ImGuiTabBarFlags_Reorderable |
            ImGuiTabBarFlags_FittingPolicyScroll |
            ImGuiTabBarFlags_NoCloseWithMiddleMouseButton))
        {
            // Closing a tab drops its cached editor state (text buffers, graphs,
            // preview textures) so per-session cost stays flat no matter how many
            // tabs were opened. Dirty states are kept: reopening the tab restores
            // the unsaved edits instead of silently discarding them.
            auto forgetTabState = [](const AppContext::EditorTab& t){
                if (t.assetPath.empty()) return;
                if (ScriptEditorPanel::isDirty(t.assetPath)   ||
                    MaterialEditorPanel::isDirty(t.assetPath) ||
                    UIEditorPanel::isDirty(t.assetPath)       ||
                    HorizonCodeClassPanel::isDirty(t.assetPath)) return;
                ScriptEditorPanel::forget(t.assetPath);
                MaterialEditorPanel::forget(t.assetPath);
                UIEditorPanel::forget(t.assetPath);
                HorizonCodeClassPanel::forget(t.assetPath);
            };

            for (int i = 0; i < static_cast<int>(s_tabs.size()); )
            {
                auto& tab = s_tabs[i];
                if (!tab.open) { forgetTabState(tab); s_tabs.erase(s_tabs.begin() + i); continue; }

                ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
                // Force-select only on an explicit one-shot request (double-click). Using
                // s_activeTab here every frame is wrong: BeginTabItem mutates s_activeTab
                // mid-loop, so the Scene tab (rendered first) steals it back — and the
                // constant SetSelected also swallows manual tab clicks.
                if (i == s_tabSelectRequest) flags |= ImGuiTabItemFlags_SetSelected;

                bool pOpen = tab.closable ? tab.open : true;
                // Stable ID (### + assetPath) so appending a dirty marker to the visible
                // label never changes the tab's identity — which would reset its state.
                const bool tabDirty = !tab.assetPath.empty() &&
                    (ScriptEditorPanel::isDirty(tab.assetPath) ||
                     MaterialEditorPanel::isDirty(tab.assetPath) ||
                     UIEditorPanel::isDirty(tab.assetPath) ||
                     HorizonCodeClassPanel::isDirty(tab.assetPath));
                const std::string shown = tab.label + (tabDirty ? " *" : "")
                    + "###tab_" + (tab.assetPath.empty() ? std::string("scene") : tab.assetPath);
                if (ImGui::BeginTabItem(shown.c_str(), tab.closable ? &pOpen : nullptr, flags))
                {
                    s_activeTab = i;
                    ImGui::EndTabItem();
                }
                if (tab.closable) tab.open = pOpen;
                ++i;
            }
            // Remove closed tabs (dropping their cached editor state)
            for (const auto& t : s_tabs)
                if (t.closable && !t.open) forgetTabState(t);
            s_tabs.erase(
                std::remove_if(s_tabs.begin(), s_tabs.end(),
                    [](const AppContext::EditorTab& t){ return t.closable && !t.open; }),
                s_tabs.end());
            // Keep the active index valid after a tab closes (else it dangles or points
            // at the wrong tab). Fall back to the Scene tab (index 0) when out of range.
            if (s_activeTab >= static_cast<int>(s_tabs.size()))
                s_activeTab = static_cast<int>(s_tabs.size()) - 1;
            if (s_activeTab < 0) s_activeTab = 0;
            s_tabSelectRequest = -1;   // consume the one-shot select request

            ImGui::EndTabBar();
        }

        if (ctx.fontBody) ImGui::PopFont();

        ImGui::End();
    }

    // ── Top-level tab gating ───────────────────────────────────────────────────
    // The built-in "Scene" tab (empty assetPath) shows the dockspace + all panels
    // below. A script tab instead fills that same area with its code editor and we
    // skip the scene panels for this frame. Default to the scene when the active
    // index is out of range. (All ImGui windows above are already balanced, so the
    // early return is safe; modals/menus render before this point.)
    const bool sceneTabActive =
        ctx.activeTab < 0 || ctx.activeTab >= static_cast<int>(ctx.tabs.size())
        || ctx.tabs[ctx.activeTab].assetPath.empty();
    if (!sceneTabActive)
    {
        // The scene viewport (and its RMB fly-look release) won't run this frame. If the
        // user switched here mid-look via a keyboard shortcut, force-release the capture so
        // the cursor isn't left hidden/pinned with ImGui mouse input disabled.
        releaseViewportLookCapture(ctx.window ? ctx.window->GetNativeWindow() : nullptr);

        const ImGuiViewport* vpTab = ImGui::GetMainViewport();
        const std::string& tabPath = ctx.tabs[ctx.activeTab].assetPath;
        const ImVec2 tabPos(vpTab->WorkPos.x, vpTab->WorkPos.y + kTabBarH);
        const ImVec2 tabSize(vpTab->WorkSize.x, vpTab->WorkSize.y - kFooterH - kTabBarH);
        // Dispatch by asset type: material assets get the node-graph editor, script
        // assets the code editor. (Cheap header sniff; both panels cache their state.)
        // The Level Script + Game Instance are virtual tabs (no backing .hasset).
        if (tabPath == LevelScriptPanel::kTabPath)
            LevelScriptPanel::render(ctx, tabPos, tabSize);
        else if (tabPath == GameInstancePanel::kTabPath)
            GameInstancePanel::render(ctx, tabPos, tabSize);
        else if (MaterialEditorPanel::isMaterialAsset(tabPath) ||
            MaterialEditorPanel::isMaterialFunctionAsset(tabPath))
            MaterialEditorPanel::render(ctx, tabPath, tabPos, tabSize);
        else if (UIEditorPanel::isWidgetAsset(tabPath))
            UIEditorPanel::render(ctx, tabPath, tabPos, tabSize);
        else if (HorizonCodeClassPanel::isClassAsset(tabPath))
            HorizonCodeClassPanel::render(ctx, tabPath, tabPos, tabSize);
        else
            ScriptEditorPanel::render(ctx, tabPath, tabPos, tabSize);
        return;
    }

    // ── DockSpace (shrunk by footer + tabbar height so docked windows never overlap)
    {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + kTabBarH), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - kFooterH - kTabBarH), ImGuiCond_Always);
        ImGui::SetNextWindowViewport(vp->ID);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg,  ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

        ImGui::Begin("##EditorDockSpace", nullptr,
            ImGuiWindowFlags_NoTitleBar         |
            ImGuiWindowFlags_NoResize           |
            ImGuiWindowFlags_NoMove             |
            ImGuiWindowFlags_NoScrollbar        |
            ImGuiWindowFlags_NoSavedSettings    |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoBackground);

        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);

        const ImGuiID dockspaceId = ImGui::GetID("##MainDockSpace");
        // Build the default layout on first run (no saved layout in imgui.ini) or
        // on demand via View > Reset Layout. A layout loaded from imgui.ini
        // otherwise always wins, so user customisations persist.
        if (s_resetLayoutRequested || ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
        {
            BuildDefaultDockLayout(dockspaceId, ImGui::GetContentRegionAvail());
            s_resetLayoutRequested = false;
            // Persist immediately so the layout survives an early exit and becomes
            // the baseline the user then customises.
            if (const char* ini = ImGui::GetIO().IniFilename)
                ImGui::SaveIniSettingsToDisk(ini);
        }

        ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f),
            ImGuiDockNodeFlags_PassthruCentralNode);

        ImGui::End();
    }

	// ── Scene viewport (offscreen render target as dockable window) ─────────
	{
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		// NoMove is essential: the viewport content is a plain ImGui::Image (an
		// id-less item), so a click-drag on it would otherwise be treated as a
		// click on empty window space and start an ImGui window/dock move. That
		// move fights the ImGuizmo drag for the same mouse button, so the gizmo
		// never manipulates the object (translate/rotate/scale all dead). The
		// docked Scene window is still relocated via its tab, so NoMove costs
		// nothing here.
		ImGui::Begin("Scene", nullptr,
		             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
		             ImGuiWindowFlags_NoMove);
		ImGui::PopStyleVar();

		// ── Inline toolbar (always at top of Scene, works docked or floating) ──
		{
			constexpr float kToolbarH = 36.0f;
			ImGui::SetCursorPos(ImVec2(4.0f, 6.0f));

			ImGui::SetNextItemWidth(120.0f);
			if (ImGui::BeginCombo("##ModeSelector", ctx.editorConfig.modeString().c_str()))
			{
				if (ImGui::Selectable("View"))      ctx.editorConfig.mode = EditorMode::View;
				if (ImGui::Selectable("Landscape")) ctx.editorConfig.mode = EditorMode::Landscape;
				ImGui::EndCombo();
			}
			ImGui::SameLine();

			auto toolBtn = [&](const char* label, ImGuizmo::OPERATION op, const char* tip)
			{
				const bool active = (s_gizmoOp == op);
				if (active) ImGui::PushStyleColor(ImGuiCol_Button,
					ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
				if (ImGui::Button(label)) s_gizmoOp = op;
				if (active) ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
			};
			toolBtn("Move",   ImGuizmo::TRANSLATE, "Move (W)");   ImGui::SameLine();
			toolBtn("Rotate", ImGuizmo::ROTATE,    "Rotate (E)"); ImGui::SameLine();
			toolBtn("Scale",  ImGuizmo::SCALE,     "Scale (R)");  ImGui::SameLine();

			{
				int modeIdx = (s_gizmoMode == ImGuizmo::WORLD) ? 1 : 0;
				const char* modeItems[] = { "Local", "World" };
				ImGui::SetNextItemWidth(90.0f);
				if (ImGui::Combo("##GizmoMode", &modeIdx, modeItems, 2))
					s_gizmoMode = (modeIdx == 1) ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Gizmo axis orientation:\nLocal (object axes) or World (axis-aligned)");
			}
			ImGui::SameLine();

			ImGui::Checkbox("Screen ring", &s_rotateScreen);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Show the rotate gizmo's outer screen-space ring\n(rotates about the view axis)");
			ImGui::SameLine();

			// Play / Stop button — centered in the remaining space
			{
				const bool playing = ctx.isPlaying;
				constexpr float btnSize = 20.0f;
				const float centerX = (ImGui::GetContentRegionAvail().x - 120 - btnSize) * 0.5f;
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerX);

				ImTextureID icon = playing ? ctx.toolbarIcons.stop : ctx.toolbarIcons.play;
				ImVec4 tint = playing
					? ImVec4(1.0f, 0.35f, 0.35f, 1.0f)
					: ImVec4(0.35f, 1.0f, 0.55f, 1.0f);

				ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1,1,1,0.08f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1,1,1,0.16f));
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));

				bool toggled = false;
				if (icon)
					toggled = ImGui::ImageButton("##tbPlay", icon, ImVec2(btnSize, btnSize),
						ImVec2(0,0), ImVec2(1,1), ImVec4(0,0,0,0), tint);
				else
					toggled = ImGui::Button(playing ? "Stop" : "Play", ImVec2(btnSize * 2.0f, btnSize));

				if (toggled && ctx.setPlayMode) ctx.setPlayMode(!playing);

				ImGui::PopStyleVar();
				ImGui::PopStyleColor(3);
			}

			// Advance cursor to fixed toolbar height and draw a separator line
			ImGui::SetCursorPos(ImVec2(0.0f, kToolbarH));
			ImGui::Separator();
		}

		ImVec2 avail = ImGui::GetContentRegionAvail();

		// HE_VIEWPORT_RESIZE_STRESS=1 oscillates the viewport size every frame
		// to stress-test render-target recreation — a crash here means a
		// texture-lifetime bug in the backend (retired textures must outlive
		// the ImGui draw list that references them).
		static const bool kResizeStress = std::getenv("HE_VIEWPORT_RESIZE_STRESS") != nullptr;
		if (kResizeStress)
		{
			static int s_stressFrame = 0;
			++s_stressFrame;
			avail.x = std::max(64.0f, avail.x - static_cast<float>((s_stressFrame % 13) * 16));
			avail.y = std::max(64.0f, avail.y - static_cast<float>((s_stressFrame %  7) * 16));
		}

		if (ctx.renderer && avail.x >= 1.0f && avail.y >= 1.0f)
		{
			// Render at framebuffer resolution (HiDPI aware)
			const ImVec2 fbScale = ImGui::GetIO().DisplayFramebufferScale;
			s_viewportPxW = static_cast<int>(avail.x * fbScale.x);
			s_viewportPxH = static_cast<int>(avail.y * fbScale.y);
			ctx.renderer->SetViewportSize(
				static_cast<uint32_t>(s_viewportPxW),
				static_cast<uint32_t>(s_viewportPxH));

			if (void* tex = ctx.renderer->GetViewportTexture())
			{
				// OpenGL FBO textures have a bottom-left origin — flip vertically
				const bool flipY = (ctx.backend == HE::RendererBackend::OpenGL);
				ImGui::Image(reinterpret_cast<ImTextureID>(tex), avail,
				             flipY ? ImVec2(0, 1) : ImVec2(0, 0),
				             flipY ? ImVec2(1, 0) : ImVec2(1, 1));

				const ImVec2 rectMin = ImGui::GetItemRectMin();
				const ImVec2 rectMax = ImGui::GetItemRectMax();
				const bool viewportHovered = ImGui::IsItemHovered();
				ImGuiIO& io = ImGui::GetIO();

				// ── Editor camera: drive from viewport input ────────────────
				// In play mode the game's scene camera takes over, so the
				// override is cleared and editor navigation is disabled.
				bool navigating = false;
				SDL_Window* sdlWin = ctx.window ? ctx.window->GetNativeWindow() : nullptr;
				// Drop fly-look capture: warp the cursor back to the press point BEFORE
				// leaving relative mode (SDL applies the warp as the post-relative
				// position, landing it exactly where the look-drag began). Shared with the
				// tab-switch safety release (releaseViewportLookCapture, file scope).
				auto endLookCapture = [&]() { releaseViewportLookCapture(sdlWin); };
				if (ctx.editorCamera && ctx.isPlaying)
				{
					endLookCapture();
					ctx.renderer->SetEditorCamera(EditorCameraOverride{}); // active=false

					// Feed the in-game UI pointer: mouse relative to the viewport
					// image, scaled to render-target pixels (the space the UI pass
					// and UISystem hit-tests operate in).
					if (ctx.reportPlayUIPointer)
					{
						const float mx = (io.MousePos.x - rectMin.x) * fbScale.x;
						const float my = (io.MousePos.y - rectMin.y) * fbScale.y;
						ctx.reportPlayUIPointer(mx, my,
							static_cast<float>(s_viewportPxW),
							static_cast<float>(s_viewportPxH),
							ImGui::IsMouseDown(ImGuiMouseButton_Left),
							viewportHovered);
					}
				}
				else if (ctx.editorCamera)
				{
					EditorCamera& cam = *ctx.editorCamera;
					const bool imageHovered = ImGui::IsItemHovered();
					const bool rmb    = ImGui::IsMouseDown(ImGuiMouseButton_Right);
					const bool mmb    = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
					const bool altLmb = io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left);
					const bool anyNav = rmb || mmb || altLmb;

					// Latch navigation so a drag keeps going if the cursor leaves the image.
					static bool s_navActive = false;
					if (imageHovered && anyNav) s_navActive = true;
					if (!anyNav)                s_navActive = false;
					navigating = s_navActive;

					// RMB fly-look capture: put the window into relative-mouse mode so we
					// read raw OS motion deltas (event.motion.xrel/yrel) instead of the
					// absolute cursor position. The previous approach sampled the absolute
					// position once per frame and warped the cursor back to the press point,
					// which discarded the sub-pixel remainder every frame. At high frame
					// rates the per-frame movement is tiny, so that cumulative loss (plus OS
					// pointer-acceleration) made looking feel sluggish and frame-rate
					// dependent. Relative mode delivers acceleration-free, frame-rate-
					// independent deltas with no warping and no display-edge collisions.
					//
					// Capture tracks the look predicate EXACTLY (rmb && !altLmb): Alt+LMB
					// is the orbit gesture, which needs a visible cursor and io.MouseDelta,
					// so engaging Alt mid-RMB must drop relative mode — otherwise orbit
					// freezes and the stale accumulator snaps the view when look resumes.
					if (sdlWin)
					{
						// Engage on a FRESH right-press over the viewport (click edge), never on
						// "RMB happens to be down" — otherwise arriving on the Scene tab with a
						// stale/held button state would capture the cursor without the user
						// starting a look here.
						const bool rmbClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
						if (rmbClicked && !altLmb && imageHovered && !s_rmbCaptured)
						{
							SDL_GetMouseState(&s_rmbStartX, &s_rmbStartY);
							SDL_SetWindowRelativeMouseMode(sdlWin, true);
							SDL_HideCursor(); // relative mode alone doesn't reliably hide the OS cursor (SDL3/macOS)
							// Discard any relative motion accumulated before capture so the
							// first look frame doesn't jump by a stale delta.
							SDL_GetRelativeMouseState(nullptr, nullptr);
							s_rmbCaptured = true;
						}
						else if ((!rmb || altLmb) && s_rmbCaptured)
						{
							endLookCapture();
						}
					}

					EditorCamera::Input cin;
					cin.dt             = dt;
					cin.viewportHeight = avail.y;
					if (navigating)
					{
						cin.orbit      = altLmb;
						cin.pan        = mmb && !altLmb;
						cin.look       = rmb && !altLmb;
						cin.mouseDelta = glm::vec2(io.MouseDelta.x, io.MouseDelta.y);
						if (cin.look)
						{
							// Relative mode keeps the OS cursor pinned, so this is the raw
							// frame motion delta — no warp, no absolute-position truncation.
							if (s_rmbCaptured && sdlWin)
							{
								float rx = 0.f, ry = 0.f;
								SDL_GetRelativeMouseState(&rx, &ry);
								cin.mouseDelta = glm::vec2(rx, ry);
								io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // don't let ImGui re-show it mid-look
								io.ConfigFlags |= ImGuiConfigFlags_NoMouse;            // block ImGui hover/click while free-looking
								SDL_HideCursor();
								ImGui::SetMouseCursor(ImGuiMouseCursor_None);
							}
							cin.fast = io.KeyShift;
							if (ImGui::IsKeyDown(ImGuiKey_D)) cin.moveAxis.x += 1.0f;
							if (ImGui::IsKeyDown(ImGuiKey_A)) cin.moveAxis.x -= 1.0f;
							if (ImGui::IsKeyDown(ImGuiKey_E)) cin.moveAxis.y += 1.0f;
							if (ImGui::IsKeyDown(ImGuiKey_Q)) cin.moveAxis.y -= 1.0f;
							if (ImGui::IsKeyDown(ImGuiKey_W)) cin.moveAxis.z += 1.0f;
							if (ImGui::IsKeyDown(ImGuiKey_S)) cin.moveAxis.z -= 1.0f;
						}
					}
					// Wheel zoom works on hover without holding a button.
					if (imageHovered) cin.wheel = io.MouseWheel;

					// Focus on selection (F) — frame the selected entity.
					if (imageHovered && !io.WantTextInput && !navigating &&
					    ImGui::IsKeyPressed(ImGuiKey_F) &&
					    ctx.world && ctx.selectedEntity != entt::null &&
					    ctx.world->registry().valid(ctx.selectedEntity))
					{
						if (auto* t = ctx.world->registry().try_get<TransformComponent>(ctx.selectedEntity))
						{
							const glm::vec3 center = glm::vec3(t->worldMatrix[3]);
							const float     radius = glm::length(t->scale) * 0.75f + 0.5f;
							cam.focusOn(center, radius);
						}
					}

					cam.update(cin);
					// Push to the backend so this frame's render uses it.
					ctx.renderer->SetEditorCamera(cam.makeOverride());
				}

				// Camera + object snapshot, identical to what the backend
				// renders with (extractor recomputes world matrices). The
				// editor camera overrides any scene camera so the gizmo and
				// picking ray match exactly what is on screen.
				static RenderExtractor s_extractor;
				static RenderWorld     s_sceneSnapshot;
				const EditorCameraOverride camOverride =
					(ctx.editorCamera && !ctx.isPlaying) ? ctx.editorCamera->makeOverride()
					                                     : EditorCameraOverride{};
				if (ctx.world)
					s_extractor.extract(*ctx.world, s_sceneSnapshot, avail.x / avail.y,
					                    camOverride.active ? &camOverride : nullptr);


				// Suppress the gizmo while the camera is being driven so Alt+LMB
				// orbit and RMB fly-look don't fight the manipulator.
				ImGuizmo::Enable(!navigating && !io.KeyAlt);

				// ── Gizmo on the selected entity ────────────────────────────
				// Suppressed in Landscape mode: there LMB belongs to the sculpt
				// brush, and a stray gizmo drag would silently move/scale the
				// terrain — which then breaks the brush's world↔grid mapping.
				bool gizmoActive = false;
				if (ctx.editorConfig.mode != EditorMode::Landscape &&
				    ctx.world && ctx.selectedEntity != entt::null &&
				    ctx.world->registry().valid(ctx.selectedEntity))
				if (auto* t = ctx.world->registry().try_get<TransformComponent>(ctx.selectedEntity))
				{
					// W/E/R switch operation while the viewport is hovered
					// (but not while flying — W/A/S/D drive the camera then). The
					// viewport toolbar's Move/Rotate/Scale buttons set the same
					// shared s_gizmoOp.
					if (ImGui::IsWindowHovered() && !ImGui::GetIO().WantTextInput && !navigating)
					{
						if (ImGui::IsKeyPressed(ImGuiKey_W)) s_gizmoOp = ImGuizmo::TRANSLATE;
						if (ImGui::IsKeyPressed(ImGuiKey_E)) s_gizmoOp = ImGuizmo::ROTATE;
						if (ImGui::IsKeyPressed(ImGuiKey_R)) s_gizmoOp = ImGuizmo::SCALE;
					}

					ImGuizmo::SetOrthographic(false);
					ImGuizmo::SetDrawlist();
					ImGuizmo::SetRect(rectMin.x, rectMin.y,
					                  rectMax.x - rectMin.x, rectMax.y - rectMin.y);

					// Pre-state for undo — captured ONLY on the frame a gizmo drag is
					// about to begin (mouse pressed over the gizmo), NOT every frame.
					// capturePre() serializes the WHOLE world (expensive with terrain),
					// so the old per-frame call dropped the editor to ~15 ms the moment
					// anything was selected. IsOver()+MouseClicked fires once, just
					// before Manipulate first mutates the transform this frame.
					if (ctx.undoSys && !ImGuizmo::IsUsing() && ImGuizmo::IsOver()
					    && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
						ctx.undoSys->capturePre();

					// For rotation, optionally drop ImGuizmo's outer screen-space
					// ring (rotate about the view axis) — it's the confusing white
					// circle. Toggled from the toolbar.
					ImGuizmo::OPERATION effectiveOp = s_gizmoOp;
					if (s_gizmoOp == ImGuizmo::ROTATE && !s_rotateScreen)
						effectiveOp = ImGuizmo::ROTATE_X | ImGuizmo::ROTATE_Y | ImGuizmo::ROTATE_Z;

					glm::mat4 world = t->worldMatrix;
					ImGuizmo::Manipulate(
						&s_sceneSnapshot.camera.view[0][0],
						&s_sceneSnapshot.camera.projection[0][0],
						effectiveOp, s_gizmoMode, &world[0][0]);

					// Undo session: one entry per drag
					static bool s_gizmoWasUsing = false;
					if (ctx.undoSys)
					{
						if (ImGuizmo::IsUsing() && !s_gizmoWasUsing) ctx.undoSys->stashPre();
						if (!ImGuizmo::IsUsing() && s_gizmoWasUsing) ctx.undoSys->commitPending();
					}
					s_gizmoWasUsing = ImGuizmo::IsUsing();

					gizmoActive = ImGuizmo::IsOver() || ImGuizmo::IsUsing();

					if (ImGuizmo::IsUsing())
					{
						// world → local: divide out the parent's world matrix
						glm::mat4 parentWorld(1.0f);
						if (auto* h = ctx.world->registry().try_get<HierarchyComponent>(ctx.selectedEntity);
						    h && h->parent != entt::null)
							if (auto* pt = ctx.world->registry().try_get<TransformComponent>(h->parent))
								parentWorld = pt->worldMatrix;
						glm::mat4 local = glm::inverse(parentWorld) * world;

						float pos[3], rot[3], scale[3];
						ImGuizmo::DecomposeMatrixToComponents(&local[0][0], pos, rot, scale);
						t->position = { pos[0],   pos[1],   pos[2]   };
						t->rotation = { rot[0],   rot[1],   rot[2]   };
						t->scale    = { scale[0], scale[1], scale[2] };
						t->dirty    = true;
					}
				}

				// ── Picking: click in the viewport selects the hit entity ──
				// Disabled in Landscape mode so a brush stroke can't deselect /
				// reselect entities and pop the gizmo back up mid-sculpt.
				if (ctx.editorConfig.mode != EditorMode::Landscape &&
				    ctx.world && !gizmoActive && !navigating && !io.KeyAlt &&
				    ImGui::IsItemClicked(ImGuiMouseButton_Left))
				{
					const ImVec2 mouse = ImGui::GetMousePos();
					const float  u = (mouse.x - rectMin.x) / (rectMax.x - rectMin.x);
					const float  v = (mouse.y - rectMin.y) / (rectMax.y - rectMin.y);

					// Unproject the click to a world-space ray
					const glm::mat4 invVP = glm::inverse(
						s_sceneSnapshot.camera.projection * s_sceneSnapshot.camera.view);
					const glm::vec4 ndcNear(2.0f * u - 1.0f, 1.0f - 2.0f * v, -1.0f, 1.0f);
					const glm::vec4 ndcFar (ndcNear.x, ndcNear.y, 1.0f, 1.0f);
					glm::vec4 pNear = invVP * ndcNear; pNear /= pNear.w;
					glm::vec4 pFar  = invVP * ndcFar;  pFar  /= pFar.w;
					const glm::vec3 rayOrigin(pNear);
					const glm::vec3 rayDir(glm::vec3(pFar) - glm::vec3(pNear));

					// Local-space AABBs per mesh asset, cached (file-scope).
					// Entities without an asset use the built-in fallback cube's box.
					static const HE::AABB s_cubeBox = []{
						HE::AABB b; b.expand({-0.5f,-0.5f,-0.5f}); b.expand({0.5f,0.5f,0.5f}); return b;
					}();

					Entity hit     = entt::null;
					float  hitDist = std::numeric_limits<float>::max();
					for (const RenderObject& obj : s_sceneSnapshot.objects)
					{
						HE::AABB box = s_cubeBox;
						if (obj.meshAssetId != HE::UUID{} && ctx.contentManager)
						{
							auto it = s_aabbCache.find(obj.meshAssetId);
							if (it == s_aabbCache.end())
							{
								if (const StaticMeshAsset* mesh =
									ctx.contentManager->getStaticMesh(obj.meshAssetId))
									it = s_aabbCache.emplace(obj.meshAssetId,
										HE::AABB::fromPositions(mesh->vertices.data(),
										                        mesh->vertices.size() / 3)).first;
							}
							if (it != s_aabbCache.end() && it->second.isValid())
								box = it->second;
						}

						// Ray → object space (exact test for rotated objects)
						const glm::mat4 invModel = glm::inverse(obj.transform);
						const glm::vec3 o = glm::vec3(invModel * glm::vec4(rayOrigin, 1.0f));
						const glm::vec3 d = glm::vec3(invModel * glm::vec4(rayDir,    0.0f));

						float t = 0.0f;
						if (box.intersectRay(o, d, t) && t < hitDist)
						{
							hitDist = t;
							hit     = static_cast<Entity>(obj.entityId);
						}
					}
					ctx.selectedEntity = hit; // miss = deselect
				}

				// ── Landscape brush cursor + sculpt ────────────────────────
				if (ctx.editorConfig.mode == EditorMode::Landscape && ctx.world)
				{
					auto& terrainReg = ctx.world->registry();
					auto  tvw        = terrainReg.view<TerrainComponent>();
					if (!tvw.empty())
					{
						Entity terrainEnt = tvw.front();
						auto&  tc         = terrainReg.get<TerrainComponent>(terrainEnt);

						// Terrain world translation. The brush works in world XZ but
						// the height grid is local, so every world↔grid conversion has
						// to subtract / add this offset (recovers a terrain that an
						// earlier stray gizmo drag may have displaced).
						glm::vec3 terrainWorldPos(0.0f);
						if (const auto* xf = terrainReg.try_get<TransformComponent>(terrainEnt))
							terrainWorldPos = xf->position;
						const float terrainWorldY = terrainWorldPos.y;

						// ── Terrain height sampler (bilinear, local space) ────────
						// Returns the sculpted height at world XZ; 0 if no sculpt data.
						const uint32_t tcRes   = std::clamp(tc.resolution, 2u, 1024u);
						const float    tcHalfX = tc.sizeX * 0.5f;
						const float    tcHalfZ = tc.sizeZ * 0.5f;
						const float    tcStepX = tc.sizeX / static_cast<float>(tcRes - 1);
						const float    tcStepZ = tc.sizeZ / static_cast<float>(tcRes - 1);

						auto sampleH = [&](float wx, float wz) -> float
						{
							const float lx = wx - terrainWorldPos.x;  // world → local XZ
							const float lz = wz - terrainWorldPos.z;
							// No baked heights yet (e.g. a freshly-loaded seeded
							// terrain): sample the generated surface so the cursor
							// still tracks fBm bumps. Brushes bake on first stroke.
							if (tc.sculptHeights.empty())
								return terrainHeightAt(tc, lx, lz);
							const float gx = std::clamp((lx + tcHalfX) / tcStepX,
							                            0.0f, static_cast<float>(tcRes - 1));
							const float gz = std::clamp((lz + tcHalfZ) / tcStepZ,
							                            0.0f, static_cast<float>(tcRes - 1));
							const int xi0 = static_cast<int>(gx);
							const int zi0 = static_cast<int>(gz);
							const int xi1 = std::min(xi0 + 1, static_cast<int>(tcRes) - 1);
							const int zi1 = std::min(zi0 + 1, static_cast<int>(tcRes) - 1);
							const float fx = gx - static_cast<float>(xi0);
							const float fz = gz - static_cast<float>(zi0);
							const float h00 = tc.sculptHeights[zi0 * tcRes + xi0];
							const float h10 = tc.sculptHeights[zi0 * tcRes + xi1];
							const float h01 = tc.sculptHeights[zi1 * tcRes + xi0];
							const float h11 = tc.sculptHeights[zi1 * tcRes + xi1];
							return h00*(1-fx)*(1-fz) + h10*fx*(1-fz)
							     + h01*(1-fx)*fz     + h11*fx*fz;
						};

						// ── Unproject mouse → refined terrain surface hit ─────────
						const ImVec2 mouse = ImGui::GetMousePos();
						const bool mouseInViewport =
							mouse.x >= rectMin.x && mouse.x <= rectMax.x &&
							mouse.y >= rectMin.y && mouse.y <= rectMax.y;

						bool      hasHit = false;
						glm::vec3 hitWS{};

						const glm::mat4 VP    = s_sceneSnapshot.camera.projection
						                      * s_sceneSnapshot.camera.view;
						const glm::mat4 invVP = glm::inverse(VP);

						if (mouseInViewport)
						{
							const float u = (mouse.x - rectMin.x) / (rectMax.x - rectMin.x);
							const float v = (mouse.y - rectMin.y) / (rectMax.y - rectMin.y);
							const glm::vec4 ndcNear(2.0f*u-1.0f, 1.0f-2.0f*v, -1.0f, 1.0f);
							const glm::vec4 ndcFar (ndcNear.x, ndcNear.y,       1.0f, 1.0f);
							glm::vec4 pNear = invVP * ndcNear; pNear /= pNear.w;
							glm::vec4 pFar  = invVP * ndcFar;  pFar  /= pFar.w;
							const glm::vec3 rayOrigin(pNear);
							const glm::vec3 rayDir = glm::normalize(glm::vec3(pFar) - glm::vec3(pNear));
							const float denom = rayDir.y;
							if (std::abs(denom) > 1e-5f)
							{
								// Start at the flat base plane (Y = terrainWorldY), then
								// fixed-point iterate t so the hit lands on the actual
								// sculpted surface Y = terrainWorldY + h(p.xz). One step
								// suffices for gentle slopes; a few more keep the cursor
								// glued to the surface on steep, heavily-sculpted terrain.
								float t = (terrainWorldY - rayOrigin.y) / denom;
								if (t > 0.0f)
								{
									glm::vec3 p = rayOrigin + t * rayDir;
									for (int it = 0; it < 8; ++it)
									{
										const float surfaceY = terrainWorldY + sampleH(p.x, p.z);
										const float tn = (surfaceY - rayOrigin.y) / denom;
										if (tn <= 0.0f) break;
										const bool converged = std::abs(tn - t) < 1e-3f;
										t = tn;
										p = rayOrigin + t * rayDir;
										if (converged) break;
									}
									if (t > 0.0f)
									{
										hitWS  = p;
										hasHit = true;
									}
								}
							}
						}

						// ── Draw brush circles on the terrain surface ─────────────
						if (hasHit && !navigating)
						{
							ImDrawList* dl    = ImGui::GetWindowDrawList();
							const float viewW = rectMax.x - rectMin.x;
							const float viewH = rectMax.y - rectMin.y;

							// Project a world XZ point at its actual terrain height → screen
							auto projectPt = [&](float wx, float wz, ImVec2& outPt) -> bool
							{
								const float wy = terrainWorldY + sampleH(wx, wz);
								glm::vec4 clip = VP * glm::vec4(wx, wy, wz, 1.0f);
								if (clip.w <= 0.0f) return false;
								clip /= clip.w;
								if (clip.z < -1.0f || clip.z > 1.0f) return false;
								outPt = ImVec2(
									rectMin.x + (clip.x * 0.5f + 0.5f) * viewW,
									rectMin.y + (0.5f   - clip.y * 0.5f) * viewH);
								return true;
							};

							constexpr int   kSeg = 48;
							constexpr float kPi2 = 6.28318530f;
							const float totalR   = s_brushRadius + s_falloffRadius;

							for (int ci = 0; ci < 2; ++ci)
							{
								const float r     = (ci == 0) ? s_brushRadius : totalR;
								const ImU32 col   = (ci == 0) ? IM_COL32(255,255,255,210)
								                              : IM_COL32(180,180,180,120);
								const float thick = (ci == 0) ? 1.5f : 1.0f;
								if (r < 0.01f) continue;

								ImVec2 prev{}; bool prevValid = false;
								for (int i = 0; i <= kSeg; ++i)
								{
									const float a = kPi2 * i / kSeg;
									ImVec2 cur{}; bool curValid = projectPt(
										hitWS.x + r * std::cos(a),
										hitWS.z + r * std::sin(a), cur);
									if (prevValid && curValid)
										dl->AddLine(prev, cur, col, thick);
									prev = cur; prevValid = curValid;
								}
							}

							// Ramp guide: a line from the stroke start to the cursor so
							// the gradient direction is visible while dragging.
							if (s_terrainTool == TerrainTool::Ramp && s_rampValid &&
							    ImGui::IsMouseDown(ImGuiMouseButton_Left))
							{
								ImVec2 ps{}, pe{};
								if (projectPt(s_rampStartWS.x, s_rampStartWS.z, ps) &&
								    projectPt(hitWS.x, hitWS.z, pe))
									dl->AddLine(ps, pe, IM_COL32(120,200,255,230), 2.0f);
							}
						}

						// ── Apply brush on LMB drag ───────────────────────────────
						const bool lmbDown =
							ImGui::IsMouseDown(ImGuiMouseButton_Left) && !io.KeyAlt
							&& (viewportHovered || s_brushWasDown);

						if (lmbDown && !s_brushWasDown)
						{
							if (ctx.undoSys) ctx.undoSys->snapshotNow();
							// Lazy-init sculptHeights from current terrain geometry
							if (tc.sculptHeights.empty())
							{
								const size_t nVerts = static_cast<size_t>(tcRes) * tcRes;
								const StaticMeshAsset tmp = generateTerrainMesh(tc);
								tc.sculptHeights.resize(nVerts);
								for (size_t vi = 0; vi < nVerts; ++vi)
									tc.sculptHeights[vi] = tmp.vertices[vi * 3 + 1];
							}
							// Capture stroke-scoped targets (sculptHeights is now
							// populated, so sampleH returns the real local height).
							if (hasHit)
							{
								s_flattenTarget = sampleH(hitWS.x, hitWS.z);
								s_rampStartWS   = hitWS;
								s_rampStartH    = s_flattenTarget;
								s_rampValid     = true;
							}
							else
								s_rampValid = false;
						}
						s_brushWasDown = lmbDown;

						if (lmbDown && hasHit && !tc.sculptHeights.empty())
						{
							const float totalR  = s_brushRadius + s_falloffRadius;
							const float totalR2 = totalR * totalR;
							const float delta   = s_brushStrength * static_cast<float>(dt);
							bool anyChange = false;

							auto brushWeight = [&](float dist2) -> float
							{
								if (dist2 >= totalR2) return 0.0f;
								const float dist = std::sqrt(dist2);
								if (dist <= s_brushRadius) return 1.0f;
								if (s_falloffRadius < 0.001f) return 0.0f;
								return 1.0f - (dist - s_brushRadius) / s_falloffRadius;
							};

							if (s_terrainTool == TerrainTool::Smooth)
							{
								std::vector<float> smoothed = tc.sculptHeights;
								for (uint32_t zi = 0; zi < tcRes; ++zi)
								{
									for (uint32_t xi = 0; xi < tcRes; ++xi)
									{
										const float wx = terrainWorldPos.x - tcHalfX + static_cast<float>(xi) * tcStepX;
										const float wz = terrainWorldPos.z - tcHalfZ + static_cast<float>(zi) * tcStepZ;
										const float d2 = (hitWS.x-wx)*(hitWS.x-wx)
										               + (hitWS.z-wz)*(hitWS.z-wz);
										const float w  = brushWeight(d2);
										if (w <= 0.0f) continue;
										float sum = 0.0f; int cnt = 0;
										for (int dzi = -1; dzi <= 1; ++dzi)
											for (int dxi = -1; dxi <= 1; ++dxi)
											{
												const int ni = static_cast<int>(zi) + dzi;
												const int nj = static_cast<int>(xi) + dxi;
												if (ni >= 0 && ni < static_cast<int>(tcRes) &&
												    nj >= 0 && nj < static_cast<int>(tcRes))
												{ sum += tc.sculptHeights[ni*tcRes+nj]; ++cnt; }
											}
										const float avg = cnt > 0 ? sum/cnt
										                          : tc.sculptHeights[zi*tcRes+xi];
										// Blend factor: 10× the raise/lower rate so smooth
										// is visually comparable in speed.
										const float blend = std::min(w * delta * 10.0f, w);
										smoothed[zi*tcRes+xi] = tc.sculptHeights[zi*tcRes+xi]
										    + blend * (avg - tc.sculptHeights[zi*tcRes+xi]);
										anyChange = true;
									}
								}
								if (anyChange) tc.sculptHeights = std::move(smoothed);
							}
							else if (s_terrainTool == TerrainTool::Flatten)
							{
								// Pull heights toward the height sampled where the
								// stroke began — levels bumps without a fixed plane.
								for (uint32_t zi = 0; zi < tcRes; ++zi)
								for (uint32_t xi = 0; xi < tcRes; ++xi)
								{
									const float wx = terrainWorldPos.x - tcHalfX + static_cast<float>(xi) * tcStepX;
									const float wz = terrainWorldPos.z - tcHalfZ + static_cast<float>(zi) * tcStepZ;
									const float d2 = (hitWS.x-wx)*(hitWS.x-wx) + (hitWS.z-wz)*(hitWS.z-wz);
									const float w  = brushWeight(d2);
									if (w <= 0.0f) continue;
									const float blend = std::min(w * delta * 6.0f, w);
									float& h = tc.sculptHeights[zi*tcRes+xi];
									h += blend * (s_flattenTarget - h);
									anyChange = true;
								}
							}
							else if (s_terrainTool == TerrainTool::Ramp)
							{
								// Linear height gradient from the stroke start to the
								// cursor, inside a corridor the width of the brush.
								const glm::vec2 a(s_rampStartWS.x, s_rampStartWS.z);
								const glm::vec2 b(hitWS.x, hitWS.z);
								const glm::vec2 ab = b - a;
								const float L2 = glm::dot(ab, ab);
								if (s_rampValid && L2 > 1e-3f)
								{
									const float endH = sampleH(hitWS.x, hitWS.z);
									for (uint32_t zi = 0; zi < tcRes; ++zi)
									for (uint32_t xi = 0; xi < tcRes; ++xi)
									{
										const float wx = terrainWorldPos.x - tcHalfX + static_cast<float>(xi) * tcStepX;
										const float wz = terrainWorldPos.z - tcHalfZ + static_cast<float>(zi) * tcStepZ;
										const glm::vec2 p(wx, wz);
										const float t = std::clamp(glm::dot(p - a, ab) / L2, 0.0f, 1.0f);
										const glm::vec2 d = p - (a + t * ab);
										const float w = brushWeight(glm::dot(d, d)); // dist² to ramp line
										if (w <= 0.0f) continue;
										const float target = s_rampStartH + (endH - s_rampStartH) * t;
										const float blend  = std::min(w * delta * 6.0f, w);
										float& h = tc.sculptHeights[zi*tcRes+xi];
										h += blend * (target - h);
										anyChange = true;
									}
								}
							}
							else if (s_terrainTool == TerrainTool::Roughen)
							{
								// Stable per-vertex hash → consistent bumps, no shimmer.
								auto vhash = [](uint32_t xi, uint32_t zi) -> float
								{
									uint32_t n = xi * 73856093u ^ zi * 19349663u;
									n = (n ^ 61u) ^ (n >> 16u); n += n << 3u;
									n ^= n >> 4u; n *= 0x27D4EB2Du; n ^= n >> 15u;
									return static_cast<float>(n & 0x00FFFFFFu)
									     / static_cast<float>(0x01000000u) * 2.0f - 1.0f;
								};
								for (uint32_t zi = 0; zi < tcRes; ++zi)
								for (uint32_t xi = 0; xi < tcRes; ++xi)
								{
									const float wx = terrainWorldPos.x - tcHalfX + static_cast<float>(xi) * tcStepX;
									const float wz = terrainWorldPos.z - tcHalfZ + static_cast<float>(zi) * tcStepZ;
									const float d2 = (hitWS.x-wx)*(hitWS.x-wx) + (hitWS.z-wz)*(hitWS.z-wz);
									const float w  = brushWeight(d2);
									if (w <= 0.0f) continue;
									tc.sculptHeights[zi*tcRes+xi] += vhash(xi, zi) * w * delta;
									anyChange = true;
								}
							}
							else
							{
								const float sign = (s_terrainTool == TerrainTool::Raise) ? 1.0f : -1.0f;
								for (uint32_t zi = 0; zi < tcRes; ++zi)
								{
									for (uint32_t xi = 0; xi < tcRes; ++xi)
									{
										const float wx = terrainWorldPos.x - tcHalfX + static_cast<float>(xi) * tcStepX;
										const float wz = terrainWorldPos.z - tcHalfZ + static_cast<float>(zi) * tcStepZ;
										const float d2 = (hitWS.x-wx)*(hitWS.x-wx)
										               + (hitWS.z-wz)*(hitWS.z-wz);
										const float w  = brushWeight(d2);
										if (w <= 0.0f) continue;
										tc.sculptHeights[zi*tcRes+xi] += sign * w * delta;
										anyChange = true;
									}
								}
							}

							if (anyChange && ctx.contentManager)
							{
								if (const auto* mc = terrainReg.try_get<MeshComponent>(terrainEnt))
									s_aabbCache.erase(mc->meshAssetId);
								// Regenerate only the chunks the brush touched (terrain-local
								// XZ rect around the hit + brush extent), not all 64+ chunks —
								// otherwise interactive sculpting rebuilds the whole terrain each
								// frame. The first stroke still triggers a full build (the chunk
								// grid doesn't exist yet → TerrainSystem detects the grid change).
								const float r = s_brushRadius + s_falloffRadius;
								float mnX = hitWS.x - r, mxX = hitWS.x + r;
								float mnZ = hitWS.z - r, mxZ = hitWS.z + r;
								if (s_terrainTool == TerrainTool::Ramp && s_rampValid)
								{
									mnX = std::min(mnX, s_rampStartWS.x - r); mxX = std::max(mxX, s_rampStartWS.x + r);
									mnZ = std::min(mnZ, s_rampStartWS.z - r); mxZ = std::max(mxZ, s_rampStartWS.z + r);
								}
								tc.dirtyMinX = mnX - terrainWorldPos.x; tc.dirtyMaxX = mxX - terrainWorldPos.x;
								tc.dirtyMinZ = mnZ - terrainWorldPos.z; tc.dirtyMaxZ = mxZ - terrainWorldPos.z;
								tc.regionDirty = true;
								TerrainSystem::updateTerrains(*ctx.world, *ctx.contentManager,
								                              ctx.renderer);
							}
						}
					}
				}

			}
			else
				ImGui::TextDisabled("  Viewport not available on this backend yet.");
		}
		ImGui::End();
	}


    // ── Quick Settings / Landscape panel ────────────────────────────────────
    // When Landscape mode is active the panel becomes the Landscape tool panel;
    // otherwise it shows the user-pinned Quick Settings as normal.
    if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
    ImGui::Begin("Quick Settings");
    if (ctx.fontHeading) ImGui::PopFont();

    if (ctx.editorConfig.mode == EditorMode::Landscape && ctx.world)
    {
        // ── Check for an existing terrain entity ─────────────────────────────
        auto& reg = ctx.world->registry();
        auto terrainView = reg.view<TerrainComponent>();
        const bool hasTerrain = !terrainView.empty();

        if (!hasTerrain)
        {
            // ── No terrain yet — show creation form ──────────────────────────
            // Parameters live on EditorConfig so the renderer can draw a 3D grid
            // preview of the terrain-to-be (see EditorApplication debug-draw).
            auto& np = ctx.editorConfig.newTerrain;
            ImGui::SeparatorText("Create Landscape");
            ImGui::TextDisabled("Green grid in the viewport previews the result");

            ImGui::DragFloat("Width (X)",    &np.sizeX,       1.0f,  1.0f, 10000.0f, "%.1f m");
            ImGui::DragFloat("Depth (Z)",    &np.sizeZ,       1.0f,  1.0f, 10000.0f, "%.1f m");
            ImGui::DragInt  ("Resolution",   &np.resolution,  1,     2,    512);
            ImGui::DragFloat("Height Scale", &np.heightScale, 0.5f,  0.0f, 1000.0f,  "%.1f m");
            ImGui::SeparatorText("Noise (seed 0 = flat)");
            ImGui::DragInt  ("Seed",         &np.seed,        1,     0,    0x7fffffff);
            ImGui::DragInt  ("Octaves",      &np.octaves,     1,     1,    8);
            ImGui::DragFloat("Frequency",    &np.frequency,   0.01f, 0.01f, 16.0f, "%.2f");
            ImGui::DragFloat("Lacunarity",   &np.lacunarity,  0.01f, 1.0f,  8.0f,  "%.2f");
            ImGui::DragFloat("Gain",         &np.gain,        0.01f, 0.0f,  1.0f,  "%.2f");

            ImGui::Spacing();
            const float btnW = 160.0f;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - btnW) * 0.5f
                                 + ImGui::GetCursorPosX());
            if (ImGui::Button("Create Landscape", ImVec2(btnW, 0)))
            {
                if (ctx.undoSys) ctx.undoSys->snapshotNow();

                TerrainComponent tc;
                tc.sizeX      = np.sizeX;
                tc.sizeZ      = np.sizeZ;
                tc.resolution = static_cast<uint32_t>(std::clamp(np.resolution, 2, 1024));
                tc.heightScale= np.heightScale;
                tc.seed       = np.seed;
                tc.octaves    = np.octaves;
                tc.frequency  = np.frequency;
                tc.lacunarity = np.lacunarity;
                tc.gain       = np.gain;
                tc.dirty      = true;

                // Bake a seeded surface into editable per-vertex heights right
                // away. From here the terrain is a plain heightfield the brushes
                // edit directly — the seed/noise is a one-time creation input and
                // no longer feeds the mesh (sculptHeights overrides it). A flat
                // terrain (seed 0) is left empty and stays flat until first sculpt.
                if (tc.seed != 0)
                {
                    const StaticMeshAsset gen = generateTerrainMesh(tc);
                    const size_t nVerts = gen.vertices.size() / 3;
                    tc.sculptHeights.resize(nVerts);
                    for (size_t vi = 0; vi < nVerts; ++vi)
                        tc.sculptHeights[vi] = gen.vertices[vi * 3 + 1];
                }

                Entity e = ctx.world->createEntity("Terrain");
                ctx.world->addComponent(e, TransformComponent{});
                ctx.world->addComponent(e, tc);
                MaterialComponent mc;
                mc.materialAssetId = HE::kDefaultTerrainMaterialId;
                ctx.world->addComponent(e, mc);

                ctx.world->markHierarchyDirty();
                ctx.selectedEntity = e;
                Logger::Log(Logger::LogLevel::Info, "Editor: created Terrain entity");
            }
        }
        else
        {
            // ── Terrain exists — show sculpt tools ───────────────────────────
            ImGui::SeparatorText("Sculpt Tool");

            const char* toolLabels[] = { "Raise", "Lower", "Smooth",
                                         "Flatten", "Ramp", "Roughen" };
            const int toolCount = static_cast<int>(sizeof(toolLabels) / sizeof(toolLabels[0]));
            const int toolIdx   = static_cast<int>(s_terrainTool);
            for (int i = 0; i < toolCount; ++i)
            {
                if (i % 3 != 0) ImGui::SameLine();   // 3 tools per row
                if (ImGui::RadioButton(toolLabels[i], toolIdx == i))
                    s_terrainTool = static_cast<TerrainTool>(i);
            }

            ImGui::Spacing();
            ImGui::DragFloat("Radius##brush",   &s_brushRadius,   0.5f,  0.5f, 500.0f, "%.1f m");
            ImGui::DragFloat("Falloff##brush",  &s_falloffRadius, 0.5f,  0.0f, 500.0f, "%.1f m");
            ImGui::DragFloat("Strength##brush", &s_brushStrength, 0.1f,  0.1f,  50.0f, "%.2f");
            s_brushRadius   = std::max(0.5f, s_brushRadius);
            s_falloffRadius = std::max(0.0f, s_falloffRadius);

            ImGui::Spacing();
            // Per-tool hint — Ramp and Flatten read the point where the drag began.
            switch (s_terrainTool)
            {
            case TerrainTool::Ramp:
                ImGui::TextDisabled("Drag from one spot to another:");
                ImGui::TextDisabled("ramps between their heights");
                break;
            case TerrainTool::Flatten:
                ImGui::TextDisabled("Flattens toward the height");
                ImGui::TextDisabled("where the drag began");
                break;
            case TerrainTool::Roughen:
                ImGui::TextDisabled("Adds fixed-noise bumps under the brush");
                break;
            default:
                ImGui::TextDisabled("LMB drag in viewport to sculpt");
                break;
            }
            ImGui::TextDisabled("Ctrl+click a field to type a value");

            ImGui::Spacing();
            if (ImGui::Button("Reset Sculpting"))
            {
                Entity terrainEnt = terrainView.front();
                auto& tc = reg.get<TerrainComponent>(terrainEnt);
                if (ctx.undoSys) ctx.undoSys->snapshotNow();
                tc.sculptHeights.clear();
                tc.dirty = true;
            }
        }
    }
    else
    {
        // Quick Settings = the engine settings the user pinned in Preferences.
        DrawEngineSettings(ctx, SettingsMode::QuickSettings);
    }

    ImGui::End();

    // World Outliner
    if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
    ImGui::Begin("World Outliner");
    if (ctx.fontHeading) ImGui::PopFont();

    if (ctx.world)
    {
        // ── Cached hierarchy snapshot ─────────────────────────────────────
        struct OutlinerNode
        {
            Entity      entity;
            std::string name;
            int         depth;
            bool        hasChildren;
        };
        static std::vector<OutlinerNode> s_outlinerCache;
        static HorizonWorld*             s_lastWorld = nullptr;

        // Rebuild cache only when the hierarchy changed or the world switched
        if (ctx.world->isHierarchyDirty() || s_lastWorld != ctx.world)
        {
            s_lastWorld = ctx.world;
            s_outlinerCache.clear();

            auto& registry = ctx.world->registry();
            Entity root    = ctx.world->rootEntity();

            std::function<void(Entity, int)> collect = [&](Entity entity, int depth)
            {
                if (!registry.valid(entity)) return;
                // The built-in environment sun/moon lights belong to the World's
                // Environment, and runtime terrain chunks are generated from the
                // TerrainComponent — hide both from the Outliner.
                if (entity != ctx.world->rootEntity() &&
                    (registry.all_of<EnvironmentLightComponent>(entity) ||
                     registry.all_of<TerrainChunkComponent>(entity)))
                    return;
                auto* name = registry.try_get<NameComponent>(entity);
                auto* hier = registry.try_get<HierarchyComponent>(entity);
                s_outlinerCache.push_back({
                    entity,
                    name ? name->name : "(unnamed)",
                    depth,
                    hier && !hier->children.empty()
                });
                if (hier)
                    for (Entity child : hier->children)
                        collect(child, depth + 1);
            };
            collect(root, 0);

            char buf[96];
            std::snprintf(buf, sizeof(buf), "[Outliner] rebuilt: %zu nodes", s_outlinerCache.size());
            Logger::Log(HE::LogLevel::Info, buf);

            ctx.world->clearHierarchyDirty();
        }

        // ── Entity rename popup state ─────────────────────────────────────
        static Entity s_renameEntity     = entt::null;
        static char   s_entityRenameBuf[256] = {};
        static bool   s_openEntityRename = false;

        // ── Render from cache ─────────────────────────────────────────────
        int prevDepth      = -1;
        int skipBelowDepth = INT_MAX; // skip children of closed nodes

        for (const auto& node : s_outlinerCache)
        {
            // If a parent was closed, skip all its children
            if (node.depth > skipBelowDepth)
                continue;
            skipBelowDepth = INT_MAX; // back at or above the closed level → reset

            // Close tree levels we've left
            while (prevDepth >= node.depth)
            {
                ImGui::TreePop();
                --prevDepth;
            }

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                     | ImGuiTreeNodeFlags_SpanAvailWidth
                                     | ImGuiTreeNodeFlags_DefaultOpen;
            if (!node.hasChildren)
                flags |= ImGuiTreeNodeFlags_Leaf;
            if (node.entity == ctx.selectedEntity)
                flags |= ImGuiTreeNodeFlags_Selected;

            bool open = ImGui::TreeNodeEx(
                reinterpret_cast<void*>(static_cast<uintptr_t>(
                    static_cast<uint32_t>(node.entity))),
                flags, "%s", node.name.c_str());

            // Click (not on the arrow) → select
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
                ctx.selectedEntity = node.entity;

            // ── Drag & drop reparenting ───────────────────────────────────
            const bool isRoot = (node.entity == ctx.world->rootEntity());
            if (!isRoot && ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload("HE_ENTITY", &node.entity, sizeof(Entity));
                ImGui::TextUnformatted(node.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HE_ENTITY"))
                {
                    Entity dragged{};
                    std::memcpy(&dragged, payload->Data, sizeof(Entity));
                    if (ctx.undoSys) ctx.undoSys->snapshotNow();
                    ctx.world->reparentEntity(dragged, node.entity);
                }
                ImGui::EndDragDropTarget();
            }

            // ── Per-entity context menu ───────────────────────────────────
            if (ImGui::BeginPopupContextItem())
            {
                ctx.selectedEntity = node.entity;
                if (ImGui::MenuItem("Create Child Entity"))
                {
                    if (ctx.undoSys) ctx.undoSys->snapshotNow();
                    Entity child = ctx.world->createEntity("Entity");
                    ctx.world->addComponent(child, TransformComponent{});
                    ctx.world->reparentEntity(child, node.entity);
                    ctx.selectedEntity = child;
                }
                if (ImGui::MenuItem("Rename"))
                {
                    s_renameEntity = node.entity;
                    std::strncpy(s_entityRenameBuf, node.name.c_str(), sizeof(s_entityRenameBuf) - 1);
                    s_entityRenameBuf[sizeof(s_entityRenameBuf) - 1] = '\0';
                    s_openEntityRename = true;
                }
                if (!isRoot && ImGui::MenuItem("Save as Prefab") && ctx.contentManager)
                {
                    SceneSerializer ser;
                    auto data = ser.serializeSubtree(*ctx.world, node.entity);
                    PrefabAsset prefab;
                    prefab.name = node.name;
                    prefab.data = std::move(data);
                    ctx.contentManager->registerPrefab(std::move(prefab));
                }
                if (!isRoot && ImGui::MenuItem("Delete"))
                {
                    if (ctx.selectedEntity == node.entity)
                        ctx.selectedEntity = entt::null;
                    if (ctx.undoSys) ctx.undoSys->snapshotNow();
                    ctx.world->destroyEntity(node.entity);
                }
                ImGui::EndPopup();
            }

            if (open)
                prevDepth = node.depth;
            else
                skipBelowDepth = node.depth; // don't enter children
        }
        // Close remaining open levels
        while (prevDepth >= 0)
        {
            ImGui::TreePop();
            --prevDepth;
        }

        // ── Drop onto the empty area below the tree → un-parent to the World root ──
        // The root is not a normal drop target (it's a built-in), so dragging an entity
        // onto the outliner background is how you detach a child back to the top level.
        // A rect-based target (not a Dummy item) so it doesn't suppress the background
        // right-click "Create Entity" menu (which uses NoOpenOverItems).
        {
            const ImVec2 dropMin = ImGui::GetCursorScreenPos();
            const ImVec2 avail   = ImGui::GetContentRegionAvail();
            const ImRect dropBB(dropMin, ImVec2(dropMin.x + avail.x,
                                                dropMin.y + std::max(avail.y, 24.0f)));
            if (ImGui::BeginDragDropTargetCustom(dropBB, ImGui::GetID("##outliner_root_drop")))
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HE_ENTITY"))
                {
                    Entity dragged{};
                    std::memcpy(&dragged, payload->Data, sizeof(Entity));
                    if (ctx.undoSys) ctx.undoSys->snapshotNow();
                    ctx.world->reparentEntity(dragged, ctx.world->rootEntity());
                }
                ImGui::EndDragDropTarget();
            }
        }

        // ── Background context menu: create entity at root level ──────────
        if (ImGui::BeginPopupContextWindow("##outliner_bg_ctx",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        {
            if (ImGui::MenuItem("Create Entity"))
            {
                if (ctx.undoSys) ctx.undoSys->snapshotNow();
                Entity e = ctx.world->createEntity("Entity");
                ctx.world->addComponent(e, TransformComponent{});
                ctx.selectedEntity = e;
            }
            ImGui::EndPopup();
        }

        // ── Entity rename popup ────────────────────────────────────────────
        if (s_openEntityRename)
        {
            ImGui::OpenPopup("##entity_rename_popup");
            s_openEntityRename = false;
        }
        ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_Always);
        if (ImGui::BeginPopupModal("##entity_rename_popup", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
        {
            ImGui::TextUnformatted("Rename Entity");
            ImGui::Separator();
            ImGui::SetNextItemWidth(-1.0f);
            bool confirm = ImGui::InputText("##entity_rename_input",
                s_entityRenameBuf, sizeof(s_entityRenameBuf),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            if (ImGui::Button("OK", ImVec2(140, 0)) || confirm)
            {
                if (s_entityRenameBuf[0] != '\0' &&
                    ctx.world->registry().valid(s_renameEntity))
                {
                    if (ctx.undoSys) ctx.undoSys->snapshotNow();
                    ctx.world->renameEntity(s_renameEntity, s_entityRenameBuf);
                }
                s_renameEntity = entt::null;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(140, 0)))
            {
                s_renameEntity = entt::null;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    else
    {
        ImGui::TextDisabled("(no world loaded)");
    }

    ImGui::End();

    RenderInspector(ctx);

    DrawPreferencesWindow(ctx, s_showPreferences);
    DrawProfilerWindow(ctx, s_showProfiler);
    // Level Script + Game Instance now render as editor tabs (see the tab dispatch).

    //Content Browser
	auto [contentFolder, contentLock] = ctx.globalState->lockContentFolder();
    if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
    ImGui::Begin("Content Browser", nullptr, ImGuiWindowFlags_NoTitleBar);
    if (ctx.fontHeading) ImGui::PopFont();

    {
        const float totalWidth    = ImGui::GetContentRegionAvail().x;
        const float totalHeight   = ImGui::GetContentRegionAvail().y;
        const float splitterW     = 6.0f;
        const float minPaneWidth  = 60.0f;

        if (ctx.cbTreeWidth < 0.0f)
            ctx.cbTreeWidth = totalWidth * 0.25f;

        ctx.cbTreeWidth = std::clamp(ctx.cbTreeWidth, minPaneWidth, totalWidth - minPaneWidth - splitterW);

		ImGui::BeginChild("##cb_tree", ImVec2(ctx.cbTreeWidth, totalHeight), false);

		if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
		ImGui::Text("Content");
		if (ctx.fontSubheading) ImGui::PopFont();
		ImGui::Separator();

		// ── Tree: single-click = expand/collapse, double-click = navigate ──
		static const Folder* s_selectedTreeFolder = nullptr;

		std::function<void(const Folder*, int)> renderTree = [&](const Folder* folder, int depth)
		{
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
									 | ImGuiTreeNodeFlags_SpanAvailWidth;
			if (folder->subfolders.empty())
				flags |= ImGuiTreeNodeFlags_Leaf;

			bool open = ImGui::TreeNodeEx(folder->name.c_str(), flags);

			// Double-click anywhere on the item → navigate grid to this folder
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				s_selectedTreeFolder = folder;

			if (open)
			{
				for (auto* sub : folder->subfolders)
					renderTree(sub, depth + 1);
				ImGui::TreePop();
			}
		};

		// Root "Content" node
		{
			ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow
										 | ImGuiTreeNodeFlags_DefaultOpen
										 | ImGuiTreeNodeFlags_SpanAvailWidth;
			bool rootOpen = ImGui::TreeNodeEx("Content", rootFlags);
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				s_selectedTreeFolder = nullptr; // back to root
			if (rootOpen)
			{
				for (auto* sub : contentFolder.subfolders)
					renderTree(sub, 1);
				ImGui::TreePop();
			}
		}

		ImGui::EndChild();
		ImGui::SameLine();

		ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.40f, 0.40f, 0.40f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

		ImGui::Button("##cb_splitter", ImVec2(splitterW, totalHeight));

		if (ImGui::IsItemHovered() || ImGui::IsItemActive())
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

		if (ImGui::IsItemActive())
			ctx.cbTreeWidth += ImGui::GetIO().MouseDelta.x;

		ImGui::PopStyleVar();
		ImGui::PopStyleColor(3);
		ImGui::SameLine();

		ImGui::BeginChild("##cb_content", ImVec2(0.0f, totalHeight), false);

		// ── Determine which folder's content to show ──────────────────────
		// s_selectedTreeFolder nullptr → root; otherwise the selected sub-folder
		static const Folder* s_gridFolder = nullptr;

		// refreshContentFolder() (asset create/rename/delete, project load) deletes
		// and rebuilds every Folder node, so the navigation statics above dangle
		// after each refresh — dereferencing them was a use-after-free crash when
		// creating an asset inside a sub-folder. Re-resolve the remembered path in
		// the fresh tree; if the folder no longer exists, fall back to the root.
		static uint64_t    s_treeVersionSeen = ~0ull;
		static std::string s_gridFolderPath;
		const uint64_t treeVersion =
			ctx.globalState ? ctx.globalState->contentFolderVersion.load(std::memory_order_acquire) : 0;
		if (treeVersion != s_treeVersionSeen)
		{
			s_treeVersionSeen = treeVersion;
			const Folder* fresh = nullptr;
			if (!s_gridFolderPath.empty())
			{
				std::function<const Folder*(const Folder*)> findByPath =
					[&](const Folder* cur) -> const Folder*
				{
					if (cur->fullPath == s_gridFolderPath) return cur;
					for (const Folder* sub : cur->subfolders)
						if (const Folder* hit = findByPath(sub)) return hit;
					return nullptr;
				};
				fresh = findByPath(&contentFolder);
				if (fresh == &contentFolder) fresh = nullptr; // root is the null state
			}
			s_gridFolder         = fresh;
			s_selectedTreeFolder = fresh;
		}

		// Sync from tree double-click
		if (s_selectedTreeFolder != s_gridFolder)
			s_gridFolder = s_selectedTreeFolder;

		const Folder* displayFolder = s_gridFolder ? s_gridFolder : &contentFolder;
		s_gridFolderPath = s_gridFolder ? s_gridFolder->fullPath : std::string{};

		// ── Breadcrumb ────────────────────────────────────────────────────
		if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
		ImGui::Text("Assets");
		if (ctx.fontSubheading) ImGui::PopFont();

		// Simple breadcrumb: root > ... > current folder name
		{
			ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f,0.3f,0.3f,0.5f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.4f,0.4f,0.4f,0.7f));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 1));

			// Build ancestor chain via DFS
			std::vector<const Folder*> crumbs;
			if (s_gridFolder)
			{
				std::function<bool(const Folder*)> findPath = [&](const Folder* cur) -> bool
				{
					if (cur == s_gridFolder) return true;
					for (auto* sub : cur->subfolders)
					{
						crumbs.push_back(sub);
						if (findPath(sub)) return true;
						crumbs.pop_back();
					}
					return false;
				};
				findPath(&contentFolder);
			}

			if (ImGui::SmallButton("Content##bc_root"))
			{
				s_gridFolder         = nullptr;
				s_selectedTreeFolder = nullptr;
			}

			for (int ci = 0; ci < static_cast<int>(crumbs.size()); ++ci)
			{
				ImGui::SameLine(0, 2);
				ImGui::TextDisabled(">");
				ImGui::SameLine(0, 2);
				const Folder* crumb = crumbs[ci];
				bool isLast = (ci == static_cast<int>(crumbs.size()) - 1);
				if (isLast)
				{
					// Current folder – shown as a dimmed button (no navigation)
					ImGui::BeginDisabled();
					ImGui::SmallButton(crumb->name.c_str());
					ImGui::EndDisabled();
				}
				else
				{
					std::string btnId = crumb->name + "##bc_" + std::to_string(ci);
					if (ImGui::SmallButton(btnId.c_str()))
					{
						s_gridFolder         = crumb;
						s_selectedTreeFolder = crumb;
					}
				}
			}

			ImGui::PopStyleVar();
			ImGui::PopStyleColor(3);
		}

		ImGui::Separator();

		// ── Grid ──────────────────────────────────────────────────────────
		constexpr float k_cellSize    = 72.0f;
		constexpr float k_iconPad     = 6.0f;   // padding inside ImageButton
		constexpr float k_iconSize    = k_cellSize - k_iconPad * 2.0f;
		constexpr float k_labelHeight = 16.0f;
		constexpr float k_padding     = 8.0f;
		constexpr float k_itemSize    = k_cellSize + k_labelHeight + k_padding;

		// Helper: pick an asset icon based on file extension
		auto pickAssetIcon = [&](const std::string& ext) -> ImTextureID
		{
			std::string e = ext;
			for (auto& c : e) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
			if (e == ".mat")                                   return ctx.cbIcons.material;
			if (e == ".obj" || e == ".fbx" || e == ".gltf"
				|| e == ".glb" || e == ".dae")                 return ctx.cbIcons.model3d;
			if (e == ".svg" || e == ".ai")                     return ctx.cbIcons.model2d;
			if (e == ".cs"  || e == ".lua" || e == ".py"
				|| e == ".js")                                  return ctx.cbIcons.script;
			if (e == ".wav" || e == ".mp3" || e == ".ogg"
				|| e == ".flac")                                return ctx.cbIcons.sound;
			if (e == ".png" || e == ".jpg" || e == ".jpeg"
					|| e == ".bmp" || e == ".tga" || e == ".hdr")  return ctx.cbIcons.texture;
				if (e == ".hescene")                               return ctx.cbIcons.scene;
				return 0;
		};

		const float availW     = ImGui::GetContentRegionAvail().x;
		const int   columns    = (std::max)(1, static_cast<int>(availW / k_itemSize));

		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(k_padding, k_padding));

		int col = 0;

		// ── Shared selection/rename/context state ────────────────────────
		static std::string s_selectedItem;
		static bool        s_selectedIsFolder = false;
		static std::string s_ctxMenuItem;
		static bool        s_ctxMenuIsFolder  = false;
		static std::string s_renameTarget;
		static bool        s_renameIsFolder   = false;
		static char        s_renameBuf[256]   = {};
		static bool        s_openRenamePopup  = false;
		static bool        s_renameIsCreate   = false; // naming a freshly created item
		static int         s_renameScriptLang = -1;    // creating a script: 0=Lua 1=Python; -1=not a script
		static bool        s_rightClickOnItem = false;

		// ── Folders first ─────────────────────────────────────────────────
		for (auto* sub : displayFolder->subfolders)
		{
			if (col > 0 && col < columns) ImGui::SameLine();
			if (col >= columns) col = 0;

			const bool isSel = (s_selectedItem == sub->fullPath);

			ImGui::BeginGroup();
			ImGui::PushID(sub->fullPath.c_str());

			if (isSel)
			{
				ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.26f, 0.46f, 0.78f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.54f, 0.90f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.20f, 0.38f, 0.68f, 1.0f));
			}
			else
			{
				ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.28f, 0.28f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.38f, 0.38f, 0.38f, 1.0f));
			}
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(k_iconPad, k_iconPad));

			if (ctx.cbIcons.folder)
			{
				ImGui::ImageButton("##icon", ctx.cbIcons.folder,
					ImVec2(k_iconSize, k_iconSize),
					ImVec2(0,0), ImVec2(1,1),
					ImVec4(0,0,0,0),
					ImVec4(0.96f, 0.78f, 0.26f, 1.0f));
			}
			else
			{
				ImGui::Button("##icon", ImVec2(k_cellSize, k_cellSize));
			}

			// Left click → select
			if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				s_selectedItem     = sub->fullPath;
				s_selectedIsFolder = true;
			}
			// Double-click → navigate into folder
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				s_gridFolder         = sub;
				s_selectedTreeFolder = sub;
			}
			// Right click → select only, open menu after loop
			if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
			{
				s_selectedItem     = sub->fullPath;
				s_selectedIsFolder = true;
				s_ctxMenuItem      = sub->fullPath;
				s_ctxMenuIsFolder  = true;
				s_rightClickOnItem = true;
			}

			ImGui::PopStyleVar();
			ImGui::PopStyleColor(3);
			ImGui::PopID();

			// Centered label (truncated)
			const float labelW = k_cellSize;
			std::string label  = sub->name;
			if (ImGui::CalcTextSize(label.c_str()).x > labelW)
			{
				while (!label.empty() &&
					   ImGui::CalcTextSize((label + "...").c_str()).x > labelW)
					label.pop_back();
				label += "...";
			}
			float textOff = (labelW - ImGui::CalcTextSize(label.c_str()).x) * 0.5f;
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + textOff);
			if (isSel)
				ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "%s", label.c_str());
			else
				ImGui::TextUnformatted(label.c_str());

			ImGui::EndGroup();
			++col;
		}

		// ── Files ─────────────────────────────────────────────────────────
		for (auto* file : displayFolder->files)
		{
			if (col > 0 && col < columns) ImGui::SameLine();
			if (col >= columns) col = 0;

			const bool isSel = (s_selectedItem == file->fullPath);

			ImGui::BeginGroup();
			ImGui::PushID(file->fullPath.c_str());

			if (isSel)
			{
				ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.26f, 0.46f, 0.78f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.54f, 0.90f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.20f, 0.38f, 0.68f, 1.0f));
			}
			else
			{
				ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.28f, 0.28f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.38f, 0.38f, 0.38f, 1.0f));
			}
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(k_iconPad, k_iconPad));

			ImTextureID assetIcon = pickAssetIcon(file->extension);
			if (assetIcon)
			{
				ImVec4 tint{0.75f, 0.85f, 1.0f, 1.0f};
				if (assetIcon == ctx.cbIcons.material) tint = {0.60f, 0.90f, 0.60f, 1.0f};
				if (assetIcon == ctx.cbIcons.model3d)  tint = {0.70f, 0.80f, 1.00f, 1.0f};
				if (assetIcon == ctx.cbIcons.model2d)  tint = {0.80f, 0.70f, 1.00f, 1.0f};
				if (assetIcon == ctx.cbIcons.script)   tint = {0.90f, 0.90f, 0.50f, 1.0f};
				if (assetIcon == ctx.cbIcons.sound)    tint = {0.60f, 0.90f, 0.90f, 1.0f};
				if (assetIcon == ctx.cbIcons.texture)  tint = {0.90f, 0.75f, 0.60f, 1.0f};
				if (assetIcon == ctx.cbIcons.scene)    tint = {0.75f, 0.65f, 1.00f, 1.0f};

				ImGui::ImageButton("##icon", assetIcon,
					ImVec2(k_iconSize, k_iconSize),
					ImVec2(0,0), ImVec2(1,1),
					ImVec4(0,0,0,0), tint);
			}
			else
			{
				ImGui::Button("##icon", ImVec2(k_cellSize, k_cellSize));
			}

			ImGui::PopStyleVar();
			ImGui::PopStyleColor(3);

			// Drag source — carries the asset's absolute path so drop targets
			// (e.g. the Material slot in the inspector) can load it.
			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
			{
				ImGui::SetDragDropPayload("HE_ASSET_PATH",
					file->fullPath.c_str(), file->fullPath.size() + 1);
				ImGui::TextUnformatted(
					std::filesystem::path(file->name).stem().string().c_str());
				ImGui::EndDragDropSource();
			}

			// Left click → select
			if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				s_selectedItem     = file->fullPath;
				s_selectedIsFolder = false;
			}
			// Double-click → open tab
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				if (std::filesystem::path(file->fullPath).extension() == ".hescene")
				{
					requestGuarded(GuardedAction::OpenScenePath, file->fullPath);
				}
				// Script assets open the code editor tab, material assets the node-graph
				// editor tab. Other asset types have no dedicated editor yet → no-op.
				else if (ScriptEditorPanel::isScriptAsset(file->fullPath) ||
				         MaterialEditorPanel::isMaterialAsset(file->fullPath) ||
				         MaterialEditorPanel::isMaterialFunctionAsset(file->fullPath) ||
				         UIEditorPanel::isWidgetAsset(file->fullPath) ||
				         HorizonCodeClassPanel::isClassAsset(file->fullPath))
				{
				const std::string tabLabel = std::filesystem::path(file->name).stem().string();
				auto it = std::find_if(ctx.tabs.begin(), ctx.tabs.end(),
					[&](const AppContext::EditorTab& t){ return t.assetPath == file->fullPath; });
				if (it == ctx.tabs.end())
				{
					ctx.tabs.push_back({ tabLabel, file->fullPath, true, true });
					ctx.activeTab = static_cast<int>(ctx.tabs.size()) - 1;
				}
				else
				{
					ctx.activeTab = static_cast<int>(std::distance(ctx.tabs.begin(), it));
				}
					// Force the tab bar to select this tab next frame (else ImGui keeps the
					// Scene tab selected and the editor never opens).
					s_tabSelectRequest = ctx.activeTab;
				}
			}
			// Right click → select only, open menu after loop
			if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
			{
				s_selectedItem     = file->fullPath;
				s_selectedIsFolder = false;
				s_ctxMenuItem      = file->fullPath;
				s_ctxMenuIsFolder  = false;
				s_rightClickOnItem = true;
			}

			ImGui::PopID();

			// Centered label (stem only, truncated)
			const float labelW = k_cellSize;
			std::string label  = std::filesystem::path(file->name).stem().string();
			if (ImGui::CalcTextSize(label.c_str()).x > labelW)
			{
				while (!label.empty() &&
					   ImGui::CalcTextSize((label + "...").c_str()).x > labelW)
					label.pop_back();
				label += "...";
			}
			float textOff = (labelW - ImGui::CalcTextSize(label.c_str()).x) * 0.5f;
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + textOff);
			if (isSel)
				ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "%s", label.c_str());
			else
				ImGui::TextUnformatted(label.c_str());

			ImGui::EndGroup();
			++col;
		}

		ImGui::PopStyleVar();

		// ── Open item context menu after loops ────────────────────────────
		// ── Shared "Create Asset" menu body ─────────────────────────────────
		// Used by BOTH the background right-click popup and the item context
		// menu's Create submenu, so creating an asset works wherever the user
		// right-clicks in the Content Browser.
		auto drawCreateAssetItems = [&](const std::string& targetFolder)
		{
			auto tryCreate = [&](const char* defaultName, const char* ext, HE::AssetType type,
			                     ScriptLanguage scriptLang = ScriptLanguage::Lua)
			{
				// Build a path that does not yet exist
				std::string base = targetFolder + "/" + defaultName;
				std::string path = base + ext;
				int counter = 1;
				while (std::filesystem::exists(path))
					path = base + std::to_string(counter++) + ext;

				// Create an empty asset file via ContentManager
				std::string relative = std::filesystem::relative(
					path,
					std::filesystem::path(ctx.projectManager->currentProject().path).parent_path()
				).string();
				std::replace(relative.begin(), relative.end(), '\\', '/');

				// Write a minimal binary asset stub so the file exists on disk.
				// The UUID minted here is the asset's permanent identity.
				{
					const HE::UUID assetId = HE::UUID::generate();
					HAsset::Writer w;
					std::vector<uint8_t> meta;
					HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(type));
					HAsset::Writer::appendPOD(meta, assetId.hi);
					HAsset::Writer::appendPOD(meta, assetId.lo);
					HAsset::Writer::appendString(meta, defaultName);
					HAsset::Writer::appendString(meta, relative);
					w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
					// Scripts are born with a language and a starter template. The
					// language byte (CHUNK_SLNG) is the single source of truth for
					// routing Lua vs Python, so it must be written here at birth —
					// this stub bypasses the ContentManager save path.
					if (type == HE::AssetType::Script)
					{
						const int lang = static_cast<int>(scriptLang);
						const char* starter = scriptStarterTemplate(lang);
						w.addChunk(HAsset::CHUNK_SRC, starter, std::char_traits<char>::length(starter));
						const uint8_t lb = static_cast<uint8_t>(lang);
						w.addChunk(HAsset::CHUNK_SLNG, &lb, 1);
					}
					// UI widgets are born with an empty 1920×1080 tree so the widget
					// editor has valid JSON to open straight away.
					if (type == HE::AssetType::Widget)
					{
						const std::string tree = HE::uiWidgetTreeToJson(HE::UIWidgetTree{});
						w.addChunk(HAsset::CHUNK_UIWT, tree.data(), tree.size());
					}
					if (type == HE::AssetType::HorizonCodeClass)
					{
						const std::string graph = HorizonCode::toJson(HorizonCode::Graph{});
						w.addChunk(HAsset::CHUNK_HCGR, graph.data(), graph.size());
					}
					w.write(path, static_cast<uint16_t>(type));
				}

				// Show it now (don't wait for the next auto-refresh) and let the
				// user name it straight away via the rename/name dialog.
				s_selectedItem    = path;
				s_renameTarget    = path;
				s_renameIsFolder  = false;
				s_renameIsCreate  = true;
				s_renameScriptLang = (type == HE::AssetType::Script) ? static_cast<int>(scriptLang) : -1;
				std::strncpy(s_renameBuf, defaultName, sizeof(s_renameBuf) - 1);
				s_renameBuf[sizeof(s_renameBuf) - 1] = '\0';
				s_openRenamePopup = true;
				s_quietContentRefresh = true;
				ImGui::CloseCurrentPopup();
			};

			if (ImGui::MenuItem("Scene"))        tryCreate("NewScene",    ".hescene", HE::AssetType::Scene);
			if (ImGui::MenuItem("Material"))     tryCreate("NewMaterial", ".hasset",  HE::AssetType::Material);
			if (ImGui::MenuItem("Material Function")) tryCreate("NewMaterialFunction", ".hasset", HE::AssetType::MaterialFunction);
			if (ImGui::MenuItem("UI Widget"))    tryCreate("NewWidget",   ".hasset",  HE::AssetType::Widget);
			if (ImGui::MenuItem("HorizonCode Class")) tryCreate("NewClass", ".hasset", HE::AssetType::HorizonCodeClass);
			if (ImGui::MenuItem("Texture"))      tryCreate("NewTexture",  ".hasset",  HE::AssetType::Texture);
			if (ImGui::MenuItem("Static Mesh"))  tryCreate("NewMesh",     ".hasset",  HE::AssetType::StaticMesh);
			if (ImGui::MenuItem("Skeletal Mesh"))tryCreate("NewSkelMesh", ".hasset",  HE::AssetType::SkeletalMesh);
			if (ImGui::MenuItem("Script"))       tryCreate("NewScript",   ".hasset",  HE::AssetType::Script, ScriptLanguage::Lua);
			if (ImGui::MenuItem("Shader"))       tryCreate("NewShader",   ".hasset",  HE::AssetType::Shader);
			if (ImGui::MenuItem("Audio"))        tryCreate("NewAudio",    ".hasset",  HE::AssetType::Audio);
			if (ImGui::MenuItem("Font"))         tryCreate("NewFont",     ".hasset",  HE::AssetType::Font);
		};

		if (s_rightClickOnItem)
		{
			ImGui::OpenPopup("##cb_item_ctx");
			s_rightClickOnItem = false;
		}

		// ── Item context menu (folder + file) ─────────────────────────────
		if (ImGui::BeginPopup("##cb_item_ctx"))
		{
			std::string displayName = s_ctxMenuIsFolder
				? std::filesystem::path(s_ctxMenuItem).filename().string()
				: std::filesystem::path(s_ctxMenuItem).stem().string();
			ImGui::TextDisabled("%s", displayName.c_str());
			ImGui::Separator();

			// ── Import source file → .hasset ─────────────────────────────
			if (!s_ctxMenuIsFolder)
			{
				const std::filesystem::path srcPath(s_ctxMenuItem);
				std::string ext = srcPath.extension().string();
				for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));

				const bool isMeshSrc    = (ext == ".gltf" || ext == ".glb");
				const bool isTextureSrc = (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
				                           ext == ".tga" || ext == ".bmp" || ext == ".hdr");
				const bool isAudioSrc   = (ext == ".wav");
				const bool isMatSrc     = (ext == ".hmat");
				const bool isFontSrc    = (ext == ".ttf" || ext == ".otf");

				if ((isMeshSrc || isTextureSrc || isAudioSrc || isMatSrc || isFontSrc) &&
				    ImGui::MenuItem("Import"))
				{
					const std::filesystem::path root = contentFolder.fullPath;
					std::error_code ec;
					std::filesystem::path relDir =
						std::filesystem::relative(srcPath.parent_path(), root, ec);
					if (ec || relDir == ".") relDir.clear();

					bool ok = false;
					if      (isMeshSrc)    ok = MeshImporter::import(srcPath, root, relDir)     != nullptr;
					else if (isTextureSrc) ok = TextureImporter::import(srcPath, root, relDir)  != nullptr;
					else if (isAudioSrc)   ok = AudioImporter::import(srcPath, root, relDir)    != nullptr;
					else if (isMatSrc)     ok = MaterialImporter::import(srcPath, root, relDir) != nullptr;
					else if (isFontSrc)    ok = FontImporter::import(srcPath, root, relDir)     != nullptr;

					if (!ok)
						Logger::Log(Logger::LogLevel::Error,
							("Editor: import failed for " + srcPath.string()).c_str());
					ctx.contentRefreshPending = true;
					ImGui::CloseCurrentPopup();
				}

				// ── Material → create a child INSTANCE (params/switches only) ──
				if (ext == ".hasset" && ctx.contentManager &&
				    MaterialEditorPanel::isMaterialAsset(s_ctxMenuItem) &&
				    ImGui::MenuItem("Create Material Instance"))
				{
					std::error_code ec;
					const std::filesystem::path root(contentFolder.fullPath);
					const std::string parentRel =
						std::filesystem::relative(srcPath, root, ec).generic_string();
					if (!ec)
					{
						// Unique sibling: <stem>_Inst[.N].hasset
						std::filesystem::path dst =
							srcPath.parent_path() / (srcPath.stem().string() + "_Inst.hasset");
						for (int k = 2; std::filesystem::exists(dst) && k < 100; ++k)
							dst = srcPath.parent_path() /
								(srcPath.stem().string() + "_Inst" + std::to_string(k) + ".hasset");
						MaterialAsset inst;
						inst.type = HE::AssetType::Material;
						inst.name = dst.stem().string();
						inst.path = std::filesystem::relative(dst, root, ec).generic_string();
						inst.parentMaterialPath = parentRel;
						const HE::UUID iid = ctx.contentManager->registerMaterial(std::move(inst));
						ctx.contentManager->syncMaterialInstance(iid); // derive shader/params
						if (MaterialAsset* mi = ctx.contentManager->getMaterialMutable(iid))
							ctx.contentManager->saveAsset(*mi);
						ctx.contentRefreshPending = true;
						Logger::Log(Logger::LogLevel::Info,
							("Editor: created material instance of '" + parentRel + "'").c_str());
					}
					ImGui::CloseCurrentPopup();
				}

				// ── Add a StaticMesh .hasset to the scene ─────────────────
				if (ext == ".hasset" && ctx.world && ctx.contentManager &&
				    ImGui::MenuItem("Add to Scene"))
				{
					std::error_code ec;
					std::string rel = std::filesystem::relative(
						srcPath, std::filesystem::path(contentFolder.fullPath), ec).generic_string();
					if (!ec)
					{
						const HE::UUID id = ctx.contentManager->loadAsset(rel);
						if (const StaticMeshAsset* mesh = ctx.contentManager->getStaticMesh(id))
						{
							if (ctx.undoSys) ctx.undoSys->snapshotNow();
							Entity e = ctx.world->createEntity(mesh->name);
							ctx.world->addComponent(e, TransformComponent{});
							ctx.world->addComponent(e, MeshComponent{ .meshAssetId = id });
							ctx.world->markHierarchyDirty();
							Logger::Log(Logger::LogLevel::Info,
								("Editor: added '" + mesh->name + "' to scene").c_str());
						}
						else
							Logger::Log(Logger::LogLevel::Warning,
								("Editor: " + rel + " is not a loadable static mesh").c_str());
					}
					ImGui::CloseCurrentPopup();
				}
			}

			if (ImGui::MenuItem("Rename"))
			{
				s_renameTarget   = s_ctxMenuItem;
				s_renameIsFolder = s_ctxMenuIsFolder;
				s_renameIsCreate = false;
				s_renameScriptLang = -1;
				std::strncpy(s_renameBuf, displayName.c_str(), sizeof(s_renameBuf) - 1);
				s_renameBuf[sizeof(s_renameBuf) - 1] = '\0';
				s_openRenamePopup = true;
				ImGui::CloseCurrentPopup();
			}
			if (!s_ctxMenuIsFolder && ImGui::MenuItem("Delete"))
			{
				std::error_code ec;
				std::filesystem::remove(s_ctxMenuItem, ec);
				if (s_selectedItem == s_ctxMenuItem)
					s_selectedItem.clear();
				s_ctxMenuItem.clear();
				ctx.contentRefreshPending = true;
				ImGui::CloseCurrentPopup();
			}

			ImGui::Separator();
			if (ImGui::BeginMenu("Create Asset"))
			{
				const std::string createDir = s_ctxMenuIsFolder
					? s_ctxMenuItem
					: std::filesystem::path(s_ctxMenuItem).parent_path().string();
				drawCreateAssetItems(createDir);
				ImGui::EndMenu();
			}

			ImGui::EndPopup();
		}

		// Trigger rename popup outside item context popup. Gated on the content
		// refresh having settled so it never competes with the ##ContentRefresh
		// modal (e.g. right after a create, which requests both).
		if (s_openRenamePopup && !ctx.contentRefreshPending && !ctx.contentRefreshDone)
		{
			ImGui::OpenPopup("##cb_rename_popup");
			s_openRenamePopup = false;
		}

		// ── Rename / name-on-create popup ─────────────────────────────────
		ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_Always);
		if (ImGui::BeginPopupModal("##cb_rename_popup", nullptr,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		{
			const char* verb = s_renameIsCreate ? "Name" : "Rename";
			ImGui::Text("%s %s", verb, s_renameIsFolder ? "Folder" : "Asset");
			ImGui::Separator();
			ImGui::Spacing();

			// Focus the field as the dialog opens so the user can type at once.
			if (ImGui::IsWindowAppearing())
				ImGui::SetKeyboardFocusHere();
			ImGui::SetNextItemWidth(-1.0f);
			bool confirm = ImGui::InputText("##rename_input", s_renameBuf, sizeof(s_renameBuf),
				ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
			ImGui::Spacing();

			// Language picker for a script being created — flipping it rewrites the
			// stub starter + language byte in place (the UUID in META is kept).
			if (s_renameScriptLang >= 0)
			{
				ImGui::TextUnformatted("Language"); ImGui::SameLine();
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::Combo("##script_lang", &s_renameScriptLang, "Lua\0Python\0"))
					rewriteScriptStubLanguage(s_renameTarget, s_renameScriptLang);
				ImGui::Spacing();
			}

			if (ImGui::Button("OK", ImVec2(140, 0)) || confirm)
			{
				std::string newName(s_renameBuf);
				if (!newName.empty() && !s_renameTarget.empty())
				{
					std::filesystem::path oldPath(s_renameTarget);
					std::filesystem::path newPath;
					if (s_renameIsFolder)
						newPath = oldPath.parent_path() / newName;
					else
						newPath = oldPath.parent_path() / (newName + oldPath.extension().string());
					std::error_code ec;
					std::filesystem::rename(oldPath, newPath, ec);
					if (!ec)
					{
						if (s_selectedItem == s_renameTarget)
							s_selectedItem = newPath.string();
						if (!s_renameIsFolder)
						{
							for (auto& t : ctx.tabs)
								if (t.assetPath == s_renameTarget)
								{
									t.assetPath = newPath.string();
									t.label     = newName;
								}
						}
						s_quietContentRefresh = true;
					}
				}
				s_renameTarget.clear();
				s_renameIsCreate = false;
				s_renameScriptLang = -1;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(140, 0)))
			{
				// On create, Cancel just keeps the default name — the file already
				// exists on disk; nothing to undo.
				s_renameTarget.clear();
				s_renameIsCreate = false;
				s_renameScriptLang = -1;
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}

		// ── Background left-click → clear selection ───────────────────────
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
			ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
			!ImGui::IsAnyItemHovered())
		{
			s_selectedItem.clear();
		}

		// ── Background right-click context menu
		// Only open when clicking on the panel background (not on any item)
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
			ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
			!ImGui::IsAnyItemHovered())
		{
			ImGui::OpenPopup("##cb_create_ctx");
		}

		if (ImGui::BeginPopup("##cb_create_ctx"))
		{
			ImGui::TextDisabled("Create Asset");
			ImGui::Separator();

			const std::string targetFolder = displayFolder ? displayFolder->fullPath
														   : contentFolder.fullPath;
			drawCreateAssetItems(targetFolder);
			if (ImGui::MenuItem("Folder"))
			{
				std::string base = targetFolder + "/NewFolder";
				std::string dir  = base;
				int counter = 1;
				while (std::filesystem::exists(dir))
					dir = base + std::to_string(counter++);
				std::filesystem::create_directory(dir);

				// Show it now and let the user name it straight away.
				const std::string folderName = std::filesystem::path(dir).filename().string();
				s_selectedItem    = dir;
				s_renameTarget    = dir;
				s_renameIsFolder  = true;
				s_renameIsCreate  = true;
				std::strncpy(s_renameBuf, folderName.c_str(), sizeof(s_renameBuf) - 1);
				s_renameBuf[sizeof(s_renameBuf) - 1] = '\0';
				s_openRenamePopup = true;
				s_quietContentRefresh = true;
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}

		ImGui::EndChild();
    }

    ImGui::End();
#endif // HE_IMGUI_ENABLED
}

// ─── Inspector (Details panel) ────────────────────────────────────────────────
void EditorUI::RenderInspector(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	if (ctx.fontHeading) ImGui::PushFont(ctx.fontHeading);
	ImGui::Begin("Details");
	if (ctx.fontHeading) ImGui::PopFont();

	if (!ctx.world || ctx.selectedEntity == entt::null ||
	    !ctx.world->registry().valid(ctx.selectedEntity))
	{
		ImGui::TextDisabled("(no entity selected)");
		ImGui::End();
		return;
	}

	auto&  registry = ctx.world->registry();
	Entity entity   = ctx.selectedEntity;

	// Pre-frame world state for undo. capturePre() serializes the WHOLE world, so it
	// must NOT run every frame — doing so dropped the editor to ~15 ms the instant any
	// entity was selected (the terrain's sculptHeights alone is 263k floats). An edit
	// can only START on a mouse press inside this panel, so capture the pre-state only
	// then; the widget's IsItemActivated (same frame) stashes it.
	if (ctx.undoSys
	    && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)
	    && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		ctx.undoSys->capturePre();
	auto trackEdit = [&]
	{
		if (!ctx.undoSys) return;
		if (ImGui::IsItemActivated())            ctx.undoSys->stashPre();
		if (ImGui::IsItemDeactivatedAfterEdit()) ctx.undoSys->commitPending();
	};

	// ── Name ────────────────────────────────────────────────────────────────
	if (auto* name = registry.try_get<NameComponent>(entity))
	{
		char buf[256];
		std::strncpy(buf, name->name.c_str(), sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::InputText("##entity_name", buf, sizeof(buf),
		                     ImGuiInputTextFlags_EnterReturnsTrue))
		{
			if (ctx.undoSys) ctx.undoSys->snapshotNow();
			ctx.world->renameEntity(entity, buf);
		}
	}
	ImGui::Separator();

	// ── Environment (scene-wide sky settings, on the World root entity) ──────
	// Edited here so it persists with the scene; pushed to the renderer each frame
	// by EditorApplication::pushEnvironment.
	if (auto* env = registry.try_get<EnvironmentComponent>(entity))
	{
		if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Checkbox("Day-Night Cycle", &env->dayNightCycle); trackEdit();

			// Format the 0..1 time as a HH:MM clock shown inside the slider.
			int minutes = static_cast<int>(env->timeOfDay * 1440.0f) % 1440;
			if (minutes < 0) minutes += 1440;
			char clock[8];
			std::snprintf(clock, sizeof(clock), "%02d:%02d", minutes / 60, minutes % 60);
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::SliderFloat("##timeofday", &env->timeOfDay, 0.0f, 1.0f, clock,
			                       ImGuiSliderFlags_NoRoundToFormat))
				env->dayNightCycle = true;
			trackEdit();
			ImGui::TextDisabled(env->dayNightCycle
				? "Drives the sun, sky & shadows."
				: "Move the slider to start a day-night cycle.");

			if (ImGui::Checkbox("Auto-Advance", &env->autoAdvance) && env->autoAdvance)
				env->dayNightCycle = true;
			trackEdit();
			ImGui::BeginDisabled(!env->autoAdvance);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##cyclelen", &env->cycleSeconds, 5.0f, 600.0f,
			                   "Full day: %.0f s", ImGuiSliderFlags_Logarithmic); trackEdit();
			ImGui::EndDisabled();

			if (ImGui::TreeNodeEx("Sun & Moon", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::ColorEdit3("Sun Color",  &env->sunColor.x, ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::SliderFloat("Sun Brightness",  &env->sunIntensity,  0.0f, 10.0f, "%.2f"); trackEdit();
			ImGui::ColorEdit3("Moon Color", &env->moonColor.x, ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::SliderFloat("Moon Brightness", &env->moonIntensity, 0.0f, 10.0f, "%.2f"); trackEdit();

			ImGui::SeparatorText("Moon Phase");
			{
				float mp = env->moonPhase;
				const char* nm = (mp < 0.03f || mp > 0.97f) ? "New Moon" :
				                 mp < 0.22f ? "Waxing Crescent" :
				                 mp < 0.28f ? "First Quarter" :
				                 mp < 0.47f ? "Waxing Gibbous" :
				                 mp < 0.53f ? "Full Moon" :
				                 mp < 0.72f ? "Waning Gibbous" :
				                 mp < 0.78f ? "Last Quarter" : "Waning Crescent";
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::SliderFloat("##moonphase", &env->moonPhase, 0.0f, 1.0f, "Phase: %.3f")) trackEdit();
				ImGui::TextDisabled("%s", nm);
				if (ImGui::Checkbox("Auto Lunar Cycle", &env->moonPhaseAuto)) trackEdit();
				ImGui::SameLine(); ImGui::TextDisabled("(needs Auto-Advance)");
				ImGui::BeginDisabled(!env->moonPhaseAuto);
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::SliderFloat("##mooncycledays", &env->moonCycleDays, 1.0f, 60.0f, "Lunar cycle: %.1f days")) trackEdit();
				ImGui::EndDisabled();
			}

			ImGui::TreePop(); } // end Sun & Moon

			// These are always editable. A Weather preset (below) sets a whole set of
			// these values when applied / transitioning; otherwise they're yours to move.
			if (ImGui::TreeNodeEx("Clouds", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##cloudcoverage", &env->cloudCoverage, 0.0f, 1.0f, "Coverage: %.2f"); trackEdit();
			ImGui::TextDisabled("Full overcast dims the sun & fills with ambient light.");
			// Cloud render mode (OpenGL backend): sky-dome (cheap, infinite — no parallax)
			// vs 3D volumetric (world-anchored — clouds parallax as you move through the
			// scene). 3D exposes a height slider to match the world's unit scale.
			{
				const char* cloudModes[] = { "Sky-dome (default)", "3D volumetric (parallax)" };
				int cmode = (env->cloudMode == 1) ? 1 : 0;
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::Combo("##cloudmode", &cmode, cloudModes, 2)) { env->cloudMode = cmode; trackEdit(); }
				if (env->cloudMode == 1)
				{
					ImGui::SetNextItemWidth(-1.0f);
					ImGui::SliderFloat("##cloudheight", &env->cloudHeight, 20.0f, 2000.0f,
					                   "3D height: %.0f"); trackEdit();
					ImGui::TextDisabled("Lifts the cloud band higher in the sky (clear sky opens toward the\nhorizon); the clouds keep the same size & shape (OpenGL only).");
				}
			}
			// Cloud quality (performance): scales the raymarch step counts + sun
			// light-march. Drop to Low on integrated GPUs / Apple Silicon Air if the
			// clouds are costing frames. (Metal first; other backends follow.)
			{
				const char* cloudQ[] = { "Low (fastest)", "Medium", "High (best)" };
				int q = (env->cloudQuality < 0) ? 0 : (env->cloudQuality > 2 ? 2 : env->cloudQuality);
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::Combo("##cloudquality", &q, cloudQ, 3)) { env->cloudQuality = q; trackEdit(); }
				ImGui::TextDisabled("Lower = cheaper. Clouds are a top GPU cost; Low ~halves their step count.");
				if (ImGui::Checkbox("Low-res clouds (quarter-res pass)", &env->lowResClouds)) trackEdit();
				ImGui::TextDisabled("Raymarch clouds at 1/4 res + upsample. Big win in open-sky views.\nToggle + F9 to A/B the cost. (Metal first.)");
			}
			// Cloud appearance: tweak the look without re-rolling the pattern.
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##clouddensity", &env->cloudDensity, 0.2f, 2.5f, "Density: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##cloudfluffy", &env->cloudFluffiness, 0.0f, 1.0f, "Fluffiness: %.2f"); trackEdit();
			ImGui::ColorEdit3("Cloud Tint", &env->cloudTint.x, ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::TextDisabled("Density thickens, fluffiness breaks the bodies into puffy cauliflower lumps.");
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##winddir", &env->windDirection, 0.0f, 360.0f, "Wind direction: %.0f\xc2\xb0"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##windspeed", &env->windSpeed, 0.0f, 4.0f, "Wind speed: %.2f"); trackEdit();

			ImGui::TreePop(); } // end Clouds

			if (ImGui::TreeNodeEx("Contrails & Cirrus")) {
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##contrails", &env->contrailAmount, 0.0f, 1.0f, "Contrails: %.2f"); trackEdit();
			ImGui::TextDisabled("Scattered vapour-trail lines to fill a clear daytime sky; fade as clouds build.");
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##cirrus", &env->cirrusAmount, 0.0f, 1.0f, "Cirrus: %.2f"); trackEdit();
			ImGui::BeginDisabled(env->cirrusAmount <= 0.0f);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##cirrusseed", &env->cirrusSeed, 0.0f, 50.0f, "Cirrus seed: %.1f"); trackEdit();
			ImGui::EndDisabled();
			ImGui::TextDisabled("Thin high wispy clouds. Intensity = cover, seed re-rolls the pattern (OpenGL).");

			ImGui::TreePop(); } // end Contrails & Cirrus

			if (ImGui::TreeNodeEx("Sun Effects")) {
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##godrays", &env->godRays, 0.0f, 1.0f, "God rays: %.2f"); trackEdit();
			ImGui::TextDisabled("Warm crepuscular glow where sunlight breaks through gaps in the cloud cover. Needs broken cloud (Coverage > 0) and the sun up; off when overcast or clear.");
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##lensflare", &env->lensFlare, 0.0f, 1.0f, "Lens flare: %.2f"); trackEdit();
			ImGui::TextDisabled("Camera lens flare for the sun: core, ghost discs and a halo along the sun\xe2\x86\x92screen-centre axis. Fades when the sun is off-screen, below the horizon, or occluded. A camera artifact.");

			ImGui::TreePop(); } // end Sun Effects

			if (ImGui::TreeNodeEx("Atmospheric Fog")) {
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##fogdensity", &env->fogDensity, 0.0f, 0.15f, "Density: %.3f"); trackEdit();
			ImGui::BeginDisabled(env->fogDensity <= 0.0f);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##fogheight", &env->fogHeightFalloff, 0.0f, 1.0f, "Ground hugging: %.2f"); trackEdit();
			ImGui::EndDisabled();
			ImGui::TextDisabled("Distant objects blend into the horizon (warm at sunset).");

			ImGui::TreePop(); } // end Atmospheric Fog

			if (ImGui::TreeNodeEx("Precipitation & Ground")) {
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##rain", &env->rainAmount, 0.0f, 1.0f, "Rain: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##snow", &env->snowAmount, 0.0f, 1.0f, "Snow: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##wetness", &env->wetness, 0.0f, 1.0f, "Wetness: %.2f"); trackEdit();
			ImGui::TextDisabled("Rain/snow spawn particles; wetness darkens & snow whitens the ground.");

			ImGui::TreePop(); } // end Precipitation & Ground

			if (ImGui::TreeNodeEx("Stars & Milky Way")) {
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##starbright", &env->starBrightness, 0.0f, 3.0f, "Star Brightness: %.2f"); trackEdit();
			ImGui::ColorEdit3("Star Color", &env->starColor.x, ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##stardensity", &env->starDensity, 0.0f, 1.0f, "Star Amount: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##starsize", &env->starSize, 0.3f, 2.5f, "Star Size: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##starsizevar", &env->starSizeVariation, 0.0f, 1.0f, "Size Variation: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##starglow", &env->starGlow, 0.0f, 3.0f, "Star Glow: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##startwinkle", &env->starTwinkle, 0.0f, 1.0f, "Twinkle: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##milkyway", &env->milkyWayIntensity, 0.0f, 1.0f, "Milky Way: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##shootingstars", &env->shootingStars, 0.0f, 1.0f, "Shooting Stars: %.2f"); trackEdit();
			ImGui::TextDisabled("Occasional meteors streak across the night sky; higher = more frequent. Night only.");

			ImGui::TreePop(); } // end Stars & Milky Way

			if (ImGui::TreeNodeEx("Nebula")) {
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##nebula", &env->nebulaIntensity, 0.0f, 1.0f, "Intensity: %.2f"); trackEdit();
			{
				// Combo index == nebulaQuality (0 Performance, 1 High, 2 Max).
				int nebQ = env->nebulaQuality < 0 ? 0 : (env->nebulaQuality > 2 ? 2 : env->nebulaQuality);
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::Combo("##nebulafidelity", &nebQ,
				    "High Performance (lighter)\0High Fidelity (detailed)\0Max Quality (most detail)\0"))
				{ env->nebulaQuality = nebQ; trackEdit(); }
				if (nebQ == 2)
					ImGui::TextDisabled("Extra filament octaves + crisper lines (night sky; pricier). Metal/OpenGL.");
			}
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##nebulaseed", &env->nebulaSeed, 0.0f, 50.0f, "Seed: %.1f"); trackEdit();
			ImGui::ColorEdit3("Nebula Color 1", &env->nebulaColor.x,  ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::ColorEdit3("Nebula Color 2", &env->nebulaColor2.x, ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::ColorEdit3("Nebula Color 3", &env->nebulaColor3.x, ImGuiColorEditFlags_NoInputs); trackEdit();

			ImGui::TreePop(); } // end Nebula

			if (ImGui::TreeNodeEx("Aurora")) {
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##aurora", &env->auroraIntensity, 0.0f, 1.0f, "Intensity: %.2f"); trackEdit();
			ImGui::ColorEdit3("Color (base)", &env->auroraColor.x, ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::ColorEdit3("Color (top)",  &env->auroraColorTop.x, ImGuiColorEditFlags_NoInputs); trackEdit();
			ImGui::BeginDisabled(env->auroraIntensity <= 0.0f);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##auroraheight", &env->auroraHeight, 0.0f, 1.0f, "Height: %.2f"); trackEdit();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::SliderFloat("##aurorafrag", &env->auroraFragmentation, 0.0f, 1.0f, "Fragmentation: %.2f"); trackEdit();
			ImGui::EndDisabled();
			ImGui::TextDisabled("Stars, Milky Way & nebula turn with the day; aurora drifts.");
			ImGui::TreePop(); } // end Aurora
		}
		ImGui::Separator();
	}

	// ── Weather (scene-wide; drives the sky's clouds / fog / wind) ──────────
	if (registry.all_of<EnvironmentComponent>(entity))
	{
		if (auto* w = registry.try_get<WeatherComponent>(entity))
		{
			if (ImGui::CollapsingHeader("Weather", ImGuiTreeNodeFlags_DefaultOpen))
			{
				const char* kinds[] = { "Clear","Cloudy","Overcast","Foggy","Rain","Storm","Snow" };
				int target = static_cast<int>(w->targetKind);
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::Combo("##weatherkind", &target, kinds, IM_ARRAYSIZE(kinds)))
				{
					w->targetKind = static_cast<WeatherKind>(target);
					trackEdit();
				}
				ImGui::SliderFloat("Intensity",  &w->intensity, 0.0f, 1.0f, "%.2f"); trackEdit();
				ImGui::SliderFloat("Transition", &w->transitionDuration, 0.0f, 30.0f, "%.1f s"); trackEdit();
				ImGui::Checkbox("Auto-Cycle", &w->autoCycle); trackEdit();
				ImGui::BeginDisabled(!w->autoCycle);
				ImGui::SliderFloat("Cycle Time", &w->cycleSeconds, 5.0f, 600.0f, "%.0f s",
				                   ImGuiSliderFlags_Logarithmic); trackEdit();
				ImGui::EndDisabled();
				ImGui::TextDisabled("Picking a preset sets clouds/fog/wind/precip; the sliders above\nstay editable, so you can nudge any value afterwards.");

				if (w->currentKind != w->targetKind)
					ImGui::Text("Transitioning %s -> %s",
					            kinds[static_cast<int>(w->currentKind)],
					            kinds[static_cast<int>(w->targetKind)]);
				else
					ImGui::Text("Current: %s", kinds[static_cast<int>(w->currentKind)]);
				ImGui::TextDisabled("Cloud %.2f  Fog %.3f  Wind %.2f  Precip %.2f",
				                    w->curCloudCoverage, w->curFogDensity,
				                    w->curWindSpeed, w->curPrecip);

				ImGui::SeparatorText("Precipitation");
				ImGui::DragInt("Max Rain", &w->maxRainParticles, 10.0f, 0, 20000); trackEdit();
				ImGui::DragInt("Max Snow", &w->maxSnowParticles, 10.0f, 0, 20000); trackEdit();
				ImGui::DragFloat("Ground Y", &w->groundLevel, 0.1f, -1000.0f, 1000.0f,
				                 "%.1f (fallback floor)"); trackEdit();
				ImGui::TextDisabled("Drops collide via physics in Play; else die at Ground Y.");

				// Thunder sound — drop an audio .hasset here (played on each strike).
				{
					const char* tlabel = (w->thunderSound == HE::UUID{})
						? "Thunder: (none — drop audio)" : "Thunder: (set)";
					ImGui::Button(tlabel);
					if (ImGui::BeginDragDropTarget())
					{
						const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH");
						if (p && ctx.contentManager)
						{
							std::error_code ec;
							const std::string rel = std::filesystem::relative(
								static_cast<const char*>(p->Data),
								ctx.contentManager->contentRoot(), ec).generic_string();
							const HE::UUID id = (ec || rel.empty())
								? HE::UUID{} : ctx.contentManager->loadAsset(rel);
							if (id != HE::UUID{} && ctx.contentManager->getAudio(id))
							{
								if (ctx.undoSys) ctx.undoSys->snapshotNow();
								w->thunderSound = id;
							}
						}
						ImGui::EndDragDropTarget();
					}
					if (w->thunderSound != HE::UUID{})
					{
						ImGui::SameLine();
						if (ImGui::SmallButton("Clear##thunder")) w->thunderSound = HE::UUID{};
					}
				}

			}
			ImGui::Separator();
		}
	}

	// Header with a right-click "Remove Component" menu. Returns true when
	// the section is open; sets `removed` when the user removed the component.
	auto componentHeader = [&](const char* label, bool removable, bool& removed) -> bool
	{
		const bool open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
		removed = false;
		if (removable && ImGui::BeginPopupContextItem())
		{
			if (ImGui::MenuItem("Remove Component"))
				removed = true;
			ImGui::EndPopup();
		}
		return open && !removed;
	};
	bool removed = false;

	// ── Transform ───────────────────────────────────────────────────────────
	if (auto* t = registry.try_get<TransformComponent>(entity))
	{
		if (componentHeader("Transform", true, removed))
		{
			bool changed = false;
			changed |= ImGui::DragFloat3("Position", &t->position.x, 0.05f); trackEdit();
			changed |= ImGui::DragFloat3("Rotation", &t->rotation.x, 0.5f);  trackEdit();
			changed |= ImGui::DragFloat3("Scale",    &t->scale.x,    0.05f); trackEdit();
			if (changed) t->dirty = true;
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<TransformComponent>(entity); }
	}

	// ── Transform 2D ────────────────────────────────────────────────────────
	if (auto* t = registry.try_get<Transform2DComponent>(entity))
	{
		if (componentHeader("Transform 2D", true, removed))
		{
			bool changed = false;
			changed |= ImGui::DragFloat2("Position##2d", &t->position.x, 0.05f); trackEdit();
			changed |= ImGui::DragFloat("Rotation##2d",  &t->rotation,   0.5f);  trackEdit();
			changed |= ImGui::DragFloat2("Scale##2d",    &t->scale.x,    0.05f); trackEdit();
			if (changed) t->dirty = true;
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<Transform2DComponent>(entity); }
	}

	// ── Mesh ────────────────────────────────────────────────────────────────
	if (auto* m = registry.try_get<MeshComponent>(entity))
	{
		if (componentHeader("Mesh", true, removed))
		{
			if (m->meshAssetId == HE::UUID{})
				ImGui::TextDisabled("Asset: (none — renders fallback cube)");
			else if (ctx.contentManager)
			{
				const StaticMeshAsset* asset = ctx.contentManager->getStaticMesh(m->meshAssetId);
				ImGui::Text("Asset: %s", asset ? asset->name.c_str() : "(not loaded)");
			}
			int lod = m->lodBias;
			if (ImGui::InputInt("LOD Bias", &lod))
				m->lodBias = static_cast<uint8_t>(std::clamp(lod, 0, 255));
			ImGui::Checkbox("Casts Shadow",    &m->castsShadow); trackEdit();
			ImGui::Checkbox("Receives Shadow", &m->receivesShadow); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<MeshComponent>(entity); }
	}

	// ── Skeletal Mesh ────────────────────────────────────────────────────────
	if (auto* sm = registry.try_get<SkeletalMeshComponent>(entity))
	{
		if (componentHeader("Skeletal Mesh", true, removed))
		{
			if (sm->meshAssetId == HE::UUID{})
				ImGui::TextDisabled("Asset: (none)");
			else if (ctx.contentManager)
			{
				const SkeletalMeshAsset* asset = ctx.contentManager->getSkeletalMesh(sm->meshAssetId);
				ImGui::Text("Asset: %s", asset ? asset->name.c_str() : "(not loaded)");
				if (asset)
					ImGui::Text("Joints: %d | Bone matrices: %d",
					    (int)asset->skeleton.size(), (int)sm->boneMatrices.size());
			}
			ImGui::Checkbox("Casts Shadow",    &sm->castsShadow);    trackEdit();
			ImGui::Checkbox("Receives Shadow", &sm->receivesShadow); trackEdit();

			// Drag-drop asset slot
			ImGui::TextUnformatted("Asset");
			ImGui::SameLine();
			const SkeletalMeshAsset* cur = (sm->meshAssetId != HE::UUID{} && ctx.contentManager)
			    ? ctx.contentManager->getSkeletalMesh(sm->meshAssetId) : nullptr;
			const std::string label = cur ? cur->name : (sm->meshAssetId == HE::UUID{} ? "(none)" : "(not loaded)");
			ImGui::Button((label + "##smslot").c_str());
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
				{
					std::error_code ec;
					std::string rel = std::filesystem::relative(
					    static_cast<const char*>(p->Data),
					    ctx.contentManager ? ctx.contentManager->contentRoot() : "",
					    ec).generic_string();
					if (!ec && !rel.empty() && rel.rfind("..", 0) != 0)
					{
						const HE::UUID id = ctx.contentManager->loadAsset(rel);
						if (id != HE::UUID{} && ctx.contentManager->getSkeletalMesh(id))
						{
							if (ctx.undoSys) ctx.undoSys->snapshotNow();
							sm->meshAssetId = id;
							sm->dirty = true;
						}
					}
				}
				ImGui::EndDragDropTarget();
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<SkeletalMeshComponent>(entity); }
	}

	// ── Animator ────────────────────────────────────────────────────────────
	if (auto* an = registry.try_get<AnimatorComponent>(entity))
	{
		if (componentHeader("Animator", true, removed))
		{
			// Clip asset slot
			ImGui::TextUnformatted("Clip");
			ImGui::SameLine();
			const AnimationClipAsset* cur = (an->clipAssetId != HE::UUID{} && ctx.contentManager)
				? ctx.contentManager->getAnimationClip(an->clipAssetId) : nullptr;
			const std::string clipLabel = cur ? cur->name
				: (an->clipAssetId == HE::UUID{} ? "(none)" : "(not loaded)");
			ImGui::Button((clipLabel + "##animslot").c_str());
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
				{
					std::error_code ec;
					std::string rel = std::filesystem::relative(
						static_cast<const char*>(p->Data),
						ctx.contentManager ? ctx.contentManager->contentRoot() : "",
						ec).generic_string();
					if (!ec && !rel.empty() && rel.rfind("..", 0) != 0)
					{
						const HE::UUID id = ctx.contentManager->loadAsset(rel);
						if (id != HE::UUID{} && ctx.contentManager->getAnimationClip(id))
						{
							if (ctx.undoSys) ctx.undoSys->snapshotNow();
							an->clipAssetId = id;
						}
					}
				}
				ImGui::EndDragDropTarget();
			}

			// Playback controls
			ImGui::DragFloat("Speed##an",    &an->playbackSpeed, 0.01f, -4.0f, 4.0f, "%.2f"); trackEdit();
			ImGui::DragFloat("Time##an",     &an->playbackTime,  0.01f,  0.0f, 999.0f, "%.3f s"); trackEdit();
			ImGui::Checkbox("Looping##an",   &an->looping); trackEdit();
			ImGui::SameLine();
			ImGui::Checkbox("Playing##an",   &an->playing); trackEdit();

			if (cur)
				ImGui::Text("Duration: %.3f s  |  Channels: %d",
					cur->duration, (int)cur->channels.size());
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<AnimatorComponent>(entity); }
	}

	// ── Animator Blend ───────────────────────────────────────────────────────
	if (auto* ab = registry.try_get<AnimatorBlendComponent>(entity))
	{
		if (componentHeader("Animator Blend", true, removed))
		{
			auto clipSlot = [&](const char* label, HE::UUID& slotId)
			{
				ImGui::TextUnformatted(label);
				ImGui::SameLine();
				const AnimationClipAsset* cur = (slotId != HE::UUID{} && ctx.contentManager)
					? ctx.contentManager->getAnimationClip(slotId) : nullptr;
				const std::string clipLabel = cur ? cur->name
					: (slotId == HE::UUID{} ? "(none)" : "(not loaded)");
				ImGui::Button((clipLabel + "##" + label).c_str());
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
					{
						std::error_code ec;
						std::string rel = std::filesystem::relative(
							static_cast<const char*>(p->Data),
							ctx.contentManager ? ctx.contentManager->contentRoot() : "",
							ec).generic_string();
						if (!ec && !rel.empty() && rel.rfind("..", 0) != 0)
						{
							const HE::UUID id = ctx.contentManager->loadAsset(rel);
							if (id != HE::UUID{} && ctx.contentManager->getAnimationClip(id))
							{
								if (ctx.undoSys) ctx.undoSys->snapshotNow();
								slotId = id;
							}
						}
					}
					ImGui::EndDragDropTarget();
				}
			};

			clipSlot("Clip A", ab->clipAId);
			clipSlot("Clip B", ab->clipBId);
			ImGui::SliderFloat("Blend##ab",  &ab->blendAlpha,    0.0f, 1.0f, "%.2f"); trackEdit();
			ImGui::DragFloat("Speed##ab",    &ab->playbackSpeed, 0.01f, -4.0f, 4.0f, "%.2f"); trackEdit();
			ImGui::DragFloat("Time##ab",     &ab->playbackTime,  0.01f,  0.0f, 999.0f, "%.3f s"); trackEdit();
			ImGui::Checkbox("Looping##ab",   &ab->looping); trackEdit();
			ImGui::SameLine();
			ImGui::Checkbox("Playing##ab",   &ab->playing); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<AnimatorBlendComponent>(entity); }
	}

	// ── Animator State Machine ──────────────────────────────────────────────
	if (auto* asm_ = registry.try_get<AnimatorStateMachineComponent>(entity))
	{
		if (componentHeader("Animator State Machine", true, removed))
		{
			// Current state + params read-out
			ImGui::LabelText("State##sm",  asm_->currentStateName.empty() ? "(none)" : asm_->currentStateName.c_str());
			ImGui::DragFloat("Speed##sm",  &asm_->playbackSpeed, 0.01f, -4.0f, 4.0f, "%.2f"); trackEdit();
			ImGui::DragFloat("Time##sm",   &asm_->clipTime,      0.01f,  0.0f, 999.0f, "%.3f s"); trackEdit();
			if (asm_->inTransition)
			{
				ImGui::LabelText("→##sm", asm_->transitionTarget.c_str());
				float pct = asm_->transitionDuration > 0.0f
					? asm_->transitionElapsed / asm_->transitionDuration : 0.0f;
				ImGui::ProgressBar(std::min(pct, 1.0f), ImVec2(-1, 0), "crossfade");
			}

			// States list
			if (ImGui::TreeNodeEx("States##sm", ImGuiTreeNodeFlags_DefaultOpen))
			{
				int stateToDelete = -1;
				for (int i = 0; i < static_cast<int>(asm_->states.size()); ++i)
				{
					auto& s = asm_->states[i];
					ImGui::PushID(i);
					char buf[128]; std::snprintf(buf, sizeof(buf), "%s", s.name.c_str());
					if (ImGui::InputText("Name##s", buf, sizeof(buf)))
					{ s.name = buf; trackEdit(); }
					ImGui::SameLine();
					ImGui::Checkbox("Loop##s", &s.looping); trackEdit();
					ImGui::SameLine();
					if (ImGui::SmallButton("Set##s"))
					{ if (ctx.undoSys) ctx.undoSys->snapshotNow(); asm_->currentStateName = s.name; }
					ImGui::SameLine();
					if (ImGui::SmallButton("X##s")) stateToDelete = i;
					ImGui::PopID();
				}
				if (stateToDelete >= 0)
				{ if (ctx.undoSys) ctx.undoSys->snapshotNow(); asm_->states.erase(asm_->states.begin() + stateToDelete); }
				if (ImGui::SmallButton("+ State##sm"))
				{ if (ctx.undoSys) ctx.undoSys->snapshotNow(); asm_->states.push_back({"NewState", HE::UUID{}, true}); }
				ImGui::TreePop();
			}

			// Transitions list
			if (ImGui::TreeNodeEx("Transitions##sm", ImGuiTreeNodeFlags_None))
			{
				const char* opNames[] = { ">", "<", "==" };
				int transToDelete = -1;
				for (int i = 0; i < static_cast<int>(asm_->transitions.size()); ++i)
				{
					auto& t = asm_->transitions[i];
					ImGui::PushID(i + 1000);
					char fb[64], tb[64], pb[64];
					std::snprintf(fb, sizeof(fb), "%s", t.fromState.c_str());
					std::snprintf(tb, sizeof(tb), "%s", t.toState.c_str());
					std::snprintf(pb, sizeof(pb), "%s", t.paramName.c_str());
					if (ImGui::InputText("From##t", fb, sizeof(fb)))  { t.fromState  = fb; trackEdit(); }
					ImGui::SameLine();
					if (ImGui::InputText("To##t",   tb, sizeof(tb)))  { t.toState    = tb; trackEdit(); }
					int opIdx = static_cast<int>(t.op);
					if (ImGui::Combo("Op##t", &opIdx, opNames, 3))
					{ t.op = static_cast<TransitionOp>(opIdx); trackEdit(); }
					ImGui::SameLine();
					if (ImGui::InputText("Param##t", pb, sizeof(pb))) { t.paramName  = pb; trackEdit(); }
					ImGui::DragFloat("Thresh##t", &t.threshold, 0.01f, -999.0f, 999.0f, "%.2f"); trackEdit();
					ImGui::DragFloat("Duration##t", &t.duration, 0.01f, 0.0f, 10.0f, "%.2f s"); trackEdit();
					ImGui::SameLine();
					if (ImGui::SmallButton("X##t")) transToDelete = i;
					ImGui::PopID();
				}
				if (transToDelete >= 0)
				{ if (ctx.undoSys) ctx.undoSys->snapshotNow(); asm_->transitions.erase(asm_->transitions.begin() + transToDelete); }
				if (ImGui::SmallButton("+ Transition##sm"))
				{ if (ctx.undoSys) ctx.undoSys->snapshotNow(); asm_->transitions.push_back({}); }
				ImGui::TreePop();
			}

			// Float params
			if (ImGui::TreeNodeEx("Params##sm", ImGuiTreeNodeFlags_None))
			{
				for (auto& [k, v] : asm_->params)
				{
					ImGui::PushID(k.c_str());
					ImGui::DragFloat(k.c_str(), &v, 0.01f, -999.0f, 999.0f, "%.2f"); trackEdit();
					ImGui::PopID();
				}
				ImGui::TreePop();
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<AnimatorStateMachineComponent>(entity); }
	}

	// ── Property Animator ───────────────────────────────────────────────────
	if (auto* pa = registry.try_get<PropertyAnimatorComponent>(entity))
	{
		if (componentHeader("Property Animator", true, removed))
		{
			// Clip drag-drop slot
			ImGui::TextUnformatted("Clip");
			ImGui::SameLine();
			const PropertyAnimClipAsset* cur = (pa->clipId != HE::UUID{} && ctx.contentManager)
				? ctx.contentManager->getPropertyAnimClip(pa->clipId) : nullptr;
			const std::string clipLabel = cur ? cur->name : (pa->clipId == HE::UUID{} ? "(none)" : "(not loaded)");
			ImGui::Button((clipLabel + "##pac").c_str());
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
				{
					std::error_code ec;
					std::string rel = std::filesystem::relative(
						static_cast<const char*>(p->Data),
						ctx.contentManager ? ctx.contentManager->contentRoot() : "",
						ec).generic_string();
					if (!ec && !rel.empty() && rel.rfind("..", 0) != 0)
					{
						const HE::UUID id = ctx.contentManager->loadAsset(rel);
						if (id != HE::UUID{} && ctx.contentManager->getPropertyAnimClip(id))
						{
							if (ctx.undoSys) ctx.undoSys->snapshotNow();
							pa->clipId = id;
						}
					}
				}
				ImGui::EndDragDropTarget();
			}

			ImGui::DragFloat("Speed##pa",  &pa->playbackSpeed, 0.01f, -4.0f, 4.0f, "%.2f"); trackEdit();
			ImGui::DragFloat("Time##pa",   &pa->playbackTime,  0.01f,  0.0f, 999.0f, "%.3f s"); trackEdit();
			ImGui::Checkbox("Looping##pa", &pa->looping); trackEdit();
			ImGui::SameLine();
			ImGui::Checkbox("Playing##pa", &pa->playing); trackEdit();

			if (cur)
			{
				ImGui::Separator();
				ImGui::Text("Duration: %.2f s | Channels: %zu", cur->duration, cur->channels.size());
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<PropertyAnimatorComponent>(entity); }
	}

	// ── NavMesh ─────────────────────────────────────────────────────────────
	if (auto* nmc = registry.try_get<NavMeshComponent>(entity))
	{
		if (componentHeader("Nav Mesh", true, removed))
		{
			ImGui::DragFloat("Cell Size##nm",       &nmc->config.cellSize,      0.01f, 0.05f, 2.0f,   "%.2f"); trackEdit();
			ImGui::DragFloat("Cell Height##nm",     &nmc->config.cellHeight,    0.01f, 0.05f, 2.0f,   "%.2f"); trackEdit();
			ImGui::DragFloat("Walk Height##nm",     &nmc->config.walkableHeight,0.1f,  0.5f,  5.0f,   "%.2f"); trackEdit();
			ImGui::DragFloat("Walk Climb##nm",      &nmc->config.walkableClimb, 0.1f,  0.0f,  2.0f,   "%.2f"); trackEdit();
			ImGui::DragFloat("Walk Radius##nm",     &nmc->config.walkableRadius,0.05f, 0.0f,  2.0f,   "%.2f"); trackEdit();
			ImGui::DragFloat("Max Slope##nm",       &nmc->config.maxSlope,      1.0f,  0.0f,  90.0f,  "%.1f°"); trackEdit();
			ImGui::Separator();
			ImGui::Text("Geometry: %zu verts  %zu tris",
				nmc->geometry.verts.size() / 3,
				nmc->geometry.tris.size()  / 3);
			const bool baked = (bool)nmc->navMesh;
			ImGui::Text("NavMesh: %s", baked ? "baked" : "not baked");
			if (ImGui::Button("Bake##nm"))
			{
				if (ctx.undoSys) ctx.undoSys->snapshotNow();
				NavigationSystem::bake(*nmc);
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<NavMeshComponent>(entity); }
	}

	// ── NavAgent ────────────────────────────────────────────────────────────
	if (auto* na = registry.try_get<NavAgentComponent>(entity))
	{
		if (componentHeader("Nav Agent", true, removed))
		{
			ImGui::DragFloat3("Target##na",     glm::value_ptr(na->targetPos), 0.1f); trackEdit();
			ImGui::DragFloat("Speed##na",       &na->speed,        0.1f, 0.0f, 20.0f, "%.1f m/s"); trackEdit();
			ImGui::DragFloat("Stop Dist##na",   &na->stoppingDist, 0.01f,0.0f, 2.0f,  "%.2f m"); trackEdit();
			ImGui::Separator();
			ImGui::Text("Path: %zu pts  idx=%zu  %s",
				na->path.size(), na->pathIdx,
				na->moving ? "MOVING" : "stopped");
			if (ImGui::Button("Go##na"))
			{ na->hasPath = false; na->moving = true; }
			ImGui::SameLine();
			if (ImGui::Button("Stop##na"))
			{ na->moving = false; na->hasPath = false; }
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<NavAgentComponent>(entity); }
	}

	// ── Material ────────────────────────────────────────────────────────────
	if (auto* m = registry.try_get<MaterialComponent>(entity))
	{
		if (componentHeader("Material", true, removed))
		{
			// Resolve a Content-Browser drag (absolute path) to a content-root-
			// relative path, returning empty if it lives outside the root.
			auto toRelative = [&](const char* absPath) -> std::string
			{
				if (!ctx.contentManager) return {};
				std::error_code ec;
				std::string rel = std::filesystem::relative(
					absPath, ctx.contentManager->contentRoot(), ec).generic_string();
				if (ec || rel.empty() || rel.rfind("..", 0) == 0) return {};
				return rel;
			};

			// ── Material asset slot — drop a material .hasset here ────────────
			const MaterialAsset* asset = (m->materialAssetId == HE::UUID{} || !ctx.contentManager)
				? nullptr : ctx.contentManager->getMaterial(m->materialAssetId);
			const std::string slotLabel = (m->materialAssetId == HE::UUID{})
				? std::string("(none — drop a material here)")
				: (asset ? asset->name : std::string("(not loaded)"));

			ImGui::TextUnformatted("Asset");
			ImGui::SameLine();
			ImGui::Button((slotLabel + "##matslot").c_str());
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
				{
					const std::string rel = toRelative(static_cast<const char*>(p->Data));
					const HE::UUID id = rel.empty() ? HE::UUID{}
					                                : ctx.contentManager->loadAsset(rel);
					if (id != HE::UUID{} && ctx.contentManager->getMaterial(id))
					{
						if (ctx.undoSys) ctx.undoSys->snapshotNow();
						m->materialAssetId = id;
						m->dirty = true;
						if (ctx.renderer) ctx.renderer->InvalidateMaterial(id);
					}
					else
						Logger::Log(Logger::LogLevel::Warning,
							"Editor: dropped asset is not a material");
				}
				ImGui::EndDragDropTarget();
			}
			if (m->materialAssetId != HE::UUID{})
			{
				ImGui::SameLine();
				if (ImGui::SmallButton("Clear"))
				{
					if (ctx.undoSys) ctx.undoSys->snapshotNow();
					m->materialAssetId = HE::UUID{};
					m->dirty = true;
				}
			}

			// ── Editable slots of the assigned material ──────────────────────
			MaterialAsset* mat = (m->materialAssetId == HE::UUID{} || !ctx.contentManager)
				? nullptr : ctx.contentManager->getMaterialMutable(m->materialAssetId);
			if (mat)
			{
				ImGui::Separator();
				ImGui::TextDisabled("%s", mat->path.c_str());

				// Shader path
				char sbuf[260];
				std::strncpy(sbuf, mat->shaderPath.c_str(), sizeof(sbuf) - 1);
				sbuf[sizeof(sbuf) - 1] = '\0';
				if (ImGui::InputText("Shader", sbuf, sizeof(sbuf)))
					mat->shaderPath = sbuf;

				// Surface (PBR scalars) — applied live (the renderer reads them
				// from the shared MaterialAsset each frame); "Save Material" persists.
				ImGui::SeparatorText("Surface");
				ImGui::ColorEdit3("Base Color", mat->baseColor);
				ImGui::SliderFloat("Metallic",  &mat->metallic,  0.0f, 1.0f, "%.2f");
				ImGui::SliderFloat("Roughness", &mat->roughness, 0.0f, 1.0f, "%.2f");
				// Opacity < 1 routes the object into the sorted, alpha-blended
				// transparency pass.
				ImGui::SliderFloat("Opacity",   &mat->opacity,   0.0f, 1.0f, "%.2f");

				// Texture slots — editable text + per-slot drop target + remove.
				ImGui::TextUnformatted("Textures");
				int removeSlot = -1;
				for (size_t i = 0; i < mat->texturePaths.size(); ++i)
				{
					ImGui::PushID(static_cast<int>(i));
					char tbuf[260];
					std::strncpy(tbuf, mat->texturePaths[i].c_str(), sizeof(tbuf) - 1);
					tbuf[sizeof(tbuf) - 1] = '\0';
					ImGui::SetNextItemWidth(-30.0f);
					if (ImGui::InputText("##tex", tbuf, sizeof(tbuf)))
						mat->texturePaths[i] = tbuf;
					if (ImGui::IsItemDeactivatedAfterEdit() && ctx.renderer)
						ctx.renderer->InvalidateMaterial(m->materialAssetId);
					if (ImGui::BeginDragDropTarget())
					{
						if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("HE_ASSET_PATH"))
						{
							const std::string rel = toRelative(static_cast<const char*>(p->Data));
							if (!rel.empty())
							{
								mat->texturePaths[i] = rel;
								if (ctx.renderer) ctx.renderer->InvalidateMaterial(m->materialAssetId);
							}
						}
						ImGui::EndDragDropTarget();
					}
					ImGui::SameLine();
					if (ImGui::SmallButton("X")) removeSlot = static_cast<int>(i);
					ImGui::PopID();
				}
				if (removeSlot >= 0)
				{
					mat->texturePaths.erase(mat->texturePaths.begin() + removeSlot);
					if (ctx.renderer) ctx.renderer->InvalidateMaterial(m->materialAssetId);
				}
				if (ImGui::SmallButton("+ Texture Slot"))
					mat->texturePaths.emplace_back();

				// ── Node-graph parameters (per-ENTITY override) ──────────────────
				// Live-tweak this entity's exposed material parameters without opening
				// the material editor. Values write to MaterialComponent::paramOverrides
				// (this entity only) — the shared material asset is untouched. The
				// extractor merges them onto the material's defaults each frame.
				if (!mat->graphParamNames.empty())
				{
					ImGui::SeparatorText("Material Parameters (this entity)");
					// Index of the override for `nm`, or -1. Index-based (never a pointer
					// into the vector) so a push_back/erase this frame can't dangle.
					auto overrideIndex = [&](const std::string& nm) -> int {
						for (size_t j = 0; j < m->paramOverrides.size(); ++j)
							if (m->paramOverrides[j].name == nm) return (int)j;
						return -1;
					};
					int resetIndex = -1; // deferred erase until after the loop
					for (size_t i = 0; i < mat->graphParamNames.size(); ++i)
					{
						const std::string nm = mat->graphParamNames[i]; // copy — vector may move
						const HE::MatParamKind kind = (i < mat->graphParamTypes.size())
							? static_cast<HE::MatParamKind>(mat->graphParamTypes[i])
							: HE::MatParamKind::Float;
						const int ovi = overrideIndex(nm);
						// Working value: the override if present, else the material default.
						float val[4] = { 0, 0, 0, 0 };
						if (ovi >= 0) { for (int k = 0; k < 4; ++k) val[k] = m->paramOverrides[ovi].value[k]; }
						else if (i * 4 + 3 < mat->shaderParamData.size())
							for (int k = 0; k < 4; ++k) val[k] = mat->shaderParamData[i * 4 + k];

						ImGui::PushID(static_cast<int>(i));
						bool edited = false;
						ImGui::SetNextItemWidth(-60.0f);
						const char* label = nm.empty() ? "param" : nm.c_str();
						switch (kind)
						{
							case HE::MatParamKind::Color:
								edited = ImGui::ColorEdit3(label, val, ImGuiColorEditFlags_Float); break;
							case HE::MatParamKind::Vec2:
								edited = ImGui::DragFloat2(label, val, 0.01f); break;
							case HE::MatParamKind::Vec4:
								edited = ImGui::DragFloat4(label, val, 0.01f); break;
							case HE::MatParamKind::Bool:
							{
								bool b = val[0] > 0.5f;
								if (ImGui::Checkbox(label, &b)) { val[0] = b ? 1.0f : 0.0f; edited = true; }
								break;
							}
							default: // Float
								edited = ImGui::DragFloat(label, val, 0.01f); break;
						}
						if (edited)
						{
							int w = ovi;
							if (w < 0) { MaterialParamOverride ov; ov.name = nm; m->paramOverrides.push_back(ov); w = (int)m->paramOverrides.size() - 1; }
							for (int k = 0; k < 4; ++k) m->paramOverrides[w].value[k] = val[k];
							m->dirty = true;
						}
						if (ovi >= 0)
						{
							ImGui::SameLine();
							if (ImGui::SmallButton("Reset")) resetIndex = ovi;
						}
						ImGui::PopID();
					}
					if (resetIndex >= 0 && resetIndex < (int)m->paramOverrides.size())
					{
						m->paramOverrides.erase(m->paramOverrides.begin() + resetIndex);
						m->dirty = true;
					}
					if (!m->paramOverrides.empty())
						ImGui::TextDisabled("%zu override(s) on this entity", m->paramOverrides.size());
				}

				// Custom shader (fragment GLSL). Empty → built-in PBR. When set, the
				// renderer cross-compiles it (shared MaterialShaderLibrary) and draws this
				// material with its own pipeline. Edited in a separate buffer so the pipeline
				// isn't recompiled on every keystroke — applied on focus-loss / Apply, then
				// picked up live (the renderer re-resolves the shader each frame).
				ImGui::SeparatorText("Custom Shader (Fragment GLSL)");
				ImGui::TextDisabled("in vec3 vNormal (loc0), vColor (loc1)  ->  out vec4 oColor (loc0)");
				static std::string s_shaderEdit;
				static HE::UUID    s_shaderEditFor{};
				if (!(s_shaderEditFor == m->materialAssetId))
				{
					s_shaderEdit    = mat->customShaderFragGlsl;
					s_shaderEditFor = m->materialAssetId;
				}
				ImGui::InputTextMultiline("##customShader", &s_shaderEdit, ImVec2(-1.0f, 160.0f));
				bool applyShader = ImGui::IsItemDeactivatedAfterEdit();
				if (ImGui::Button("Apply Shader")) applyShader = true;
				ImGui::SameLine();
				if (ImGui::Button("Clear (use PBR)")) { s_shaderEdit.clear(); applyShader = true; }
				ImGui::SameLine();
				ImGui::TextDisabled(mat->customShaderFragGlsl.empty() ? "(built-in PBR)" : "(custom active)");
				if (applyShader) mat->customShaderFragGlsl = s_shaderEdit;

				ImGui::Spacing();
				if (ImGui::Button("Save Material"))
				{
					const bool ok = ctx.contentManager->saveAsset(*mat);
					if (ok && ctx.renderer) ctx.renderer->InvalidateMaterial(m->materialAssetId);
					Logger::Log(ok ? Logger::LogLevel::Info : Logger::LogLevel::Error,
						("Editor: " + std::string(ok ? "saved" : "failed to save")
						 + " material '" + mat->name + "'").c_str());
				}
				ImGui::SameLine();
				ImGui::TextDisabled("(edits apply live; Save writes to disk)");
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<MaterialComponent>(entity); }
	}

	// ── Camera ──────────────────────────────────────────────────────────────
	if (auto* c = registry.try_get<CameraComponent>(entity))
	{
		if (componentHeader("Camera", true, removed))
		{
			ImGui::DragFloat("FOV",        &c->fovDegrees, 0.5f, 1.0f, 179.0f); trackEdit();
			ImGui::DragFloat("Near Plane", &c->nearPlane,  0.01f, 0.001f, 100.0f); trackEdit();
			ImGui::DragFloat("Far Plane",  &c->farPlane,   1.0f,  0.1f, 100000.0f); trackEdit();
			ImGui::Checkbox("Main Camera", &c->isMain); trackEdit();
			ImGui::Checkbox("Orthographic", &c->orthographic); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<CameraComponent>(entity); }
	}

	// ── Light ───────────────────────────────────────────────────────────────
	if (auto* l = registry.try_get<LightComponent>(entity))
	{
		if (componentHeader("Light", true, removed))
		{
			static const char* kLightTypes[] = { "Directional", "Point", "Spot" };
			int type = static_cast<int>(l->type);
			if (ImGui::Combo("Type", &type, kLightTypes, 3))
			{
				if (ctx.undoSys) ctx.undoSys->snapshotNow();
				l->type = static_cast<LightType>(type);
			}
			ImGui::ColorEdit3("Color",    &l->color.x); trackEdit();
			ImGui::DragFloat("Intensity", &l->intensity, 0.05f, 0.0f, 1000.0f); trackEdit();
			if (l->type != LightType::Directional)
				ImGui::DragFloat("Range", &l->range, 0.1f, 0.0f, 10000.0f); trackEdit();
			if (l->type == LightType::Spot)
				ImGui::DragFloat("Spot Angle", &l->spotAngle, 0.5f, 1.0f, 179.0f); trackEdit();
			ImGui::Checkbox("Casts Shadow##light", &l->castsShadow); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<LightComponent>(entity); }
	}

	// ── Rigid Body ──────────────────────────────────────────────────────────
	if (auto* r = registry.try_get<RigidBodyComponent>(entity))
	{
		if (componentHeader("Rigid Body", true, removed))
		{
			static const char* kBodyTypes[] = { "Static", "Dynamic", "Kinematic" };
			int type = static_cast<int>(r->type);
			if (ImGui::Combo("Body Type", &type, kBodyTypes, 3))
			{
				if (ctx.undoSys) ctx.undoSys->snapshotNow();
				r->type = static_cast<RigidBodyType>(type);
			}
			ImGui::DragFloat("Mass",        &r->mass,        0.1f, 0.0f, 100000.0f); trackEdit();
			ImGui::DragFloat("Friction",    &r->friction,    0.01f, 0.0f, 1.0f); trackEdit();
			ImGui::DragFloat("Restitution", &r->restitution, 0.01f, 0.0f, 1.0f); trackEdit();
			ImGui::Checkbox("2D Physics",   &r->is2D); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<RigidBodyComponent>(entity); }
	}

	// ── Collider ──────────────────────────────────────────────────────────────
	if (auto* col = registry.try_get<ColliderComponent>(entity))
	{
		if (componentHeader("Collider", true, removed))
		{
			static const char* kShapes[] = { "Box", "Sphere", "Capsule" };
			int shape = static_cast<int>(col->shape);
			if (ImGui::Combo("Shape", &shape, kShapes, 3))
			{
				if (ctx.undoSys) ctx.undoSys->snapshotNow();
				col->shape = static_cast<ColliderShape>(shape);
			}
			switch (col->shape)
			{
			case ColliderShape::Box:
				ImGui::DragFloat3("Half Extents", &col->halfExtents.x, 0.01f, 0.001f, 100.0f); trackEdit();
				break;
			case ColliderShape::Sphere:
				ImGui::DragFloat("Radius", &col->radius, 0.01f, 0.001f, 100.0f); trackEdit();
				break;
			case ColliderShape::Capsule:
				ImGui::DragFloat("Radius",       &col->radius, 0.01f, 0.001f, 100.0f); trackEdit();
				ImGui::DragFloat("Total Height", &col->height, 0.01f, 0.001f, 100.0f); trackEdit();
				break;
			}
			ImGui::Checkbox("Is Trigger", &col->isTrigger); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<ColliderComponent>(entity); }
	}

	// ── Character Controller ──────────────────────────────────────────────────
	if (auto* cc = registry.try_get<CharacterControllerComponent>(entity))
	{
		if (componentHeader("Character Controller", true, removed))
		{
			ImGui::DragFloat("Slope Limit (deg)", &cc->slopeLimit, 0.5f, 1.0f, 90.0f); trackEdit();
			ImGui::DragFloat("Step Height (m)",   &cc->stepHeight, 0.01f, 0.0f, 2.0f); trackEdit();
			ImGui::DragFloat("Skin Width (m)",    &cc->skinWidth,  0.001f, 0.001f, 0.5f); trackEdit();
			ImGui::DragFloat("Mass (kg)",          &cc->mass,       0.5f, 1.0f, 500.0f); trackEdit();
			ImGui::DragFloat("Gravity (m/s²)",     &cc->gravity,    0.1f, 0.0f, 30.0f); trackEdit();
			ImGui::Separator();
			ImGui::BeginDisabled(true);
			ImGui::Checkbox("Is Grounded", &cc->isGrounded);
			float v[3] = { cc->velocity.x, cc->velocity.y, cc->velocity.z };
			ImGui::DragFloat3("Velocity", v);
			ImGui::EndDisabled();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<CharacterControllerComponent>(entity); }
	}

	// ── Script (Lua) ────────────────────────────────────────────────────────
	if (auto* s = registry.try_get<ScriptComponent>(entity))
	{
		if (componentHeader("Script", true, removed))
		{
			char buf[256];
			std::strncpy(buf, s->moduleName.c_str(), sizeof(buf) - 1);
			buf[sizeof(buf) - 1] = '\0';
			if (ImGui::InputText("Script Name", buf, sizeof(buf)))
			{
				s->moduleName = buf;
				trackEdit();
			}
			ImGui::SameLine();
			ImGui::TextDisabled("(?)");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Logical name matching ScriptEngine::loadScript(name, source).\n"
				                  "Script must export onStart(self) and/or onUpdate(self, dt).");
			ImGui::Checkbox("Enabled", &s->enabled); trackEdit();

			// ── Declared properties (M.properties table) ──────────────────
			if (ctx.propScriptEngine && ctx.contentManager && !s->moduleName.empty())
			{
				const ScriptAsset* asset = nullptr;
				if (s->scriptAssetId != HE::UUID{})
					asset = ctx.contentManager->getScript(s->scriptAssetId);
				if (asset && !asset->sourceCode.empty())
				{
					if (!ctx.propScriptEngine->isScriptLoaded(s->moduleName))
						ctx.propScriptEngine->loadScript(s->moduleName, asset->sourceCode);
					auto defs = ctx.propScriptEngine->getScriptProperties(s->moduleName);
					if (!defs.empty())
					{
						ImGui::Separator();
						ImGui::TextDisabled("Properties");
						for (const auto& def : defs)
						{
							auto it = s->properties.find(def.name);
							if (it == s->properties.end())
							{
								s->properties[def.name] = def.defaultVal;
								it = s->properties.find(def.name);
							}
							ScriptPropValue& val = it->second;
							switch (val.type)
							{
							case ScriptPropType::Float:
								if (ImGui::DragFloat(def.name.c_str(), &val.f, 0.1f)) trackEdit();
								break;
							case ScriptPropType::Int:
								if (ImGui::DragInt(def.name.c_str(), &val.i)) trackEdit();
								break;
							case ScriptPropType::Bool:
								if (ImGui::Checkbox(def.name.c_str(), &val.b)) trackEdit();
								break;
							case ScriptPropType::String:
							{
								char sbuf[256];
								std::strncpy(sbuf, val.s.c_str(), sizeof(sbuf) - 1);
								sbuf[sizeof(sbuf) - 1] = '\0';
								if (ImGui::InputText(def.name.c_str(), sbuf, sizeof(sbuf)))
								{
									val.s = sbuf;
									trackEdit();
								}
								break;
							}
							}
						}
					}
				}
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<ScriptComponent>(entity); }
	}

	// ── Terrain ─────────────────────────────────────────────────────────────
	if (auto* t = registry.try_get<TerrainComponent>(entity))
	{
		if (componentHeader("Terrain", true, removed))
		{
			bool changed = false;
			changed |= ImGui::DragFloat("Width (X)##tc",    &t->sizeX,      1.0f,  1.0f, 10000.0f, "%.1f m"); trackEdit();
			changed |= ImGui::DragFloat("Depth (Z)##tc",    &t->sizeZ,      1.0f,  1.0f, 10000.0f, "%.1f m"); trackEdit();
			int res = static_cast<int>(t->resolution);
			if (ImGui::SliderInt("Resolution##tc", &res, 2, 512)) { t->resolution = static_cast<uint32_t>(res); changed = true; }
			trackEdit();
			changed |= ImGui::DragFloat("Height Scale##tc", &t->heightScale, 0.5f,  0.0f, 1000.0f,  "%.1f m"); trackEdit();

			// Noise is a one-time creation input: it is baked into editable
			// heights when the landscape is created, so these are read-only here
			// (shown for reference) and can no longer change the terrain.
			ImGui::SeparatorText("Noise (set at creation)");
			ImGui::BeginDisabled();
			ImGui::InputInt  ("Seed##tc",       &t->seed);
			int oct = t->octaves;
			ImGui::SliderInt ("Octaves##tc",    &oct, 1, 8);
			ImGui::DragFloat ("Frequency##tc",  &t->frequency,  0.01f, 0.01f, 16.0f, "%.2f");
			ImGui::DragFloat ("Lacunarity##tc", &t->lacunarity, 0.01f, 1.0f,  8.0f,  "%.2f");
			ImGui::DragFloat ("Gain##tc",       &t->gain,       0.01f, 0.0f,  1.0f,  "%.2f");
			ImGui::EndDisabled();

			if (changed) t->dirty = true;
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<TerrainComponent>(entity); }
	}

	// ── Audio Source ────────────────────────────────────────────────────────
	if (auto* a = registry.try_get<AudioSourceComponent>(entity))
	{
		if (componentHeader("Audio Source", true, removed))
		{
			char buf[64];
			snprintf(buf, sizeof(buf), "%llu:%llu", (unsigned long long)a->assetId.hi,
			         (unsigned long long)a->assetId.lo);
			ImGui::LabelText("Asset ID", "%s", buf);
			char busBuf[64];
			std::strncpy(busBuf, a->busName.c_str(), sizeof(busBuf) - 1);
			busBuf[sizeof(busBuf) - 1] = '\0';
			if (ImGui::InputText("Bus##as", busBuf, sizeof(busBuf))) { a->busName = busBuf; trackEdit(); }
			ImGui::DragFloat("Volume##as", &a->volume, 0.01f, 0.0f, 2.0f); trackEdit();
			ImGui::DragFloat("Pitch##as",  &a->pitch,  0.01f, 0.1f, 4.0f); trackEdit();
			ImGui::Checkbox("Loop##as",        &a->loop);        trackEdit();
			ImGui::Checkbox("Play on Start##as",&a->playOnStart); trackEdit();
			ImGui::Checkbox("Spatial##as",     &a->spatial);     trackEdit();
			if (a->spatial)
			{
				ImGui::DragFloat("Inner Range##as",   &a->innerRange,    0.1f, 0.0f, 1000.0f, "%.1f m"); trackEdit();
				ImGui::DragFloat("Range##as",         &a->range,         0.5f, 0.0f, 1000.0f, "%.1f m"); trackEdit();
				ImGui::DragFloat("Rolloff Factor##as", &a->rolloffFactor, 0.1f, 0.0f, 10.0f,  "%.2f");   trackEdit();
			}
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<AudioSourceComponent>(entity); }
	}

	// ── Audio Listener ──────────────────────────────────────────────────────
	if (auto* l = registry.try_get<AudioListenerComponent>(entity))
	{
		if (componentHeader("Audio Listener", true, removed))
		{
			ImGui::DragFloat("Master Volume##al", &l->masterVolume, 0.01f, 0.0f, 2.0f); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<AudioListenerComponent>(entity); }
	}

	// ── Particle System ─────────────────────────────────────────────────────
	if (auto* ps = registry.try_get<ParticleSystemComponent>(entity))
	{
		if (componentHeader("Particle System", true, removed))
		{
			ImGui::DragFloat("Emit Rate##ps",      &ps->emitRate,      0.5f, 0.0f, 10000.0f); trackEdit();
			ImGui::DragFloat("Lifetime Min##ps",   &ps->lifetimeMin,   0.05f, 0.01f, 100.0f); trackEdit();
			ImGui::DragFloat("Lifetime Max##ps",   &ps->lifetimeMax,   0.05f, 0.01f, 100.0f); trackEdit();
			ImGui::DragFloat("Start Size##ps",     &ps->startSize,     0.01f, 0.0f, 100.0f); trackEdit();
			ImGui::DragFloat("End Size##ps",       &ps->endSize,       0.01f, 0.0f, 100.0f); trackEdit();
			ImGui::ColorEdit3("Start Color##ps",   glm::value_ptr(ps->startColor)); trackEdit();
			ImGui::ColorEdit3("End Color##ps",     glm::value_ptr(ps->endColor)); trackEdit();
			ImGui::DragFloat("Start Alpha##ps",    &ps->startAlpha,    0.01f, 0.0f, 1.0f); trackEdit();
			ImGui::DragFloat("End Alpha##ps",      &ps->endAlpha,      0.01f, 0.0f, 1.0f); trackEdit();
			ImGui::DragFloat3("Velocity##ps",      glm::value_ptr(ps->initialVelocity), 0.1f); trackEdit();
			ImGui::DragFloat("Spread (rad)##ps",   &ps->velocitySpread, 0.01f, 0.0f, 3.14f); trackEdit();
			ImGui::DragFloat3("Gravity##ps",       glm::value_ptr(ps->gravity), 0.1f); trackEdit();
			ImGui::DragInt("Max Particles##ps",    &ps->maxParticles, 1, 1, 10000); trackEdit();
			ImGui::Checkbox("Playing##ps",         &ps->playing); trackEdit();
			ImGui::SameLine();
			ImGui::Checkbox("Looping##ps",         &ps->looping); trackEdit();
			// Live particle count (read-only info).
			ImGui::Text("Live: %zu", ps->particles.size());
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<ParticleSystemComponent>(entity); }
	}

	// ── Foliage ──────────────────────────────────────────────────────────────
	if (auto* fol = registry.try_get<FoliageComponent>(entity))
	{
		if (componentHeader("Foliage", true, removed))
		{
			ImGui::DragFloat("Density##fol",       &fol->density,      0.01f, 0.001f, 10.f); if (ImGui::IsItemDeactivatedAfterEdit()) { fol->dirty = true; trackEdit(); }
			ImGui::DragFloat("Draw Distance##fol", &fol->drawDistance, 1.0f,  1.0f,  500.f); trackEdit();
			ImGui::DragFloat("Min Scale##fol",     &fol->minScale,     0.01f, 0.01f, 10.f);  if (ImGui::IsItemDeactivatedAfterEdit()) { fol->dirty = true; trackEdit(); }
			ImGui::DragFloat("Max Scale##fol",     &fol->maxScale,     0.01f, 0.01f, 10.f);  if (ImGui::IsItemDeactivatedAfterEdit()) { fol->dirty = true; trackEdit(); }
			ImGui::DragInt  ("Seed##fol",          &fol->seed,         1);                    if (ImGui::IsItemDeactivatedAfterEdit()) { fol->dirty = true; trackEdit(); }
			ImGui::Text("Instances: %zu", fol->cachedInstances.size());
			if (ImGui::Button("Regenerate")) { fol->dirty = true; trackEdit(); }
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<FoliageComponent>(entity); }
	}

	// ── LOD ──────────────────────────────────────────────────────────────────
	if (auto* lod = registry.try_get<LODComponent>(entity))
	{
		if (componentHeader("LOD", true, removed))
		{
			ImGui::Text("Levels: %zu   Active: %u", lod->levels.size(), lod->current);
			ImGui::Spacing();
			for (int li = 0; li < static_cast<int>(lod->levels.size()); ++li)
			{
				auto& lvl = lod->levels[static_cast<size_t>(li)];
				ImGui::PushID(li);
				ImGui::Text("LOD %d", li);
				ImGui::SameLine();
				char distBuf[32];
				std::snprintf(distBuf, sizeof(distBuf), "%.1f", lvl.maxDistance);
				ImGui::SetNextItemWidth(80.f);
				if (ImGui::InputText("##maxDist", distBuf, sizeof(distBuf),
				                     ImGuiInputTextFlags_EnterReturnsTrue))
				{
					lvl.maxDistance = std::strtof(distBuf, nullptr);
					trackEdit();
				}
				ImGui::SameLine();
				ImGui::TextDisabled("UUID %llx", static_cast<unsigned long long>(lvl.meshId.hi));
				if (ImGui::Button("Remove##lodlvl")) { lod->levels.erase(lod->levels.begin() + li); trackEdit(); --li; }
				ImGui::PopID();
			}
			if (ImGui::Button("+ Level")) { lod->levels.push_back({}); trackEdit(); }
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<LODComponent>(entity); }
	}

	// ── UI Canvas ───────────────────────────────────────────────────────────
	if (auto* cv = registry.try_get<UICanvasComponent>(entity))
	{
		if (componentHeader("UI Canvas", true, removed))
		{
			ImGui::DragFloat("Width##cv",  &cv->width,  1.0f, 1.0f, 7680.0f); trackEdit();
			ImGui::DragFloat("Height##cv", &cv->height, 1.0f, 1.0f, 4320.0f); trackEdit();
			int rm = static_cast<int>(cv->renderMode);
			if (ImGui::Combo("Render Mode##cv", &rm, "Screen Space\0World Space\0")) {
				cv->renderMode = static_cast<UIRenderMode>(rm); trackEdit();
			}
			ImGui::Checkbox("Active##cv", &cv->active); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<UICanvasComponent>(entity); }
	}

	// ── UI Element ──────────────────────────────────────────────────────────
	if (auto* el = registry.try_get<UIElementComponent>(entity))
	{
		if (componentHeader("UI Element", true, removed))
		{
			ImGui::DragFloat2("Position##el", glm::value_ptr(el->position), 1.0f); trackEdit();
			ImGui::DragFloat2("Size##el",     glm::value_ptr(el->size),     1.0f, 0.0f, 10000.0f); trackEdit();
			ImGui::DragFloat2("Pivot##el",    glm::value_ptr(el->pivot),    0.01f, 0.0f, 1.0f); trackEdit();
			ImGui::DragFloat("Rotation##el",  &el->rotation, 0.5f); trackEdit();
			int anch = static_cast<int>(el->anchor);
			const char* anchNames = "Top Left\0Top Center\0Top Right\0"
			                        "Mid Left\0Mid Center\0Mid Right\0"
			                        "Bot Left\0Bot Center\0Bot Right\0";
			if (ImGui::Combo("Anchor##el", &anch, anchNames)) {
				el->anchor = static_cast<UIAnchor>(anch); trackEdit();
			}
			ImGui::DragInt("Layer##el",  &el->layer, 1); trackEdit();
			ImGui::Checkbox("Active##el", &el->active); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<UIElementComponent>(entity); }
	}

	// ── UI Text ─────────────────────────────────────────────────────────────
	if (auto* txt = registry.try_get<UITextComponent>(entity))
	{
		if (componentHeader("UI Text", true, removed))
		{
			char buf[256];
			strncpy(buf, txt->text.c_str(), sizeof(buf) - 1); buf[sizeof(buf)-1] = '\0';
			if (ImGui::InputText("Text##txt", buf, sizeof(buf))) { txt->text = buf; trackEdit(); }
			ImGui::DragFloat("Font Size##txt", &txt->fontSize, 0.5f, 4.0f, 256.0f); trackEdit();
			ImGui::ColorEdit4("Color##txt",    glm::value_ptr(txt->color)); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<UITextComponent>(entity); }
	}

	// ── UI Image ─────────────────────────────────────────────────────────────
	if (auto* img = registry.try_get<UIImageComponent>(entity))
	{
		if (componentHeader("UI Image", true, removed))
		{
			ImGui::ColorEdit4("Tint##img", glm::value_ptr(img->tint)); trackEdit();
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<UIImageComponent>(entity); }
	}

	// ── UI Button ───────────────────────────────────────────────────────────
	if (auto* btn = registry.try_get<UIButtonComponent>(entity))
	{
		if (componentHeader("UI Button", true, removed))
		{
			ImGui::ColorEdit4("Normal##btn",  glm::value_ptr(btn->normalColor)); trackEdit();
			ImGui::ColorEdit4("Hovered##btn", glm::value_ptr(btn->hoveredColor)); trackEdit();
			ImGui::ColorEdit4("Pressed##btn", glm::value_ptr(btn->pressedColor)); trackEdit();
			char buf[128];
			strncpy(buf, btn->onClickFunction.c_str(), sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';
			if (ImGui::InputText("OnClick##btn", buf, sizeof(buf))) { btn->onClickFunction = buf; trackEdit(); }
		}
		if (removed) { if (ctx.undoSys) ctx.undoSys->snapshotNow(); registry.remove<UIButtonComponent>(entity); }
	}

	// ── Add Component ───────────────────────────────────────────────────────
	// Not for the World root — it only carries the scene's Environment, no
	// arbitrary components (and the built-in sun/moon are managed automatically).
	if (!ctx.world->isBuiltin(entity))
	{
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		const float buttonW = 180.0f;
		ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - buttonW) * 0.5f
		                     + ImGui::GetCursorPosX());
		if (ImGui::Button("Add Component", ImVec2(buttonW, 0)))
			ImGui::OpenPopup("##add_component");

		if (ImGui::BeginPopup("##add_component"))
		{
			auto addItem = [&]<typename T>(const char* label, T)
			{
				if (!registry.all_of<T>(entity) && ImGui::MenuItem(label))
				{
					if (ctx.undoSys) ctx.undoSys->snapshotNow();
					registry.emplace<T>(entity);
				}
			};
			addItem("Transform",    TransformComponent{});
			addItem("Transform 2D", Transform2DComponent{});
			addItem("Mesh",          MeshComponent{});
			addItem("Skeletal Mesh", SkeletalMeshComponent{});
			addItem("Animator",       AnimatorComponent{});
			addItem("Animator Blend",          AnimatorBlendComponent{});
			addItem("Animator State Machine",  AnimatorStateMachineComponent{});
			addItem("Property Animator",       PropertyAnimatorComponent{});
			addItem("Nav Mesh",                NavMeshComponent{});
			addItem("Nav Agent",               NavAgentComponent{});
			addItem("Material",     MaterialComponent{});
			addItem("Camera",       CameraComponent{});
			addItem("Light",        LightComponent{});
			addItem("Rigid Body",          RigidBodyComponent{});
			addItem("Collider",            ColliderComponent{});
			addItem("Character Controller", CharacterControllerComponent{});
			addItem("Script",         ScriptComponent{});
			addItem("Audio Source",    AudioSourceComponent{});
			addItem("Audio Listener",  AudioListenerComponent{});
			addItem("Particle System", ParticleSystemComponent{});
			addItem("LOD",             LODComponent{});
			addItem("Foliage",         FoliageComponent{});
			addItem("UI Canvas",       UICanvasComponent{});
			addItem("UI Element",      UIElementComponent{});
			addItem("UI Text",         UITextComponent{});
			addItem("UI Image",        UIImageComponent{});
			addItem("UI Button",       UIButtonComponent{});
			ImGui::EndPopup();
		}
	}

	ImGui::End();
#else
	(void)ctx;
#endif // HE_IMGUI_ENABLED
}