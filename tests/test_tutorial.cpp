#include "doctest.h"
#include "TestFsUtil.h"
#include "TutorialSteps.h"
#include "ProjectManager.h"
#include <ContentManager/DefaultAssets.h>
#include <Types/Enums.h>   // HE::LightType

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/LightComponent.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fs  = std::filesystem;
namespace tut = HE::tut;
using json    = nlohmann::json;

// ─── Curriculum integrity ─────────────────────────────────────────────────────
// The step tables are hand-written data, so the things a typo would break — a
// duplicate id (two steps sharing a saved position), an empty body, a check that
// names a component nobody can add — are asserted here rather than discovered by
// a user halfway through the tour.

TEST_CASE("tutorial: every step is complete and uniquely identified")
{
	REQUIRE(tut::chapterCount() > 0);
	REQUIRE(tut::totalSteps() >= tut::chapterCount());

	std::set<std::string> stepIds, chapterIds;
	int counted = 0;

	for (int ci = 0; ci < tut::chapterCount(); ++ci)
	{
		const tut::Chapter& c = tut::chapters()[ci];
		CHECK(c.id != nullptr);
		CHECK(std::string(c.id).size() > 0);
		CHECK(std::string(c.title).size() > 0);
		CHECK(std::string(c.summary).size() > 0);
		CHECK(c.stepCount > 0);
		CHECK(chapterIds.insert(c.id).second);   // no duplicate chapter ids

		for (int si = 0; si < c.stepCount; ++si)
		{
			const tut::Step& s = c.steps[si];
			CAPTURE(c.id);
			CAPTURE(s.id);
			CHECK(std::string(s.id).size() > 0);
			CHECK(std::string(s.title).size() > 0);
			CHECK(std::string(s.body).size() > 0);
			CHECK(s.action != nullptr);        // may be empty (pure reading step)
			CHECK(s.focusWindow != nullptr);
			CHECK(s.arg != nullptr);
			CHECK(stepIds.insert(s.id).second); // ids are the persisted progress key
			++counted;
		}
	}
	CHECK(counted == tut::totalSteps());
}

namespace
{
// Every window name a step is allowed to point at. The highlight resolves these
// with ImGui::FindWindowByName, which silently finds NOTHING on a typo — the step
// would then quietly highlight the wrong thing or nothing at all, which is
// exactly the failure this list exists to prevent. Keep it in sync with the
// ImGui::Begin() calls in EditorUI.cpp and the panels it drives:
//   EditorUI.cpp          "…###Quick Settings"  (id is the ### part)
//   ViewportPanel.cpp     "Scene"
//   OutlinerPanel.cpp     "World Outliner"
//   InspectorPanel.cpp    "Details"
//   ContentBrowserPanel   "Content Browser"
//   EnvironmentPanel.cpp  "Environment"
//   ProfilerPanel.cpp     "Performance Profiler"
const std::set<std::string> kKnownPanels = {
	"Scene", "World Outliner", "Details", "Content Browser", "Quick Settings",
	"Environment", "Performance Profiler",
};

// Split a '|'-separated list the way TutorialSteps does.
std::vector<std::string> splitBars(std::string_view list)
{
	std::vector<std::string> out;
	for (int i = 0, n = tut::listEntryCount(list); i < n; ++i)
		out.emplace_back(tut::listEntry(list, i));
	return out;
}
} // namespace

