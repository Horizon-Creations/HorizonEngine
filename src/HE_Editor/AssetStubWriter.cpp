#include "AssetStubWriter.h"

#include <ContentManager/HAsset.h>
#include <HorizonCode/HorizonCode.h>
#include <UIWidget/UITheme.h>
#include <UIWidget/UIWidgetTree.h>
#include <Types/UUID.h>

#include <cstring>
#include <string>
#include <vector>

namespace HE::Ed
{

namespace
{

// Starter template for a freshly created script, by language. Verbatim from the
// Content Browser's create menu, which is now a caller rather than the owner.
const char* scriptStarterTemplate(HE::ScriptLanguage lang)
{
	static const char* kLua =
		"local M = {}\n\n"
		"function M.onStart(self)\nend\n\n"
		"function M.onUpdate(self, dt)\nend\n\n"
		"return M\n";
	static const char* kPy =
		"import horizon\n\n"
		"class NewScript(horizon.Behavior):\n"
		"    def on_start(self):\n        pass\n\n"
		"    def on_update(self, dt):\n        pass\n";
	return (lang == HE::ScriptLanguage::Python) ? kPy : kLua;
}

} // namespace

bool writeAssetStub(const std::string& absolutePath,
                    const std::string& contentRelativePath,
                    const std::string& displayName,
                    HE::AssetType      type,
                    const AssetStubSpec& spec)
{
	const HE::UUID assetId = HE::UUID::generate();
	HAsset::Writer w;

	std::vector<uint8_t> meta;
	HAsset::Writer::appendPOD(meta, static_cast<uint16_t>(type));
	HAsset::Writer::appendPOD(meta, assetId.hi);
	HAsset::Writer::appendPOD(meta, assetId.lo);
	HAsset::Writer::appendString(meta, displayName);
	HAsset::Writer::appendString(meta, contentRelativePath);
	w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());

	// Scripts are born with a language and a starter template. The language byte
	// (CHUNK_SLNG) is the single source of truth for routing Lua vs Python, so it
	// must be written here at birth — this stub bypasses the ContentManager save
	// path.
	if (type == HE::AssetType::Script)
	{
		const char* starter = scriptStarterTemplate(spec.scriptLanguage);
		w.addChunk(HAsset::CHUNK_SRC, starter, std::char_traits<char>::length(starter));
		const uint8_t lb = static_cast<uint8_t>(spec.scriptLanguage);
		w.addChunk(HAsset::CHUNK_SLNG, &lb, 1);
	}
	// UI widgets are born with an empty 1920×1080 tree so the widget editor has
	// valid JSON to open straight away.
	if (type == HE::AssetType::Widget)
	{
		const std::string tree = HE::uiWidgetTreeToJson(HE::UIWidgetTree{});
		w.addChunk(HAsset::CHUNK_UIWT, tree.data(), tree.size());
	}
	// A theme is born as the shipped default, not as an empty file: an author
	// edits colours, they do not invent nine roles from nothing, and an empty
	// theme would be black.
	if (type == HE::AssetType::Theme)
	{
		const std::string json = HE::uiThemeToJson(HE::uiDefaultTheme());
		w.addChunk(HAsset::CHUNK_THEM, json.data(), json.size());
	}
	if (type == HE::AssetType::HorizonCodeClass)
	{
		const std::string graph = HorizonCode::toJson(HorizonCode::Graph{});
		w.addChunk(HAsset::CHUNK_HCGR, graph.data(), graph.size());
		// Absent chunk = plain Object.
		if (!spec.horizonCodeBaseClass.empty())
			w.addChunk(HAsset::CHUNK_HCBC, spec.horizonCodeBaseClass.data(),
			           spec.horizonCodeBaseClass.size());
	}
	// Input assets are born with valid minimal JSON so their editors and the
	// runtime parser never see an empty payload.
	if (type == HE::AssetType::InputAction)
	{
		const char* json = "{\"valueType\":\"Button\"}";
		w.addChunk(HAsset::CHUNK_IACT, json, std::strlen(json));
	}
	if (type == HE::AssetType::InputMappingContext)
	{
		const char* json = "{\"entries\":[]}";
		w.addChunk(HAsset::CHUNK_IMAP, json, std::strlen(json));
	}
	// Type-definition assets are born with valid empty JSON so the TypeAssetPanel
	// and the TypeRegistry never see an empty payload.
	if (type == HE::AssetType::StructType)
	{
		const char* json = "{\"fields\":[]}";
		w.addChunk(HAsset::CHUNK_STDF, json, std::strlen(json));
	}
	if (type == HE::AssetType::EnumType)
	{
		const char* json = "{\"entries\":[]}";
		w.addChunk(HAsset::CHUNK_ENDF, json, std::strlen(json));
	}
	if (type == HE::AssetType::SaveGameTemplate)
	{
		const char* json = "{\"fields\":[]}";
		w.addChunk(HAsset::CHUNK_SGTP, json, std::strlen(json));
	}

	return w.write(absolutePath, static_cast<uint16_t>(type));
}

bool isCreatableAssetType(HE::AssetType type)
{
	// No default label, for the reason the two switches in Types/Enums.h give:
	// a new AssetType then has to be classified deliberately instead of falling
	// through to a silent answer.
	switch (type)
	{
	// Authored in the editor, and a stub of it is something the matching panel
	// can open. Every one of these is a row of the Content Browser's create menu.
	case HE::AssetType::Material:
	case HE::AssetType::MaterialFunction:
	case HE::AssetType::Widget:
	case HE::AssetType::Theme:
	case HE::AssetType::HorizonCodeClass:
	case HE::AssetType::Script:
	case HE::AssetType::InputAction:
	case HE::AssetType::InputMappingContext:
	case HE::AssetType::ParticleSystem:
	case HE::AssetType::AnimatorStateMachine:
	case HE::AssetType::BoneMask:
	case HE::AssetType::BlendSpace:
	case HE::AssetType::StructType:
	case HE::AssetType::EnumType:
	case HE::AssetType::SaveGameTemplate:
		return true;

	// Imported, not authored: a stub carries no geometry, no pixels and no
	// samples, so the file would exist and every panel would fail to open it.
	case HE::AssetType::StaticMesh:
	case HE::AssetType::SkeletalMesh:
	case HE::AssetType::Texture:
	case HE::AssetType::Audio:
	case HE::AssetType::Font:
	case HE::AssetType::AnimationClip:
	case HE::AssetType::PropertyAnimClip:
	// Generated by the material codegen, never authored as a file.
	case HE::AssetType::Shader:
	// Made by dragging an entity subtree out of the scene — a prefab with no
	// PFAB payload is an empty file, not an empty prefab.
	case HE::AssetType::Prefab:
	// Not an .hasset at all: a scene is JSON at a .hescene path, and creating one
	// is a scene-persistence question rather than an asset one.
	case HE::AssetType::Scene:
	case HE::AssetType::Unknown:
		return false;
	}
	return false;
}

HE::AssetType assetTypeFromName(const std::string& name)
{
	if (name.empty()) return HE::AssetType::Unknown;
	// Walked rather than tabulated, so the mapping stays derived from
	// `assetTypeName` — one spelling, defined once, in Types/Enums.h.
	for (std::uint32_t i = 1; i <= static_cast<std::uint32_t>(HE::AssetType::BlendSpace); ++i)
	{
		const auto t = static_cast<HE::AssetType>(i);
		if (name == HE::assetTypeName(t)) return t;
	}
	return HE::AssetType::Unknown;
}

} // namespace HE::Ed
