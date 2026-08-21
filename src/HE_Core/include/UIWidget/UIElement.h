#pragma once
#include <cstdint>
#include <Renderer/UIRenderObject.h>
#include <Types/UUID.h>
#include <Types/Defines.h>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace HE {

// Mouse cursor shown while a UI element is hovered (backend-agnostic; the app
// maps it to a system cursor). Default = leave the cursor unchanged.
enum class UICursor : uint8_t
{
    Default = 0, Arrow, Hand, Text, Crosshair, ResizeWE, ResizeNS, Move, No, Wait, COUNT
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
};

struct UIWidgetRect { float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f; };

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

    // Optional node-graph material on the quad (empty = solid color). Shared
    // storage; only types with hasMaterialSlot() expose it in the editor.
    std::string material;

    // Optional Font asset (content-relative path) for this element's text; empty
    // = the shared default UI font. Resolved to fontAtlasKey at widget creation.
    std::string font;
    uint32_t    fontAtlasKey = 0; // transient: baked-atlas key (not serialized)

    // Pointer interaction: hitTestable false = transparent to the mouse (never
    // hovered/clicked, pointer passes through). hoverCursor = the cursor the app
    // shows while this element is hovered (Default = unchanged).
    bool        hitTestable = true;
    UICursor    hoverCursor = UICursor::Default;

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

    // Events this type can fire (Designer "add event" + HorizonCode).
    virtual std::vector<UIEventDesc> events() const { return {}; }
    virtual bool hasMaterialSlot() const { return false; }

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
    virtual void applyAutoSize(float resolvedWidth) { (void)resolvedWidth; }

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
        dst.font = font; dst.fontAtlasKey = fontAtlasKey;
        dst.hitTestable = hitTestable; dst.hoverCursor = hoverCursor;
    }
};

// Factory + registry (JSON load, editor palette).
HE_API std::unique_ptr<UIElement> makeUIElement(UIWidgetType type);
HE_API const std::vector<UIWidgetType>& uiWidgetTypeRegistry();
HE_API const char* uiWidgetTypeName(UIWidgetType t);
HE_API UIWidgetType uiWidgetTypeFromName(const std::string& s);

} // namespace HE
