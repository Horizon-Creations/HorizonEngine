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

namespace
{

// One helper per numeric target; each switch lists the source types that convert
// and falls back to the target's own raw field (a String, Color, ... value).
float asFloat(const HorizonCode::Value& v)
{
    using P = HorizonCode::PinType;
    switch (v.type)
    {
        case P::Int:    return (float)v.i;
        case P::Bool:   return v.b ? 1.0f : 0.0f;
        case P::Double: return (float)v.d;
        default:        return v.f;
    }
}

int asInt(const HorizonCode::Value& v)
{
    using P = HorizonCode::PinType;
    switch (v.type)
    {
        case P::Float:  return (int)v.f;
        case P::Bool:   return v.b ? 1 : 0;
        case P::Double: return (int)v.d;
        default:        return v.i;
    }
}

bool asBool(const HorizonCode::Value& v)
{
    using P = HorizonCode::PinType;
    switch (v.type)
    {
        case P::Float:  return v.f != 0.0f;
        case P::Int:    return v.i != 0;
        case P::Double: return v.d != 0.0;
        default:        return v.b;
    }
}

} // namespace

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
//   • Otherwise only Float↔Double↔Int↔Bool convert.
UIPropValue uiHcValueToProp(const HorizonCode::Value& v, UIPropType want)
{
    UIPropValue out;
    out.type = want;
    switch (want)
    {
        case UIPropType::Float:  out.f = v.isArray ? v.f : asFloat(v); break;
        case UIPropType::Int:    out.i = v.isArray ? v.i : asInt(v);   break;
        case UIPropType::Bool:   out.b = v.isArray ? v.b : asBool(v);  break;
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
