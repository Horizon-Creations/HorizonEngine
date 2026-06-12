# HorizonAsset + HorizonTools – Structure Task for GitHub Copilot

## Context

This covers two modules:
- `HorizonAsset` — shared library, runtime asset loading (.hasset files + .hpak archives)
- `HorizonTools` — standalone executables for the asset pipeline (editor-only, never shipped)

Dependency rules:
- `HorizonAsset` depends on `HorizonCore` only
- `HorizonTools` depends on `HorizonAsset` + `HorizonCore` + external libs (Assimp, stb_image)
- `HorizonRendering` and `HorizonScene` depend on `HorizonAsset` for asset loading
- Nothing in `HorizonTools` is ever linked into a packaged game

---

## Part A — HorizonAsset (runtime, shipped with game)

### Target folder structure

```
HE_Asset/
├── CMakeLists.txt
├── include/
│   └── HorizonAsset/
│       ├── HorizonAsset.h          ← master include
│       ├── AssetRegistry.h         ← UUID → file path / hpak offset mapping
│       ├── AssetLoader.h           ← loads .hasset files or reads from .hpak
│       ├── AssetCache.h            ← ref-counted in-memory cache
│       ├── HpakReader.h            ← reads encrypted .hpak archives
│       └── Assets/
│           ├── MeshAsset.h
│           ├── TextureAsset.h
│           ├── AudioAsset.h
│           ├── MaterialAsset.h
│           └── SceneAsset.h
└── src/
    ├── AssetRegistry.cpp
    ├── AssetLoader.cpp
    ├── AssetCache.cpp
    ├── HpakReader.cpp
    └── Assets/
        ├── MeshAsset.cpp
        ├── TextureAsset.cpp
        ├── AudioAsset.cpp
        ├── MaterialAsset.cpp
        └── SceneAsset.cpp
```

---

### .hasset file format (editor, human-inspectable)

Each asset on disk is a single `.hasset` file. Format:

```
[4 bytes]  magic: 0x48 0x41 0x53 0x54  ("HAST")
[4 bytes]  version: uint32_t (current = 1)
[16 bytes] UUID (two uint64_t, little-endian)
[4 bytes]  AssetType enum (uint32_t)
[4 bytes]  metaLength: uint32_t
[N bytes]  meta: JSON blob (UTF-8, metaLength bytes)
[8 bytes]  dataLength: uint64_t
[N bytes]  data: raw binary payload (format depends on AssetType)
```

The JSON meta block contains human-readable fields (name, source file path,
import settings). The binary data block contains the runtime-ready payload
(e.g. interleaved vertex buffer for meshes, compressed pixels for textures).

---

### .hpak file format (packaged game)

```
[4 bytes]   magic: 0x48 0x50 0x41 0x4B  ("HPAK")
[4 bytes]   version: uint32_t (current = 1)
[4 bytes]   entryCount: uint32_t
[16 bytes]  AES-256 IV (initialisation vector, random per pack)
── per entry (entryCount × 44 bytes, unencrypted index) ──
  [16 bytes] UUID
  [8 bytes]  offset into encrypted payload section
  [8 bytes]  encryptedSize
  [4 bytes]  AssetType
  [8 bytes]  unencryptedSize (for decompression/validation)
── encrypted payload section ──
  AES-256-CBC encrypted concatenation of all .hasset data blocks
  Key is derived at runtime from a project-specific secret (see HpakReader)
```

The index is unencrypted so the reader can seek without decrypting everything.
Only the data payloads are encrypted. Each entry is independently decryptable
(IV + offset allows random access).

---

### AssetType enum

Create `include/HorizonAsset/AssetRegistry.h` with this enum:

```cpp
#pragma once
#include <HorizonCore/Types/UUID.h>
#include <filesystem>
#include <unordered_map>
#include <cstdint>

enum class AssetType : uint32_t {
    Unknown  = 0,
    Mesh     = 1,
    Texture  = 2,
    Audio    = 3,
    Material = 4,
    Scene    = 5,
};

// Maps UUID → asset location (either a .hasset path or a .hpak entry)
struct AssetRecord {
    UUID                  id;
    AssetType             type;
    std::filesystem::path hassetPath;   // empty if loaded from .hpak
    uint64_t              hpakOffset = 0;
    uint64_t              hpakSize   = 0;
};

class AssetRegistry {
public:
    // Editor: scan a project directory for all .hasset files
    void scanDirectory(const std::filesystem::path& projectRoot);

    // Runtime: load the index from a .hpak archive
    void loadFromHpak(const std::filesystem::path& hpakPath);

    bool            has(const UUID& id) const;
    const AssetRecord* find(const UUID& id) const;

    void registerAsset(AssetRecord record);
    void unregisterAsset(const UUID& id);

    uint32_t count() const;

private:
    std::unordered_map<UUID, AssetRecord> records_;
};
```

