#include "doctest.h"
#include "TestFsUtil.h"
#include "EditorTabs.h"
#include <ContentManager/ContentManager.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// "Double-click an asset that is already open and you get that tab, not a
// second one." Every way of opening an asset tab (the Content Browser, a
// material node's Open Function, the console's go-to, the session restore) goes
// through EditorTabs::openOrFocus, so what is asked here is the lookup: the same
// file spelled two ways is ONE tab, two different files are two, and a tab
// follows its asset when the asset is renamed.

namespace fs = std::filesystem;

namespace
{
using Tab  = AppContext::EditorTab;
using Tabs = std::vector<Tab>;

// A project on disk: <root>/Content/... and <root>/EngineContent/..., with
// real files, because "is this the same file" is a question about the disk.
struct Project
{
	fs::path root;
	fs::path content;
	fs::path engine;

	explicit Project(const char* tag)
	{
		root    = fs::temp_directory_path() / (std::string("he_test_editor_tabs_") + tag);
		content = root / "Content";
		engine  = root / "EngineContent";
		he_test::removeAllQuiet(root);
		fs::create_directories(content);
		fs::create_directories(engine);
	}
	~Project() { he_test::removeAllQuiet(root); }

	static fs::path touch(const fs::path& p)
	{
		fs::create_directories(p.parent_path());
		std::ofstream(p) << "x";
		return p;
	}
};

Tabs sceneOnly() { return Tabs{ { "Viewport", "", false, true } }; }

int assetTabCount(const Tabs& tabs)
{
	int n = 0;
	for (const Tab& t : tabs) if (!t.assetPath.empty()) ++n;
	return n;
}
} // namespace

TEST_CASE("EditorTabs: opening the same asset twice gives one tab, and it is the active one")
{
	Project p("twice");
	const std::string mat = Project::touch(p.content / "Materials" / "Water.hasset").string();

	Tabs tabs = sceneOnly();
	int  active = 0, select = -1;

	const int first = EditorTabs::openOrFocus(tabs, active, select, mat);
	REQUIRE(first == 1);
	CHECK(assetTabCount(tabs) == 1);
	CHECK(tabs[1].label == "Water");
	CHECK(active == 1);
	CHECK(select == 1);

	// The user is back on the scene tab (where the Content Browser lives) and
	// double-clicks the same tile.
	active = 0;
	select = -1;
	const int second = EditorTabs::openOrFocus(tabs, active, select, mat);
	CHECK(second == first);
	CHECK(assetTabCount(tabs) == 1);   // no second tab
	CHECK(active == 1);                // and the one that exists is in front
	CHECK(select == 1);                // the tab bar is told to select it
}

TEST_CASE("EditorTabs: one file spelled two ways is one tab")
{
	Project p("spelling");
	const fs::path file = Project::touch(p.content / "Materials" / "Water.hasset");

	Tabs tabs = sceneOnly();
	int  active = 0, select = -1;
	EditorTabs::openOrFocus(tabs, active, select, file.string());

	SUBCASE("a doubled separator and a trailing-slash root, as a root joined to a relative path")
	{
		const std::string joined = p.content.string() + "/" + "/Materials/Water.hasset";
		EditorTabs::openOrFocus(tabs, active, select, joined);
		CHECK(assetTabCount(tabs) == 1);
	}
	SUBCASE("a dot-dot detour")
	{
		const std::string detour = (p.content / "Materials" / ".." / "Materials" / "Water.hasset").string();
		EditorTabs::openOrFocus(tabs, active, select, detour);
		CHECK(assetTabCount(tabs) == 1);
	}
	SUBCASE("a symlinked folder")
	{
		std::error_code ec;
		fs::create_directory_symlink(p.content / "Materials", p.content / "Linked", ec);
		if (ec) return;   // no symlinks here (an unprivileged Windows account)
		EditorTabs::openOrFocus(tabs, active, select, (p.content / "Linked" / "Water.hasset").string());
		CHECK(assetTabCount(tabs) == 1);
	}
	CHECK(active == 1);
}

TEST_CASE("EditorTabs: the path ContentManager joins is the browser's tile path")
{
	Project p("contentmanager");
	const fs::path file = Project::touch(p.content / "Materials" / "Water.hasset");

	// A content root with a trailing slash: resolveAbsolutePath joins with "/" on
	// top of it, so this is the spelling "Open Function" brings back.
	ContentManager cm(p.content.string() + "/");
	const std::string viaJoin = cm.resolveAbsolutePath("Materials/Water.hasset");
	REQUIRE(viaJoin != file.string());

	Tabs tabs = sceneOnly();
	int  active = 0, select = -1;
	EditorTabs::openOrFocus(tabs, active, select, file.string(), {}, &cm);
	EditorTabs::openOrFocus(tabs, active, select, viaJoin, {}, &cm);
	CHECK(assetTabCount(tabs) == 1);
	CHECK(active == 1);
}

