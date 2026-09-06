#pragma once
#include <algorithm>
#include <cstdint>
#include <Renderer/UIFont.h>       // the baked atlas + the UTF-8 walk the caret shares with it
#include <Renderer/UIRenderObject.h>
#include <Types/UUID.h>
#include <Types/Defines.h>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace HE {

// The container these elements live in. Declared here and defined in
// UIWidgetTree.h (which includes this file): a container that hides one of its
// children has to be able to ask which child it is, and that is the tree's
// knowledge, not the element's.
struct UIWidgetTree;

// Mouse cursor shown while a UI element is hovered (backend-agnostic; the app
// maps it to a system cursor). Default = leave the cursor unchanged.
enum class UICursor : uint8_t
{
    Default = 0, Arrow, Hand, Text, Crosshair, ResizeWE, ResizeNS, Move, No, Wait,
    // The two diagonals came with the borderless window frame (F3): a corner
    // grip that shows the horizontal arrows is a corner that promises one axis
    // and moves two. Appended rather than sorted in beside their neighbours,
    // because the value is what a saved widget holds.
    ResizeNWSE, ResizeNESW,
    COUNT
};
HE_API const char* uiCursorName(UICursor c);

// ── UI element model ─────────────────────────────────────────────────────────
// A UI Widget asset is a tree of UIElements. UIElement is the BASE class shared
// by every widget type (identity + layout); each concrete widget (Button,
// CheckBox, Slider, …) is a subclass that adds its own data, declares its own
// editable properties + fireable events, and knows how to draw itself. The
// editor builds a per-type detail panel from properties(); HorizonCode reads and
// writes those same properties by name; the runtime fires the events() an
// interaction produces. Adding a new widget type is a single new subclass.

enum class UIWidgetType : uint8_t
{
    Panel = 0,   // colored container
    Image,       // quad with tint (+ optional material)
    Text,        // static text run
    Button,      // clickable, hover/press states + label
    CheckBox,    // toggle + label
    Slider,      // draggable value in [min,max]
    ProgressBar, // read-only fill in [0,1]
    TextInput,   // editable single-line text field
    ComboBox,    // dropdown selection from a list
    // ── Layout containers ────────────────────────────────────────────────────
    // These do not draw: they PLACE their direct children, stacked along an
    // axis, and a child of one ignores its own anchors (the box decides). New
    // types are appended, never inserted — the on-disk form is the type NAME,
    // but the enum order is what every table below is indexed by.
    VerticalBox,
    HorizontalBox,
    ScrollBox,   // a vertical box whose content may be taller than it is
    // Another UI Widget asset, embedded here. The runtime grafts that asset's
    // tree in under this element and runs its logic as its own script instance,
    // which is what lets a health bar or a settings row be authored once and
    // used everywhere instead of copied.
    WidgetRef,
    // Nothing, with a size. It draws no pixel and takes no click; all it does
    // is occupy its slot, which in a layout box is what pushes the elements
    // after it along — down in a vertical box, right in a horizontal one. With
    // Slot Fill above 0 it eats the space left over instead, which is how one
    // pins something to the far end of a row.
    Spacer,
    // A list of arbitrary length, built from ONE authored row. It holds no
    // items: it holds a COUNT and a row template, realizes only the rows the
    // view can show, and asks its owner to fill each one. That is what lets a
    // list of ten thousand be ten elements instead of ten thousand.
    ListView,
    // A horizontal box that runs out of room and starts a new line. Tags,
    // chips, a toolbar that has to survive a narrow window — everything that is
    // a row until it cannot be one. Appended, never inserted.
    WrapBox,
    // Rows and columns, with spans. The container a FORM is made of: a label
    // column that fits its labels beside a field column that takes the rest.
    // Two stacked boxes can only fake that, and only by hand-matching sizes.
    Grid,
    // Pages behind a strip of tabs, one visible at a time. Its CHILDREN are the
    // pages and each child's NAME is its label — there is no second list to keep
    // in step, which is the way every parallel-list design eventually goes wrong.
    TabBox,
    // Two panes and a divider you can drag. The other half of "an app with a
    // sidebar": a Tab Box says which page, a Splitter says how much room.
    Splitter,
    // A month at a time, as a grid of days you can click. Built IN rather than
    // as a popup: a picker that has to hang out of its own rect is the manager's
    // business (see the ComboBox's open list), and a calendar that is simply an
    // element can be put in a form, in a panel, or — if somebody wants it
    // floating — in a modal from B4, which is the same thing without a second
    // mechanism.
    DatePicker,
    // A hue strip and a saturation/value field, out of the one shape this
    // system has. Same reason for being embedded as the calendar above.
    ColorPicker,
    COUNT
};

// Typed property/event value used across the editor, HorizonCode and events.
enum class UIPropType : uint8_t { Float, Int, Bool, String, Color, Vec2, StringList };

struct UIPropValue
{
    UIPropType type = UIPropType::Float;
    float       f = 0.0f;
    int         i = 0;
    bool        b = false;
    glm::vec2   v2{ 0.0f };
    glm::vec4   col{ 1.0f, 1.0f, 1.0f, 1.0f };
    std::string s;
    std::vector<std::string> list;

    static UIPropValue ofFloat(float v)             { UIPropValue r; r.type = UIPropType::Float;  r.f = v;  return r; }
    static UIPropValue ofInt(int v)                 { UIPropValue r; r.type = UIPropType::Int;    r.i = v;  return r; }
    static UIPropValue ofBool(bool v)               { UIPropValue r; r.type = UIPropType::Bool;   r.b = v;  return r; }
    static UIPropValue ofString(std::string v)      { UIPropValue r; r.type = UIPropType::String; r.s = std::move(v); return r; }
    static UIPropValue ofColor(const glm::vec4& v)  { UIPropValue r; r.type = UIPropType::Color;  r.col = v; return r; }
    static UIPropValue ofVec2(const glm::vec2& v)   { UIPropValue r; r.type = UIPropType::Vec2;   r.v2 = v; return r; }
};