TEST_CASE("tutorial: parameterised checks name something that exists")
{
	for (int ci = 0; ci < tut::chapterCount(); ++ci)
	{
		const tut::Chapter& c = tut::chapters()[ci];
		for (int si = 0; si < c.stepCount; ++si)
		{
			const tut::Step& s = c.steps[si];
			CAPTURE(s.id);
			if (s.check == tut::Check::ComponentAdded)
			{
				// An unknown component name would make the step impossible to finish.
				// Compared as ints so doctest can stringify a mismatch.
				CHECK(static_cast<int>(tut::compFromString(s.arg)) !=
				      static_cast<int>(tut::Comp::Count));
			}
			if (s.check == tut::Check::AssetOfTypeAdded ||
			    s.check == tut::Check::TabOfTypeOpened)
			{
				CHECK(static_cast<int>(tut::assetFromString(s.arg)) !=
				      static_cast<int>(tut::Asset::Count));
			}
			if (s.check == tut::Check::TabOpen)
				CHECK(std::string(s.arg).size() > 0);
			if (s.check == tut::Check::ContentRootShown)
			{
				const std::string a = s.arg;
				CHECK((a == "content" || a == "engine" || a == "source"));
			}
			// A PanelsVisited step's arg is what the user must click; naming a window
			// that does not exist makes the step unfinishable.
			if (s.check == tut::Check::PanelsVisited)
			{
				const auto names = splitBars(s.arg);
				CHECK(names.size() > 0);
				for (const std::string& n : names)
				{
					CAPTURE(n);
					CHECK(kKnownPanels.count(n) == 1);
				}
			}

			for (const std::string& w : splitBars(s.focusWindow))
			{
				CAPTURE(w);
				CHECK(kKnownPanels.count(w) == 1);
			}
		}
	}
}

// The whole point of the rework: the tour follows the user. A step that advances
// on a button press alone would be a step the user can click past without ever
// touching the thing it teaches.
TEST_CASE("tutorial: every step is observed, none is a bare Next")
{
	int readCards = 0;
	for (int ci = 0; ci < tut::chapterCount(); ++ci)
	{
		const tut::Chapter& c = tut::chapters()[ci];
		for (int si = 0; si < c.stepCount; ++si)
		{
			const tut::Step& s = c.steps[si];
			CAPTURE(s.id);

			// A ReadAck card is the only kind with nothing to observe in the editor,
			// so it must have nothing to DO either — an action line with no check
			// behind it is precisely the "press Next to skip" this replaced.
			if (s.check == tut::Check::ReadAck)
			{
				++readCards;
				CHECK(std::string(s.action).empty());
			}
			else
			{
				// Everything else tells the user what to do, and is watched for it.
				CHECK(std::string(s.action).size() > 0);
			}
		}
	}
	// Prose cards are the exception, not the tour. If this ever trips, a step that
	// should be observable was quietly turned into a click-through.
	CHECK(readCards * 4 < tut::totalSteps());
}

// A check that fires on a STATE rather than a transition ticks its step off the
// moment the tour reaches it — the furnished sandbox scene already has meshes,
// materials and lights, and the editor already has tabs open. Feeding every step
// the same snapshot as base and now is the cheapest way to catch that.
TEST_CASE("tutorial: no step is already satisfied when it opens")
{
	tut::Signals s;
	// A world that looks lived-in, which is what the tutorial sandbox actually is.
	s.entityCount = 12;
	s.assetCount  = 40;
	s.playSessions = 3;
	s.undoCount   = 7;
	s.importOpens = 2;
	s.materialsAssigned = 4;
	s.selectionSet = true;
	s.selectedEntity = 99;
	s.sceneUnsaved = false;
	s.landscapeMode = true;
	s.preferencesOpen = true;
	s.profilerOpen = true;
	s.environmentOpen = true;
	s.exportOpen = true;
	s.acknowledged = false;
	s.skyPresent = true;
	s.timeOfDay = 0.5f;
	s.contentRootKind = 1;
	s.camX = 3.0f; s.camY = 4.0f; s.camZ = 5.0f;
	s.camYaw = 1.0f; s.camPitch = 0.3f; s.camPivot = 9.0f;
	s.openTabs = "Scene\n\nMyMaterial\n/Content/MyMaterial.hasset\n::LevelScript::\n";
	s.visitedPanels = "Scene\nWorld Outliner\nDetails\nContent Browser\n";
	for (int i = 0; i < static_cast<int>(tut::Comp::Count); ++i)
		s.add(static_cast<tut::Comp>(i), 3);
	for (int i = 0; i < static_cast<int>(tut::Asset::Count); ++i)
	{
		s.add(static_cast<tut::Asset>(i), 3);
		s.addTab(static_cast<tut::Asset>(i), 2);
	}

	for (int ci = 0; ci < tut::chapterCount(); ++ci)
	{
		const tut::Chapter& c = tut::chapters()[ci];
		for (int si = 0; si < c.stepCount; ++si)
		{
			const tut::Step& step = c.steps[si];
			CAPTURE(step.id);
			// PanelsVisited is the one honest exception: its accumulator is cleared
			// when the step opens, so an identical base/now pair cannot model it.
			if (step.check == tut::Check::PanelsVisited) continue;
			CHECK_FALSE(tut::satisfied(step, s, s));
		}
	}
}

