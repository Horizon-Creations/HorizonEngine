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
    HE::UUID materialAssetId;
    bool dirty = true;
    // Per-entity node-graph parameter overrides (empty = use the material's own
    // values). Merged on top of MaterialAsset::shaderParamData at extract time.
    std::vector<MaterialParamOverride> paramOverrides;
};
