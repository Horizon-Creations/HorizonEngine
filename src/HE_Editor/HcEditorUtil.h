#pragma once
#include <Types/Enums.h>
#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class ContentManager;
namespace HorizonCode { enum class PinType : std::uint8_t; enum class NodeType : std::uint8_t;
                        enum class ContainerKind : std::uint8_t;
                        struct Graph; struct Node; struct Variable; struct Value; }
namespace HE::api { struct ApiFn; }

// Small editor helper: enumerate the project's assets of a given type (for the
// asset/object picker dropdowns — HorizonCode classes, widgets, textures, …).
// Walks the content root and sniffs each .hasset header — cheap enough to call
// while a combo is open.
namespace HcEditorUtil
{
	struct ClassRef
	{
		std::string label; // display name (file stem)
		std::string path;  // content-relative path (what nodes store)
		// On-disk size of the .hasset. Free to collect while scanning, and the only
		// thing a picker can say about an asset's WEIGHT before loading it — the
		// Material Editor's preview-mesh picker warns on the heavy ones with it.
		std::uint64_t bytes = 0;
	};
	std::vector<ClassRef> listAssets(ContentManager* cm, HE::AssetType type);

	// Does this asset match what someone typed into a picker's search box?
	// Case-insensitive, and every whitespace-separated term has to appear in
	// either the display name or the path — so "char idle" finds
	// "Characters/Hero/Idle" without anyone having to type it in order. An
	// empty query matches everything, which is what an untouched search field
	// has to mean.
	//
	// Pure string work on purpose: it is the rule that decides whether an asset
	// is reachable at all in a searchable dropdown, and it is the one piece of
	// such a dropdown that can be asserted without a window. Inline so a test
	// can reach it without linking the rest of this unit (which wants ImGui and
	// a ContentManager).
	inline bool assetMatchesQuery(const std::string& label, const std::string& path,
	                              const std::string& query)
	{
		const auto lower = [](std::string s)
		{
			for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		};
		const std::string hay = lower(label) + '\n' + lower(path);
		const std::string q   = lower(query);

		// Term by term, all of them required. Splitting on whitespace instead of
		// matching the query as one string is what makes "char idle" find
		// "Characters/Hero/Idle": nobody remembers a path in order, they
		// remember two words out of it.
		for (size_t i = 0; i < q.size(); )
		{
			while (i < q.size() && std::isspace(static_cast<unsigned char>(q[i]))) ++i;
			const size_t start = i;
			while (i < q.size() && !std::isspace(static_cast<unsigned char>(q[i]))) ++i;
			if (i == start) break;   // trailing whitespace, no term left
			if (hay.find(q.substr(start, i - start)) == std::string::npos) return false;
		}
		return true;   // no terms at all (an untouched search box) matches everything
	}
	// Convenience wrapper for the Create Object class picker.
	std::vector<ClassRef> listHorizonCodeClasses(ContentManager* cm);
	// Every .hescene under the project root, as project-relative paths (e.g.
	// "Content/123.hescene") — the exact string scene.load expects. Feeds the
	// scene-path dropdown on scene.* Engine Call nodes.
	std::vector<ClassRef> listScenes(ContentManager* cm);

	// One line under an engine call's body saying where its listed values come
	// from, or null. The lists themselves are drawn at the pins; this is the
	// half of the old scene/save-field pickers that was worth keeping.
	const char* engineParamHint(const HorizonCode::Node& n);

	// What a STRING parameter of an engine call is allowed to be, when the
	// answer is a list: the easing curves, a play direction, the project's
	// scenes, its HorizonCode classes, a savegame template's fields.
	//
	// Typing those was never a real choice. A misspelled easing plays Linear, a
	// misspelled animation plays nothing at all, a misspelled scene path loads
	// nothing, and none of the three says so — the whole class of bug a list
	// makes impossible. Empty means "there is no list", and the pin falls back
	// to a text box, which is the honest answer for a name, a title or a URL.
	//
	// The NODE, not just the parameter name: which field a save call may touch
	// depends on the accessor (getNumber lists the numbers), and which
	// properties an element has depends on which element the node names.
	//
	// This is the shared half. A host that knows more about the asset being
	// edited — which animations THIS widget carries, what its elements are
	// called — answers first (HcGraphHost::Host::paramChoices).
	std::vector<std::string> engineParamChoices(const HorizonCode::Node& n,
	                                            const std::string& param,
	                                            ContentManager* cm);

