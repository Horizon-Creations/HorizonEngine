#include "AutoLodDialog.h"
#include "EditorApplication.h"   // AppContext
#include "EditorHelp.h"          // the "Generate LODs" scope for the rows
#include "EditorWidgets.h"       // Row, buttons, pinDialogToEditorWindow
#include "NotificationStore.h"   // the result line

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

	Outcome fail(std::string message)
	{
		Outcome o;
		o.message = std::move(message);
		return o;
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
		changed |= EditorWidgets::checkbox("End on Cross Card##autolod", &o.crossCard);

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
		fo.crossCard     = o.crossCard;
		const size_t n = HE::AutoLodApply::fillFoliageLayer(*layer, levels, fo);
		out.message = "Generated " + std::to_string(levels.size()) + " level(s) for " + lod0Path + " ("
		            + std::to_string(out.lod0Tris) + " to " + std::to_string(out.lastTris) + " triangles); the layer now has "
		            + std::to_string(n) + " detail level(s).";
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
