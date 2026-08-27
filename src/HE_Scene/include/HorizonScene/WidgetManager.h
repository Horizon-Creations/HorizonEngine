#pragma once
#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetBinding.h>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Renderer/UIRenderObject.h>
#include <Types/UUID.h>
#include <string>
#include <unordered_map>
#include <vector>

class ContentManager;

// Live UI widgets — created from UI Widget assets, existing OUTSIDE the entity
// world and rendered directly (no components, no entities). Owned by
// HorizonWorld only for lifetime convenience (cleared with the world, so PIE
// stop drops play-created widgets); scripts drive it through the horizon API:
// createWidget / showWidget / hideWidget / destroyWidget / setWidgetZOrder /
// callWidgetFunction. Each instance carries its own deep copy of the widget
// tree (mutable state: text/colors/layout/visibility) plus its HorizonCode
// graph, whose events fire from pointer input, keyboard and the frame tick.
class WidgetManager
{
public:
    // Instantiate a widget asset (content-relative path). Resolves per-element
    // material references, fires the "Construct" event, returns the widget id
    // (0 = asset missing or invalid tree).
    int createWidget(ContentManager& content, const std::string& assetPath);

    void destroyWidget(int id);
    void showWidget(int id);
    void hideWidget(int id);
    void setZOrder(int id, int z);

    // Read-only view of a live widget's element tree (nullptr = no such
    // widget). The manager owns a deep copy per widget; this is how a caller
    // looks at the live state — the caret in a text field, what a script last
    // wrote into a label — without being able to swap the tree out under it.
    const HE::UIWidgetTree* tree(int widgetId) const;

    bool isAlive(int id) const;
    bool isVisible(int id) const;
    int  zOrder(int id) const;
    size_t count() const { return m_instances.size(); }

    // Route a script call to a HorizonCode function. False when the widget or
    // the function is missing — or the function is not public (access modifier).
    bool callFunction(int id, const std::string& name);

    // Fire the "Tick" event (Float arg = dt) on every visible widget.
    void tick(float dt);

    // Pointer input in render-target pixels: hit-tests interactive elements
    // (buttons/checkboxes/sliders/combos/text fields + elements bound by
    // pointer-event nodes), drives element visual state and fires the matching
    // HorizonCode events. `valid` false (mouse captured / off-viewport) clears
    // hover. Returns true when the pointer is over an interactive element
    // (callers may swallow the click).
    bool processPointer(float vpWidth, float vpHeight,
                        float mouseX, float mouseY,
                        bool primaryDown, bool valid);

    // What the last processPointer answered. Both apps drop that return value,
    // so this is how anyone else — gameplay code and scripts, through
    // HE::api::ui::pointerOverUI — finds out that this frame's click belongs to
    // the UI: pressing "Start" in a menu must not also fire into the world.
    bool pointerOverUI() const { return m_pointerOverUI; }

    // Keyboard routing for the focused TextInput (the apps call these). All are
    // no-ops when no TextInput is focused.
    void inputText(const std::string& utf8); // insert at the caret, replacing any selection
    void inputBackspace();                    // delete the selection, else the character before
    void inputSubmit();                       // fire OnTextCommitted

    // ── Caret + selection ────────────────────────────────────────────────────
    // The one field that is really EDITED rather than pressed: it needs a caret
    // you can move, a selection you can extend and a way to get text in and out
    // of the system clipboard. The clipboard itself stays with the app (SDL
    // owns it) — this side hands over the selected text and takes a paste.
    // WordLeft/WordRight move across whole words (Ctrl+Arrow, Alt+Arrow on a
    // Mac); DeleteWordLeft is Ctrl/Alt+Backspace. Appended, not inserted — the
    // enum is used positionally by the apps' key tables.
    enum class TextEdit { Left, Right, Home, End, Delete, SelectAll,
                          WordLeft, WordRight, DeleteWordLeft };
    // True when something changed (and OnTextChanged fired where it applies).
    bool editFocusedText(TextEdit op, bool extendSelection);
    // Extend the selection to where the pointer is, without moving the anchor —
    // the drag half of click-and-drag selection. `mouseX` is in render-target
    // pixels like every other pointer coordinate here. False when nothing moved.
    bool dragCaretFromPointer(float vpWidth, float vpHeight, float mouseX);
    // Select the word (or the whole line) under the pointer: double- and
    // triple-click. Both leave the caret at the end of what they selected.
    bool selectWordAtPointer(float vpWidth, float vpHeight, float mouseX);
    bool selectAllFocused();
    // What is selected in the focused field, for a copy or a cut.
    std::string focusedSelection() const;
    // Drop it (the second half of a cut). False when nothing was selected.
    bool deleteFocusedSelection();
    // Put the caret where a click landed. `mouseX` is in render-target pixels,
    // like every other pointer coordinate here.
    bool setCaretFromPointer(float vpWidth, float vpHeight, float mouseX);
    // True while a TextInput has keyboard focus — the apps use it to decide
    // whether to route text/keys here instead of to gameplay/camera.
    bool hasFocusedTextField() const { return m_focusWidget != 0; }

