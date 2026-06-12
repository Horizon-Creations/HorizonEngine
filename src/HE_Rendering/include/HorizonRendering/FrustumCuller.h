#pragma once
#include "RenderWorld.h"
#include <vector>

class FrustumCuller {
public:
    void cull(const RenderWorld& world, std::vector<bool>& outVisible);
};
