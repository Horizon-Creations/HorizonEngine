#include "ExportDialogPanel.h"
#include <algorithm>
#include <cstdint>
#include "BuildProgressDialog.h"         // the window a running export is watched in
#include "EditorApplication.h"           // AppContext, ProjectManager
#include "EditorWidgets.h"               // pinDialogToEditorWindow
#include "HcEditorUtil.h"                // asset enumeration for the codegen source set
#include "HorizonVersion.h"
#include <Hpak/ProjectExporter.h>
#include <HorizonScene/HcCodegen.h>      // HorizonCode → C++ codegen (compile-on-export)
#include <HorizonScene/HorizonScene.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ParticleGraph/ParticleGraph.h>
#include <material/MaterialShaderLibrary.h>
#include <MaterialGraph/MaterialGraph.h>
#include <HorizonCode/HorizonCode.h>
#include <Types/Enums.h>
#include <HorizonRendering/ParticleShaderTemplates.h>

#ifdef _WIN32
#include <windows.h>  // must come before any header that pulls in rpcdce.h
#endif

#include <Diagnostics/Logger.h>
#include <SDL3/SDL.h>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h> // InputText overloads for std::string
#endif

namespace ExportDialogPanel
{

// Build > Export Project modal state. Editable fields mirror the selected
// ExportProfile (persisted in the .heproj); the export itself runs on a worker
// thread so packing/compression never freezes the UI.
static bool   s_showExportModal   = false;
// Whether the modal actually drew last frame. Recomputed inside render() rather
// than derived from ImGui::IsPopupOpen, which hashes the id against whatever
// window happens to be current at the call site. Read through isOpen().
static bool   s_modalVisible      = false;
static int    s_exportProfileIdx  = 0;             // index into exportProfiles
static std::string s_exportOutputDir;
static bool   s_exportCompress    = true;
static bool   s_exportEncrypt     = false;
static bool   s_exportModSupport  = false;
static std::string s_exportExcludes;               // one glob pattern per line
static bool   s_exportIncremental = true;
static bool   s_exportAppBundle   = false;         // macOS .app bundle
static bool   s_exportCompileHC   = false;         // compile HorizonCode → C++ (Host targets, needs cmake + compiler)
static bool   s_exportHcStop      = false;         // a graph that will not compile fails the export
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
// Worker-thread state. What the user watches while it runs — the steps, each
// step's own progress, each step's log and the result — is BuildProgressDialog's
// model, written by the worker itself; this panel owns the settings and the
// thread, and nothing has to travel back here.
static std::atomic<bool> s_exportRunning{false};
static std::thread       s_exportThread;
// The run is started from outside the settings popup: a popup opened while
// another popup is the current window becomes its child and closes with it, so
// the Export button only asks and render() does it one level up.
static bool              s_startRequest = false;

namespace Build = BuildProgressDialog::Build;

// Defined below render(), which is where a run is started from.
void startExport(AppContext& ctx);

// The C++ step's progress, read off the toolchain's own output: ninja prints
// "[7/38] Building CXX object…", make "[ 45%] Building CXX object…". A line
// that is neither leaves the ring spinning — during cmake's configure phase
// there is genuinely nothing to measure, and inventing a number there would be
// a lie that stalls at 10 % for a minute.
static std::optional<float> parseToolchainProgress(const std::string& line)
{
	const auto open = line.find('[');
	if (open == std::string::npos || open > 4) return std::nullopt;
	const auto close = line.find(']', open);
	if (close == std::string::npos || close <= open + 1 || close - open > 16) return std::nullopt;

	std::string in;
	for (size_t i = open + 1; i < close; ++i)
		if (!std::isspace(static_cast<unsigned char>(line[i]))) in += line[i];
	if (in.empty()) return std::nullopt;

	if (in.back() == '%')
	{
		in.pop_back();
		if (in.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
		return std::clamp(std::stof(in) / 100.0f, 0.0f, 1.0f);
	}
	const auto slash = in.find('/');
	if (slash == std::string::npos) return std::nullopt;
	const std::string a = in.substr(0, slash), b = in.substr(slash + 1);
	if (a.empty() || b.empty() ||
	    a.find_first_not_of("0123456789") != std::string::npos ||
	    b.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
	const float total = std::stof(b);
	if (total <= 0.0f) return std::nullopt;
	return std::clamp(std::stof(a) / total, 0.0f, 1.0f);
}

bool isOpen()
{
	return s_modalVisible || BuildProgressDialog::isOpen();
}

void joinPendingExport()
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
			HE_LOG_WARN(Editor, "%s",
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

// Particle-system analogue of CompileMaterialShaderVariants above — but needs no
// shader cross-compiler at all: HE::generateParticleShaderSource hand-templates
// GLSL/MSL directly (see HorizonRendering::ParticleShaderTemplates), so this is
// pure text formatting. Only OpenGL/Metal are ever produced (GL+Metal-first, same
// as the rest of the particle system); other requested bits are silently skipped —
// the shipped game falls back to on-demand-compiling the same templates at
// runtime for those backends (HE_Rendering's getOrBuildParticleProgram).
static std::vector<uint8_t> CompileParticleShaderVariants(const std::string& nodeGraphJson, uint32_t backends)
{
	if (nodeGraphJson.empty() || backends == 0) return {};

	HE::ParticleGraph graph;
	if (!HE::particleGraphFromJson(nodeGraphJson, graph)) return {};
	// Deterministic seed so a RandomRange-driven Start/End Color bakes the SAME
	// value on every export — reproducible builds, matching how ParticleSystem-
	// Component seeds its own simulation rng ({42}) by default.
	std::mt19937 rng{ 42 };
	const HE::ParticleEmitterConfig config = HE::evaluateParticleGraph(graph, rng);

	std::vector<ParticleShaderVariant> variants;
	if (backends & (1u << static_cast<uint8_t>(HE::RendererBackend::OpenGL)))
	{
		const HE::ParticleShaderGen gen = HE::generateParticleShaderSource(config, /*metalSyntax*/false);
		ParticleShaderVariant var;
		var.backend  = static_cast<uint8_t>(HE::RendererBackend::OpenGL);
		var.vertex   = HE::buildParticleVertexGLSL(gen.colorFn, gen.alphaFn);
		var.fragment = HE::buildParticleFragmentGLSL();
		variants.push_back(std::move(var));
	}
	if (backends & (1u << static_cast<uint8_t>(HE::RendererBackend::Metal)))
	{
		const HE::ParticleShaderGen gen = HE::generateParticleShaderSource(config, /*metalSyntax*/true);
		ParticleShaderVariant var;
		var.backend  = static_cast<uint8_t>(HE::RendererBackend::Metal);
		var.vertex   = HE::buildParticleVertexMSL(gen.colorFn, gen.alphaFn);
		var.fragment = HE::buildParticleFragmentMSL();
		variants.push_back(std::move(var));
	}

	if (variants.empty()) return {};
	return HE::encodeParticleShaderVariants(variants);
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
	s_exportHcStop       = p.hcStopOnFailure;
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
	p.hcStopOnFailure    = s_exportHcStop;
}

void open(AppContext& ctx)
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

	s_exportBundleKey.clear(); // re-stat the runtime bundle on open
	s_showExportModal = true;
}

// ─── The modal itself (drawn every frame; also reaps a finished worker) ──────
void render(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
    s_modalVisible = false;   // set again below if BeginPopupModal draws this frame
    // ── Export Project modal ────────────────────────────────────────────────
    if (s_showExportModal)
    {
        ImGui::OpenPopup("Export Project##build");
        s_showExportModal = false;
    }
    {
        // Reap a finished export worker. This runs outside the modal so a
        // completed export is joined even if the popup was closed; its result is
        // already in the Build window's model, put there by the worker itself.
        if (!s_exportRunning.load() && s_exportThread.joinable())
            s_exportThread.join();

        // Holding the project still while the worker packs is the Build window's
        // job now — it is the modal that stays up for the whole run.

        ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Always);
        EditorWidgets::pinDialogToEditorWindow();
        if (ImGui::BeginPopupModal("Export Project##build", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        {
            s_modalVisible = true;
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
                //
                // Both lines below end in an absolute path, which is longer than
                // this dialog is wide — unwrapped, the user reads "Game runtime:
                // /Users/…/Horizon" and the part that says WHICH build is about to
                // ship is clipped off the right edge. Wrapped at an absolute column
                // rather than at the window edge: the popup is AlwaysAutoResize and
                // only the SetNextWindowSize above keeps its width from following
                // its own content, so a wrap at "the current width" is one dropped
                // line away from shrinking the dialog frame over frame. 550 = the
                // 560 the dialog is pinned to, minus the window padding.
                EditorWidgets::WrapText wrap(550.0f);
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

            // "Compile HorizonCode" needs cmake + a working compiler; disable it (and
            // force it off) when the startup probe couldn't find them, so the export
            // can't try a codegen build that would just fail. Null probe = not finished
            // yet → leave it enabled. Graphs still ship and run interpreted regardless.
            const bool hcCompileOk =
                !ctx.toolchainProbe ||
                (ctx.toolchainProbe->cmakeFound && ctx.toolchainProbe->compilerFound);
            if (!hcCompileOk) { s_exportCompileHC = false; ImGui::BeginDisabled(); }
            ImGui::Checkbox("Compile HorizonCode",   &s_exportCompileHC);
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Translate HorizonCode graphs (classes, widgets, level scripts,\n"
                                  "GameInstance) to native C++ and ship them as a compiled library.\n"
                                  "Needs cmake + a C++ toolchain on this machine, matching this\n"
                                  "editor build. Graphs always ship too: anything that fails\n"
                                  "validation or compilation runs interpreted, per asset.\n"
                                  "Host platform only (cross-targets ship interpreted).");
            // What an untranslatable graph means. The default keeps compiling an
            // optimization; the other makes "everything is native" a build
            // guarantee — which is also what lets calls between compiled classes
            // skip the Runtime's name-based seam.
            if (s_exportCompileHC)
            {
                ImGui::Indent();
                int mode = s_exportHcStop ? 1 : 0;
                if (ImGui::RadioButton("Interpret on failure", &mode, 0)) s_exportHcStop = false;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("A graph that cannot be translated ships interpreted.\n"
                                      "The build always succeeds; compiled and interpreted\n"
                                      "instances interoperate.");
                ImGui::SameLine();
                if (ImGui::RadioButton("Stop on failure", &mode, 1)) s_exportHcStop = true;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("A graph that cannot be translated FAILS the export,\n"
                                      "naming every offending graph and node. Use this when\n"
                                      "the packaged build must be fully native.");
                ImGui::Unindent();
            }
            if (!hcCompileOk) ImGui::EndDisabled();
            if (!hcCompileOk)
            {
                // The dialog's width is pinned (SetNextWindowSize above), so this
                // conditional line cannot widen it — the only thing it can do when
                // it does not fit is lose its tail, and the tail is the half that
                // says the export still works ("will ship interpreted"). Same
                // absolute column as the rest.
                EditorWidgets::WrapText wrap(550.0f);
                ImGui::TextDisabled("Disabled — no cmake/C++ compiler found. HorizonCode will ship interpreted.");
            }

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
                                  "Engine defaults are matched with their Engine/ prefix.\n"
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

            // Progress, the step list and the build log used to live here, below
            // the settings. They grew downwards while a build ran — the buttons
            // ended up off the bottom of a dialog nobody could close — and the
            // compiler's output shared one box with the packer's. They are the
            // Build window now (BuildProgressDialog), which this one hands over
            // to on Export and gets handed back from on "Build Settings".

            const bool canExport = !s_exportOutputDir.empty()
                                && ctx.contentManager && !running;
            if (!canExport) ImGui::BeginDisabled();
            if (EditorWidgets::primaryButton("Export", ImVec2(110, 0)))
            {
                // Asked for here, done in render() one level up — see s_startRequest.
                s_startRequest = true;
                ImGui::CloseCurrentPopup();
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

    BuildProgressDialog::render(ctx);

    // What the progress dialog's buttons asked for. Both run outside every
    // popup, for the same reason the Export button does.
    switch (BuildProgressDialog::takeAction())
    {
    case BuildProgressDialog::Action::Rebuild:     s_startRequest    = true; break;
    case BuildProgressDialog::Action::BackToSetup: s_showExportModal = true; break;
    case BuildProgressDialog::Action::None:                                  break;
    }

    if (s_startRequest && !s_exportRunning.load())
    {
        s_startRequest = false;
        startExport(ctx);
    }
#else
	(void)ctx;
#endif // HE_IMGUI_ENABLED
}

// ─── Starting a run ──────────────────────────────────────────────────────────
// Everything from "the user pressed Export" to a worker thread doing the work.
// A function of its own, not the button's body, because two buttons in two
// different windows start a run — Export here and "Build Again" in the progress
// dialog — and because the preparation below reads the project from disk, so a
// rebuild picks up whatever was saved since the last one.
//
// Note the shape: the first half runs on the UI thread (it needs the
// ContentManager and the editor's world) and does block the editor while it
// serializes every scene; only what follows is threaded. That freeze predates
// this dialog and is why the rings only appear once preparation is over.
void startExport(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
            ProjectManager* pm = ctx.projectManager;
            if (!pm || !ctx.contentManager) return;
            const std::filesystem::path projectRoot =
                std::filesystem::path(pm->currentProject().path).parent_path();
            {
                // HorizonCode → C++ codegen runs for Host exports only (v1: the
                // generated dylib is built with the host toolchain, plan §8.4).
                // Cross-platform targets ship interpreted with an explicit note.
                const bool hcCompile = s_exportCompileHC
                    && exportPlatformFromName(s_exportPlatform) == ExportPlatform::Host;
                if (s_exportCompileHC && !hcCompile)
                    HE_LOG_WARN(Editor, "%s",
                        "Export: HorizonCode compile skipped — target platform != host; "
                        "shipping interpreted.");

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
                std::vector<HE::hccg::ClassSource> hcSources;   // codegen inputs (hcCompile)
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
                            {
                                const std::string rel =
                                    sit->path().lexically_relative(projectRoot2).generic_string();
                                // Level script → codegen source, keyed by the scene's
                                // pak UUID (the same key the runtime derives, §9.1).
                                if (hcCompile)
                                    if (const std::string ls = w2.levelScriptJson(); !ls.empty())
                                    {
                                        HE::hccg::ClassSource src;
                                        src.key   = levelScriptKeyForUuid(sceneUuidForPath(rel));
                                        src.label = rel + " (level script)";
                                        if (HorizonCode::fromJson(ls, src.graph))
                                            hcSources.push_back(std::move(src));
                                    }
                                extraScenes.emplace_back(rel, std::move(bytes));
                            }
                            else
                                HE_LOG_WARN(Editor, "%s",
                                    ("Export: skipping unreadable scene " + sit->path().string()).c_str());
                        }
                        ec2.clear();
                        sit.increment(ec2);
                    }
                }

                // App-wide GameInstance graph (project GameInstance.hcode). Packed
                // into the hpak so the shipped game runs OnInit + the app lifecycle
                // (and whatever UI it creates). Read from the project root — the
                // same file the editor edits via the Game Instance tab.
                std::string gameInstanceJson;
                {
                    const std::filesystem::path giPath =
                        std::filesystem::path(ctx.projectManager->currentProject().path).parent_path()
                        / "GameInstance.hcode";
                    std::ifstream gif(giPath, std::ios::binary);
                    if (gif)
                        gameInstanceJson.assign(std::istreambuf_iterator<char>(gif),
                                                std::istreambuf_iterator<char>());
                }

                // Remaining codegen sources: every HC class + widget asset (keyed
                // by the content-relative path Create Object/Widget nodes store)
                // and the GameInstance graph. Collected on the main thread (they
                // need the ContentManager); generation + the C++ build run on
                // the export worker below.
                if (hcCompile)
                {
                    auto collectAssets = [&](HE::AssetType type)
                    {
                        for (const auto& c : HcEditorUtil::listAssets(ctx.contentManager, type))
                        {
                            const HE::UUID id = ctx.contentManager->loadAsset(c.path);
                            std::string json;
                            std::string baseClass;
                            if (type == HE::AssetType::HorizonCodeClass)
                            {
                                if (const auto* a = ctx.contentManager->getHorizonCodeClass(id))
                                {
                                    json      = a->graphJson;
                                    baseClass = a->baseClass;
                                }
                            }
                            else if (const auto* w = ctx.contentManager->getWidget(id))
                                json = w->graphJson;
                            if (json.empty()) continue;   // no logic → nothing to compile
                            HE::hccg::ClassSource src;
                            src.key       = c.path;
                            src.label     = c.path;
                            src.baseClass = baseClass;
                            if (HorizonCode::fromJson(json, src.graph))
                                hcSources.push_back(std::move(src));
                        }
                    };
                    collectAssets(HE::AssetType::HorizonCodeClass);
                    collectAssets(HE::AssetType::Widget);
                    if (!gameInstanceJson.empty())
                    {
                        HE::hccg::ClassSource src;
                        src.key   = kGameInstanceEntry;
                        src.label = "GameInstance.hcode";
                        if (HorizonCode::fromJson(gameInstanceJson, src.graph))
                            hcSources.push_back(std::move(src));
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

                // ── The run's shape is known now: build the step list and show it.
                //
                // Setup is already over by the time anyone sees it — the work above
                // runs on the UI thread — but it is where a scene that will not
                // load or a missing runtime is reported, so it is a step and not a
                // footnote. Build only exists when something is actually compiled:
                // an export with no HorizonCode to translate would otherwise show a
                // ring that turns green without ever having done anything.
                const bool hasBuild = hcCompile && !hcSources.empty();
                std::vector<std::string> stepNames{ "Setup" };
                if (hasBuild) stepNames.emplace_back("Build");
                stepNames.emplace_back("Package");
                stepNames.emplace_back("Clean up");
                const int stepSetup   = 0;
                const int stepBuild   = hasBuild ? 1 : -1;
                const int stepPackage = hasBuild ? 2 : 1;
                const int stepCleanup = stepPackage + 1;

                Build::begin(stepNames);
                Build::stepBegin(stepSetup);
                Build::log(0, std::to_string(extraScenes.size()) + " scene(s) serialized"
                              + (hasBuild ? ", " + std::to_string(hcSources.size())
                                            + " HorizonCode graph(s) collected"
                                          : std::string()));
                BuildProgressDialog::requestOpen();

                if (!sceneOk)
                {
                    const std::string m = "Error: startup scene could not be loaded: " + scenePath;
                    Build::log(2, m);
                    Build::finish(false, m);
                }
                else if (runtimeDir.empty())
                {
                    const std::string m = platform == ExportPlatform::Host
                        ? std::string("Error: no game runtime found — build the HorizonGame "
                                      "target, then export again.")
                        : "Error: no " + s_exportPlatform + " runtime bundle at "
                          + resolveRuntimeDir(base, platform).lexically_normal().string()
                          + " — build the game runtime on " + s_exportPlatform
                          + " and place it there.";
                    Build::log(2, m);
                    Build::finish(false, m);
                }
                else
                {
                ExportSettings es;
                es.compress         = s_exportCompress;
                es.encrypt          = s_exportEncrypt;
                es.enableModSupport = s_exportModSupport;
                es.excludePatterns  = parseExcludeLines(s_exportExcludes.c_str());
                es.incremental      = s_exportIncremental;
                // The project's default SaveGameTemplate rides into project.hcfg
                // so save.create() in the shipped game finds the same schema.
                if (ctx.projectManager)
                    es.defaultSaveTemplate = ctx.projectManager->currentProject().defaultSaveTemplate;
                es.appBundle        = s_exportAppBundle && exportAppBundleApplicable(s_exportPlatform);
                // Texture-compression cook target, chosen automatically from the
                // export target's GPU family (all encoding happens at pack time —
                // the shipped game only uploads the resulting blocks). Values are
                // HE::TextureFormat: 1 ASTC_4x4, 2 BC7, 3 BC3.
                //   • Apple target (Host/macOS on a Mac) → ASTC for Metal, BUT BC3
                //     when this editor runs the OpenGL backend, since macOS GL 4.1
                //     samples S3TC and neither ASTC nor BC7. A single pak can't
                //     serve both Metal (ASTC) and GL (BC3) on Apple Silicon; the
                //     active backend decides which one ships.
                //   • Windows/Linux desktop → BC7 (D3D11/D3D12/Vulkan and GL 4.6
                //     all sample it; best RGBA quality).
                // A format the target can't encode/sample degrades to RGBA8 in the
                // cook, and the runtime skips a format its GPU can't sample.
                uint8_t texComp;
                if (exportAppBundleApplicable(s_exportPlatform))
                    texComp = (ctx.backend == HE::RendererBackend::OpenGL)
                                  ? static_cast<uint8_t>(3)  // BC3 — macOS GL
                                  : static_cast<uint8_t>(1); // ASTC_4x4 — Metal
                else
                    texComp = static_cast<uint8_t>(2);       // BC7 — desktop D3D/Vulkan/GL
                es.textureCompression = texComp;
                es.gameRuntimeDir   = runtimeDir;
                // Engine-wide default content (primitive meshes, default materials)
                // ships INSIDE the pak under "Engine/…": it lives next to the editor,
                // not in the project, so without this the shipped game resolves no
                // built-in asset at all and every default primitive renders as the
                // renderers' fallback cube.
                es.engineContentDir = ctx.contentManager->engineContentRoot();
                // Ship the bundled Python stdlib (pythonXY.zip + ._pth) only for
                // Python-language projects — a non-Python game never inits Python, so
                // the ~10 MB stdlib is wasted. The libpython dylib/.so still ships
                // regardless (it's a load-time dependency of HorizonScene).
                es.bundlePythonStdlib =
                    ctx.projectManager &&
                    ctx.projectManager->currentProject().scriptLanguage == ProjectScriptLanguage::Python;
                // Precompile node-graph material shaders into the pak for the
                // selected backends (0 → runtime cross-compiles as before).
                es.shaderBackends        = s_exportShaderBackends;
                es.compileShaderVariants = &CompileMaterialShaderVariants;
                // Same bitmask + toggle UI as materials — particles only ever bake
                // OpenGL/Metal variants (see CompileParticleShaderVariants), so
                // selecting D3D/Vulkan here has no effect on particles.
                es.compileParticleShaderVariants = &CompileParticleShaderVariants;
                // Worker → UI: the packer's per-file counter fills the Package
                // ring, and the file itself is the activity line under the log.
                es.progress = [](int done, int total, const std::string& current)
                {
                    Build::stepProgress(done, total);
                    if (!current.empty()) Build::setActivity(current);
                };
                // …and the exporter's phases decide which ring that is. Packing
                // and shipping the runtime are Package; writing the config and
                // finishing (or codesigning) the bundle are Clean up. Both are
                // real work — an ad-hoc codesign of an .app takes seconds — which
                // is why they are two rings and not one that sits at 100 %.
                es.onStage = [stepPackage, stepCleanup](const char* what)
                {
                    const std::string s = what;
                    if (s == "pack")
                    {
                        Build::stepBegin(stepPackage);
                        Build::setActivity("Packing assets\xe2\x80\xa6");
                    }
                    else if (s == "binaries")
                    {
                        Build::setActivity("Copying the game runtime\xe2\x80\xa6");
                    }
                    else if (s == "config")
                    {
                        Build::stepBegin(stepCleanup);
                        Build::stepProgress(-1.0f);   // no measure — spin, don't guess
                        Build::setActivity("Writing project.hcfg\xe2\x80\xa6");
                    }
                    else if (s == "bundle")
                    {
                        Build::setActivity("Finishing the app bundle\xe2\x80\xa6");
                    }
                };

                const std::string projName = pm ? pm->currentProject().name : "Game";
                const std::string contentDir = ctx.contentManager->contentRoot();
                const std::string outDir     = effOutDir.string();
                const bool hostTarget        = platform == ExportPlatform::Host;

                s_exportRunning.store(true);
                if (s_exportThread.joinable()) s_exportThread.join(); // defensive; reaped above
                s_exportThread = std::thread([es, contentDir, projName, sceneName,
                                              outDir, sceneBinary, extraScenes,
                                              gameInstanceJson, hcCompile, hcSources,
                                              hcBase = base, stepBuild, hostTarget]()
                {
                    // An exception escaping a std::thread is std::terminate — and
                    // exportProject touches the filesystem (unreadable dirs,
                    // disk-full) and allocates compression buffers (bad_alloc).
                    std::string msg;
                    try
                    {
                        // ── HorizonCode → C++ (plan §8.1): generate + build the
                        // classes dylib BEFORE packing, so the exporter can ship
                        // it and the result line can report the outcome. Any
                        // failure here falls back to the interpreter loudly —
                        // the export itself never fails because of codegen
                        // (graphs are packed regardless).
                        ExportSettings esEff = es;
                        std::string hcMsg;
                        std::string hcFatal;   // Stop mode: why the export must not proceed
                        if (hcCompile && !hcSources.empty())
                        {
                            Build::stepBegin(stepBuild);
                            HE::hccg::Options opt;
                            opt.engineVersion = HE_VERSION_STRING;
                            opt.onFailure = s_exportHcStop ? HE::hccg::OnFailure::Stop
                                                           : HE::hccg::OnFailure::Interpret;
                            // Translating and compiling share the Build ring, so
                            // they share its 0..1: translation fills the first
                            // 40 %, the toolchain the rest. Two halves of one bar
                            // rather than a ring that reaches full and restarts.
                            opt.onClass = [](const std::string& label, size_t idx, size_t count)
                            {
                                if (count)
                                    Build::stepProgress(0.4f * static_cast<float>(idx + 1)
                                                             / static_cast<float>(count));
                                Build::setActivity("Translating " + label);
                                Build::log(0, "[" + std::to_string(idx + 1) + "/" +
                                              std::to_string(count) + "] Translating " + label);
                            };
                            const auto gen = HE::hccg::generate(hcSources, opt);
                            for (const auto& w : gen.warnings)
                            {
                                Build::log(1, "warning: " + w);
                                HE_LOG_WARN(Editor, "%s",
                                    ("HorizonCode codegen: " + w).c_str());
                            }
                            for (const auto& fb : gen.fallbacks)
                            {
                                const std::string where =
                                    fb.node ? " (node " + std::to_string(fb.node) + ")" : std::string();
                                Build::log(1, (s_exportHcStop ? "ERROR " : "INTERPRETED ") +
                                                fb.key + " — " + fb.reason + where);
                                if (s_exportHcStop)
                                    HE_LOG_ERROR(Editor, "%s",
                                        ("HorizonCode codegen: '" + fb.key +
                                         "' cannot be compiled — " + fb.reason).c_str());
                                else
                                    HE_LOG_WARN(Editor, "%s",
                                        ("HorizonCode codegen: '" + fb.key +
                                         "' ships interpreted — " + fb.reason).c_str());
                            }
                            // Stop mode: the graphs are the build's problem. Say
                            // which ones and stop before the toolchain runs —
                            // there is nothing to salvage, the point of the mode
                            // is that nothing falls back.
                            if (s_exportHcStop && !gen.fallbacks.empty())
                            {
                                hcFatal = std::to_string(gen.fallbacks.size()) +
                                    " HorizonCode graph(s) could not be compiled — "
                                    "fix them, or switch to \"Interpret on failure\".";
                                for (const auto& fb : gen.fallbacks)
                                    hcFatal += "\n  " + fb.key + ": " + fb.reason;
                            }
                            Build::log(0, std::to_string(hcSources.size() - gen.fallbacks.size()) +
                                            " class(es) translated, " +
                                            std::to_string(gen.fallbacks.size()) +
                                            " interpreted, " + std::to_string(gen.files.size()) +
                                            " file(s) generated");

                            const std::filesystem::path genDir =
                                std::filesystem::path(outDir) / "_hcgen";
                            std::error_code hcEc;
                            std::filesystem::create_directories(genDir, hcEc);
                            auto writeIfChanged = [](const std::filesystem::path& p,
                                                     const std::string& contents)
                            {
                                if (std::ifstream in(p, std::ios::binary); in)
                                {
                                    std::stringstream ss;
                                    ss << in.rdbuf();
                                    if (ss.str() == contents) return true;
                                }
                                std::ofstream f(p, std::ios::binary | std::ios::trunc);
                                if (!f) return false;
                                f << contents;
                                return (bool)f;
                            };
                            bool wroteAll = !hcEc;
                            std::vector<std::string> cppFiles;
                            for (const auto& file : gen.files)
                            {
                                if (file.name.size() > 4 &&
                                    file.name.compare(file.name.size() - 4, 4, ".cpp") == 0)
                                    cppFiles.push_back(file.name);
                                wroteAll &= writeIfChanged(genDir / file.name, file.contents);
                            }
                            wroteAll &= writeIfChanged(genDir / "CMakeLists.txt",
                                HE::hccg::generateCMakeLists(opt, cppFiles));

                            const int total    = (int)hcSources.size();
                            const int fellBack = (int)gen.fallbacks.size();
                            std::string buildLine;
                            if (!hcFatal.empty())
                            {
                                Build::log(2, hcFatal);
                                Build::stepFailed(stepBuild);
                            }
                            else if (!gen.ok || !wroteAll)
                            {
                                Build::log(2, "generation failed — shipping interpreted");
                                Build::stepFailed(stepBuild);
                                hcMsg = " — HorizonCode: generation failed, shipped interpreted";
                            }
                            else
                            {
                                Build::setActivity("Compiling the generated C++\xe2\x80\xa6");
                                Build::stepProgress(0.4f);
                                const auto sdk   = HE::hccg::resolveSdk(hcBase);
                                const auto built = HE::hccg::buildDylib(genDir, sdk,
                                    [](const std::string& line)
                                    {
                                        // Severity from the toolchain line itself so
                                        // compiler diagnostics pop in the output view.
                                        const int sev =
                                            line.find("error") != std::string::npos ? 2
                                          : line.find("warning") != std::string::npos ? 1 : 0;
                                        Build::log(sev, line);
                                        // The toolchain's own count, mapped into the
                                        // Build ring's second 60 %.
                                        if (const auto p = parseToolchainProgress(line))
                                            Build::stepProgress(0.4f + 0.6f * *p);
                                    });
                                if (built.ok)
                                {
                                    esEff.horizonCodeGenLib = built.artifact;
                                    hcMsg = " — HorizonCode: "
                                        + std::to_string(total - fellBack) + " compiled"
                                        + (fellBack ? ", " + std::to_string(fellBack)
                                                      + " interpreted (validation)"
                                                    : std::string());
                                    buildLine = "build: OK (" + built.artifact.filename().string() + ")";
                                    Build::log(0, "Built " + built.artifact.filename().string());
                                }
                                else
                                {
                                    HE_LOG_WARN(Editor, "%s",
                                        ("HorizonCode codegen: " + built.message).c_str());
                                    hcMsg = " — HorizonCode: compile failed, shipped "
                                            "interpreted (" + built.message + ")";
                                    buildLine = "build: FAILED — " + built.message;
                                    Build::log(2, built.message + " — shipping interpreted");
                                    Build::stepFailed(stepBuild);
                                }
                            }

                            // Persist the per-class report beside the generated
                            // sources — the durable answer to "what compiled,
                            // what fell back, and why" (the result line only
                            // carries the counts).
                            {
                                std::string rep = "HorizonCode compile report\n";
                                rep += "==========================\n";
                                rep += "classes: " + std::to_string(total)
                                     + ", compiled: " + std::to_string(total - fellBack)
                                     + ", interpreted: " + std::to_string(fellBack) + "\n";
                                if (!buildLine.empty()) rep += buildLine + "\n";
                                rep += "\n";
                                for (const auto& s : hcSources)
                                {
                                    const auto fb = std::find_if(gen.fallbacks.begin(), gen.fallbacks.end(),
                                        [&](const auto& f) { return f.key == s.key; });
                                    if (fb == gen.fallbacks.end())
                                        rep += "  COMPILED     " + s.key + "\n";
                                    else
                                        rep += "  INTERPRETED  " + s.key + " — " + fb->reason
                                             + (fb->node ? " (node " + std::to_string(fb->node) + ")" : std::string())
                                             + "\n";
                                }
                                if (!gen.warnings.empty())
                                {
                                    rep += "\nwarnings:\n";
                                    for (const auto& w : gen.warnings) rep += "  " + w + "\n";
                                }
                                writeIfChanged(genDir / "hc_report.txt", rep);
                            }
                        }

                        // Stop mode said no: nothing gets packed. The worker's own
                        // failure path reports it and finishes the step list.
                        if (!hcFatal.empty()) throw std::runtime_error(hcFatal);

                        // Package and Clean up are entered by the exporter itself,
                        // through es.onStage — it is the only thing that knows when
                        // packing ends and finishing begins.
                        const auto res = ProjectExporter::exportProject(
                            contentDir, projName, sceneName,
                            std::filesystem::path(outDir), esEff, sceneBinary, extraScenes,
                            gameInstanceJson);
                        if (res.success)
                            Build::log(0, "Packed " + std::to_string(res.assetsPacked) +
                                          " asset(s) (" + std::to_string(res.assetsReused) +
                                          " reused), " + std::to_string(res.binaryFilesCopied) +
                                          " runtime file(s) copied");
                        else
                            Build::log(2, res.errorMessage);
                        // Only a host-target export produces a binary this machine
                        // can run; a Windows build made on a Mac is a file, not a
                        // program, and the dialog's Start Game says so.
                        Build::setLaunchTarget(res.executablePath, hostTarget);
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
                        if (res.success) msg += hcMsg;
                    }
                    catch (const std::exception& e)
                    {
                        msg = std::string("Error: ") + e.what();
                    }
                    catch (...)
                    {
                        msg = "Error: export failed with an unknown error";
                    }
                    const bool ok = msg.rfind("OK:", 0) == 0;
                    Build::log(ok ? 0 : 2, msg);
                    Build::finish(ok, msg);
                    s_exportRunning.store(false); // last: UI may join right after
                });
                }
            }
#else
	(void)ctx;
#endif // HE_IMGUI_ENABLED
}

} // namespace ExportDialogPanel
