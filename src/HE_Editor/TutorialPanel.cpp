#include "TutorialPanel.h"
#include "TutorialSteps.h"
#include "EditorApplication.h"   // AppContext, EditorConfig, ProjectManager
#include "EditorWidgets.h"       // dialog placement + the help-aware button
#include "EditorHelp.h"          // "Project Hub/<label>" for the welcome card
#include "EditorTheme.h"         // brand palette (the action line)
#include "EditorAssetTypeCache.h" // asset-type sniff for the create/open checks
#include "PanelSpotlight.h"      // the pulsing panel outline, shared with the docs reader
#include "EditorRewards.h"       // reward moment: Tutorial complete
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName — used to outline the step's panel
#include <misc/cpp/imgui_stdlib.h>
// The test harness's CPU rasteriser, for the HE_DUMP_TUTORIALUI witness only.
#include "../../tests/ImGuiSoftwareRaster.h"
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
	// move anybody's saved position. "Tutorial.HorizonCode" is the welcome card's
	// choice whether the HorizonCode chapter is part of the tour; missing means yes,
	// so a config from before the option existed walks the tour it always did.
	constexpr const char* kCfgOffered     = "Tutorial.Offered";
	constexpr const char* kCfgStep        = "Tutorial.Step";
	constexpr const char* kCfgHorizonCode = "Tutorial.HorizonCode";

	bool         s_open        = false;
	bool         s_forceWelcome = false;  // re-offer the sandbox even once answered
	bool         s_loaded      = false;   // cursor read back from the config yet?
	// Which chapters the tour includes. Every tut:: cursor call below takes it:
	// one call without it and the card's progress and its Next/Back disagree
	// about where the HorizonCode chapter is.
	tut::Options s_tour        = {};
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
#ifdef HE_IMGUI_ENABLED
	// Keeps the card off the panel the step outlines (see placeCard). Reset per
	// step: a card the user dragged somewhere is theirs until the next one.
	HE::Ed::Spotlight::KeepClear s_cardPlacement;
