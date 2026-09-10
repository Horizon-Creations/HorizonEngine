#include "McpToolRegistry.h"

#include "EditorAssetTypeCache.h"     // the cached header sniff — what a path holds
#include "InputMappingModel.h"        // the decoded mapping + its JSON codec
#include "McpToolCommon.h"            // the argument readers and the ONE confinement rule

#include <Application/InputAssets.h>  // the payload vocabulary, shared with the loader
#include <Application/InputMapping.h> // AxisSource
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// ─── Binding a key from outside the editor ───────────────────────────────────
// The rationale for the shape of this file — why input needs tools of its own,
// and why an open tab is refused rather than edited — is in McpToolRegistry.h
// beside McpInputHooks. What is worth stating HERE is what the handlers promise:
//
//   • EVERY NAME IS CHECKED AGAINST THE LOADER'S OWN TABLE. This is the whole
//     point. `applyInputMappingContext` skips a name it cannot parse and says
//     nothing, so "Spacebar" instead of "Space" is a binding that exists in the
//     file, shows up in the panel and does nothing in the game. Names go through
//     SDL's tables here (`SDL_GetScancodeFromName`,
//     `SDL_GetGamepadButtonFromString`) and `HE::mouseButtonFromName` /
//     `HE::axisSourceFromName` — the very functions the loader calls — and a
//     miss is a refusal that names the tool to look the spelling up with.
//
//   • THE ACTION IS RESOLVED BEFORE ITS BINDING IS ACCEPTED. A key on an Axis
//     action would land in "keys", which `mapAxis` never reads: a binding that
//     is present and dead. So the action's declared value type decides which
//     argument is legal, and the wrong one is refused with the right shape
//     spelled out.
//
//   • EVERY ENTRY'S VALUE TYPE IS RE-RESOLVED BEFORE ANYTHING IS ENCODED, not
//     just the entry a call touched. `decodeMapping` leaves `valueType` at -1
//     and `encodeMapping` falls back to the old shape rule for that — under
//     which a 2D entry whose Y list is still empty writes "axes" instead of
//     "axesX", which registers a ONE-dimensional mapping and makes
//     `axis2DValue()` answer 0,0 forever. Re-resolving one entry and saving
//     would break the others.
//
//   • A READER NEVER LOADS. Both payloads are one small JSON chunk, so the
//     readers open the FILE (`HAsset::Reader`) instead of calling `loadAsset`,
//     which registers the asset as a side effect, moves the dense asset pool and
//     invalidates every pointer the editor is holding at that moment — the same
//     rule the asset tools follow, and the reason `input_actions` can list a
//     whole project's actions without touching what is resident.
//
//   • NOTHING IS WRITTEN BEHIND AN UNSAVED TAB. The panel has no undo, so an
//     open tab with edits is a refusal (`dirty`), and a clean one is told to
//     re-read the file afterwards.

