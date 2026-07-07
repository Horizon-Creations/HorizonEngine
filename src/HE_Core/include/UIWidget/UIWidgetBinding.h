#pragma once
#include <UIWidget/UIElement.h>
#include <HorizonCode/HorizonCode.h>

// Bridges the UI element property system (UIPropValue/UIPropType) and the
// generic HorizonCode value system (HorizonCode::Value/PinType). WidgetManager
// uses it to wire the interpreter Context to live elements; the editor uses the
// type mapping to create correctly-typed Get/Set/Event nodes.

namespace HE {

HorizonCode::PinType uiPropTypeToPin(UIPropType t);
HorizonCode::Value   uiPropToHcValue(const UIPropValue& v);
UIPropValue          uiHcValueToProp(const HorizonCode::Value& v, UIPropType want);

} // namespace HE
