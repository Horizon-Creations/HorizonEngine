#include "AutoLodDialog.h"
#include "EditorApplication.h"   // AppContext
#include "EditorHelp.h"          // the "Generate LODs" scope for the rows
#include "EditorWidgets.h"       // Row, buttons, pinDialogToEditorWindow
#include "NotificationStore.h"   // the result line

#include "Impostor.h"

#include <HorizonScene/AutoLodApply.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/LODComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>
#include <Diagnostics/Logger.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace AutoLodDialog
{

namespace
{
	Request s_request;
	bool    s_wantOpen = false;

	bool startsWith(const std::string& s, const char* prefix)
	{
		return s.rfind(prefix, 0) == 0;
	}

	// The alpha channel of a material's base-colour texture, the mask the foliage
	// simplifier measures leaf area against. Empty when the material has none or
	// the texture is not plain RGBA8 (a packed project's compressed textures).
	struct AlphaMask
	{
		std::vector<uint8_t> data;
		int width = 0, height = 0;
	};

	AlphaMask alphaMaskOf(ContentManager& cm, HE::UUID materialId, const std::string& materialPath)
	{
		AlphaMask out;
		HE::UUID matId = materialId;
		if (matId == HE::UUID{} && !materialPath.empty()) matId = cm.loadAsset(materialPath);
		if (matId == HE::UUID{}) return out;
		const MaterialAsset* mat = cm.getMaterial(matId);
		if (!mat) return out;
		HE::UUID texId;
		std::string texPath;
		if (!mat->textureIds.empty() && !(mat->textureIds[0] == HE::UUID{})) texId = mat->textureIds[0];
		else if (!mat->texturePaths.empty()) texPath = mat->texturePaths[0];
		// loadAsset invalidates `mat` (asset pointers die with the next load).
		if (texId == HE::UUID{} && !texPath.empty()) texId = cm.loadAsset(texPath);
		if (texId == HE::UUID{}) return out;
		const TextureAsset* tex = cm.getTexture(texId);
		if (!tex || tex->format != TextureFormat::RGBA8 || tex->channels != 4 || tex->width == 0 || tex->height == 0)
			return out;
		const size_t texels = static_cast<size_t>(tex->width) * tex->height;
		if (tex->data.size() < texels * 4) return out;
		out.width  = static_cast<int>(tex->width);
		out.height = static_cast<int>(tex->height);
		out.data.resize(texels);
		for (size_t i = 0; i < texels; ++i) out.data[i] = tex->data[i * 4 + 3];
		// A texture that is opaque everywhere says nothing about leaf area.
		if (std::all_of(out.data.begin(), out.data.end(), [](uint8_t a) { return a == 255; }))
			out = {};
		return out;
	}

	// One material slot of the plant for the impostor bake: the base-colour texture's
	// pixels (copied: the next load invalidates the asset), the material's tint and
	// whether it cuts out. A material the bake cannot read as RGBA8 (a packed project's
	// compressed texture) comes back flat, which is said in `note`.
	struct SlotSource
	{
		std::vector<uint8_t> pixels;
		Impostor::SectionSource src;
	};

	SlotSource slotSourceOf(ContentManager& cm, HE::UUID materialId, const std::string& materialPath, std::string& note)
	{
		SlotSource out;
		HE::UUID matId = materialId;
		if (matId == HE::UUID{} && !materialPath.empty()) matId = cm.loadAsset(materialPath);
		const MaterialAsset* mat = matId == HE::UUID{} ? nullptr : cm.getMaterial(matId);
		if (!mat)
		{
			out.src.alphaTest = false;
			out.src.tint[0] = 0.35f; out.src.tint[1] = 0.45f; out.src.tint[2] = 0.2f;
			note = "A material was not found, so its part of the plant is flat green.";
			return out;
		}
		for (int k = 0; k < 3; ++k) out.src.tint[k] = mat->baseColor[k];
		out.src.alphaTest = mat->blendMode != 0;   // Opaque draws without its alpha
		HE::UUID texId;
		std::string texPath;
		if (!mat->textureIds.empty() && !(mat->textureIds[0] == HE::UUID{})) texId = mat->textureIds[0];
		else if (!mat->texturePaths.empty()) texPath = mat->texturePaths[0];
		if (texId == HE::UUID{} && texPath.empty()) return out;   // no texture: the tint is the colour
		if (texId == HE::UUID{}) texId = cm.loadAsset(texPath);
		const TextureAsset* tex = texId == HE::UUID{} ? nullptr : cm.getTexture(texId);
		if (!tex || tex->format != TextureFormat::RGBA8 || tex->channels != 4 || tex->width == 0 || tex->height == 0
		    || tex->data.size() < static_cast<size_t>(tex->width) * tex->height * 4)
		{
			note = "A texture could not be read as plain RGBA8, so its part of the plant is flat.";
			out.src.alphaTest = false;
			return out;
		}
		out.pixels.assign(tex->data.begin(), tex->data.begin() + static_cast<size_t>(tex->width) * tex->height * 4);
		out.src.width  = static_cast<int>(tex->width);
		out.src.height = static_cast<int>(tex->height);
		out.src.srgb   = tex->srgb;
		return out;
	}

	Outcome fail(std::string message)
	{
		Outcome o;
		o.message = std::move(message);
		return o;
	}

	// The bake and the files: a picture of `src` seen from two sides, the card sized to
	// it, and the material. `layer` (may be null) says which material the plant is drawn
	// with: a layer material replaces the mesh's own on every slot. The written assets
	// are loaded so their ids are the ones a scene will name.
	Outcome bakeImpostorFiles(AppContext& /*ctx*/, ContentManager& cm, const StaticMeshAsset& src,
	                          const FoliageLayer* layer, const Options& o, HE::UUID& cardOut, HE::UUID& materialOut)
	{
		if (src.path.empty()) return fail("The mesh has no asset path to put the impostor next to.");
		if (src.cooked) return fail("A packed mesh cannot be baked.");
		const size_t slotCount = std::max<size_t>(src.sections.size(), 1);
		std::vector<SlotSource> slots;
		std::string note;
		for (size_t i = 0; i < slotCount; ++i)
		{
			HE::UUID    matId   = layer ? layer->materialAssetId : HE::UUID{};
			std::string matPath;
			if (matId == HE::UUID{})
			{
				if (!src.sections.empty()) { matId = src.sections[i].materialId; matPath = src.sections[i].materialPath; }
				if (matId == HE::UUID{} && matPath.empty()) { matId = src.materialId; matPath = src.materialPath; }
			}
			slots.push_back(slotSourceOf(cm, matId, matPath, note));
		}
		std::vector<Impostor::SectionSource> sources;
		for (const SlotSource& s : slots) sources.push_back(s.src);   // pixel buffers do not move: the vectors own them
		for (size_t i = 0; i < slots.size(); ++i)
			if (!slots[i].pixels.empty()) sources[i].rgba = slots[i].pixels.data();

		Impostor::Settings bs;
		bs.size = Options::kSizeFor(o.impostorSize);
		const Impostor::Baked baked = Impostor::bake(src, sources, bs);
		if (!baked.ok()) return fail(baked.error);
		const Impostor::Result written = Impostor::write(baked, src.path, cm.contentRoot());
		if (!written.ok()) return fail(written.error);

		cardOut     = cm.loadAsset(written.meshPath);
		materialOut = cm.loadAsset(written.materialPath);
		cm.loadAsset(written.texturePath);
		if (cardOut == HE::UUID{})     cardOut     = written.mesh;
		if (materialOut == HE::UUID{}) materialOut = written.material;
		if (cardOut == HE::UUID{} || materialOut == HE::UUID{}) return fail("The impostor was written but could not be loaded.");

		Outcome out;
		out.ok = true;
		char buf[200];
		std::snprintf(buf, sizeof(buf), "Baked an impostor (%d x %d texels, %.0f %% / %.0f %% of the two views covered) as %s.",
		              baked.width, baked.height, baked.coverage[0] * 100.0f, baked.coverage[1] * 100.0f, written.materialPath.c_str());
		out.message = buf;
		if (!note.empty()) out.message += " " + note;
		return out;
	}
}

Options& options()
{
	static Options o;
	return o;
}

AutoLod::Settings toSettings(const Options& o)
{
	AutoLod::Settings s;
	s.ratios  = AutoLod::ratiosFor(o.levels, o.step, o.lowest / 100.0f);
	s.foliage = o.foliage;
	return s;
}

bool drawOptions(Options& o, Kind kind)
{
	bool changed = false;
#ifdef HE_IMGUI_ENABLED
	HE::Ed::Help::Scope helpScope("Generate LODs");
	changed |= EditorWidgets::Row::sliderInt("Levels##autolod", &o.levels, 1, 6);
	changed |= EditorWidgets::Row::sliderFloat("Reduction Per Level##autolod", &o.step, 0.2f, 0.9f, "keeps %.2f");
	changed |= EditorWidgets::Row::sliderFloat("Lowest Level##autolod", &o.lowest, 1.0f, 50.0f, "%.0f %% of the triangles");
	if (kind != Kind::MeshAsset)
		changed |= EditorWidgets::Row::sliderFloat("Pixel Error##autolod", &o.tolerancePx, 0.5f, 8.0f, "%.1f px");
	changed |= EditorWidgets::checkbox("Foliage Mode##autolod", &o.foliage);
	if (kind == Kind::FoliageLayer)
	{
		changed |= EditorWidgets::checkbox("End on Cross Card##autolod", &o.crossCard);
		changed |= EditorWidgets::checkbox("Bake Impostor##autolod", &o.bakeImpostor);
		if (o.bakeImpostor)
			changed |= EditorWidgets::Row::comboZ("Impostor Size##autolod", &o.impostorSize, "256 texels\0" "512 texels\0" "1024 texels\0");
	}

	// What it will make, in the words of the request: 100 / 50 / 25 / 10 %.
	std::string line = "100";
	for (float r : AutoLod::ratiosFor(o.levels, o.step, o.lowest / 100.0f))
	{
		char buf[24];
		std::snprintf(buf, sizeof(buf), " / %.0f", r * 100.0f);
		line += buf;
	}
	ImGui::TextDisabled("%s %% of the triangles", line.c_str());
#else
	(void)o; (void)kind;
#endif
	return changed;
}

Outcome run(AppContext& ctx, const Request& req, const Options& o)
{
	if (!ctx.contentManager) return fail("No project is open.");
	ContentManager& cm = *ctx.contentManager;

	// ── LOD0 ─────────────────────────────────────────────────────────────────
	HE::UUID lod0 = req.mesh;
	HorizonWorld* world = ctx.world;
	FoliageLayer* layer = nullptr;
	if (req.kind == Kind::FoliageLayer)
	{
		if (!world || !world->registry().valid(req.entity)) return fail("The landscape is gone.");
		auto* fol = world->registry().try_get<FoliageComponent>(req.entity);
		if (!fol || req.layer < 0 || static_cast<size_t>(req.layer) >= fol->layers.size())
			return fail("The foliage layer is gone.");
		layer = &fol->layers[static_cast<size_t>(req.layer)];
		lod0  = layer->meshAssetId;
	}
	else if (req.kind == Kind::Entity)
	{
		if (!world || !world->registry().valid(req.entity)) return fail("The entity is gone.");
		if (const auto* existing = world->registry().try_get<LODComponent>(req.entity);
		    existing && !existing->levels.empty() && !(existing->levels[0].meshId == HE::UUID{}))
			lod0 = existing->levels[0].meshId;
		else if (const auto* mc = world->registry().try_get<MeshComponent>(req.entity))
			lod0 = mc->meshAssetId;
	}
	if (lod0 == HE::UUID{}) return fail("There is no mesh to make levels from.");

	const std::string lod0Path = cm.pathForId(lod0);
	if (lod0Path.empty()) return fail("The mesh is not a file in the project.");
	if (startsWith(lod0Path, "Engine/"))
		return fail("Engine meshes already come with their own levels. Copy the mesh into the project first.");
	if (AutoLod::isLodSidecarPath(lod0Path))
		return fail("This mesh is itself a generated level; generate from the original.");

	if (!cm.getStaticMesh(lod0)) cm.ensureResident(lod0);
	const StaticMeshAsset* loaded = cm.getStaticMesh(lod0);
	if (!loaded) return fail("The mesh could not be loaded.");
	StaticMeshAsset src = *loaded;       // a copy: the next load invalidates `loaded`
	if (src.path.empty()) src.path = lod0Path;

	// ── The simplification ───────────────────────────────────────────────────
	AutoLod::Settings settings = toSettings(o);
	AlphaMask mask;
	if (settings.foliage)
	{
		mask = alphaMaskOf(cm, layer ? layer->materialAssetId : HE::UUID{}, layer ? std::string{} : src.materialPath);
		if (mask.data.empty() && layer == nullptr) mask = alphaMaskOf(cm, src.materialId, src.materialPath);
		if (!mask.data.empty())
		{
			settings.alphaMask   = mask.data.data();
			settings.alphaWidth  = mask.width;
			settings.alphaHeight = mask.height;
		}
	}
	const AutoLod::Chain chain = AutoLod::generate(src, cm.contentRoot(), settings);
	if (!chain.error.empty()) return fail(chain.error);

	// Make the new files known (a regenerated one is picked up by the hot reload).
	std::vector<HE::AutoLodApply::Level> levels;
	for (size_t i = 1; i < chain.levels.size(); ++i)
	{
		HE::UUID id = cm.loadAsset(chain.levels[i].path);
		if (id == HE::UUID{}) id = chain.levels[i].id;
		levels.push_back({ id, chain.levels[i].worldError });
	}

	Outcome out;
	out.ok       = true;
	out.levels   = levels.size();
	out.lod0Tris = chain.levels.front().triangles;
	out.lastTris = chain.levels.back().triangles;

	// ── The consumer ─────────────────────────────────────────────────────────
	HE::LodScreenMetrics screen;
	screen.tolerancePx = o.tolerancePx;
	if (layer)
	{
		if (ctx.undoSys) ctx.undoSys->snapshotNow("Generate LODs");
		HE::AutoLodApply::FoliageOptions fo;
		fo.screen        = screen;
		fo.instanceScale = 0.5f * (layer->minScale + layer->maxScale);
		fo.crossCard     = o.crossCard || o.bakeImpostor;
		std::string impostorNote;
		if (o.bakeImpostor)
		{
			const Outcome baked = bakeImpostorFiles(ctx, cm, src, layer, o, fo.impostorMesh, fo.impostorMaterial);
			if (!baked.ok) impostorNote = " Impostor not baked: " + baked.message;
			else impostorNote = " " + baked.message;
		}
		const size_t n = HE::AutoLodApply::fillFoliageLayer(*layer, levels, fo);
		out.message = "Generated " + std::to_string(levels.size()) + " level(s) for " + lod0Path + " ("
		            + std::to_string(out.lod0Tris) + " to " + std::to_string(out.lastTris) + " triangles); the layer now has "
		            + std::to_string(n) + " detail level(s)." + impostorNote;
	}
	else if (req.kind == Kind::Entity)
	{
		if (ctx.undoSys) ctx.undoSys->snapshotNow("Generate LODs");
		auto& reg = world->registry();
		LODComponent& lc = reg.get_or_emplace<LODComponent>(req.entity);
		float scale = 1.0f;
		if (const auto* tc = reg.try_get<TransformComponent>(req.entity))
			scale = std::max({ std::abs(tc->scale.x), std::abs(tc->scale.y), std::abs(tc->scale.z) });
		HE::AutoLodApply::fillLodComponent(lc, lod0, levels, screen, scale);
		out.message = "Generated " + std::to_string(levels.size()) + " level(s) for " + lod0Path + " ("
		            + std::to_string(out.lod0Tris) + " to " + std::to_string(out.lastTris) + " triangles).";
	}
	else
	{
		out.message = "Generated " + std::to_string(levels.size()) + " level(s) next to " + lod0Path + " ("
		            + std::to_string(out.lod0Tris) + " to " + std::to_string(out.lastTris) + " triangles).";
	}
	if (settings.foliage && mask.data.empty())
		out.message += " No alpha mask was found, so leaf area was not measured.";
	return out;
}

Outcome bakeImpostor(AppContext& ctx, const Request& req, const Options& o)
{
	if (!ctx.contentManager) return fail("No project is open.");
	if (req.kind != Kind::FoliageLayer) return fail("An impostor is baked for a foliage layer.");
	HorizonWorld* world = ctx.world;
	if (!world || !world->registry().valid(req.entity)) return fail("The landscape is gone.");
	auto* fol = world->registry().try_get<FoliageComponent>(req.entity);
	if (!fol || req.layer < 0 || static_cast<size_t>(req.layer) >= fol->layers.size())
		return fail("The foliage layer is gone.");
	FoliageLayer& layer = fol->layers[static_cast<size_t>(req.layer)];
	ContentManager& cm = *ctx.contentManager;
	if (layer.meshAssetId == HE::UUID{}) return fail("The layer has no mesh.");
	const std::string path = cm.pathForId(layer.meshAssetId);
	if (path.empty()) return fail("The mesh is not a file in the project.");
	if (startsWith(path, "Engine/"))
		return fail("Engine meshes cannot be baked here. Copy the mesh into the project first.");
	if (!cm.getStaticMesh(layer.meshAssetId)) cm.ensureResident(layer.meshAssetId);
	const StaticMeshAsset* loaded = cm.getStaticMesh(layer.meshAssetId);
	if (!loaded) return fail("The mesh could not be loaded.");
	StaticMeshAsset src = *loaded;       // a copy: the next load invalidates `loaded`
	if (src.path.empty()) src.path = path;

	HE::UUID card, material;
	Outcome out = bakeImpostorFiles(ctx, cm, src, &layer, o, card, material);
	if (!out.ok) return out;
	if (ctx.undoSys) ctx.undoSys->snapshotNow("Bake Impostor");
	if (!HE::AutoLodApply::applyImpostorLevel(layer, card, material))
	{
		out.ok = false;
		out.message += " The layer has no room for another level (or it would start past the Draw Distance), so it was not added.";
		return out;
	}
	out.message += " The layer now has " + std::to_string(layer.lods.size()) + " detail level(s); the impostor starts at "
	            + std::to_string(static_cast<int>(layer.lods.back().distance)) + " m.";
	return out;
}

void open(const Request& req)
{
	s_request  = req;
	s_wantOpen = true;
}

void draw(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	constexpr const char* kTitle = "Generate LODs##autolod";
	if (s_wantOpen)
	{
		// Never over another root-level modal: OpenPopup there would replace it.
		if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
			return;
		s_wantOpen = false;
		ImGui::OpenPopup(kTitle);
	}

	constexpr float kDialogWidth = 420.0f;
	ImGui::SetNextWindowSize(ImVec2(kDialogWidth, 0.0f), ImGuiCond_Appearing);
	EditorWidgets::pinDialogToEditorWindow();
	if (!ImGui::BeginPopupModal(kTitle, nullptr,
	                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
	                            ImGuiWindowFlags_NoSavedSettings))
		return;

	HE::Ed::Help::Scope helpScope("Generate LODs");
	Options& o = options();
	{
		EditorWidgets::WrapText wrap(kDialogWidth - ImGui::GetStyle().WindowPadding.x);
		const std::string path = ctx.contentManager ? ctx.contentManager->pathForId(s_request.mesh) : std::string{};
		if (s_request.kind == Kind::MeshAsset && !path.empty())
			ImGui::Text("%s", path.c_str());
		ImGui::TextDisabled(
			"Makes simpler copies of the mesh next to it (Name_LOD1, Name_LOD2 ...). "
			"Running it again replaces them and keeps their references.");
		ImGui::Spacing();
	}
	drawOptions(o, s_request.kind);
	ImGui::Spacing();

	bool close = false;
	if (EditorWidgets::primaryButton("Generate##autolod", ImVec2(120, 0)))
	{
		const Outcome r = run(ctx, s_request, o);
		HE_LOG_INFO(Editor, "%s", ("Generate LODs: " + r.message).c_str());
		HE::Ed::notify(r.ok ? HE::Ed::NoteLevel::Info : HE::Ed::NoteLevel::Important,
		               r.ok ? "LODs generated" : "LODs not generated", r.message);
		close = true;
	}
	EditorWidgets::helpForLabel("Generate##autolod");
	ImGui::SameLine();
	if (EditorWidgets::button("Cancel##autolod", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
		close = true;
	EditorWidgets::helpForLabel("Cancel##autolod");
	if (close) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
#else
	(void)ctx;
#endif
}

} // namespace AutoLodDialog