// ── A property value on disk ─────────────────────────────────────────────────
// Every widget type serializes its own fields with their own key names, so a
// UIPropValue never needed a form of its own. Component parameters do need one:
// what a host stores against a parameter name is a value whose TYPE is only
// known by looking at the component, so the type has to travel with it.
//
// The type is written as its enum NAME, not its number: this is an on-disk
// format, and a number would silently change meaning the day a type is inserted
// into UIPropType rather than appended to it.
HE_API void       uiPropValueToJson(nlohmann::json& out, const UIPropValue& v);
HE_API UIPropValue uiPropValueFromJson(const nlohmann::json& o);

// The same value as type `want`. Needed where a stored value and the property
// it is written into can disagree — a component parameter outlives the property
// it names, and the property's type is the one the element actually reads.
// Only the conversions that mean something are made (a number is a number
// whichever slot holds it, a string is what a number prints as); everything
// else yields that type's zero rather than reinterpreting bytes.
HE_API UIPropValue uiPropValueCoerce(const UIPropValue& v, UIPropType want);


// One editable property (drives the editor detail panel + HorizonCode pins).
struct UIPropDesc
{
    std::string name;
    UIPropType  type;
    float       minV = 0.0f, maxV = 0.0f; // Float slider range when minV < maxV
    // String properties only: author over several lines (the editor shows a
    // multi-line box, so a literal newline can be typed into the value).
    bool        multiline = false;
};

// ── The properties every element has ─────────────────────────────────────────
// getPropAny/setPropAny serve two sets of names: the type's own table, and the
// BASE ones every element shares — "Visible", "Tooltip", "Corner Radius". The
// base half had no descriptor list because nothing needed to enumerate it: the
// details panel draws those rows by hand, each in the place it belongs.
//
// A component parameter does need one. "Show the help line only on the rows
// that have help" is the most ordinary thing a component wants, and it is
// "Visible" on one element — so a list of knobs that stopped at the type's own
// table would leave that out for no reason an author could see.
//
// Kept in step with getBaseProp/setBaseProp by a test, not by discipline: those
// two are an if-chain, and a name added to them and not here would simply never
// be offered.
HE_API const std::vector<UIPropDesc>& uiBaseProperties();

class UIElement;

// ── Per-type property table ──────────────────────────────────────────────────
// One row = one editable property: its descriptor plus the accessors that read
// and write the backing field. A widget type declares its properties ONCE as a
// table of these, and properties()/getProp()/setProp() are all served from it —
// before, every subclass kept three parallel hand-written lists (a describe
// list plus an if-chain each way) that could silently drift apart.
//
// *** THE NAMES IN THESE TABLES ARE AN ON-DISK FORMAT. *** UI Widget assets are
// serialized with them and HorizonCode graphs reference properties by name, so
// renaming a row breaks every saved widget and every graph that touches it.
// test_ui_widgets.cpp pins the full per-type name+type list.
struct UIPropSlot
{
    UIPropDesc  desc;
    UIPropValue (*get)(const UIElement&);
    void        (*set)(UIElement&, const UIPropValue&);
};
using UIPropTable = std::vector<UIPropSlot>;

// ── Declaring a table row ────────────────────────────────────────────────────
// A row is normally `uiprop::slot<&Class::field>({ "Name", UIPropType::X })`:
// the field's C++ type picks which UIPropValue slot carries the value, so the
// row is the ONE place the property exists. uiprop::custom() covers the rare
// property that is not a plain field (e.g. a bool exposed over an int field).
namespace uiprop {

template <class T> struct MemberOf;
template <class C, class F> struct MemberOf<F C::*> { using Class = C; };

// UIPropValue ⇄ field, one overload per field type a widget can expose.
inline UIPropValue read(float v)              { return UIPropValue::ofFloat(v); }
inline UIPropValue read(int v)                { return UIPropValue::ofInt(v); }
inline UIPropValue read(bool v)               { return UIPropValue::ofBool(v); }
inline UIPropValue read(const std::string& v) { return UIPropValue::ofString(v); }
inline UIPropValue read(const glm::vec2& v)   { return UIPropValue::ofVec2(v); }
inline UIPropValue read(const glm::vec4& v)   { return UIPropValue::ofColor(v); }
inline UIPropValue read(const std::vector<std::string>& v)
{ UIPropValue r; r.type = UIPropType::StringList; r.list = v; return r; }

inline void write(float& d, const UIPropValue& v)       { d = v.f; }
inline void write(int& d, const UIPropValue& v)         { d = v.i; }
inline void write(bool& d, const UIPropValue& v)        { d = v.b; }
inline void write(std::string& d, const UIPropValue& v) { d = v.s; }
inline void write(glm::vec2& d, const UIPropValue& v)   { d = v.v2; }
inline void write(glm::vec4& d, const UIPropValue& v)   { d = v.col; }
inline void write(std::vector<std::string>& d, const UIPropValue& v) { d = v.list; }

// A slot only ever runs on its own class (the table is reached through that
// class's propTable()), so the downcast is safe by construction.
template <auto M>
UIPropSlot slot(UIPropDesc d)
{
    using C = typename MemberOf<decltype(M)>::Class;
    return { std::move(d),
             [](const UIElement& e) { return read(static_cast<const C&>(e).*M); },
             [](UIElement& e, const UIPropValue& v) { write(static_cast<C&>(e).*M, v); } };
}

inline UIPropSlot custom(UIPropDesc d,
                         UIPropValue (*get)(const UIElement&),
                         void (*set)(UIElement&, const UIPropValue&))
{ return { std::move(d), get, set }; }

} // namespace uiprop

// One event a widget type can fire. `argType` is the type of the value the
// event carries (e.g. Slider OnValueChanged → Float); `hasArg` false = pure exec.
struct UIEventDesc
{
    std::string name;
    UIPropType  argType = UIPropType::Float;
    bool        hasArg  = false;
};

