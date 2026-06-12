# HorizonRendering – Class Structure Task for GitHub Copilot

## Context

This is part of a C++ game engine called **Horizon Engine**.
`HorizonRendering` is a **shared library** that sits above the rendering backends (OpenGL, Vulkan, D3D11, D3D12).
It depends on `HorizonCore` (for `UUID`, `RenderHandle`, math types) and reads from `HorizonScene` (ECS components) — but `HorizonScene` must never depend on `HorizonRendering`.

All classes in this task are **stubs only** — declarations with no implementation bodies, unless noted otherwise. The `SlotMap<T>` is the one exception: implement it fully (see below).

---

## Target folder structure

```
HE_Rendering/
├── CMakeLists.txt
├── include/
│   └── HorizonRendering/
│       ├── HorizonRendering.h          ← master include
│       ├── RenderHandle.h
│       ├── RenderExtractor.h
│       ├── RenderWorld.h
│       ├── RenderObject.h
│       ├── FrustumCuller.h
│       ├── RenderSorter.h
│       ├── RenderResourceManager.h
│       ├── GPUMemoryAllocator.h
│       ├── RenderGraph.h
│       ├── RenderPass.h
│       ├── CommandBuffer.h
│       └── IRenderDevice.h
└── src/
    ├── SlotMap.h                       ← implementation header (not public API)
    ├── RenderExtractor.cpp
    ├── RenderWorld.cpp
    ├── RenderResourceManager.cpp
    ├── GPUMemoryAllocator.cpp
    ├── RenderGraph.cpp
    ├── RenderPass.cpp
    └── CommandBuffer.cpp
```

---

## Step 1 — SlotMap (fully implement this one)

Create `src/SlotMap.h`. This is an internal implementation file, not exposed in the public `include/` folder.

A SlotMap provides O(1) insert/delete with stable external handles (generational indices) and a dense internal data array for cache-friendly iteration.

```cpp
// src/SlotMap.h
#pragma once
#include <vector>
#include <cstdint>
#include <cassert>

// A handle into a SlotMap. Opaque outside this file.
struct SlotHandle {
    uint32_t index;       // index into slots[]
    uint32_t generation;  // must match slots[index].generation to be valid
};

template<typename T>
class SlotMap {
public:
    // Insert a value, returns a stable handle.
    SlotHandle insert(T value) {
        uint32_t slotIndex;
        if (!freeList_.empty()) {
            slotIndex = freeList_.back();
            freeList_.pop_back();
        } else {
            slotIndex = static_cast<uint32_t>(slots_.size());
            slots_.push_back({});
        }

        uint32_t dataIndex = static_cast<uint32_t>(data_.size());
        data_.push_back(std::move(value));
        erase_.push_back(slotIndex);

        slots_[slotIndex].dataIndex  = dataIndex;
        slots_[slotIndex].generation++;   // bump so old handles become invalid

        return { slotIndex, slots_[slotIndex].generation };
    }

    // Returns nullptr if the handle is stale or invalid.
    T* get(SlotHandle handle) {
        if (handle.index >= slots_.size()) return nullptr;
        Slot& slot = slots_[handle.index];
        if (slot.generation != handle.generation) return nullptr;
        return &data_[slot.dataIndex];
    }

    const T* get(SlotHandle handle) const {
        if (handle.index >= slots_.size()) return nullptr;
        const Slot& slot = slots_[handle.index];
        if (slot.generation != handle.generation) return nullptr;
        return &data_[slot.dataIndex];
    }

    bool isValid(SlotHandle handle) const {
        return handle.index < slots_.size() &&
               slots_[handle.index].generation == handle.generation;
    }

    // Remove an entry. Invalidates the handle. O(1) via swap-and-pop.
    void remove(SlotHandle handle) {
        if (!isValid(handle)) return;

        uint32_t dataIndex  = slots_[handle.index].dataIndex;
        uint32_t lastData   = static_cast<uint32_t>(data_.size()) - 1;

        if (dataIndex != lastData) {
            // Swap removed element with last, fix up the slot that pointed to last.
            data_[dataIndex]  = std::move(data_[lastData]);
            erase_[dataIndex] = erase_[lastData];
            slots_[erase_[dataIndex]].dataIndex = dataIndex;
        }

        data_.pop_back();
        erase_.pop_back();

        // Invalidate slot by bumping generation, return to free list.
        slots_[handle.index].generation++;
        freeList_.push_back(handle.index);
    }

    // Iterate over all live elements (dense, cache-friendly).
    T*       begin()       { return data_.data(); }
    T*       end()         { return data_.data() + data_.size(); }
    const T* begin() const { return data_.data(); }
    const T* end()   const { return data_.data() + data_.size(); }

    uint32_t size()  const { return static_cast<uint32_t>(data_.size()); }
    bool     empty() const { return data_.empty(); }

    void clear() {
        data_.clear();
        erase_.clear();
        freeList_.clear();
        for (auto& s : slots_) s.generation++;  // invalidate all existing handles
    }

private:
    struct Slot {
        uint32_t dataIndex  = 0;
        uint32_t generation = 1;   // starts at 1 so default SlotHandle{0,0} is always invalid
    };

    std::vector<Slot>     slots_;
    std::vector<T>        data_;   // dense — iterate this
    std::vector<uint32_t> erase_;  // erase_[i] = which slot owns data_[i]
    std::vector<uint32_t> freeList_;
};
```

