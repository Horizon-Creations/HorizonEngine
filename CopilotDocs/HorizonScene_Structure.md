# HorizonScene – Structure Task for GitHub Copilot

## Context

This is part of a C++ game engine called **Horizon Engine**.
`HorizonScene` is a **shared library** that owns the ECS world, scene graph, and serialization.

Dependency rules (strictly enforced):
- `HorizonScene` depends on `HorizonCore` only
- `HorizonRendering` reads from `HorizonScene` — never the reverse
- `HorizonGame` and `HorizonEditor` depend on both

The ECS backend is **EnTT** right now, but the entire engine must be insulated from it.
EnTT must never appear in any public header — only in `.cpp` files and one internal `ECSBackend.h`.
This is the migration seam: replacing `ECSBackend.h` + `HorizonWorld.cpp` later is the only thing
needed to switch to an Archetype-ECS without touching the rest of the engine.

---

## Target folder structure

```
HE_Scene/
├── CMakeLists.txt
├── include/
│   └── HorizonScene/
│       ├── HorizonScene.h          ← master include
│       ├── Entity.h                ← opaque entity handle
│       ├── HorizonWorld.h          ← main ECS facade (NO EnTT here)
│       ├── SceneGraph.h            ← parent/child transform hierarchy
│       ├── SceneSerializer.h       ← JSON (editor) + binary (packaged) serialization
│       └── Components/
│           ├── TransformComponent.h
│           ├── Transform2DComponent.h
│           ├── MeshComponent.h
│           ├── MaterialComponent.h
│           ├── CameraComponent.h
│           ├── LightComponent.h
│           ├── RigidBodyComponent.h
│           ├── ScriptComponent.h
│           └── HierarchyComponent.h
└── src/
    ├── ECSBackend.h                ← ONLY file allowed to include <entt/entt.hpp>
    ├── HorizonWorld.cpp
    ├── SceneGraph.cpp
    ├── SceneSerializer.cpp
    └── Components/
        └── TransformComponent.cpp
```

---

## Step 1 — Entity handle

Create `include/HorizonScene/Entity.h`.

Entity is a plain integer handle. It must NOT be an `entt::entity` in the public header.
The typedef below makes the future migration trivial — only this one line changes.

```cpp
// include/HorizonScene/Entity.h
#pragma once
#include <cstdint>

// Opaque entity identifier.
// Internal representation is deliberately hidden from the public API.
// To migrate to a different ECS backend, only ECSBackend.h needs to change —
// the uint32_t here stays stable as the external "entity ID" concept.
using Entity = uint32_t;

constexpr Entity INVALID_ENTITY = UINT32_MAX;
```

---

## Step 2 — ECS backend isolation (internal only)

Create `src/ECSBackend.h`. This is the ONLY file in the entire engine that includes EnTT.
All EnTT types are aliased here. To migrate to a different ECS, replace this file only.

```cpp
// src/ECSBackend.h
#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// MIGRATION SEAM
// This is the only file in Horizon Engine that includes EnTT directly.
// To switch to an Archetype-ECS (e.g. Flecs, a custom implementation):
//   1. Replace the includes and aliases below
//   2. Update HorizonWorld.cpp to use the new registry API
//   3. Nothing else in the engine needs to change
// ─────────────────────────────────────────────────────────────────────────────

#include <entt/entt.hpp>

// Internal aliases — never leak into public headers
using ECSRegistry = entt::registry;
using ECSEntity   = entt::entity;

// Conversion helpers between our public Entity (uint32_t) and the internal ECSEntity.
inline ECSEntity  toECS   (Entity e) { return static_cast<ECSEntity>(e); }
inline Entity     fromECS (ECSEntity e) { return static_cast<Entity>(e); }
```

---

## Step 3 — Components

Create each file as a plain struct with `#pragma once`. No logic, no EnTT includes.

