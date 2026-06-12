#pragma once
#include <cstdint>

struct RenderHandle {
    uint32_t index;
    uint32_t generation;

    bool isValid() const { return generation != 0; }
    static RenderHandle invalid() { return {0, 0}; }
    bool operator==(const RenderHandle&) const = default;
};