// Transient per-instance interaction state passed to render() (not serialized).
struct UIElementRenderState
{
    bool hovered = false;
    bool pressed = false;
    bool focused = false;
    // …and, for a text field, whether it is being EDITED rather than merely
    // stood on. Tab walks onto a field and leaves it alone; one confirmation
    // gives it the keyboard. A caret blinking in a field that is not taking
    // keystrokes is the picture lying about what the keys will do, so the caret,
    // the selection and the input method's preedit all hang on this and not on
    // `focused` (see WidgetManager::isEditingText).
    bool editing = false;
    // …and whether something is being dragged over it right now and would land
    // HERE if it were let go. Its own state and not `hovered`, because during a
    // drag the pointer is over an element without touching it: the hover is
    // about the mouse, this is about the payload.
    bool dropTarget = false;
    // …and whether THIS element is the one being carried. The thing under the
    // hand should look like it left its place, or a drag reads as a copy.
    bool dragging = false;

    // How far ALONG the hover and the press are, for an element with a
    // "Transition" time — 0 is untouched, 1 is fully there, and the values in
    // between are what a blend between the two colours is made of. Eased
    // already: the manager stores the progress linear and shapes it here, so a
    // renderer only ever mixes.
    //
    // -1 means "nobody is driving this", and that is the DEFAULT on purpose.
    // The designer's preview and the tests build their own render state and
    // know nothing about transitions; a 0 default would tell them every element
    // is un-hovered and quietly turn off every highlight outside the running
    // application. Read them through hoverAmount()/pressAmount(), which fall
    // back to the bools, and an untouched caller keeps drawing what it drew.
    float hoverT = -1.0f;
    float pressT = -1.0f;

    float hoverAmount() const { return hoverT >= 0.0f ? hoverT : (hovered ? 1.0f : 0.0f); }
    float pressAmount() const { return pressT >= 0.0f ? pressT : (pressed ? 1.0f : 0.0f); }

    // Seconds since the manager started, for the one thing that moves without
    // anybody asking it to: an indeterminate progress bar. ONE clock on the
    // manager rather than a phase on every element — a spinner has no state of
    // its own, it is a function of the time, and a phase per element would be a
    // number to keep, to serialize by accident, and to get out of step with the
    // spinner beside it. A caller that builds its own state (the designer's
    // preview, the tests) leaves it at 0 and sees the first frame of the cycle.
    float time = 0.0f;

    // The reader's own text size, as a factor on every authored font size —
    // 1 is what the designer drew, 1.5 is a reader who needs it bigger. It
    // rides HERE and not in `pxScaleY` on purpose: pxScaleY is the canvas
    // turning units into pixels, and it also stretches corner radii, tab
    // strips and stepper arrows. Text that grows while its rounding stays put
    // is the point; a whole interface zoomed is the display scale, and that
    // already exists (WidgetManager::setDisplayScale).
    //
    // 1.0 and not 0 for the same reason hoverT is -1: the designer's preview
    // and every test build their own state, and a 0 default would silently
    // draw every label at nothing.
    //
    // The rule that makes it usable rather than merely present: EVERY place
    // where a font size becomes pixels or gets measured must see this factor,
    // or the picture and the pointer say different things. The two that break
    // quietly are UITextInput::caretAtPoint (the caret lands on the wrong
    // character) and applyAutoSize (labels are cut off at 150 %), and both take
    // it as a trailing argument because they have no render state to read.
    float fontScale = 1.0f;

    // What an authored font size is worth in pixels, once. Every render() goes
    // through this instead of writing the product out, so there is one place to
    // look when the answer is wrong.
    float fontPx(float fontSize, float pxScaleY) const
    { return fontSize * pxScaleY * fontScale; }
};

struct UIWidgetRect { float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f; };

// ── The scrollbar of anything that scrolls ───────────────────────────────────
// A scroll box and a list view both draw a thumb on their right edge, and both
// worked it out with their own copy of the same six lines. The thumb is now ONE
// sum (uiScrollThumbRect) fed by this: what the element contributes is three
// numbers and a colour, and where the thumb lands is nobody's private business.
//
// That matters more than tidiness, because grabbing the bar asks the same
// question drawing it does. The day those are two sums is the day the bar is
// grabbed somewhere other than where it is drawn — the lesson tabLayout and the
// rich-text hit test each arrived at separately.
struct UIScrollBarStyle
{
    float     barWidth = 0.0f;   // canvas units; 0 = no bar at all
    float     inset    = 0.0f;   // the box's own padding: top, bottom and right
    float     extent   = 0.0f;   // total height of the content, canvas units
    glm::vec4 color{ 1.0f };
    // Where the TRACK starts, when that is not the same as the inset. A table's
    // header sits above the rows and the bar must not run up behind it, so the
    // top and the bottom stop being one number. Negative = "the inset", which
    // is what every scrolling element that has no band at its top says, and it
    // is last in the struct so the existing four-value inits stay four values.
    float     topInset = -1.0f;
};

// ── Base class ────────────────────────────────────────────────────────────────
class HE_API UIElement
{
public:
    // Identity + hierarchy.
    int         id = 0;
    int         parentId = 0;   // 0 = direct child of the canvas
    std::string name;

    // Layout — SHARED by every element (anchor/sizing is the same for all).
    //
    // The anchor is a RECTANGLE in the parent's space, both corners in 0..1 —
    // the same model UMG uses. On an axis where min == max it is a POINT: the
    // element keeps its size and pos is the offset from that point (what a
    // 9-point anchor always was). Where max > min the element is anchored to a
    // SPAN of the parent — a whole side, or the whole rect — and grows and
    // shrinks with it; pos/size then read as the offset and the size RELATIVE
    // to that span (size 0 = exactly the anchored span, negative = inset).
    // uiAnchorPreset* in UIWidgetTree.h names the sixteen useful rectangles.
    float   posX = 0.0f,  posY = 0.0f;
    float   sizeX = 120.0f, sizeY = 32.0f;
    float   pivotX = 0.5f, pivotY = 0.5f;
    float   anchorMinX = 0.0f, anchorMinY = 0.0f;
    float   anchorMaxX = 0.0f, anchorMaxY = 0.0f;
    int     layer  = 0;
    bool    visible = true;

