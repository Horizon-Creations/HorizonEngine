#include "doctest.h"
#include "TestFsUtil.h"
#include "AssetIdentity.h"
#include "EditorPanelState.h"
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// An asset editor's open document is filed under the asset's UUID, not under the
// string its tab happens to hold. What that buys, and what these cases pin:
//   * a collaboration peer's edit comes back as resolveSavePath(key) — another
//     spelling of the tab's path — and still finds the tab;
//   * a rename or move keeps the document (its edits, its dirty flag) and refreshes
//     the path it caches;
//   * a copy made outside the editor carries the original's UUID and must not
//     share its document;
//   * a state filed before the file had a UUID follows it when it gets one.

namespace fs = std::filesystem;

namespace
{
// The editor's panels all have the dirty flag and most cache the content-relative
// path as `relPath`; this is the smallest State that does both.
struct PlainState
{
	bool        dirty = false;
	std::string relPath;
	int         edits = 0;
};

// A State that takes over its own move (the HorizonCode class panel does).
struct MovingState
{
	bool        dirty = false;
	std::string relPath;
	std::string movedTo;
	void onAssetMoved(const std::string& rel) { movedTo = rel; relPath = rel; }
};

struct Project
{
	fs::path           content;
	ContentManager     cm;

	explicit Project(const char* tag)
		: content(fs::temp_directory_path() / (std::string("he_test_asset_identity_") + tag))
		, cm((he_test::removeAllQuiet(content), fs::create_directories(content), content.string()))
	{
		HE::Ed::AssetIdentity::bind(&cm);
		HE::Ed::AssetIdentity::resetCache();
	}
	~Project()
	{
		HE::Ed::AssetIdentity::bind(nullptr);
		he_test::removeAllQuiet(content);
	}

	// A real .hasset with a UUID in its header, at a content-relative path.
	HE::UUID makeClass(const std::string& rel)
	{
		HorizonCodeClassAsset a;
		a.type      = HE::AssetType::HorizonCodeClass;
		a.name      = fs::path(rel).stem().string();
		a.path      = rel;
		a.graphJson = R"({"nextId":1,"nodes":[],"links":[],"variables":[]})";
		REQUIRE(cm.saveAsset(a));
		return a.id;
	}
	std::string abs(const std::string& rel) const { return (content / rel).string(); }
};
} // namespace

TEST_CASE("AssetIdentity: a file with a UUID is that UUID, however its path is spelled")
{
	Project p("spelling");
	const HE::UUID id = p.makeClass("Logic.hasset");
	fs::create_directories(p.content / "Sub");   // a detour through a folder that exists

	const std::string plain   = p.abs("Logic.hasset");
	const std::string detour  = (p.content / "Sub" / ".." / "Logic.hasset").string();
	const std::string doubled = p.content.string() + "//Logic.hasset";
	const std::string saved   = p.cm.resolveSavePath("Logic.hasset");

	const std::string key = HE::Ed::AssetIdentity::keyFor(plain);
	CHECK(key.rfind("uuid:", 0) == 0);
	CHECK(key.size() == 5 + 32);
	CHECK(HE::Ed::AssetIdentity::keyFor(detour)  == key);
	CHECK(HE::Ed::AssetIdentity::keyFor(doubled) == key);
	CHECK(HE::Ed::AssetIdentity::keyFor(saved)   == key);
	(void)id;
}

TEST_CASE("AssetIdentity: no UUID means the normalised path, virtual tabs stay as they are")
{
	Project p("nouuid");
	fs::create_directories(p.content / "Source");
	std::ofstream(p.content / "Source" / "Player.cpp") << "int x;";

	const std::string a = (p.content / "Source" / "Player.cpp").string();
	const std::string b = (p.content / "Source" / "." / "Player.cpp").string();
	CHECK(HE::Ed::AssetIdentity::keyFor(a).rfind("path:", 0) == 0);
	CHECK(HE::Ed::AssetIdentity::keyFor(a) == HE::Ed::AssetIdentity::keyFor(b));

	CHECK(HE::Ed::AssetIdentity::keyFor(":Settings") == ":Settings");
	CHECK(HE::Ed::AssetIdentity::keyFor("").empty());
}

TEST_CASE("AssetPanelState: the document is found under another spelling of its path")
{
	Project p("peer");
	p.makeClass("Logic.hasset");
	fs::create_directories(p.content / "Sub");

	// The Content Browser opens the tab with whatever its directory walk produced.
	const std::string tabPath = (p.content / "Sub" / ".." / "Logic.hasset").string();
	AssetPanelState<PlainState> states;
	states[tabPath].dirty = true;

	// A collaboration peer's delta addresses it as the content-relative key; the
	// editor turns that back into a path with resolveSavePath.
	const std::string fromKey = p.cm.resolveSavePath("Logic.hasset");
	REQUIRE(states.find(fromKey) != nullptr);
	CHECK(states.find(fromKey) == states.find(tabPath));
	CHECK(states.dirty(fromKey));
}

