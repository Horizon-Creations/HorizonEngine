#include "doctest.h"

#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <Platform/ProjectWriteGate.h>
#include <UIWidget/UIWidgetTree.h>

#include "TestFsUtil.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

// ── The export lock ──────────────────────────────────────────────────────────
// An export reads Content/ on a worker thread; a save, import or rename in the
// middle of that gives a package that holds half of each. The editor installs a
// probe ("is an export running?") and a handler ("tell the user") and the writers
// ask HE::ProjectWriteGate::refuse() where they are about to change a file.

namespace
{
	// A directory of its own under the system temp, removed again (quietly) at the end.
	struct TempDir
	{
		std::filesystem::path path;
		explicit TempDir(const char* name)
		{
			path = std::filesystem::temp_directory_path() / (std::string(name) + "_" + std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
			he_test::removeAllQuiet(path);
			std::filesystem::create_directories(path);
		}
		~TempDir() { he_test::removeAllQuiet(path); }
	};

	std::atomic<bool> g_exporting{ false };
	std::atomic<int>  g_told{ 0 };
	std::string       g_toldWhat;

	bool probe() { return g_exporting.load(); }
	void handler(const char* what) { ++g_told; g_toldWhat = what ? what : ""; }

	// Whatever the case does, the gate is open again afterwards: it is process-wide
	// state and every later test in this binary writes files.
	struct Installed
	{
		Installed()  { g_exporting = false; g_told = 0; g_toldWhat.clear();
		               HE::ProjectWriteGate::install(&probe, &handler); }
		~Installed() { HE::ProjectWriteGate::install(nullptr, nullptr); g_exporting = false; }
	};
}

TEST_CASE("Write gate: nothing installed means nothing is locked")
{
	HE::ProjectWriteGate::install(nullptr, nullptr);
	CHECK_FALSE(HE::ProjectWriteGate::locked());
	CHECK_FALSE(HE::ProjectWriteGate::refuse("save this asset"));
}

TEST_CASE("Write gate: refuse() is true and tells the user only while the probe says so")
{
	Installed gate;
	CHECK_FALSE(HE::ProjectWriteGate::refuse("save this asset"));
	CHECK(g_told == 0);

	g_exporting = true;
	CHECK(HE::ProjectWriteGate::locked());
	CHECK(HE::ProjectWriteGate::refuse("rename a file"));
	CHECK(g_told == 1);
	CHECK(g_toldWhat == "rename a file");
	// locked() alone asks without bothering anyone.
	CHECK(HE::ProjectWriteGate::locked());
	CHECK(g_told == 1);

	g_exporting = false;
	CHECK_FALSE(HE::ProjectWriteGate::refuse("rename a file"));
}

TEST_CASE("Write gate: a locked ContentManager does not write the asset, and writes it once open")
{
	TempDir dir("he_write_gate");
	ContentManager cm(dir.path.string());
	std::filesystem::create_directories(dir.path / "UI");

	UIWidgetAsset asset;
	asset.type     = HE::AssetType::Widget;
	asset.name     = "Gated";
	asset.path     = "UI/Gated.hasset";
	asset.treeJson = HE::uiWidgetTreeToJson(HE::UIWidgetTree{});

	Installed gate;
	g_exporting = true;
	CHECK_FALSE(cm.saveAsset(asset));
	CHECK(g_told == 1);
	CHECK_FALSE(std::filesystem::exists(dir.path / "UI" / "Gated.hasset"));

	// The edit was not lost: the same asset saves as soon as the export is over.
	g_exporting = false;
	CHECK(cm.saveAsset(asset));
	CHECK(std::filesystem::exists(dir.path / "UI" / "Gated.hasset"));
}
