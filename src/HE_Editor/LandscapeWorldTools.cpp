#include "LandscapeWorldTools.h"
#include "EditorApplication.h"   // AppContext
#include "EditorHelp.h"
#include "NotificationStore.h"
#include <HorizonScene/Components/LandscapeWorldComponent.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/LandscapePackBuild.h>
#include <HorizonScene/TransformHierarchy.h>
#include <Diagnostics/Logger.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>

#ifdef HE_IMGUI_ENABLED
#include <SDL3/SDL_dialog.h>
#include <imgui.h>
#include "EditorWidgets.h"
#endif

namespace fs = std::filesystem;
#ifdef HE_IMGUI_ENABLED
namespace Row = EditorWidgets::Row;
#endif

namespace LandscapeWorldTools
{
namespace
{
enum class Kind { Generate, Migrate, Import };

struct State
{
	HE::LandscapeBuildJob job;
	Entity       target = entt::null;
	std::string  outPath;
	std::string  previousPack;     // what the component named before the build: the same path means the runtime has to be told to open it again
	Kind         kind = Kind::Generate;
	// The runtime opens a pack when the component's path CHANGES. A rebuilt pack under the same path is made known by switching the component
	// off for one frame (the runtime lets go of the old file, and Windows could not replace it otherwise), then on again.
	Entity       reenable = entt::null;
	int          reenableFrames = 0;
	std::string  status;
	bool         statusError = false;

