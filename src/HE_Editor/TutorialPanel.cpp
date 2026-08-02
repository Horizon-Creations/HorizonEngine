#include "TutorialPanel.h"
#include "TutorialSteps.h"
#include "EditorApplication.h"   // AppContext, EditorConfig, ProjectManager
#include "EditorWidgets.h"       // dialog placement (stay inside the editor window)
#include "EditorAssetTypeCache.h" // asset-type sniff for the create/open checks
#include "HorizonVersion.h"

#include <HorizonScene/HorizonWorld.h>
#include <HorizonCode/HorizonCode.h>   // level-script / game-instance graph node counts
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/LightComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/NavMeshComponent.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/AudioSourceComponent.h>
#include <HorizonScene/Components/AnimatorComponent.h>
#include <HorizonScene/Components/UICanvasComponent.h>

#include <Diagnostics/GlobalState.h>
#include <Diagnostics/Logger.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName — used to outline the step's panel
#include <misc/cpp/imgui_stdlib.h>
#endif

namespace fs = std::filesystem;
namespace tut = HE::tut;

namespace TutorialPanel
{

namespace
{
	// ── Persisted state ───────────────────────────────────────────────────────
	// "Tutorial.Offered" is the first-start gate: once the welcome card has been
	// answered (either way) it never reappears on its own. "Tutorial.Step" is the
	// serialized cursor — a step id, so inserting steps in a later release does not
	// move anybody's saved position.
	constexpr const char* kCfgOffered = "Tutorial.Offered";
	constexpr const char* kCfgStep    = "Tutorial.Step";

	bool         s_open        = false;
	bool         s_forceWelcome = false;  // re-offer the sandbox even once answered
	bool         s_loaded      = false;   // cursor read back from the config yet?
	tut::Cursor  s_cursor      = {};
	tut::Signals s_base;                  // snapshot from when the current step opened
	bool         s_baseValid   = false;
	bool         s_stepDone    = false;   // the current step's check fired
	float        s_doneTimer   = 0.0f;    // seconds since it fired (drives auto-advance)
	bool         s_ackPressed  = false;   // ReadAck: the acknowledge button was pressed
	bool         s_readToEnd   = false;   // ReadAck: the body has been scrolled through
	// Panels focused since the current step opened, '\n'-delimited and
	// '\n'-terminated (see Check::PanelsVisited). Cleared by gotoCursor.
	std::string  s_visitedPanels;
	int          s_playSessions = 0;      // play→stop transitions since the editor started
	bool         s_wasPlaying  = false;

	// Seconds a completed step stays on screen before the tour moves on. Long
	// enough that the user sees WHICH step they just finished, short enough that
	// it never feels like waiting.
	constexpr float kAutoAdvanceDelay = 1.4f;

	void persist(GlobalState* gs)
	{
		if (!gs) return;
		gs->setCustomConfigEntry(kCfgStep, tut::serialize(s_cursor));
		gs->writeConfig();
	}

	void loadOnce(GlobalState* gs)
	{
		if (s_loaded || !gs) return;
		s_cursor = tut::deserialize(gs->getCustomConfigString(kCfgStep, ""));
		s_loaded = true;
	}

	// Every piece of per-step state is reset here — a step's baseline, whether its
	// check has fired, and the two ReadAck flags. Missing one of these is how a
	// step would arrive pre-completed, which is exactly what this tour must not do.
	void gotoCursor(tut::Cursor c, GlobalState* gs)
	{
		s_cursor    = tut::clamp(c);
		s_baseValid = false;
		s_stepDone  = false;
		s_doneTimer = 0.0f;
		s_ackPressed = false;
		s_readToEnd  = false;
		s_visitedPanels.clear();
		persist(gs);
	}

