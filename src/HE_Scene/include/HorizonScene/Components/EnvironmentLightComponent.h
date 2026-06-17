#pragma once
#include <cstdint>

// Tags the two built-in directional lights the World's EnvironmentComponent
// drives: the sun and the moon. These entities are auto-created on the World
// root, never serialised (always recreated on load), hidden from the Outliner
// and non-deletable — they are conceptually part of the environment, not loose
// scene entities. `role` distinguishes the two; the RenderExtractor reads it to
// drive each light from the environment (arc direction, colour, intensity).
struct EnvironmentLightComponent
{
    enum class Role : uint8_t { Sun = 0, Moon = 1 };
    Role role = Role::Sun;
};
