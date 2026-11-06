# MoltenVK as the Vulkan implementation on macOS (on by default when found,
# -DHE_ENABLE_MOLTENVK=OFF to leave it out).
#
# macOS has no Vulkan loader of its own: Vulkan runs through MoltenVK, which maps it
# onto Metal. The engine's Vulkan backend (RendererVulkan, the ImGui bridge, the SPIR-V
# scene shaders) is the same code as on Windows/Linux; this module only finds
# MoltenVK and hands it to the existing "find Vulkan" logic. We link libMoltenVK
# directly instead of going through the Khronos loader, so there is no ICD lookup and no
# VK_ICD_FILENAMES to set. SDL's Vulkan loader resolves to the same dylib.
#
# Sets, when MoltenVK was found:
#   HE_MOLTENVK_INCLUDE_DIR   directory that holds vulkan/vulkan.h
#   HE_MOLTENVK_LIBRARY       libMoltenVK.dylib
# and leaves them empty otherwise (the Vulkan backend then stays off, as before).
#
# Search order: -DHE_MOLTENVK_ROOT=<dir>, the LunarG SDK ($VULKAN_SDK/macOS or
# $VULKAN_SDK itself), then Homebrew's molten-vk (Apple silicon, then Intel).

set(HE_MOLTENVK_INCLUDE_DIR "" CACHE INTERNAL "MoltenVK: directory with vulkan/vulkan.h")
set(HE_MOLTENVK_LIBRARY     "" CACHE INTERNAL "MoltenVK: libMoltenVK.dylib")

if(APPLE AND HE_ENABLE_MOLTENVK)
    set(HE_MOLTENVK_ROOT "" CACHE PATH "Prefix of a MoltenVK install (overrides the search)")

    set(_he_mvk_libhints
        "${HE_MOLTENVK_ROOT}/lib" "${HE_MOLTENVK_ROOT}/macOS/lib"
        "$ENV{VULKAN_SDK}/macOS/lib" "$ENV{VULKAN_SDK}/lib"
        /opt/homebrew/opt/molten-vk/lib /usr/local/opt/molten-vk/lib)
    set(_he_mvk_inchints
        "${HE_MOLTENVK_ROOT}/libexec/include" "${HE_MOLTENVK_ROOT}/include" "${HE_MOLTENVK_ROOT}/macOS/include"
        "$ENV{VULKAN_SDK}/macOS/include" "$ENV{VULKAN_SDK}/include"
        /opt/homebrew/opt/molten-vk/libexec/include /usr/local/opt/molten-vk/libexec/include)

    # Local variables, not cache entries: a stale NOTFOUND in the cache would pin the
    # result after MoltenVK gets installed, and the answer is cheap to recompute.
    unset(_he_mvk_lib CACHE)
    unset(_he_mvk_inc CACHE)
    find_library(_he_mvk_lib NAMES MoltenVK HINTS ${_he_mvk_libhints} NO_DEFAULT_PATH)
    find_path(_he_mvk_inc NAMES vulkan/vulkan.h HINTS ${_he_mvk_inchints} NO_DEFAULT_PATH)

    if(_he_mvk_lib AND _he_mvk_inc)
        set(HE_MOLTENVK_LIBRARY     "${_he_mvk_lib}" CACHE INTERNAL "MoltenVK: libMoltenVK.dylib" FORCE)
        set(HE_MOLTENVK_INCLUDE_DIR "${_he_mvk_inc}" CACHE INTERNAL "MoltenVK: directory with vulkan/vulkan.h" FORCE)
        # FindVulkan takes these two as presets (the VULKAN_SDK branches in
        # src/HE_Rendering and src/HE_Editor do the same). They are cache entries, not
        # directory variables: cmake/HorizonEngineBundle.cmake runs find_package(Vulkan)
        # again at the root scope, and an earlier NOTFOUND in the cache would win there.
        set(Vulkan_INCLUDE_DIR "${_he_mvk_inc}" CACHE PATH     "Vulkan include dir (MoltenVK)" FORCE)
        set(Vulkan_LIBRARY     "${_he_mvk_lib}" CACHE FILEPATH "Vulkan library (MoltenVK)"     FORCE)
        message(STATUS "MoltenVK: ${HE_MOLTENVK_LIBRARY} (headers ${HE_MOLTENVK_INCLUDE_DIR})")
    else()
        # The default is ON, so a Mac without MoltenVK is the normal case, not a mistake:
        # only an explicit -DHE_MOLTENVK_ROOT that finds nothing is worth a warning.
        if(HE_MOLTENVK_ROOT)
            message(WARNING "MoltenVK not found under HE_MOLTENVK_ROOT=${HE_MOLTENVK_ROOT}. "
                            "The Vulkan backend stays off.")
        else()
            message(STATUS "MoltenVK: not found (brew install molten-vk) - Vulkan backend off on macOS")
        endif()
    endif()
    unset(_he_mvk_lib CACHE)
    unset(_he_mvk_inc CACHE)
endif()
