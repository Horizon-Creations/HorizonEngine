#include "doctest.h"
#include "ImGuiSoftwareRaster.h"

#include "AutoLodDialog.h"
#include "EditorApplication.h"   // AppContext
#include "EditorSelection.h"
#include "EditorTheme.h"
#include "EditorUndo.h"
#include "EditorWidgets.h"
#include "ImporterCommon.h"      // Importer::writeAsset

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/LODComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// ── Generate LODs, driven headless ───────────────────────────────────────────
// The dialog opens on request and closes on Escape without running anything; the
// runner behind it writes the sidecars and puts the chain where the request asked
// (a foliage layer's Detail Levels, an entity's LOD component, nowhere), refuses an
// engine mesh, and the scene edit is one undo step. HE_UI_DUMP_DIR writes a frame.

namespace fs = std::filesystem;
using namespace HE::Ed;

namespace
{
	constexpr int W = 640, H = 520;
	constexpr const char* kPopup = "Generate LODs##autolod";

	struct Harness
	{
		Harness()
		{
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(float(W), float(H));
			io.DeltaTime   = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
			io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
#ifdef HE_EDITOR_DEPS_DIR
			const std::string font = std::string(HE_EDITOR_DEPS_DIR) + "/Fonts/Roboto_Condensed-Bold.ttf";
			if (std::FILE* f = std::fopen(font.c_str(), "rb"))
			{
				std::fclose(f);
				ImFontConfig cfg;
				cfg.OversampleH = 2;
				cfg.OversampleV = 2;
				io.FontDefault = io.Fonts->AddFontFromFileTTF(font.c_str(), 15.0f, &cfg);
			}
#endif
			applyHorizonDarkTheme();
		}
		~Harness() { ImGui::DestroyContext(); }
	};

	struct ContextBits
	{
		EditorConfig    config;
		bool            vsync = false;
		std::string     backendName = "Software";
		EditorSelection selection;
		std::string     scenePath;
		bool            exitRequested = false, projectLoaded = true;
		bool            refreshPending = false, refreshDone = false;
		int   fpsOffset = 0, fpsCount = 0;
		float fpsAccum = 0.0f, smoothFps = 0.0f;
		std::vector<AppContext::EditorTab> tabs;
		int   activeTab = 0;
		float cbTreeWidth = 200.0f;
		int   hubPreset = 0, hubLang = 0;
		bool  hubFx = false;
		std::string hubCreateError, hubOpenError;
		int   hubRemoveIndex = -1;
		bool  hubRemoveRequested = false;
		std::string dirResult, fileResult;
		bool  dirReady = false, fileReady = false;

		AppContext make()
		{
			return AppContext{
				.editorConfig          = config,
				.vsync                 = vsync,
				.backendName           = backendName,
				.selection             = selection,
				.currentScenePath      = scenePath,
				.exitRequested         = exitRequested,
				.projectLoaded         = projectLoaded,
				.contentRefreshPending = refreshPending,
				.contentRefreshDone    = refreshDone,
				.fpsHistoryOffset      = fpsOffset,
				.fpsAccum              = fpsAccum,
				.fpsAccumCount         = fpsCount,
				.smoothFps             = smoothFps,
				.tabs                  = tabs,
				.activeTab             = activeTab,
				.cbTreeWidth           = cbTreeWidth,
				.hubSelectedPreset     = hubPreset,
				.hubSelectedLang       = hubLang,
				.hubAdvancedShaderFx   = hubFx,
				.hubCreateError        = hubCreateError,
				.hubOpenError          = hubOpenError,
				.hubRemoveIndex        = hubRemoveIndex,
				.hubRemoveRequested    = hubRemoveRequested,
				.pendingDirResult      = dirResult,
				.pendingDirReady       = dirReady,
				.pendingFileResult     = fileResult,
				.pendingFileReady      = fileReady,
			};
		}
	};

