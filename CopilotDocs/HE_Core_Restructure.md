# HorizonCore – Restructuring Task for GitHub Copilot

## Context

This is part of a C++ game engine called **Horizon Engine**.
`HorizonCore` is a **shared library (.dll on Windows, .so on Linux/Mac)** that all other engine modules link against at runtime.
The goal is to restructure the existing `HE_Core/` directory to match the layout below.

---

## Current State (what exists right now)

```
HE_Core/
├── Diagnostics/
│   ├── CMakeLists.txt       ← DELETE this file
│   ├── DiagnosticsStructs.h ← KEEP, move to new location (see below)
│   ├── GlobalState.cpp      ← KEEP, move to new location (see below)
│   └── GlobalState.h        ← KEEP, move to new location (see below)
├── CMakeLists.txt           ← REPLACE content (see below)
└── Core.cpp                 ← DELETE this file
```

---

## Target State (what it should look like after)

```
HE_Core/
├── CMakeLists.txt
├── include/
│   └── HorizonCore/
│       ├── HorizonCore.h
│       ├── Math/
│       │   ├── Math.h
│       │   ├── Transform.h
│       │   ├── Transform2D.h
│       │   └── AABB.h
│       ├── Memory/
│       │   ├── Allocator.h
│       │   ├── PoolAllocator.h
│       │   └── Ref.h
│       ├── Types/
│       │   ├── Defines.h
│       │   ├── UUID.h
│       │   ├── Handle.h
│       │   └── StringID.h
│       ├── Platform/
│       │   ├── Platform.h
│       │   ├── FileSystem.h
│       │   └── DynLib.h
│       └── Diagnostics/
│           ├── DiagnosticsStructs.h
│           ├── GlobalState.h
│           ├── Logger.h
│           ├── Profiler.h
│           ├── Assert.h
│           └── CrashHandler.h
└── src/
    ├── Diagnostics/
    │   ├── GlobalState.cpp
    │   ├── Logger.cpp
    │   ├── Profiler.cpp
    │   └── CrashHandler.cpp
    ├── Memory/
    │   ├── PoolAllocator.cpp
    │   └── Ref.cpp
    ├── Types/
    │   ├── UUID.cpp
    │   └── StringID.cpp
    └── Platform/
        ├── FileSystem.cpp
        └── DynLib.cpp
```

---

## Step-by-step Instructions

### 1. Delete these files
- `HE_Core/Diagnostics/CMakeLists.txt`
- `HE_Core/Core.cpp`

### 2. Move existing files to new locations
- `HE_Core/Diagnostics/DiagnosticsStructs.h` → `HE_Core/include/HorizonCore/Diagnostics/DiagnosticsStructs.h`
- `HE_Core/Diagnostics/GlobalState.h`        → `HE_Core/include/HorizonCore/Diagnostics/GlobalState.h`
- `HE_Core/Diagnostics/GlobalState.cpp`      → `HE_Core/src/Diagnostics/GlobalState.cpp`

After moving, update the `#include` path in `GlobalState.cpp` if it references `GlobalState.h` relatively.

### 3. Create all missing header files as empty stubs

Create each of the following files with only a `#pragma once` guard:

- `include/HorizonCore/HorizonCore.h`
- `include/HorizonCore/Math/Math.h`
- `include/HorizonCore/Math/Transform.h`
- `include/HorizonCore/Math/Transform2D.h`
- `include/HorizonCore/Math/AABB.h`
- `include/HorizonCore/Memory/Allocator.h`
- `include/HorizonCore/Memory/PoolAllocator.h`
- `include/HorizonCore/Memory/Ref.h`
- `include/HorizonCore/Types/UUID.h`
- `include/HorizonCore/Types/Handle.h`
- `include/HorizonCore/Types/StringID.h`
- `include/HorizonCore/Platform/Platform.h`
- `include/HorizonCore/Platform/FileSystem.h`
- `include/HorizonCore/Platform/DynLib.h`
- `include/HorizonCore/Diagnostics/Logger.h`
- `include/HorizonCore/Diagnostics/Profiler.h`
- `include/HorizonCore/Diagnostics/Assert.h`
- `include/HorizonCore/Diagnostics/CrashHandler.h`

