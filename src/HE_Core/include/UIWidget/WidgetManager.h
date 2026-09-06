#pragma once
#include <UIWidget/UIWidgetTree.h>
#include <UIWidget/UIWidgetAnim.h>   // UIEase — animate() takes one
#include <UIWidget/UIElements.h>
#include <UIWidget/UIWidgetBinding.h>
#include <UIWidget/AppMenu.h>        // the application's menu bar (plan A6)
#include <UIWidget/UIWindowFrame.h>  // UIWindowHit — windowHitAt (plan F3)
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Renderer/UIRenderObject.h>
#include <Types/UUID.h>
#include <Types/Defines.h>   // HE_API — HorizonCore exports explicitly, see below
#include <memory>
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
// HE_API because this class lives in HorizonCore, which (unlike HorizonScene)
// is NOT built with WINDOWS_EXPORT_ALL_SYMBOLS: without it the editor, the game
// host and the tests find no symbols to link against on Windows. The nested
// types (StateSnapshot, NavDir, TextEdit, …) need no mark of their own — they
// are enums or have only inline members.
class HE_API WidgetManager
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

    // ── The language everything is written in ────────────────────────────────
    // The catalog every widget's bound text resolves against (see
    // UIElement::textKeys and uiApplyTextCatalog). Exactly the shape the theme
    // above has, and for the same reason: setting either re-resolves EVERY live
    // widget straight away, which is what makes switching language a switch
    // rather than a reload.
    //
    // The default language is empty, which resolves to the catalog's fallback:
    // an application that never sets one still shows its catalog's base
    // language rather than the keys.
    void setTextCatalog(const HE::UITextCatalog& catalog);
    const HE::UITextCatalog& textCatalog() const { return m_catalog; }
    void setLanguage(const std::string& lang);
    const std::string& language() const { return m_language; }

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
    // The script instance of the component embedded in the slot called
    // `elementName` (0 = no such slot, or nothing grafted into it). A component
    // IS another instance, so this is the reference every by-reference node
    // wants: Call Function, Bind Event, Get/Set (Ref). The same handle
    // listRow hands out for a row, for a slot that was placed in the designer.
    HorizonCode::InstanceId childInstance(int widgetId, const std::string& elementName) const;
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

    // ── The application's menu bar (plan A6) ─────────────────────────────────
    // A strip along the top of the render target, drawn by the MANAGER and not
    // by any widget: it belongs to the application, and no page should have to
    // contain it in order for the application to have one. Setting it replaces
    // it whole — the API that fills it (app.addMenu…) rebuilds rather than
    // patches, because a menu usually changes as a set.
    //
    // It OVERLAYS the canvas rather than shrinking it, and eats the pointer in
    // its own band. Shrinking would mean an origin on UIWidgetCanvas, which
    // every rect computation in HE_Core reads — a bigger change than the bar,
    // and one to make on purpose rather than in passing. Until then a page that
    // wants to sit clear of the bar asks how tall it is.
    void setMenuBar(std::vector<HE::AppMenu> menus);
    const std::vector<HE::AppMenu>& menuBar() const { return m_menuBar; }
    // Render-target pixels; 0 when there is no bar. Fixed, like the tooltip's
    // metrics: the bar is chrome, not content, and it does not scale with a
    // canvas it is not part of.
    float menuBarHeight() const;
    // On macOS the same menus go into the SYSTEM bar instead (AppMacMenu, in
    // HE_Game — the Cocoa half cannot live here). Then the strip must not also
    // be drawn: two bars carrying the same entries is not twice the feature,
    // and the system one is not inside the window, so nothing is left to make
    // room for either. With this on, menuBarHeight() is 0, nothing is drawn,
    // and the band belongs to the page again. Off macOS it stays false.
    void  setMenuBarNative(bool on);
    bool  menuBarNative() const { return m_menuNative; }

    // ── The chord that chooses an entry without opening the menu ─────────────
    // The whole shortcut path, in one call: the host hands over the key it just
    // saw (an SDL_GetKeyName string) and the three modifier flags, and the first
    // entry whose chord that is fires OnMenuItem — the SAME door the click uses,
    // and for the reason "open with" comes through the same door as a drop. A
    // second event would be a second thing to keep in step with the first.
    //
    // Returns true when an entry OWNS the key, so the caller stops looking and
    // lets nothing behind it see the press.
    //
    // Owning and firing come apart on exactly one platform. While the SYSTEM
    // draws the bar, AppKit has already chosen the entry off its own key
    // equivalent, and SDL reports the same press anyway (Cocoa_DispatchEvent
    // runs before [super sendEvent:]) — so there it answers true and fires
    // NOTHING. A manager that also fired would choose the entry twice on one
    // keystroke, and one that answered false would let the key fall through to
    // the game behind a menu that just acted on it. Both halves of the trap, one
    // line apart.
    bool  fireMenuShortcut(const std::string& keyName, bool ctrl, bool shift, bool alt);
    // Who would take it, without firing anything — the id, or empty. Answers the
    // same on every platform, native bar or not: this is "whose chord is this",
    // and the bar is the same bar either way.
    std::string menuShortcutTarget(const std::string& keyName, bool ctrl,
                                   bool shift, bool alt) const;
    // Which menu is open (-1 = none). The strip's own state, not a widget's.
    int   openMenu() const { return m_menuOpen; }
    // Where a title sits, for a caller that has to aim at one (a test, and later
    // an editor overlay). A title is as wide as its word, so this is asked and
    // never assumed.
    bool  menuTitleBox(std::size_t index, float& x, float& width) const
    { return menuTitleRect(index, x, width); }
    // …and where the OPEN menu's card sits. False when none is open. Same
    // reason as the title's: a row is as wide as its label and its chord
    // together, and a caller that wants to say so has to be able to ask.
    bool  menuPopupBox(float& x, float& y, float& width, float& height) const
    { return menuPopupRect(x, y, width, height); }

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

    // ── Keeping what the preview holds across a reload (plan E4, Stufe 3) ────
    // The live preview is rebuilt from the assets every time one is saved, and
    // that throws away everything the person had typed, scrolled and picked.
    // Correct for "restart", wrong for "I fixed a label" — which is most saves.
    //
    // Two halves, matched by two different keys, because they are two different
    // questions:
    //
    //   ELEMENTS are matched by (asset path, which copy of it, element id). An
    //   id is handed out once by the designer and never renumbered, so it
    //   survives every edit that is not a delete — INCLUDING a rename, which is
    //   why renaming a field does not lose what was typed into it.
    //
    //   VARIABLES are matched by name on the widget's script instance. A graph
    //   has no stable id for a variable; the name IS its identity, so renaming
    //   one is indistinguishable from deleting it and adding another, and its
    //   value is gone. That is the one loss worth saying out loud, and the
    //   editor says it rather than leaving it in a comment.
    //
    // Anything unmatched keeps what the asset was authored with, which is what a
    // fresh widget is. Nothing here restores RUNNING work: a Delay that was
    // counting down is gone with the instance that was counting it.
    struct StateSnapshot
    {
        // Which live widget a row belongs to: the asset, plus which copy of it
        // in creation order. Two copies of one widget on a page are ordinary,
        // and keying on the path alone would give both of them the first one's
        // state.
        struct Key
        {
            std::string asset;
            int         copy = 0;
            bool operator==(const Key& o) const { return asset == o.asset && copy == o.copy; }
        };
        struct Element
        {
            Key  key;
            int  elementId = 0;
            // What the element HOLDS, by the property names its own table uses.
            // Only what a person can put something into is captured: a label a
            // script wrote is not state, it is output, and restoring it would
            // paste yesterday's answer over a freshly computed one.
            std::vector<std::pair<std::string, HE::UIPropValue>> props;
            // The two pieces of state that are not properties: a text field's
            // caret, and how far a scrolling container has been scrolled.
            // -1 / no value = this element has neither.
            long long caret  = -1;
            float     scroll = 0.0f;
            bool      hasScroll = false;
        };
        std::vector<Element> elements;
        // The focused element per widget (0 = none). Its own list because focus
        // is a property of the WIDGET, not of the element that happens to hold
        // it, and there is exactly one per widget.
        std::vector<std::pair<Key, int>> focus;
        // The script instance's variables per widget, by name.
        std::vector<std::pair<Key, std::unordered_map<std::string, HorizonCode::Value>>> vars;
    };

    StateSnapshot captureState() const;
    // Write a snapshot back onto whatever is live now. Returns how many pieces
    // actually landed, so a caller can tell "restored" from "matched nothing"
    // — the second is what a restructured widget looks like, and it should be
    // sayable rather than silent.
    int restoreState(const StateSnapshot& snapshot);

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

    // Fire the "Tick" event (Float arg = dt) on every visible widget, and move
    // every running animation one step (see animate).
    void tick(float dt);

    // ── Animation ────────────────────────────────────────────────────────────
    // Move a property from where it is now to `to` over `seconds`, along a
    // curve. Floats, colours and Vec2s — which is fade (Render Opacity), slide
    // (Position), grow (Size), and every colour a type has.
    //
    // Written through setPropAny each tick, like everything else that changes a
    // widget, so nothing downstream needs to know an animation exists.
    //
    // On a themed element it is a script write stretched over time, and the
    // existing rule applies unchanged: while it runs it wins, because it
    // rewrites every tick; once it ends the theme reclaims the property at its
    // next apply (a mode switch, a graft). Deliberately no side effect on the
    // element's bindings — an animation that silently locked a property against
    // the theme would be action at a distance.
    //
    // `seconds <= 0` writes `to` at once and reports finished, which is what
    // makes a duration a knob an author can turn to zero.
    //
    // Starting one on a property that is already animating REPLACES it, from
    // wherever the value has got to — retargeting mid-flight is smooth, and the
    // replaced animation does NOT report finished. Cancelled is not finished.
    //
    // Returns false when the widget, the element or the property is not there,
    // or the property is of a type that cannot be interpolated.
    bool animate(int widgetId, int elemId, const std::string& prop,
                 const HE::UIPropValue& to, float seconds,
                 HE::UIEase ease = HE::UIEase::Linear);
    // Stop what is running on one property, or (empty `prop`) on the whole
    // element, or (elemId 0) in the whole widget. The value stays where it got
    // to: a stop is not a rewind. Returns how many were stopped.
    int  stopAnimations(int widgetId, int elemId = 0, const std::string& prop = {});
    // The same two, addressing the element by the NAME it carries in the
    // designer — the form a script has, since element ids are the asset's
    // private business. Nothing found = false / 0, like every other by-name
    // entry point here.
    bool animateNamed(int widgetId, const std::string& elemName, const std::string& prop,
                      const HE::UIPropValue& to, float seconds,
                      HE::UIEase ease = HE::UIEase::Linear);
    int  stopAnimationsNamed(int widgetId, const std::string& elemName,
                             const std::string& prop = {});
    // Is anything moving? The application asks every frame: an event-driven app
    // sleeps until something happens, and "the clock advanced" is not an event
    // it would otherwise hear — without this an animation would run at the idle
    // heartbeat's ten frames a second.
    bool isAnimating() const;

    // ── Authored clips (the Designer's timeline) ─────────────────────────────
    // Play one of the widget's own named animations. `loop` overrides what the
    // clip says when given; without it the clip decides, which is what makes
    // "this one loops" a property of the animation rather than of every call.
    //
    // Playing a clip STOPS any single-property animation on the properties it
    // drives — two writers on one property is the fight replace-on-same-property
    // exists to prevent, and a clip is the more specific instruction. The other
    // way round is the same rule: animate() replaces whatever was writing that
    // property, a clip's track included, and the clip keeps its other tracks.
    //
    // Restarting a clip that is already playing rewinds it to 0 rather than
    // stacking a second player.
    // `dir` runs it forwards, backwards, or out and back; `restore` puts the
    // properties it drove back the way they were before anything animated them
    // when it FINISHES — not when it is stopped by hand, and never for a loop,
    // which never finishes. See Instance::originals for what "the way they
    // were" means.
    bool playAnimation(int widgetId, const std::string& clip, const bool* loop = nullptr,
                       HE::UIAnimDirection dir = HE::UIAnimDirection::Forward,
                       bool restore = false);
    // Everything at once: the clips and the single-property animations. The one
    // a graph reaches for when a screen is being torn down or replaced, so it
    // does not have to name what it started. Values stay where they got to;
    // restoreOriginalState is the one that puts them back.
    int  stopAllAnimations(int widgetId);
    // Stop everything and put every property an animation ever touched back the
    // way it was. Stopping first is not optional: a clip left running would
    // overwrite the restored values on the very next tick, which would look
    // like the node doing nothing.
    //
    // Returns how many properties were put back (0 = nothing had been animated).
    int  restoreOriginalState(int widgetId);
    // The widget a HorizonCode instance belongs to, or 0. What lets an engine
    // call from a widget's own graph mean "this widget" when nobody wired the
    // pin — the same courtesy the entity rows do for their Target.
    int  widgetIdForScript(HorizonCode::InstanceId scriptId) const;
    // Stop one clip, or (empty name) every clip of the widget. Values stay where
    // they got to, and nothing is reported — cancelled is not finished.
    int  stopAnimationClip(int widgetId, const std::string& clip = {});
    bool isPlayingAnimation(int widgetId, const std::string& clip = {}) const;
    // Where a playing clip stands, in seconds (-1 = it is not playing). What a
    // timeline's playhead reads back.
    float animationTime(int widgetId, const std::string& clip) const;

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

    // ── Files dragged in from the desktop (docs/he-apps-plan.md B7) ──────────
    // The OS drag is a gesture in three parts and these are two of them: while
    // it hovers, and when it is let go. Both take the pointer in the same
    // render-target pixels processPointer does, and both resolve the element
    // through the same hit test, because a drop that lands somewhere other than
    // where the highlight promised is worse than no highlight at all.
    //
    // dropHover marks the element that WOULD take it (`false` clears the mark,
    // which is what a drag leaving the window means). Returns true when there is
    // a target — the app turns that into the OS "this is allowed" cursor.
    bool dropHover(float vpWidth, float vpHeight, float x, float y, bool active);
    // …and the drop itself: one OnFileDropped per path on the first element at
    // or above the pointer that accepts drops. Nothing there? Then the WINDOW
    // took it, and the event goes to the GameInstance with elem 0 — an app that
    // opens whatever it is given wants exactly one place to say so, not one per
    // panel. Returns true when an element took it.
    bool processDrop(float vpWidth, float vpHeight, float x, float y,
                     const std::vector<std::string>& paths);

    // ── Dragging INSIDE the application ──────────────────────────────────────
    // Driven entirely by processPointer, because it is made of a press, a move
    // and a release and those are already there. What is public is only what
    // somebody outside has to know or be able to say.
    //
    // Is something being carried right now, and what. 0 when nothing is.
    bool isDragging() const { return m_dragActive; }
    int  dragSourceElement() const { return m_dragActive ? m_dragElem : 0; }
    // Put it back: Escape, the window losing focus, a reload under a running
    // drag. Fires OnDragEnded(false) on the source, exactly like a release over
    // nothing, because that is what it is.
    void cancelDrag();

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
    // Home and End are per LINE, which in a single-line field is the whole
    // field — so they answer exactly as they always did. Up and Down do nothing
    // unless the field is multiline (B1b).
    enum class TextEdit { Left, Right, Home, End, Delete, SelectAll,
                          WordLeft, WordRight, DeleteWordLeft, Up, Down };
    // True when something changed (and OnTextChanged fired where it applies).
    bool editFocusedText(TextEdit op, bool extendSelection);
    // Extend the selection to where the pointer is, without moving the anchor —
    // the drag half of click-and-drag selection. The pointer is in render-target
    // pixels like every other coordinate here. False when nothing moved.
    //
    // Y matters since fields can hold more than one line (B1b) and is a real
    // argument rather than a defaulted zero, which would silently mean "line
    // one" at any call site that had not learned about it.
    bool dragCaretFromPointer(float vpWidth, float vpHeight, float mouseX, float mouseY);
    // Select the word (or the whole line) under the pointer: double- and
    // triple-click. Both leave the caret at the end of what they selected.
    bool selectWordAtPointer(float vpWidth, float vpHeight, float mouseX, float mouseY);
    // What a DOUBLE-click means where it is not text: the row under the pointer
    // is activated (OnRowActivated). The app tries the text field first and
    // falls through to here, so one gesture keeps one meaning per thing it lands
    // on. False = nothing under the pointer had an "open me".
    bool activateAtPointer(float vpWidth, float vpHeight, float mouseX, float mouseY);
    bool selectAllFocused();
    // Ctrl+Z / Ctrl+Shift+Z (and Ctrl+Y) inside a text field: take back or put
    // back one group of edits in the FOCUSED field only. A group is a run of
    // typing or of deleting; a caret move, a click or a change of focus ends it.
    // False when there is nothing left on the respective stack, or when the
    // field is read-only. See UITextInput::undoEdit for what a group is.
    bool undoFocusedText();
    bool redoFocusedText();
    // What is selected in the focused field, for a copy or a cut.
    std::string focusedSelection() const;
    // Drop it (the second half of a cut). False when nothing was selected.
    bool deleteFocusedSelection();
    // Put the caret where a click landed, in render-target pixels like every
    // other pointer coordinate here.
    bool setCaretFromPointer(float vpWidth, float vpHeight, float mouseX, float mouseY);
    // True while a TextInput has keyboard focus — the apps use it to decide
    // whether to route text/keys here instead of to gameplay/camera.
    //
    // It used to answer `m_focusWidget != 0`, which is "something is focused",
    // and that is not the same question at all: setFocus writes m_focusWidget
    // for ANY element, so focusing a BUTTON claimed the keyboard. The arrow keys
    // are gated on this in both apps, so the first press moved the focus and the
    // second one was never routed — menu navigation worked exactly once. It also
    // started SDL text input, and swallowed every key from gameplay, for a
    // focused button.
    bool hasFocusedTextField() const;

    // The cursor the currently-hovered element requests (set by processPointer;
    // Default when nothing is hovered). The app maps it to a system cursor.
    HE::UICursor hoverCursor() const { return m_hoverCursor; }

    // ── What a borderless window's frame answers (docs/he-apps-plan.md F3) ───
    // An application without a system title bar has to tell the OS which parts
    // of its own picture stand in for the frame. This is that answer, for ONE
    // point, and it is the whole contract: the host installs it as SDL's hit
    // test and does nothing else.
    //
    // x, y, vpWidth and vpHeight are DRAWABLE PIXELS — the same space
    // processPointer works in, not the window points SDL hands the hit test.
    // Converting is the host's job because the host is where sx/sy already live.
    // `borderPx` is in that same space; 0 turns edge resizing off.
    //
    // Three rules, in this order: anything interactive keeps its click (a button
    // in the title bar is a button first), an edge band beats everything else,
    // and a press that is already in flight on a widget can never turn into a
    // window move halfway through.
    HE::UIWindowHit windowHitAt(float vpWidth, float vpHeight,
                                float x, float y, float borderPx);

    // ── Keyboard / gamepad navigation ────────────────────────────────────────
    // A menu has to be usable without a mouse. The focus moves SPATIALLY: from
    // the focused element towards the given direction, the nearest interactive
    // element in that direction wins, so a grid of buttons navigates the way it
    // looks rather than in tree order. With nothing focused yet, the first
    // direction press takes the topmost candidate.
    // …except where a LAYER has taken the input. What up and down mean is
    // decided by what is on top: with a dropdown open they step through its
    // options, and moving the focus to the button behind it would be answering
    // a question nobody asked. Same idea as the focus trap in a modal, one
    // level further in.
    enum class NavDir { Up, Down, Left, Right };
    // True when the focus moved (or a focused slider took the step, or an open
    // list moved its highlight) — false means nothing here wanted the key and
    // the caller may still have it.
    bool navigate(NavDir dir, float vpWidth, float vpHeight);
    // Fire what a click would fire on the focused element: a button clicks, a
    // checkbox toggles, a combo opens — or, with a list already open, the
    // entry the keys walked to is taken. False when nothing is focused.
    //
    // On a TEXT FIELD it starts EDITING, which is a state of its own. Tab walks
    // onto a field without giving it the keyboard, and it takes the keyboard
    // only when somebody says so — that is what stops the tab order dying in
    // the first search box it meets, and it is how a field behaves under a
    // gamepad everywhere. See isEditingText.
    bool activateFocused();

    // ── Focused, and editing, are two states ─────────────────────────────────
    // A field with the focus wears the ring and is where Tab left off. A field
    // being EDITED owns the keyboard: the letters, the arrows and the caret.
    // Between them lies one confirmation — Enter, Space, the pad's south
    // button, or a click, which says the same thing with a mouse.
    //
    // Without the distinction Tab moved INTO a field and never out of it: the
    // field had the keyboard from the moment it was reached, so every key after
    // that was text, Tab included.
    bool isEditingText() const;
    // Leave the field without moving the focus (Escape). False when nothing was
    // being edited, so a caller can fall through to what Escape means next.
    bool stopEditingText();

    // ── Tab order ────────────────────────────────────────────────────────────
    // The other way through a form, and the one people actually use in one:
    // spatial navigation answers "what is below this", Tab answers "what is
    // NEXT", and in a form those are different questions. The order is the
    // HIERARCHY's — depth first, parents before children, exactly the order the
    // designer's tree shows — because that is the order an author arranges and
    // the only one they can predict.
    //
    // Wraps at both ends, stays inside the widget that owns the input (the
    // topmost layer), and works while a text field has the keyboard: leaving a
    // field is what Tab is for.
    //
    // With a list hanging open it steps through the LIST instead — the layer on
    // top decides what a key means, and inside an open list Tab is the down
    // arrow. Escape leaves it without choosing, Enter takes the row.
    bool focusNext(bool backwards, float vpWidth, float vpHeight);

    // True while a list hangs open. The apps ask because the arrow keys then
    // belong to it even if a text field holds the focus.
    bool hasOpenDropdown() const;
    // The focused element of the focused widget (0 = none). The focus ring is
    // drawn around it in extract().
    int  focusedElement() const;
    // …and which widget it belongs to (0 = none). Element ids are per WIDGET,
    // so an element id alone does not say where the focus is — two widgets both
    // have an element 1.
    int  focusedWidget() const { return m_focusWidget; }
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

    // ── The system scaling of the screen these widgets are on ────────────────
    // How many real pixels one device-independent pixel is worth: SDL's
    // `SDL_GetWindowDisplayScale`, which is the window's pixel density times
    // the display's content scale, so 200% on Windows and a Retina panel on
    // macOS both arrive here as 2. The host sets it once per frame (a display
    // scale can change while the window is open — dragging it to a second
    // monitor is enough), and only the ConstantPixel canvas mode reads it: the
    // other modes measure against the viewport, which already counted the
    // pixels. 1 is an unscaled display and the default, which is what a
    // headless test and the editor's simulated screen both want.
    void  setDisplayScale(float s) { m_displayScale = s > 0.0f ? s : 1.0f; }
    float displayScale() const { return m_displayScale; }

    // ── The reader's text size (docs/he-apps-plan.md B10) ────────────────────
    // A factor on every authored font size, and on nothing else: 1.25 is a
    // reader who wants larger text, and the buttons keep the corners and the
    // padding the designer gave them. The display scale above is the other
    // knob — that one is "this screen has more pixels" and it moves everything.
    //
    // It is runtime state on the manager and NOT a property of a tree: nothing
    // serializes it, so every .hasset written before this existed stays byte
    // for byte what it was, and one setting covers every widget at once, which
    // is what a reader is actually asking for.
    //
    // Clamped like setDisplayScale, and for the same reason: 0 or a negative
    // number is not a smaller font, it is text that has stopped existing. The
    // upper end is 3 — past that a label is bigger than the box it names and
    // the answer is a different layout, not a larger number.
    void  setFontScale(float s);
    float fontScale() const { return m_fontScale; }