### `include/HorizonScene/Components/TransformComponent.h`
```cpp
#pragma once
#include <HorizonCore/Math/Math.h>

struct TransformComponent {
    glm::vec3 position  = glm::vec3(0.0f);
    glm::vec3 rotation  = glm::vec3(0.0f);  // Euler angles in degrees
    glm::vec3 scale     = glm::vec3(1.0f);
    bool      dirty     = true;             // set when changed, cleared by RenderExtractor

    glm::mat4 worldMatrix = glm::mat4(1.0f);  // computed, not serialized
};
```

### `include/HorizonScene/Components/Transform2DComponent.h`
```cpp
#pragma once
#include <HorizonCore/Math/Math.h>

struct Transform2DComponent {
    glm::vec2 position = glm::vec2(0.0f);
    float     rotation = 0.0f;             // degrees
    glm::vec2 scale    = glm::vec2(1.0f);
    bool      dirty    = true;

    glm::mat3 worldMatrix = glm::mat3(1.0f);
};
```

### `include/HorizonScene/Components/HierarchyComponent.h`
```cpp
#pragma once
#include <HorizonScene/Entity.h>
#include <vector>

// Stores the parent/child relationship for the scene graph.
// HierarchyComponent alone does NOT update transforms —
// SceneGraph::propagateTransforms() does that each frame.
struct HierarchyComponent {
    Entity              parent   = INVALID_ENTITY;
    std::vector<Entity> children;
};
```

### `include/HorizonScene/Components/MeshComponent.h`
```cpp
#pragma once
#include <HorizonCore/Types/UUID.h>

struct MeshComponent {
    UUID    meshAssetId;
    uint8_t lodBias = 0;   // 0 = auto LOD
    bool    castsShadow    = true;
    bool    receivesShadow = true;
    bool    dirty          = true;
};
```

### `include/HorizonScene/Components/MaterialComponent.h`
```cpp
#pragma once
#include <HorizonCore/Types/UUID.h>

struct MaterialComponent {
    UUID materialAssetId;
    bool dirty = true;
};
```

### `include/HorizonScene/Components/CameraComponent.h`
```cpp
#pragma once
#include <HorizonCore/Math/Math.h>

struct CameraComponent {
    float fovDegrees  = 60.0f;
    float nearPlane   = 0.1f;
    float farPlane    = 1000.0f;
    bool  isMain      = false;   // only one camera per world may have isMain = true
    bool  orthographic = false;
};
```

### `include/HorizonScene/Components/LightComponent.h`
```cpp
#pragma once
#include <HorizonCore/Math/Math.h>

enum class LightType : uint8_t {
    Directional = 0,
    Point       = 1,
    Spot        = 2,
};

struct LightComponent {
    LightType type      = LightType::Point;
    glm::vec3 color     = glm::vec3(1.0f);
    float     intensity = 1.0f;
    float     range     = 10.0f;      // point/spot only
    float     spotAngle = 30.0f;      // spot only, degrees
    bool      castsShadow = false;
};
```

### `include/HorizonScene/Components/RigidBodyComponent.h`
```cpp
#pragma once
#include <HorizonCore/Math/Math.h>

enum class RigidBodyType : uint8_t {
    Static    = 0,
    Dynamic   = 1,
    Kinematic = 2,
};

struct RigidBodyComponent {
    RigidBodyType type    = RigidBodyType::Static;
    float         mass    = 1.0f;
    float         friction    = 0.5f;
    float         restitution = 0.3f;
    bool          is2D   = false;   // use 2D physics solver if true
};
```

### `include/HorizonScene/Components/ScriptComponent.h`
```cpp
#pragma once
#include <HorizonCore/Types/UUID.h>
#include <string>

// Holds a reference to a Python script module.
// HorizonScripting reads this component and manages the Python object lifecycle.
struct ScriptComponent {
    UUID        scriptAssetId;       // asset ID of the .py file
    std::string moduleName;          // e.g. "player_controller"
    bool        enabled = true;
};
```

---

## Step 4 — HorizonWorld facade

Create `include/HorizonScene/HorizonWorld.h`.