	// HE::AssetType → the tour's own Asset enum. Only the kinds a step asks for
	// map; everything else is Asset::Count and is simply not counted.
	tut::Asset tutAsset(HE::AssetType t)
	{
		switch (t)
		{
		case HE::AssetType::Material:             return tut::Asset::Material;
		case HE::AssetType::MaterialFunction:     return tut::Asset::MaterialFunction;
		case HE::AssetType::ParticleSystem:       return tut::Asset::ParticleSystem;
		case HE::AssetType::Widget:               return tut::Asset::Widget;
		case HE::AssetType::AnimatorStateMachine: return tut::Asset::AnimatorStateMachine;
		case HE::AssetType::InputAction:          return tut::Asset::InputAction;
		case HE::AssetType::InputMappingContext:  return tut::Asset::InputMappingContext;
		case HE::AssetType::HorizonCodeClass:     return tut::Asset::HorizonCodeClass;
		case HE::AssetType::Scene:                return tut::Asset::Scene;
		case HE::AssetType::Texture:              return tut::Asset::Texture;
		case HE::AssetType::StaticMesh:           return tut::Asset::StaticMesh;
		case HE::AssetType::SkeletalMesh:         return tut::Asset::SkeletalMesh;
		case HE::AssetType::Script:               return tut::Asset::Script;
		case HE::AssetType::Audio:                return tut::Asset::Audio;
		case HE::AssetType::Font:                 return tut::Asset::Font;
		case HE::AssetType::Prefab:               return tut::Asset::Prefab;
		case HE::AssetType::AnimationClip:        return tut::Asset::AnimationClip;
		default:                                  return tut::Asset::Count;
		}
	}