---

### Asset base + concrete types

Create `include/HorizonAsset/Assets/MeshAsset.h`:

```cpp
#pragma once
#include <HorizonCore/Types/UUID.h>
#include <HorizonAsset/AssetRegistry.h>
#include <vector>
#include <HorizonCore/Math/Math.h>

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec4 tangent;
};

struct SubMesh {
    uint32_t indexOffset;
    uint32_t indexCount;
    uint32_t materialSlot;
};

struct MeshAsset {
    UUID                 id;
    std::vector<Vertex>  vertices;
    std::vector<uint32_t> indices;
    std::vector<SubMesh> subMeshes;
};
```

Create `include/HorizonAsset/Assets/TextureAsset.h`:

```cpp
#pragma once
#include <HorizonCore/Types/UUID.h>
#include <vector>
#include <cstdint>

enum class TextureFormat : uint32_t {
    RGBA8    = 0,
    BC1      = 1,   // DXT1 — opaque, compressed
    BC3      = 2,   // DXT5 — alpha, compressed
    BC7      = 3,   // high quality, compressed
};

struct TextureAsset {
    UUID          id;
    uint32_t      width, height, mipLevels;
    TextureFormat format;
    std::vector<uint8_t> pixels;  // all mip levels concatenated
};
```

Create `include/HorizonAsset/Assets/MaterialAsset.h`:

```cpp
#pragma once
#include <HorizonCore/Types/UUID.h>

struct MaterialAsset {
    UUID      id;
    UUID      albedoTexture;
    UUID      normalTexture;
    UUID      roughnessTexture;
    UUID      metallicTexture;
    float     roughness   = 0.5f;
    float     metallic    = 0.0f;
    bool      doubleSided = false;
};
```

Create `include/HorizonAsset/Assets/AudioAsset.h`:

```cpp
#pragma once
#include <HorizonCore/Types/UUID.h>
#include <vector>
#include <cstdint>

struct AudioAsset {
    UUID                 id;
    uint32_t             sampleRate;
    uint16_t             channels;
    uint16_t             bitsPerSample;
    std::vector<uint8_t> pcmData;   // decoded PCM, ready for audio backend
};
```

Create `include/HorizonAsset/Assets/SceneAsset.h`:

```cpp
#pragma once
#include <HorizonCore/Types/UUID.h>
#include <vector>
#include <cstdint>

// A SceneAsset is a serialised HorizonWorld.
// In editor: JSON blob. In hpak: binary blob from SceneSerializer::saveBinary().
struct SceneAsset {
    UUID                 id;
    std::vector<uint8_t> data;       // raw serialized bytes
    bool                 isBinary;   // false = JSON (editor), true = binary (runtime)
};
```

---

### AssetLoader and AssetCache (stubs)

Create `include/HorizonAsset/AssetLoader.h`:

```cpp
#pragma once
#include "AssetRegistry.h"
#include "Assets/MeshAsset.h"
#include "Assets/TextureAsset.h"
#include "Assets/AudioAsset.h"
#include "Assets/MaterialAsset.h"
#include "Assets/SceneAsset.h"
#include <memory>

class HpakReader;

// Loads assets from .hasset files (editor) or from an open HpakReader (runtime).
// Returns nullptr on failure.
class AssetLoader {
public:
    explicit AssetLoader(const AssetRegistry& registry,
                         HpakReader*          hpak = nullptr); // null = editor mode

    std::unique_ptr<MeshAsset>     loadMesh    (const UUID& id);
    std::unique_ptr<TextureAsset>  loadTexture (const UUID& id);
    std::unique_ptr<AudioAsset>    loadAudio   (const UUID& id);
    std::unique_ptr<MaterialAsset> loadMaterial(const UUID& id);
    std::unique_ptr<SceneAsset>    loadScene   (const UUID& id);

private:
    const AssetRegistry& registry_;
    HpakReader*          hpak_;

    std::vector<uint8_t> readRawData(const AssetRecord& record);
};
```

Create `include/HorizonAsset/AssetCache.h`:

