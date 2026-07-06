#pragma once
#include <Types/UUID.h>

// References a UI Widget asset (UMG-style widget tree). At play start
// UIWidgetInstantiator expands the tree into child entities carrying the
// regular UI components (UIElement/UIText/UIImage/UIButton + Script), so
// rendering, input and scripting all run on the standard paths. The host
// entity receives a UICanvasComponent sized from the tree.
struct UIWidgetComponent
{
    HE::UUID widgetAssetId;
    bool     active = true;
};