TEST_CASE("EditorTabs: the shipped Engine default and the project's override of it are one asset")
{
	Project p("engine");
	const fs::path shipped   = Project::touch(p.engine / "Materials" / "Water.hasset");
	const fs::path overriden = Project::touch(p.content / "Engine" / "Materials" / "Water.hasset");

	ContentManager cm(p.content.string());
	cm.setEngineContentRoot(p.engine.string());
	// The panels load through the content-relative path, so both land on
	// "Engine/Materials/Water.hasset" — one asset, one editor.
	REQUIRE(cm.toContentRelativePath(shipped.string()) == "Engine/Materials/Water.hasset");
	REQUIRE(cm.toContentRelativePath(overriden.string()) == "Engine/Materials/Water.hasset");

	Tabs tabs = sceneOnly();
	int  active = 0, select = -1;
	EditorTabs::openOrFocus(tabs, active, select, shipped.string(), {}, &cm);
	EditorTabs::openOrFocus(tabs, active, select, overriden.string(), {}, &cm);
	CHECK(assetTabCount(tabs) == 1);

	// Without the ContentManager there is nothing to say they are related.
	Tabs bare = sceneOnly();
	EditorTabs::openOrFocus(bare, active, select, shipped.string());
	EditorTabs::openOrFocus(bare, active, select, overriden.string());
	CHECK(assetTabCount(bare) == 2);
}

TEST_CASE("EditorTabs: assets that only share a name stay separate tabs")
{
	Project p("samename");
	ContentManager cm(p.content.string());
	cm.setEngineContentRoot(p.engine.string());

	const std::string a = Project::touch(p.content / "Props" / "Rock.hasset").string();
	const std::string b = Project::touch(p.content / "Terrain" / "Rock.hasset").string();   // other folder
	const std::string c = Project::touch(p.content / "Props" / "Rock.png").string();         // other type
	const std::string d = Project::touch(p.engine / "Props" / "Rock.hasset").string();       // Engine/Props/Rock

	Tabs tabs = sceneOnly();
	int  active = 0, select = -1;
	for (const std::string* path : { &a, &b, &c, &d })
		EditorTabs::openOrFocus(tabs, active, select, *path, {}, &cm);
	CHECK(assetTabCount(tabs) == 4);
	CHECK(active == 4);

	// And each one is found again as itself, not as a namesake.
	for (const std::string* path : { &a, &b, &c, &d })
	{
		const int idx = EditorTabs::openOrFocus(tabs, active, select, *path, {}, &cm);
		CHECK(tabs[idx].assetPath == *path);
	}
	CHECK(assetTabCount(tabs) == 4);
}

TEST_CASE("EditorTabs: a tab whose close is pending is kept, not left to be erased under the selection")
{
	Project p("closing");
	const std::string mat = Project::touch(p.content / "Water.hasset").string();

	Tabs tabs = sceneOnly();
	int  active = 0, select = -1;
	EditorTabs::openOrFocus(tabs, active, select, mat);
	tabs[1].open = false;   // closed in this frame; the tab bar erases it on its next pass

	active = 0;
	select = -1;
	EditorTabs::openOrFocus(tabs, active, select, mat);
	CHECK(assetTabCount(tabs) == 1);
	CHECK(tabs[1].open);
	CHECK(active == 1);
	CHECK(select == 1);

	// Once the tab bar HAS erased a closed tab, opening is a fresh tab again.
	tabs.erase(tabs.begin() + 1);
	EditorTabs::openOrFocus(tabs, active, select, mat);
	CHECK(assetTabCount(tabs) == 1);
	CHECK(active == 1);
}

TEST_CASE("EditorTabs: virtual tabs match by name only and keep their label")
{
	Tabs tabs = sceneOnly();
	int  active = 0, select = -1;
	EditorTabs::openOrFocus(tabs, active, select, "::Preferences::", "Preferences");
	EditorTabs::openOrFocus(tabs, active, select, "::LevelScript::", "Level Script");
	active = 0;
	EditorTabs::openOrFocus(tabs, active, select, "::Preferences::", "Preferences");
	CHECK(assetTabCount(tabs) == 2);
	CHECK(active == 1);
	CHECK(tabs[1].label == "Preferences");

	CHECK(EditorTabs::key("::Preferences::") == "::Preferences::");
	CHECK(EditorTabs::openOrFocus(tabs, active, select, "") == -1);   // nothing to open
}