TEST_CASE("tutorial: component names round-trip")
{
	// Comp is compared as int throughout: doctest stringifies both sides of a
	// failing CHECK, and it has no rendering for a bare enum class.
	for (int i = 0; i < static_cast<int>(tut::Comp::Count); ++i)
	{
		const auto c = static_cast<tut::Comp>(i);
		CAPTURE(tut::compName(c));
		CHECK(static_cast<int>(tut::compFromString(tut::compName(c))) == i);
	}
	CHECK(static_cast<int>(tut::compFromString("nosuchcomponent")) ==
	      static_cast<int>(tut::Comp::Count));
	CHECK(static_cast<int>(tut::compFromString("")) == static_cast<int>(tut::Comp::Count));
	CHECK(std::string(tut::compName(tut::Comp::Count)).empty());

	for (int i = 0; i < static_cast<int>(tut::Asset::Count); ++i)
	{
		const auto a = static_cast<tut::Asset>(i);
		CAPTURE(tut::assetName(a));
		CHECK(static_cast<int>(tut::assetFromString(tut::assetName(a))) == i);
	}
	CHECK(static_cast<int>(tut::assetFromString("nosuchasset")) ==
	      static_cast<int>(tut::Asset::Count));
	CHECK(std::string(tut::assetName(tut::Asset::Count)).empty());
}

// ─── Cursor arithmetic ────────────────────────────────────────────────────────

TEST_CASE("tutorial: advancing walks every step exactly once and then finishes")
{
	tut::Cursor c{ 0, 0 };
	int visited = 0;
	while (!tut::finished(c))
	{
		REQUIRE(tut::stepAt(c) != nullptr);
		REQUIRE(tut::chapterAt(c) != nullptr);
		CHECK(tut::flatIndex(c) == visited);
		c = tut::advance(c);
		++visited;
		REQUIRE(visited <= tut::totalSteps() + 1); // guards against a non-terminating advance
	}
	CHECK(visited == tut::totalSteps());
	CHECK(tut::stepAt(c) == nullptr);
	CHECK(tut::chapterAt(c) == nullptr);
	CHECK(tut::flatIndex(c) == tut::totalSteps());
	CHECK(tut::advance(c) == c);   // finished stays finished
}

TEST_CASE("tutorial: retreat undoes advance and stops at the start")
{
	tut::Cursor start{ 0, 0 };
	CHECK(tut::retreat(start) == start);

	tut::Cursor c = start;
	for (int i = 0; i < tut::totalSteps(); ++i)
	{
		const tut::Cursor next = tut::advance(c);
		if (tut::finished(next)) break;
		CHECK(tut::retreat(next) == c);
		c = next;
	}

	// Retreating out of "finished" lands on the last real step.
	const tut::Cursor end{ tut::chapterCount(), 0 };
	const tut::Cursor last = tut::retreat(end);
	CHECK_FALSE(tut::finished(last));
	CHECK(tut::flatIndex(last) == tut::totalSteps() - 1);
}

TEST_CASE("tutorial: skipping a chapter lands on the next chapter's first step")
{
	tut::Cursor c{ 0, 0 };
	int chapters = 0;
	while (!tut::finished(c))
	{
		CHECK(c.step == 0);
		CHECK(c.chapter == chapters);
		c = tut::nextChapter(c);
		++chapters;
		REQUIRE(chapters <= tut::chapterCount() + 1);
	}
	CHECK(chapters == tut::chapterCount());
}