	// The section's remembered inputs.
	GenerateSettings gen;
	ImportSettings   imp;
	std::string      outField;
	// A file or folder chosen in a native dialog arrives on another thread: parked here until the section draws again.
	std::mutex   dialogMutex;
	std::string  dialogPath;
	int          dialogPurpose = 0;   // 0 none, 1 pack file, 2 output file, 3 import file, 4 import folder
	std::atomic<bool> dialogReady{ false };
};

State& st()
{
	static State s;
	return s;
}

std::string absolutePath(const std::string& p)
{
	std::error_code ec;
	const fs::path a = fs::absolute(fs::path(p), ec);
	return ec ? p : a.lexically_normal().string();
}

bool targetValid(HorizonWorld& world, Entity e)
{
	return e != entt::null && world.registry().valid(e) && world.registry().all_of<LandscapeWorldComponent>(e);
}

std::string begin(HorizonWorld& world, Entity target, const std::string& outPath, Kind kind, const char* label, HE::LandscapeBuildJob::Work work)
{
	State& s = st();
	if (s.job.running()) return "A build is already running.";
	if (outPath.empty()) return "Choose the pack file to write first.";
	if (!targetValid(world, target)) return "Select the entity with the Tile Landscape component.";
	s.target = target;
	s.outPath = absolutePath(outPath);
	s.previousPack = world.registry().get<LandscapeWorldComponent>(target).pack;
	s.kind = kind;
	s.status = std::string(label) + "...";
	s.statusError = false;
	if (!s.job.start(label, std::move(work))) return "A build is already running.";
	return {};
}
} // namespace

Entity migrationSource(HorizonWorld& world, Entity nearEntity)
{
	auto& reg = world.registry();
	if (nearEntity != entt::null && reg.valid(nearEntity) && reg.all_of<TerrainComponent>(nearEntity)) return nearEntity;
	for (auto e : reg.view<TerrainComponent>()) return e;
	return entt::null;
}

std::string startGenerate(HorizonWorld& world, Entity target, const std::string& outPath, const GenerateSettings& g)
{
	if (g.tilesX < 1 || g.tilesZ < 1 || g.tilesX > 4096 || g.tilesZ > 4096) return "The world needs 1 to 4096 tiles a side.";
	if (!(g.heightRangeM > 0.0f)) return "The height range must be positive.";
	HE::ProceduralLandscapeParams p;
	p.tilesX = g.tilesX;
	p.tilesZ = g.tilesZ;
	p.seed = g.seed;
	p.octaves = std::clamp(g.octaves, 1, 12);
	p.heightScale = g.heightRangeM;
	const std::string out = absolutePath(outPath);
	return begin(world, target, outPath, Kind::Generate, "Generating the world", [p, out](const HE::LandscapeBuildOptions& o) {
		return HE::buildProceduralPack(out, p, o);
	});
}

std::string startMigrate(HorizonWorld& world, Entity target, const std::string& outPath)
{
	const Entity terrain = migrationSource(world, target);
	if (terrain == entt::null) return "There is no Terrain in this scene to migrate.";
	// The copy the thread works on: the scene goes on being edited meanwhile.
	TerrainComponent copy = world.registry().get<TerrainComponent>(terrain);
	HE::LandscapeMigrateParams mp;
	mp.yOffsetM = HE::worldPositionOf(world, terrain).y;
	const std::string out = absolutePath(outPath);
	return begin(world, target, outPath, Kind::Migrate, "Migrating the terrain", [copy = std::move(copy), mp, out](const HE::LandscapeBuildOptions& o) {
		return HE::migrateTerrainToPack(copy, out, mp, o);
	});
}

std::string startImport(HorizonWorld& world, Entity target, const std::string& outPath, const std::string& fileOrFolder, const ImportSettings& s)
{
	if (fileOrFolder.empty()) return "Choose a height map first.";
	if (!(s.metersPerPixel > 0.0f)) return "Metres per pixel must be positive.";
	if (!(s.heightRangeM > 0.0f)) return "The height range must be positive.";
	std::error_code ec;
	if (!fs::exists(fileOrFolder, ec)) return "\"" + fileOrFolder + "\" does not exist.";
	HE::LandscapeHeightmapFiles files = fs::is_directory(fileOrFolder, ec) ? HE::findHeightmapTileFiles(fileOrFolder) : HE::singleHeightmapFile(fileOrFolder);
	if (!files.error.empty()) return files.error;
	HE::LandscapeHeightmapImportParams ip;
	ip.metersPerPixel = s.metersPerPixel;
	ip.heightMinM = s.heightMinM;
	ip.heightScaleM = s.heightRangeM;
	ip.flipY = s.flipFileRows;
	ip.autoWeights = s.paintFromShape;
	const std::string out = absolutePath(outPath);
	return begin(world, target, outPath, Kind::Import, "Importing the height map", [files = std::move(files), ip, out](const HE::LandscapeBuildOptions& o) {
		return HE::importHeightmapToPack(files, out, ip, o);
	});
}

bool buildRunning() { return st().job.running(); }
void cancelBuild() { st().job.cancel(); }
const std::string& lastStatus() { return st().status; }
bool lastStatusIsError() { return st().statusError; }

void resetState()
{
	State& s = st();
	s.job.cancel();
	for (int i = 0; i < 4000 && s.job.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
	HE::LandscapeBuildResult dropped;
	s.job.poll(dropped);
	s.target = s.reenable = entt::null;
	s.reenableFrames = 0;
	s.status.clear();
	s.statusError = false;
	s.gen = {};
	s.imp = {};
	s.outField.clear();
	s.dialogReady = false;
}

void pump(HorizonWorld& world)
{
	State& s = st();
	auto& reg = world.registry();
	if (s.reenable != entt::null)
	{
		if (--s.reenableFrames <= 0)
		{
			if (reg.valid(s.reenable))
				if (auto* lw = reg.try_get<LandscapeWorldComponent>(s.reenable)) lw->enabled = true;
			s.reenable = entt::null;
		}
	}
	HE::LandscapeBuildResult r;
	if (!s.job.poll(r)) return;
	s.status = r.summary();
	s.statusError = !r.ok && !r.cancelled;
	if (r.ok)
	{
		HE_LOG_INFO(Editor, "Tile landscape pack written: %s (%s)", s.outPath.c_str(), r.summary().c_str());
		if (targetValid(world, s.target))
		{
			auto& lw = reg.get<LandscapeWorldComponent>(s.target);
			const bool samePath = lw.pack == s.outPath;
			lw.pack = s.outPath;
			// The heights of the new pack: the runtime reads them from the pack's header, the component keeps them as the fallback.
			lw.heightMinM = r.heightMinM;
			lw.heightScaleM = r.heightScaleM;
			if (samePath && lw.enabled)
			{
				lw.enabled = false;
				s.reenable = s.target;
				s.reenableFrames = 2;
			}
		}
		HE::Ed::notify(HE::Ed::NoteLevel::Info, "Tile landscape written", r.summary());
	}
	else if (!r.cancelled)
	{
		HE_LOG_WARN(Editor, "Tile landscape build failed: %s", r.error.c_str());
		HE::Ed::notify(HE::Ed::NoteLevel::Important, "Tile landscape build failed.", r.error);
	}
}

#ifdef HE_IMGUI_ENABLED
namespace
{
void openDialog(AppContext& ctx, int purpose)
{
	State& s = st();
	static const SDL_DialogFileFilter kPackFilter[] = { { "Landscape packs", "hlp" }, { "All files", "*" } };
	static const SDL_DialogFileFilter kPictureFilter[] = { { "Height maps", "png;pgm;r16;raw;jpg;jpeg;bmp;tga" }, { "All files", "*" } };
	// The callback runs on another thread (or at once, on some platforms): it only parks the answer.
	const auto cb = [](void* user, const char* const* list, int /*filter*/) {
		State& state = st();
		const int purpose = static_cast<int>(reinterpret_cast<intptr_t>(user));
		if (list && list[0])
		{
			std::lock_guard<std::mutex> lk(state.dialogMutex);
			state.dialogPath = list[0];
			state.dialogPurpose = purpose;
			state.dialogReady.store(true, std::memory_order_release);
		}
	};
	void* user = reinterpret_cast<void*>(static_cast<intptr_t>(purpose));
	SDL_Window* win = ctx.window ? ctx.window->GetNativeWindow() : nullptr;
	switch (purpose)
	{
		case 1: SDL_ShowOpenFileDialog(cb, user, win, kPackFilter, 2, nullptr, false); break;
		case 2: SDL_ShowSaveFileDialog(cb, user, win, kPackFilter, 2, nullptr); break;
		case 3: SDL_ShowOpenFileDialog(cb, user, win, kPictureFilter, 2, nullptr, false); break;
		case 4: SDL_ShowOpenFolderDialog(cb, user, win, nullptr, false); break;
		default: break;
	}
	(void)s;
}
} // namespace

void drawSection(AppContext& ctx, Entity entity, const std::function<void()>& afterEdit)
{
	if (!ctx.world) return;
	const auto edit = [&] { if (afterEdit) afterEdit(); };
	auto& reg = ctx.world->registry();
	auto* lw = reg.try_get<LandscapeWorldComponent>(entity);
	if (!lw) return;
	State& s = st();
	HE::Ed::Help::Scope helpScope("Tile Landscape");
	EditorWidgets::WrapText wrap;

	// A file or folder chosen since the last draw.
	if (s.dialogReady.exchange(false, std::memory_order_acquire))
	{
		std::string path;
		int purpose = 0;
		{
			std::lock_guard<std::mutex> lk(s.dialogMutex);
			path = s.dialogPath;
			purpose = s.dialogPurpose;
		}
		if (purpose == 1) lw->pack = path;
		else if (purpose == 2) s.outField = path;
		else if (purpose == 3 || purpose == 4)
		{
			const std::string why = startImport(*ctx.world, entity, s.outField.empty() ? lw->pack : s.outField, path, s.imp);
			if (!why.empty()) { s.status = why; s.statusError = true; }
		}
	}

	EditorWidgets::checkbox("Enabled##tlw", &lw->enabled); edit();
	Row::inputText("Pack File##tlw", &lw->pack); edit();
	EditorWidgets::helpForKey("Tile Landscape/Pack File");
	if (EditorWidgets::button("Browse...##tlwpack")) openDialog(ctx, 1);
	Row::dragFloat("Load Radius##tlw", &lw->loadRadius, 100.0f, 500.0f, 50000.0f, "%.0f m"); edit();
	int budget = static_cast<int>(lw->budgetMB);
	if (Row::dragInt("Memory Budget##tlw", &budget, 8.0f, 16, 16384)) lw->budgetMB = static_cast<uint32_t>(std::max(16, budget));
	edit();
	Row::dragFloat("Height Min##tlw", &lw->heightMinM, 1.0f, -20000.0f, 20000.0f, "%.1f m"); edit();
	Row::dragFloat("Height Range##tlw", &lw->heightScaleM, 1.0f, 1.0f, 40000.0f, "%.1f m"); edit();
	static const char* const kGroups[] = { "Flat colours", "Mountains", "Forest", "Grassland", "Desert" };
	int group = lw->textureGroup + 1;
	if (Row::combo("Texture Group##tlw", &group, kGroups, 5)) lw->textureGroup = group - 1;
	edit();
	Row::dragFloat("Texture Metres##tlw", &lw->textureMeters, 0.25f, 0.5f, 512.0f, "%.1f m"); edit();

	ImGui::SeparatorText("Make a pack");
	if (s.outField.empty()) s.outField = lw->pack;
	if (s.outField.empty() && ctx.projectManager && !ctx.projectManager->currentProject().path.empty())
		s.outField = (fs::path(ctx.projectManager->currentProject().path) / "Landscape" / "world.hlp").string();   // somewhere sensible to start
	Row::inputText("Output File##tlw", &s.outField);
	EditorWidgets::helpForKey("Tile Landscape/Output File");
	if (EditorWidgets::button("Choose Output...##tlwout")) openDialog(ctx, 2);

	const bool busy = s.job.running();
	ImGui::BeginDisabled(busy);

	ImGui::SeparatorText("Generate");
	Row::dragInt("Tiles X##tlwg", &s.gen.tilesX, 1.0f, 1, 4096);
	Row::dragInt("Tiles Z##tlwg", &s.gen.tilesZ, 1.0f, 1, 4096);
	Row::inputInt("Seed##tlwg", &s.gen.seed);
	Row::sliderInt("Octaves##tlwg", &s.gen.octaves, 1, 12);
	Row::dragFloat("Generated Height Range##tlwg", &s.gen.heightRangeM, 10.0f, 10.0f, 20000.0f, "%.0f m");
	if (EditorWidgets::button("100 km World##tlwg")) s.gen.tilesX = s.gen.tilesZ = 196;
	ImGui::SameLine();
	if (EditorWidgets::primaryButton("Generate Procedural World"))
	{
		const std::string why = startGenerate(*ctx.world, entity, s.outField, s.gen);
		if (!why.empty()) { s.status = why; s.statusError = true; }
	}
	ImGui::TextDisabled("%.1f km x %.1f km, 512 m tiles", s.gen.tilesX * 0.512, s.gen.tilesZ * 0.512);

	ImGui::SeparatorText("Migrate");
	const Entity terrain = migrationSource(*ctx.world, entity);
	if (terrain == entt::null)
		ImGui::TextDisabled("No Terrain in this scene.");
	else
	{
		const auto* name = reg.try_get<NameComponent>(terrain);
		const auto& tc = reg.get<TerrainComponent>(terrain);
		ImGui::TextDisabled("Terrain: %s (%.0f x %.0f m)", name ? name->name.c_str() : "Terrain", static_cast<double>(tc.sizeX), static_cast<double>(tc.sizeZ));
	}
	ImGui::BeginDisabled(terrain == entt::null);
	if (EditorWidgets::button("Migrate Terrain to Tiles"))
	{
		const std::string why = startMigrate(*ctx.world, entity, s.outField);
		if (!why.empty()) { s.status = why; s.statusError = true; }
	}
	ImGui::EndDisabled();

	ImGui::SeparatorText("Import");
	Row::dragFloat("Metres per Pixel##tlwi", &s.imp.metersPerPixel, 0.1f, 0.01f, 1000.0f, "%.2f m");
	Row::dragFloat("Import Height Min##tlwi", &s.imp.heightMinM, 1.0f, -20000.0f, 20000.0f, "%.1f m");
	Row::dragFloat("Import Height Range##tlwi", &s.imp.heightRangeM, 10.0f, 1.0f, 40000.0f, "%.0f m");
	EditorWidgets::checkbox("Flip File Rows##tlwi", &s.imp.flipFileRows);
	EditorWidgets::checkbox("Paint Layers from Shape##tlwi", &s.imp.paintFromShape);
	if (EditorWidgets::button("Import Height Map File...")) openDialog(ctx, 3);
	ImGui::SameLine();
	if (EditorWidgets::button("Import Tiled Height Map Folder...")) openDialog(ctx, 4);
	ImGui::EndDisabled();

	if (busy)
	{
		const size_t total = s.job.total(), done = s.job.done();
		char overlay[64];
		std::snprintf(overlay, sizeof overlay, "%zu / %zu tiles", done, total);
		ImGui::ProgressBar(total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f, ImVec2(-FLT_MIN, 0.0f), overlay);
		if (EditorWidgets::dangerButton("Cancel Build")) s.job.cancel();
	}
	if (!s.status.empty())
	{
		if (s.statusError) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s", s.status.c_str());
		else ImGui::TextDisabled("%s", s.status.c_str());
	}
}
#else
void drawSection(AppContext&, Entity, const std::function<void()>&) {}
#endif

} // namespace LandscapeWorldTools