	// IsPopupOpen(const char*) needs a current window; between frames there is none, so
	// look the popup up by its window name in the open stack.
	bool popupOpen()
	{
		const ImGuiContext& g = *ImGui::GetCurrentContext();
		for (const ImGuiPopupData& p : g.OpenPopupStack)
			if (p.Window && std::strstr(p.Window->Name, kPopup)) return true;
		return false;
	}

	void frame(AppContext& ctx, he_ui::Image* shot = nullptr)
	{
		ImGui::NewFrame();
		AutoLodDialog::draw(ctx);
		EditorWidgets::drawQueuedHelp();
		ImGui::Render();
		if (shot) *shot = he_ui::rasterize(ImGui::GetDrawData(), W, H);
	}

	void settle(AppContext& ctx, he_ui::Image* shot = nullptr)
	{
		for (int i = 0; i < 4; ++i) frame(ctx, i == 3 ? shot : nullptr);
	}

	void dump(const he_ui::Image& img, const char* name)
	{
		if (const char* dir = std::getenv("HE_UI_DUMP_DIR"))
			he_ui::writeBmp(img, (fs::path(dir) / name).string());
	}

	// A sphere written into the project as Meshes/Ball.hasset, loaded into `cm`.
	HE::UUID makeBall(ContentManager& cm, const fs::path& root)
	{
		StaticMeshAsset m;
		m.type = HE::AssetType::StaticMesh;
		m.path = "Meshes/Ball.hasset";
		m.name = "Ball";
		const int lat = 24, lon = 32;
		for (int y = 0; y <= lat; ++y)
			for (int x = 0; x <= lon; ++x)
			{
				const float th = float(y) / lat * 3.14159265f, ph = float(x) / lon * 6.2831853f;
				const float nx = std::sin(th) * std::cos(ph), ny = std::cos(th), nz = std::sin(th) * std::sin(ph);
				m.vertices.insert(m.vertices.end(), { nx, ny, nz });
				m.normals.insert(m.normals.end(), { nx, ny, nz });
				m.uvs.insert(m.uvs.end(), { float(x) / lon, float(y) / lat });
			}
		const uint32_t row = lon + 1;
		for (int y = 0; y < lat; ++y)
			for (int x = 0; x < lon; ++x)
			{
				const uint32_t a = uint32_t(y) * row + uint32_t(x), b = a + 1, c = a + row, d = c + 1;
				m.indices.insert(m.indices.end(), { a, c, b, b, c, d });
			}
		REQUIRE(Importer::writeAsset(m, root));
		return cm.loadAsset("Meshes/Ball.hasset");
	}

	struct Project
	{
		fs::path       root;
		ContentManager cm;
		HE::UUID       ball;
		explicit Project(const char* name)
			: root(fs::temp_directory_path() / name), cm((fs::remove_all(root), fs::create_directories(root), root.string()))
		{
			ball = makeBall(cm, root);
		}
		~Project() { std::error_code ec; fs::remove_all(root, ec); }
	};
}

TEST_CASE("Generate LODs: the dialog opens on request and Escape closes it without running anything")
{
	Harness h;
	ContextBits bits;
	AppContext ctx = bits.make();
	frame(ctx);
	CHECK_FALSE(popupOpen());

	AutoLodDialog::Request req;
	req.kind = AutoLodDialog::Kind::MeshAsset;
	AutoLodDialog::open(req);
	he_ui::Image shot;
	settle(ctx, &shot);
	CHECK(popupOpen());
	dump(shot, "auto_lod_dialog.bmp");

	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
	frame(ctx);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
	settle(ctx);
	CHECK_FALSE(popupOpen());
}