    // ── The floor and the ceiling ────────────────────────────────────────────
    // Bounds on the size this element ENDS UP with, in its own units, applied to
    // the finished rectangle (uiElementRect) rather than to the size field. That
    // is deliberate and it is the only place they can work: on a stretched
    // anchor the size field is a negative inset and says nothing about how wide
    // the element is, and inside a layout container the field is not read at
    // all — the box decides. The rect is the one answer the hit test, the
    // renderer and the designer all read, so the bound belongs there.
    //
    // 0 = no bound, on both ends, which is what every element authored before
    // this carries. A max below a min loses: the floor is applied last, because
    // "never smaller than this" is the promise a layout can actually keep.
    //
    // Where the bound bites, the element keeps its PIVOT in place — the same
    // rule solveAxis already follows, so a centred element stays centred and a
    // left-pinned one stays where its left edge was.
    //
    // minSize used to be four numbers on the four container types, read only
    // while "Size To Content" was on. It is one pair here, it means the same
    // thing there, and it now also means something on a Panel and a Button.
    float   minSizeX = 0.0f, minSizeY = 0.0f;
    float   maxSizeX = 0.0f, maxSizeY = 0.0f;

    // Opacity of this element AND everything under it, multiplied down the
    // chain. This is what fades a whole menu in and out: one value on the root
    // panel instead of an animation per colour of every element in it.
    float   renderOpacity = 1.0f;

    // Seconds a change of interaction state takes: how long this element needs
    // to go from its resting colour to its hovered one, and from there to its
    // pressed one. 0 = at once, and that is the default, so every widget
    // authored before this draws exactly as it did.
    //
    // ONE number for both directions and both states. A hover that fades in
    // over a fifth of a second and out over a whole one is a thing a designer
    // sometimes wants and a thing nobody can keep in step across forty
    // elements; the single number is the one that gets set on a style and holds
    // for a whole application.
    float   transition = 0.0f;

    // Where this element sits in the TAB order, when the tree order is not what
    // the author means. Tab already walks the hierarchy depth-first (the order
    // the designer's tree shows), and that is right for nearly everything; this
    // is the escape hatch for the two cases it is not.
    //
    //   0  — the default: take my place from the tree. Byte-identical files.
    //   >0 — come first, in ascending order, ahead of every 0. Two fields side
    //        by side in a grid that reads down the columns, say.
    //   <0 — skipped by Tab. Only by Tab: a click, an arrow key (navigate) and
    //        setFocus still reach it, because "not on the tab route" and "not
    //        focusable" are different claims and conflating them would make a
    //        toolbar button unreachable to a pointer as well.
    int     tabIndex = 0;

    // Off = greyed out and inert: the element (and its subtree) still draws,
    // dimmed, but nothing in it hovers, clicks, drags or takes the keyboard.
    // A disabled button that still reacts is the classic UI lie, so this is
    // enforced in the hit test, not left to the widget types.
    bool    enabled = true;

    // Rotation in degrees, clockwise, about this element's pivot. It is a
    // RENDER transform: layout is computed unrotated (so a rotated element does
    // not shove its siblings around) and the finished rect is turned. It is
    // inherited — a rotated panel turns everything inside it, or a tilted card
    // would come apart into a tilted background with upright text on it.
    float   rotation = 0.0f;

    // ── Border ("Schicht 0", docs/he-apps-plan.md D5) ────────────────────────
    // An outline on the element's own surface, in pixels, drawn INSIDE the rect
    // and following the corner radius. 0 = none. Only the types that HAVE a
    // surface read it (the same test the material slot uses) — outlining a text
    // label would outline nothing.
    //
    // On the base rather than per type, because it is one authored idea: a
    // Panel, an Image and a Button that all carry a 1 px line should carry the
    // same property, not three that have to be kept in step.
    // Rounded corners on the element's own surface, in pixels; 0 = square, and
    // min(w,h)/2 is a circle. Authored rather than baked into each type's
    // render(): a Button used to hard-code 6 and a Panel could not be rounded at
    // all, which is a style decision the engine was making for its user.
    //
    // The types that HAVE a surface set their own default in their constructor,
    // so every widget authored before this existed still draws exactly as it did.
    //
    // One per corner, in CSS order: x = top-left, y = top-right, z =
    // bottom-right, w = bottom-left. A tab, a chat bubble and the top of a card
    // each round some corners and not others, and a single number could author
    // none of them. Authoring all four to the same value is still the common
    // case, and the editor and the on-disk form both keep it a one-liner.
    glm::vec4 cornerRadius{ 0.0f, 0.0f, 0.0f, 0.0f };
    // True when the four are the same number — the question the writer, the
    // editor's link toggle and every "is this plain?" check all ask.
    bool uniformCornerRadius() const
    {
        return cornerRadius.x == cornerRadius.y && cornerRadius.y == cornerRadius.z &&
               cornerRadius.z == cornerRadius.w;
    }
    float maxCornerRadius() const
    {
        return std::max(std::max(cornerRadius.x, cornerRadius.y),
                        std::max(cornerRadius.z, cornerRadius.w));
    }
    float     borderWidth = 0.0f;
    glm::vec4 borderColor{ 0.0f, 0.0f, 0.0f, 1.0f };
    // A linear fade across the same surface: off, the element is its own colour
    // throughout. The angle is clockwise from "down" — 0 fades top to bottom,
    // 90 left to right — because a vertical fade is what a button or a header
    // almost always wants.
    bool      gradient = false;
    glm::vec4 gradientColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    float     gradientAngle = 0.0f;   // degrees
    // 0 = linear, along the angle above. 1 = radial, from the middle of the
    // element out to its farthest corner — a glow behind a dialog, a spotlit
    // card, the soft middle of a button. The angle means nothing then, and the
    // editor stops offering it.
    int       gradientShape = 0;
    // ── Shadows ──────────────────────────────────────────────────────────────
    // A drop shadow is the element's own shape drawn again underneath it, in
    // one colour, offset and softened. Off by default, because a shadow on
    // everything is a shadow on nothing.
    bool      shadow = false;
    glm::vec4 shadowColor{ 0.0f, 0.0f, 0.0f, 0.45f };
    float     shadowBlur = 8.0f;      // canvas units of falloff
    float     shadowOffsetX = 0.0f;   // canvas units, positive = right
    float     shadowOffsetY = 3.0f;   // canvas units, positive = down
    // The same falloff cast INWARDS from the element's own edge, drawn on its
    // surface: a pressed key, a well, an inset field. Independent of the drop
    // shadow — an element may have both, one, or neither.
    bool      innerShadow = false;
    glm::vec4 innerShadowColor{ 0.0f, 0.0f, 0.0f, 0.45f };
    float     innerShadowBlur = 6.0f;