```cpp
#pragma once
#include "AssetLoader.h"
#include <unordered_map>
#include <memory>
#include <cstdint>

// Ref-counted in-memory cache. Assets are evicted when refCount drops to 0.
// Thread-safety: single-threaded for now. Add mutex when async loading is added.
class AssetCache {
public:
    explicit AssetCache(AssetLoader& loader);

    // Increment ref, load if not cached.
    const MeshAsset*     getMesh    (const UUID& id);
    const TextureAsset*  getTexture (const UUID& id);
    const AudioAsset*    getAudio   (const UUID& id);
    const MaterialAsset* getMaterial(const UUID& id);
    const SceneAsset*    getScene   (const UUID& id);

    void retain (const UUID& id);
    void release(const UUID& id);   // evicts when refCount == 0

    uint32_t cachedCount() const;

private:
    struct CacheEntry {
        std::unique_ptr<void, void(*)(void*)> asset { nullptr, nullptr };
        uint32_t refCount = 0;
    };

    AssetLoader& loader_;
    std::unordered_map<UUID, CacheEntry> cache_;
};
```

Create `include/HorizonAsset/HpakReader.h`:

```cpp
#pragma once
#include "AssetRegistry.h"
#include <filesystem>
#include <vector>
#include <cstdint>

// Opens a .hpak archive and provides decrypted random-access reads.
// Uses AES-256-CBC via OpenSSL or mbedTLS (see CMakeLists.txt).
class HpakReader {
public:
    // key must be exactly 32 bytes (256 bit).
    bool open(const std::filesystem::path& hpakPath,
              const uint8_t key[32]);
    void close();
    bool isOpen() const;

    // Populate an AssetRegistry from this archive's index section.
    void populateRegistry(AssetRegistry& registry) const;

    // Read and decrypt a single asset's data block.
    // Returns empty vector on failure.
    std::vector<uint8_t> readEntry(const AssetRecord& record);

private:
    std::vector<uint8_t> iv_;          // 16-byte AES IV from file header
    std::filesystem::path path_;
    bool open_ = false;
};
```

---

### CMakeLists.txt for HorizonAsset

```cmake
add_library(HorizonAsset SHARED
    src/AssetRegistry.cpp
    src/AssetLoader.cpp
    src/AssetCache.cpp
    src/HpakReader.cpp
    src/Assets/MeshAsset.cpp
    src/Assets/TextureAsset.cpp
    src/Assets/AudioAsset.cpp
    src/Assets/MaterialAsset.cpp
    src/Assets/SceneAsset.cpp
)

target_include_directories(HorizonAsset
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(HorizonAsset
    PRIVATE HE_ASSET_BUILD_DLL
    PUBLIC  HE_ASSET_DLL
)

target_link_libraries(HorizonAsset
    PUBLIC  HorizonCore
    PRIVATE mbedtls          # AES-256 — add via FetchContent (see below)
)

# mbedTLS for AES-256-CBC (lightweight, no OpenSSL dependency)
# Add to root CMakeLists.txt:
# include(FetchContent)
# FetchContent_Declare(mbedtls
#     GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
#     GIT_TAG        v3.6.0
# )
# FetchContent_MakeAvailable(mbedtls)
```

---

## Part B — HorizonTools (editor pipeline, never shipped)

### Target folder structure

```
HE_Tools/
├── CMakeLists.txt
└── src/
    ├── AssetCompiler/
    │   ├── main.cpp               ← asset_compiler.exe entrypoint
    │   ├── MeshImporter.h/cpp     ← Assimp → MeshAsset
    │   ├── TextureImporter.h/cpp  ← stb_image → TextureAsset
    │   ├── AudioImporter.h/cpp    ← raw PCM → AudioAsset
    │   └── MaterialImporter.h/cpp ← JSON → MaterialAsset
    └── Packer/
        ├── main.cpp               ← hpak_packer.exe entrypoint
        ├── HpakWriter.h/cpp       ← writes .hpak from a list of .hasset files
        └── KeyDerivation.h/cpp    ← derives AES key from project secret
```

---

### MeshImporter stub

Create `src/AssetCompiler/MeshImporter.h`:

```cpp
#pragma once
#include <HorizonAsset/Assets/MeshAsset.h>
#include <filesystem>
#include <memory>

// Imports any Assimp-supported format (FBX, OBJ, GLTF, …) into a MeshAsset.
// Outputs a .hasset file alongside the source file.
class MeshImporter {
public:
    struct ImportSettings {
        bool  generateNormals    = true;
        bool  generateTangents   = true;
        bool  mergeSubMeshes     = false;
        float uniformScale       = 1.0f;
    };

    // Returns nullptr on failure. Writes .hasset to outputDir.
    std::unique_ptr<MeshAsset> import(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& outputDir,
        const ImportSettings& settings = {});
};
```

### TextureImporter stub

Create `src/AssetCompiler/TextureImporter.h`:

