#include "TutorialPanel.h"
#include "TutorialSteps.h"
#include "EditorApplication.h"   // AppContext, EditorConfig, ProjectManager
#include "EditorWidgets.h"       // dialog placement (stay inside the editor window)
#include "HorizonVersion.h"

#include <HorizonScene/HorizonWorld.h>
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

	void gotoCursor(tut::Cursor c, GlobalState* gs)
	{
		s_cursor    = tut::clamp(c);
		s_baseValid = false;
		s_stepDone  = false;
		s_doneTimer = 0.0f;
		persist(gs);
	}

	// ── Sampling the live editor ──────────────────────────────────────────────
	// Everything a completion check may look at, gathered once per frame. Kept in
	// one place so the checks stay pure functions over data (see TutorialSteps.h)
	// and adding a check never means reaching into another panel.
	//
	// `withAssets` gates the one expensive part — walking the whole content tree
	// under its shared lock — to the single step kind that needs it. Both the
	// baseline snapshot and the live one are taken for the SAME step, so gating
	// cannot produce a spurious "an asset appeared": either both counts are real
	// or both are zero.
	tut::Signals sample(AppContext& ctx, const UiFlags& flags, bool withAssets)
	{
		tut::Signals s;

		if (ctx.world)
		{
			auto& reg = ctx.world->registry();
			// Every entity carries a NameComponent (HorizonWorld::createEntity and the
			// scene loader both set one), so this view is the entity count.
			s.entityCount = static_cast<int>(reg.view<NameComponent>().size());

			auto mark = [&](tut::Comp c, bool present) { if (present) s.set(c); };
			mark(tut::Comp::Mesh,           !reg.view<MeshComponent>().empty());
			mark(tut::Comp::Material,       !reg.view<MaterialComponent>().empty());
			mark(tut::Comp::Light,          !reg.view<LightComponent>().empty());
			mark(tut::Comp::RigidBody,      !reg.view<RigidBodyComponent>().empty());
			mark(tut::Comp::Collider,       !reg.view<ColliderComponent>().empty());
			mark(tut::Comp::ParticleSystem, !reg.view<ParticleSystemComponent>().empty());
			mark(tut::Comp::Script,         !reg.view<ScriptComponent>().empty());
			mark(tut::Comp::Terrain,        !reg.view<TerrainComponent>().empty());
			mark(tut::Comp::Foliage,        !reg.view<FoliageComponent>().empty());
			mark(tut::Comp::NavMesh,        !reg.view<NavMeshComponent>().empty());
			mark(tut::Comp::Camera,         !reg.view<CameraComponent>().empty());
			mark(tut::Comp::AudioSource,    !reg.view<AudioSourceComponent>().empty());
			mark(tut::Comp::Animator,       !reg.view<AnimatorComponent>().empty());
			mark(tut::Comp::UICanvas,       !reg.view<UICanvasComponent>().empty());

			s.selectionSet = ctx.selectedEntity != entt::null &&
			                 reg.valid(ctx.selectedEntity);
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
		}
#endif

		s.playing         = ctx.isPlaying;
		s.playSessions    = s_playSessions;
		s.sceneUnsaved    = ctx.sceneDirty || ctx.currentScenePath.empty();
		s.landscapeMode   = ctx.editorConfig.mode == EditorMode::Landscape;
		s.profilerOpen    = flags.profilerOpen;
		s.environmentOpen = flags.environmentOpen;
		s.exportOpen      = flags.exportOpen;
		return s;
	}

#ifdef HE_IMGUI_ENABLED
	// A soft pulsing outline around the panel a step is talking about. Drawn on the
	// foreground list so it sits over docked windows, and skipped for a collapsed or
	// hidden window rather than outlining a zero-size rect somewhere off-screen.
	void outlineWindow(const char* name, float time)
	{
		if (!name || name[0] == '\0') return;
		ImGuiWindow* w = ImGui::FindWindowByName(name);
		if (!w || !w->WasActive || w->Hidden || w->Collapsed) return;
		if (w->Size.x < 2.0f || w->Size.y < 2.0f) return;

		const float pulse = 0.45f + 0.35f * (0.5f + 0.5f * std::sin(time * 3.2f));
		ImDrawList* dl = ImGui::GetForegroundDrawList();
		dl->AddRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y),
		            ImGui::GetColorU32(ImVec4(0.45f, 0.72f, 1.0f, pulse)),
		            6.0f, 0, 3.0f);
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
				Logger::Log(Logger::LogLevel::Info,
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

	const tut::Signals now =
		sample(ctx, flags, step && step->check == tut::Check::AssetAdded);
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
	ImGui::EndChild();

	// ── What to do ────────────────────────────────────────────────────────────
	ImGui::Separator();
	if (step->action[0] != '\0')
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.80f, 1.0f, 1.0f));
		ImGui::TextWrapped("%s", actionLine.c_str());
		ImGui::PopStyleColor();
	}

	if (step->check == tut::Check::Manual)
	{
		ImGui::TextDisabled("Read on when you are ready.");
	}
	else if (s_stepDone)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.90f, 0.55f, 1.0f));
		ImGui::TextUnformatted("Done.");
		ImGui::PopStyleColor();
	}
	else
	{
		ImGui::TextDisabled("Waiting for you - or press Next to skip it.");
	}

	// ── Navigation ────────────────────────────────────────────────────────────
	const float w = ImGui::GetContentRegionAvail().x;
	const float bw = (w - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;

	ImGui::BeginDisabled(s_cursor.chapter == 0 && s_cursor.step == 0);
	if (ImGui::Button("Back", ImVec2(bw, 0.0f)))
		gotoCursor(tut::retreat(s_cursor), ctx.globalState);
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Skip chapter", ImVec2(bw, 0.0f)))
		gotoCursor(tut::nextChapter(s_cursor), ctx.globalState);
	ImGui::SameLine();
	{
		const bool last = tut::finished(tut::advance(s_cursor));
		if (ImGui::Button(last ? "Finish" : "Next", ImVec2(bw, 0.0f)))
			gotoCursor(tut::advance(s_cursor), ctx.globalState);
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
		outlineWindow(step->focusWindow, static_cast<float>(ImGui::GetTime()));

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