    // ── Bound to the theme ───────────────────────────────────────────────────
    // Property name → theme role name, for the properties whose value should
    // come from the theme rather than from a literal ("Color" → "Surface",
    // "Text Color" → "MutedText"). Empty for every element authored before
    // themes existed, and empty is what "I decided this colour myself" means.
    //
    // A generic map rather than a companion field per colour, because the
    // surface colour is called something different on every type ("Color",
    // "Tint", "Normal Color", "Back Color"): a per-field member would mean
    // touching all six types and would still miss the seventh.
    //
    // Resolved by ASSIGNMENT, not per frame: uiApplyTheme writes the role's
    // colour into the ordinary property, so the runtime, the designer preview,
    // the thumbnails and the software renderer all get themed colours through
    // the field reads they already do. One consequence worth knowing: a script
    // that writes a literal into a bound property is overwritten the next time
    // the theme or the mode changes.
    std::vector<std::pair<std::string, std::string>> themeRoles;
    // What the map says for `prop`, or an empty string.
    const std::string& themeRoleFor(const std::string& prop) const
    {
        static const std::string none;
        for (const auto& [p, r] : themeRoles) if (p == prop) return r;
        return none;
    }
    // Bind (role non-empty) or unbind (role empty).
    void setThemeRole(const std::string& prop, const std::string& role)
    {
        for (auto it = themeRoles.begin(); it != themeRoles.end(); ++it)
            if (it->first == prop)
            {
                if (role.empty()) themeRoles.erase(it); else it->second = role;
                return;
            }
        if (!role.empty()) themeRoles.emplace_back(prop, role);
    }

    // ── Bound to the text catalog ────────────────────────────────────────────
    // The same idea as themeRoles, for the same reason, resolved the same way:
    // property name → catalog key, and uiApplyTextCatalog writes the translation
    // into the ordinary property. So the runtime, the designer's preview and the
    // thumbnails all show translated text through the field reads they already
    // do, and switching language is one pass rather than a lookup at every draw.
    //
    // Generic over any String property, not a "Text Key" field on the types that
    // have text: a Tooltip is a sentence a person reads, a TextInput's
    // Placeholder is too, and a per-field member would have to be added to each
    // of them separately and would still miss the next one.
    //
    // Same consequence as a theme binding, and worth knowing: a script that
    // writes a literal into a bound property is overwritten at the next language
    // switch.
    std::vector<std::pair<std::string, std::string>> textKeys;
    const std::string& textKeyFor(const std::string& prop) const
    {
        static const std::string none;
        for (const auto& [p, k] : textKeys) if (p == prop) return k;
        return none;
    }
    void setTextKey(const std::string& prop, const std::string& key)
    {
        for (auto it = textKeys.begin(); it != textKeys.end(); ++it)
            if (it->first == prop)
            {
                if (key.empty()) textKeys.erase(it); else it->second = key;
                return;
            }
        if (!key.empty()) textKeys.emplace_back(prop, key);
    }

    // ── Bound to the theme, the other way round ─────────────────────────────
    // A role binding decides ONE value. This decides the element's whole look:
    // it says "I am a Button, dress me like the theme's buttons", which is the
    // difference between one decision and six — and it is the only way hover and
    // pressed can be themed at all, because no role vocabulary has a name for
    // "the accent, but hovered".
    //
    //   themeStyled == false → the theme's styles do not touch this element
    //   themeStyled == true, themeStyle empty → the style named after its TYPE
    //   themeStyled == true, themeStyle "Card" → that style
    //
    // Default true for an element that is CONSTRUCTED, false for one read from a
    // file that does not mention it: a widget authored before styles existed
    // must keep the colours somebody typed into it, while everything placed from
    // now on follows the theme without being asked to. A per-property role
    // binding still wins over the style, and kUIThemeLiteral shuts both out.
    //
    // themeTag is the CSS class of this: an element of its type, but a
    // particular KIND of one. A Button with the tag "success" takes "Button"
    // first and then "Button.success" on top of it, property by property — so a
    // variant that names one colour still gets the rounding from the base. That
    // layering is the whole reason a tag beats a second full style.
    bool        themeStyled = true;
    std::string themeStyle;
    std::string themeTag;

    // Only read when the PARENT is a layout container: 0 = keep my own size on
    // the box's axis, > 0 = take a share of whatever space is left over, split
    // between the filling children in proportion. Ignored everywhere else, and
    // that is deliberate — one field on the base beats a parallel "slot" object
    // that has to be kept in step with the element list.
    float   slotFill = 0.0f;

    // ── Which cell of a Grid this element sits in (docs/he-apps-plan.md B3) ──
    // -1 on either axis means "the next free cell", which is what almost every
    // child wants: fill a form top to bottom and never type a coordinate.
    // Setting them PINS the element, and pinned children are placed first, so an
    // explicitly placed one cannot have its cell taken by an automatic one that
    // happens to come earlier in the tree.
    //
    // The spans are how many cells it covers, at least 1. Ignored outside a
    // Grid, like slotFill is outside a box.
    int     gridColumn = -1, gridRow = -1;
    int     gridColumnSpan = 1, gridRowSpan = 1;

    // Optional node-graph material on the quad (empty = solid color). Shared
    // storage; only types with hasMaterialSlot() expose it in the editor.
    std::string material;