---

## Step 2 — RenderHandle (public type, lives in HorizonCore)

> **Note for Copilot**: `RenderHandle` is already defined in `HorizonCore` (`include/HorizonCore/Types/Handle.h`). Do not redefine it. Just make sure `HorizonRendering` includes `<HorizonCore/Types/Handle.h>` wherever it needs it.

`RenderHandle` is an opaque wrapper around a `SlotHandle`. It is the only rendering concept that `HorizonScene` ever sees.

If `Handle.h` in HorizonCore does not yet have this, add it there (not here):

```cpp
// In HorizonCore/Types/Handle.h
#pragma once
#include <cstdint>

struct RenderHandle {
    uint32_t index;
    uint32_t generation;

    bool isValid() const { return generation != 0; }
    static RenderHandle invalid() { return {0, 0}; }
    bool operator==(const RenderHandle&) const = default;
};
```

---

## Step 3 — Public header stubs

Create each file with `#pragma once`, the includes listed, and the class declaration. **No method bodies.**

### `include/HorizonRendering/RenderObject.h`
```cpp
#pragma once
#include <HorizonCore/Types/Handle.h>
#include <HorizonCore/Math/Math.h>
#include <cstdint>

// One renderable entity extracted from the ECS world each frame.
struct RenderObject {
    RenderHandle meshHandle;
    RenderHandle materialHandle;
    glm::mat4    transform;
    uint32_t     entityId;    // back-reference to ECS entity for editor picking
    uint8_t      lod;         // LOD level chosen during extraction
};
```

### `include/HorizonRendering/RenderWorld.h`
```cpp
#pragma once
#include "RenderObject.h"
#include <HorizonCore/Math/Math.h>
#include <vector>
#include <cstdint>

struct CameraData {
    glm::mat4 view;
    glm::mat4 projection;
    glm::vec3 position;
};

struct LightData {
    glm::vec3 position;
    glm::vec3 color;
    float     intensity;
    uint8_t   type;   // 0 = directional, 1 = point, 2 = spot
};

// Renderer-internal scene snapshot. Scene never sees this.
class RenderWorld {
public:
    void clear();

    std::vector<RenderObject> objects;
    std::vector<LightData>    lights;
    CameraData                camera;
};
```

### `include/HorizonRendering/RenderExtractor.h`
```cpp
#pragma once

// Forward declarations — avoids pulling HorizonScene headers into the renderer's public API.
namespace horizon {
    class World;   // ECS world from HorizonScene
}
class RenderWorld;

// Reads dirty ECS entities each frame and fills a RenderWorld.
// This is the ONLY class in HorizonRendering that touches HorizonScene.
class RenderExtractor {
public:
    void extract(const horizon::World& world, RenderWorld& outWorld);
};
```

### `include/HorizonRendering/FrustumCuller.h`
```cpp
#pragma once
#include "RenderWorld.h"
#include <vector>

// Produces a visibility bitmask — no allocations per frame.
class FrustumCuller {
public:
    // Fills outVisible[i] = true if objects[i] is inside the frustum.
    void cull(const RenderWorld& world, std::vector<bool>& outVisible);
};
```

### `include/HorizonRendering/RenderSorter.h`
```cpp
#pragma once
#include "RenderWorld.h"
#include <vector>
#include <cstdint>

// Sorts visible object indices by material to minimise GPU state changes.
class RenderSorter {
public:
    // outSortedIndices contains indices into world.objects, sorted for optimal batching.
    void sort(const RenderWorld& world,
              const std::vector<bool>& visible,
              std::vector<uint32_t>& outSortedIndices);
};
```