### 4. Create `Types/Defines.h` with DLL export macro

```cpp
// include/HorizonCore/Types/Defines.h
#pragma once

#ifdef _WIN32
  #ifdef HE_CORE_BUILD_DLL
    #define HE_API __declspec(dllexport)
  #else
    #define HE_API __declspec(dllimport)
  #endif
#else
  #define HE_API __attribute__((visibility("default")))
#endif
```

### 5. Create `HorizonCore.h` as master include

```cpp
// include/HorizonCore/HorizonCore.h
#pragma once

#include "Types/Defines.h"
#include "Types/UUID.h"
#include "Types/Handle.h"
#include "Types/StringID.h"

#include "Math/Math.h"
#include "Math/Transform.h"
#include "Math/Transform2D.h"
#include "Math/AABB.h"

#include "Memory/Allocator.h"
#include "Memory/PoolAllocator.h"
#include "Memory/Ref.h"

#include "Platform/Platform.h"
#include "Platform/FileSystem.h"
#include "Platform/DynLib.h"

#include "Diagnostics/DiagnosticsStructs.h"
#include "Diagnostics/GlobalState.h"
#include "Diagnostics/Logger.h"
#include "Diagnostics/Profiler.h"
#include "Diagnostics/Assert.h"
#include "Diagnostics/CrashHandler.h"
```

### 6. Create empty `.cpp` stubs for all new source files

Create the following files, each with only a matching `#include` at the top:

- `src/Diagnostics/Logger.cpp`       → `#include "HorizonCore/Diagnostics/Logger.h"`
- `src/Diagnostics/Profiler.cpp`     → `#include "HorizonCore/Diagnostics/Profiler.h"`
- `src/Diagnostics/CrashHandler.cpp` → `#include "HorizonCore/Diagnostics/CrashHandler.h"`
- `src/Memory/PoolAllocator.cpp`     → `#include "HorizonCore/Memory/PoolAllocator.h"`
- `src/Memory/Ref.cpp`               → `#include "HorizonCore/Memory/Ref.h"`
- `src/Types/UUID.cpp`               → `#include "HorizonCore/Types/UUID.h"`
- `src/Types/StringID.cpp`           → `#include "HorizonCore/Types/StringID.h"`
- `src/Platform/FileSystem.cpp`      → `#include "HorizonCore/Platform/FileSystem.h"`
- `src/Platform/DynLib.cpp`          → `#include "HorizonCore/Platform/DynLib.h"`

### 7. Replace `HE_Core/CMakeLists.txt` with this content

```cmake
add_library(HorizonCore SHARED
    src/Diagnostics/GlobalState.cpp
    src/Diagnostics/Logger.cpp
    src/Diagnostics/Profiler.cpp
    src/Diagnostics/CrashHandler.cpp
    src/Memory/PoolAllocator.cpp
    src/Memory/Ref.cpp
    src/Types/UUID.cpp
    src/Types/StringID.cpp
    src/Platform/FileSystem.cpp
    src/Platform/DynLib.cpp
)

target_include_directories(HorizonCore
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(HorizonCore
    PRIVATE HE_CORE_BUILD_DLL
    PUBLIC  HE_CORE_DLL
)

target_link_libraries(HorizonCore
    PRIVATE glm::glm
)
```

---

## Notes for Copilot

- All other engine modules include Core headers via `#include <HorizonCore/...>` — the double-path (`include/HorizonCore/`) is intentional to avoid name collisions across modules.
- `HE_API` from `Defines.h` must be added to any class or function that needs to be visible across the DLL boundary. Example: `class HE_API GlobalState { ... };`
- Do not add `#include <windows.h>` or any platform header globally — platform-specific includes belong only in the corresponding `.cpp` files under `src/Platform/`.
- Math headers may include `<glm/glm.hpp>` where needed — GLM defines are handled per-backend in `HorizonRendering`, not here.