**Critical**: No EnTT in this header. Forward-declare the impl struct instead.

```cpp
// include/HorizonScene/HorizonWorld.h
#pragma once
#include "Entity.h"
#include <HorizonCore/Types/UUID.h>
#include <memory>
#include <functional>

// Forward declaration of the internal impl — keeps EnTT out of the public header.
struct WorldImpl;

class HorizonWorld {
public:
     HorizonWorld();
    ~HorizonWorld();

    // ── Entity lifecycle ──────────────────────────────────────────────────
    Entity createEntity();
    Entity createEntityWithUUID(const UUID& id);  // for deserialization
    void   destroyEntity(Entity e);
    bool   isValid(Entity e) const;

    // ── Component access ─────────────────────────────────────────────────

    // Add a component. Args forwarded to the component constructor.
    template<typename T, typename... Args>
    T& addComponent(Entity e, Args&&... args);

    // Remove a component. No-op if entity doesn't have it.
    template<typename T>
    void removeComponent(Entity e);

    // Returns pointer or nullptr if not present.
    template<typename T>
    T* getComponent(Entity e);

    template<typename T>
    const T* getComponent(Entity e) const;

    template<typename T>
    bool hasComponent(Entity e) const;

    // ── Iteration ─────────────────────────────────────────────────────────

    // Call func(Entity, A&, B&) for every entity that has all listed components.
    // This maps to entt::view internally. Replace with archetype query later.
    template<typename... Components, typename Func>
    void each(Func&& func);

    template<typename... Components, typename Func>
    void each(Func&& func) const;

    // ── Change detection ──────────────────────────────────────────────────

    // Called by RenderExtractor: iterate only entities whose TransformComponent
    // or MeshComponent has dirty = true.
    template<typename T, typename Func>
    void eachDirty(Func&& func);

    void clearDirtyFlags();   // call after extraction is complete

    // ── Entity count ──────────────────────────────────────────────────────
    uint32_t entityCount() const;

private:
    std::unique_ptr<WorldImpl> impl_;  // pimpl — hides entt::registry
};
```

Create `src/HorizonWorld.cpp` with the EnTT implementation:

```cpp
// src/HorizonWorld.cpp
#include "HorizonScene/HorizonWorld.h"
#include "ECSBackend.h"   // only .cpp files include this

struct WorldImpl {
    ECSRegistry registry;
};

HorizonWorld::HorizonWorld()  : impl_(std::make_unique<WorldImpl>()) {}
HorizonWorld::~HorizonWorld() = default;

Entity HorizonWorld::createEntity() {
    ECSEntity e = impl_->registry.create();
    return fromECS(e);
}

Entity HorizonWorld::createEntityWithUUID(const UUID& id) {
    // Create entity, then attach UUID as a component for lookup during deserialization
    ECSEntity e = impl_->registry.create();
    impl_->registry.emplace<UUID>(e, id);
    return fromECS(e);
}

void HorizonWorld::destroyEntity(Entity e) {
    impl_->registry.destroy(toECS(e));
}

bool HorizonWorld::isValid(Entity e) const {
    return impl_->registry.valid(toECS(e));
}

uint32_t HorizonWorld::entityCount() const {
    return static_cast<uint32_t>(impl_->registry.alive());
}

// Template implementations — defined here via explicit instantiation or
// kept inline in the header with #include "HorizonWorld.inl".
// For now: explicit instantiations for known component types at bottom of this file.

template<typename T, typename... Args>
T& HorizonWorld::addComponent(Entity e, Args&&... args) {
    return impl_->registry.emplace<T>(toECS(e), std::forward<Args>(args)...);
}

template<typename T>
void HorizonWorld::removeComponent(Entity e) {
    impl_->registry.remove<T>(toECS(e));
}

template<typename T>
T* HorizonWorld::getComponent(Entity e) {
    return impl_->registry.try_get<T>(toECS(e));
}

template<typename T>
const T* HorizonWorld::getComponent(Entity e) const {
    return impl_->registry.try_get<T>(toECS(e));
}

template<typename T>
bool HorizonWorld::hasComponent(Entity e) const {
    return impl_->registry.all_of<T>(toECS(e));
}

template<typename... Components, typename Func>
void HorizonWorld::each(Func&& func) {
    impl_->registry.view<Components...>().each(std::forward<Func>(func));
}

template<typename... Components, typename Func>
void HorizonWorld::each(Func&& func) const {
    impl_->registry.view<Components...>().each(std::forward<Func>(func));
}

template<typename T, typename Func>
void HorizonWorld::eachDirty(Func&& func) {
    impl_->registry.view<T>().each([&](ECSEntity e, T& comp) {
        if (comp.dirty) func(fromECS(e), comp);
    });
}

void HorizonWorld::clearDirtyFlags() {
    // Clear dirty on all components that have one.
    // Add a line here for every component type that has a dirty flag.
    impl_->registry.view<TransformComponent>().each(
        [](TransformComponent& t) { t.dirty = false; });
    impl_->registry.view<MeshComponent>().each(
        [](MeshComponent& m) { m.dirty = false; });
    impl_->registry.view<MaterialComponent>().each(
        [](MaterialComponent& m) { m.dirty = false; });
}
```