	// ── HC class registry ─────────────────────────────────────────────────────
	// The public interface of one HorizonCode class the editor knows about — an
	// asset (widget / HC class) or an in-memory graph (level / GameInstance). This
	// is what the drag-off menu and the object-type pickers read.
	struct MemberFn  { std::string name; std::vector<HorizonCode::PinType> paramTypes; bool hasResult = false; };
	struct MemberVar { std::string name; HorizonCode::PinType type; std::string className; };
	struct ClassInfo
	{
		std::string label;                 // display name
		std::string path;                  // content-relative asset path ("" for level/GI)
		enum Kind { Class, Widget, Level, GameInstance } kind = Class;
		std::vector<MemberFn>  functions;  // public FunctionEntry nodes
		std::vector<MemberVar> variables;  // public variables
	};
	// Reduce a graph to its public interface.
	ClassInfo classInfoFromGraph(const HorizonCode::Graph& g, const std::string& label,
	                             const std::string& path, ClassInfo::Kind kind);
	// Load an asset (HC class or widget) and extract its public interface. False if
	// the path resolves to no HorizonCode graph.
	bool classInfoForPath(ContentManager* cm, const std::string& path, ClassInfo& out);
	// Every HC class in the project (class + widget assets), plus the level and
	// GameInstance graphs when supplied.
	std::vector<ClassInfo> listClasses(ContentManager* cm,
	                                   const HorizonCode::Graph* levelGraph,
	                                   const HorizonCode::Graph* giGraph);

	// Searchable type dropdown: default value types (Float/Bool/Int/…) plus object
	// types (the project's HC classes). Writes `type` and, for an object type,
	// `className` (the class path); clears className for a default type. Shows the
	// class name for an object type instead of a bare "Object". Returns true when
	// changed. Pass className=nullptr where object types aren't allowed.
	// With `typeName` non-null the picker also offers the project's Enum and
	// Struct assets (HE::TypeRegistry): picking one sets type to Enum/Struct and
	// writes the definition's asset path into `typeName` (cleared otherwise).
	bool drawTypePicker(const char* label, ContentManager* cm,
	                    HorizonCode::PinType& type, std::string* className,
	                    std::string* typeName = nullptr);

	// Single value / Array / Set / Map. Writes BOTH halves of the pair the engine
	// stores (see HorizonCode::ContainerKind): `isArray` says "is a container",
	// `container` says which — so the two can never drift apart in the editor.
	// Returns true when changed.
	bool drawContainerPicker(const char* label, bool& isArray,
	                         HorizonCode::ContainerKind& container);

	// The KEY type of a map: Int, String, Enum or Object, and nothing else
	// (HorizonCode::isValidMapKeyType — Float and the composites have no
	// identity a keyed lookup can rest on). Enum keys additionally name their
	// definition asset in `keyTypeName`.
	bool drawKeyTypePicker(const char* label, HorizonCode::PinType& keyType,
	                       std::string& keyTypeName);

	// "Map<String, Bool>" / "Array<Bool>" / "Set<String>" / "Bool" as text.
	// The key comes FIRST, the way the type is read aloud and the way
	// docs/horizoncode-reference.md already writes it.
	//
	// `typeName` names the definition behind an Enum/Struct value ("Bool" is a
	// type, "Enum" is not — the asset stem is) or the CLASS behind an Object,
	// `keyTypeName` the same for an Enum key. Both are asset paths; only the stem
	// is shown, exactly like every other place the editor names a user-defined
	// type.
	std::string typeLabel(HorizonCode::PinType type, bool isArray,
	                      HorizonCode::ContainerKind container, HorizonCode::PinType keyType,
	                      const std::string& typeName = {}, const std::string& keyTypeName = {});
	// The same label, but drawn: each part in ITS pin colour — the key in the key
	// type's colour, the value in the value type's, the container word and the
	// punctuation dimmed. That is what tells a Map from an Array at a glance,
	// before the words are read at all.
	//
	// Draws at the current cursor and leaves it where a plain
	// ImGui::TextUnformatted of the whole label would (same line for a following
	// SameLine, next line otherwise).
	void drawTypeLabel(HorizonCode::PinType type, bool isArray,
	                   HorizonCode::ContainerKind container, HorizonCode::PinType keyType,
	                   const std::string& typeName = {}, const std::string& keyTypeName = {});

