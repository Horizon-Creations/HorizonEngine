#pragma once
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <ContentManager/ContentManager.h>

// Drives the ScriptContext from a ContentManager and HorizonWorld.
// Call pollHotReload() each editor-tick while in Play Mode to apply
// source changes to live script instances.
struct ScriptSystem
{
    // Poll ContentManager for changed ScriptAssets and hot-reload their modules.
    // Instances bound to the changed script have their function fields patched;
    // data fields (self.hp, self.total, etc.) are preserved.
    static void pollHotReload(HorizonWorld& /*world*/, ScriptContext& ctx,
                              ContentManager* content)
    {
        if (!content) return;
        for (HE::UUID id : content->pollHotReload())
        {
            if (content->assetType(id) != HE::AssetType::Script) continue;
            const ScriptAsset* asset = content->getScript(id);
            if (!asset || asset->sourceCode.empty()) continue;
            ctx.hotReloadScript(asset->name, asset->sourceCode);
        }
    }
};