---

## Step 5 — SceneGraph

Create `include/HorizonScene/SceneGraph.h`:

```cpp
// include/HorizonScene/SceneGraph.h
#pragma once
#include "Entity.h"
#include "HorizonWorld.h"

// Manages parent/child relationships and propagates world transforms.
// Call propagateTransforms() once per frame before RenderExtractor::extract().
class SceneGraph {
public:
    // Attach child to parent. Updates HierarchyComponent on both entities.
    void setParent(Entity child, Entity parent, HorizonWorld& world);

    // Detach from parent. Child becomes a root entity.
    void detach(Entity child, HorizonWorld& world);

    // Recompute worldMatrix for every TransformComponent in parent-first order.
    // Only processes entities where dirty == true (or whose parent chain is dirty).
    void propagateTransforms(HorizonWorld& world);

private:
    // Recursive helper — propagates from a root entity downward.
    void propagateEntity(Entity entity,
                         const glm::mat4& parentWorld,
                         HorizonWorld& world);
};
```

---

## Step 6 — SceneSerializer

Create `include/HorizonScene/SceneSerializer.h`:

```cpp
// include/HorizonScene/SceneSerializer.h
#pragma once
#include "HorizonWorld.h"
#include <string>
#include <filesystem>

enum class SerializeFormat {
    JSON,    // editor — human-readable, versioned
    Binary,  // packaged game — compact, fast to load
};

class SceneSerializer {
public:
    // Save the entire world to disk.
    bool save(const HorizonWorld& world,
              const std::filesystem::path& path,
              SerializeFormat format);

    // Load a world from disk. Clears the world first.
    bool load(HorizonWorld& world,
              const std::filesystem::path& path,
              SerializeFormat format);

private:
    bool saveJSON  (const HorizonWorld& world, const std::filesystem::path& path);
    bool saveBinary(const HorizonWorld& world, const std::filesystem::path& path);
    bool loadJSON  (HorizonWorld& world, const std::filesystem::path& path);
    bool loadBinary(HorizonWorld& world, const std::filesystem::path& path);
};
```

---

## Step 7 — Master include

Create `include/HorizonScene/HorizonScene.h`:

```cpp
#pragma once

#include "Entity.h"
#include "HorizonWorld.h"
#include "SceneGraph.h"
#include "SceneSerializer.h"

#include "Components/TransformComponent.h"
#include "Components/Transform2DComponent.h"
#include "Components/HierarchyComponent.h"
#include "Components/MeshComponent.h"
#include "Components/MaterialComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/RigidBodyComponent.h"
#include "Components/ScriptComponent.h"
```

---

## Step 8 — CMakeLists.txt