	// ── Variable list rows ───────────────────────────────────────────────────
	// How a variable list spells one variable. The user picks this on
	// Preferences ▸ Editor ▸ HorizonCode.
	//
	// The style is a PARAMETER rather than something this unit looks up: the
	// panels read the setting once per frame instead of once per row, this unit
	// keeps knowing nothing about where preferences live, and a test can shoot
	// both looks without linking the settings panel.
	enum class VariableRowStyle : std::uint8_t
	{
		Detailed = 0, // glyph + name, with the type spelled out on a second line
		Compact  = 1, // glyph + name + type, all on one line
	};

	// One variable, as much of it as a list row shows.
	struct VariableRowDesc
	{
		const char*                name = "";
		HorizonCode::PinType       type{};
		bool                       isArray = false;
		HorizonCode::ContainerKind container{};
		HorizonCode::PinType       keyType{};
		std::string typeName;    // Enum/Struct: the definition asset
		std::string keyTypeName; // an Enum key's definition asset
		std::string className;   // Object: the class it holds
		// Drawn INSTEAD of the type when set. The inherited list uses it to say
		// "private", where what the reader needs is not the type.
		const char* note = nullptr;
	};

	// Draws one row as a Selectable and answers whether it was clicked.
	//
	// The Selectable stays the LAST ImGui item: name, type and glyph are painted
	// into the draw list on top of it, not emitted as further items. So the
	// caller keeps IsItemHovered(), BeginDragDropSource() and its tooltip, and
	// they all address the whole row.
	bool variableRow(const VariableRowDesc& v, bool selected, VariableRowStyle style);

	// A value type's name as the editor spells it ("Vec3", "Bool", "Exec").
	// Exported because the node reference in the manual is built outside this
	// file and has to name types the same way the graph editor does.
	const char* pinTypeName(HorizonCode::PinType t);

	// Interface editor for a HorizonCode function: edit the FunctionEntry's typed
	// Inputs (params) and Outputs (results). On any change it re-syncs the matching
	// Call/Return nodes and prunes now-invalid links, then sets `edited`. Shared by
	// the level/GI/class graph editor and the widget graph editor.
	void drawFunctionInterface(HorizonCode::Graph& g, HorizonCode::Node& entry, bool& edited);

	// ── Renaming a function without losing its calls ──────────────────────────
	// A function has no identity beyond its name: FunctionCall and FunctionReturn
	// nodes find it by comparing strings. So a rename has to carry to them, and
	// the obvious way to write that row does NOT do it: ImGui::InputText writes
	// into the node on every keystroke, so a name captured at the top of the
	// frame is already the NEW one by the time IsItemDeactivatedAfterEdit() fires
	// and the propagation compares two identical strings. Both graph editors
	// shipped it that way, and in both it never ran once.
	//
	// So the field edits a scratch buffer and the rename is committed in one
	// step, while the node still holds the old name:
	//
	//     HcEditorUtil::seedFunctionName(*n, st.fnNameEdit);
	//     ImGui::InputText("Name", &st.fnNameEdit.buf);
	//     EditorWidgets::helpForLabel("Name");
	//     if (ImGui::IsItemDeactivatedAfterEdit())
	//         edited |= HcEditorUtil::commitFunctionName(graph, *n, st.fnNameEdit);
	//
	// Neither call touches ImGui, which is what lets the rename be tested at all
	// (tests/test_hc_function_rename.cpp, which also drives a real InputText).
	struct FnNameEdit
	{
		std::string buf;       // what the field shows, and what is being typed into it
		int         node = 0;  // the FunctionEntry `buf` belongs to
		std::string seed;      // the name `buf` was filled from
	};

	// Fill the buffer when the panel starts showing a different function — or
	// when the shown one was renamed behind the panel's back (an undo, a
	// collaborator, another graph reusing the node id). Keying on the id alone
	// misses all three; keying on the name alone cannot tell two unnamed
	// functions apart. Does nothing while the name is being typed, which is what
	// keeps the keystrokes.
	void seedFunctionName(const HorizonCode::Node& entry, FnNameEdit& e);

