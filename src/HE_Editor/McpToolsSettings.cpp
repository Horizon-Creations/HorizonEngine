#include "EditorSettingsCatalog.h"
#include "McpToolCommon.h"
#include "McpToolRegistry.h"

#include "ProjectManager.h"            // ProjectData, ExportProfile, HE::tools::toString
#include <Physics/CollisionLayers.h>
#include <Renderer/UIFont.h>           // UIFontScriptGreek / UIFontScriptCyrillic

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace HE::Ed
{
namespace
{
using json = nlohmann::json;
namespace fs = std::filesystem;

ToolResult failNoProject()
{
	return ToolResult::fail("no_project",
		"no project is open — the editor starts without one, which is a real "
		"state, and there are no project settings until File > Open Project has run");
}

const char* typeName(SettingType t)
{
	switch (t)
	{
	case SettingType::Bool:   return "boolean";
	case SettingType::Int:    return "integer";
	case SettingType::Float:  return "number";
	case SettingType::String: return "string";
	case SettingType::Enum:   return "enum";
	}
	return "unknown";
}

std::string lowered(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

// ─── The project half of the catalogue ───────────────────────────────────────
// ProjectData has forty fields and most of them are not settings: `id` is an
// identity, `exportProfiles` is a list with its own tool, `documentTypes` is a
// table. What is here is what a person can change on the Preferences pages that
// say "these are the project's" — plus the five that are readable and will not
// be written, each carrying the reason in its own row rather than in a comment
// nobody downstream can read.
struct ProjSetting
{
	std::string key;
	std::string label;
	std::string category;       // the Preferences page it is drawn on
	SettingType type = SettingType::String;
	std::string help;
	bool        writable = true;
	std::string readOnlyReason;
	std::vector<std::string> options;

	std::function<json(const ProjectData&)> get;
	// Null for a read-only row. `outChanged` distinguishes "set to what it
	// already was" from a real edit — the caller uses it to decide whether the
	// manifest has to be written at all.
	std::function<bool(ProjectData&, const json&, std::string& outError,
	                   bool& outChanged)> set;
};

fs::path projectRootOf(const ProjectData& p)
{
	return p.path.empty() ? fs::path() : fs::path(p.path).parent_path();
}

// A plain bool field.
ProjSetting boolProj(const char* key, const char* label, const char* category,
                     bool ProjectData::* member, const char* help)
{
	ProjSetting d;
	d.key = key; d.label = label; d.category = category;
	d.type = SettingType::Bool; d.help = help;
	d.get = [member](const ProjectData& p) { return json(p.*member); };
	d.set = [member](ProjectData& p, const json& in, std::string& err, bool& changed)
	{
		if (!in.is_boolean()) { err = "expected a boolean"; return false; }
		const bool v = in.get<bool>();
		changed = (p.*member != v);
		p.*member = v;
		return true;
	};
	return d;
}

// A plain string field with no meaning beyond its text.
ProjSetting strProj(const char* key, const char* label, const char* category,
                    std::string ProjectData::* member, const char* help)
{
	ProjSetting d;
	d.key = key; d.label = label; d.category = category;
	d.type = SettingType::String; d.help = help;
	d.get = [member](const ProjectData& p) { return json(p.*member); };
	d.set = [member](ProjectData& p, const json& in, std::string& err, bool& changed)
	{
		if (!in.is_string()) { err = "expected a string"; return false; }
		std::string v = in.get<std::string>();
		changed = (p.*member != v);
		p.*member = std::move(v);
		return true;
	};
	return d;
}

ProjSetting readOnlyProj(const char* key, const char* label, const char* category,
                         SettingType type, const char* reason, const char* help,
                         std::function<json(const ProjectData&)> get)
{
	ProjSetting d;
	d.key = key; d.label = label; d.category = category; d.type = type;
	d.help = help; d.writable = false; d.readOnlyReason = reason;
	d.get = std::move(get);
	return d;
}

// One font-script bit as its own boolean. The field is a mask, but "set bit 1 of
// fontScripts" is not a setting anybody can act on, and a mask handed to a model
// is a number it will get wrong in the direction of turning something off.
ProjSetting fontScriptProj(const char* key, const char* label, std::uint32_t bit,
                           const char* help)
{
	ProjSetting d;
	d.key = key; d.label = label; d.category = "Fonts";
	d.type = SettingType::Bool; d.help = help;
	d.get = [bit](const ProjectData& p) { return json((p.fontScripts & bit) != 0u); };
	d.set = [bit](ProjectData& p, const json& in, std::string& err, bool& changed)
	{
		if (!in.is_boolean()) { err = "expected a boolean"; return false; }
		const bool v = in.get<bool>();
		const bool had = (p.fontScripts & bit) != 0u;
		changed = (had != v);
		if (v) p.fontScripts |= bit; else p.fontScripts &= ~bit;
		return true;
	};
	return d;
}

std::vector<ProjSetting> buildProjectCatalog()
{
	std::vector<ProjSetting> t;

	// ── What the project IS. Readable, never writable ───────────────────────
	t.push_back(readOnlyProj("project.name", "Project Name", "Application",
		SettingType::String,
		"the manifest's file name is derived from it; renaming a project is a "
		"File menu operation, not a setting",
		"Display name of the project.",
		[](const ProjectData& p) { return json(p.name); }));
	t.push_back(readOnlyProj("project.path", "Manifest Path", "Application",
		SettingType::String, "where the project lives on disk",
		"Absolute path of the .heproj manifest.",
		[](const ProjectData& p) { return json(p.path); }));
	t.push_back(readOnlyProj("project.id", "Project Id", "Application",
		SettingType::String,
		"the stable identity a collaboration session compares — two people "
		"routinely have differently named copies of the same project",
		"Stable project identity, minted on creation.",
		[](const ProjectData& p) { return json(p.id); }));
	t.push_back(readOnlyProj("project.scriptLanguage", "Scripting Language",
		"Application", SettingType::String,
		"it decides which assets may exist at all (a Lua project has no "
		"HorizonCode classes) — changing it under a project full of scripts is a "
		"conversion nobody wrote, not a setting",
		"HorizonCode, Lua, Python or Cpp — chosen in the new-project wizard.",
		[](const ProjectData& p) { return json(HE::tools::toString(p.scriptLanguage)); }));
	t.push_back(readOnlyProj("project.appProject", "Application Project",
		"Application", SettingType::Bool,
		"it decides whether this project has a world at all",
		"True = an APPLICATION (no scene, no physics, draws on change); "
		"false = a game.",
		[](const ProjectData& p) { return json(p.appProject); }));

	// ── Startup ─────────────────────────────────────────────────────────────
	{
		ProjSetting d;
		d.key = "project.startupScene"; d.label = "Startup Scene";
		d.category = "Application"; d.type = SettingType::String;
		// Readable, and NOT writable, and the reason is in ProjectManager rather
		// than in a policy: saveProject is a read-modify-write that deliberately
		// preserves the manifest's own "startupScene" key and never writes this
		// field back (the comment there names it). A setter here would change the
		// struct, report `persisted: true` because the manifest WAS written, and
		// lose the value on the next load — the worst of the three possible
		// behaviours. The editor has no surface for it either.
		//
		// What IS settable is the export profile's own startupScene, which is
		// what a packaged build boots into: project_package takes it as an
		// argument.
		d.writable = false;
		d.readOnlyReason =
			"the .heproj manifest keeps this key and ProjectManager::saveProject "
			"never rewrites it, so a change here would be lost on the next load. "
			"To choose what a BUILD boots into, pass `startupScene` to "
			"project_package";
		d.help = "Project-relative path of the .hescene the editor opens with. "
		         "Empty = none. Stored absolute; answered project-relative, "
		         "because an absolute path off this machine means nothing to "
		         "anybody else who opens the project.";
		d.get = [](const ProjectData& p) -> json
		{
			if (p.startupScene.empty()) return json("");
			const fs::path root = projectRootOf(p);
			const fs::path rel = fs::path(p.startupScene).lexically_relative(root);
			if (rel.empty() || rel.native().rfind("..", 0) == 0)
				return json(p.startupScene);   // outside the project: say so plainly
			return json(rel.generic_string());
		};
		t.push_back(std::move(d));
	}
	t.push_back(strProj("project.defaultSaveTemplate", "Default Save Template",
	                    "Application", &ProjectData::defaultSaveTemplate,
	                    "Content-relative path of the SaveGameTemplate asset "
	                    "save.create() uses. Empty = the project's single "
	                    "template if it has exactly one."));

	// ── Look ────────────────────────────────────────────────────────────────
	t.push_back(strProj("project.theme", "Theme", "Appearance", &ProjectData::theme,
	                    "Content-relative path of a Theme asset. Empty = the "
	                    "engine's built-in default."));
	{
		ProjSetting d;
		d.key = "project.themeMode"; d.label = "Theme Mode"; d.category = "Appearance";
		d.type = SettingType::Enum; d.options = { "System", "Light", "Dark" };
		d.help = "Which of the theme's two sides is asked for. Empty in the "
		         "manifest reads as System.";
		d.get = [](const ProjectData& p) -> json
		{ return json(p.themeMode.empty() ? std::string("System") : p.themeMode); };
		d.set = [](ProjectData& p, const json& in, std::string& err, bool& changed)
		{
			if (!in.is_string()) { err = "expected a string"; return false; }
			static const char* kModes[] = { "System", "Light", "Dark" };
			const std::string want = lowered(in.get<std::string>());
			for (const char* m : kModes)
				if (lowered(m) == want)
				{
					// An empty field already READS as System, so asking for System
					// on a fresh project is not a change — reporting one would make
					// "set it to what settings_get told me" look like an edit.
					const std::string cur =
						p.themeMode.empty() ? std::string("System") : p.themeMode;
					changed = (cur != m);
					p.themeMode = m;
					return true;
				}
			err = "unknown value; expected one of: System, Light, Dark";
			return false;
		};
		t.push_back(std::move(d));
	}
	t.push_back(boolProj("project.fontWeightBold", "Bold UI Text", "Fonts",
	                     &ProjectData::fontWeightBold,
	                     "True is what the engine has always drawn. A new "
	                     "application is written with false, so <b> has "
	                     "something to be bolder than."));
	t.push_back(fontScriptProj("project.fontScriptGreek", "Greek",
	                           HE::UIFontScriptGreek,
	                           "Bake Greek glyphs into the UI atlases. Costs "
	                           "atlas area, which is why it is asked for."));
	t.push_back(fontScriptProj("project.fontScriptCyrillic", "Cyrillic",
	                           HE::UIFontScriptCyrillic,
	                           "Bake Cyrillic glyphs into the UI atlases."));

	// ── What a script here may reach outside the project ────────────────────
	t.push_back(boolProj("project.allowFiles", "Allow File Access", "Permissions",
	                     &ProjectData::allowFiles,
	                     "May a script NAME an absolute path on its own? A path "
	                     "the human picked in a file dialog is granted whatever "
	                     "this says — the choosing is the permission. The editor "
	                     "preview is gated by the same flag as the shipped app."));
	t.push_back(boolProj("project.allowProcesses", "Allow Process Launch",
	                     "Permissions", &ProjectData::allowProcesses,
	                     "May a script start another program?"));
	t.push_back(boolProj("project.allowNetwork", "Allow Network", "Permissions",
	                     &ProjectData::allowNetwork,
	                     "Reserved for the `http` API."));
	t.push_back(boolProj("project.advancedShaderEffects", "Advanced Shader Effects",
	                     "Application", &ProjectData::advancedShaderEffects,
	                     "May this project author MATERIALS (the node graphs)? "
	                     "Off hides the material creators, keeps the material "
	                     "editor shut and lets the packaged runtime be built "
	                     "without a shader compiler."));

	// ── What the application is, to the system around it ────────────────────
	t.push_back(strProj("project.appIconName", "Icon", "Application",
	                    &ProjectData::appIconName,
	                    "Name of the built-in icon the app icon is generated "
	                    "from. Empty = no icon written, the runtime's default "
	                    "stands."));
	t.push_back(strProj("project.appIconColor", "Icon Colour", "Application",
	                    &ProjectData::appIconColor, "\"#RRGGBB\"."));
	t.push_back(strProj("project.bundleId", "Bundle Id", "Application",
	                    &ProjectData::bundleId,
	                    "Empty = derived from the project name."));
	t.push_back(strProj("project.appVersion", "Version", "Application",
	                    &ProjectData::appVersion, ""));

	// ── Export ──────────────────────────────────────────────────────────────
	{
		ProjSetting d;
		d.key = "project.activeExportProfile"; d.label = "Active Export Profile";
		d.category = "Application"; d.type = SettingType::String;
		d.help = "Which profile Build > Export Project (and project_package "
		         "without a `profile` argument) uses.";
		d.get = [](const ProjectData& p) { return json(p.activeExportProfile); };
		d.set = [](ProjectData& p, const json& in, std::string& err, bool& changed)
		{
			if (!in.is_string()) { err = "expected a string"; return false; }
			const std::string v = in.get<std::string>();
			// A name that matches no profile would leave the export dialog
			// selecting profile 0 and this field lying about it.
			std::string names;
			for (const ExportProfile& p2 : p.exportProfiles)
			{
				if (!names.empty()) names += ", ";
				names += p2.name;
				if (p2.name == v)
				{
					changed = (p.activeExportProfile != v);
					p.activeExportProfile = v;
					return true;
				}
			}
			err = "no export profile named '" + v + "' — this project has: " + names;
			return false;
		};
		t.push_back(std::move(d));
	}

	return t;
}

const std::vector<ProjSetting>& projectCatalog()
{
	static const std::vector<ProjSetting> table = buildProjectCatalog();
	return table;
}

const ProjSetting* findProjectSetting(const std::string& key)
{
	for (const ProjSetting& d : projectCatalog())
		if (d.key == key) return &d;
	return nullptr;
}

// ─── The collision matrix, which is not a flat field ─────────────────────────
// Sixteen names and a 16×16 triangle. It does not fit a key/value table, so it
// gets two key SHAPES instead of a row each:
//   project.collisionLayers.name.<i>        → the layer's display name
//   project.collisionLayers.collides.<a>.<b> → may those two touch
// Both halves are read out whole by settings_get, so a client never has to guess
// which indices exist.
bool parseIndex(const std::string& s, int& out)
{
	if (s.empty() || s.size() > 2) return false;
	for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
	out = std::stoi(s);
	return out >= 0 && out < HE::CollisionLayerConfig::kCount;
}

// Returns false when `key` is not one of the two shapes at all (so the caller
// can fall through to "unknown key"); `handled` true with a failure means the
// shape matched but the value did not.
bool setCollisionLayerKey(ProjectData& proj, const std::string& key, const json& in,
                          std::string& err, bool& changed, bool& handled)
{
	handled = false;
	static const std::string kNamePrefix = "project.collisionLayers.name.";
	static const std::string kCollPrefix = "project.collisionLayers.collides.";

	if (key.rfind(kNamePrefix, 0) == 0)
	{
		handled = true;
		int idx = 0;
		if (!parseIndex(key.substr(kNamePrefix.size()), idx))
		{
			err = "layer index out of range (0 … "
			    + std::to_string(HE::CollisionLayerConfig::kCount - 1) + ")";
			return false;
		}
		if (!in.is_string()) { err = "expected a string"; return false; }
		const std::string v = in.get<std::string>();
		changed = (proj.collisionLayers.names[idx] != v);
		proj.collisionLayers.setLayerName(idx, v);
		return true;
	}
	if (key.rfind(kCollPrefix, 0) == 0)
	{
		handled = true;
		const std::string rest = key.substr(kCollPrefix.size());
		const auto dot = rest.find('.');
		int a = 0, b = 0;
		if (dot == std::string::npos ||
		    !parseIndex(rest.substr(0, dot), a) ||
		    !parseIndex(rest.substr(dot + 1), b))
		{
			err = "expected project.collisionLayers.collides.<a>.<b> with both "
			      "indices in 0 … "
			    + std::to_string(HE::CollisionLayerConfig::kCount - 1);
			return false;
		}
		if (!in.is_boolean()) { err = "expected a boolean"; return false; }
		const bool v = in.get<bool>();
		changed = (proj.collisionLayers.collides(a, b) != v);
		// setCollides writes BOTH cells — Jolt does not promise which order it
		// asks in, and a half-filled matrix collides on some frames and not on
		// others.
		proj.collisionLayers.setCollides(a, b, v);
		return true;
	}
	return false;
}

json collisionLayersToJson(const HE::CollisionLayerConfig& c)
{
	json names = json::array();
	for (int i = 0; i < HE::CollisionLayerConfig::kCount; ++i)
		names.push_back(json{
			{ "index", i },
			{ "name",  c.layerName(i) },
			{ "key",   "project.collisionLayers.name." + std::to_string(i) },
		});
	// Only the upper triangle: the matrix is symmetric by construction, and
	// listing both halves would invite a client to set one and wonder why the
	// other moved.
	json pairs = json::array();
	for (int a = 0; a < HE::CollisionLayerConfig::kCount; ++a)
		for (int b = a; b < HE::CollisionLayerConfig::kCount; ++b)
			if (!c.collides(a, b))
				pairs.push_back(json{
					{ "a", a }, { "b", b },
					{ "key", "project.collisionLayers.collides."
					         + std::to_string(a) + "." + std::to_string(b) },
				});
	return json{
		{ "layers",       std::move(names) },
		{ "isDefault",    c.isDefault() },
		{ "blockedPairs", std::move(pairs) },
		{ "note", "Everything collides unless it is in `blockedPairs`. Write a "
		          "pair with settings_set and the key given there; the matrix is "
		          "kept symmetric for you." },
	};
}

json exportProfilesToJson(const ProjectData& p)
{
	json out = json::array();
	for (const ExportProfile& e : p.exportProfiles)
		out.push_back(json{
			{ "name",               e.name },
			{ "targetPlatform",     e.targetPlatform },
			{ "outputDir",          e.outputDir },
			{ "startupScene",       e.startupScene },
			{ "compress",           e.compress },
			{ "encrypt",            e.encrypt },
			{ "enableModSupport",   e.enableModSupport },
			{ "incremental",        e.incremental },
			{ "appBundle",          e.appBundle },
			{ "compileHorizonCode", e.compileHorizonCode },
			{ "hcStopOnFailure",    e.hcStopOnFailure },
			{ "excludePatterns",    e.excludePatterns },
			{ "active",             e.name == p.activeExportProfile },
		});
	return out;
}

// ─── Describing one row ──────────────────────────────────────────────────────
json describeProject(const ProjSetting& d, const ProjectData& p)
{
	json j{
		{ "key",      d.key },
		{ "label",    d.label },
		{ "category", d.category },
		{ "type",     typeName(d.type) },
		{ "value",    d.get(p) },
		{ "writable", d.writable && d.set != nullptr },
	};
	if (!d.help.empty())            j["help"]           = d.help;
	if (!d.readOnlyReason.empty())  j["readOnlyReason"] = d.readOnlyReason;
	if (!d.options.empty())         j["options"]        = d.options;
	return j;
}

json describeEditor(const SettingDesc& d, const EditorConfig* cfg,
                    const McpSettingsHooks& hooks)
{
	json j{
		{ "key",      d.key },
		{ "label",    d.label },
		{ "category", d.category },
		{ "row",      d.row },
		{ "type",     typeName(d.type) },
		{ "writable", d.writable },
	};
	if (!d.help.empty())           j["help"]           = d.help;
	if (!d.readOnlyReason.empty()) j["readOnlyReason"] = d.readOnlyReason;
	if (!d.options.empty())        j["options"]        = d.options;
	if (d.hasRange && (d.type == SettingType::Int || d.type == SettingType::Float))
	{
		j["min"] = d.minValue;
		j["max"] = d.maxValue;
	}

	json value;
	bool have = false;
	if (d.storage == SettingStorage::Config && cfg)
		have = readEditorSetting(*cfg, d, value);
	else if (d.storage == SettingStorage::External && hooks.readExternalSetting)
		have = hooks.readExternalSetting(d.key, value);

	if (have)
	{
		j["value"] = std::move(value);
		// The enum's index alongside its name, for a caller that would rather
		// count than spell.
		if (d.type == SettingType::Enum && cfg && d.pi)
			j["index"] = cfg->*(d.pi);
	}
	else
		j["unavailable"] = "this editor does not expose that value";
	return j;
}

// ─── settings_get ────────────────────────────────────────────────────────────
void addGet(McpToolRegistry& registry, const McpSettingsHooks& h)
{
	McpTool t;
	t.name = "settings_get";
	t.description =
		"Read the project's settings (the .heproj manifest: permissions, fonts, "
		"startup scene, export profiles, collision layers) and this editor's own "
		"preferences (rendering, viewport, collaboration). Every row comes back "
		"with its key, its type, its range or its allowed values and whether it "
		"may be written — so settings_set never has to be guessed at.";
	t.mutates = false;
	t.inputSchema = objectSchema(json{
		{ "scope", json{
			{ "type", "string" },
			{ "enum", json::array({ "project", "editor", "all" }) },
			{ "description",
			  "Which half to read. Default 'all'. The editor presents both in one "
			  "place (there is no project settings surface — the project pages "
			  "live in Preferences), so both are here." } } },
		{ "key", stringProp(
			"One setting by key, instead of the whole list. The scope is inferred "
			"from the key.") },
		{ "category", stringProp(
			"Only the rows of one category, e.g. 'Permissions' or 'Post-Processing'.") },
	}, {});

	McpSettingsHooks hooks = h;
	t.handler = [hooks](const json& args) -> ToolResult
	{
		const std::string scope    = strArg(args, "scope");
		const std::string oneKey   = strArg(args, "key");
		const std::string category = strArg(args, "category");
		if (!scope.empty() && scope != "project" && scope != "editor" && scope != "all")
			return ToolResult::fail("invalid_argument",
				"unknown scope '" + scope + "' — expected project, editor or all");

		ProjectData*  proj = hooks.project      ? hooks.project()      : nullptr;
		EditorConfig* cfg  = hooks.editorConfig ? hooks.editorConfig() : nullptr;

		// ── One key ─────────────────────────────────────────────────────────
		if (!oneKey.empty())
		{
			if (const ProjSetting* d = findProjectSetting(oneKey))
			{
				if (!proj) return failNoProject();
				return ToolResult::ok(describeProject(*d, *proj));
			}
			if (const SettingDesc* d = findEditorSetting(oneKey))
				return ToolResult::ok(describeEditor(*d, cfg, hooks));
			return ToolResult::fail("not_found",
				"no setting named '" + oneKey + "' — call settings_get without a "
				"key for the list");
		}

		json out = json::object();

		if (scope.empty() || scope == "all" || scope == "project")
		{
			if (!proj && scope == "project") return failNoProject();
			if (proj)
			{
				json rows = json::array();
				for (const ProjSetting& d : projectCatalog())
					if (category.empty() || d.category == category)
						rows.push_back(describeProject(d, *proj));
				json p{
					{ "settings", std::move(rows) },
				};
				// The three things that are not flat rows. Left out of a filtered
				// read so a category query answers only what it asked for.
				if (category.empty())
				{
					p["exportProfiles"]  = exportProfilesToJson(*proj);
					p["collisionLayers"] = collisionLayersToJson(proj->collisionLayers);
				}
				out["project"] = std::move(p);
			}
			else
				out["project"] = json{ { "open", false },
					{ "note", "no project is open" } };
		}

		if (scope.empty() || scope == "all" || scope == "editor")
		{
			json rows = json::array();
			for (const SettingDesc& d : editorSettingCatalog())
				if (category.empty() || d.category == category)
					rows.push_back(describeEditor(d, cfg, hooks));
			json e{ { "settings", std::move(rows) } };
			if (!cfg) e["note"] = "this editor does not expose its config";
			// Honest about where a write lands: on the running editor now, on
			// disk only when somebody writes it.
			e["persistsImmediately"] = hooks.persistEditorConfig != nullptr;
			out["editor"] = std::move(e);
		}

		return ToolResult::ok(std::move(out));
	};
	registry.add(std::move(t));
}

// ─── settings_set ────────────────────────────────────────────────────────────
void addSet(McpToolRegistry& registry, const McpSettingsHooks& h)
{
	McpTool t;
	t.name = "settings_set";
	t.description =
		"Write one project or editor setting by key. The scope is inferred from "
		"the key, so settings_get is the only place the keys have to be learned. "
		"A project write is saved to the .heproj manifest; an editor write takes "
		"effect at once and reaches disk when the editor closes (the result says "
		"which happened). The Remote Control settings — the MCP bridge's own "
		"switch and port — are readable and are never written from here.";
	t.mutates = true;
	t.inputSchema = objectSchema(json{
		{ "key", stringProp("The setting's key, as settings_get reports it.") },
		{ "value", json{
			{ "description",
			  "The new value. A boolean for a boolean, a number for a number, and "
			  "for an enum either its name or its index." } } },
	}, { "key", "value" });

	McpSettingsHooks hooks = h;
	t.handler = [hooks](const json& args) -> ToolResult
	{
		const std::string key = strArg(args, "key");
		if (key.empty())
			return ToolResult::fail("invalid_argument", "`key` is required");
		if (!args.contains("value"))
			return ToolResult::fail("invalid_argument", "`value` is required");
		const json& value = args["value"];

		// ── project scope ───────────────────────────────────────────────────
		const bool looksProject = key.rfind("project.", 0) == 0;
		if (looksProject)
		{
			ProjectData* proj = hooks.project ? hooks.project() : nullptr;
			if (!proj) return failNoProject();

			std::string err;
			bool changed = false;
			bool touchedLayers = false;

			bool handled = false;
			if (setCollisionLayerKey(*proj, key, value, err, changed, handled))
				touchedLayers = true;
			else if (handled)
				return ToolResult::fail("invalid_argument", err);
			else
			{
				const ProjSetting* d = findProjectSetting(key);
				if (!d)
					return ToolResult::fail("not_found",
						"no project setting named '" + key + "' — call settings_get "
						"with scope 'project' for the list");
				if (!d->writable || !d->set)
					return ToolResult::fail("read_only",
						"'" + key + "' cannot be written: "
						+ (d->readOnlyReason.empty() ? std::string("read-only")
						                             : d->readOnlyReason));
				if (!d->set(*proj, value, err, changed))
					return ToolResult::fail("invalid_argument", err);
			}

			// The manifest is written even when nothing moved — a caller asking
			// for a value it already had should not be able to tell the
			// difference, and saving an unchanged project costs one file write.
			bool persisted = false;
			if (hooks.saveProject) persisted = hooks.saveProject();
			if (hooks.saveProject && !persisted)
				return ToolResult::fail("write_failed",
					"the value was set in memory but the .heproj manifest could not "
					"be written — the change will be lost when the project closes");

			// The collision matrix is the one project setting that has to reach a
			// RUNNING simulation, and it only gets there through this callback.
			// Outside play mode it does nothing, which is right: the next play
			// start reads the matrix afresh.
			if (touchedLayers && hooks.applyCollisionLayers) hooks.applyCollisionLayers();

			json result{
				{ "scope",     "project" },
				{ "key",       key },
				{ "changed",   changed },
				{ "persisted", persisted },
			};
			if (!touchedLayers)
				if (const ProjSetting* d = findProjectSetting(key))
					result["value"] = d->get(*proj);
			if (touchedLayers)
				result["appliedToRunningSimulation"] = hooks.applyCollisionLayers != nullptr;
			return ToolResult::ok(std::move(result));
		}

		// ── editor scope ────────────────────────────────────────────────────
		const SettingDesc* d = findEditorSetting(key);
		if (!d)
			return ToolResult::fail("not_found",
				"no setting named '" + key + "' — call settings_get for the list");

		// Named before the writability check so the reason a client reads is the
		// specific one rather than a generic read_only.
		if (d->category == kRemoteControlCategory)
			return ToolResult::fail("read_only",
				"'" + key + "' is a Remote Control setting — this is the bridge "
				"that carries this very call, and it will not switch itself off "
				"or move its own port. Change it in Preferences > Editor > "
				"Remote Control.");
		if (!d->writable)
			return ToolResult::fail("read_only",
				"'" + key + "' cannot be written: "
				+ (d->readOnlyReason.empty() ? std::string("read-only")
				                             : d->readOnlyReason));

		if (d->storage == SettingStorage::External)
		{
			if (!hooks.writeExternalSetting)
				return ToolResult::fail("unavailable",
					"this editor does not expose '" + key + "' for writing");
			std::string err;
			if (!hooks.writeExternalSetting(key, value, err))
				return ToolResult::fail("invalid_argument",
					err.empty() ? std::string("the editor refused that value") : err);
			json out{
				{ "scope",   "editor" },
				{ "key",     key },
				{ "changed", true },
				// An External value does not live in config.json at all; saying
				// "persisted: false" would suggest it is waiting to be written.
				{ "persisted", false },
				{ "note", "applied to the running editor" },
			};
			json read;
			if (hooks.readExternalSetting && hooks.readExternalSetting(key, read))
				out["value"] = std::move(read);
			return ToolResult::ok(std::move(out));
		}

		EditorConfig* cfg = hooks.editorConfig ? hooks.editorConfig() : nullptr;
		if (!cfg)
			return ToolResult::fail("unavailable",
				"this editor does not expose its config");

		std::string err;
		bool changed = false;
		if (!writeEditorSetting(*cfg, *d, value, err, changed))
			return ToolResult::fail("invalid_argument", err);

		// Most render settings are re-read from the config every frame, so the
		// field write IS the whole job. The two that are not (the frame cap, the
		// collision matrix) reach their owner through this callback, and a write
		// that skipped it would be a number in a file that changes nothing.
		if (!d->apply.empty() && hooks.applySetting) hooks.applySetting(d->apply, *cfg);

		bool persisted = false;
		if (hooks.persistEditorConfig) persisted = hooks.persistEditorConfig();

		json value2;
		readEditorSetting(*cfg, *d, value2);
		json out{
			{ "scope",     "editor" },
			{ "key",       key },
			{ "value",     std::move(value2) },
			{ "changed",   changed },
			{ "persisted", persisted },
		};
		if (!persisted)
			out["note"] = "in effect now; the editor writes config.json when it closes";
		return ToolResult::ok(std::move(out));
	};
	registry.add(std::move(t));
}

} // namespace

void registerSettingsTools(McpToolRegistry& registry, McpSettingsHooks hooks)
{
	addGet(registry, hooks);
	addSet(registry, hooks);
}

} // namespace HE::Ed