    // The cursor the currently-hovered element requests (set by processPointer;
    // Default when nothing is hovered). The app maps it to a system cursor.
    HE::UICursor hoverCursor() const { return m_hoverCursor; }

    // ── Keyboard / gamepad navigation ────────────────────────────────────────
    // A menu has to be usable without a mouse. The focus moves SPATIALLY: from
    // the focused element towards the given direction, the nearest interactive
    // element in that direction wins, so a grid of buttons navigates the way it
    // looks rather than in tree order. With nothing focused yet, the first
    // direction press takes the topmost candidate.
    enum class NavDir { Up, Down, Left, Right };
    // True when the focus moved (or a focused slider took the step) — false
    // means nothing here wanted the key and the caller may still have it.
    bool navigate(NavDir dir, float vpWidth, float vpHeight);
    // Fire what a click would fire on the focused element: a button clicks, a
    // checkbox toggles, a combo advances. False when nothing is focused.
    bool activateFocused();
    // The focused element of the focused widget (0 = none). The focus ring is
    // drawn around it in extract().
    int  focusedElement() const;
    // Move the focus by hand (0 = clear it), e.g. when a menu opens and wants
    // its first button focused. False when the id is not focusable.
    bool setFocus(int widgetId, int elementId);

    // Mouse wheel in render-target pixels: scrolls the scroll box under the
    // pointer, innermost first (a list inside a list scrolls the one the cursor
    // is actually in). `wheel` is in notches, positive = away from the user.
    // Returns true when a box actually moved — false means the wheel was not
    // consumed and the caller (a camera zoom, say) may still have it.
    bool processWheel(float vpWidth, float vpHeight,
                      float mouseX, float mouseY, float wheel);

    // Append draw quads for all visible widgets, sorted by (zOrder, layer,
    // depth). Called AFTER the entity-UI extraction, so widgets draw on top.
    void extract(float vpWidth, float vpHeight, std::vector<UIRenderObject>& out);

    void clear();

    // Route every widget's HorizonCode through this central runtime instead of
    // the manager's own. HorizonWorld injects the scene-wide runtime so widgets,
    // the level script (and later the GameInstance) share one interpreter and can
    // reference each other. Null (the default) falls back to an internal runtime
    // for standalone use / tests.
    void setRuntime(HorizonCode::Runtime* r) { m_runtime = r; }

private:
    struct Instance
    {
        int id = 0;
        int zOrder = 0;
        HE::UIWidgetTree  tree;    // live deep copy (scripts mutate it)
        // This widget's script instance in the runtime (owns the graph + the
        // private variable store); 0 = no logic graph.
        HorizonCode::InstanceId scriptId = 0;
        bool visible = true;       // ShowSelf/HideSelf nodes flip this
        // Transient interaction state (element ids; 0 = none).
        int hoveredElem   = 0;
        int pressedElem   = 0;
        // The focused element: a TextInput taking keys, or whatever the
        // keyboard/gamepad navigation last landed on.
        int focusedElem   = 0;
        int draggingSlider = 0;    // slider being dragged
        // TextInput whose selection is being dragged out with the mouse held
        // down. Its own field rather than a flag on draggingSlider, because the
        // two do entirely different things with the same pointer stream.
        int draggingText   = 0;
        // Resolved material references (element id → material asset).
        std::unordered_map<int, HE::UUID> materials;

        // ── Embedded widgets (WidgetRef) ─────────────────────────────────────
        // A WidgetRef grafts another asset's tree in under itself. The elements
        // land in THIS tree with their ids shifted by idOffset, so two copies of
        // the same widget never collide — but that asset's logic graph runs as
        // its own script instance, and it knows its elements by their ORIGINAL
        // ids. Every event and every property access therefore goes through the
        // two translators below.
        struct Embed
        {
            int rootElem = 0;   // the WidgetRef element the tree hangs under
            int idOffset = 0;   // host id = local id + idOffset
            int idMax    = 0;   // last host id that belongs to this embed
            HorizonCode::InstanceId scriptId = 0;
        };
        std::vector<Embed> embeds;   // ascending by idOffset; nested ones nest
    };