### `include/HorizonRendering/GPUMemoryAllocator.h`
```cpp
#pragma once
#include <HorizonCore/Types/Handle.h>
#include <cstdint>

// Tracks VRAM budget and decides when to evict unused resources.
class GPUMemoryAllocator {
public:
    explicit GPUMemoryAllocator(uint64_t budgetBytes);

    // Returns false if budget is exceeded and eviction failed.
    bool requestAllocation(uint64_t sizeBytes, RenderHandle handle);
    void freeAllocation(RenderHandle handle);

    void onHandleUsed(RenderHandle handle);   // call each time a handle is drawn — updates LRU
    void evictLRU();                          // evict least-recently-used until under budget

    uint64_t usedBytes()  const;
    uint64_t totalBudget() const;

private:
    uint64_t budgetBytes_;
    uint64_t usedBytes_ = 0;
};
```

### `include/HorizonRendering/RenderResourceManager.h`
```cpp
#pragma once
#include <HorizonCore/Types/Handle.h>
#include <HorizonCore/Types/UUID.h>
#include "GPUMemoryAllocator.h"
#include <unordered_map>

struct MeshData    { /* raw vertex/index data — defined by asset pipeline */ };
struct TextureData { /* raw pixel data + format */ };
struct MaterialDesc { /* shader params, texture handles */ };

// GPU resource lifecycle. Only class that creates/destroys GPU objects.
class RenderResourceManager {
public:
    explicit RenderResourceManager(GPUMemoryAllocator& allocator);

    // Upload returns a stable RenderHandle. Calling with the same AssetID
    // twice returns the existing handle (ref-counted internally).
    RenderHandle uploadMesh    (const UUID& assetId, const MeshData&     data);
    RenderHandle uploadTexture (const UUID& assetId, const TextureData&  data);
    RenderHandle createMaterial(const UUID& assetId, const MaterialDesc& desc);

    void release(RenderHandle handle);

    // Used by RenderExtractor: translate AssetID → RenderHandle without re-uploading.
    RenderHandle findHandle(const UUID& assetId) const;
    bool         isLoaded  (const UUID& assetId) const;

private:
    GPUMemoryAllocator& allocator_;

    // AssetID → RenderHandle bridge (invisible to HorizonScene)
    std::unordered_map<UUID, RenderHandle> assetIndex_;
};
```

### `include/HorizonRendering/CommandBuffer.h`
```cpp
#pragma once
#include <HorizonCore/Types/Handle.h>
#include <HorizonCore/Math/Math.h>
#include <vector>

// A draw call recorded into the command buffer.
struct DrawCall {
    RenderHandle mesh;
    RenderHandle material;
    glm::mat4    transform;
    uint32_t     instanceCount = 1;
};

// Collects draw calls for one frame. Submitted in bulk by IRenderDevice::submit().
class CommandBuffer {
public:
    void reset();
    void recordDraw(const DrawCall& call);

    const std::vector<DrawCall>& drawCalls() const;

private:
    std::vector<DrawCall> drawCalls_;
};
```

### `include/HorizonRendering/RenderPass.h`
```cpp
#pragma once
#include "RenderWorld.h"
#include "CommandBuffer.h"
#include <vector>
#include <cstdint>

// Base class for all render passes.
class RenderPass {
public:
    virtual ~RenderPass() = default;

    // Called once per frame with the sorted visible object indices.
    virtual void execute(const RenderWorld&          world,
                         const std::vector<uint32_t>& sortedIndices,
                         CommandBuffer&               outCmds) = 0;

    virtual const char* name() const = 0;
};

// Concrete pass stubs — implement later.
class GeometryPass  : public RenderPass {
public:
    void execute(const RenderWorld&, const std::vector<uint32_t>&, CommandBuffer&) override;
    const char* name() const override { return "GeometryPass"; }
};

class ShadowPass    : public RenderPass {
public:
    void execute(const RenderWorld&, const std::vector<uint32_t>&, CommandBuffer&) override;
    const char* name() const override { return "ShadowPass"; }
};

class PostProcessPass : public RenderPass {
public:
    void execute(const RenderWorld&, const std::vector<uint32_t>&, CommandBuffer&) override;
    const char* name() const override { return "PostProcessPass"; }
};
```

### `include/HorizonRendering/RenderGraph.h`
```cpp
#pragma once
#include "RenderPass.h"
#include <vector>
#include <memory>

// Owns the ordered list of render passes and executes them each frame.
// Dependencies between passes are implicit by order for now;
// add explicit edges later when needed.
class RenderGraph {
public:
    void addPass(std::unique_ptr<RenderPass> pass);
    void execute(const RenderWorld&          world,
                 const std::vector<uint32_t>& sortedIndices,
                 CommandBuffer&               outCmds);
    void clear();

private:
    std::vector<std::unique_ptr<RenderPass>> passes_;
};
```