TEST_CASE("Generate LODs: the option rows change the level run they announce")
{
	Harness h;
	ImGui::NewFrame();
	ImGui::SetNextWindowSize(ImVec2(400, 300));
	ImGui::Begin("rows");
	AutoLodDialog::Options o;
	CHECK_FALSE(AutoLodDialog::drawOptions(o, AutoLodDialog::Kind::FoliageLayer));   // drawing alone changes nothing
	ImGui::End();
	ImGui::Render();

	const AutoLod::Settings s = AutoLodDialog::toSettings(o);
	REQUIRE(s.ratios.size() == 3u);                       // 100 / 50 / 25 / 10 %
	CHECK(s.ratios[0] == doctest::Approx(0.5f));
	CHECK(s.ratios[2] == doctest::Approx(0.1f));
	AutoLodDialog::Options more = o;
	more.levels = 5;
	CHECK(AutoLodDialog::toSettings(more).ratios.size() == 5u);
}

TEST_CASE("Generate LODs: a foliage layer gets sorted detail levels from the chain, and Undo takes them back")
{
	Project proj("he_test_auto_lod_ui_layer");
	HorizonWorld world;
	const Entity land = world.createEntity("Land");
	FoliageComponent fol;
	fol.layers[0].meshAssetId  = proj.ball;
	fol.layers[0].drawDistance = 3000.0f;
	world.registry().emplace<FoliageComponent>(land, fol);

	ContextBits bits;
	AppContext ctx = bits.make();
	ctx.contentManager = &proj.cm;
	ctx.world          = &world;
	EditorUndo undo;
	undo.setWorld(&world);
	ctx.undoSys = &undo;

	AutoLodDialog::Request req;
	req.kind   = AutoLodDialog::Kind::FoliageLayer;
	req.entity = land;
	AutoLodDialog::Options opt;
	opt.levels = 2;
	const AutoLodDialog::Outcome r = AutoLodDialog::run(ctx, req, opt);
	INFO(r.message);
	REQUIRE(r.ok);
	CHECK(r.levels == 2u);
	CHECK(r.lastTris < r.lod0Tris);
	CHECK(fs::exists(proj.root / "Meshes/Ball_LOD1.hasset"));
	CHECK(fs::exists(proj.root / "Meshes/Ball_LOD2.hasset"));

	const FoliageLayer& layer = world.registry().get<FoliageComponent>(land).layers[0];
	REQUIRE(layer.lods.size() == 2u);
	CHECK(proj.cm.pathForId(layer.lods[0].meshAssetId) == "Meshes/Ball_LOD1.hasset");
	CHECK(layer.lods[1].distance > layer.lods[0].distance);
	CHECK(layer.drawDistance == 3000.0f);
	CHECK(undo.canUndo());

	REQUIRE(undo.undo());
	bool found = false;
	for (auto [e, f] : world.registry().view<FoliageComponent>().each())
	{
		found = true;
		CHECK(f.layers[0].lods.empty());
	}
	CHECK(found);
}

