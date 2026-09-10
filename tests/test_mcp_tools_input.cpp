#include "doctest.h"

#include "AssetStubWriter.h"
#include "EditorAssetTypeCache.h"
#include "McpToolRegistry.h"
#include "TestFsUtil.h"

#include <Application/InputAssets.h>
#include <Application/InputMapping.h>
#include <ContentManager/Assets.h>
#include <ContentManager/ContentManager.h>

#include <SDL3/SDL.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// ─── Binding a key from outside the editor ───────────────────────────────────
// The one claim these tools make that nothing else in the codebase can make for
// them: A BINDING THEY WROTE IS A BINDING THE RUNTIME READS. That is not the
// same as "the JSON round-trips through our own decoder", because the runtime's
// reader — `applyInputMappingContext` — SKIPS every name it cannot parse and
// says nothing about it. A context whose keys are all misspelled loads clean,
// draws correctly in the panel and produces a game that ignores the keyboard.
//
// So the load-bearing tests here hand the file the tool wrote straight to that
// function, on a real `InputMapping`, and assert that it bound something. The
// rest are the places where a shortcut would look green and be wrong later:
//
//   • is a name really checked, and is a refusal really a NO-OP (the file byte
//     for byte as it was),
//   • does an axis row that became a stick really refuse to keep its keys — the
//     runtime reads a Key row's pairs whatever the source says, so those would
//     bind invisibly,
//   • does a 2D entry encode as axesX/axesY even with one list still empty, and
//     — the regression that is invisible until it bites — does an unrelated
//     entry in the same context SURVIVE a save that was about another one,
//   • does an unsaved editor tab really stop a write, and does a clean one
//     really get told to re-read the file,
//   • does every refusal arrive under a code a client can branch on.

using HE::Ed::McpInputHooks;
using HE::Ed::McpTool;
using HE::Ed::McpToolRegistry;
using HE::Ed::ToolResult;
using nlohmann::json;

namespace fs = std::filesystem;

namespace {

struct Fixture
{
	fs::path        root;
	ContentManager  content;
	McpToolRegistry registry;

	// The editor half, as flags a test sets.
	bool        playing = false;
	std::string lockedRel;   // non-empty = a peer holds it
	std::string dirtyRel;    // non-empty = an open tab holds it with edits
	std::string openRel;     // non-empty = an open tab holds it (clean or not)
	int         reloadCalls = 0;

	explicit Fixture(const std::string& name)
	{
		root = fs::temp_directory_path() /
		       ("he_test_mcp_input_" + name + "_" + std::to_string(::rand()));
		fs::create_directories(root);
		content.setContentRoot(root.string());
		EditorAssetTypeCache::invalidateAll();

		McpInputHooks h;
		h.isPlaying     = [this] { return playing; };
		h.lockedByOther = [this](const std::string& rel) {
			return !lockedRel.empty() && rel == lockedRel;
		};
		h.isDirty = [this](const std::string& rel) {
			return !dirtyRel.empty() && rel == dirtyRel;
		};
		h.reloadFromDisk = [this](const std::string& rel) {
			if (openRel.empty() || rel != openRel) return false;
			++reloadCalls;
			return true;
		};

		HE::Ed::registerInputTools(registry, content, std::move(h));
	}

	~Fixture()
	{
		EditorAssetTypeCache::invalidateAll();
		he_test::removeAllQuiet(root);
	}

	ToolResult call(const char* name, json args)
	{
		const McpTool* t = registry.find(name);
		REQUIRE(t != nullptr);
		return t->handler(args);
	}

	// An asset straight onto disk, bypassing the tools — the state a test starts
	// FROM. The stub writer is the editor's own, so what is in a newborn input
	// asset here is what is in one the create menu made.
	void writeStub(const std::string& rel, HE::AssetType type)
	{
		const fs::path abs = root / rel;
		fs::create_directories(abs.parent_path());
		REQUIRE(HE::Ed::writeAssetStub(abs.string(), rel, fs::path(rel).stem().string(), type));
		EditorAssetTypeCache::invalidate(abs.string());
	}

	// An Input Action of a given kind, without going through input_action_set —
	// so a test of the binder is not also a test of the setter.
	void writeAction(const std::string& rel, const char* valueType,
	                 bool runWhilePaused = false)
	{
		writeStub(rel, HE::AssetType::InputAction);
		const HE::UUID id = content.loadAsset(rel);
		REQUIRE_FALSE(id == HE::UUID{});
		InputActionAsset* a = content.getInputActionMutable(id);
		REQUIRE(a != nullptr);
		a->json = HE::makeInputActionJson(valueType, runWhilePaused);
		REQUIRE(content.saveAsset(*a));
	}

	// A mapping context carrying exactly this payload — including one no tool
	// would ever produce, which is how the "dead name" reporting and the
	// sibling-entry regression are set up.
	void writeMapping(const std::string& rel, const std::string& payload = {})
	{
		writeStub(rel, HE::AssetType::InputMappingContext);
		if (payload.empty()) return;
		const HE::UUID id = content.loadAsset(rel);
		REQUIRE_FALSE(id == HE::UUID{});
		InputMappingContextAsset* m = content.getInputMappingContextMutable(id);
		REQUIRE(m != nullptr);
		m->json = payload;
		REQUIRE(content.saveAsset(*m));
	}