```cpp
#pragma once
#include <HorizonAsset/Assets/TextureAsset.h>
#include <filesystem>
#include <memory>

class TextureImporter {
public:
    struct ImportSettings {
        TextureFormat targetFormat  = TextureFormat::BC7;
        bool          generateMips  = true;
        uint32_t      maxResolution = 4096;
    };

    std::unique_ptr<TextureAsset> import(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& outputDir,
        const ImportSettings& settings = {});
};
```

### HpakWriter stub

Create `src/Packer/HpakWriter.h`:

```cpp
#pragma once
#include <HorizonAsset/AssetRegistry.h>
#include <filesystem>
#include <vector>
#include <cstdint>

// Reads all .hasset files from an AssetRegistry,
// encrypts their data blocks with AES-256-CBC,
// and writes a single .hpak archive.
class HpakWriter {
public:
    struct PackSettings {
        uint8_t  key[32];                    // AES-256 key (32 bytes)
        bool     compressBeforeEncrypt = true; // LZ4 compression before AES
    };

    bool write(const AssetRegistry&          registry,
               const std::filesystem::path&  outputPath,
               const PackSettings&           settings);
};
```

### KeyDerivation stub

Create `src/Packer/KeyDerivation.h`:

```cpp
#pragma once
#include <cstdint>
#include <string>

// Derives a deterministic 256-bit AES key from a project secret string.
// Uses PBKDF2-HMAC-SHA256 with a fixed salt per project.
// The same secret + same salt always yields the same key —
// so HorizonGame can re-derive the key at runtime without storing it in plaintext.
class KeyDerivation {
public:
    // secret = project-specific passphrase (never hardcode in source, load from env)
    // salt   = project UUID as bytes (stable, not secret)
    // outKey = exactly 32 bytes output
    static void derive(const std::string& secret,
                       const uint8_t      salt[16],
                       uint8_t            outKey[32]);
};
```

---

### CMakeLists.txt for HorizonTools

```cmake
# asset_compiler.exe
add_executable(asset_compiler
    src/AssetCompiler/main.cpp
    src/AssetCompiler/MeshImporter.cpp
    src/AssetCompiler/TextureImporter.cpp
    src/AssetCompiler/AudioImporter.cpp
    src/AssetCompiler/MaterialImporter.cpp
)
target_link_libraries(asset_compiler
    PRIVATE HorizonAsset
    PRIVATE HorizonCore
    PRIVATE assimp       # FetchContent or vcpkg
    PRIVATE stb          # header-only, FetchContent
)
target_include_directories(asset_compiler PRIVATE src/AssetCompiler)

# hpak_packer.exe
add_executable(hpak_packer
    src/Packer/main.cpp
    src/Packer/HpakWriter.cpp
    src/Packer/KeyDerivation.cpp
)
target_link_libraries(hpak_packer
    PRIVATE HorizonAsset
    PRIVATE HorizonCore
    PRIVATE mbedtls
    PRIVATE lz4          # FetchContent — LZ4 compression before encryption
)
target_include_directories(hpak_packer PRIVATE src/Packer)

# HorizonTools is NEVER linked into HorizonGame or any runtime target.
# It is an editor/build-time only dependency.
```

---

## Notes for Copilot

- `AssetCache` uses type-erased `void*` storage internally for simplicity.
  A cleaner approach for later: one typed `SlotMap<T>` per asset type,
  same pattern as `RenderResourceManager`.
- `HpakReader` and `HpakWriter` must use the same IV derivation logic.
  IV is stored in the .hpak header (not secret — randomness is enough for CBC).
- `KeyDerivation::derive()` is called by both `hpak_packer` (at build time)
  and `HorizonGame` (at runtime). The secret must never be hardcoded —
  load it from an environment variable or a build-time CMake define:
  `target_compile_definitions(HorizonGame PRIVATE HE_PROJECT_SECRET="...")`.
  For shipping: inject via CI/CD pipeline, not source code.
- `stb_image` is header-only. Add to root CMakeLists.txt:
  ```cmake
  FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git)
  FetchContent_MakeAvailable(stb)
  ```
  Then in TextureImporter.cpp: `#define STB_IMAGE_IMPLEMENTATION` before include.
- Assimp via FetchContent:
  ```cmake
  FetchContent_Declare(assimp
      GIT_REPOSITORY https://github.com/assimp/assimp.git
      GIT_TAG        v5.3.1
  )
  set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(ASSIMP_INSTALL     OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(assimp)
  ```
- LZ4 via FetchContent:
  ```cmake
  FetchContent_Declare(lz4
      GIT_REPOSITORY https://github.com/lz4/lz4.git
      GIT_TAG        v1.9.4
  )
  FetchContent_MakeAvailable(lz4)
  ```
