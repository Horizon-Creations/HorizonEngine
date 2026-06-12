#pragma once

namespace horizon {
    class World;
}
class RenderWorld;

class RenderExtractor {
public:
    void extract(const horizon::World& world, RenderWorld& outWorld);
};
