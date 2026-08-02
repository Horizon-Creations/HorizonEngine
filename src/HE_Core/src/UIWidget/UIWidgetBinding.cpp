#include <UIWidget/UIWidgetBinding.h>

namespace HE {

HorizonCode::PinType uiPropTypeToPin(UIPropType t)
{
    using P = HorizonCode::PinType;
    switch (t)
    {
        case UIPropType::Float:      return P::Float;
        case UIPropType::Int:        return P::Int;
        case UIPropType::Bool:       return P::Bool;
        case UIPropType::String:     return P::String;
        case UIPropType::Color:      return P::Color;
        case UIPropType::Vec2:       return P::Vec2;
        case UIPropType::StringList: return P::String; // lists aren't a pin type
    }
    return P::Float;
}

HorizonCode::Value uiPropToHcValue(const UIPropValue& v)
{
    using P = HorizonCode::PinType;
    HorizonCode::Value out;
    out.type = uiPropTypeToPin(v.type);
    switch (v.type)
    {
        case UIPropType::Float:  out.f = v.f; break;
        case UIPropType::Int:    out.i = v.i; break;
        case UIPropType::Bool:   out.b = v.b; break;
        case UIPropType::String: out.s = v.s; break;
        case UIPropType::Color:  out.col = v.col; break;
        case UIPropType::Vec2:   out.v2 = v.v2; break;
        case UIPropType::StringList:
            out.type = P::String;
            out.s = v.list.empty() ? std::string() : v.list.front();
            break;
    }
    return out;
}

// The third implementation of HorizonCode's coercion rule (the other two are the
// interpreter's `coerce` in HorizonCode.cpp and `hc::coerce*` in
// HorizonCodeGenSupport.h — see the notes there). It cannot simply call either:
// it coerces into UIPropValue, not into Value. It must follow the same rule
// though, so a property written from a graph gets the value the interpreter
// computed:
//   • ARRAYS pass through UNCOERCED — read the wanted type's raw field, never a
//     cross-type conversion driven by v.type (which for an array names the
//     ELEMENT type, so converting off it produced garbage: an Int-element array
//     landing on a Float property used to be read as (float)v.i).
//   • Otherwise only Float↔Int↔Bool convert.
UIPropValue uiHcValueToProp(const HorizonCode::Value& v, UIPropType want)
{
    using P = HorizonCode::PinType;
    UIPropValue out;
    out.type = want;
    switch (want)
    {
        case UIPropType::Float:  out.f = v.isArray ? v.f : (v.type == P::Int ? (float)v.i : (v.type == P::Bool ? (v.b ? 1.0f : 0.0f) : v.f)); break;
        case UIPropType::Int:    out.i = v.isArray ? v.i : (v.type == P::Float ? (int)v.f : (v.type == P::Bool ? (v.b ? 1 : 0) : v.i)); break;
        case UIPropType::Bool:   out.b = v.isArray ? v.b : (v.type == P::Float ? v.f != 0.0f : (v.type == P::Int ? v.i != 0 : v.b)); break;
        case UIPropType::String: out.s = v.s; break;
        case UIPropType::Color:  out.col = v.col; break;
        case UIPropType::Vec2:   out.v2 = v.v2; break;
        // StringList has no HorizonCode counterpart (see uiPropTypeToPin): a
        // String value becomes a one-entry list. An ARRAY value's items are NOT
        // spread into the list — that would be a new feature, not a coercion
        // rule, and no caller expects it today.
        case UIPropType::StringList: out.list = { v.s }; break;
    }
    return out;
}

} // namespace HE