namespace HE::Ed
{

using nlohmann::json;
namespace fs = std::filesystem;

namespace
{

// ── Value types ──────────────────────────────────────────────────────────────
// 0 Button, 1 Axis, 2 Axis 2D — the panel's own numbering (InputAssetPanel's
// PanelState::valueType), and -1/-2 keep the meaning MapEntry gives them:
// unresolved, and unresolvable because the action is missing.
constexpr int kVtButton = 0;
constexpr int kVtAxis   = 1;
constexpr int kVtAxis2D = 2;

const char* valueTypeName(int vt)
{
	return vt == kVtAxis2D ? "Axis2D" : vt == kVtAxis ? "Axis" : "Button";
}

// -1 = not one of the three. Deliberately exact: `makeInputActionJson` writes
// whatever string it is handed, and "axis" would become an action the loader
// reads as a Button.
int valueTypeFromName(const std::string& s)
{
	if (s == "Button") return kVtButton;
	if (s == "Axis")   return kVtAxis;
	if (s == "Axis2D") return kVtAxis2D;
	return -1;
}

// The HorizonCode events a graph can listen for. Exactly one kind per value
// type — PlayerHost::tick fires Pressed/Released for a Button, Axis for an Axis
// and Axis2D for a 2D one, and nothing else — so listing all four would invite a
// client to wire up a handler that can never run.
json eventsOf(const std::string& actionName, int vt)
{
	json j = json::object();
	if (vt == kVtAxis2D)   j["axis2D"] = HE::inputEventAxis2D(actionName);
	else if (vt == kVtAxis) j["axis"]  = HE::inputEventAxis(actionName);
	else
	{
		j["pressed"]  = HE::inputEventPressed(actionName);
		j["released"] = HE::inputEventReleased(actionName);
	}
	return j;
}

// ── Reading a payload without loading the asset ──────────────────────────────
struct Payload
{
	bool          ok = false;
	HE::AssetType type = HE::AssetType::Unknown;
	std::string   json;      // the IACT / IMAP chunk, verbatim
};

Payload readPayload(const std::string& abs)
{
	Payload p;
	p.type = EditorAssetTypeCache::assetTypeOf(abs);
	if (p.type != HE::AssetType::InputAction && p.type != HE::AssetType::InputMappingContext)
		return p;

	HAsset::Reader r;
	if (!r.open(abs)) return p;
	const uint32_t want = p.type == HE::AssetType::InputAction ? HAsset::CHUNK_IACT
	                                                           : HAsset::CHUNK_IMAP;
	if (const HAsset::Reader::Chunk* c = r.findChunk(want))
		p.json.assign(reinterpret_cast<const char*>(c->data.data()), c->data.size());
	// An absent chunk is not a failure: an input asset written before the stub
	// writer put valid JSON in it carries nothing, and the panel reads that as
	// an empty Button action / an empty context. The tools must agree, or a file
	// a human can edit would be one MCP refuses to touch.
	p.ok = true;
	return p;
}

// The value type an InputAction asset declares, read from its file. -2 when the
// path holds no InputAction at all — the same "unresolvable" MapEntry uses,
// because that is exactly what it means for an entry that references it.
int valueTypeOfActionFile(ContentManager& content, const std::string& actionRel)
{
	if (actionRel.empty()) return -2;
	const std::string abs = content.resolveAbsolutePath(actionRel);
	if (abs.empty()) return -2;
	const Payload p = readPayload(abs);
	if (!p.ok || p.type != HE::AssetType::InputAction) return -2;
	return HE::inputActionIsAxis2D(p.json) ? kVtAxis2D
	     : HE::inputActionIsAxis(p.json)   ? kVtAxis
	                                       : kVtButton;
}

// Fill in the value type of EVERY entry before anything is encoded. See the
// third promise at the top of the file: doing only the touched entry would
// downgrade a sibling 2D entry from axesX to axes on the same save.
void resolveEntryValueTypes(ContentManager& content, std::vector<MapEntry>& entries)
{
	for (MapEntry& e : entries) e.valueType = valueTypeOfActionFile(content, e.actionPath);
}

// ── The vocabulary, as the loader spells it ──────────────────────────────────
bool isKnownKey(const std::string& name)
{
	return !name.empty() && SDL_GetScancodeFromName(name.c_str()) != SDL_SCANCODE_UNKNOWN;
}

bool isKnownPadButton(const std::string& name)
{
	return !name.empty() &&
	       SDL_GetGamepadButtonFromString(name.c_str()) != SDL_GAMEPAD_BUTTON_INVALID;
}

bool isKnownMouseButton(const std::string& name)
{
	return HE::mouseButtonFromName(name) >= 0;
}

// `axisSourceFromName` answers Key for anything it does not know — "never as an
// error", which is right for the loader and wrong here. The round trip is what
// separates the real "Key" from garbage.
bool isKnownAxisSource(const std::string& name)
{
	return HE::axisSourceName(HE::axisSourceFromName(name)) == name;
}

std::string allAxisSourceNames()
{
	std::string s;
	for (int i = 0; i <= static_cast<int>(AxisSource::GamepadRightTrigger); ++i)
		s += (s.empty() ? "" : ", ") + HE::axisSourceName(static_cast<AxisSource>(i));
	return s;
}

// Every named scancode, in scancode order — the order that groups letters,
// digits and the function row, which is what makes the list readable at all.
std::vector<std::string> allKeyNames()
{
	std::vector<std::string> v;
	for (int sc = 0; sc < SDL_SCANCODE_COUNT; ++sc)
		if (const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(sc)); n && n[0])
			v.emplace_back(n);
	return v;
}

// ── The addressed asset ──────────────────────────────────────────────────────
// One struct for both types, because the path checks, the play-mode gate, the
// lock gate and the unsaved-tab gate are the same four questions for an action
// and for a context.
struct Doc
{
	std::string rel;
	std::string abs;
	std::string payload;                        // the IACT / IMAP chunk
	bool        ok = false;
	bool        openDirty = false;              // a tab holds it with edits
	ToolResult  failure = ToolResult::ok(json::object());
};

Doc openDoc(ContentManager& content, const McpInputHooks& h, const json& args,
            HE::AssetType want, bool forWrite)
{
	Doc d;
	const PathCheck p = checkPath(content, strArg(args, "path"), /*mustExist=*/true, "path");
	if (!p.ok) { d.failure = p.failure; return d; }
	d.rel = p.rel;
	d.abs = p.abs;

	const Payload payload = readPayload(p.abs);
	if (!payload.ok || payload.type != want)
	{
		const char* wanted = want == HE::AssetType::InputAction ? "an Input Action"
		                                                       : "an Input Mapping Context";
		const char* other  = want == HE::AssetType::InputAction ? "input_mappings"
		                                                       : "input_actions";
		d.failure = ToolResult::fail("invalid_path",
			"'" + p.rel + "' is not " + std::string(wanted) + " asset. asset_resolve "
			"reports what a path holds, " + other + " lists the other kind, and "
			"asset_create with type '" +
			std::string(want == HE::AssetType::InputAction ? "InputAction"
			                                               : "InputMappingContext") +
			"' makes a new one.");
		return d;
	}
	d.payload   = payload.json;
	d.openDirty = h.isDirty && h.isDirty(p.rel);

	if (forWrite)
	{
		if (p.engine) { d.failure = failEngineReadOnly(p.rel); return d; }
		if (h.isPlaying && h.isPlaying())
		{
			d.failure = ToolResult::fail("play_mode",
				"Play-in-editor is running, and the running session has already applied "
				"this mapping — a change now would be half in effect and half not. It is "
				"refused rather than half-applied. Ask the user to stop play mode.");
			return d;
		}
		if (h.lockedByOther && h.lockedByOther(p.rel))
		{
			d.failure = ToolResult::fail("locked_by_other",
				"Another participant in the collaboration session holds '" + p.rel +
				"' right now. Wait until they let go, or work on something else.");
			return d;
		}
		if (d.openDirty)
		{
			d.failure = ToolResult::fail("dirty",
				"'" + p.rel + "' is open in the editor with unsaved changes. The Input "
				"Asset editor keeps no undo history, so there is no safe place to put "
				"this edit: writing the file would be reverted by the human's next Save, "
				"and writing into the tab would be a change they could not take back. "
				"Ask the user to save or close that tab, then call again.");
			return d;
		}
	}
	d.ok = true;
	return d;
}

// Write a payload back and tell an open tab to re-read it. The load is the ONLY
// one in the writing path and nothing is loaded after it, so the pointer taken
// here cannot be moved out from under us by a second asset registering
// (ContentManager.h: the pool is a dense vector).
ToolResult writePayload(ContentManager& content, const McpInputHooks& h, Doc& d,
                        HE::AssetType type, const std::string& payload, json out)
{
	const HE::UUID id = content.loadAsset(d.rel);
	bool wrote = false;
	if (!(id == HE::UUID{}))
	{
		if (type == HE::AssetType::InputAction)
		{
			if (InputActionAsset* a = content.getInputActionMutable(id))
			{
				a->json = payload;
				wrote = content.saveAsset(*a);
			}
		}
		else if (InputMappingContextAsset* m = content.getInputMappingContextMutable(id))
		{
			m->json = payload;
			wrote = content.saveAsset(*m);
		}
	}
	if (!wrote)
		return ToolResult::fail("failed",
			"Could not write '" + d.rel + "'. A read-only file or a full disk is the "
			"usual cause; the editor log carries the reason.");

	// The tab was clean — openDoc refused otherwise — so this is the collab-peer
	// path: drop what it had and read the file again next frame.
	out["path"]             = d.rel;
	out["reloadedInEditor"] = h.reloadFromDisk ? h.reloadFromDisk(d.rel) : false;
	return ToolResult::ok(std::move(out));
}

// ── Walking the project for input assets ─────────────────────────────────────
// The same rules asset_list walks by: dotfiles are VCS/OS bookkeeping, the list
// is sorted so two calls on an unchanged project answer identically, and the
// count is capped so a large project cannot stall the editor's frame (MCP
// handlers run on the frame thread — McpBridge.h).
std::vector<std::pair<std::string, std::string>> findInputAssets(
	ContentManager& content, HE::AssetType want, int limit, bool& truncated)
{
	std::vector<std::pair<std::string, std::string>> found;   // (rel, abs)
	truncated = false;
	const std::string root = content.contentRoot();
	if (root.empty()) return found;

	std::vector<std::string> absPaths;
	std::error_code ec;
	fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
	const fs::recursive_directory_iterator end;
	for (; !ec && it != end; it.increment(ec))
	{
		if (it->path().filename().string().rfind('.', 0) == 0)
		{
			std::error_code dirEc;
			if (it->is_directory(dirEc)) it.disable_recursion_pending();
			continue;
		}
		std::error_code e;
		if (!it->is_regular_file(e)) continue;
		if (it->path().extension() != ".hasset") continue;
		absPaths.push_back(it->path().lexically_normal().string());
	}
	std::sort(absPaths.begin(), absPaths.end());

	for (const std::string& abs : absPaths)
	{
		if (EditorAssetTypeCache::assetTypeOf(abs) != want) continue;
		if (static_cast<int>(found.size()) >= limit) { truncated = true; break; }
		found.emplace_back(content.toContentRelativePath(abs), abs);
	}
	return found;
}

// ── Reporting a mapping ──────────────────────────────────────────────────────
json axisRowJson(const AxisRow& r)
{
	json j = json::object();
	j["source"] = HE::axisSourceName(r.source);
	j["scale"]  = r.scale;
	if (!r.positive.empty())       j["positive"]       = r.positive;
	if (!r.negative.empty())       j["negative"]       = r.negative;
	if (!r.positiveButton.empty()) j["positiveButton"] = r.positiveButton;
	if (!r.negativeButton.empty()) j["negativeButton"] = r.negativeButton;
	return j;
}

// Names in this row the LOADER will skip. Reported rather than hidden: a file a
// human hand-edited, or one written before these tools existed, can carry them,
// and "the binding is in the file" is not the same as "the binding works".
void collectDeadNames(const AxisRow& r, const char* list, int index, json& into)
{
	const auto add = [&](const char* field, const std::string& name) {
		if (name.empty() || isKnownKey(name)) return;
		into.push_back(json{ { "list", list }, { "index", index },
		                     { "field", field }, { "name", name } });
	};
	const auto addPad = [&](const char* field, const std::string& name) {
		if (name.empty() || isKnownPadButton(name)) return;
		into.push_back(json{ { "list", list }, { "index", index },
		                     { "field", field }, { "name", name } });
	};
	add("positive", r.positive);
	add("negative", r.negative);
	addPad("positiveButton", r.positiveButton);
	addPad("negativeButton", r.negativeButton);
}

json entryJson(const MapEntry& e)
{
	json j = json::object();
	j["action"]     = e.actionPath;
	j["actionName"] = HE::inputActionNameFromPath(e.actionPath);
	j["valueType"]  = e.valueType < 0 ? "unresolved" : valueTypeName(e.valueType);
	if (e.valueType == -2) j["actionMissing"] = true;
	if (!e.keys.empty())           j["keys"]           = e.keys;
	if (!e.gamepadButtons.empty()) j["gamepadButtons"] = e.gamepadButtons;
	if (!e.mouseButtons.empty())   j["mouseButtons"]   = e.mouseButtons;

	const char* xList = e.valueType == kVtAxis2D ? "axesX" : "axes";
	if (!e.axes.empty())
	{
		json arr = json::array();
		for (const AxisRow& r : e.axes) arr.push_back(axisRowJson(r));
		j[xList] = std::move(arr);
	}
	if (!e.axesY.empty())
	{
		json arr = json::array();
		for (const AxisRow& r : e.axesY) arr.push_back(axisRowJson(r));
		j["axesY"] = std::move(arr);
	}

	json dead = json::array();
	for (size_t i = 0; i < e.keys.size(); ++i)
		if (!isKnownKey(e.keys[i]))
			dead.push_back(json{ { "list", "keys" }, { "index", (int)i },
			                     { "name", e.keys[i] } });
	for (size_t i = 0; i < e.gamepadButtons.size(); ++i)
		if (!isKnownPadButton(e.gamepadButtons[i]))
			dead.push_back(json{ { "list", "gamepadButtons" }, { "index", (int)i },
			                     { "name", e.gamepadButtons[i] } });
	for (size_t i = 0; i < e.mouseButtons.size(); ++i)
		if (!isKnownMouseButton(e.mouseButtons[i]))
			dead.push_back(json{ { "list", "mouseButtons" }, { "index", (int)i },
			                     { "name", e.mouseButtons[i] } });
	for (size_t i = 0; i < e.axes.size(); ++i)   collectDeadNames(e.axes[i],  xList,   (int)i, dead);
	for (size_t i = 0; i < e.axesY.size(); ++i)  collectDeadNames(e.axesY[i], "axesY", (int)i, dead);
	if (!dead.empty()) j["deadNames"] = std::move(dead);
	return j;
}

// ── Argument helpers ─────────────────────────────────────────────────────────
json actionPathProp()
{
	return stringProp("Content-relative path of the Input Action asset, e.g. "
	                  "'Input/IA_Jump.hasset'. input_actions names them.");
}

// Which of the two axis lists a call addresses. An Axis 2D entry has two, and
// they are separate lists rather than a shape inside one so the 1D reader can
// never half-read a 2D entry (InputAssets.cpp).
bool wantsYComponent(const json& args, int valueType, std::string& why)
{
	const std::string c = strArg(args, "component");
	if (valueType != kVtAxis2D)
	{
		if (!c.empty() && c != "x")
			why = "'component' only means something for an Axis2D action; this one is " +
			      std::string(valueTypeName(valueType)) + ".";
		return false;
	}
	if (c == "x") return false;
	if (c == "y") return true;
	why = "'component' is required for an Axis2D action and must be 'x' or 'y' — the "
	      "two dimensions are bound separately, and an axis row belongs to exactly one "
	      "of them.";
	return false;
}

} // namespace