```cmake
add_library(HorizonScene SHARED
    src/HorizonWorld.cpp
    src/SceneGraph.cpp
    src/SceneSerializer.cpp
    src/Components/TransformComponent.cpp
)

target_include_directories(HorizonScene
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(HorizonScene
    PRIVATE HE_SCENE_BUILD_DLL
    PUBLIC  HE_SCENE_DLL
)

target_link_libraries(HorizonScene
    PUBLIC  HorizonCore
    PRIVATE entt::entt      # EnTT only visible to .cpp files via PRIVATE
)
```

> **Note**: Add EnTT to the project via CMake FetchContent or vcpkg.
> FetchContent example for root CMakeLists.txt:
> ```cmake
> include(FetchContent)
> FetchContent_Declare(
>     entt
>     GIT_REPOSITORY https://github.com/skypjack/entt.git
>     GIT_TAG        v3.13.0
> )
> FetchContent_MakeAvailable(entt)
> ```

---

## Migration guide — switching to Archetype-ECS later

When you want maximum performance at 50k+ entities, this is the complete migration path.
The rest of the engine does NOT change — only the files listed here.

### What changes

**1. `src/ECSBackend.h`** — swap the backend aliases:
```cpp
// BEFORE (EnTT)
#include <entt/entt.hpp>
using ECSRegistry = entt::registry;
using ECSEntity   = entt::entity;

// AFTER (custom archetype ECS or Flecs)
#include <HorizonECS/ArchetypeRegistry.h>
using ECSRegistry = horizon::ArchetypeRegistry;
using ECSEntity   = horizon::ArchetypeEntity;
```

**2. `src/HorizonWorld.cpp`** — update the registry calls to match the new API.
The public interface (`addComponent<T>`, `each<A,B>`, etc.) stays identical.
Only the internal forwarding to `impl_->registry` changes.

**3. `CMakeLists.txt`** — swap `entt::entt` for the new library in `target_link_libraries`.

### What does NOT change (zero modifications needed)
- All public headers in `include/HorizonScene/`
- All component structs
- `HorizonRendering` (RenderExtractor uses only `HorizonWorld` public API)
- `HorizonGame`, `HorizonEditor`, `HorizonScripting`
- Any game code that calls `world.addComponent<T>(...)` or `world.each<A,B>(...)`

### Performance headroom with EnTT in the meantime
EnTT's sparse sets are already very fast for typical game entity counts (< 20k).
To get more out of EnTT before migrating:
- Use `entt::group` (mapped in `HorizonWorld` as `eachGroup<A,B>`) for component pairs
  you always access together (e.g. `TransformComponent` + `MeshComponent`)
- Keep `dirty` flags to minimize work in `RenderExtractor`
- Avoid `std::vector` inside components — prefer flat arrays or UUIDs as references

---

## Notes for Copilot

- `entt::entt` must NEVER appear in any file under `include/`. Only in `src/`.
- `ECSBackend.h` is `PRIVATE` in CMake — it is never installed or exported.
- `HorizonWorld` uses pimpl (`std::unique_ptr<WorldImpl>`) so that `entt::registry`
  (which is large and slow to compile) stays out of every translation unit that
  includes `HorizonWorld.h`.
- Template methods on `HorizonWorld` that forward to EnTT must be defined in
  `HorizonWorld.cpp` with explicit instantiations, OR moved to a `HorizonWorld.inl`
  file that is included at the bottom of `HorizonWorld.h` but NOT included anywhere
  else. Do not put EnTT includes in the `.inl` file — use forward declarations.
- `UUID` must have `std::hash<UUID>` specialization in `HorizonCore` for any
  `unordered_map<UUID, ...>` usage in serialization.
- `HE_API` export macro from `HorizonCore/Types/Defines.h` must be applied to
  `HorizonWorld`, `SceneGraph`, and `SceneSerializer` class declarations.
- JSON serialization: use `nlohmann/json` (header-only, add via FetchContent).
  Binary serialization: write raw structs with a version header — no external dep needed.