	// Commit what was typed: rename the entry, and with it every FunctionCall and
	// FunctionReturn that named it. Returns true when something changed (the
	// caller's cue for an undo snapshot). Renaming TO nothing leaves the calls
	// alone rather than pointing them all at an unnamed function, and naming a
	// function that had no name does not adopt the calls that have none either.
	//
	// `selfKey` is this graph's own asset path, which is what a Call Function
	// (Ref) pointed back at this class through a Get Self resolves to. Empty when
	// the asset has none yet: nothing outside can reference it either way.
	// Reaching the REST of the project is the sweep's job (HcRename), not this
	// function's — this one only ever touches the graph it is handed.
	bool commitFunctionName(HorizonCode::Graph& g, HorizonCode::Node& entry, FnNameEdit& e,
	                        const std::string& selfKey);

	// ── Literal node bodies (inline value editors on the node) ────────────────
	// Const/literal nodes show their value right on the node body: a checkbox for
	// Bool, a number field for Int/Float, two fields for Vec2, a swatch for Color,
	// and a multi-line entry for String that grows with the text up to a cap then
	// scrolls. Height is in graph units (px at zoom 1); 0 for non-literal nodes so
	// the node stays compact/body-less.
	float literalNodeBodyHeight(const HorizonCode::Node& n);
	// Draws the inline editor into the current body child. Returns true if the value
	// changed this frame; sets `committed` when the edit finished (undo snapshot).
	bool  drawLiteralNodeBody(HorizonCode::Node& n, bool& committed);

	// ── Searchable menu: ranking + keyboard driving ───────────────────────────
	// The HorizonCode palettes (right-click add-node, drag-off-a-pin) rank every
	// entry against the search query, highlight the best match and let ↑/↓ move it
	// and Enter insert it — so the common case is "right-click, type three
	// letters, Enter" without ever aiming at the list.
	//
	// Every entry a menu offers MUST be drawn with searchMenuItem(): a raw
	// ImGui::Selectable is invisible to the ranking, so the keyboard skips over it
	// and a worse entry behind it wins the highlight. Usage inside the popup:
	//
	//     const std::string q = HcEditorUtil::searchMenuBegin("##search", "Search…", 220.0f);
	//     ImGui::BeginChild(…);                       // the scrolling list
	//       if (HcEditorUtil::searchMenuItem("Branch")) { … create the node … }
	//     ImGui::EndChild();
	//     HcEditorUtil::searchMenuEnd();
	//
	// Draws the search field and starts a session; returns the lowercased query
	// (the caller still does its own filtering — ranking only orders what is shown).
	std::string searchMenuBegin(const char* id, const char* hint, float width);
	// One entry. `label` is the ImGui label as usual (an "##id" suffix is ignored
	// for ranking). Returns true when picked — clicked, or Enter on the highlight.
	// A disabled entry is drawn greyed out and left out of the ranking entirely.
	bool searchMenuItem(const std::string& label, bool disabled = false);
	// Closes the session: settles which entry the highlight lands on next frame.
	void searchMenuEnd();

	// Draws the HE::api engine registry as an add-menu section: entries grouped by
	// category, filtered by `lowerQuery` (already lowercased; empty = show all).
	// Returns the picked registry id when a selectable is clicked this frame, else
	// "". The caller resolves it via HE::api::find and builds an EngineCall node
	// (copying the descriptor's isExec → hasArg and params/results onto the node).
	// Entries go through searchMenuItem, so they join the keyboard ranking.
	// `allowedGroups` restricts which HE::api groups are offered ("player",
	// "math", …). Empty = everything, which is what every general-purpose editor
	// passes. See HcGraphHost::MenuOpts::apiGroups.
	//
	// `onlyCategory` limits the walk to one category, and `headerDrawn` then lets
	// the caller SHARE a header with something else: the add menu draws one
	// "UI" section holding the UI nodes AND the UI engine calls, rather than a
	// node section and a second "Engine · UI" section under it. Pass nullptr for
	// both to get the whole registry with its own lazy per-category headers.
	std::string drawEngineApiMenu(const std::string& lowerQuery,
	                              const std::vector<const char*>& allowedGroups = {},
	                              const char* onlyCategory = nullptr,
	                              bool* headerDrawn = nullptr);