TEST_CASE("tutorial: clamp repairs out-of-range and stale cursors")
{
	CHECK(tut::clamp(tut::Cursor{ -3, -9 }) == tut::Cursor{ 0, 0 });
	CHECK(tut::finished(tut::clamp(tut::Cursor{ 9999, 0 })));
	// A step index past the end of its chapter rolls into the next chapter rather
	// than silently snapping back onto a step the user already did.
	const tut::Cursor rolled = tut::clamp(tut::Cursor{ 0, 9999 });
	CHECK(rolled.step == 0);
	CHECK(rolled.chapter == 1);
}

TEST_CASE("tutorial: flatIndex and fromFlat round-trip")
{
	for (int i = 0; i < tut::totalSteps(); ++i)
		CHECK(tut::flatIndex(tut::fromFlat(i)) == i);
	CHECK(tut::finished(tut::fromFlat(tut::totalSteps())));
	CHECK(tut::finished(tut::fromFlat(tut::totalSteps() + 50)));
	CHECK(tut::fromFlat(-1) == tut::Cursor{ 0, 0 });
}

// ─── Persisted progress ───────────────────────────────────────────────────────

TEST_CASE("tutorial: progress round-trips through its serialized form")
{
	tut::Cursor c{ 0, 0 };
	while (!tut::finished(c))
	{
		CAPTURE(tut::serialize(c));
		CHECK(tut::deserialize(tut::serialize(c)) == c);
		c = tut::advance(c);
	}
	CHECK(tut::serialize(c) == "done");
	CHECK(tut::finished(tut::deserialize("done")));

	// A blank config value means "never started"; an id that no longer exists must
	// not strand the user on a step that cannot be rendered.
	CHECK(tut::deserialize("") == tut::Cursor{ 0, 0 });
	CHECK(tut::finished(tut::deserialize("a-step-that-was-removed")));
}

// ─── Completion checks ────────────────────────────────────────────────────────

TEST_CASE("tutorial: checks fire on the transition they describe")
{
	tut::Signals base;   // defaults: nothing selected, not playing, scene unsaved
	tut::Signals now = base;

	auto step = [](tut::Check check, const char* arg = "")
	{
		return tut::Step{ "t", "T", "B", "", "", check, arg };
	};

	// A prose card waits for its acknowledgement and nothing else.
	CHECK_FALSE(tut::satisfied(step(tut::Check::ReadAck), base, now));
	now.acknowledged = true;
	CHECK(tut::satisfied(step(tut::Check::ReadAck), base, now));
	now.acknowledged = false;

	// Counters need to GROW, not merely be non-zero.
	base.entityCount = 4; now.entityCount = 4;
	CHECK_FALSE(tut::satisfied(step(tut::Check::EntityAdded), base, now));
	now.entityCount = 5;
	CHECK(tut::satisfied(step(tut::Check::EntityAdded), base, now));

	base.assetCount = 12; now.assetCount = 12;
	CHECK_FALSE(tut::satisfied(step(tut::Check::AssetAdded), base, now));
	now.assetCount = 13;
	CHECK(tut::satisfied(step(tut::Check::AssetAdded), base, now));

	// A save only counts when there was something unsaved to begin with.
	base.sceneUnsaved = false; now.sceneUnsaved = false;
	CHECK_FALSE(tut::satisfied(step(tut::Check::SceneSaved), base, now));
	base.sceneUnsaved = true;
	CHECK(tut::satisfied(step(tut::Check::SceneSaved), base, now));

	// A play session has to have ended, not just started.
	base.playSessions = 0; now.playSessions = 0; now.playing = true;
	CHECK_FALSE(tut::satisfied(step(tut::Check::PlayCycled), base, now));
	now.playing = false; now.playSessions = 1;
	CHECK(tut::satisfied(step(tut::Check::PlayCycled), base, now));

	base.undoCount = 2; now.undoCount = 2;
	CHECK_FALSE(tut::satisfied(step(tut::Check::UndoUsed), base, now));
	now.undoCount = 3;
	CHECK(tut::satisfied(step(tut::Check::UndoUsed), base, now));

	base.importOpens = 1; now.importOpens = 1;
	CHECK_FALSE(tut::satisfied(step(tut::Check::ImportOpened), base, now));
	now.importOpens = 2;
	CHECK(tut::satisfied(step(tut::Check::ImportOpened), base, now));

	base.materialsAssigned = 1; now.materialsAssigned = 1;
	CHECK_FALSE(tut::satisfied(step(tut::Check::MaterialAssigned), base, now));
	now.materialsAssigned = 2;
	CHECK(tut::satisfied(step(tut::Check::MaterialAssigned), base, now));

	// Selecting the entity that was already selected is not "click an entity".
	base.selectionSet = true; base.selectedEntity = 7;
	now.selectionSet  = true; now.selectedEntity  = 7;
	CHECK_FALSE(tut::satisfied(step(tut::Check::SelectionChanged), base, now));
	now.selectedEntity = 8;
	CHECK(tut::satisfied(step(tut::Check::SelectionChanged), base, now));

	// Window toggles want the OPENING, so a window that was already up when the
	// step began cannot tick it off.
	base.landscapeMode = true; now.landscapeMode = true;
	CHECK_FALSE(tut::satisfied(step(tut::Check::LandscapeMode), base, now));
	base.landscapeMode = false;
	CHECK(tut::satisfied(step(tut::Check::LandscapeMode), base, now));

	for (const auto check : { tut::Check::PreferencesOpen, tut::Check::ProfilerOpen,
	                          tut::Check::EnvironmentOpen, tut::Check::ExportOpen })
	{
		tut::Signals b, n;
		CHECK_FALSE(tut::windowOpenIn(check, b));
		CHECK(tut::wantsWindowOpened(check));
		// Flip the one flag this check reads, in both snapshots then in only one.
		for (bool inBase : { true, false })
		{
			b = tut::Signals{}; n = tut::Signals{};
			switch (check)
			{
			case tut::Check::PreferencesOpen: b.preferencesOpen = inBase; n.preferencesOpen = true; break;
			case tut::Check::ProfilerOpen:    b.profilerOpen    = inBase; n.profilerOpen    = true; break;
			case tut::Check::EnvironmentOpen: b.environmentOpen = inBase; n.environmentOpen = true; break;
			default:                          b.exportOpen      = inBase; n.exportOpen      = true; break;
			}
			CHECK(tut::windowOpenIn(check, n));
			CHECK(tut::satisfied(step(check), b, n) == !inBase);
		}
	}
}