TEST_CASE("EditorTabs: focusSceneTab selects the tab with no asset behind it")
{
	Tabs tabs = sceneOnly();
	tabs.push_back({ "A", "/p/A.hasset", true, true });
	int active = 1, select = -1;
	CHECK(EditorTabs::focusSceneTab(tabs, active, select));
	CHECK(active == 0);
	CHECK(select == 0);

	Tabs none;
	CHECK_FALSE(EditorTabs::focusSceneTab(none, active, select));
}

TEST_CASE("EditorTabs: the open scene is recognised however its path is spelled")
{
	Project p("scene");
	const fs::path scene = Project::touch(p.content / "Levels" / "Main.hescene");
	// What the Content Browser double-click compares: the editor's current scene
	// against the tile — equal files, so the scene tab is focused, not reloaded.
	CHECK(EditorTabs::sameAsset(scene.string(),
	                            (p.content / "Levels" / ".." / "Levels" / "Main.hescene").string()));
	CHECK_FALSE(EditorTabs::sameAsset(scene.string(),
	                                  Project::touch(p.content / "Levels" / "Other.hescene").string()));
	CHECK_FALSE(EditorTabs::sameAsset("", scene.string()));   // an unsaved scene is none of them
}

TEST_CASE("EditorTabs: a renamed or moved asset keeps its tab, and a double-click on it finds that tab")
{
	Project p("retarget");
	const fs::path oldFile = Project::touch(p.content / "Materials" / "Water.hasset");
	const fs::path newFile = p.content / "Materials" / "Ocean.hasset";

	Tabs tabs = sceneOnly();
	int  active = 0, select = -1;
	EditorTabs::openOrFocus(tabs, active, select, oldFile.string());
	EditorTabs::openOrFocus(tabs, active, select, Project::touch(p.content / "Materials" / "Mud.hasset").string());

	fs::rename(oldFile, newFile);
	CHECK(EditorTabs::retarget(tabs, oldFile.string(), newFile.string()) == 1);
	CHECK(tabs[1].assetPath == newFile.string());
	CHECK(tabs[1].label == "Ocean");
	CHECK(tabs[2].label == "Mud");

	const int idx = EditorTabs::openOrFocus(tabs, active, select, newFile.string());
	CHECK(idx == 1);
	CHECK(assetTabCount(tabs) == 2);
}

TEST_CASE("EditorTabs: renaming a folder moves the tabs on the assets beneath it, and only those")
{
	Project p("retarget_folder");
	const fs::path dir    = p.content / "Mat";
	const fs::path newDir = p.content / "Surfaces";

	Tabs tabs = sceneOnly();
	int  active = 0, select = -1;
	EditorTabs::openOrFocus(tabs, active, select, Project::touch(dir / "Sand.hasset").string());
	EditorTabs::openOrFocus(tabs, active, select, Project::touch(dir / "Deep" / "Mud.hasset").string());
	// "Materials" starts with the same letters as "Mat" but is another folder.
	const std::string bystander = Project::touch(p.content / "Materials" / "Water.hasset").string();
	EditorTabs::openOrFocus(tabs, active, select, bystander);

	CHECK(EditorTabs::retarget(tabs, dir.string(), newDir.string(), /*folder=*/true) == 2);
	CHECK(tabs[1].assetPath == (newDir / "Sand.hasset").string());
	CHECK(tabs[2].assetPath == (newDir / "Deep" / "Mud.hasset").string());
	CHECK(tabs[2].label == "Mud");
	CHECK(tabs[3].assetPath == bystander);

	// Not asked as a folder, a path that merely lies under the old one is not moved.
	Tabs again = sceneOnly();
	EditorTabs::openOrFocus(again, active, select, (dir / "Sand.hasset").string());
	CHECK(EditorTabs::retarget(again, dir.string(), newDir.string(), /*folder=*/false) == 0);
}

TEST_CASE("EditorTabs: a saved tab list with duplicates comes back without them, and the active tab follows")
{
	Project p("restore");
	const fs::path a = Project::touch(p.content / "A.hasset");
	const fs::path b = Project::touch(p.content / "B.hasset");

	Tabs tabs = sceneOnly();
	tabs.push_back({ "A", a.string(), true, true });
	tabs.push_back({ "B", b.string(), true, true });
	tabs.push_back({ "A", (p.content / "." / "A.hasset").string(), true, true });   // the same file again
	tabs.push_back({ "C", Project::touch(p.content / "C.hasset").string(), true, true });

	const std::vector<int> where = EditorTabs::dedupe(tabs);
	REQUIRE(where.size() == 5);
	CHECK(tabs.size() == 4);
	CHECK(where[0] == 0);
	CHECK(where[3] == 1);   // the duplicate A is the first A
	CHECK(where[4] == 3);   // C moved up by one
	CHECK(tabs[3].label == "C");
}