	// The registry categories that have at least one row matching `lowerQuery`
	// and `allowedGroups`, in registry order and without duplicates. The add menu
	// needs the list to know which sections exist beyond the node categories its
	// frontend declares.
	std::vector<const char*> engineApiCategories(const std::string& lowerQuery,
	                                             const std::vector<const char*>& allowedGroups = {});

	// Is this api id in `allowedGroups`? Empty list = yes. Thin pass-through to
	// HE::api::groupAllowed, which is where the rule lives — the two menus that
	// use it must not be able to disagree.
	bool apiGroupAllowed(const char* apiId, const std::vector<const char*>& allowedGroups);
	// Readable title for an EngineCall node ("Sine" for math.sin) — the registry's
	// displayName, falling back to the raw id.
	std::string engineCallTitle(const std::string& apiId);

	// ── Cast node targets ───────────────────────────────────────────────────
	// A Cast's target (Node::s) is ONE key namespace: either an engine class
	// name from HorizonCode::engineClasses() or a content-relative HC class
	// asset path. They cannot collide — a path always carries a '/' and a
	// '.hasset' — so nothing has to record which kind it is.
	//
	// castTargetLabel reduces a key to what the UI shows ("Goblin" for
	// "Content/Enemies/Goblin.hasset"); castTitle is the node's header
	// ("Cast To Goblin"). The node's own DISPLAY NAME stays the bare "Cast":
	// that string is the key a saved graph is read back by, so it must not
	// depend on the target.
	std::string castTargetLabel(const std::string& targetKey);
	std::string castTitle(const std::string& targetKey);

	// A node whose whole job is "make one of THIS asset" — Create Widget, Create
	// Object — reading as the bare "Create Widget" means the graph does not say
	// which one, and five of them side by side are indistinguishable until you
	// click each. Same answer Cast gives: put the asset in the header.
	//
	// The stem, like every other asset picker in the editor shows (ClassRef's
	// label is the stem too). The node's DISPLAY NAME stays the bare one — that
	// string is a key a saved graph is read back by and must not depend on the
	// target, which is the rule the Cast block above states.
	std::string assetNodeTitle(const char* base, const std::string& assetPath);
	// Point a Cast node at `targetKey` and mirror the readable output-pin name
	// ("As Goblin") onto the node, the same way an EngineCall mirrors its
	// descriptor's params: PinDesc::name is a borrowed pointer, so the label has
	// to live on the node rather than be composed while the signature is built.
	// The pin COUNT never changes, so unlike a struct/enum rebind this needs no
	// link remapping.
	void setCastTarget(HorizonCode::Node& n, const std::string& targetKey);

	// Default-value slots for an ARRAY variable: add/remove/edit elements, each
	// with the element type's editor (drag-float, checkbox, color swatch, …).
	// Returns true when anything changed (the caller commits for undo).
	bool drawArrayDefaultEditor(HorizonCode::Variable& v);

	// The slot list behind it, reusable wherever an array default is authored
	// (an array VARIABLE's seed, a struct FIELD's seed). The list has no fixed
	// length — slots are added/removed here, and Array Append/Insert/Remove do
	// the same at runtime. Enum slots persist the entry NAME; struct slots seed
	// from their own definition and are therefore read-only here.
	bool drawArraySlotsEditor(std::vector<HorizonCode::Value>& items,
	                          HorizonCode::PinType elemType, const std::string& elemTypeName);

	// Default editor for a STRUCT variable: one row per field of its definition,
	// each overridable PER GRAPH (Variable::structDefaults, name-keyed). A row
	// with no override shows the definition's own default and says so; "Reset"
	// drops back to it. Nested-struct and array fields are read-only in v1 (they
	// carry their own defaults). Returns true when something changed. Shared by
	// the level/GI/class variable panel and the widget one, so the two can't
	// drift apart.
	bool drawStructDefaultEditor(HorizonCode::Variable& v);