TEST_CASE("tutorial: camera checks separate flying from zooming")
{
	auto step = [](tut::Check check, const char* arg = "")
	{
		return tut::Step{ "t", "T", "B", "", "", check, arg };
	};

	tut::Signals base, now;
	base.camPivot = now.camPivot = 8.0f;

	// Turning on the spot is not flying, and neither is sliding without looking.
	now.camYaw = 0.9f;
	CHECK_FALSE(tut::satisfied(step(tut::Check::CameraFlown), base, now));
	now = base; now.camX = 12.0f;
	CHECK_FALSE(tut::satisfied(step(tut::Check::CameraFlown), base, now));
	now.camYaw = 0.9f;
	CHECK(tut::satisfied(step(tut::Check::CameraFlown), base, now));

	// Flying all over the place must not satisfy the zoom step — only the pivot
	// distance, which nothing but the wheel and the orbit dolly touch, does.
	CHECK_FALSE(tut::satisfied(step(tut::Check::CameraZoomed), base, now));
	now.camPivot = 3.0f;
	CHECK(tut::satisfied(step(tut::Check::CameraZoomed), base, now));
}

TEST_CASE("tutorial: the sky step needs a sky to be there")
{
	auto step = [](tut::Check check) { return tut::Step{ "t", "T", "B", "", "", check, "" }; };

	tut::Signals base, now;
	base.timeOfDay = 0.5f; now.timeOfDay = 0.9f;
	// No Sky entity in the scene — the slider the step asks for does not exist, so
	// the step must stay open rather than pass on a stale value.
	CHECK_FALSE(tut::satisfied(step(tut::Check::TimeOfDayChanged), base, now));
	base.skyPresent = now.skyPresent = true;
	CHECK(tut::satisfied(step(tut::Check::TimeOfDayChanged), base, now));
	now.timeOfDay = base.timeOfDay;
	CHECK_FALSE(tut::satisfied(step(tut::Check::TimeOfDayChanged), base, now));
}

