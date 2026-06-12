cmake_minimum_required(VERSION 3.20)
project(HorizonTools VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(HorizonTools SHARED
            src/FileOps/ProjectManager.cpp
)

target_compile_definitions(HorizonTools PRIVATE HE_TOOLS_BUILD_DLL)

target_link_libraries(HorizonTools
    PRIVATE HorizonCore
    PRIVATE nlohmann_json::nlohmann_json
    # PRIVATE mbedtls  # FetchContent — add when implementing encryption in ProjectManager
)

target_include_directories(HorizonTools PUBLIC src/FileOps)

he_deploy_dll(HorizonTools)

# ─── asset_compiler.exe ────────────────────────────────────────────────────────
add_executable(asset_compiler
    src/AssetCompiler/main.cpp
)
target_link_libraries(asset_compiler
    PRIVATE HorizonCore
    # PRIVATE assimp   # FetchContent or vcpkg — add when implementing importers
    # PRIVATE stb      # header-only — add when implementing TextureImporter
)
target_include_directories(asset_compiler PRIVATE src/AssetCompiler)

# ─── hpak_packer.exe ───────────────────────────────────────────────────────────
add_executable(hpak_packer
    src/Packer/main.cpp
    src/Packer/HpakWriter.cpp
    src/Packer/KeyDerivation.cpp
)
target_link_libraries(hpak_packer
    PRIVATE HorizonCore
    PRIVATE HorizonScene
    # PRIVATE mbedtls  # FetchContent — add when implementing AES + PBKDF2
    # PRIVATE lz4      # FetchContent — LZ4 compression before encryption
)
target_include_directories(hpak_packer PRIVATE src/Packer)

# ─── shader_compiler.exe ───────────────────────────────────────────────────────
add_executable(shader_compiler
    src/ShaderCompiler/main.cpp
)
# Note: shader_compiler requires glslc (from Vulkan SDK) in PATH
target_include_directories(shader_compiler PRIVATE src/ShaderCompiler)

# HorizonTools is NEVER linked into HorizonGame or any runtime target.
# It is an editor/build-time only dependency.