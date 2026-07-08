#pragma once
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
};

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
    float   posX = 0.0f,  posY = 0.0f;
    float   sizeX = 120.0f, sizeY = 32.0f;
    float   pivotX = 0.5f, pivotY = 0.5f;
    uint8_t anchor = 0;         // 9-point (0 = TopLeft … 8 = BottomRight)
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

    // Type-specific editable properties + generic access by name.
    virtual std::vector<UIPropDesc> properties() const = 0;
    virtual UIPropValue getProp(const std::string& name) const = 0;
    virtual void        setProp(const std::string& name, const UIPropValue& v) = 0;

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
        dst.pivotX = pivotX; dst.pivotY = pivotY; dst.anchor = anchor;
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