	// ── Sampling the live editor ──────────────────────────────────────────────
	// Everything a completion check may look at, gathered once per frame. Kept in
	// one place so the checks stay pure functions over data (see TutorialSteps.h)
	// and adding a check never means reaching into another panel.
	//
	// `withAssets` gates the expensive part — walking the whole content tree under
	// its shared lock, and sniffing each file's asset type — to the step kinds that
	// need it. Both the baseline snapshot and the live one are taken for the SAME
	// step, so gating cannot produce a spurious "an asset appeared": either both
	// counts are real or both are zero.
	tut::Signals sample(AppContext& ctx, const UiFlags& flags, bool withAssets)
	{
		tut::Signals s;

		if (ctx.world)
		{
			auto& reg = ctx.world->registry();
			// Every entity carries a NameComponent (HorizonWorld::createEntity and the
			// scene loader both set one), so this view is the entity count.
			s.entityCount = static_cast<int>(reg.view<NameComponent>().size());

			// Counts, not "any": a furnished scene already has a light and a mesh, so
			// "does one exist" would tick those steps off before the user acted. The
			// checks want one MORE than when the step opened.
			auto count = [&](tut::Comp c, size_t n) { s.add(c, static_cast<int>(n)); };
			count(tut::Comp::Mesh,           reg.view<MeshComponent>().size());
			count(tut::Comp::Material,       reg.view<MaterialComponent>().size());
			count(tut::Comp::Light,          reg.view<LightComponent>().size());
			count(tut::Comp::RigidBody,      reg.view<RigidBodyComponent>().size());
			count(tut::Comp::Collider,       reg.view<ColliderComponent>().size());
			count(tut::Comp::ParticleSystem, reg.view<ParticleSystemComponent>().size());
			count(tut::Comp::Script,         reg.view<ScriptComponent>().size());
			count(tut::Comp::Terrain,        reg.view<TerrainComponent>().size());
			count(tut::Comp::Foliage,        reg.view<FoliageComponent>().size());
			count(tut::Comp::NavMesh,        reg.view<NavMeshComponent>().size());
			count(tut::Comp::Camera,         reg.view<CameraComponent>().size());
			count(tut::Comp::AudioSource,    reg.view<AudioSourceComponent>().size());
			count(tut::Comp::Animator,       reg.view<AnimatorComponent>().size());
			count(tut::Comp::UICanvas,       reg.view<UICanvasComponent>().size());

			// "The material slot was filled in", not "a Material component exists" —
			// the component is added empty one step earlier.
			for (auto [e, mat] : reg.view<MaterialComponent>().each())
				if (mat.materialAssetId != HE::UUID{}) ++s.materialsAssigned;

			s.selectionSet = ctx.selectedEntity != entt::null &&
			                 reg.valid(ctx.selectedEntity);
			if (s.selectionSet)
				s.selectedEntity = static_cast<uint32_t>(entt::to_integral(ctx.selectedEntity));

			// Sky time of day. environmentEntity() is the one Sky in the scene; with
			// no Sky the step cannot be satisfied at all (skyPresent gates it), which
			// is right — there is no slider to drag.
			// HorizonCode: the two graphs every project has, whatever its scripting
			// language. Both are plain data on the world/app, so the tour reads them
			// directly instead of asking the tab panels (which do not run while the
			// user is on another tab).
			const HorizonCode::Graph& ls = ctx.world->levelScript();
			s.hcNodes     = static_cast<int>(ls.nodes.size());
			s.hcVariables = static_cast<int>(ls.variables.size());

			const entt::entity sky = ctx.world->environmentEntity();
			if (sky != entt::null && reg.valid(sky) && reg.all_of<EnvironmentComponent>(sky))
			{
				s.skyPresent = true;
				s.timeOfDay  = reg.get<EnvironmentComponent>(sky).timeOfDay;
			}
		}

		if (withAssets && ctx.globalState)
		{
			auto [folder, lock] = ctx.globalState->lockContentFolder();
			// Iterative, not recursive: the content tree is user-shaped and a deep
			// nesting must not put a stack overflow between a user and their tutorial.
			std::vector<const HE::Folder*> stack{ &folder };
			while (!stack.empty())
			{
				const HE::Folder* f = stack.back();
				stack.pop_back();
				if (!f) continue;
				s.assetCount += static_cast<int>(f->files.size());
				for (const HE::File* file : f->files)
				{
					if (!file) continue;
					// Scenes are identified by extension, as everywhere else in the
					// editor. A scene SAVED by the serializer is JSON with no HAsset
					// header, so the header sniff below reports Unknown for it — the
					// "create a Scene" step would then never see one appear.
					if (file->extension == ".hescene") { s.add(tut::Asset::Scene); continue; }
					// Cached per path (EditorAssetTypeCache), so this is one header
					// sniff per asset for the lifetime of the content tree, not one
					// per frame.
					const tut::Asset a = tutAsset(EditorAssetTypeCache::assetTypeOf(file->fullPath));
					if (a != tut::Asset::Count) s.add(a);
				}
				for (const HE::Folder* sub : f->subfolders) stack.push_back(sub);
			}
		}

#ifdef HE_IMGUI_ENABLED
		for (const AppContext::EditorTab& t : ctx.tabs)
		{
			s.openTabs += t.label;
			s.openTabs += '\n';
			s.openTabs += t.assetPath;
			s.openTabs += '\n';
			// By type, not by label: the tour tells the user to CREATE the asset and
			// the create flow lets them name it, so matching "Material" against the
			// tab title fails for everyone who typed their own name.
			if (!t.assetPath.empty())
			{
				const tut::Asset a = tutAsset(EditorAssetTypeCache::assetTypeOf(t.assetPath));
				if (a != tut::Asset::Count) s.addTab(a);
			}
		}
		s.visitedPanels = s_visitedPanels;
#endif

		if (ctx.editorCamera)
		{
			const glm::vec3 p = ctx.editorCamera->position();
			s.camX     = p.x;
			s.camY     = p.y;
			s.camZ     = p.z;
			s.camYaw   = ctx.editorCamera->yaw();
			s.camPitch = ctx.editorCamera->pitch();
			s.camPivot = ctx.editorCamera->pivotDistance();
		}

		if (ctx.gameInstanceGraph)
		{
			s.hcNodes     += static_cast<int>(ctx.gameInstanceGraph->nodes.size());
			s.hcVariables += static_cast<int>(ctx.gameInstanceGraph->variables.size());
		}

		s.playing         = ctx.isPlaying;
		s.playSessions    = s_playSessions;
		s.undoCount       = ctx.undoSys ? static_cast<int>(ctx.undoSys->undoCount()) : 0;
		s.sceneUnsaved    = ctx.sceneDirty || ctx.currentScenePath.empty();
		s.landscapeMode   = ctx.editorConfig.mode == EditorMode::Landscape;
		s.preferencesOpen = flags.preferencesOpen;
		s.profilerOpen    = flags.profilerOpen;
		s.environmentOpen = flags.environmentOpen;
		s.exportOpen      = flags.exportOpen;
		s.importOpens     = flags.importDialogOpens;
		s.contentRootKind = flags.contentRootKind;
		s.acknowledged    = s_ackPressed;
		return s;
	}

#ifdef HE_IMGUI_ENABLED
	// ── Panel highlight ───────────────────────────────────────────────────────
	// The outline has to land on the panel the step is actually talking about, in
	// every layout the user can produce. Three things make that non-obvious:
	//
	//  * A DOCKED window's Pos/Size is the node's *inner* rect — it excludes the
	//    tab bar. Outlining that draws a box that does not line up with what the
	//    user perceives as the panel, so the dock node's rect is used instead.
	//  * A docked window whose tab is NOT selected is inactive and has a stale
	//    rect. Outlining it would put the box on top of whatever tab IS showing —
	//    the wrong panel entirely. The node is outlined in that case, so the box
	//    frames the tab bar the user has to click.
	//  * A window that does not exist yet (never opened, or opened into another
	//    viewport) must not be outlined at all rather than at {0,0}.
	//
	// Drawn on the target viewport's foreground list, so it sits over docked
	// windows and lands in the right OS window when a panel was dragged out.
	//
	// Returns false when there was nothing to outline, which is how the card knows
	// to tell the user the panel is closed instead of silently pointing at nothing.
	bool outlineWindow(const char* name, float time, bool dimmed)
	{
		if (!name || name[0] == '\0') return false;
		ImGuiWindow* w = ImGui::FindWindowByName(name);
		if (!w) return false;

		ImVec2 pos, size;
		if (ImGuiDockNode* node = w->DockNode; node && node->HostWindow)
		{
			// The whole docked slot, tab bar included — and valid even while another
			// tab of the same node is the visible one. The host must be on screen,
			// though: a node inside a hidden host has a stale rect that would put the
			// outline somewhere the user is not looking.
			if (!node->HostWindow->WasActive) return false;
			pos  = node->Pos;
			size = node->Size;
		}
		else
		{
			if (!w->WasActive || w->Hidden || w->Collapsed) return false;
			pos  = w->Pos;
			size = w->Size;
		}
		if (size.x < 8.0f || size.y < 8.0f) return false;

		const float pulse = dimmed ? 0.22f
		                           : 0.45f + 0.35f * (0.5f + 0.5f * std::sin(time * 3.2f));
		const ImVec4 col = dimmed ? ImVec4(0.45f, 0.85f, 0.55f, pulse)   // already done
		                          : ImVec4(0.45f, 0.72f, 1.00f, pulse);
		ImDrawList* dl = ImGui::GetForegroundDrawList(w->Viewport);
		// Inset by half the stroke so the rectangle sits ON the panel edge instead
		// of half outside it (which reads as covering the neighbouring panel).
		const float inset = 2.0f;
		dl->AddRect(ImVec2(pos.x + inset, pos.y + inset),
		            ImVec2(pos.x + size.x - inset, pos.y + size.y - inset),
		            ImGui::GetColorU32(col), 6.0f, 0, 3.0f);
		return true;
	}