    // Optional Texture asset (content-relative path) drawn on the quad, tinted
    // by the element's own colour. Shared storage like `material`; only types
    // with hasTextureSlot() expose it. This is the plain "show me this PNG"
    // path — before it, an image in the UI needed a material graph with a
    // texture node in it. A material, when set, wins: it owns the pixels.
    std::string texture;
    HE::UUID    textureAssetId{}; // transient: resolved from `texture` at runtime
    // Source size in pixels, transient like the id above. Only 9-slicing needs
    // it (pixel margins have to become UVs somehow), and only the runtime and
    // the editor can know it — the element itself never loads anything.
    uint32_t    textureW = 0, textureH = 0;

    // Optional Font asset (content-relative path) for this element's text; empty
    // = the shared default UI font. Resolved to fontAtlasKey at widget creation.
    std::string font;
    uint32_t    fontAtlasKey = 0; // transient: baked-atlas key (not serialized)

    // Pointer interaction: hitTestable false = transparent to the mouse (never
    // hovered/clicked, pointer passes through). hoverCursor = the cursor the app
    // shows while this element is hovered (Default = unchanged).
    bool        hitTestable = true;

    // ── The title bar of a borderless window (docs/he-apps-plan.md F3) ───────
    // windowDrag true = pressing here moves the WINDOW, the way the title bar
    // the application asked the OS to leave off would have. It is deliberately
    // NOT hitTestable's opposite: a container that a click passes through can
    // still be the thing the window is carried by, and that is the normal case —
    // a title bar is a horizontal box with a caption and a few buttons in it.
    // Anything interactive on top of it keeps its click; see
    // WidgetManager::windowHitAt, which is the only reader.
    bool        windowDrag = false;

    // ── Tooltip (docs/he-apps-plan.md B4) ────────────────────────────────────
    // What this element says about itself when the pointer rests on it. Empty =
    // nothing to say, which is the case for almost every element, so it is a
    // string on the base rather than a type of its own: a tooltip is a sentence
    // about a control, not a control.
    //
    // The DELAY is the whole feature — a hint that appears instantly is a hint
    // that gets in the way — and it belongs to the manager, which is the only
    // thing that knows how long the pointer has been still.
    std::string tooltip;
    UICursor    hoverCursor = UICursor::Default;

    // Cut this element's descendants off at its own rect — pixels outside are
    // neither drawn nor hit-tested. Off by default: a widget whose children
    // stick out is a perfectly good design, and clipping costs a scissor change
    // per run of quads. This is what makes a list that is longer than its box,
    // or a text that overflows its field, look like one instead of spilling
    // across the screen.
    bool        clipChildren = false;

    // ── Where the focus ring goes ────────────────────────────────────────────
    // Set on an ANCESTOR: while anything inside it has the focus, the ring is
    // drawn around THIS element instead of around the focused one. CSS calls the
    // same idea :focus-within, and every search field ever built needs it.
    //
    // A control people see as one thing is often several: a rounded frame, an
    // icon, and a text field inset between them. The field is what takes the
    // keyboard, so the ring landed on the field — a small rectangle floating
    // inside a pill, marking a part nobody thinks of as the control. This is how
    // the frame says "I am the control", and it is a flag rather than something
    // clever, because which element that is cannot be worked out from the tree:
    // a form component holds a dozen fields and wants the ring on each of them.
    bool        focusFrame = false;

    // ── Can something be dropped on this? (docs/he-apps-plan.md B7) ──────────
    // Off by default, and that is the whole design: a drop has to be ACCEPTED
    // somewhere, and an element that never said so is transparent to it. The
    // drop then travels up from whatever the pointer hit to the first ancestor
    // that says yes — the same bubbling a click does, and for the same reason:
    // a file dragged onto the caption of a card is dragged onto the card.
    //
    // A flag rather than a widget type, because "drop a file here" is a thing a
    // panel, a list, an image and a text field all want to be, and none of them
    // wants to be a different type for it.
    bool        acceptsDrop = false;

    // ── …and can this one be PICKED UP? ──────────────────────────────────────
    // The other half of the same gesture. Also bubbling: a press on the caption
    // of a draggable card starts the card moving, not the caption.
    //
    // A drag begins at a DISTANCE, never at the press — anything else turns
    // every click on a draggable thing into a drag of it, and the press is
    // where a click begins too. Which distance is the manager's business
    // (kDragThreshold); that it is a distance is this comment's.
    bool        draggable = false;
    // What this element IS, in the eye of whatever it is dropped on. A string,
    // set by whoever knows: a list row writes its index in here when the drag
    // starts, a tool button its tool. Empty falls back to the element's NAME,
    // which is right often enough that the simple case needs no script at all.
    std::string dragPayload;

    virtual ~UIElement() = default;

    virtual UIWidgetType type() const = 0;
    virtual const char*  typeName() const = 0;
    virtual std::unique_ptr<UIElement> clone() const = 0;

    // Type-specific editable properties + generic access by name. A subclass
    // declares ONE table (see UIPropSlot) and gets all three accessors from it;
    // an unknown name reads as a default-constructed value and writes nowhere,
    // exactly as the old per-class if-chains did.
    virtual const UIPropTable& propTable() const = 0;
    std::vector<UIPropDesc> properties() const;
    UIPropValue getProp(const std::string& name) const;
    void        setProp(const std::string& name, const UIPropValue& v);

    // ── Shared base properties (every element) ────────────────────────────
    // The base fields (Visible, Hit Testable, Position, Size, Layer, Hover
    // Cursor, Material, Font) unified with the subclass's own list, so generic
    // consumers — Get/Set Property nodes, the runtime bindings, scripting —
    // can read and write EVERY property by name, not just the type-specific
    // ones. Base names are checked first; no subclass reuses them.
    std::vector<UIPropDesc> allProperties() const;
    UIPropValue getPropAny(const std::string& name) const;
    void        setPropAny(const std::string& name, const UIPropValue& v);

