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
    // The instance is created HIDDEN — creating and showing are two steps, the
    // way they are in every UI framework: a menu is usually built long before it
    // is put up, and building it visible makes it flash. Call showWidget() when
    // it should appear.
    int createWidget(ContentManager& content, const std::string& assetPath);

    void destroyWidget(int id);
    void showWidget(int id);
    void hideWidget(int id);
    void setZOrder(int id, int z);

    // ── What the application looks like ──────────────────────────────────────
    // The theme every widget's bound properties resolve against (see
    // UIElement::themeRoles and uiApplyTheme). Setting either of these
    // re-resolves EVERY live widget straight away — which is what makes light
    // and dark one switch instead of a reload.
    //
    // There is always a theme: absent, it is HE::uiDefaultTheme(). Nothing has
    // to answer "what if none is set".
    void setTheme(const HE::UITheme& theme);
    const HE::UITheme& theme() const { return m_theme; }

    // What was ASKED for, which is not the same as what it resolves to. The
    // default is "follow the desktop", and the desktop's answer arrives from the
    // host through setSystemThemeMode — this class links no SDL and must not
    // learn to.
    void setThemePreference(HE::UIThemePreference pref);
    HE::UIThemePreference themePreference() const { return m_themePref; }
    // The host's reading of the desktop (SDL_GetSystemTheme, and again on
    // SDL_EVENT_SYSTEM_THEME_CHANGED). Only changes the picture while the
    // preference is System.
    void setSystemThemeMode(HE::UIThemeMode mode);
    // What the two above actually come out as — the mode every bound colour is
    // resolved against.
    HE::UIThemeMode themeMode() const
    {
        return m_themePref == HE::UIThemePreference::Light  ? HE::UIThemeMode::Light
             : m_themePref == HE::UIThemePreference::Dark   ? HE::UIThemeMode::Dark
                                                            : m_systemMode;
    }

    // ── Building the interface while it runs ─────────────────────────────────
    // A list of things — todos, search results, files, messages — is the most
    // ordinary thing an application shows, and until this existed it was the one
    // thing the widget system could not do: every element had to be authored in
    // the designer, so a list was N pre-made rows with a hard ceiling.
    //
    // `addChild` grafts a widget ASSET under the element named `parentName`,
    // which is the same machinery a WidgetRef uses — one row is its own widget,
    // authored once, with its own logic running as its own script instance. Put
    // the parent in a Vertical Box and the rows stack themselves.
    //
    // Returns that instance's id, so the caller can reach the new row the way it
    // reaches any other object: Set External on a public variable, Call External
    // on a public function, Bind Event on what it emits. 0 = the widget, the
    // parent or the asset was not found.
    HorizonCode::InstanceId addChild(ContentManager& content, int widgetId,
                                     const std::string& parentName,
                                     const std::string& assetPath);
    // Take one back out, addressed by the instance id addChild returned. Its
    // elements and its script instance both go.
    bool removeChild(int widgetId, HorizonCode::InstanceId child);
    // …or all of them under one parent. Returns how many were removed, which is
    // what "rebuild the list from scratch" is written with.
    int  clearChildren(int widgetId, const std::string& parentName);

    // ── Lists (docs/he-apps-plan.md B2) ──────────────────────────────────────
    // A ListView holds no items — it holds a COUNT and a row template — so this
    // is the whole of "here is my data": say how many there are, and answer
    // OnRowBind for the rows that end up on screen. Ten thousand items cost the
    // rows the window can show, not ten thousand elements.
    //
    // False = no such widget, or no element of that name, or it is not a list.
    bool setListCount(int widgetId, const std::string& listName, int count);
    int  listCount(int widgetId, const std::string& listName) const;
    // The live instance showing item `index`, or 0 when that item is not
    // currently realized (scrolled out, or past the end). This is what an
    // OnRowBind handler reaches the row with — Set External on a public
    // variable, Call External on a public function, exactly like addChild's
    // return value.
    HorizonCode::InstanceId listRow(int widgetId, const std::string& listName,
                                    int index) const;
    // Bind every realized row again, without moving anything. What a sort, a
    // filter or an edit of the underlying data is written with: the list never
    // saw the items, so it cannot notice they changed.
    bool refreshList(int widgetId, const std::string& listName);
    // Selection, by item index. Setting fires OnSelectionChanged when the set
    // actually changes; -1 to select means "clear".
    bool setListSelected(int widgetId, const std::string& listName, int index, bool on);
    int  listSelected(int widgetId, const std::string& listName) const;   // -1 = none
    bool scrollListToItem(int widgetId, const std::string& listName, int index);

    // ── Layers: dialogs, popups, menus (docs/he-apps-plan.md B4) ────────────
    // Three things that look different and ARE the same thing: while one of
    // them is up, input belongs to it and to nothing underneath. A modal dims
    // what is behind it and only leaves on command; a popup dismisses itself
    // the moment you click somewhere else; an open dropdown is a popup that
    // happens to be drawn by the element that owns it.
    //
    // They stack, because a confirmation over a settings dialog is the ordinary
    // case and a single slot would silently drop one of them.
    //
    // A modal is put ON TOP when it opens (its z-order is raised above every
    // other widget), because a dialog that blocks input while drawing behind
    // something is the worst of both.
    void showModal(int widgetId);
    // Put the widget at a point on screen — in render-target pixels, the same
    // space every other coordinate here is in — and let a click anywhere else
    // dismiss it. The widget's ROOT elements are moved there and clamped so the
    // whole thing stays on screen, whatever anchors they were authored with.
    void openPopupAt(int widgetId, float x, float y);
    // …at the pointer, which is where a context menu goes. The manager knows
    // where the pointer last was, so a graph does not have to.
    void openPopupAtPointer(int widgetId);
    // Close the topmost layer: what Escape does, and the Back button. The closed
    // widget is hidden and its own graph gets OnDismissed. False = nothing was
    // open, and the key belongs to whoever asked (a game's pause menu, say).
    bool closeTopLayer();
    // Is anything holding the input, and is any of it a modal? The second one is
    // what makes the whole screen count as "the pointer is on the UI" — the dim
    // is drawn by this class and is not an element, so nothing else could say so.
    bool hasLayer() const { return !m_grabs.empty(); }
    bool hasModal() const;

    // Read-only view of a live widget's element tree (nullptr = no such
    // widget). The manager owns a deep copy per widget; this is how a caller
    // looks at the live state — the caret in a text field, what a script last
    // wrote into a label — without being able to swap the tree out under it.
    const HE::UIWidgetTree* tree(int widgetId) const;

    bool isAlive(int id) const;
    bool isVisible(int id) const;
    int  zOrder(int id) const;
    size_t count() const { return m_instances.size(); }
    // Ids of every live widget, in creation order. The editor's outliner shows
    // an application's widget hierarchy instead of a world it does not have
    // (docs/he-apps-plan.md E2), and `tree(id)` alone cannot say WHICH ids exist.
    std::vector<int> liveIds() const;

    // ── Did anything change what is on screen? ───────────────────────────────
    // An application draws when something CHANGED, not sixty times a second
    // (docs/he-apps-plan.md A2), and the widget layer is where almost all of
    // that change happens: a script writing a label, a hover, a caret moving, a
    // widget appearing. Everything here that touches the picture raises this.
    //
    // Deliberately COARSE: one flag for the whole manager, not a dirty rect and
    // not per widget. The consumer only has to answer "redraw or sleep", and a
    // finer signal would be more code with no more answer in it.
    //
    // Consume, don't peek: whoever asks is the one drawing the frame that
    // settles it, and two consumers would mean the second one sleeps through a
    // change the first one already cleared.
    bool consumeVisualDirty() { const bool d = m_visualDirty; m_visualDirty = false; return d; }
    // Raise it from outside — for the paths that change the picture without
    // going through this class (an asset finishing its load, say).
    void markVisualDirty() { m_visualDirty = true; }

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
    // `secondaryDown` is the right button. It is the LAST parameter and it has a
    // default because every caller that does not care about it should not have
    // to say so — a right-click is one feature (the context menu), not a change
    // to how pointing works.
    bool processPointer(float vpWidth, float vpHeight,
                        float mouseX, float mouseY,
                        bool primaryDown, bool valid,
                        bool secondaryDown = false);

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

    // ── Input methods (IME) ──────────────────────────────────────────────────
    // What the OS input method is building but has not committed: the preedit
    // run, plus where it put its own caret inside it (-1 = unspecified). Typing
    // on a Chinese, Japanese or Korean keyboard produces a stream of these
    // before a single finished character arrives as ordinary text input, and a
    // field that ignores them shows nothing at all while the user types.
    //
    // An empty string ends the composition — which is also what the OS sends
    // when it is cancelled.
    void inputComposition(const std::string& utf8, int cursorByte);
    // True while the focused field is holding preedit text.
    bool hasComposition() const;
    // Where the focused field sits on screen, in render-target pixels. The app
    // hands this to SDL_SetTextInputArea so the candidate window opens next to
    // the field instead of in the corner of the screen. False when nothing is
    // focused.
    bool focusedFieldRect(float vpWidth, float vpHeight, HE::UIWidgetRect& out) const;

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
    // What a DOUBLE-click means where it is not text: the row under the pointer
    // is activated (OnRowActivated). The app tries the text field first and
    // falls through to here, so one gesture keeps one meaning per thing it lands
    // on. False = nothing under the pointer had an "open me".
    bool activateAtPointer(float vpWidth, float vpHeight, float mouseX, float mouseY);
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
        // The asset this instance came from. Kept because a graft has to know
        // it: addChild grafting THIS widget into itself is a circle, and the
        // guard that catches it works on the chain of paths already visited.
        std::string assetPath;
        HE::UIWidgetTree  tree;    // live deep copy (scripts mutate it)
        // This widget's script instance in the runtime (owns the graph + the
        // private variable store); 0 = no logic graph.
        HorizonCode::InstanceId scriptId = 0;
        // Created HIDDEN. Create Widget makes an instance of the widget class —
        // it does not put it on screen; Show Widget does that, the way every UI
        // framework separates the two. Flipped by showWidget/hideWidget and by
        // the ShowSelf/HideSelf nodes.
        bool visible = false;
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
    // Put one widget asset in as a child of `parentElem` and give it its own
    // script instance: the whole of what addChild does once the parent is found,
    // and what a list realizes each of its rows with. `rowIndex` >= 0 marks the
    // new element as a list row (see UIWidgetRef::rowIndex). 0 = refused.
    HorizonCode::InstanceId graftChildRef(Instance& w, ContentManager& content,
                                          int parentElem, const std::string& assetPath,
                                          int rowIndex);
    // The script instance a grafted child element belongs to (0 = none).
    HorizonCode::InstanceId instanceOfChild(const Instance& w, int refElemId) const;

    // ── Realizing list rows ──────────────────────────────────────────────────
    // Work out which items each ListView can currently show, make sure exactly
    // that many rows exist, point them at the right items and bind the ones
    // whose item changed. Idempotent and cheap when nothing moved, which is why
    // it can simply run before every frame and before every pointer event
    // instead of being hooked to each thing that could invalidate it.
    void syncLists();
    void syncLists(Instance& w);
    // ── Why a latch ──────────────────────────────────────────────────────────
    // OnRowBind runs the owner's graph, and an ordinary thing for that graph to
    // do is change the list: "bound the last row → there are fifty more"
    // (endless scrolling), or a filter that shrinks the count. Both land back in
    // setListCount, which syncs — and a nested sync GRAFTS AND REMOVES rows
    // while the outer one is walking them. Removing destroys elements, so that
    // is a use-after-free reachable from a graph anybody could author.
    //
    // The latch turns it into what it should be: the change is recorded, the
    // picture is marked dirty, and the next sync — one is run before every frame
    // and before every pointer event — acts on it. One frame later at the very
    // worst, which is exactly what event-driven drawing is built to survive.
    bool m_syncingLists = false;
    // Elements of a list, in tree order. Nullable entries are never returned.
    std::vector<HE::UIWidgetRef*> listRowsOf(Instance& w, int listId);
    // Find a named ListView in a widget (nullptr = no widget, no such name, or
    // the name belongs to something that is not a list).
    HE::UIListView* findList(int widgetId, const std::string& listName);
    const HE::UIListView* findList(int widgetId, const std::string& listName) const;
    // Pick one row and fire what that means. Shared by the press and by the
    // keyboard, so the two cannot drift.
    void selectListRow(Instance& w, HE::UIListView& lv, int item);

    // ── One question, asked by every input entry point ───────────────────────
    // While a layer is up, everything under it is inert — and "everything" has
    // to mean every route in, not just the pointer scan. The wheel, the
    // keyboard navigation, the caret and the double-click all ask this, because
    // the one that does not ask is the one through which a dialog leaks.
    bool takesInput(int widgetId) const;
    struct Grab
    {
        enum class Kind : uint8_t { Modal, Popup, Dropdown };
        Kind kind = Kind::Popup;
        int  widget = 0;   // the widget that holds the input
        int  elem   = 0;   // Dropdown only: the ComboBox that is open
        // Put back when it closes. A dialog that leaves the focus wherever it
        // happened to end up is the classic dialog bug.
        int  prevFocusWidget = 0, prevFocusElem = 0;
        int  prevZOrder = 0;   // Modal only: it was raised to the top
    };
    std::vector<Grab> m_grabs;
    // Close the top layer WITHOUT firing OnDismissed — for the paths where the
    // widget is going away anyway (destroyed, cleared).
    void popGrab(bool notify);
    // Let go of every layer this widget holds, as if it had been closed. Hiding
    // a dialog and closing one are the same event from two sides.
    void releaseGrabsOf(int widgetId);
    // Where the pointer last was, in render-target pixels. Kept because two
    // features need it long after the call that reported it: a context menu
    // opens at it, and a tooltip is drawn beside it.
    float m_pointerX = 0.0f, m_pointerY = 0.0f;
    // …and how big the render target last was. A popup is placed by a call that
    // gets no viewport (a graph says "open this menu here"), and placing needs
    // the canvas, which needs the viewport. Remembered from the last frame, like
    // the pointer position above: both are answers to "where", and "where" is
    // only ever known during a frame.
    float m_lastViewportW = 1920.0f, m_lastViewportH = 1080.0f;

    // ── Tooltips ─────────────────────────────────────────────────────────────
    // The element whose tooltip is being waited out, how long it has been
    // hovered, and whether the wait is over. The timer runs in tick(), which is
    // also where it has to raise the dirty flag: an application that only
    // redraws on change would otherwise show the tooltip when the mouse next
    // moves, which is exactly when a tooltip is no longer wanted.
    // Both hang outside the element they belong to and both have to be over
    // every widget, not just their own — so the manager draws them, the way it
    // already draws the focus ring and the modal scrim.
    void drawOpenDropdown(float vpWidth, float vpHeight, std::vector<UIRenderObject>& out);
    void drawTooltip(float vpWidth, float vpHeight, std::vector<UIRenderObject>& out);

    int   m_tooltipWidget = 0, m_tooltipElem = 0;
    float m_tooltipHeld = 0.0f;
    bool  m_tooltipUp = false;

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
    bool m_wasSecondaryDown = false;   // the right button, for its own edge
    bool m_pointerOverUI = false;  // last processPointer verdict (see pointerOverUI)
    // Starts true: the first frame after anything is created has never been
    // drawn, so "nothing changed since the last draw" is false by construction.
    // What roles resolve to. A copy rather than a pointer: a theme asset can be
    // reloaded or deleted under a running application, and a dangling theme is
    // an application that draws in whatever was in that memory.
    HE::UITheme           m_theme = HE::uiDefaultTheme();
    HE::UIThemePreference m_themePref = HE::UIThemePreference::System;
    // What the desktop last said. Dark until the host says otherwise: a tool
    // that flashes white on a dark desktop for one frame is the thing "follow
    // the system" exists to avoid.
    HE::UIThemeMode       m_systemMode = HE::UIThemeMode::Dark;
    bool m_visualDirty = true;     // see consumeVisualDirty
    int  m_focusWidget = 0;        // widget id owning the focused TextInput
    HE::UICursor m_hoverCursor = HE::UICursor::Default; // cursor the hovered element wants
};