	// What is REALLY in the file. Read through a content manager of its own: the
	// fixture's has the asset loaded, and an answer out of its cache would pass
	// for a write that never happened.
	std::string payloadFromDisk(const std::string& rel) const
	{
		ContentManager fresh;
		fresh.setContentRoot(root.string());
		const HE::UUID id = fresh.loadAsset(rel);
		if (id == HE::UUID{}) return {};
		if (const InputActionAsset* a = fresh.getInputAction(id)) return a->json;
		if (const InputMappingContextAsset* m = fresh.getInputMappingContext(id)) return m->json;
		return {};
	}

	std::string fileBytes(const std::string& rel) const
	{
		std::ifstream f((root / rel).string(), std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	}
};

// The runtime's own reader, on the file the tool wrote. Returns the number of
// binding GROUPS it took — 0 means "the game ignores this context".
size_t wouldBind(const std::string& payload)
{
	InputMapping mapping;
	return HE::applyInputMappingContext(mapping, payload);
}

} // namespace

// ─── The vocabulary is answerable without a window ──────────────────────────
// Every check in this file rests on SDL's name tables being readable with no
// SDL_Init behind them: the editor only ever calls them inside a running app.
// If that ever stops holding, it must fail HERE and not as six confusing
// refusals further down.
TEST_CASE("mcp input: SDL name tables answer without SDL_Init")
{
	CHECK(SDL_GetScancodeFromName("Space") != SDL_SCANCODE_UNKNOWN);
	CHECK(SDL_GetScancodeFromName("Spacebar") == SDL_SCANCODE_UNKNOWN);
	CHECK(SDL_GetGamepadButtonFromString("dpup") != SDL_GAMEPAD_BUTTON_INVALID);
	CHECK(SDL_GetGamepadButtonFromString("nonsense") == SDL_GAMEPAD_BUTTON_INVALID);
	const char* w = SDL_GetScancodeName(SDL_SCANCODE_W);
	REQUIRE(w != nullptr);
	CHECK(std::string(w) == "W");
}

TEST_CASE("mcp input: the six tools register with legal names and honest mutates")
{
	Fixture f("registry");
	const char* readers[] = { "input_actions", "input_mappings", "input_bindable" };
	const char* writers[] = { "input_action_set", "input_mapping_bind",
	                          "input_mapping_unbind" };
	for (const char* n : readers)
	{
		const McpTool* t = f.registry.find(n);
		REQUIRE(t != nullptr);
		CHECK(McpToolRegistry::enforceNameRule(t->name));
		CHECK_FALSE(t->mutates);
		CHECK(t->inputSchema.is_object());
		CHECK_FALSE(t->description.empty());
	}
	for (const char* n : writers)
	{
		const McpTool* t = f.registry.find(n);
		REQUIRE(t != nullptr);
		CHECK(McpToolRegistry::enforceNameRule(t->name));
		CHECK(t->mutates);
		CHECK(t->inputSchema.is_object());
	}
}

// ─── input_bindable ─────────────────────────────────────────────────────────

TEST_CASE("mcp input: input_bindable lists what the runtime can actually parse")
{
	Fixture f("bindable");
	const ToolResult r = f.call("input_bindable", json::object());
	REQUIRE_FALSE(r.isError);
	const json& c = r.content;

	REQUIRE(c["keys"].is_array());
	// Not a spot check of names we like: every name offered has to come back out
	// of the loader's own parser, or the list is a list of lies.
	CHECK(c["keys"].size() > 100);
	for (const json& k : c["keys"])
		REQUIRE(SDL_GetScancodeFromName(k.get<std::string>().c_str()) != SDL_SCANCODE_UNKNOWN);
	for (const json& b : c["gamepadButtons"])
		REQUIRE(SDL_GetGamepadButtonFromString(b["name"].get<std::string>().c_str()) !=
		        SDL_GAMEPAD_BUTTON_INVALID);
	for (const json& m : c["mouseButtons"])
		REQUIRE(HE::mouseButtonFromName(m["name"].get<std::string>()) >= 0);
	for (const json& s : c["axisSources"])
		REQUIRE(HE::axisSourceName(HE::axisSourceFromName(s["name"].get<std::string>())) ==
		        s["name"].get<std::string>());

	// The pad's stored name and its label are different strings on purpose: "a"
	// is what the file holds, "A (South)" is what a person recognises.
	bool sawSouth = false;
	for (const json& b : c["gamepadButtons"])
		if (b["name"] == "a") { sawSouth = true; CHECK(b["label"] == "A (South)"); }
	CHECK(sawSouth);

	CHECK(c["axisSources"].size() == 10);   // Key + 3 mouse + 6 gamepad
	bool sawDelta = false, sawHeld = false;
	for (const json& s : c["axisSources"])
	{
		if (s["name"] == "MouseX")       { sawDelta = true; CHECK(s["isDelta"] == true); }
		if (s["name"] == "GamepadLeftX") { sawHeld  = true; CHECK(s["isDelta"] == false); }
		if (s["name"] == "Key")          CHECK(s["needsKeys"] == true);
	}
	CHECK(sawDelta);
	CHECK(sawHeld);
}

TEST_CASE("mcp input: input_bindable's filter narrows the key list only")
{
	Fixture f("bindable_filter");
	const ToolResult r = f.call("input_bindable", json{ { "filter", "Shift" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["keys"].size() >= 2);           // Left Shift, Right Shift
	for (const json& k : r.content["keys"])
		CHECK(k.get<std::string>().find("Shift") != std::string::npos);
	// The short lists stay complete — a filter is about the 240 keys.
	CHECK(r.content["mouseButtons"].size() == 5);
	CHECK(r.content["axisSources"].size() == 10);
}

// ─── input_actions ──────────────────────────────────────────────────────────

TEST_CASE("mcp input: input_actions reports the events an action ACTUALLY fires")
{
	Fixture f("actions_events");
	f.writeAction("Input/IA_Jump.hasset", "Button");
	f.writeAction("Input/IA_Move.hasset", "Axis2D");
	f.writeAction("Input/IA_Look.hasset", "Axis", /*runWhilePaused=*/true);

	// A Button fires Pressed/Released and nothing else — PlayerHost::tick has one
	// case per kind, so offering all four names would invite a handler that can
	// never run.
	ToolResult r = f.call("input_actions", json{ { "path", "Input/IA_Jump.hasset" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["name"] == "IA_Jump");
	CHECK(r.content["valueType"] == "Button");
	CHECK(r.content["runWhilePaused"] == false);
	CHECK(r.content["events"]["pressed"] == "Input.IA_Jump.Pressed");
	CHECK(r.content["events"]["released"] == "Input.IA_Jump.Released");
	CHECK_FALSE(r.content["events"].contains("axis"));

	r = f.call("input_actions", json{ { "path", "Input/IA_Move.hasset" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["valueType"] == "Axis2D");
	// A 2D action gets its OWN event name, not a Vec2 under ".Axis" — a stale 1D
	// handler must stop firing rather than be handed the wrong type.
	CHECK(r.content["events"]["axis2D"] == "Input.IA_Move.Axis2D");
	CHECK_FALSE(r.content["events"].contains("axis"));

	r = f.call("input_actions", json{ { "path", "Input/IA_Look.hasset" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["valueType"] == "Axis");
	CHECK(r.content["runWhilePaused"] == true);
	CHECK(r.content["events"]["axis"] == "Input.IA_Look.Axis");
}

TEST_CASE("mcp input: input_actions lists the project and names the stem collisions")
{
	Fixture f("actions_list");
	f.writeAction("Input/IA_Fire.hasset", "Button");
	f.writeAction("Input/Vehicle/IA_Fire.hasset", "Axis");
	f.writeAction("Input/IA_Jump.hasset", "Button");
	// Neither of these is an Input Action, and neither may show up.
	f.writeMapping("Input/IMC_Default.hasset");
	f.writeStub("UI/Menu.hasset", HE::AssetType::Widget);

	const ToolResult r = f.call("input_actions", json::object());
	REQUIRE_FALSE(r.isError);
	REQUIRE(r.content["actions"].is_array());
	CHECK(r.content["actions"].size() == 3);

	// Events and bindings key on the file STEM, not the path, so two IA_Fire in
	// different folders are ONE action to the runtime. No other tool is in a
	// position to notice, which is why this reader says it.
	REQUIRE(r.content.contains("duplicateNames"));
	CHECK(r.content["duplicateNames"].size() == 1);
	CHECK(r.content["duplicateNames"][0] == "IA_Fire");
	CHECK(r.content.contains("duplicateNamesNote"));
}

TEST_CASE("mcp input: input_actions refuses a path that holds something else")
{
	Fixture f("actions_wrongtype");
	f.writeMapping("Input/IMC_Default.hasset");
	f.writeStub("UI/Menu.hasset", HE::AssetType::Widget);

	ToolResult r = f.call("input_actions", json{ { "path", "Input/IMC_Default.hasset" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_path");
	// The refusal has to say where to look instead, or a client with the wrong
	// path has nowhere to go.
	CHECK(r.errorMessage.find("input_mappings") != std::string::npos);

	r = f.call("input_actions", json{ { "path", "UI/Menu.hasset" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_path");

	r = f.call("input_actions", json{ { "path", "Input/Nope.hasset" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "not_found");

	// The confinement rule, which is one function and has to keep holding here.
	r = f.call("input_actions", json{ { "path", "../../secrets.hasset" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_path");
	r = f.call("input_actions", json{ { "path", "/etc/passwd" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_path");
}

// ─── input_action_set ───────────────────────────────────────────────────────

TEST_CASE("mcp input: input_action_set reaches the disk and reports the new events")
{
	Fixture f("action_set");
	f.writeAction("Input/IA_Move.hasset", "Button");

	const ToolResult r = f.call("input_action_set", json{
		{ "path", "Input/IA_Move.hasset" },
		{ "valueType", "Axis2D" },
		{ "runWhilePaused", true } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["valueType"] == "Axis2D");
	CHECK(r.content["valueTypeWas"] == "Button");
	CHECK(r.content["runWhilePaused"] == true);
	CHECK(r.content["events"]["axis2D"] == "Input.IA_Move.Axis2D");

	// Asked of a SECOND content manager, so an answer out of the first one's
	// cache cannot pass for a write.
	const std::string onDisk = f.payloadFromDisk("Input/IA_Move.hasset");
	CHECK(HE::inputActionIsAxis2D(onDisk));
	CHECK(HE::inputActionRunsWhilePaused(onDisk));
	CHECK_FALSE(HE::inputActionIsAxis(onDisk));   // deliberately false for a 2D action
}

TEST_CASE("mcp input: input_action_set keeps what it was not asked to change")
{
	Fixture f("action_set_partial");
	f.writeAction("Input/IA_Pause.hasset", "Button", /*runWhilePaused=*/true);

	const ToolResult r = f.call("input_action_set", json{
		{ "path", "Input/IA_Pause.hasset" }, { "valueType", "Axis" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["runWhilePaused"] == true);
	CHECK(HE::inputActionRunsWhilePaused(f.payloadFromDisk("Input/IA_Pause.hasset")));
}

TEST_CASE("mcp input: input_action_set refuses garbage and an empty call, and touches nothing")
{
	Fixture f("action_set_refuse");
	f.writeAction("Input/IA_Jump.hasset", "Button");
	const std::string before = f.fileBytes("Input/IA_Jump.hasset");

	ToolResult r = f.call("input_action_set", json{
		{ "path", "Input/IA_Jump.hasset" }, { "valueType", "axis" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_payload");
	// "axis" would have gone straight into the payload and read back as a Button.
	CHECK(f.fileBytes("Input/IA_Jump.hasset") == before);

	r = f.call("input_action_set", json{ { "path", "Input/IA_Jump.hasset" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_payload");
	CHECK(f.fileBytes("Input/IA_Jump.hasset") == before);
}

TEST_CASE("mcp input: input_action_set names the mappings a retype just broke")
{
	Fixture f("action_set_affected");
	f.writeAction("Input/IA_Jump.hasset", "Button");
	f.writeAction("Input/IA_Fire.hasset", "Button");
	f.writeMapping("Input/IMC_Default.hasset");
	f.writeMapping("Input/IMC_Other.hasset");
	REQUIRE_FALSE(f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" }, { "key", "Space" } }).isError);
	REQUIRE_FALSE(f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Other.hasset" },
		{ "action", "Input/IA_Fire.hasset" }, { "key", "F" } }).isError);

	const ToolResult r = f.call("input_action_set", json{
		{ "path", "Input/IA_Jump.hasset" }, { "valueType", "Axis" } });
	REQUIRE_FALSE(r.isError);
	REQUIRE(r.content.contains("affectedMappings"));
	CHECK(r.content["affectedMappings"].size() == 1);
	CHECK(r.content["affectedMappings"][0] == "Input/IMC_Default.hasset");

	// Only when the retype actually changed something: the same call again is a
	// no-op and must not raise an alarm.
	const ToolResult again = f.call("input_action_set", json{
		{ "path", "Input/IA_Jump.hasset" }, { "valueType", "Axis" } });
	REQUIRE_FALSE(again.isError);
	CHECK_FALSE(again.content.contains("affectedMappings"));
}

// ─── input_mapping_bind: does the RUNTIME read it ────────────────────────────

TEST_CASE("mcp input: a bound key is a binding the runtime's own reader takes")
{
	Fixture f("bind_runtime");
	f.writeAction("Input/IA_Jump.hasset", "Button");
	f.writeMapping("Input/IMC_Default.hasset");

	const ToolResult r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" },
		{ "key", "Space" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["entryCreated"] == true);
	CHECK(r.content["valueType"] == "Button");
	CHECK(r.content["bound"]["list"] == "keys");
	CHECK(r.content["bound"]["index"] == 0);

	// THE test. `applyInputMappingContext` is what the game runs, and it skips a
	// name it cannot parse without a word — so a payload our own decoder is happy
	// with proves nothing at all.
	const std::string onDisk = f.payloadFromDisk("Input/IMC_Default.hasset");
	CHECK(wouldBind(onDisk) == 1);

	// A second binding on the same action joins the entry rather than making a
	// second one.
	const ToolResult r2 = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" },
		{ "gamepadButton", "a" } });
	REQUIRE_FALSE(r2.isError);
	CHECK(r2.content["entryCreated"] == false);
	CHECK(r2.content["bound"]["list"] == "gamepadButtons");
	const ToolResult read = f.call("input_mappings",
	                               json{ { "path", "Input/IMC_Default.hasset" } });
	REQUIRE_FALSE(read.isError);
	REQUIRE(read.content["entries"].size() == 1);
	CHECK(read.content["entries"][0]["keys"].size() == 1);
	CHECK(read.content["entries"][0]["gamepadButtons"][0] == "a");
	CHECK(wouldBind(f.payloadFromDisk("Input/IMC_Default.hasset")) == 1);
}

TEST_CASE("mcp input: an axis row the runtime takes, on all three shapes")
{
	Fixture f("bind_axis");
	f.writeAction("Input/IA_Look.hasset", "Axis");
	f.writeMapping("Input/IMC_Default.hasset");

	// A key pair.
	REQUIRE_FALSE(f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Look.hasset" },
		{ "axis", json{ { "positive", "D" }, { "negative", "A" } } } }).isError);
	CHECK(wouldBind(f.payloadFromDisk("Input/IMC_Default.hasset")) == 1);

	// A one-sided pad-button row — a legal one-way axis.
	REQUIRE_FALSE(f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Look.hasset" },
		{ "axis", json{ { "positiveButton", "dpright" } } } }).isError);

	// A device value, with a scale.
	const ToolResult r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Look.hasset" },
		{ "axis", json{ { "source", "GamepadLeftY" }, { "scale", -1.0 } } } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["bound"]["list"] == "axes");
	CHECK(r.content["bound"]["index"] == 2);

	const ToolResult read = f.call("input_mappings",
	                               json{ { "path", "Input/IMC_Default.hasset" } });
	REQUIRE_FALSE(read.isError);
	const json& axes = read.content["entries"][0]["axes"];
	REQUIRE(axes.size() == 3);
	CHECK(axes[0]["source"] == "Key");
	CHECK(axes[0]["positive"] == "D");
	CHECK(axes[0]["negative"] == "A");
	CHECK(axes[1]["positiveButton"] == "dpright");
	CHECK(axes[2]["source"] == "GamepadLeftY");
	CHECK(axes[2]["scale"].get<double>() == doctest::Approx(-1.0));
	// All three rows survive the runtime's reader, which drops a Key row that has
	// neither a key nor a button.
	CHECK(wouldBind(f.payloadFromDisk("Input/IMC_Default.hasset")) == 1);
}

TEST_CASE("mcp input: a 2D action writes axesX/axesY even with one side still empty")
{
	Fixture f("bind_2d");
	f.writeAction("Input/IA_Move.hasset", "Axis2D");
	f.writeMapping("Input/IMC_Default.hasset");

	const ToolResult r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Move.hasset" },
		{ "component", "x" },
		{ "axis", json{ { "positive", "D" }, { "negative", "A" } } } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["bound"]["list"] == "axesX");

	// The regression this exists for: "axes" would register a ONE-dimensional
	// mapping and axis2DValue() would answer 0,0 forever. The Y list is still
	// empty and it must STILL be axesX.
	std::string onDisk = f.payloadFromDisk("Input/IMC_Default.hasset");
	CHECK(onDisk.find("axesX") != std::string::npos);
	CHECK(onDisk.find("\"axes\"") == std::string::npos);
	CHECK(wouldBind(onDisk) == 1);

	REQUIRE_FALSE(f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Move.hasset" },
		{ "component", "y" },
		{ "axis", json{ { "source", "GamepadLeftY" }, { "scale", -1.0 } } } }).isError);
	onDisk = f.payloadFromDisk("Input/IMC_Default.hasset");
	CHECK(onDisk.find("axesY") != std::string::npos);
	CHECK(onDisk.find("\"axes\"") == std::string::npos);
	CHECK(wouldBind(onDisk) == 1);

	const ToolResult read = f.call("input_mappings",
	                               json{ { "path", "Input/IMC_Default.hasset" } });
	REQUIRE_FALSE(read.isError);
	CHECK(read.content["entries"][0]["valueType"] == "Axis2D");
	CHECK(read.content["entries"][0]["axesX"].size() == 1);
	CHECK(read.content["entries"][0]["axesY"].size() == 1);
}

TEST_CASE("mcp input: a 2D action demands to know which component")
{
	Fixture f("bind_2d_component");
	f.writeAction("Input/IA_Move.hasset", "Axis2D");
	f.writeMapping("Input/IMC_Default.hasset");
	const std::string before = f.fileBytes("Input/IMC_Default.hasset");

	ToolResult r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Move.hasset" },
		{ "axis", json{ { "positive", "D" } } } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_payload");
	CHECK(r.errorMessage.find("component") != std::string::npos);
	CHECK(f.fileBytes("Input/IMC_Default.hasset") == before);

	r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Move.hasset" }, { "component", "z" },
		{ "axis", json{ { "positive", "D" } } } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_payload");

	// And on a one-dimensional action it is meaningless rather than ignored.
	f.writeAction("Input/IA_Look.hasset", "Axis");
	r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Look.hasset" }, { "component", "y" },
		{ "axis", json{ { "positive", "D" } } } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_payload");
}

// ─── The regression that is invisible until it bites ────────────────────────

TEST_CASE("mcp input: a save about one entry does not downgrade another entry's 2D shape")
{
	Fixture f("bind_sibling_2d");
	f.writeAction("Input/IA_Move.hasset", "Axis2D");
	f.writeAction("Input/IA_Jump.hasset", "Button");
	// A context as it exists on somebody's disk: a 2D entry whose Y list was
	// never filled in. `decodeMapping` leaves valueType at -1 for it, and the
	// encoder's fallback shape rule would then write "axes" — a one-dimensional
	// mapping, and axis2DValue() answering 0,0 forever.
	f.writeMapping("Input/IMC_Default.hasset",
		R"({"entries":[{"action":"Input/IA_Move.hasset",)"
		R"("axesX":[{"positive":"D","negative":"A","scale":1.0,"source":"Key"}]}]})");
	REQUIRE(f.payloadFromDisk("Input/IMC_Default.hasset").find("axesX") != std::string::npos);

	// A call about a completely different action.
	REQUIRE_FALSE(f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" }, { "key", "Space" } }).isError);

	const std::string onDisk = f.payloadFromDisk("Input/IMC_Default.hasset");
	CHECK(onDisk.find("axesX") != std::string::npos);
	CHECK(onDisk.find("\"axes\"") == std::string::npos);
	// Two groups: the 2D axis mapping and the new button one.
	CHECK(wouldBind(onDisk) == 2);
}

// ─── Validation is the load-bearing half ────────────────────────────────────

TEST_CASE("mcp input: a misspelled name is refused, and the file is untouched")
{
	Fixture f("bind_badnames");
	f.writeAction("Input/IA_Jump.hasset", "Button");
	f.writeAction("Input/IA_Look.hasset", "Axis");
	f.writeMapping("Input/IMC_Default.hasset");
	const std::string before = f.fileBytes("Input/IMC_Default.hasset");

	// Each refusal has to be recoverable from on its own: the two long name
	// spaces point at input_bindable, and the five mouse buttons are short enough
	// that the message simply lists them.
	struct Bad { json arg; const char* mustSay; };
	const Bad bad[] = {
		{ json{ { "key", "Spacebar" } },        "input_bindable" }, // the classic
		{ json{ { "gamepadButton", "cross" } }, "input_bindable" }, // SDL's are Xbox letters
		{ json{ { "mouseButton", "Left" } },    "'left'" },         // the names are lower case
	};
	for (const Bad& b : bad)
	{
		json args{ { "path", "Input/IMC_Default.hasset" },
		           { "action", "Input/IA_Jump.hasset" } };
		args.update(b.arg);
		const ToolResult r = f.call("input_mapping_bind", args);
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_payload");
		CHECK(r.errorMessage.find(b.mustSay) != std::string::npos);
	}

	// Inside an axis row too — every one of the four fields.
	const char* fields[] = { "positive", "negative" };
	for (const char* fld : fields)
	{
		const ToolResult r = f.call("input_mapping_bind", json{
			{ "path", "Input/IMC_Default.hasset" },
			{ "action", "Input/IA_Look.hasset" },
			{ "axis", json{ { fld, "Spacebar" } } } });
		CHECK(r.isError);
		CHECK(r.errorCode == "invalid_payload");
	}
	const ToolResult padRow = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Look.hasset" },
		{ "axis", json{ { "positiveButton", "cross" } } } });
	CHECK(padRow.isError);
	const ToolResult badSource = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Look.hasset" },
		// `axisSourceFromName` answers Key for garbage "never as an error", which
		// is right for the loader and would be a silent lie here.
		{ "axis", json{ { "source", "MouseZ" } } } });
	CHECK(badSource.isError);
	CHECK(badSource.errorCode == "invalid_payload");

	CHECK(f.fileBytes("Input/IMC_Default.hasset") == before);
}

TEST_CASE("mcp input: the wrong binding shape for the action is refused with the right one")
{
	Fixture f("bind_wrongshape");
	f.writeAction("Input/IA_Jump.hasset", "Button");
	f.writeAction("Input/IA_Look.hasset", "Axis");
	f.writeMapping("Input/IMC_Default.hasset");
	const std::string before = f.fileBytes("Input/IMC_Default.hasset");

	// An axis row on a Button action would land in a list `mapAction` never
	// reads: present in the file, visible in the editor, dead.
	ToolResult r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" },
		{ "axis", json{ { "positive", "D" } } } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_payload");
	CHECK(r.errorMessage.find("input_action_set") != std::string::npos);

	// And a key on an Axis action, the other way round.
	r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Look.hasset" }, { "key", "Space" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_payload");
	CHECK(r.errorMessage.find("axis") != std::string::npos);

	CHECK(f.fileBytes("Input/IMC_Default.hasset") == before);
}

TEST_CASE("mcp input: a device row must not keep keys, and a key row needs one")
{
	Fixture f("bind_rowrules");
	f.writeAction("Input/IA_Look.hasset", "Axis");
	f.writeMapping("Input/IMC_Default.hasset");
	const std::string before = f.fileBytes("Input/IMC_Default.hasset");

	// The runtime reads a Key row's pairs WHATEVER the source says, so keys on a
	// stick row keep binding invisibly. The editor's source combo clears them;
	// here it is a refusal, because silently dropping half a client's request is
	// the thing this interface must not do.
	ToolResult r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Look.hasset" },
		{ "axis", json{ { "source", "GamepadLeftX" }, { "positive", "D" } } } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_payload");

	// And an empty Key row is dropped by the loader, so it would vanish from a
	// file it was reported as written to.
	r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Look.hasset" },
		{ "axis", json{ { "scale", 2.0 } } } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_payload");

	CHECK(f.fileBytes("Input/IMC_Default.hasset") == before);
}

TEST_CASE("mcp input: exactly one binding per call")
{
	Fixture f("bind_arity");
	f.writeAction("Input/IA_Jump.hasset", "Button");
	f.writeMapping("Input/IMC_Default.hasset");
	const std::string before = f.fileBytes("Input/IMC_Default.hasset");

	ToolResult r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_payload");

	r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" },
		{ "key", "Space" }, { "gamepadButton", "a" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_payload");
	CHECK(f.fileBytes("Input/IMC_Default.hasset") == before);
}

TEST_CASE("mcp input: binding to something that is not an action is refused")
{
	Fixture f("bind_badaction");
	f.writeMapping("Input/IMC_Default.hasset");
	f.writeStub("UI/Menu.hasset", HE::AssetType::Widget);

	ToolResult r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "UI/Menu.hasset" }, { "key", "Space" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_path");

	r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/Gone.hasset" }, { "key", "Space" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "not_found");
}

// ─── input_mappings ─────────────────────────────────────────────────────────

TEST_CASE("mcp input: input_mappings reports what the runtime will SKIP")
{
	Fixture f("mappings_dead");
	f.writeAction("Input/IA_Jump.hasset", "Button");
	// A hand-edited context, which is a real thing to find on a disk: two of
	// these three names do not exist and the runtime will drop them without a
	// word. The panel would show all three.
	f.writeMapping("Input/IMC_Default.hasset",
		R"({"entries":[{"action":"Input/IA_Jump.hasset",)"
		R"("keys":["Space","Spacebar"],"mouseButtons":["Left"]}]})");

	const ToolResult r = f.call("input_mappings",
	                            json{ { "path", "Input/IMC_Default.hasset" } });
	REQUIRE_FALSE(r.isError);
	REQUIRE(r.content["entries"].size() == 1);
	const json& e = r.content["entries"][0];
	CHECK(e["actionName"] == "IA_Jump");
	CHECK(e["valueType"] == "Button");
	REQUIRE(e.contains("deadNames"));
	CHECK(e["deadNames"].size() == 2);

	// A context whose action was deleted is a state worth naming, not a crash.
	f.writeMapping("Input/IMC_Orphan.hasset",
		R"({"entries":[{"action":"Input/IA_Gone.hasset","keys":["Space"]}]})");
	const ToolResult o = f.call("input_mappings",
	                            json{ { "path", "Input/IMC_Orphan.hasset" } });
	REQUIRE_FALSE(o.isError);
	CHECK(o.content["entries"][0]["actionMissing"] == true);
	CHECK(o.content["entries"][0]["valueType"] == "unresolved");
}

TEST_CASE("mcp input: input_mappings lists the contexts and only the contexts")
{
	Fixture f("mappings_list");
	f.writeAction("Input/IA_Jump.hasset", "Button");
	f.writeMapping("Input/IMC_Default.hasset");
	f.writeMapping("Input/IMC_Menu.hasset");
	REQUIRE_FALSE(f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" }, { "key", "Space" } }).isError);

	const ToolResult r = f.call("input_mappings", json::object());
	REQUIRE_FALSE(r.isError);
	REQUIRE(r.content["contexts"].size() == 2);
	for (const json& c : r.content["contexts"])
	{
		if (c["path"] == "Input/IMC_Default.hasset") CHECK(c["entryCount"] == 1);
		if (c["path"] == "Input/IMC_Menu.hasset")    CHECK(c["entryCount"] == 0);
	}
}

// ─── input_mapping_unbind ───────────────────────────────────────────────────

TEST_CASE("mcp input: input_mapping_unbind takes one binding out by list and index")
{
	Fixture f("unbind_one");
	f.writeAction("Input/IA_Jump.hasset", "Button");
	f.writeMapping("Input/IMC_Default.hasset");
	for (const char* k : { "Space", "Return", "K" })
		REQUIRE_FALSE(f.call("input_mapping_bind", json{
			{ "path", "Input/IMC_Default.hasset" },
			{ "action", "Input/IA_Jump.hasset" }, { "key", k } }).isError);

	const ToolResult r = f.call("input_mapping_unbind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" }, { "list", "keys" }, { "index", 1 } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["removed"] == "Return");
	// The entry comes back so a client does not have to guess how the indices
	// shifted underneath it.
	REQUIRE(r.content["entry"]["keys"].size() == 2);
	CHECK(r.content["entry"]["keys"][0] == "Space");
	CHECK(r.content["entry"]["keys"][1] == "K");
	CHECK(wouldBind(f.payloadFromDisk("Input/IMC_Default.hasset")) == 1);
}

TEST_CASE("mcp input: input_mapping_unbind removes a whole entry, and reaches the disk")
{
	Fixture f("unbind_entry");
	f.writeAction("Input/IA_Jump.hasset", "Button");
	f.writeAction("Input/IA_Fire.hasset", "Button");
	f.writeMapping("Input/IMC_Default.hasset");
	REQUIRE_FALSE(f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" }, { "key", "Space" } }).isError);
	REQUIRE_FALSE(f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Fire.hasset" }, { "mouseButton", "left" } }).isError);
	REQUIRE(wouldBind(f.payloadFromDisk("Input/IMC_Default.hasset")) == 2);

	const ToolResult r = f.call("input_mapping_unbind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["entryRemoved"] == true);
	CHECK(wouldBind(f.payloadFromDisk("Input/IMC_Default.hasset")) == 1);

	const ToolResult read = f.call("input_mappings",
	                               json{ { "path", "Input/IMC_Default.hasset" } });
	REQUIRE(read.content["entries"].size() == 1);
	CHECK(read.content["entries"][0]["action"] == "Input/IA_Fire.hasset");
}

TEST_CASE("mcp input: input_mapping_unbind can clear an entry whose action is gone")
{
	Fixture f("unbind_orphan");
	// An entry can outlive the action it names, and that entry is exactly the one
	// somebody wants gone — so the action path must NOT have to exist.
	f.writeMapping("Input/IMC_Default.hasset",
		R"({"entries":[{"action":"Input/IA_Gone.hasset","keys":["Space"]}]})");
	const ToolResult r = f.call("input_mapping_unbind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Gone.hasset" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["entryRemoved"] == true);
	CHECK(f.payloadFromDisk("Input/IMC_Default.hasset").find("IA_Gone") == std::string::npos);
}

TEST_CASE("mcp input: input_mapping_unbind refuses what it cannot address")
{
	Fixture f("unbind_refuse");
	f.writeAction("Input/IA_Jump.hasset", "Button");
	f.writeMapping("Input/IMC_Default.hasset");
	REQUIRE_FALSE(f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" }, { "key", "Space" } }).isError);
	const std::string before = f.fileBytes("Input/IMC_Default.hasset");

	ToolResult r = f.call("input_mapping_unbind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" }, { "list", "keys" }, { "index", 7 } });
	CHECK(r.isError);
	CHECK(r.errorCode == "not_found");

	r = f.call("input_mapping_unbind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" }, { "list", "buttons" }, { "index", 0 } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_payload");

	// A list without an index is an address that names two different things.
	r = f.call("input_mapping_unbind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" }, { "list", "keys" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "invalid_payload");

	r = f.call("input_mapping_unbind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Fire.hasset" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "not_found");
	// The refusal names what IS bound, or a client whose paths drifted cannot
	// recover.
	CHECK(r.errorMessage.find("IA_Jump") != std::string::npos);

	CHECK(f.fileBytes("Input/IMC_Default.hasset") == before);
}

TEST_CASE("mcp input: input_mapping_unbind accepts axesX, which is the name it reported")
{
	Fixture f("unbind_axesx");
	f.writeAction("Input/IA_Move.hasset", "Axis2D");
	f.writeMapping("Input/IMC_Default.hasset");
	REQUIRE_FALSE(f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Move.hasset" }, { "component", "x" },
		{ "axis", json{ { "positive", "D" } } } }).isError);

	const ToolResult r = f.call("input_mapping_unbind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Move.hasset" }, { "list", "axesX" }, { "index", 0 } });
	REQUIRE_FALSE(r.isError);
	CHECK_FALSE(r.content["entry"].contains("axesX"));
}

// ─── The editor's own state ─────────────────────────────────────────────────

TEST_CASE("mcp input: play mode and a peer's lock refuse every writer")
{
	Fixture f("gates");
	f.writeAction("Input/IA_Jump.hasset", "Button");
	f.writeMapping("Input/IMC_Default.hasset");
	const std::string beforeAction = f.fileBytes("Input/IA_Jump.hasset");
	const std::string beforeMap    = f.fileBytes("Input/IMC_Default.hasset");

	f.playing = true;
	ToolResult r = f.call("input_action_set", json{
		{ "path", "Input/IA_Jump.hasset" }, { "valueType", "Axis" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "play_mode");
	r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" }, { "key", "Space" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "play_mode");
	// A reader is not refused: knowing what is bound while the game runs is the
	// most useful moment to ask.
	CHECK_FALSE(f.call("input_actions", json{ { "path", "Input/IA_Jump.hasset" } }).isError);
	f.playing = false;

	f.lockedRel = "Input/IMC_Default.hasset";
	r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" }, { "key", "Space" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "locked_by_other");
	CHECK_FALSE(f.call("input_mappings",
	                   json{ { "path", "Input/IMC_Default.hasset" } }).isError);

	CHECK(f.fileBytes("Input/IA_Jump.hasset") == beforeAction);
	CHECK(f.fileBytes("Input/IMC_Default.hasset") == beforeMap);
}

TEST_CASE("mcp input: an unsaved editor tab stops the write instead of losing it")
{
	Fixture f("dirty_tab");
	f.writeAction("Input/IA_Jump.hasset", "Button");
	f.writeMapping("Input/IMC_Default.hasset");
	const std::string before = f.fileBytes("Input/IMC_Default.hasset");

	// The Input Asset editor keeps no undo, so there is no safe place for this
	// edit: the file would be reverted by the human's next Save, and the tab
	// would be a change they could not take back.
	f.dirtyRel = "Input/IMC_Default.hasset";
	const ToolResult r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" }, { "key", "Space" } });
	CHECK(r.isError);
	CHECK(r.errorCode == "dirty");
	CHECK(f.fileBytes("Input/IMC_Default.hasset") == before);
	CHECK(f.reloadCalls == 0);

	// The reader still answers, and says the file it read is not what the editor
	// is showing.
	const ToolResult read = f.call("input_mappings",
	                               json{ { "path", "Input/IMC_Default.hasset" } });
	REQUIRE_FALSE(read.isError);
	CHECK(read.content["openInEditorUnsaved"] == true);
}

TEST_CASE("mcp input: a clean editor tab is told to re-read the file")
{
	Fixture f("clean_tab");
	f.writeAction("Input/IA_Jump.hasset", "Button");
	f.writeMapping("Input/IMC_Default.hasset");
	f.openRel = "Input/IMC_Default.hasset";     // a tab holds it, with nothing unsaved

	const ToolResult r = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" }, { "key", "Space" } });
	REQUIRE_FALSE(r.isError);
	CHECK(r.content["reloadedInEditor"] == true);
	CHECK(f.reloadCalls == 1);
	CHECK(wouldBind(f.payloadFromDisk("Input/IMC_Default.hasset")) == 1);

	// And with no tab at all the write is just a write — reported as such rather
	// than as a reload that did not happen.
	f.openRel.clear();
	const ToolResult r2 = f.call("input_mapping_bind", json{
		{ "path", "Input/IMC_Default.hasset" },
		{ "action", "Input/IA_Jump.hasset" }, { "key", "Return" } });
	REQUIRE_FALSE(r2.isError);
	CHECK(r2.content["reloadedInEditor"] == false);
	CHECK(f.reloadCalls == 1);
}
