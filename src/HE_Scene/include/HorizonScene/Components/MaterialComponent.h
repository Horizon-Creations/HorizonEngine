#pragma once
#include <Types/UUID.h>
#include <string>
#include <vector>

// A per-entity override of one node-graph material parameter, keyed by the
// parameter's name (MaterialAsset::graphParamNames). Lets two entities share one
// material asset but show different parameter values — a lightweight
// MaterialInstance, editable live from the entity Details panel. All four vec4
// components are stored; the shader reads only those its Param node uses.
struct MaterialParamOverride {
    std::string name;
    float       value[4] = { 0, 0, 0, 0 };
};

struct MaterialComponent {
    // The whole-mesh override: set, it replaces EVERY material slot of the mesh
    // (the Godot material_override rule) — the one-material world every scene
    // before sections lived in, unchanged.
    HE::UUID materialAssetId;
    bool dirty = true;
    // Per-entity node-graph parameter overrides (empty = use the material's own
    // values). Merged on top of MaterialAsset::shaderParamData at extract time.
    // They belong to materialAssetId's material and reach only the draws that
    // use it — a slot overridden with another material draws that one plain.
    std::vector<MaterialParamOverride> paramOverrides;
    // Per-SLOT overrides, indexed by the material slot of the entity's LOD0
    // mesh (MeshSection order — the "Materials (N slots)" list of the mesh
    // editor). A null entry, and every slot past the end, is not overridden.
    // Precedence per slot: this entry, else materialAssetId, else the slot's
    // own material from the asset. Empty for every entity that never set one,
    // which is what keeps the old whole-mesh draw path and its batching for
    // them. A LOD level's sections reach these through HE::lodSlotMap.
    std::vector<HE::UUID> slotOverrides;

    // Whether any slot is actually overridden (a list of nulls is not).
    bool hasSlotOverride() const
    {
        for (const HE::UUID& id : slotOverrides)
            if (id != HE::UUID{}) return true;
        return false;
    }
    // The override for `slot`, or null when there is none / the slot is off
    // the end / the slot is -1 (a LOD section without a LOD0 partner).
    HE::UUID slotOverride(int32_t slot) const
    {
        if (slot < 0 || static_cast<size_t>(slot) >= slotOverrides.size()) return {};
        return slotOverrides[static_cast<size_t>(slot)];
    }
};