	// Outline every '|'-separated entry of `list`. On a PanelsVisited step the ones
	// already clicked are drawn dim and green, so the card's "2 of 4" and the
	// highlights always agree on what is left. Matched by NAME rather than by list
	// position — focusWindow and the check's argument are two separate lists and
	// must not be assumed to be in the same order.
	int outlineWindows(const char* list, bool dimVisited, float time,
	                   std::string_view visited)
	{
		if (!list || list[0] == '\0') return 0;
		int drawn = 0;
		const int n = tut::listEntryCount(list);
		for (int i = 0; i < n; ++i)
		{
			const std::string name(tut::listEntry(list, i));
			if (name.empty()) continue;
			const bool done = dimVisited && tut::panelVisited(name, visited);
			if (outlineWindow(name.c_str(), time, done)) ++drawn;
		}
		return drawn;
	}

	// The window the user last clicked into, as the tour's "visited" signal.
	// RootWindow deliberately does NOT cross dock nodes, so a docked panel reports
	// itself while a child window inside it (the Content Browser's asset grid, the
	// Details scroll region) reports the panel — which is what the user clicked.
	//
	// Everything from "###" on is stripped: the left panel is "Landscape###Quick
	// Settings" in Landscape mode and "Quick Settings###Quick Settings" otherwise,
	// and the step tables name panels by the stable id, not by the visible title.
	std::string focusedPanelName()
	{
		ImGuiContext* g = ImGui::GetCurrentContext();
		if (!g || !g->NavWindow) return {};
		const ImGuiWindow* w = g->NavWindow->RootWindow ? g->NavWindow->RootWindow
		                                                : g->NavWindow;
		if (!w->Name) return {};
		std::string name = w->Name;
		if (const size_t hash = name.find("###"); hash != std::string::npos)
			name = name.substr(hash + 3);
		return name;
	}