// ─── The tools ───────────────────────────────────────────────────────────────

void registerInputTools(McpToolRegistry& registry, ContentManager& content,
                        McpInputHooks hooks)
{
	ContentManager* cm = &content;
	auto h = std::make_shared<McpInputHooks>(std::move(hooks));

	// ── input_bindable ───────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "input_bindable";
		t.description =
			"Every name that can be bound: keyboard keys, gamepad buttons, mouse buttons "
			"and axis sources. This is what makes input_mapping_bind callable without "
			"guessing, and guessing is the one thing that must not happen here — the "
			"runtime SKIPS a binding whose name it cannot parse, without a word, so "
			"'Spacebar' instead of 'Space' is a binding that sits in the file, shows up "
			"in the editor and does nothing in the game. Names are SDL's own "
			"('Space', 'Left Shift', 'a' for the pad's South button), so use this list "
			"rather than a guess at the spelling.";
		t.inputSchema = objectSchema(json{
			{ "filter", stringProp("Report only keys whose name contains this, case "
			                       "sensitive — the key list is ~240 names long. The "
			                       "pad, mouse and axis lists are short and always "
			                       "complete.") },
		}, {});
		t.handler = [](const json& args) -> ToolResult {
			const std::string filter = strArg(args, "filter");
			json keys = json::array();
			for (const std::string& n : allKeyNames())
				if (filter.empty() || n.find(filter) != std::string::npos)
					keys.push_back(n);

			json pads = json::array();
			for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
			{
				const char* n = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(b));
				if (!n || !n[0]) continue;
				pads.push_back(json{
					{ "name",  n },
					// The stored name is SDL's Xbox-layout letter; the label is what a
					// person recognises on their own pad.
					{ "label", HE::gamepadButtonDisplayName(static_cast<SDL_GamepadButton>(b)) } });
			}

			json mice = json::array();
			for (int b = 0; b < kMouseButtonCount; ++b)
				mice.push_back(json{ { "name",  HE::mouseButtonName(b) },
				                     { "label", HE::mouseButtonDisplayName(b) } });

			json sources = json::array();
			for (int i = 0; i <= static_cast<int>(AxisSource::GamepadRightTrigger); ++i)
			{
				const AxisSource s = static_cast<AxisSource>(i);
				sources.push_back(json{
					{ "name", HE::axisSourceName(s) },
					// The difference decides whether game code may multiply by delta
					// time: a delta source already says "how far", not "how fast".
					{ "isDelta", axisSourceIsDelta(s) },
					{ "needsKeys", s == AxisSource::Key } });
			}

			return ToolResult::ok(json{
				{ "keys",           std::move(keys) },
				{ "gamepadButtons", std::move(pads) },
				{ "mouseButtons",   std::move(mice) },
				{ "axisSources",    std::move(sources) },
			});
		};
		registry.add(std::move(t));
	}

	// ── input_actions ────────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "input_actions";
		t.description =
			"Read the project's Input Actions: what each one is called, whether it is a "
			"Button, an Axis or an Axis 2D, whether it keeps firing while the game is "
			"paused, and the exact HorizonCode event names it fires. Without 'path' it "
			"lists every action in the project; with one it reports that action. The "
			"value type is what decides which bindings an action can take, so read this "
			"before input_mapping_bind.";
		t.inputSchema = objectSchema(json{
			{ "path",  stringProp("Report only this action, content-relative, e.g. "
			                      "'Input/IA_Jump.hasset'. Omit for all of them.") },
			{ "limit", json{ { "type", "integer" },
			                 { "description", "Most actions to report when listing. "
			                                  "Default 200." } } },
		}, {});
		t.handler = [cm, h](const json& args) -> ToolResult {
			if (hasArg(args, "path"))
			{
				Doc d = openDoc(*cm, *h, args, HE::AssetType::InputAction, /*forWrite=*/false);
				if (!d.ok) return d.failure;
				const std::string name = HE::inputActionNameFromPath(d.rel);
				const int vt = HE::inputActionIsAxis2D(d.payload) ? kVtAxis2D
				             : HE::inputActionIsAxis(d.payload)   ? kVtAxis : kVtButton;
				json out{
					{ "path",           d.rel },
					{ "name",           name },
					{ "valueType",      valueTypeName(vt) },
					{ "runWhilePaused", HE::inputActionRunsWhilePaused(d.payload) },
					{ "events",         eventsOf(name, vt) },
				};
				if (d.openDirty) out["openInEditorUnsaved"] = true;
				return ToolResult::ok(std::move(out));
			}

			const int limit = std::max(1, intArg(args, "limit", 200));
			bool truncated = false;
			const auto found = findInputAssets(*cm, HE::AssetType::InputAction, limit, truncated);

			json arr = json::array();
			// Stems, because the mapping references an action by PATH while events
			// fire on its stem (`inputActionNameFromPath`) — two actions called
			// IA_Fire in different folders are one action to the runtime, and no
			// other tool is in a position to notice.
			std::vector<std::string> names;
			for (const auto& [rel, abs] : found)
			{
				const Payload p = readPayload(abs);
				if (!p.ok) continue;
				const std::string name = HE::inputActionNameFromPath(rel);
				const int vt = HE::inputActionIsAxis2D(p.json) ? kVtAxis2D
				             : HE::inputActionIsAxis(p.json)   ? kVtAxis : kVtButton;
				json j{
					{ "path",           rel },
					{ "name",           name },
					{ "valueType",      valueTypeName(vt) },
					{ "runWhilePaused", HE::inputActionRunsWhilePaused(p.json) },
				};
				if (h->isDirty && h->isDirty(rel)) j["openInEditorUnsaved"] = true;
				arr.push_back(std::move(j));
				names.push_back(name);
			}

			json out{ { "actions", std::move(arr) } };
			if (truncated) out["truncated"] = true;

			std::sort(names.begin(), names.end());
			json dupes = json::array();
			for (size_t i = 1; i < names.size(); ++i)
				if (names[i] == names[i - 1] &&
				    (dupes.empty() || dupes.back() != names[i]))
					dupes.push_back(names[i]);
			if (!dupes.empty())
			{
				out["duplicateNames"] = std::move(dupes);
				out["duplicateNamesNote"] =
					"Two or more actions share a name. Events and bindings key on the "
					"NAME (the file stem), not the path, so those actions are one action "
					"to the runtime. Rename one with asset_move.";
			}
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}

	// ── input_action_set ─────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "input_action_set";
		t.description =
			"Change what an Input Action is: its value type and whether it runs while "
			"the game is paused. Retyping an action changes which event it fires — a "
			"Button fires Pressed/Released, an Axis fires Axis, an Axis 2D fires its own "
			"Axis2D — so a graph that listened for the old one simply stops being "
			"called, and bindings already made for the old shape stop being read. Every "
			"mapping context that references this action is reported for exactly that "
			"reason.";
		t.mutates     = true;
		t.inputSchema = objectSchema(json{
			{ "path",           actionPathProp() },
			{ "valueType",      stringProp("'Button', 'Axis' or 'Axis2D'. Omit to keep "
			                               "the one it has.") },
			{ "runWhilePaused", json{ { "type", "boolean" },
			                          { "description", "Keep firing while the game is "
			                                           "paused (time scale 0) or routed "
			                                           "UI-only. Default false, which is "
			                                           "what a pause is for; a pause "
			                                           "menu's own actions opt in. Omit "
			                                           "to keep the current setting." } } },
		}, { "path" });
		t.handler = [cm, h](const json& args) -> ToolResult {
			Doc d = openDoc(*cm, *h, args, HE::AssetType::InputAction, /*forWrite=*/true);
			if (!d.ok) return d.failure;

			const int wasVt = HE::inputActionIsAxis2D(d.payload) ? kVtAxis2D
			                : HE::inputActionIsAxis(d.payload)   ? kVtAxis : kVtButton;
			const bool wasPaused = HE::inputActionRunsWhilePaused(d.payload);

			int vt = wasVt;
			if (hasArg(args, "valueType"))
			{
				const std::string want = strArg(args, "valueType");
				vt = valueTypeFromName(want);
				if (vt < 0)
					return ToolResult::fail("invalid_payload",
						"'" + want + "' is not a value type. The three are 'Button' (a "
						"key/button that is pressed and released), 'Axis' (one float from "
						"a key pair, a stick or the mouse) and 'Axis2D' (two, bound "
						"separately per component).");
			}
			const bool paused = hasArg(args, "runWhilePaused")
			                  ? boolArg(args, "runWhilePaused", wasPaused) : wasPaused;

			if (!hasArg(args, "valueType") && !hasArg(args, "runWhilePaused"))
				return ToolResult::fail("invalid_payload",
					"Nothing to change: pass 'valueType', 'runWhilePaused' or both. "
					"input_actions reads the current values.");

			// One writer for the payload, shared with the panel — so a field the
			// loader expects cannot be dropped by a second hand-assembled JSON.
			const std::string payload = HE::makeInputActionJson(valueTypeName(vt), paused);

			const std::string name = HE::inputActionNameFromPath(d.rel);
			json out{
				{ "name",           name },
				{ "valueType",      valueTypeName(vt) },
				{ "runWhilePaused", paused },
				{ "events",         eventsOf(name, vt) },
			};

			// The contexts that bind this action, when the retype changed what they
			// have to look like. Read from the files, so nothing is loaded.
			if (vt != wasVt)
			{
				out["valueTypeWas"] = valueTypeName(wasVt);
				bool truncated = false;
				const auto ctxs = findInputAssets(*cm, HE::AssetType::InputMappingContext,
				                                  200, truncated);
				json affected = json::array();
				for (const auto& [rel, abs] : ctxs)
				{
					const Payload p = readPayload(abs);
					if (!p.ok) continue;
					std::vector<MapEntry> entries;
					decodeMapping(p.json, entries);
					for (const MapEntry& e : entries)
						if (e.actionPath == d.rel) { affected.push_back(rel); break; }
				}
				if (!affected.empty())
				{
					out["affectedMappings"] = std::move(affected);
					out["affectedMappingsNote"] =
						"These contexts bind this action and were written for a " +
						std::string(valueTypeName(wasVt)) + " one. Their bindings for it "
						"are now the wrong shape and the runtime will not read them — "
						"input_mappings shows what is there, input_mapping_unbind clears "
						"it and input_mapping_bind puts the right shape in.";
				}
			}

			return writePayload(*cm, *h, d, HE::AssetType::InputAction, payload, std::move(out));
		};
		registry.add(std::move(t));
	}

	// ── input_mappings ───────────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "input_mappings";
		t.description =
			"Read the project's Input Mapping Contexts. Without 'path' it lists them with "
			"the number of actions each one binds; with one it reports every entry: the "
			"action it binds, that action's value type, and the keys, gamepad buttons, "
			"mouse buttons and axis rows bound to it. Names the runtime cannot parse are "
			"reported as 'deadNames' — a binding can sit in the file and show up in the "
			"editor while doing nothing in the game, and this is the only place that says "
			"so.";
		t.inputSchema = objectSchema(json{
			{ "path",  stringProp("Report only this context, content-relative, e.g. "
			                      "'Input/IMC_Default.hasset'. Omit for all of them.") },
			{ "limit", json{ { "type", "integer" },
			                 { "description", "Most contexts to report when listing. "
			                                  "Default 200." } } },
		}, {});
		t.handler = [cm, h](const json& args) -> ToolResult {
			if (hasArg(args, "path"))
			{
				Doc d = openDoc(*cm, *h, args, HE::AssetType::InputMappingContext,
				                /*forWrite=*/false);
				if (!d.ok) return d.failure;

				std::vector<MapEntry> entries;
				decodeMapping(d.payload, entries);
				resolveEntryValueTypes(*cm, entries);

				json arr = json::array();
				for (const MapEntry& e : entries) arr.push_back(entryJson(e));
				json out{ { "path", d.rel }, { "entries", std::move(arr) } };
				if (d.openDirty) out["openInEditorUnsaved"] = true;
				return ToolResult::ok(std::move(out));
			}

			const int limit = std::max(1, intArg(args, "limit", 200));
			bool truncated = false;
			const auto found = findInputAssets(*cm, HE::AssetType::InputMappingContext,
			                                   limit, truncated);
			json arr = json::array();
			for (const auto& [rel, abs] : found)
			{
				const Payload p = readPayload(abs);
				if (!p.ok) continue;
				std::vector<MapEntry> entries;
				decodeMapping(p.json, entries);
				json j{ { "path", rel }, { "entryCount", (int)entries.size() } };
				if (h->isDirty && h->isDirty(rel)) j["openInEditorUnsaved"] = true;
				arr.push_back(std::move(j));
			}
			json out{ { "contexts", std::move(arr) } };
			if (truncated) out["truncated"] = true;
			return ToolResult::ok(std::move(out));
		};
		registry.add(std::move(t));
	}

	// ── input_mapping_bind ───────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "input_mapping_bind";
		t.description =
			"Bind something to an action in a mapping context, creating the context's "
			"entry for that action if it has none yet. Exactly one of 'key', "
			"'mouseButton', 'gamepadButton' and 'axis' per call, and WHICH one is legal "
			"is decided by the action: a Button action takes the three buttons, an Axis "
			"or Axis 2D action takes 'axis' rows. Names are checked against the same "
			"tables the runtime parses with, so a misspelling is refused here instead of "
			"becoming a binding that quietly does nothing — input_bindable lists them.";
		t.mutates     = true;
		t.inputSchema = objectSchema(json{
			{ "path",   stringProp("Content-relative path of the Input Mapping Context, "
			                       "e.g. 'Input/IMC_Default.hasset'.") },
			{ "action", actionPathProp() },
			{ "key",           stringProp("Keyboard key, SDL's own name: 'Space', 'W', "
			                              "'Left Shift', 'Escape'. Button actions only.") },
			{ "mouseButton",   stringProp("'left', 'right', 'middle', 'x1' or 'x2'. "
			                              "Button actions only.") },
			{ "gamepadButton", stringProp("Gamepad button, SDL's mapping name in "
			                              "Xbox-layout terms: 'a' (South), 'b', 'x', "
			                              "'y', 'dpup', 'leftshoulder', 'start'. Button "
			                              "actions only.") },
			{ "axis", json{
				{ "type", "object" },
				{ "description",
				  "One axis row, for an Axis or Axis 2D action. With source 'Key' (the "
				  "default) it is a PAIR: 'positive' and/or 'negative' key names, and/or "
				  "'positiveButton'/'negativeButton' gamepad button names — one side "
				  "alone is a legal one-way axis. With any other source it is a device "
				  "value and must carry NO keys or buttons, because the runtime reads a "
				  "row's pairs whatever the source says. 'scale' multiplies it (default "
				  "1; use -1 for an inverted axis, and note SDL's stick Y is positive "
				  "DOWNWARD)." },
				{ "properties", json{
					{ "source",         stringProp("'Key' (default), 'MouseX', 'MouseY', "
					                               "'MouseWheel', 'GamepadLeftX', "
					                               "'GamepadLeftY', 'GamepadRightX', "
					                               "'GamepadRightY', 'GamepadLeftTrigger' "
					                               "or 'GamepadRightTrigger'.") },
					{ "positive",       stringProp("Key name driving this axis positive.") },
					{ "negative",       stringProp("Key name driving it negative.") },
					{ "positiveButton", stringProp("Gamepad button driving it positive.") },
					{ "negativeButton", stringProp("Gamepad button driving it negative.") },
					{ "scale",          numberProp("Multiplier, default 1.") },
				} },
				{ "additionalProperties", false } } },
			{ "component", stringProp("'x' or 'y' — which dimension of an Axis 2D action "
			                          "this row belongs to. Required for those, and "
			                          "meaningless for the others: the two dimensions are "
			                          "bound as separate lists.") },
		}, { "path", "action" });
		t.handler = [cm, h](const json& args) -> ToolResult {
			Doc d = openDoc(*cm, *h, args, HE::AssetType::InputMappingContext,
			                /*forWrite=*/true);
			if (!d.ok) return d.failure;

			// The action first, and from its FILE: nothing is loaded until the write,
			// so no pointer can be moved out from under this call.
			const PathCheck ap = checkPath(*cm, strArg(args, "action"), /*mustExist=*/true,
			                               "action");
			if (!ap.ok) return ap.failure;
			const int vt = valueTypeOfActionFile(*cm, ap.rel);
			if (vt < 0)
				return ToolResult::fail("invalid_path",
					"'" + ap.rel + "' is not an Input Action asset, so there is nothing to "
					"bind to. input_actions names the ones this project has.");

			const bool hasKey  = hasArg(args, "key");
			const bool hasMb   = hasArg(args, "mouseButton");
			const bool hasPb   = hasArg(args, "gamepadButton");
			const bool hasAxis = hasArg(args, "axis");
			const int  given   = (int)hasKey + (int)hasMb + (int)hasPb + (int)hasAxis;
			if (given != 1)
				return ToolResult::fail("invalid_payload",
					given == 0 ? "Nothing to bind: pass exactly one of 'key', "
					             "'mouseButton', 'gamepadButton' or 'axis'."
					           : "Pass exactly ONE of 'key', 'mouseButton', "
					             "'gamepadButton' and 'axis' per call. Two bindings are "
					             "two calls, so a refusal can never leave one of them "
					             "applied.");

			// The action's value type decides which of the four is legal. The wrong
			// one would land in a list the runtime never reads for this action:
			// present, visible in the editor, dead.
			if (vt == kVtButton && hasAxis)
				return ToolResult::fail("invalid_payload",
					"'" + ap.rel + "' is a Button action, and 'axes' is not a list the "
					"runtime reads for one. Bind 'key', 'mouseButton' or 'gamepadButton' "
					"— or make it an Axis action first with input_action_set.");
			if (vt != kVtButton && !hasAxis)
				return ToolResult::fail("invalid_payload",
					"'" + ap.rel + "' is an " + std::string(valueTypeName(vt)) +
					" action, and 'keys' is not a list the runtime reads for one. Bind an "
					"'axis' row instead — { \"positive\": \"W\", \"negative\": \"S\" } for "
					"a key pair, { \"source\": \"GamepadLeftX\" } for a stick — or make it "
					"a Button action first with input_action_set.");

			std::string why;
			const bool yComponent = wantsYComponent(args, vt, why);
			if (!why.empty()) return ToolResult::fail("invalid_payload", why);

			// ── The value, validated before the file is touched ───────────────
			AxisRow row;
			std::string keyName, mbName, pbName;
			if (hasKey)
			{
				keyName = strArg(args, "key");
				if (!isKnownKey(keyName))
					return ToolResult::fail("invalid_payload",
						"'" + keyName + "' is not a key name the runtime knows, so binding "
						"it would do nothing. The names are SDL's own — 'Space', 'W', "
						"'Left Shift', 'Return' — and input_bindable lists all of them.");
			}
			else if (hasMb)
			{
				mbName = strArg(args, "mouseButton");
				if (!isKnownMouseButton(mbName))
					return ToolResult::fail("invalid_payload",
						"'" + mbName + "' is not a mouse button. The five are 'left', "
						"'right', 'middle', 'x1' and 'x2'.");
			}
			else if (hasPb)
			{
				pbName = strArg(args, "gamepadButton");
				if (!isKnownPadButton(pbName))
					return ToolResult::fail("invalid_payload",
						"'" + pbName + "' is not a gamepad button name, so binding it would "
						"do nothing. The names are SDL's mapping strings in Xbox-layout "
						"terms — 'a', 'b', 'x', 'y', 'dpup', 'leftshoulder', 'start' — and "
						"input_bindable lists them with the label each one shows as.");
			}
			else
			{
				const json& a = args["axis"];
				if (!a.is_object())
					return ToolResult::fail("invalid_payload",
						"'axis' is an object: { \"positive\": \"W\", \"negative\": \"S\" } "
						"for a key pair, { \"source\": \"MouseX\" } for a device value.");

				const std::string src = a.contains("source") && a["source"].is_string()
				                      ? a["source"].get<std::string>() : "Key";
				if (!isKnownAxisSource(src))
					return ToolResult::fail("invalid_payload",
						"'" + src + "' is not an axis source. The sources are: " +
						allAxisSourceNames() + ". 'Key' means the key/button pair in the "
						"same row.");
				row.source = HE::axisSourceFromName(src);
				row.scale  = (float)numArg(a, "scale", 1.0);

				struct Field { const char* name; std::string* into; bool pad; };
				std::string pos, neg, posB, negB;
				const Field fields[] = { { "positive", &pos,  false },
				                         { "negative", &neg,  false },
				                         { "positiveButton", &posB, true },
				                         { "negativeButton", &negB, true } };
				for (const Field& f : fields)
				{
					const std::string v = strArg(a, f.name);
					if (v.empty()) continue;
					if (f.pad ? !isKnownPadButton(v) : !isKnownKey(v))
						return ToolResult::fail("invalid_payload",
							"'" + v + "' (in '" + f.name + "') is not " +
							(f.pad ? "a gamepad button name" : "a key name") +
							" the runtime knows, so the row would be read with that half "
							"missing. input_bindable lists the names.");
					*f.into = v;
				}
				row.positive       = pos;
				row.negative       = neg;
				row.positiveButton = posB;
				row.negativeButton = negB;
				row.uiPadRow       = !posB.empty() || !negB.empty();

				const bool anyPair = !pos.empty() || !neg.empty() || !posB.empty() || !negB.empty();
				if (row.source == AxisSource::Key && !anyPair)
					return ToolResult::fail("invalid_payload",
						"A 'Key' axis row needs at least one of 'positive', 'negative', "
						"'positiveButton' or 'negativeButton' — the runtime drops a row "
						"that has none, so this one would vanish on load. One side alone is "
						"fine: that is a one-way axis.");
				// The runtime reads a Key row's pairs whatever the source says, so keys
				// left on a stick row keep binding invisibly. That is why the editor's
				// source combo clears them, and why this is a refusal rather than a
				// silent drop.
				if (row.source != AxisSource::Key && anyPair)
					return ToolResult::fail("invalid_payload",
						"A '" + src + "' row takes its value from the device, so it must "
						"not also carry keys or buttons: the runtime would keep reading "
						"those and the row would bind twice. Send the keys as their own "
						"row with source 'Key'.");
			}

			// ── Apply ─────────────────────────────────────────────────────────
			std::vector<MapEntry> entries;
			decodeMapping(d.payload, entries);
			resolveEntryValueTypes(*cm, entries);

			auto it = std::find_if(entries.begin(), entries.end(),
				[&](const MapEntry& e) { return e.actionPath == ap.rel; });
			const bool created = it == entries.end();
			if (created)
			{
				MapEntry e;
				e.actionPath = ap.rel;
				entries.push_back(std::move(e));
				it = entries.end() - 1;
			}
			// Set from the action even for an entry that already existed: it may have
			// been decoded before the action was retyped, and the encoder needs the
			// truth to choose between "axes" and "axesX".
			it->valueType = vt;

			const char* list = nullptr;
			int index = 0;
			if (hasKey)      { it->keys.push_back(keyName);           list = "keys";           index = (int)it->keys.size() - 1; }
			else if (hasMb)  { it->mouseButtons.push_back(mbName);    list = "mouseButtons";   index = (int)it->mouseButtons.size() - 1; }
			else if (hasPb)  { it->gamepadButtons.push_back(pbName);  list = "gamepadButtons"; index = (int)it->gamepadButtons.size() - 1; }
			else
			{
				std::vector<AxisRow>& into = yComponent ? it->axesY : it->axes;
				into.push_back(row);
				list  = yComponent ? "axesY" : (vt == kVtAxis2D ? "axesX" : "axes");
				index = (int)into.size() - 1;
			}

			json out{
				{ "action",       ap.rel },
				{ "actionName",   HE::inputActionNameFromPath(ap.rel) },
				{ "valueType",    valueTypeName(vt) },
				{ "entryCreated", created },
				{ "bound",        json{ { "list", list }, { "index", index } } },
				{ "entry",        entryJson(*it) },
			};
			return writePayload(*cm, *h, d, HE::AssetType::InputMappingContext,
			                    encodeMapping(entries), std::move(out));
		};
		registry.add(std::move(t));
	}

	// ── input_mapping_unbind ─────────────────────────────────────────────────
	{
		McpTool t;
		t.name        = "input_mapping_unbind";
		t.description =
			"Remove a binding from an action's entry in a mapping context, or the whole "
			"entry. Bindings are addressed by the list they are in and their index in it, "
			"which is what input_mappings reports for every one of them — and indices "
			"shift when something before them goes, so read the entry back (this call "
			"returns it) rather than removing two by index in a row.";
		t.mutates     = true;
		t.inputSchema = objectSchema(json{
			{ "path",   stringProp("Content-relative path of the Input Mapping Context.") },
			{ "action", actionPathProp() },
			{ "list",   stringProp("Which list to remove from: 'keys', 'mouseButtons', "
			                       "'gamepadButtons', 'axes' (an Axis action, or the X "
			                       "dimension of an Axis 2D one — 'axesX' is accepted for "
			                       "it too) or 'axesY'. Omit together with 'index' to "
			                       "remove the entire entry, every binding it holds "
			                       "included.") },
			{ "index",  json{ { "type", "integer" },
			                  { "description", "Zero-based position in that list, as "
			                                   "input_mappings reports it." } } },
		}, { "path", "action" });
		t.handler = [cm, h](const json& args) -> ToolResult {
			Doc d = openDoc(*cm, *h, args, HE::AssetType::InputMappingContext,
			                /*forWrite=*/true);
			if (!d.ok) return d.failure;

			// Not `mustExist`: an entry can outlive the action it names, and that
			// entry is exactly the one somebody wants to remove.
			const PathCheck ap = checkPath(*cm, strArg(args, "action"), /*mustExist=*/false,
			                               "action");
			if (!ap.ok) return ap.failure;

			std::vector<MapEntry> entries;
			decodeMapping(d.payload, entries);
			resolveEntryValueTypes(*cm, entries);

			const auto it = std::find_if(entries.begin(), entries.end(),
				[&](const MapEntry& e) { return e.actionPath == ap.rel; });
			if (it == entries.end())
			{
				std::string have;
				for (const MapEntry& e : entries)
					have += (have.empty() ? "" : ", ") + e.actionPath;
				return ToolResult::fail("not_found",
					"'" + d.rel + "' has no entry for '" + ap.rel + "'. " +
					(have.empty() ? "It binds nothing at all."
					              : "It binds: " + have + "."));
			}

			const std::string list = strArg(args, "list");
			if (list.empty() && !hasArg(args, "index"))
			{
				json out{ { "action", ap.rel }, { "removed", "entry" },
				          { "entryRemoved", true } };
				entries.erase(it);
				return writePayload(*cm, *h, d, HE::AssetType::InputMappingContext,
				                    encodeMapping(entries), std::move(out));
			}
			if (list.empty() || !hasArg(args, "index"))
				return ToolResult::fail("invalid_payload",
					"'list' and 'index' go together: both name one binding. Send neither "
					"to remove the whole entry.");

			const int index = intArg(args, "index", -1);
			// axesX is what input_mappings CALLS the X list of a 2D entry, so a
			// client reading its own answer back must be able to send that name.
			const std::string canonical = list == "axesX" ? "axes" : list;

			std::string removed;
			const auto eraseFrom = [&](std::vector<std::string>& v) -> bool {
				if (index < 0 || index >= (int)v.size()) return false;
				removed = v[(size_t)index];
				v.erase(v.begin() + index);
				return true;
			};
			const auto eraseAxis = [&](std::vector<AxisRow>& v) -> bool {
				if (index < 0 || index >= (int)v.size()) return false;
				removed = axisRowJson(v[(size_t)index]).dump();
				v.erase(v.begin() + index);
				return true;
			};

			bool done = false;
			int  size = -1;
			if      (canonical == "keys")           { size = (int)it->keys.size();           done = eraseFrom(it->keys); }
			else if (canonical == "mouseButtons")   { size = (int)it->mouseButtons.size();   done = eraseFrom(it->mouseButtons); }
			else if (canonical == "gamepadButtons") { size = (int)it->gamepadButtons.size(); done = eraseFrom(it->gamepadButtons); }
			else if (canonical == "axes")           { size = (int)it->axes.size();           done = eraseAxis(it->axes); }
			else if (canonical == "axesY")          { size = (int)it->axesY.size();          done = eraseAxis(it->axesY); }
			else
				return ToolResult::fail("invalid_payload",
					"'" + list + "' is not a binding list. They are 'keys', "
					"'mouseButtons', 'gamepadButtons', 'axes' (or 'axesX' for the X "
					"dimension of an Axis 2D action) and 'axesY'.");

			if (!done)
				return ToolResult::fail("not_found",
					"'" + d.rel + "' has no binding at index " + std::to_string(index) +
					" of '" + list + "' for '" + ap.rel + "': that list holds " +
					std::to_string(size) + ". input_mappings reports the indices.");

			json out{
				{ "action",     ap.rel },
				{ "removed",    removed },
				{ "removedFrom", json{ { "list", list }, { "index", index } } },
				{ "entry",      entryJson(*it) },
			};
			return writePayload(*cm, *h, d, HE::AssetType::InputMappingContext,
			                    encodeMapping(entries), std::move(out));
		};
		registry.add(std::move(t));
	}
}

} // namespace HE::Ed