TEST_CASE("tutorial: PanelsVisited matches whole panel names")
{
	auto step = [](tut::Check check, const char* arg)
	{
		return tut::Step{ "t", "T", "B", "", "", check, arg };
	};
	const char* want = "Scene|World Outliner|Details";

	tut::Signals base, now;
	CHECK(tut::listEntryCount(want) == 3);
	CHECK(tut::listEntry(want, 1) == "World Outliner");
	CHECK(tut::listEntry(want, 9).empty());

	now.visitedPanels = "Scene\nDetails\n";
	CHECK_FALSE(tut::satisfied(step(tut::Check::PanelsVisited, want), base, now));
	CHECK(tut::listEntryVisited(want, 0, now.visitedPanels));
	CHECK_FALSE(tut::listEntryVisited(want, 1, now.visitedPanels));

	// A prefix must not count: "Scene Settings" is not "Scene".
	now.visitedPanels = "Scene Settings\nWorld Outliner\nDetails\n";
	CHECK_FALSE(tut::listEntryVisited(want, 0, now.visitedPanels));

	now.visitedPanels = "World Outliner\nScene\nDetails\n";
	CHECK(tut::satisfied(step(tut::Check::PanelsVisited, want), base, now));
}

TEST_CASE("tutorial: component and tab checks match on content, not position")
{
	tut::Signals base, now;

	auto step = [](tut::Check check, const char* arg)
	{
		return tut::Step{ "t", "T", "B", "", "", check, arg };
	};

	// One MORE than before, not "at least one": a furnished sandbox scene already
	// carries a rigid body, and the step asks the user to add one.
	base.add(tut::Comp::RigidBody, 2);
	now.add(tut::Comp::RigidBody, 2);
	CHECK_FALSE(tut::satisfied(step(tut::Check::ComponentAdded, "rigidbody"), base, now));
	now.add(tut::Comp::RigidBody);
	CHECK(tut::satisfied(step(tut::Check::ComponentAdded, "rigidbody"), base, now));
	CHECK_FALSE(tut::satisfied(step(tut::Check::ComponentAdded, "collider"), base, now));
	// An unknown component name is never satisfiable rather than always satisfied.
	CHECK_FALSE(tut::satisfied(step(tut::Check::ComponentAdded, "nonsense"), base, now));

	// Assets and tabs are matched by TYPE, so renaming the asset on create — which
	// the create flow invites — cannot break the step.
	now.add(tut::Asset::InputAction);
	CHECK(tut::satisfied(step(tut::Check::AssetOfTypeAdded, "inputaction"), base, now));
	CHECK_FALSE(tut::satisfied(step(tut::Check::AssetOfTypeAdded, "widget"), base, now));
	CHECK_FALSE(tut::satisfied(step(tut::Check::AssetOfTypeAdded, "nonsense"), base, now));

	now.addTab(tut::Asset::ParticleSystem);
	CHECK(tut::satisfied(step(tut::Check::TabOfTypeOpened, "particlesystem"), base, now));
	CHECK_FALSE(tut::satisfied(step(tut::Check::TabOfTypeOpened, "material"), base, now));

	// TabOpen (the synthetic Level Script tab) wants the tab to APPEAR: one that
	// was already open when the step began does not count.
	base.openTabs = "Scene\n\n";
	now.openTabs  = "Scene\n\nMyThing\n::LevelScript::\n";
	CHECK(tut::satisfied(step(tut::Check::TabOpen, "::LevelScript::"), base, now));
	base.openTabs = now.openTabs;
	CHECK_FALSE(tut::satisfied(step(tut::Check::TabOpen, "::LevelScript::"), base, now));
	CHECK_FALSE(tut::satisfied(step(tut::Check::TabOpen, ""), base, now));
}

// ─── The tutorial sandbox project ─────────────────────────────────────────────