#endif

	// Seconds a completed step stays on screen before the tour moves on. Long
	// enough that the user sees WHICH step they just finished, short enough that
	// it never feels like waiting.
	constexpr float kAutoAdvanceDelay = 1.4f;

	void persist(GlobalState* gs)
	{
		if (!gs) return;
		gs->setCustomConfigEntry(kCfgHorizonCode, s_tour.horizonCode);
		gs->setCustomConfigEntry(kCfgStep, tut::serialize(s_cursor, s_tour));
		gs->writeConfig();
	}

	void loadOnce(GlobalState* gs)
	{
		if (s_loaded || !gs) return;
		// Options first: a position saved inside a chapter that is now left out
		// resumes at the next included step, and that needs to know which.
		s_tour.horizonCode = gs->getCustomConfigBool(kCfgHorizonCode, true);
		s_cursor = tut::deserialize(gs->getCustomConfigString(kCfgStep, ""), s_tour);
		s_loaded = true;
	}

	// Every piece of per-step state is reset here — a step's baseline, whether its
	// check has fired, and the two ReadAck flags. Missing one of these is how a
	// step would arrive pre-completed, which is exactly what this tour must not do.
	void gotoCursor(tut::Cursor c, GlobalState* gs)
	{
		s_cursor    = tut::clamp(c, s_tour);
		s_baseValid = false;
		s_stepDone  = false;
		s_doneTimer = 0.0f;
		s_ackPressed = false;
		s_readToEnd  = false;
		s_visitedPanels.clear();
#ifdef HE_IMGUI_ENABLED
		s_cardPlacement.reset();
#endif
		persist(gs);
	}

	// Forward one step — the Finish/Next button and the auto-advance, the only
	// two ways the tour moves on (Back and Start Over use gotoCursor).
	void advanceStep(AppContext& ctx)
	{
		const bool lastStep = !tut::finished(s_cursor, s_tour)
		                   && tut::finished(tut::advance(s_cursor, s_tour), s_tour);
		gotoCursor(tut::advance(s_cursor, s_tour), ctx.globalState);
		// Reward moment (EditorRewards.h): TOUR FINISHED — once per run, the
		// step from the last step to finished.
		if (lastStep) HE::Ed::Rewards::fire(ctx, HE::Ed::Rewards::Moment::TourFinished);
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
		case HE::AssetType::Theme:                return tut::Asset::Theme;
		case HE::AssetType::StructType:           return tut::Asset::StructType;
		case HE::AssetType::EnumType:             return tut::Asset::EnumType;
		case HE::AssetType::SaveGameTemplate:     return tut::Asset::SaveGameTemplate;
		case HE::AssetType::BoneMask:             return tut::Asset::BoneMask;
		case HE::AssetType::BlendSpace:           return tut::Asset::BlendSpace;
		case HE::AssetType::PropertyAnimClip:     return tut::Asset::PropertyAnimClip;
		case HE::AssetType::Sequence:             return tut::Asset::Sequence;
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

			s.selectionSet = ctx.selection.primary() != entt::null &&
			                 reg.valid(ctx.selection.primary());
			if (s.selectionSet)
				s.selectedEntity = static_cast<uint32_t>(entt::to_integral(ctx.selection.primary()));

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
	// The outline itself lives in PanelSpotlight now: the documentation reader's
	// "Show me" points at panels the same way, and the three layout cases it has
	// to get right (docked node vs window rect, unselected tab, window that does
	// not exist yet) are subtle enough that a second copy would drift.
	//
	// Returns false when there was nothing to outline, which is how the card knows
	// to tell the user the panel is closed instead of silently pointing at nothing.
	bool outlineWindow(const char* name, float time, bool dimmed)
	{
		return HE::Ed::Spotlight::outline(name, time, dimmed);
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

	// ── Keeping the card off the panel it points at ───────────────────────────
	// The card floats, so wherever it sits it covers something, and the one thing
	// it must not cover is the panel the step is about: "add a component in the
	// Details panel" with the card lying on the Details panel is a step the user
	// has to fight the tour to do — and with the default layout and the card's
	// default bottom-right spot, that is exactly where it lay. Before the card is
	// drawn, the panels the step outlines are tested against where the card was
	// last frame, and when they meet, the card glides aside (Spotlight::KeepClear
	// has the how and the when: same rects as the pulse, a drag by the user wins).
	//
	// The panels still pulsing, that is: on a "visit these panels" step the ones
	// already visited are drawn dim and are done with, and the card may lie on
	// them if that is what keeps the rest clear.
	void placeCard(const tut::Step& step, std::string_view visited, float dt)
	{
		// The witness's control run (HE_DUMP_TUTORIALUI + _NOAVOID): the card stays
		// where it is, which is what the tour did before it learned to step aside.
		static const bool s_controlRun = std::getenv("HE_DUMP_TUTORIALUI") &&
		                                 std::getenv("HE_DUMP_TUTORIALUI_NOAVOID");
		if (s_controlRun) return;

		std::vector<std::string> panels;
		const int n = tut::listEntryCount(step.focusWindow);
		for (int i = 0; i < n; ++i)
		{
			std::string name(tut::listEntry(step.focusWindow, i));
			if (name.empty()) continue;
			if (step.check == tut::Check::PanelsVisited && tut::panelVisited(name, visited))
				continue;
			panels.push_back(std::move(name));
		}
		s_cardPlacement.update("Tutorial", panels, dt);
	}

	// The window the user last clicked into, as the tour's "visited" signal.
	// Same identity question the spotlight answers, so it lives next to it.
	std::string focusedPanelName()
	{
		return HE::Ed::Spotlight::focusedPanel();
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

	// ── Witness: the card in the running editor ──────────────────────────────
	// test_ui_shot proves KeepClear over a rebuilt layout. This proves it in the
	// editor itself: the real dock layout, the real render() below, the real
	// step table. With
	//
	//   HE_DUMP_TUTORIALUI=<dir>  HE_DUMP_TUTORIALUI_STEPS=add-mesh,create-asset,…
	//
	// the tour opens on each listed step in turn (the jump is not written to the
	// config), holds it for kWitnessHold frames so the card can glide, then writes
	// <dir>/tutorial-ui-<id>.bmp — the editor's own ImGui output of that frame,
	// rasterised on the CPU — and logs the card's rect, each outlined panel's
	// rect and how much of them the card covers. The editor quits after the last.
	//
	// The rasteriser is the test harness's (tests/ImGuiSoftwareRaster), and it
	// is fed a CLONE of the draw data. The live draw data carries the GPU
	// backend's texture ids and texture requests; handed those, the rasteriser
	// would claim the font atlas from the real backend. The clone's commands
	// point at CPU copies of ImGui's own textures instead, and anything that is
	// not one of them (the Scene viewport's GPU image) is drawn flat grey —
	// there is no picture of it on this side.
	struct UiWitness
	{
		bool                     active = false;
		bool                     done   = false;
		std::string              dir;
		std::vector<std::string> ids;
		size_t                   index  = 0;
		int                      frames = 0;   // frames the current step has been up
	};
	constexpr int kWitnessHold = 120;

	UiWitness& uiWitness()
	{
		static UiWitness w = [] {
			UiWitness out;
			const char* dir = std::getenv("HE_DUMP_TUTORIALUI");
			if (!dir || !*dir) return out;
			out.dir = dir;
			const char* list = std::getenv("HE_DUMP_TUTORIALUI_STEPS");
			const std::string all = list && *list
				? list : "add-mesh,create-asset,sculpt,outliner,fly,layout";
			size_t start = 0;
			for (;;)
			{
				const size_t comma = all.find(',', start);
				std::string id = all.substr(start, comma == std::string::npos
				                                   ? std::string::npos : comma - start);
				if (!id.empty()) out.ids.push_back(std::move(id));
				if (comma == std::string::npos) break;
				start = comma + 1;
			}
			std::error_code ec;
			fs::create_directories(out.dir, ec);
			out.active = !out.ids.empty();
			return out;
		}();
		return w;
	}

	// A CPU copy of the ImGui texture behind `id`, RGBA8 (the atlas may be
	// Alpha8), registered with the rasteriser. `grey` when `id` is not one of
	// ImGui's own textures.
	ImTextureID witnessTexture(ImTextureID id, ImTextureID grey,
	                           std::vector<std::pair<ImTextureID, ImTextureID>>& made)
	{
		for (const auto& [from, to] : made)
			if (from == id) return to;
		ImTextureID to = grey;
		for (ImTextureData* t : ImGui::GetPlatformIO().Textures)
		{
			if (!t || t->GetTexID() != id || !t->Pixels || t->Width <= 0 || t->Height <= 0)
				continue;
			const size_t n = static_cast<size_t>(t->Width) * static_cast<size_t>(t->Height);
			std::vector<std::uint8_t> rgba(n * 4);
			for (size_t i = 0; i < n; ++i)
			{
				if (t->BytesPerPixel == 1)
				{
					rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
					rgba[i * 4 + 3] = t->Pixels[i];
				}
				else
					std::memcpy(&rgba[i * 4], &t->Pixels[i * 4], 4);
			}
			to = he_ui::registerTexture(rgba.data(), t->Width, t->Height);
			break;
		}
		made.emplace_back(id, to);
		return to;
	}

	bool witnessCapture(const std::string& path)
	{
		const ImDrawData* dd = ImGui::GetDrawData();
		if (!dd || !dd->Valid || dd->DisplaySize.x < 1.0f || dd->DisplaySize.y < 1.0f)
			return false;

		const std::uint8_t greyPx[4] = { 58, 54, 50, 255 };
		const ImTextureID grey = he_ui::registerTexture(greyPx, 1, 1);
		std::vector<std::pair<ImTextureID, ImTextureID>> made;
		std::vector<ImDrawList*> clones;

		ImDrawData copy;
		copy.Valid            = true;
		copy.DisplayPos       = dd->DisplayPos;
		copy.DisplaySize      = dd->DisplaySize;
		copy.FramebufferScale = ImVec2(1.0f, 1.0f);   // one pixel per point is plenty
		copy.Textures         = nullptr;              // no texture requests: not ours to answer
		for (const ImDrawList* list : dd->CmdLists)
		{
			ImDrawList* c = list->CloneOutput();
			for (ImDrawCmd& cmd : c->CmdBuffer)
				if (!cmd.UserCallback)
					cmd.TexRef = ImTextureRef(witnessTexture(cmd.GetTexID(), grey, made));
			clones.push_back(c);
			// Straight into the list, not AddDrawList(): that asserts on the
			// write cursor, which CloneOutput() leaves unset.
			copy.CmdLists.push_back(c);
			copy.CmdListsCount += 1;
			copy.TotalVtxCount += c->VtxBuffer.Size;
			copy.TotalIdxCount += c->IdxBuffer.Size;
		}

		const he_ui::Image img = he_ui::rasterize(&copy, static_cast<int>(dd->DisplaySize.x),
		                                          static_cast<int>(dd->DisplaySize.y));
		const bool ok = img.valid() && he_ui::writeBmp(img, path);

		for (ImDrawList* c : clones) IM_DELETE(c);
		for (const auto& [from, to] : made)
			if (to != grey) he_ui::unregisterTexture(to);
		he_ui::unregisterTexture(grey);
		return ok;
	}

	std::string rectText(ImVec2 a, ImVec2 b)
	{
		char buf[96];
		std::snprintf(buf, sizeof(buf), "(%.0f,%.0f)-(%.0f,%.0f)", a.x, a.y, b.x, b.y);
		return buf;
	}
#endif // HE_IMGUI_ENABLED

} // namespace

void open()
{
	s_open = true;
	// A finished tour reopens from the top — "Interactive Tutorial" that shows a
	// single "you are done" card would be a dead menu item.
	if (tut::finished(s_cursor, s_tour)) s_cursor = tut::clamp(tut::Cursor{ 0, 0 }, s_tour);
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
	// The card sits on the Hub, so its two buttons belong to it.
	HE::Ed::Help::Scope helpScope("Project Hub");
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

	{
		// This card asked for 560 points and does not necessarily get them:
		// pinDialogToEditorWindow caps it to the editor's work area, so on a small
		// editor window it is narrower than it was written for. Anything not
		// wrapped is then simply cut off at the right edge — the heading loses its
		// version, the error loses the half that says what to do about it — with a
		// horizontal scrollbar as the only alternative, which is no alternative at
		// all in a modal the user is trying to answer.
		//
		// Wrapped in its own scope, here and in the tour below, because the wrap
		// position belongs to the window it was pushed on: popping it after
		// EndPopup/End would take a wrap position off whatever window is beneath
		// this one, and that window did not push it.
		EditorWidgets::WrapText wrap;

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
			"No earlier engine experience is assumed. The first chapters are only "
			"about finding your way around: what each panel is for and how to move "
			"the camera. Everything after that builds on them. Each card explains one "
			"idea, then asks you to do one thing in the real editor, and moves on "
			"once it has seen you do it.");
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
		ImGui::Spacing();

		// The sandbox is a HorizonCode project, but the chapter on it only uses
		// the Level Script and Game Instance graphs every project has — so the
		// choice is "do you want the tour of visual scripting", not the project's
		// language. Set on change: "Not now" writes the config too, and the next
		// offer should remember what was ticked here.
		if (EditorWidgets::checkbox("Include the HorizonCode chapter", &s_tour.horizonCode))
			ctx.globalState->setCustomConfigEntry(kCfgHorizonCode, s_tour.horizonCode);
		ImGui::TextDisabled(
			"HorizonCode is the visual scripting language. Writing gameplay in Lua, "
			"Python or C++ instead? Untick it and the tour skips that chapter.");

		if (!s_error.empty())
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
			ImGui::TextWrapped("%s", s_error.c_str());
			ImGui::PopStyleColor();
		}
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// From the actual content width, not the nominal 560: the card is capped to
	// the editor window, so on a small editor it is narrower than it asked for.
	const float btnW = (ImGui::GetContentRegionAvail().x - 8.0f) * 0.5f;
	if (EditorWidgets::button("Start the tutorial", ImVec2(btnW, 34.0f)))
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
	if (EditorWidgets::button("Not now", ImVec2(btnW, 34.0f)))
	{
		// Answered once = never offered again unprompted. Help ▸ Interactive
		// Tutorial is how it comes back.
		ctx.globalState->setCustomConfigEntry(kCfgOffered, true);
		ctx.globalState->writeConfig();
		s_forceWelcome = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::Spacing();
	{
		EditorWidgets::WrapText wrap;
		ImGui::TextDisabled("You can start it later from Help - Interactive Tutorial.");
	}

	ImGui::EndPopup();
#else
	(void)ctx;
#endif // HE_IMGUI_ENABLED
}

// ─── The tour itself ──────────────────────────────────────────────────────────
void render(AppContext& ctx, float dt, const UiFlags& flags)
{
	// The tour card's own controls.
	HE::Ed::Help::Scope helpScope("Tutorial");
#ifdef HE_IMGUI_ENABLED
	loadOnce(ctx.globalState);

	// Play sessions are counted whether or not the window is open: the user may
	// well press Play before opening the tutorial, and the "play then stop" step
	// should not then wait for a second session.
	if (s_wasPlaying && !ctx.isPlaying) ++s_playSessions;
	s_wasPlaying = ctx.isPlaying;

	// HE_DUMP_TUTORIALUI: put the tour on the step under test (see uiWitness).
	// Not persisted — a witness run must not move anybody's saved position.
	if (UiWitness& w = uiWitness(); w.active && !w.done)
	{
		if (w.frames == 0)
		{
			const tut::Cursor c = tut::findStep(w.ids[w.index]);
			if (tut::finished(c))
				HE_LOG_WARN(Editor, "%s", ("Tutorial witness: no step with id '" +
					w.ids[w.index] + "'").c_str());
			gotoCursor(c, nullptr);
			s_open = true;
		}
		++w.frames;
	}

	if (!s_open) return;

	const tut::Step*    step = tut::stepAt(s_cursor, s_tour);
	const tut::Chapter* chap = tut::chapterAt(s_cursor, s_tour);

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
	// …and out of the way of the panel this step points at. After the default
	// above, so a step-aside overrides it; only while the outline is drawn, since
	// a finished step points at nothing.
	if (step && !s_stepDone)
		placeCard(*step, now.visitedPanels, dt);
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
		{
			EditorWidgets::WrapText wrap;
			if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
			ImGui::TextUnformatted("Tour complete");
			if (ctx.fontSubheading) ImGui::PopFont();
			ImGui::Separator();
			ImGui::Spacing();
			ImGui::TextWrapped(
				"You have been through every chapter. Help - Interactive Tutorial starts "
				"it again from the top whenever you want a refresher.");
			ImGui::Spacing();
			if (EditorWidgets::button("Start over", ImVec2(-1.0f, 30.0f)))
				gotoCursor(tut::Cursor{ 0, 0 }, ctx.globalState);
		}
		ImGui::End();
		if (!open) s_open = false;
		return;
	}

	// ── Header: chapter + progress ────────────────────────────────────────────
	const int done  = tut::flatIndex(s_cursor, s_tour);
	const int total = tut::totalSteps(s_tour);
	{
		// The tour window can be dragged down to 340 points wide, and a chapter
		// line is "Chapter 7/9  -  " plus a title written for a card, not for a
		// column. Clipped, it reads as a chapter with no name.
		EditorWidgets::WrapText wrap;
		ImGui::TextDisabled("Chapter %d/%d  -  %s",
			tut::chapterNumber(s_cursor, s_tour), tut::chapterCount(s_tour), chap->title);
		ImGui::ProgressBar(total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 0.0f,
			ImVec2(-1.0f, 6.0f), "");
		ImGui::Spacing();

		if (ctx.fontSubheading) ImGui::PushFont(ctx.fontSubheading);
		ImGui::TextWrapped("%s", step->title);
		if (ctx.fontSubheading) ImGui::PopFont();
		ImGui::Spacing();
	}

	// ── Body ──────────────────────────────────────────────────────────────────
	const bool isReadCard = step->check == tut::Check::ReadAck;

	// The status line is the only place the tour explains itself, so it has to
	// name the *reason* a step is still open — "waiting" on its own is what makes
	// a gated tutorial feel broken. Which means it says whole sentences, and a
	// sentence that is cut off at the window's edge explains nothing: the reason
	// is always at the end of it ("...add one with View - Environment"). It is
	// wrapped where it is drawn, further down, and therefore has to be DECIDED up
	// here: the body child below can only reserve room for a line it has already
	// seen. Deciding it here and drawing this same string later is what keeps the
	// reservation and the drawing in step — measuring one sentence and then
	// printing a different one is the same bug as reserving a constant.
	std::string statusLine;
	if (s_stepDone)
		statusLine = "Done.";
	else if (isReadCard)
		statusLine = s_readToEnd ? "Ready when you are."
		                         : "Scroll to the end of the card to continue.";
	else if (step->check == tut::Check::TimeOfDayChanged && !now.skyPresent)
		// The one step whose subject can be missing from the scene. Say so instead
		// of leaving the user waiting on a slider that is not there.
		statusLine = "This scene has no Sky entity - add one with View - Environment.";
	else if (tut::wantsWindowOpened(step->check) && tut::windowOpenIn(step->check, s_base))
		// It was already open when the step began, so "open it" cannot fire. Ask
		// for the close-and-open rather than letting the step look stuck.
		statusLine = "Already open - close it and open it again.";
	else if (step->check == tut::Check::SceneSaved && !s_base.sceneUnsaved)
		// Nothing to save when the step opened, so Ctrl+S is a no-op and the check
		// cannot fire. Point at the way forward instead of at the keyboard shortcut.
		statusLine = "Nothing to save yet - change something first.";
	else if (step->check == tut::Check::ContentRootShown &&
	         now.contentRootKind == s_base.contentRootKind)
		statusLine = "Use the root buttons at the top of the Content Browser.";
	else if (step->check == tut::Check::PanelsVisited)
	{
		const int n = tut::listEntryCount(step->arg);
		int visited = 0;
		for (int i = 0; i < n; ++i)
			if (tut::listEntryVisited(step->arg, i, now.visitedPanels)) ++visited;
		statusLine = std::to_string(visited) + " of " + std::to_string(n) + " panels visited.";
	}
	else
		statusLine = "Waiting for you to do it.";

	// Reserve room for the action line, the status line and the button row so the
	// body text scrolls instead of pushing the controls off the bottom. Both lines
	// are MEASURED rather than assumed to be one line each — several action lines
	// wrap to two or three at a narrow window width, and a fixed reservation put
	// the buttons out of reach exactly there. Measuring cuts the other way too: a
	// blanket "two lines for the status" is paid at every dock width, and the line
	// it buys stays empty at all but the narrowest — the body card is then one line
	// shorter than it needs to be with a grey strip under it, and a card that used
	// to fit starts demanding a scroll before "Got it" unlocks.
	const std::string actionLine = std::string("> ") + step->action;
	const float availW  = ImGui::GetContentRegionAvail().x;
	const float actionH = step->action[0] != '\0'
		? ImGui::CalcTextSize(actionLine.c_str(), nullptr, false, availW).y +
		  ImGui::GetStyle().ItemSpacing.y
		: 0.0f;
	const float statusH = ImGui::CalcTextSize(statusLine.c_str(), nullptr, false, availW).y +
	                      ImGui::GetStyle().ItemSpacing.y;
	const float footerH = ImGui::GetFrameHeightWithSpacing()      // button row
	                    + statusH
	                    + actionH
	                    + ImGui::GetStyle().ItemSpacing.y * 2.0f; // separator + padding
	ImGui::BeginChild("##tutBody", ImVec2(0.0f, -footerH), ImGuiChildFlags_None);
	{
		EditorWidgets::WrapText wrap;
		drawParagraphs(step->body);
		// A prose card unlocks its button once the body has actually been read to
		// the end. A body that fits without scrolling has MaxY == 0 and counts as
		// read immediately — there is nothing to scroll past.
		if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) s_readToEnd = true;
	}
	ImGui::EndChild();

	// ── What to do ────────────────────────────────────────────────────────────
	ImGui::Separator();
	if (step->action[0] != '\0')
	{
		ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextHeading);
		ImGui::TextWrapped("%s", actionLine.c_str());
		ImGui::PopStyleColor();
	}

	// The sentence itself was decided above the body child, which is where the room
	// for it is reserved; here it is only wrapped and coloured. One consequence
	// worth knowing: a card whose body has just been scrolled to its end sets
	// s_readToEnd inside the child, i.e. after this string was chosen, so for a
	// single frame the button unlocks while the line still says "scroll to the
	// end". A frame of that is cheaper than a status line whose height nobody
	// measured.
	{
		EditorWidgets::WrapText wrap;
		if (s_stepDone)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.90f, 0.55f, 1.0f));
			ImGui::TextUnformatted(statusLine.c_str());
			ImGui::PopStyleColor();
		}
		else
		{
			ImGui::TextDisabled("%s", statusLine.c_str());
		}
	}

	// ── Navigation ────────────────────────────────────────────────────────────
	// There is no skip. The tour advances when the editor has SEEN the step done
	// (or, for a prose card, when it has been read and acknowledged), so "Next" is
	// only ever the shortcut past the one-second confirmation pause. Back stays
	// free — revisiting a step you already finished is not skipping one.
	const float w  = ImGui::GetContentRegionAvail().x;
	const float bw = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

	ImGui::BeginDisabled(tut::retreat(s_cursor, s_tour) == s_cursor);
	if (ImGui::Button("Back", ImVec2(bw, 0.0f)))
		gotoCursor(tut::retreat(s_cursor, s_tour), ctx.globalState);
	ImGui::EndDisabled();
	ImGui::SameLine();
	{
		const bool last  = tut::finished(tut::advance(s_cursor, s_tour), s_tour);
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
			advanceStep(ctx);
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
			advanceStep(ctx);
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

void witnessAfterRender()
{
#ifdef HE_IMGUI_ENABLED
	UiWitness& w = uiWitness();
	// frames == 0: render() has not put the tour up yet (no project loaded).
	if (!w.active || w.done || w.frames < kWitnessHold) return;

	const std::string& id = w.ids[w.index];
	const tut::Step* step = tut::stepAt(s_cursor, s_tour);
	std::string line = "Tutorial witness: step '" + id + "'";
	if (!step || id != step->id)
		line += " NOT on screen (tour is at '" + std::string(step ? step->id : "done") + "')";

	if (ImGuiWindow* card = ImGui::FindWindowByName("Tutorial"); card && step)
	{
		const ImVec2 cMin = card->Pos;
		const ImVec2 cMax(card->Pos.x + card->Size.x, card->Pos.y + card->Size.y);
		line += ", card " + rectText(cMin, cMax) + (s_stepDone ? " [step done]" : "");
		float total = 0.0f;
		const int n = tut::listEntryCount(step->focusWindow);
		for (int i = 0; i < n; ++i)
		{
			const std::string name(tut::listEntry(step->focusWindow, i));
			ImVec2 pos, size;
			if (!HE::Ed::Spotlight::panelRect(name.c_str(), pos, size))
			{
				line += ", '" + name + "' not on screen";
				continue;
			}
			const float covered = HE::Ed::Spotlight::coveredArea(card->Pos, card->Size,
				{ { pos, ImVec2(pos.x + size.x, pos.y + size.y) } });
			total += covered;
			line += ", '" + name + "' " + rectText(pos, ImVec2(pos.x + size.x, pos.y + size.y)) +
			        " covered " + std::to_string(static_cast<int>(covered)) + " pt²";
		}
		line += ", total covered " + std::to_string(static_cast<int>(total)) + " pt²";
	}

	const std::string path = w.dir + "/tutorial-ui-" + id + ".bmp";
	line += witnessCapture(path) ? " → " + path : " (capture FAILED)";
	HE_LOG_INFO(Editor, "%s", line.c_str());

	++w.index;
	w.frames = 0;
	if (w.index >= w.ids.size())
	{
		w.done = true;
		HE_LOG_INFO(Editor, "%s", "Tutorial witness: done, quitting");
		SDL_Event quit{};
		quit.type = SDL_EVENT_QUIT;
		SDL_PushEvent(&quit);
	}
#endif // HE_IMGUI_ENABLED
}

} // namespace TutorialPanel