TEST_CASE("AssetPanelState: renaming or moving an asset keeps its document and refreshes the path it caches")
{
	Project p("rename");
	p.makeClass("Logic.hasset");

	const std::string oldPath = p.abs("Logic.hasset");
	AssetPanelState<PlainState> states;
	PlainState& st = states[oldPath];
	st.relPath = "Logic.hasset";
	st.dirty   = true;
	st.edits   = 7;

	// The Content Browser's rename: the file moves, references are retargeted, the
	// tab is pointed at the new path. The panel is then asked under the new path.
	fs::create_directories(p.content / "Classes");
	const std::string newPath = p.abs("Classes/Renamed.hasset");
	fs::rename(oldPath, newPath);
	p.cm.retargetAssetReferences("Logic.hasset", "Classes/Renamed.hasset");

	PlainState* moved = states.find(newPath);
	REQUIRE(moved != nullptr);
	CHECK(moved == &st);                       // the very same document, not a fresh one
	CHECK(moved->dirty);
	CHECK(moved->edits == 7);
	CHECK(moved->relPath == "Classes/Renamed.hasset");

	// What the quit prompt and the recovery copy are told is the path it has now.
	std::vector<std::string> unsaved;
	states.appendDirtyPaths(unsaved);
	REQUIRE(unsaved.size() == 1);
	CHECK(unsaved[0] == newPath);

	// And a second ask in a different spelling of the new path is not a "move".
	const std::string respelled = p.content.string() + "//Classes//Renamed.hasset";
	CHECK(states.find(respelled) == &st);
	CHECK(st.relPath == "Classes/Renamed.hasset");
}

TEST_CASE("AssetPanelState: a State that declares onAssetMoved is told the new content-relative path")
{
	Project p("moving");
	p.makeClass("Logic.hasset");

	AssetPanelState<MovingState> states;
	const std::string oldPath = p.abs("Logic.hasset");
	states[oldPath].relPath = "Logic.hasset";

	fs::rename(oldPath, p.abs("Renamed.hasset"));
	p.cm.retargetAssetReferences("Logic.hasset", "Renamed.hasset");

	MovingState* st = states.find(p.abs("Renamed.hasset"));
	REQUIRE(st != nullptr);
	CHECK(st->movedTo == "Renamed.hasset");
}

TEST_CASE("AssetPanelState: a copy made outside the editor does not share the original's document")
{
	Project p("copy");
	p.makeClass("Original.hasset");
	REQUIRE(p.cm.loadAsset("Original.hasset") != HE::UUID{});   // the registry now places the id

	fs::copy_file(p.abs("Original.hasset"), p.abs("Copy.hasset"));
	p.cm.scanContentDirectory();

	AssetPanelState<PlainState> states;
	states[p.abs("Original.hasset")].edits = 1;
	states[p.abs("Copy.hasset")].edits     = 2;

	REQUIRE(states.find(p.abs("Original.hasset")) != nullptr);
	REQUIRE(states.find(p.abs("Copy.hasset")) != nullptr);
	CHECK(states.find(p.abs("Original.hasset")) != states.find(p.abs("Copy.hasset")));
	CHECK(states.find(p.abs("Original.hasset"))->edits == 1);
	CHECK(states.find(p.abs("Copy.hasset"))->edits == 2);
}

TEST_CASE("AssetPanelState: a document filed before the file had a UUID follows it when it gets one")
{
	Project p("legacy");
	const std::string path = p.abs("Old.hasset");
	std::ofstream(path) << "not an asset yet";   // no header, so no UUID

	AssetPanelState<PlainState> states;
	PlainState& st = states[path];
	st.edits = 3;
	CHECK(HE::Ed::AssetIdentity::keyFor(path).rfind("path:", 0) == 0);

	// The first save writes a real header.
	p.makeClass("Old.hasset");
	HE::Ed::AssetIdentity::resetCache();
	CHECK(HE::Ed::AssetIdentity::keyFor(path).rfind("uuid:", 0) == 0);

	PlainState* after = states.find(path);
	REQUIRE(after != nullptr);
	CHECK(after == &st);
	CHECK(after->edits == 3);
}

TEST_CASE("AssetPanelState: forget drops the document even after the file is gone")
{
	Project p("forget");
	p.makeClass("Gone.hasset");

	AssetPanelState<PlainState> states;
	const std::string path = p.abs("Gone.hasset");
	states[path].edits = 1;

	fs::remove(path);
	states.forget(path);

	std::vector<std::string> seen;
	states.forEach([&](const std::string& pth, const PlainState&) { seen.push_back(pth); });
	CHECK(seen.empty());
}

TEST_CASE("AssetPanelState: find never creates a document")
{
	Project p("nocreate");
	p.makeClass("A.hasset");

	AssetPanelState<PlainState> states;
	CHECK(states.find(p.abs("A.hasset")) == nullptr);
	CHECK_FALSE(states.dirty(p.abs("A.hasset")));
	std::vector<std::string> seen;
	states.forEach([&](const std::string& pth, const PlainState&) { seen.push_back(pth); });
	CHECK(seen.empty());
}