    // Events this type can fire (Designer "add event" + HorizonCode). A type
    // adds its OWN — a button's click, a field's text — and the base list
    // underneath belongs to every element there is.
    std::vector<UIEventDesc> allEvents() const
    {
        std::vector<UIEventDesc> out = events();
        // Anything can be animated, so anything can report that an animation
        // ended. It was not in the per-type lists because until animations
        // existed there was nothing a Panel or a Text could ever fire.
        out.push_back({ "OnAnimationFinished", UIPropType::String, /*hasArg=*/true });
        // …and anything can be a drop zone, for the same reason: "a file was
        // let go over me" is a thing that happens TO an element, not something
        // a particular type does. Whether it ever fires is Accepts Drop's
        // business, not this list's.
        out.push_back({ "OnFileDropped", UIPropType::String, /*hasArg=*/true });
        // The three of the gesture that happens INSIDE the application: it was
        // picked up, something was let go over me, and it is over. All three on
        // the base for the same reason as the two above — being dragged is
        // something that happens to an element, not something a type does.
        out.push_back({ "OnDragStarted", UIPropType::Bool, /*hasArg=*/false });
        out.push_back({ "OnDrop",        UIPropType::String, /*hasArg=*/true });
        out.push_back({ "OnDragEnded",   UIPropType::Bool, /*hasArg=*/true });
        return out;
    }
    virtual std::vector<UIEventDesc> events() const { return {}; }
    virtual bool hasMaterialSlot() const { return false; }
    // Types that draw a quad the user may want a picture on.
    virtual bool hasTextureSlot() const { return false; }

    // ── Does this type have a SURFACE to style? ──────────────────────────────
    // True for the types whose FIRST quad covers their whole rect — a
    // background, in other words. That is what the border, the gradient and
    // anything else in "Schicht 0" (docs/he-apps-plan.md D5) are applied to.
    //
    // Its own question, deliberately not `hasMaterialSlot()`: that one answers
    // "may a custom shader be put here", and the two sets are NOT the same.
    // A ProgressBar, a TextInput and a ComboBox all draw a proper background and
    // take no material; gating the style properties on the material slot offered
    // them to three types and quietly withheld them from three others that would
    // have worked. Two filters that can disagree is one filter too many.
    //
    // False for Text (glyphs, no surface), CheckBox and Slider (their first quad
    // is a PART — the box, the groove — not the element's rect), and for the
    // layout containers, which draw nothing at all.
    virtual bool hasSurfaceStyle() const { return false; }

    // ── May things be put INSIDE this element? ──────────────────────────────
    // The designer's containment rule, and nothing else: dropping onto such an
    // element parents the newcomer to it, dropping onto any other one makes a
    // sibling. Nesting itself is never forbidden by the tree — every element
    // has a parentId and the layout resolves it — so this is purely about what
    // the drop lands on.
    //
    // True for the Panel (the plain container), for the layout boxes (which
    // exist for nothing else), and for the Button: a Button is a SURFACE now,
    // and its caption, its icon and everything else on it are children. Saying
    // no here was what made it impossible to put a second thing on a button.
    virtual bool acceptsChildren() const { return false; }

    // A layout container PLACES its direct children: they ignore their own
    // anchors and position, and take the slot the box hands them. Returning
    // true here is what switches uiElementRect over for every child.
    virtual bool laysOutChildren() const { return false; }

    // ── Does this container hide that child of its own accord? ───────────────
    // A Tab Box shows one page at a time, so nine of its ten children are not
    // on screen — and "not on screen" has to mean the same thing to the picture
    // and to the pointer, or a button on a hidden tab answers a click at its
    // coordinates. That has bitten this codebase before, which is why this is a
    // question the PARENT answers and `uiElementEffectiveVisible` is the one
    // place that asks it. Every consumer already goes through that function.
    //
    // Distinct from the child's own `visible`: a page you switched away from is
    // not hidden, it is merely not the one showing, and switching back must not
    // have to remember what its flag used to be.
    //
    // It takes the TREE because the answer is "which child is this among my
    // children", and that is the tree's knowledge. Caching an index on the
    // element instead would be a number that can go stale, on the one path
    // where stale means a click reaching something invisible.
    virtual bool hidesChild(const UIWidgetTree&, const UIElement&) const { return false; }

    // ── Does this element scroll its own content? ────────────────────────────
    // Non-null means yes, and the float IS the offset (canvas units, 0 = the
    // top). The wheel, the per-frame clamp and the layout all ask THIS instead
    // of naming the types that scroll, so the scroll box and the list view are
    // one code path and a third scrolling container joins it for free.
    virtual float* scrollOffsetPtr() { return nullptr; }
    // How far it may be scrolled — 0 when everything already fits, which is
    // also what says "the wheel belongs to whatever is behind me".
    virtual float  maxScrollAmount() const { return 0.0f; }
    // …and what its scrollbar is made of. Answered by the same elements that
    // answer the two above; false means "no bar", which is also what a bar
    // width of 0 says. See UIScrollBarStyle for why this exists at all.
    virtual bool   scrollBar(UIScrollBarStyle&) const { return false; }

    // ── How far in from its OWN top edge this element cuts its children ──────
    // Canvas units, and only consulted where clipChildren is on. Zero for
    // everything except a table: its header band is drawn by the element and
    // the rows are drawn AFTER it, so a row scrolled half out of view would
    // otherwise paint straight over the column titles. Answering here rather
    // than shrinking the element's rect keeps the rect the layout gave it.
    virtual float  childClipTopInset() const { return 0.0f; }
    // Which way a container stacks (only asked when laysOutChildren()).
    virtual bool stacksVertically() const { return true; }

    // Emit draw quads for this element. `px` is the element rect in screen
    // pixels; `pxScaleY` maps canvas units → pixels for font sizing; `mat` is
    // the resolved material (nil = none). Text uses the shared UI font atlas.
    virtual void render(const UIWidgetRect& px, const UIElementRenderState& st,
                        const HE::UUID& mat, float pxScaleY,
                        std::vector<UIRenderObject>& out) const = 0;

    // True when a pointer press should toggle/activate this element (buttons,
    // checkboxes, combos, sliders, text fields). Panels/images/text/progress
    // are inert unless a script event binds them.
    virtual bool interactive() const { return false; }