namespace
{
json readScene(const fs::path& projectRoot)
{
	std::ifstream in(projectRoot / "Content" / "StartupScene.hescene");
	REQUIRE(in.is_open());
	return json::parse(in, nullptr, false);
}

// The scene's entity object with that name, or a null json when absent.
json entityNamed(const json& scene, const char* name)
{
	for (const json& e : scene["entities"])
		if (e.value("name", std::string{}) == name) return e;
	return json{};
}
} // namespace

TEST_CASE("tutorial: the Tutorial preset scaffolds a furnished sandbox")
{
	const auto root = fs::temp_directory_path() / "he_tutorial_project";
	he_test::removeAllQuiet(root);

	ProjectManager pm;
	REQUIRE(pm.createNewProject(root.string(), "TourSandbox",
	                            ProjectPreset::Tutorial,
	                            ProjectScriptLanguage::HorizonCode));

	// The Game folder skeleton, so every folder the tour mentions exists.
	CHECK(fs::is_directory(root / "Content" / "Scripts"));
	CHECK(fs::is_directory(root / "Content" / "Materials"));
	CHECK(fs::is_directory(root / "Content" / "Prefabs"));
	CHECK(fs::is_directory(root / "Content" / "UI"));
	CHECK(fs::exists(root / "TourSandbox.heproj"));
	CHECK(fs::exists(root / "TUTORIAL.md"));

	// The preset is persisted so a reopened sandbox is still recognisable as one.
	{
		std::ifstream in(root / "TourSandbox.heproj");
		const json manifest = json::parse(in, nullptr, false);
		REQUIRE_FALSE(manifest.is_discarded());
		CHECK(manifest.value("preset", -1) == static_cast<int>(ProjectPreset::Tutorial));
	}

	const json scene = readScene(root);
	REQUIRE_FALSE(scene.is_discarded());
	REQUIRE(scene.contains("entities"));

	const json root0 = scene["entities"][0];
	CHECK(root0.value("name", std::string{}) == "World");
	CHECK(root0["children"].size() == 5);   // Sky, Weather, Ground, Cube, Point Light

	// Entities are addressed by stable UUID, so "is a child of the root" is a
	// comparison against the root's own id rather than against the handle 0.
	REQUIRE(root0.contains("uuid"));
	const json rootId = root0["uuid"];
	for (const char* name : { "Sky", "Weather", "Ground", "Cube", "Point Light" })
	{
		CAPTURE(name);
		const json e = entityNamed(scene, name);
		REQUIRE_FALSE(e.is_null());
		CHECK(e["parent"] == rootId);
	}

	// The visible furniture references the engine's built-in cube + default
	// material by their well-known UUIDs, so the sandbox needs no imported assets.
	const json cube = entityNamed(scene, "Cube");
	REQUIRE(cube["components"].contains("mesh"));
	CHECK(cube["components"]["mesh"]["asset"][0].get<uint64_t>() == HE::kDefaultCubeMeshId.hi);
	CHECK(cube["components"]["mesh"]["asset"][1].get<uint64_t>() == HE::kDefaultCubeMeshId.lo);
	REQUIRE(cube["components"].contains("material"));
	CHECK(cube["components"]["material"]["asset"][1].get<uint64_t>() == HE::kDefaultMaterialId.lo);
	CHECK(cube["components"]["transform"]["position"][1].get<float>() == doctest::Approx(1.0f));

	// The ground is a flattened box, not a zero-thickness plane — the physics
	// chapter drops a rigid body onto it — and it uses the grey terrain material
	// so the white default-material cube stands out against it.
	const json ground = entityNamed(scene, "Ground");
	CHECK(ground["components"]["transform"]["scale"][0].get<float>() > 1.0f);
	CHECK(ground["components"]["transform"]["scale"][1].get<float>() > 0.0f);
	CHECK(ground["components"]["material"]["asset"][1].get<uint64_t>() ==
	      HE::kDefaultTerrainMaterialId.lo);
	CHECK(ground["components"]["material"]["asset"][0].get<uint64_t>() ==
	      HE::kDefaultTerrainMaterialId.hi);

	const json light = entityNamed(scene, "Point Light");
	REQUIRE(light["components"].contains("light"));
	CHECK(light["components"]["light"]["type"].get<int>() == static_cast<int>(HE::LightType::Point));

	he_test::removeAllQuiet(root);
}