private:
    // Every canvas in here goes through this, so the display scale is applied
    // in ONE place instead of at forty call sites that each have to remember.
    HE::UIWidgetCanvas resolveCanvas(const HE::UIWidgetTree& t, float vpW, float vpH) const
    { return HE::uiResolveCanvas(t, vpW, vpH, m_displayScale); }
    float m_displayScale = 1.0f;
    float m_fontScale = 1.0f;

    struct Instance
    {
        int id = 0;
        int zOrder = 0;
        // The asset this instance came from. Kept because a graft has to know
        // it: addChild grafting THIS widget into itself is a circle, and the
        // guard that catches it works on the chain of paths already visited.
        std::string assetPath;
        HE::UIWidgetTree  tree;    // live deep copy (scripts mutate it)
        // This widget's own theme, when its asset named one — null means the
        // application's. A shared_ptr because several instances of the same
        // asset share one parse, and because nothing may hold a reference into
        // a theme that a reload could move.
        std::shared_ptr<const HE::UITheme> theme;
        // What is currently moving in this widget. On the instance rather than
        // in one list on the manager, so destroying a widget takes its
        // animations with it and nothing has to remember to sweep.
        struct Anim
        {
            int            elem = 0;
            std::string    prop;
            HE::UIPropValue from, to;   // same type, checked when it starts
            float          t = 0.0f;    // seconds elapsed
            float          dur = 0.0f;  // > 0 — an instant one never gets here
            HE::UIEase     ease = HE::UIEase::Linear;
        };
        std::vector<Anim> anims;
        // A clip of this widget that is currently playing. `embed` is the index
        // into `embeds` whose clips it belongs to, or -1 for the widget's own —
        // an embedded component keeps its OWN clips with its own element
        // numbering, exactly as its graph keeps its own element references, and
        // the offset is applied when the clip is evaluated.
        struct Playing
        {
            int         embed = -1;
            std::string clip;
            // Seconds into the PASS, which is not the moment of the clip being
            // shown: a backward or ping-pong pass reads the clip from somewhere
            // else (uiAnimDirectedTime). Elapsed, so that wrapping a loop is
            // one modulo and not a direction-dependent special case.
            float       t = 0.0f;
            bool        loop = false;
            HE::UIAnimDirection dir = HE::UIAnimDirection::Forward;
            // Put the properties this clip drove back the way they were when it
            // finishes. Only on FINISHING: a clip that was stopped by hand did
            // not finish, and a looping one never does.
            bool        restore = false;
        };
        std::vector<Playing> playing;
        // What a property looked like before anything animated it. Written once,
        // when the first clip or tween to touch that property starts, and never
        // overwritten — so it is the state the widget was AUTHORED in, not the
        // state it happened to be in between two animations.
        //
        // Kept after an animation ends on purpose: "put it back the way it was"
        // is a thing somebody asks for long after the animation that moved it,
        // and the entry is the only memory of what "the way it was" means.
        struct Original
        {
            int             elem = 0;   // host-tree id (an embed's offset applied)
            std::string     prop;
            HE::UIPropValue value;
        };
        std::vector<Original> originals;
        // How far along the hover and the press of one element are, for the
        // elements whose "Transition" is greater than zero. Linear progress,
        // 0..1; the easing is put on when the render state is filled, so that
        // stopping and turning round mid-blend picks up where the value is and
        // not where the curve says it should be.
        //
        // A map with entries only while something is moving, and swept the
        // moment both values have arrived at 0 — isAnimating() reads whether it
        // is empty, and an entry that stays behind after the pointer left is an
        // event-driven application that never sleeps again.
        struct Blend { float hover = 0.0f; float press = 0.0f; };
        std::unordered_map<int, Blend> blends;
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
        // Splitter whose divider is being dragged. Its own field for the same
        // reason as the two above: one pointer stream, three different things
        // it can be doing, and a shared flag would make them fight.
        int draggingSplit  = 0;
        // ColorPicker being dragged, and WHICH of its three parts took hold:
        // 0 = the saturation/value field, 1 = the hue strip, 2 = alpha. The part
        // is decided once, at the press, and held for the whole drag — pulling
        // the pointer off the hue strip and across the field must keep changing
        // the hue, the same rule the slider and the divider already follow.
        int draggingColor = 0;
        int colorPart     = 0;
        // The scrollbar thumb being dragged, and how far below its TOP edge the
        // pointer took hold of it. Without the grab offset the thumb jumps so
        // its middle is under the pointer the moment it is touched, which reads
        // as the list moving on its own before the drag has started.
        int   draggingScroll = 0;
        float scrollGrabDy   = 0.0f;
        // What the wheel left behind: canvas units per second, per scrolling
        // element. A notch moves the box AND pushes it, and this is the push —
        // integrated and damped in tick(), erased the moment it has died down.
        //
        // Same discipline as `blends` above: entries exist only while something
        // is moving. One left behind is an event-driven application that never
        // sleeps again, because isAnimating() reads this map too.
        std::unordered_map<int, float> scrollVel;
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
            // The embedded asset's own animation clips, kept here rather than
            // merged into the host's: their tracks name elements by the LOCAL
            // ids of the widget they were authored in, and idOffset is what
            // turns those into this tree's. Merging would mean rewriting every
            // track on every graft and inventing a rule for two components that
            // both call a clip "FadeIn".
            std::vector<HE::UIAnimClip> animations;
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
    bool caretOffsetAtPointer(float vpWidth, float vpHeight, float mouseX, float mouseY,
                              size_t& outOffset);
    // Undo and redo differ by one bool and nothing else, so they share a body.
    bool stepFocusedTextHistory(bool redo);
    // Close the open undo group of whatever field currently has the focus. Used
    // where the focus is about to leave it and there is no `ti` in hand.
    void sealFocusedUndoRun();

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
    // Graft every WidgetRef's asset into `w`'s tree, and then everything those
    // bring with them. `rootChain` is what the widget being built is already
    // INSIDE of — normally just its own asset path — and each reference carries
    // its own ancestry from there, so a widget that embeds itself (directly or
    // in a circle) is refused rather than expanded forever, while two copies of
    // one component side by side are simply two copies.
    void embedWidgetRefs(Instance& w, ContentManager& content,
                         const std::vector<std::string>& rootChain);
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
        // MenuBar holds no widget (widget = 0), which is exactly what makes
        // every widget inert while a menu is open: takesInput compares against
        // the top of the stack, and nothing has id 0.
        enum class Kind : uint8_t { Modal, Popup, Dropdown, MenuBar };
        Kind kind = Kind::Popup;
        int  widget = 0;   // the widget that holds the input
        int  elem   = 0;   // Dropdown only: the ComboBox that is open
        // Put back when it closes. A dialog that leaves the focus wherever it
        // happened to end up is the classic dialog bug.
        int  prevFocusWidget = 0, prevFocusElem = 0;
        int  prevZOrder = 0;   // Modal only: it was raised to the top
    };
    std::vector<Grab> m_grabs;

    // ── The menu bar's own state and arithmetic ──────────────────────────────
    // One source for the rectangles, used by the draw AND by the pointer. The
    // day they are two is the day a menu opens under the title next to the one
    // that was clicked.
    std::vector<HE::AppMenu> m_menuBar;
    int   m_menuOpen  = -1;   // index into m_menuBar, -1 = closed
    int   m_menuHover = -1;   // item under the pointer in the open menu
    bool  m_menuNative = false; // the system bar draws it, so we do not
    // Left edge and width of a title in the strip.
    bool  menuTitleRect(std::size_t i, float& x, float& w) const;
    // The open menu's popup, and one of its rows.
    bool  menuPopupRect(float& x, float& y, float& w, float& h) const;
    // Which title is at this point (-1 = none), and which row of the open menu.
    int   menuTitleAt(float x, float y) const;
    int   menuItemAt(float x, float y) const;
    // Open / switch / close, keeping the grab stack in step.
    void  openMenuAt(int index);
    void  closeMenu();
    void  drawMenuBar(float vpWidth, float vpHeight, std::vector<UIRenderObject>& out);
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
    // The combo whose list is hanging open, or null; `owner` takes its instance
    // when it is not null. One place, because three keyboard paths ask the same
    // question and each of them getting the cast slightly wrong is three bugs.
    HE::UIComboBox* openDropdown(Instance** owner = nullptr);
    // The element of a widget by the name it carries in the designer (0 = none).
    int elementIdByName(int widgetId, const std::string& name) const;
    // The element called `name` inside what a script instance stands for: the
    // whole widget for its own graph, or only the slice of the host tree an
    // embedded component occupies. Null when there is none.
    const HE::UIElement* elementOfScript(HorizonCode::InstanceId scriptId,
                                         const std::string& name);
    // Which clip list a playing entry draws from, and the id offset its tracks
    // need. Null when the embed index is stale.
    const std::vector<HE::UIAnimClip>* clipsOf(const Instance& w, int embed,
                                               int& offset) const;
    // Record what a property was before an animation touched it — once, and
    // never again while the entry stands (Instance::originals).
    void rememberOriginal(Instance& w, int elem, const std::string& prop);
    // Put one recorded property back and drop the record.
    void restoreOne(Instance& w, int elem, const std::string& prop);
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
    // The theme THIS instance resolves against: its own when the asset named
    // one (UIWidgetTree::themeAsset), the application's otherwise. Shared and
    // const, so ten widgets pointing at one theme parse it once and none of them
    // can be left holding a dangling copy.
    const HE::UITheme& themeFor(const Instance& w) const
    { return w.theme ? *w.theme : m_theme; }
    // What the desktop last said. Dark until the host says otherwise: a tool
    // that flashes white on a dark desktop for one frame is the thing "follow
    // the system" exists to avoid.
    HE::UIThemeMode       m_systemMode = HE::UIThemeMode::Dark;
    // What bound text resolves to. Empty language = the catalog's fallback.
    HE::UITextCatalog     m_catalog;
    std::string           m_language;
    bool m_visualDirty = true;     // see consumeVisualDirty
    // The clock every indeterminate progress bar reads (UIElementRenderState::
    // time). Wrapped rather than left to grow: a float that has been counting
    // for a day has lost the resolution a 1.4-second cycle is made of.
    float m_uiClock = 0.0f;
    // …and whether any bar is actually spinning. Worked out ONCE per tick, by
    // the same pass that moves the clock, because two things need the answer —
    // "ask for another frame" and isAnimating() — and a scan per question would
    // be the same walk over every element twice.
    bool m_spinning = false;
    int  m_focusWidget = 0;        // widget id owning the focused element
    // …and whether that element, being a text field, currently owns the
    // KEYBOARD. Cleared by every focus change, so it can only ever be true for
    // the element the focus is on right now (see isEditingText).
    bool m_focusEditing = false;
    HE::UICursor m_hoverCursor = HE::UICursor::Default; // cursor the hovered element wants
    // The element a drag is currently hovering over, and would land on (0 = none).
    // Held across frames because the OS sends the position while the drag moves
    // and the drop only at the end: without remembering it, the highlight would
    // be gone by the time the file arrives.
    int m_dropWidget = 0, m_dropElem = 0;
    // ── The drag in flight, and the press that may yet become one ────────────
    // Armed on a press over a draggable element, active only once the pointer
    // has travelled far enough. The gap between the two is the whole reason a
    // draggable thing can still be clicked: a press is where a click begins as
    // well, and a drag that starts on the press eats every click there is.
    int   m_dragWidget = 0, m_dragElem = 0;
    bool  m_dragArmed = false, m_dragActive = false;
    float m_dragStartX = 0.0f, m_dragStartY = 0.0f;
    // The click this press would have been, suppressed because it turned into a
    // drag instead. Read and cleared by the release in the same call.
    bool  m_dragAteClick = false;
    static constexpr float kDragThreshold = 4.0f;   // pixels

    // ── The one hit test ─────────────────────────────────────────────────────
    // Topmost hit-testable element under a point, across every visible widget
    // that takes input: highest (widget zOrder, element sort key) wins, which is
    // the order the draw paints in. The pointer and the drop both come through
    // here, so what the highlight promises and what the drop reaches cannot
    // drift apart — they cannot be two pieces of arithmetic if they are one.
    struct PointerHit
    {
        Instance* widget = nullptr;
        int  elem = 0;             // the RAW hit, before any bubbling
        bool interactive = false;  // does it take pointer events itself?
        HE::UICursor cursor = HE::UICursor::Default;
    };
    PointerHit topmostHit(float vpWidth, float vpHeight, float x, float y);

    // The scrollbar thumb under the pointer in THIS widget, deepest first, and
    // how far below the thumb's top edge the pointer is. 0 = none.
    //
    // It asks the geometry rather than the hit test on purpose: a scroll box is
    // not hit-testable at all (it is a container, its children take the clicks)
    // and its bar still has to be grabbable. The bar is also drawn ON TOP of
    // everything in the box, so it takes the press before anything under it.
    int scrollThumbAtPointer(Instance& w, float vpWidth, float vpHeight,
                             float mouseX, float mouseY, float& grabDy) const;
    // Which link of a rich-text label a point lands on ("" = none). Its own
    // helper because two callers need it — the cursor and the click — and both
    // have to agree with what was drawn.
    std::string linkAtPoint(Instance& w, int elemId, float x, float y) const;
    // First element at or above `hitElem` that accepts a drop (0 = none).
    int dropTargetAt(Instance& w, int hitElem) const;
    // …and the same walk for the other half: the first that can be picked up.
    int draggableAt(Instance& w, int hitElem) const;
    // Mark (or unmark) what a drop would land on, and redraw only on a change.
    void setDropMark(int widgetId, int elemId);
    // Where a carried payload would land right now: the hit's drop target,
    // unless that is the source itself or something inside it — dropping a row
    // onto itself is not a move, and the picture must not promise that it is.
    int dragTargetUnder(float vpWidth, float vpHeight, float x, float y, Instance** outW);
    // End the carry: fires OnDrop on the target when there is one, then always
    // OnDragEnded on the source. One place, because "let go" and "give up" are
    // the same event with a different answer.
    void finishDrag(bool accepted, Instance* targetW, int targetElem);
};