    Instance*       find(int id);
    const Instance* find(int id) const;
    Instance*       findByScript(HorizonCode::InstanceId scriptId);

    // The runtime widgets run on: the injected shared one, else the internal
    // fallback. Resolved on each call (never a stored self-pointer), so the
    // manager stays movable.
    HorizonCode::Runtime&       rt()       { return m_runtime ? *m_runtime : m_ownRuntime; }
    const HorizonCode::Runtime& rt() const { return m_runtime ? *m_runtime : m_ownRuntime; }
    // Host bindings shared by every widget instance (property get/set + show/
    // hide), disambiguated by the runtime InstanceId.
    HorizonCode::HostBindings makeBindings();

    // True when the element can receive pointer events: interactive by type, or
    // bound by a pointer-event Event node (an Event with elem 0 makes every
    // element hot).
    bool isInteractive(const Instance& w, const HE::UIElement& e) const;

    // Fire what a click fires: button → OnClicked/OnReleased, checkbox toggles,
    // combo advances. Shared by the pointer release and by activateFocused(),
    // so a gamepad and a mouse cannot end up doing different things.
    void activateElement(Instance& w, int elemId);
    // Can the keyboard/gamepad focus land here? Interactive, enabled, visible,
    // and not clipped entirely out of sight.
    bool isFocusable(const Instance& w, const HE::UIElement& e,
                     const HE::UIWidgetCanvas& canvas) const;

    // The focused text field (nullptr when none), with its caret re-clamped: a
    // script may have rewritten the text since the last keystroke and left the
    // caret pointing past the end of it. Every editing entry point starts here.
    HE::UITextInput* focusedTextField(Instance*& outWidget);
    // Byte offset in the focused field under a pointer at `mouseX`. The single
    // home of the canvas/rect/padding arithmetic that click, drag and
    // double-click all need — three copies of it would drift.
    bool caretOffsetAtPointer(float vpWidth, float vpHeight, float mouseX, size_t& outOffset);

    // ── Embedded-widget translation ──────────────────────────────────────────
    // Which script owns an element, and what that script calls it. For anything
    // the host widget itself authored this is (w.scriptId, elemId); for an
    // element that came in with a WidgetRef it is the embedded instance and the
    // id it had in its own asset. The INNERMOST embed wins, so widgets nested
    // three deep still route to the right graph.
    struct ScriptTarget { HorizonCode::InstanceId scriptId = 0; int elem = 0; };
    ScriptTarget scriptTargetFor(const Instance& w, int elemId) const;
    // The reverse: the widget a script instance belongs to, plus the offset
    // that turns one of ITS element ids into one in the host tree.
    Instance* resolveScriptOwner(HorizonCode::InstanceId scriptId, int& idOffset);
    // Graft every WidgetRef's asset into `w`'s tree, recursively. `chain` is the
    // asset paths already being embedded on this branch — a widget that embeds
    // itself (directly or in a circle) is refused rather than expanded forever.
    void embedWidgetRefs(Instance& w, ContentManager& content,
                         std::vector<std::string>& chain, int depth);

    // Re-resolve one element's Material/Font path to its runtime state (material
    // UUID in w.materials, baked fontAtlasKey) — the same resolution createWidget
    // does for the whole tree. Called when a graph SETS those properties so the
    // change is visible immediately. No-op without a content manager.
    void refreshElementAssets(Instance& w, HE::UIElement& e);

    std::vector<Instance> m_instances;
    // The content manager of the last createWidget call — used to re-resolve
    // Material/Font when a graph sets those properties at runtime. All callers
    // pass the app's single ContentManager, which outlives this manager.
    ContentManager*       m_content = nullptr;
    HorizonCode::Runtime  m_ownRuntime;        // fallback when none is injected
    HorizonCode::Runtime* m_runtime = nullptr; // injected shared runtime (null → own)
    bool m_wasDown = false;
    bool m_pointerOverUI = false;  // last processPointer verdict (see pointerOverUI)
    int  m_focusWidget = 0;        // widget id owning the focused TextInput
    HE::UICursor m_hoverCursor = HE::UICursor::Default; // cursor the hovered element wants
};
