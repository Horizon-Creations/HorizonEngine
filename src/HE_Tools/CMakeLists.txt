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

# ─── HorizonImporters (static lib, shared by asset_compiler and the editor) ───
add_library(HorizonImporters STATIC
    src/AssetImporter/ImporterCommon.cpp
    src/AssetImporter/TextureImporter.cpp
    src/AssetImporter/MeshImporter.cpp
    src/AssetImporter/SkeletalMeshImporter.cpp
    src/AssetImporter/MaterialImporter.cpp
    src/AssetImporter/AudioImporter.cpp
    src/AssetImporter/FontImporter.cpp
)
set_target_properties(HorizonImporters PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(HorizonImporters
    PUBLIC  HorizonCore
    PUBLIC  glm::glm
    PRIVATE nlohmann_json::nlohmann_json
)
target_include_directories(HorizonImporters
    PUBLIC  src/AssetImporter
    PRIVATE vendor   # stb_image.h, cgltf.h, dr_wav.h
)

# ─── asset_compiler.exe ────────────────────────────────────────────────────────
add_executable(asset_compiler
    src/AssetCompiler/main.cpp
)
target_link_libraries(asset_compiler
    PRIVATE HorizonImporters
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
    # PRIVATE mbedtls  # FetchContent — replace XOR with AES-256-CBC
)
if(HE_HAVE_LZ4)
    target_link_libraries(hpak_packer PRIVATE lz4::lz4)
    target_compile_definitions(hpak_packer PRIVATE HE_HAVE_LZ4)
endif()
if(HE_HAVE_ZSTD)
    target_link_libraries(hpak_packer PRIVATE zstd::zstd)
    target_compile_definitions(hpak_packer PRIVATE HE_HAVE_ZSTD)
endif()
if(HE_HAVE_OPENSSL)
    target_link_libraries(hpak_packer PRIVATE OpenSSL::Crypto)
    target_compile_definitions(hpak_packer PRIVATE HE_HAVE_OPENSSL)
endif()
target_include_directories(hpak_packer
    PRIVATE src/Packer
    PRIVATE ${CMAKE_SOURCE_DIR}/src/HE_Core/include
)

# ─── hc_codegen.exe (HorizonCode → C++ generation CLI, plan §10.2) ────────────
add_executable(hc_codegen
    src/HcCodegenCli/main.cpp
)
target_link_libraries(hc_codegen
    PRIVATE HorizonCore
    PRIVATE HorizonScene
)
target_include_directories(hc_codegen
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/fixtures/hcodegen   # the shared parity fixtures
)

# hc_codegen runs as a build-time custom command (tests/CMakeLists.txt: parity
# fixture generation) with no rpath equivalent and nothing guaranteeing
# HorizonCore.dll/HorizonScene.dll are on PATH — Windows can otherwise resolve a
# stale copy from an unrelated directory (e.g. a leftover tests/ build output)
# and crash with STATUS_ENTRYPOINT_NOT_FOUND after an ABI change. Copying next
# to the exe guarantees it's found first (mirrors tests/CMakeLists.txt's he_tests).
if(WIN32)
    add_custom_command(TARGET hc_codegen POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:HorizonCore>  $<TARGET_FILE_DIR:hc_codegen>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:HorizonScene> $<TARGET_FILE_DIR:hc_codegen>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3-shared>  $<TARGET_FILE_DIR:hc_codegen>
        VERBATIM
    )
endif()

# ─── shader_compiler.exe ───────────────────────────────────────────────────────
add_executable(shader_compiler
    src/ShaderCompiler/main.cpp
)
# Note: shader_compiler requires glslc (from Vulkan SDK) in PATH
target_include_directories(shader_compiler PRIVATE src/ShaderCompiler)

# ─── mesh_gen.exe (generates the built-in default primitive meshes) ───────────
# One-shot generator: mesh_gen <out-dir> writes Cube/Sphere/Cylinder/… .hasset
# files (StaticMesh) via ContentManager, byte-identical to editor-authored ones.
# Run manually to (re)populate EditorDeps/EngineContent/Meshes; the outputs are
# committed, so this is not part of the normal build graph.
add_executable(mesh_gen
    src/MeshGen/main.cpp
)
target_link_libraries(mesh_gen PRIVATE HorizonCore)
target_include_directories(mesh_gen PRIVATE ${CMAKE_SOURCE_DIR}/src/HE_Core/include)

# HorizonTools is NEVER linked into HorizonGame or any runtime target.
# It is an editor/build-time only dependency.