	// TextWrapped over a body whose paragraphs are '\n'-separated, with a blank
	// line between them — ImGui's own wrapping keeps hard newlines, which would
	// otherwise run the paragraphs together.
	void drawParagraphs(const char* body)
	{
		const char* p = body;
		bool first = true;
		while (p && *p)
		{
			const char* nl = std::strchr(p, '\n');
			const char* end = nl ? nl : p + std::strlen(p);
			if (!first) ImGui::Spacing();
			ImGui::TextWrapped("%.*s", static_cast<int>(end - p), p);
			first = false;
			if (!nl) break;
			p = nl + 1;
		}
	}

	// Where a brand-new tutorial project should be proposed. Documents is the least
	// surprising place; the pref path (always writable) is the fallback so the
	// welcome card never offers a directory that cannot be created.
	std::string defaultProjectDir()
	{
		if (const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS); docs && *docs)
			return (fs::path(docs) / "HorizonEngine").string();
		if (char* pref = SDL_GetPrefPath("HorizonCreations", "HorizonEngine"))
		{
			std::string out = pref;
			SDL_free(pref);
			return out;
		}
		return {};
	}
#endif // HE_IMGUI_ENABLED

} // namespace

void open()
{
	s_open = true;
	// A finished tour reopens from the top — "Interactive Tutorial" that shows a
	// single "you are done" card would be a dead menu item.
	if (tut::finished(s_cursor)) s_cursor = tut::Cursor{ 0, 0 };
	s_baseValid = false;
	s_stepDone  = false;
	s_doneTimer = 0.0f;
#ifdef HE_IMGUI_ENABLED
	// Help ▸ Interactive Tutorial on an already-open tour means "show it to me":
	// pull it in front of whatever floating window is covering it. No-op the first
	// time, when the window does not exist yet (it then appears on top anyway).
	ImGui::SetWindowFocus("Tutorial");
#endif
}

bool isOpen() { return s_open; }

void showWelcome() { s_forceWelcome = true; }

