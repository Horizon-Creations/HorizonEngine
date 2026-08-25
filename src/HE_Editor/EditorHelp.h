#pragma once
#include <string>
#include <string_view>

// ── What every control in the editor is for ──────────────────────────────────
// The editor had tooltips the way most tools have them: about sixty of them,
// written where somebody happened to think of it, and absent from the one panel
// that needs them most — the Details panel, where a hundred and fifty numbers
// with names like "Fluffiness", "Rolloff Factor" and "Walk Climb" sit with
// nothing to say what they do.
//
// This is that missing half of the interface, as a table. Three properties of
// the shape are what make it work:
//
//  * It is DATA, not calls. A tooltip written at a call site is invisible to
//    review, cannot be tested, and drifts from the manual the moment either
//    changes. The table can be walked — tests/test_editor_help.cpp checks that
//    every `topic` still resolves in the shipped documentation bundle, which is
//    what stops the two from parting ways silently.
//  * It is keyed by the LABEL the user is already reading, scoped by the
//    section it sits in ("Rigid Body/Mass"). So a whole panel gets its help
//    from one Scope at the top of a component section and one lookup inside
//    EditorWidgets::Row — no per-control churn, and adding a property to a
//    component is what makes its help entry appear.
//  * Every entry can carry a `topic`, so the tooltip is not the end of the
//    line: F1 over a control opens the in-editor manual at the section that
//    explains it properly. A tooltip is one sentence; some questions need a
//    page, and this is the seam between them.
//
// Writing entries: say what the control DOES and what it affects, in a sentence
// that is not the label again. "Mass — the mass" helps nobody; "How heavy the
// body is, in kilograms. Ignored for Static and Kinematic bodies" is the answer
// to the question that made the user hover.

namespace HE::Ed::Help
{

struct Entry
{
	// "Rigid Body/Mass" for a Details row, "viewport.play" for a hand-placed one.
	// The scoped form is what Row lookups build; the dotted form is for controls
	// that are drawn by hand and name themselves.
	const char* key;
	// Heading of the tooltip. May be empty for scoped rows, where the label on
	// screen is the heading and repeating it wastes the first line.
	const char* title;
	const char* body;
	// "Ctrl+S" — rewritten to "Cmd+S" on macOS by shortcutLabel(). Empty = none.
	const char* shortcut;
	// Topic in the documentation bundle ("editor#play-mode"), or empty. Every
	// non-empty one is asserted to resolve.
	const char* topic;
};

// Exact lookup. Null when the key is unknown — which is the normal case: only
// the controls worth explaining have an entry, and a missing one means no
// tooltip rather than an empty one.
const Entry* findKey(std::string_view key);

// Scope-aware lookup for a label the user can see: tries "<scope>/<label>",
// then the bare "<label>". Anything from "##" on is ignored, so the ImGui
// "Display##id" spelling can be passed straight in.
const Entry* find(std::string_view label);

// The section a run of controls belongs to — a component in the Details panel,
// a group of settings in Preferences. Scoped rather than set-and-forget because
// panels nest and return early; a stray scope would misattribute every lookup
// after it to the wrong component.
class Scope
{
public:
	explicit Scope(const char* name);
	~Scope();
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;
};

// The current scope ("" when none) — for the widgets that build a key by hand.
const char* currentScope();

// "Ctrl+S" → "Cmd+S" on macOS, unchanged elsewhere. The editor accepts either
// modifier on every platform (KeyCtrl || KeySuper), so this is purely about
// showing a Mac user the key their keyboard has.
std::string shortcutLabel(std::string_view shortcut);

// ── "Show me where that is" ──────────────────────────────────────────────────
// A documentation topic that describes a PANEL can point at it: the reader
// opens the panel and the spotlight pulses around it. This is the mapping, and
// it is the one thing in the help system that cannot live in the generated
// bundle — ImGui window names are the editor's, not the website's.
struct PanelTopic
{
	const char* topic;    // "editor#outliner"
	const char* window;   // the ImGui window name, for focus + spotlight
	const char* menu;     // how a user opens it, for the label ("View ▸ Console")
};

// The panel for a topic. Falls back from "page#section" to the page's own entry,
// so a section without its own mapping still points somewhere sensible. Null
// when the topic is not about a panel.
const PanelTopic* panelForTopic(std::string_view topic);

// Table access, for the tests and for anything that wants to walk the set.
int          entryCount();
const Entry& entryAt(int i);
int          panelTopicCount();
const PanelTopic& panelTopicAt(int i);

} // namespace HE::Ed::Help