    // Resize this element to fit its own content, when the type supports it and
    // the user asked for it. Run once per frame over the tree (uiApplyAutoSize)
    // BEFORE layout, so a text/font change taken at runtime — from a HorizonCode
    // Set Property, say — is reflected in the same frame. No-op by default.
    //
    // `resolvedWidth` is the element's laid-out width in canvas units. On an
    // axis the anchor STRETCHES, the parent decides the size and content must
    // not touch it — but a wrapping text still has to know how wide it may run,
    // and there the field alone no longer says (it is the difference to the
    // anchored span, not the width).
    //
    // `fontScale` is the reader's text size (UIElementRenderState::fontScale).
    // It has to arrive here too, because a box that fits the text at 100 % cuts
    // it in half at 150 % — "fit your content" is a question about the drawn
    // size, not about the authored one.
    virtual void applyAutoSize(float resolvedWidth, float fontScale = 1.0f)
    { (void)resolvedWidth; (void)fontScale; }

    // ── Which axes the element works out for itself ──────────────────────────
    // Bit 1 = width, bit 2 = height. One answer, given by the type, and two
    // readers who would otherwise drift apart: `uiApplyAutoSize` holds exactly
    // these axes between Min and Max Size (the others are authored and must not
    // be touched), and the designer greys the Size row for them rather than
    // offering a number that the next frame overwrites.
    //
    // The type answers because only the type knows what its own switches leave
    // to the author: a wrapping label authors its width (that IS the wrap
    // column) and measures only its height, and no axis a stretching anchor
    // owns is ever measured — the parent decides those.
    static constexpr int kAxisX = 1;
    static constexpr int kAxisY = 2;
    virtual int autoSizedAxes() const { return 0; }

    // The axes a stretching anchor owns, in the same bits. Used by the types
    // that measure themselves, so that their answer above and the skip inside
    // their applyAutoSize come from one place.
    int stretchedAxes() const
    {
        return (anchorMaxX > anchorMinX + 1e-4f ? kAxisX : 0)
             | (anchorMaxY > anchorMinY + 1e-4f ? kAxisY : 0);
    }

    // Type-specific JSON (base fields are handled by the tree serializer).
    virtual void writeJson(nlohmann::json&) const {}
    virtual void readJson(const nlohmann::json&) {}

protected:
    // Copy the shared base fields into a freshly-constructed subclass (used by
    // clone()). Keeps the boilerplate out of every subclass.
    void copyBaseTo(UIElement& dst) const
    {
        dst.id = id; dst.parentId = parentId; dst.name = name;
        dst.posX = posX; dst.posY = posY; dst.sizeX = sizeX; dst.sizeY = sizeY;
        dst.pivotX = pivotX; dst.pivotY = pivotY;
        dst.anchorMinX = anchorMinX; dst.anchorMinY = anchorMinY;
        dst.anchorMaxX = anchorMaxX; dst.anchorMaxY = anchorMaxY;
        dst.layer = layer; dst.visible = visible; dst.material = material;
        dst.minSizeX = minSizeX; dst.minSizeY = minSizeY;
        dst.maxSizeX = maxSizeX; dst.maxSizeY = maxSizeY;
        dst.renderOpacity = renderOpacity; dst.transition = transition;
        dst.tabIndex = tabIndex;
        dst.enabled = enabled;
        dst.slotFill = slotFill; dst.rotation = rotation;
        dst.gridColumn = gridColumn; dst.gridRow = gridRow;
        dst.gridColumnSpan = gridColumnSpan; dst.gridRowSpan = gridRowSpan;
        dst.texture = texture; dst.textureAssetId = textureAssetId;
        dst.textureW = textureW; dst.textureH = textureH;
        dst.font = font; dst.fontAtlasKey = fontAtlasKey;
        dst.hitTestable = hitTestable; dst.hoverCursor = hoverCursor;
        dst.windowDrag = windowDrag;
        dst.tooltip = tooltip;
        dst.clipChildren = clipChildren;
        dst.focusFrame = focusFrame;
        dst.acceptsDrop = acceptsDrop;
        dst.draggable = draggable; dst.dragPayload = dragPayload;
        dst.cornerRadius = cornerRadius;
        dst.borderWidth = borderWidth; dst.borderColor = borderColor;
        dst.gradient = gradient; dst.gradientColor = gradientColor;
        dst.gradientAngle = gradientAngle;
        dst.gradientShape = gradientShape;
        dst.themeRoles = themeRoles;
        dst.textKeys = textKeys;
        dst.themeStyled = themeStyled; dst.themeStyle = themeStyle;
        dst.themeTag = themeTag;
        dst.shadow = shadow; dst.shadowColor = shadowColor;
        dst.shadowBlur = shadowBlur;
        dst.shadowOffsetX = shadowOffsetX; dst.shadowOffsetY = shadowOffsetY;
        dst.innerShadow = innerShadow; dst.innerShadowColor = innerShadowColor;
        dst.innerShadowBlur = innerShadowBlur;
    }
};

// ── UTF-8 cursor movement ────────────────────────────────────────────────────
// Byte offsets that never land inside a multi-byte character. Every text-field
// operation goes through these: one press of Left has to step over a whole
// character, not over one of the bytes it is made of. They live in
// Renderer/UIFont.h, next to the glyph walk that has to agree with them about
// where a character begins.

// ── The scrollbar thumb, in the element's own pixel space ────────────────────
// `px` is the element's rect in screen pixels, the same rectangle render() is
// handed. False when there is no thumb to draw: no bar, no content, or content
// that already fits.
HE_API bool uiScrollThumbRect(const UIElement& e, const UIWidgetRect& px,
                              UIWidgetRect& out);
// The inverse: the scroll offset that would put the thumb's TOP at this pixel.
// Clamped to the element's range. It lives beside the sum above because it is
// that sum read backwards, and two functions in two files would drift.
HE_API float uiScrollOffsetForThumbTop(const UIElement& e, const UIWidgetRect& px,
                                       float thumbTopPx);

// Factory + registry (JSON load, editor palette).
HE_API std::unique_ptr<UIElement> makeUIElement(UIWidgetType type);
HE_API const std::vector<UIWidgetType>& uiWidgetTypeRegistry();
HE_API const char* uiWidgetTypeName(UIWidgetType t);
HE_API UIWidgetType uiWidgetTypeFromName(const std::string& s);

} // namespace HE