### `include/HorizonRendering/IRenderDevice.h`
```cpp
#pragma once
#include "CommandBuffer.h"
#include <HorizonCore/Types/Handle.h>
#include <cstdint>

struct BufferDesc {
    uint64_t    sizeBytes;
    uint32_t    flags;      // usage hints (vertex, index, uniform, …)
};

struct TextureDesc {
    uint32_t width, height, depth;
    uint32_t mipLevels;
    uint32_t format;        // backend-agnostic format enum — define separately
};

// The only interface the rest of the engine uses to talk to a backend.
// Implement once per backend (OpenGL, Vulkan, D3D11, D3D12).
class IRenderDevice {
public:
    virtual ~IRenderDevice() = default;

    virtual RenderHandle createBuffer (const BufferDesc&  desc, const void* initialData = nullptr) = 0;
    virtual RenderHandle createTexture(const TextureDesc& desc, const void* initialData = nullptr) = 0;
    virtual void         destroyBuffer (RenderHandle handle) = 0;
    virtual void         destroyTexture(RenderHandle handle) = 0;

    // Submit all recorded draw calls to the GPU.
    virtual void submit(const CommandBuffer& cmds) = 0;

    // Swap front/back buffers. Call once per frame after submit().
    virtual void present() = 0;
};
```

### `include/HorizonRendering/HorizonRendering.h`
```cpp
#pragma once

// Master include for external modules that use HorizonRendering.
// Internal classes (SlotMap, pass implementations) are NOT exposed here.

#include "RenderHandle.h"      // only needed if not already in HorizonCore
#include "RenderObject.h"
#include "RenderWorld.h"
#include "RenderExtractor.h"
#include "FrustumCuller.h"
#include "RenderSorter.h"
#include "RenderResourceManager.h"
#include "GPUMemoryAllocator.h"
#include "RenderGraph.h"
#include "RenderPass.h"
#include "CommandBuffer.h"
#include "IRenderDevice.h"
```

---

## Step 4 — Empty .cpp stubs

Create each `.cpp` with only its matching `#include`. No implementation yet.

- `src/RenderExtractor.cpp`       → `#include "HorizonRendering/RenderExtractor.h"`
- `src/RenderWorld.cpp`           → `#include "HorizonRendering/RenderWorld.h"`
- `src/RenderResourceManager.cpp` → `#include "HorizonRendering/RenderResourceManager.h"`
- `src/GPUMemoryAllocator.cpp`    → `#include "HorizonRendering/GPUMemoryAllocator.h"`
- `src/RenderGraph.cpp`           → `#include "HorizonRendering/RenderGraph.h"`
- `src/RenderPass.cpp`            → `#include "HorizonRendering/RenderPass.h"`
- `src/CommandBuffer.cpp`         → `#include "HorizonRendering/CommandBuffer.h"`

---

## Step 5 — CMakeLists.txt

```cmake
add_library(HorizonRendering SHARED
    src/RenderExtractor.cpp
    src/RenderWorld.cpp
    src/RenderResourceManager.cpp
    src/GPUMemoryAllocator.cpp
    src/RenderGraph.cpp
    src/RenderPass.cpp
    src/CommandBuffer.cpp
)

target_include_directories(HorizonRendering
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(HorizonRendering
    PRIVATE HE_RENDERING_BUILD_DLL
    PUBLIC  HE_RENDERING_DLL
)

target_link_libraries(HorizonRendering
    PUBLIC  HorizonCore
    PRIVATE HorizonScene   # only RenderExtractor.cpp needs this — consider PRIVATE
    PRIVATE glm::glm
)
```

---

## Notes for Copilot

- `SlotMap<T>` lives in `src/` only — it is an internal implementation detail, never included via the public `include/` folder.
- `RenderResourceManager` uses `SlotMap<GPUMesh>`, `SlotMap<GPUTexture>`, `SlotMap<GPUMaterial>` internally. Define `GPUMesh`, `GPUTexture`, `GPUMaterial` as private structs inside `RenderResourceManager.cpp` — they are backend-agnostic descriptors holding a `RenderHandle` and a size in bytes.
- `HorizonScene` must never `#include` anything from `HorizonRendering`. The dependency arrow is one-way: Rendering reads Scene, not the reverse.
- `UUID` must be hashable for `std::unordered_map<UUID, RenderHandle>`. Add a `std::hash<UUID>` specialisation in `HorizonCore/Types/UUID.h` if not already present.
- Do not add `#include <windows.h>` or any backend-specific header in the public `include/` folder. Backend headers belong only in the backend static libs (OpenGL, Vulkan, D3D11, D3D12).
- `HE_API` export macro from `HorizonCore/Types/Defines.h` should be applied to every class declared in the public headers, e.g. `class HE_API RenderWorld { ... };`.
