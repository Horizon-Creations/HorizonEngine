#pragma once
#include <cstdint>
#include <functional>
#include <random>

namespace HE {

struct UUID {
    uint64_t hi = 0;
    uint64_t lo = 0;

    bool operator==(const UUID&) const = default;

    static UUID generate()
    {
        static thread_local std::mt19937_64 rng{ std::random_device{}() };
        UUID id;
        id.hi = rng();
        id.lo = rng();
        // Set version 4 (random) and variant bits per RFC 4122
        id.hi = (id.hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
        id.lo = (id.lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
        return id;
    }
};

} // namespace HE

namespace std {
    template<>
    struct hash<HE::UUID> {
        size_t operator()(const HE::UUID& id) const noexcept {
            size_t h1 = std::hash<uint64_t>{}(id.hi);
            size_t h2 = std::hash<uint64_t>{}(id.lo);
            return h1 ^ (h2 << 1);
        }
    };
}