	// Inline pin defaults: simple UNWIRED data inputs (Bool/Int/Float/String, no
	// arrays) show a small entry right on the node — no literal node needed.
	// `pinSupportsInlineDefault` gates per unified pin; the editor draws inside
	// the GraphEditor's positioned per-pin child. `committed` = snapshot time.
	bool pinSupportsInlineDefault(const HorizonCode::Node& n, int unifiedPin);
	// `choices` answers what a string pin may hold (engineParamChoices plus
	// whatever the host knows). A pin with a list gets a DROPDOWN instead of a
	// text box; without one, or with an empty list, the text box stays — a
	// widget that has no animations yet still has to be able to author a name.
	using ParamChoices =
		std::function<std::vector<std::string>(const HorizonCode::Node&, const std::string&)>;
	void drawPinDefaultEditor(HorizonCode::Node& n, int unifiedPin, bool& committed,
	                          const ParamChoices& choices = {});
	// How much room that editor needs, in graph units (0 = the canvas's default
	// slot). A string pin asks for more: a name has to be readable to be
	// checkable, and as a dropdown it carries an arrow as well.
	float pinInlineEditorWidth(const HorizonCode::Node& n, int unifiedPin);

	// ── Drag-off compatibility ────────────────────────────────────────────────
	// First unified pin index on a FRESH node of `t` (propType seeded with the
	// dragged type) that accepts the dragged pin, or -1. srcIsInput = the drag
	// started on an input pin (so the new node must OUTPUT into it); srcIsExec =
	// the dragged pin is an exec pin (data type is ignored then).
	//
	// `dragCtr` is the dragged pin's CONTAINER KIND, resolved (never the raw
	// `container` field). It has to be the kind and not a bool: Graph::connect
	// refuses an array↔set wire, so a menu that matched on "is a container"
	// would keep offering nodes whose pin the drop then cannot join.
	int dragMatchPin(HorizonCode::NodeType t, HorizonCode::PinType dragType,
	                 HorizonCode::ContainerKind dragCtr, bool srcIsInput, bool srcIsExec);
	// Same match, but against a node that already EXISTS — its definition-bound
	// pins are the real ones, which a bare template probe does not have.
	int dragMatchPinOn(const HorizonCode::Node& n, HorizonCode::PinType dragType,
	                   HorizonCode::ContainerKind dragCtr, bool srcIsInput, bool srcIsExec);
	// Same for an HE::api registry entry (an EngineCall node built from it).
	// The registry has no Set/Map parameters at all, so an api pin matches only a
	// scalar or an array drag.
	int dragMatchApiPin(const HE::api::ApiFn& fn, HorizonCode::PinType dragType,
	                    HorizonCode::ContainerKind dragCtr, bool srcIsInput, bool srcIsExec);

	// "Return from <fn>" picker for a FunctionReturn node's details — lists the
	// functions declared in the graph (those with a FunctionEntry). Sets the node's
	// owning function name + mirrors its result pins. Returns true if it changed.
	bool drawReturnFunctionPicker(HorizonCode::Graph& g, HorizonCode::Node& ret);

	// ── Node documentation ────────────────────────────────────────────────────
	// Draws what a node is: its name, what it does (HorizonCode::nodeTooltip, or
	// the engine-call description from HcNodeDocs for an Engine Call), and its
	// inputs and outputs — with the SAME glyphs and colours the canvas draws
	// those pins with, so the tooltip teaches the vocabulary the wires use
	// instead of a second one made of punctuation.
	//
	// Draws CONTENT only: the caller owns the window, which is what lets the
	// same thing appear in a hover tooltip on the canvas, on an add-menu row and
	// on the reference page in the manual.
	//
	// Returns the node's topic in the manual ("horizoncode-nodes#…", empty when
	// it has none) so a caller can offer F1 on it.
	std::string drawNodeDoc(const HorizonCode::Node& n);
	// Same, for a bare node type (add-menu items — no configured instance yet).
	std::string drawNodeDoc(HorizonCode::NodeType t);

	// The manual topic for a node without drawing anything. Engine calls are
	// keyed by their registry id, built-ins by their display name.
	std::string nodeDocTopic(const HorizonCode::Node& n);

	// ── Shared graph colors (ImU32; keep every HC editor consistent) ──────────
	// A stable color per value type — Bool always red, Float green, Ref purple, …
	std::uint32_t pinTypeColor(HorizonCode::PinType t);
	// A node's header/accent color: Events red, Functions purple, Branch/Sequence
	// gray, reference/object nodes purple, and data nodes (Get/Set/Const) colored
	// by their value type so a Bool getter is always red, a Float getter green, …
	std::uint32_t nodeHeaderColor(const HorizonCode::Node& n);
}