TEST_CASE("tutorial: the sandbox scene loads through the real scene loader")
{
	// The JSON checks above assert the shape of the file; this one asserts that
	// the engine agrees — a seeded scene that hand-written JSON gets subtly wrong
	// would open as an empty world, which is exactly the first impression the
	// tutorial cannot afford.
	const auto root = fs::temp_directory_path() / "he_tutorial_scene_load";
	he_test::removeAllQuiet(root);

	ProjectManager pm;
	REQUIRE(pm.createNewProject(root.string(), "LoadMe", ProjectPreset::Tutorial,
	                            ProjectScriptLanguage::HorizonCode));

	HorizonWorld world;
	SceneSerializer ser;
	REQUIRE(ser.load(world, root / "Content" / "StartupScene.hescene",
	                 HE::SerializeFormat::JSON));

	auto& reg = world.registry();
	// Wrapped in a bool: doctest decomposes the expression and entt::null's
	// comparison operators are then ambiguous against its Expression_lhs.
	CHECK(bool(world.environmentEntity() != entt::null));   // Sky
	CHECK(bool(world.weatherEntity()     != entt::null));   // Weather

	int meshes = 0, lights = 0;
	std::set<std::string> names;
	for (auto e : reg.view<NameComponent>())
	{
		names.insert(reg.get<NameComponent>(e).name);
		if (reg.all_of<MeshComponent>(e))  ++meshes;
		if (reg.all_of<LightComponent>(e)) ++lights;
	}
	CHECK(names.count("Ground") == 1);
	CHECK(names.count("Cube") == 1);
	CHECK(names.count("Point Light") == 1);
	CHECK(meshes == 2);   // Ground + Cube

	// The Sky entity brings hidden built-in sun + moon directional lights with it,
	// so the count is the seeded point light plus those.
	CHECK(lights >= 1);

	// The cube really did land above the ground, not at the origin.
	for (auto e : reg.view<NameComponent, TransformComponent>())
	{
		if (reg.get<NameComponent>(e).name != "Cube") continue;
		CHECK(reg.get<TransformComponent>(e).position.y == doctest::Approx(1.0f));
	}

	he_test::removeAllQuiet(root);
}

TEST_CASE("tutorial: scaffoldTutorialProject never clobbers an existing file")
{
	const auto root = fs::temp_directory_path() / "he_tutorial_scaffold";
	he_test::removeAllQuiet(root);
	fs::create_directories(root);

	{
		std::ofstream out(root / "TUTORIAL.md");
		out << "my own notes";
	}
	REQUIRE(scaffoldTutorialProject(root.string(), "Whatever"));

	std::ifstream in(root / "TUTORIAL.md");
	const std::string kept((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	CHECK(kept == "my own notes");

	he_test::removeAllQuiet(root);
}

TEST_CASE("tutorial: the other presets are untouched by the new one")
{
	const auto root = fs::temp_directory_path() / "he_tutorial_other_presets";

	SUBCASE("Game still seeds sky and weather but no furniture")
	{
		he_test::removeAllQuiet(root);
		ProjectManager pm;
		REQUIRE(pm.createNewProject(root.string(), "G", ProjectPreset::Game,
		                            ProjectScriptLanguage::Lua));
		const json scene = readScene(root);
		CHECK(scene["entities"][0]["children"].size() == 2);
		CHECK_FALSE(entityNamed(scene, "Sky").is_null());
		CHECK_FALSE(entityNamed(scene, "Weather").is_null());
		CHECK(entityNamed(scene, "Cube").is_null());
		CHECK_FALSE(fs::exists(root / "TUTORIAL.md"));
	}

	SUBCASE("Empty still starts with a bare world")
	{
		he_test::removeAllQuiet(root);
		ProjectManager pm;
		REQUIRE(pm.createNewProject(root.string(), "E", ProjectPreset::Empty,
		                            ProjectScriptLanguage::HorizonCode));
		const json scene = readScene(root);
		CHECK(scene["entities"].size() == 1);
		CHECK(scene["entities"][0]["children"].empty());
	}

	he_test::removeAllQuiet(root);
}