// Foliage mode weighs a level by the leaf area the plant's cut-out texture says it has. A layer
// with its own material draws that one; a layer without draws the mesh's own, so the mask must
// come from there (step 16). The two meshes have the same cards, only one names a material.
// The cut-out is opaque but for one corner texel no card touches, so the leaf area is the card
// area: the coverage goal then holds every level back near LOD0, the simplifier's step floor
// refuses those, and the run says there is nothing to reduce. A plain run (no mask) reduces the
// same cards fine, which is how the outcome shows whether a mask was in use.
TEST_CASE("Generate LODs: the foliage alpha mask comes from the mesh's own material when the layer has no override")
{
	Project proj("he_test_auto_lod_ui_alpha_fallback");
	const fs::path& root = proj.root;

	TextureAsset tex;
	tex.type = HE::AssetType::Texture;
	tex.name = "Leaf";
	tex.path = "Textures/Leaf.hasset";
	tex.width = 8; tex.height = 8; tex.channels = 4;
	tex.data.resize(8 * 8 * 4, 255);
	tex.data[3] = 0;
	REQUIRE(Importer::writeAsset(tex, root));
	TextureAsset solid = tex;           // opaque everywhere: says nothing about leaf area
	solid.name = "Solid";
	solid.path = "Textures/Solid.hasset";
	std::fill(solid.data.begin(), solid.data.end(), uint8_t{ 255 });
	REQUIRE(Importer::writeAsset(solid, root));

	auto writeMaterial = [&](const char* name, const char* texture) {
		MaterialAsset m;
		m.type         = HE::AssetType::Material;
		m.name         = name;
		m.path         = std::string("Materials/") + name + ".hasset";
		m.shaderPath   = "Shaders/Lit.hasset";
		m.texturePaths = { texture };
		REQUIRE(Importer::writeAsset(m, root));
		return m.path;
	};
	const std::string leafMat  = writeMaterial("Leaf",  "Textures/Leaf.hasset");
	const std::string solidMat = writeMaterial("Solid", "Textures/Solid.hasset");

	auto writeBush = [&](const char* name, const std::string& material) {
		StaticMeshAsset m;
		m.type = HE::AssetType::StaticMesh;
		m.name = name;
		m.path = std::string("Meshes/") + name + ".hasset";
		m.materialPath = material;
		for (int i = 0; i < 200; ++i)
		{
			const float a = float(i) * 0.7f, r = 0.2f + 0.05f * float(i % 7);
			const float cx = std::cos(a) * r, cz = std::sin(a) * r, h = 0.1f * float(i % 5), s = 0.08f;
			const float u0 = 0.3f, u1 = 0.7f;
			const uint32_t base = uint32_t(m.vertices.size() / 3);
			const float p[4][3]  = { { cx - s, h, cz }, { cx + s, h, cz }, { cx + s, h + 2 * s, cz }, { cx - s, h + 2 * s, cz } };
			const float uv[4][2] = { { u0, 0.1f }, { u1, 0.1f }, { u1, 0.9f }, { u0, 0.9f } };
			for (int k = 0; k < 4; ++k)
			{
				m.vertices.insert(m.vertices.end(), { p[k][0], p[k][1], p[k][2] });
				m.normals.insert(m.normals.end(), { 0.0f, 0.0f, 1.0f });
				m.uvs.insert(m.uvs.end(), { uv[k][0], uv[k][1] });
			}
			m.indices.insert(m.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
		}
		REQUIRE(Importer::writeAsset(m, root));
		return proj.cm.loadAsset(m.path);
	};
	const HE::UUID bare = writeBush("Bare", "");
	const HE::UUID bush = writeBush("Bush", leafMat);
	const HE::UUID leafId  = proj.cm.loadAsset(leafMat);
	const HE::UUID solidId = proj.cm.loadAsset(solidMat);
	REQUIRE(bare != HE::UUID{});
	REQUIRE(bush != HE::UUID{});
	REQUIRE(leafId != HE::UUID{});
	REQUIRE(solidId != HE::UUID{});

	auto lastTrisOf = [&](const std::string& label, HE::UUID mesh, HE::UUID layerMaterial) {
		INFO("run: " << label);
		HorizonWorld world;
		const Entity land = world.createEntity("Land");
		FoliageComponent fol;
		fol.layers[0].meshAssetId     = mesh;
		fol.layers[0].materialAssetId = layerMaterial;
		world.registry().emplace<FoliageComponent>(land, fol);
		ContextBits bits;
		AppContext ctx = bits.make();
		ctx.contentManager = &proj.cm;
		ctx.world          = &world;
		AutoLodDialog::Request req;
		req.kind   = AutoLodDialog::Kind::FoliageLayer;
		req.entity = land;
		AutoLodDialog::Options opt;
		opt.levels  = 2;
		opt.foliage = true;
		const AutoLodDialog::Outcome r = AutoLodDialog::run(ctx, req, opt);
		INFO(label << ": " << r.message);
		return r.ok ? r.lastTris : 0u;      // 0: refused, nothing to reduce
	};

	const uint32_t noMask    = lastTrisOf("noMask", bare, HE::UUID{});        // nothing names a cut-out
	const uint32_t fallback  = lastTrisOf("fallback", bush, HE::UUID{});      // the mesh's own material
	const uint32_t overridden = lastTrisOf("overridden", bare, leafId);       // the same material, on the layer
	const uint32_t replaced  = lastTrisOf("replaced", bush, solidId);         // the layer's material replaces the mesh's
	INFO("noMask " << noMask << " fallback " << fallback << " overridden " << overridden << " replaced " << replaced);
	REQUIRE(noMask > 0u);               // without a cut-out the cards reduce
	CHECK(fallback != noMask);          // the mask reached the simplifier
	CHECK(fallback == overridden);      // ... and is the one an override with that material would give
	CHECK(replaced == noMask);          // an override is not second-guessed by the mesh's material
}

TEST_CASE("Generate LODs: a mesh entity gets a LODComponent whose level 0 is the mesh itself")
{
	Project proj("he_test_auto_lod_ui_entity");
	HorizonWorld world;
	const Entity e = world.createEntity("Ball");
	MeshComponent mc;
	mc.meshAssetId = proj.ball;
	world.registry().emplace<MeshComponent>(e, mc);
	if (auto* tc = world.registry().try_get<TransformComponent>(e)) tc->scale = glm::vec3(2.0f);

	ContextBits bits;
	AppContext ctx = bits.make();
	ctx.contentManager = &proj.cm;
	ctx.world          = &world;

	AutoLodDialog::Request req;
	req.kind   = AutoLodDialog::Kind::Entity;
	req.entity = e;
	const AutoLodDialog::Outcome r = AutoLodDialog::run(ctx, req, AutoLodDialog::Options{});
	INFO(r.message);
	REQUIRE(r.ok);
	const LODComponent& lod = world.registry().get<LODComponent>(e);
	REQUIRE(lod.levels.size() == r.levels + 1);
	CHECK(lod.levels[0].meshId == proj.ball);
	for (size_t i = 1; i < lod.levels.size(); ++i)
		CHECK(lod.levels[i].maxDistance > lod.levels[i - 1].maxDistance);

	// A second run overwrites the files in place: the ids the component holds survive.
	const HE::UUID first = lod.levels[1].meshId;
	REQUIRE(AutoLodDialog::run(ctx, req, AutoLodDialog::Options{}).ok);
	CHECK(world.registry().get<LODComponent>(e).levels[1].meshId == first);
}

TEST_CASE("Generate LODs: engine meshes, generated levels and missing meshes are refused with a reason")
{
	Project proj("he_test_auto_lod_ui_refuse");
	HorizonWorld world;
	const Entity land = world.createEntity("Land");
	world.registry().emplace<FoliageComponent>(land, FoliageComponent{});   // layer 0 has no mesh

	ContextBits bits;
	AppContext ctx = bits.make();
	ctx.contentManager = &proj.cm;
	ctx.world          = &world;

	AutoLodDialog::Request req;
	req.kind   = AutoLodDialog::Kind::FoliageLayer;
	req.entity = land;
	AutoLodDialog::Outcome r = AutoLodDialog::run(ctx, req, AutoLodDialog::Options{});
	CHECK_FALSE(r.ok);
	CHECK_FALSE(r.message.empty());

	// A generated level is not a source for another chain.
	REQUIRE(AutoLodDialog::run(ctx, AutoLodDialog::Request{ AutoLodDialog::Kind::MeshAsset, proj.ball }, AutoLodDialog::Options{}).ok);
	const HE::UUID lod1 = proj.cm.loadAsset("Meshes/Ball_LOD1.hasset");
	REQUIRE_FALSE(lod1 == HE::UUID{});
	r = AutoLodDialog::run(ctx, AutoLodDialog::Request{ AutoLodDialog::Kind::MeshAsset, lod1 }, AutoLodDialog::Options{});
	CHECK_FALSE(r.ok);

	// No content manager at all.
	ctx.contentManager = nullptr;
	CHECK_FALSE(AutoLodDialog::run(ctx, req, AutoLodDialog::Options{}).ok);
}