// ─── First-start welcome (Project Hub) ────────────────────────────────────────
void renderWelcome(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	if (!ctx.globalState) return;
	loadOnce(ctx.globalState);
	if (ctx.globalState->getCustomConfigBool(kCfgOffered, false) && !s_forceWelcome)
		return;

	static std::string s_dir;
	static std::string s_name  = "HorizonTutorial";
	static std::string s_error;
	if (s_dir.empty()) s_dir = defaultProjectDir();

	ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Always);
	// Pinned to the editor window so the card can never protrude, get its own OS
	// window and end up buried behind the editor on the next focus change.
	EditorWidgets::pinDialogToEditorWindow();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 18.0f));
	const bool visible = ImGui::BeginPopupModal("##TutorialWelcome", nullptr,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
	ImGui::PopStyleVar();
	if (!visible)
	{
		ImGui::OpenPopup("##TutorialWelcome");
		return;
	}

	if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
	ImGui::TextUnformatted("Welcome to Horizon Engine " HE_VERSION_STRING);
	if (ctx.fontSubheading) ImGui::PopFont();
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::TextWrapped(
		"First time here? The interactive tutorial walks once through the whole "
		"editor — scenes and entities, assets and materials, terrain, physics, "
		"particles, animation, UI, gameplay logic, playing and packaging — in a "
		"sandbox project it creates for you.");
	ImGui::Spacing();
	ImGui::TextWrapped(
		"It is an ordinary project: everything you build while following along is "
		"yours to keep. You can leave and resume at any point.");
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::TextUnformatted("Project Name");
	ImGui::SetNextItemWidth(-1);
	ImGui::InputText("##twName", &s_name);
	ImGui::Spacing();
	ImGui::TextUnformatted("Created in");
	ImGui::SetNextItemWidth(-1);
	ImGui::InputText("##twDir", &s_dir);

	if (!s_error.empty())
	{
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
		ImGui::TextWrapped("%s", s_error.c_str());
		ImGui::PopStyleColor();
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// From the actual content width, not the nominal 560: the card is capped to
	// the editor window, so on a small editor it is narrower than it asked for.
	const float btnW = (ImGui::GetContentRegionAvail().x - 8.0f) * 0.5f;
	if (ImGui::Button("Start the tutorial", ImVec2(btnW, 34.0f)))
	{
		s_error.clear();
		if (s_name.empty() || s_dir.empty())
		{
			s_error = "Please give the project a name and a directory.";
		}
		else if (!ctx.projectManager)
		{
			s_error = "No project manager available.";
		}
		else
		{
			const fs::path projRoot = fs::path(s_dir) / s_name;
			if (ctx.projectManager->createNewProject(projRoot.string(), s_name,
			        ProjectPreset::Tutorial, ProjectScriptLanguage::HorizonCode))
			{
				ctx.globalState->addKnownProject(ctx.projectManager->currentProject().path);
				ctx.globalState->setCustomConfigEntry(kCfgOffered, true);
				gotoCursor(tut::Cursor{ 0, 0 }, ctx.globalState);   // also writes the config
				ctx.contentRefreshPending = true;
				ctx.projectLoaded         = true;
				s_open                    = true;
				s_forceWelcome            = false;
				HE_LOG_INFO(Editor, "%s",
					("Tutorial: created sandbox project at " + projRoot.string()).c_str());
				ImGui::CloseCurrentPopup();
			}
			else
			{
				s_error = "Could not create the project there. Check the path and permissions.";
			}
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Not now", ImVec2(btnW, 34.0f)))
	{
		// Answered once = never offered again unprompted. Help ▸ Interactive
		// Tutorial is how it comes back.
		ctx.globalState->setCustomConfigEntry(kCfgOffered, true);
		ctx.globalState->writeConfig();
		s_forceWelcome = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::Spacing();
	ImGui::TextDisabled("You can start it later from Help - Interactive Tutorial.");

	ImGui::EndPopup();
#else
	(void)ctx;
#endif // HE_IMGUI_ENABLED
}

// ─── The tour itself ──────────────────────────────────────────────────────────
void render(AppContext& ctx, float dt, const UiFlags& flags)
{
#ifdef HE_IMGUI_ENABLED
	loadOnce(ctx.globalState);

	// Play sessions are counted whether or not the window is open: the user may
	// well press Play before opening the tutorial, and the "play then stop" step
	// should not then wait for a second session.
	if (s_wasPlaying && !ctx.isPlaying) ++s_playSessions;
	s_wasPlaying = ctx.isPlaying;

	if (!s_open) return;

	const tut::Step*    step = tut::stepAt(s_cursor);
	const tut::Chapter* chap = tut::chapterAt(s_cursor);

	// Which panel the user is in, accumulated for the current step. Recorded
	// before sampling so a click this frame counts this frame, and skipped while
	// the tutorial card itself has focus — clicking Next must not tick off a
	// "visit the panels" step.
	{
		const std::string focused = focusedPanelName();
		if (!focused.empty() && focused != "Tutorial" &&
		    !tut::panelVisited(focused, s_visitedPanels))
		{
			s_visitedPanels += focused;
			s_visitedPanels += '\n';
		}
	}

	const bool needsAssetScan = step && (step->check == tut::Check::AssetAdded ||
	                                     step->check == tut::Check::AssetOfTypeAdded);
	const tut::Signals now = sample(ctx, flags, needsAssetScan);
	if (!s_baseValid) { s_base = now; s_baseValid = true; }

	// Completion is latched: a check that fired must not un-fire because the user
	// deselected the entity again while reading the confirmation.
	if (step && !s_stepDone && tut::satisfied(*step, s_base, now))
		s_stepDone = true;

	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowSize(ImVec2(430.0f, 340.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(
		ImVec2(vp->WorkPos.x + vp->WorkSize.x - 450.0f,
		       vp->WorkPos.y + vp->WorkSize.y - 380.0f),
		ImGuiCond_FirstUseEver);
	// Capped to the editor window: a floating window that protrudes gets its own
	// OS window, which the window manager is free to bury behind the editor on the
	// next focus change — the tour would then be "open" but invisible.
	ImGui::SetNextWindowSizeConstraints(
		ImVec2(340.0f, 220.0f),
		ImVec2(std::max(340.0f, std::min(900.0f,  vp->WorkSize.x - 16.0f)),
		       std::max(220.0f, std::min(1200.0f, vp->WorkSize.y - 16.0f))));

	bool open = true;
	// NoDocking on purpose: the tour points at the docked panels, so it has to
	// float above them instead of becoming one of them.
	if (!ImGui::Begin("Tutorial", &open,
	        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse))
	{
		ImGui::End();
		if (!open) s_open = false;
		return;
	}
	// Draggable, but not off the editor window (same reason as the size cap).
	EditorWidgets::clampCurrentWindowToEditorWindow();

	if (!step || !chap)
	{
		// Finished.
		if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
		ImGui::TextUnformatted("Tour complete");
		if (ctx.fontSubheading) ImGui::PopFont();
		ImGui::Separator();
		ImGui::Spacing();
		ImGui::TextWrapped(
			"You have been through every chapter. Help - Interactive Tutorial starts "
			"it again from the top whenever you want a refresher.");
		ImGui::Spacing();
		if (ImGui::Button("Start over", ImVec2(-1.0f, 30.0f)))
			gotoCursor(tut::Cursor{ 0, 0 }, ctx.globalState);
		ImGui::End();
		if (!open) s_open = false;
		return;
	}

	// ── Header: chapter + progress ────────────────────────────────────────────
	const int done  = tut::flatIndex(s_cursor);
	const int total = tut::totalSteps();
	ImGui::TextDisabled("Chapter %d/%d  -  %s",
		s_cursor.chapter + 1, tut::chapterCount(), chap->title);
	ImGui::ProgressBar(total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 0.0f,
		ImVec2(-1.0f, 6.0f), "");
	ImGui::Spacing();

	if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
	ImGui::TextWrapped("%s", step->title);
	if (ctx.fontSubheading) ImGui::PopFont();
	ImGui::Spacing();

	// ── Body ──────────────────────────────────────────────────────────────────
	// Reserve room for the action line, the status line and the button row so the
	// body text scrolls instead of pushing the controls off the bottom. The action
	// line is MEASURED rather than assumed to be one line — several of them wrap to
	// two or three at a narrow window width, and a fixed reservation put the
	// buttons out of reach exactly there.
	const std::string actionLine = std::string("> ") + step->action;
	const float availW  = ImGui::GetContentRegionAvail().x;
	const float actionH = step->action[0] != '\0'
		? ImGui::CalcTextSize(actionLine.c_str(), nullptr, false, availW).y +
		  ImGui::GetStyle().ItemSpacing.y
		: 0.0f;
	const float footerH = ImGui::GetFrameHeightWithSpacing()        // button row
	                    + ImGui::GetTextLineHeightWithSpacing()     // status line
	                    + actionH
	                    + ImGui::GetStyle().ItemSpacing.y * 2.0f;   // separator + padding
	ImGui::BeginChild("##tutBody", ImVec2(0.0f, -footerH), ImGuiChildFlags_None);
	drawParagraphs(step->body);
	// A prose card unlocks its button once the body has actually been read to the
	// end. A body that fits without scrolling has MaxY == 0 and counts as read
	// immediately — there is nothing to scroll past.
	if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) s_readToEnd = true;
	ImGui::EndChild();

	// ── What to do ────────────────────────────────────────────────────────────
	const bool isReadCard = step->check == tut::Check::ReadAck;

	ImGui::Separator();
	if (step->action[0] != '\0')
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.80f, 1.0f, 1.0f));
		ImGui::TextWrapped("%s", actionLine.c_str());
		ImGui::PopStyleColor();
	}

	// The status line is the only place the tour explains itself, so it has to
	// name the *reason* a step is still open — "waiting" on its own is what makes
	// a gated tutorial feel broken.
	if (s_stepDone)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.90f, 0.55f, 1.0f));
		ImGui::TextUnformatted("Done.");
		ImGui::PopStyleColor();
	}
	else if (isReadCard)
	{
		ImGui::TextDisabled(s_readToEnd ? "Ready when you are."
		                                : "Scroll to the end of the card to continue.");
	}
	else if (step->check == tut::Check::TimeOfDayChanged && !now.skyPresent)
	{
		// The one step whose subject can be missing from the scene. Say so instead
		// of leaving the user waiting on a slider that is not there.
		ImGui::TextDisabled("This scene has no Sky entity - add one with View - Environment.");
	}
	else if (tut::wantsWindowOpened(step->check) && tut::windowOpenIn(step->check, s_base))
	{
		// It was already open when the step began, so "open it" cannot fire. Ask
		// for the close-and-open rather than letting the step look stuck.
		ImGui::TextDisabled("Already open - close it and open it again.");
	}
	else if (step->check == tut::Check::SceneSaved && !s_base.sceneUnsaved)
	{
		// Nothing to save when the step opened, so Ctrl+S is a no-op and the check
		// cannot fire. Point at the way forward instead of at the keyboard shortcut.
		ImGui::TextDisabled("Nothing to save yet - change something first.");
	}
	else if (step->check == tut::Check::ContentRootShown &&
	         now.contentRootKind == s_base.contentRootKind)
	{
		ImGui::TextDisabled("Use the root buttons at the top of the Content Browser.");
	}
	else if (step->check == tut::Check::PanelsVisited)
	{
		const int n = tut::listEntryCount(step->arg);
		int left = 0;
		for (int i = 0; i < n; ++i)
			if (!tut::listEntryVisited(step->arg, i, now.visitedPanels)) ++left;
		ImGui::TextDisabled("%d of %d panels visited.", n - left, n);
	}
	else
	{
		ImGui::TextDisabled("Waiting for you to do it.");
	}

	// ── Navigation ────────────────────────────────────────────────────────────
	// There is no skip. The tour advances when the editor has SEEN the step done
	// (or, for a prose card, when it has been read and acknowledged), so "Next" is
	// only ever the shortcut past the one-second confirmation pause. Back stays
	// free — revisiting a step you already finished is not skipping one.
	const float w  = ImGui::GetContentRegionAvail().x;
	const float bw = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

	ImGui::BeginDisabled(s_cursor.chapter == 0 && s_cursor.step == 0);
	if (ImGui::Button("Back", ImVec2(bw, 0.0f)))
		gotoCursor(tut::retreat(s_cursor), ctx.globalState);
	ImGui::EndDisabled();
	ImGui::SameLine();
	{
		const bool last  = tut::finished(tut::advance(s_cursor));
		const bool ready = isReadCard ? s_readToEnd : s_stepDone;
		ImGui::BeginDisabled(!ready);
		const char* label = isReadCard ? (last ? "Finish" : "Got it")
		                              : (last ? "Finish" : "Next");
		if (ImGui::Button(label, ImVec2(bw, 0.0f)))
		{
			// A prose card is finished BY this button, so it advances straight away
			// — waiting out the confirmation delay for a card the user just
			// dismissed would only feel unresponsive.
			s_ackPressed = true;
			gotoCursor(tut::advance(s_cursor), ctx.globalState);
		}
		ImGui::EndDisabled();
		if (!ready && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip(isReadCard
				? "Read the card to the end first."
				: "Do the step above - the tour notices by itself.");
	}

	ImGui::End();

	// ── Auto-advance + highlight ──────────────────────────────────────────────
	// The delay lets the "Done." land before the card changes under the user.
	if (s_stepDone)
	{
		s_doneTimer += dt;
		if (s_doneTimer >= kAutoAdvanceDelay)
			gotoCursor(tut::advance(s_cursor), ctx.globalState);
	}

	if (!s_stepDone)
		outlineWindows(step->focusWindow,
		               step->check == tut::Check::PanelsVisited,
		               static_cast<float>(ImGui::GetTime()), now.visitedPanels);

	if (!open)
	{
		s_open = false;
		persist(ctx.globalState);
	}
#else
	(void)ctx; (void)dt; (void)flags;
#endif // HE_IMGUI_ENABLED
}

} // namespace TutorialPanel
