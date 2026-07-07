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

UIPropValue uiHcValueToProp(const HorizonCode::Value& v, UIPropType want)
{
    using P = HorizonCode::PinType;
    UIPropValue out;
    out.type = want;
    switch (want)
    {
        case UIPropType::Float:  out.f = v.type == P::Int ? (float)v.i : (v.type == P::Bool ? (v.b ? 1.0f : 0.0f) : v.f); break;
        case UIPropType::Int:    out.i = v.type == P::Float ? (int)v.f : (v.type == P::Bool ? (v.b ? 1 : 0) : v.i); break;
        case UIPropType::Bool:   out.b = v.type == P::Float ? v.f != 0.0f : (v.type == P::Int ? v.i != 0 : v.b); break;
        case UIPropType::String: out.s = v.s; break;
        case UIPropType::Color:  out.col = v.col; break;
        case UIPropType::Vec2:   out.v2 = v.v2; break;
        case UIPropType::StringList: out.list = { v.s }; break;
    }
    return out;
}

} // namespace HE
