# ─── AMD FSR (FidelityFX SDK) upscaler: headers + signed runtime DLLs ─────────────
# docs/upscalers-fsr-dlss-xess-plan-2026-11-27.md. The AMD FSR SDK v2.4.0 (FSR 3.1.x and FSR 4 behind the FidelityFX
# API) is consumed as AMD ships it: a handful of C headers to compile against, and signed DLLs that are loaded at RUN
# time (ffx_api_loader.h: LoadLibrary + GetProcAddress), so nothing is linked. Windows x64, D3D12 and Vulkan.
#
# Downloaded at CONFIGURE time, file by file from the tag's commit with a pinned SHA-256 each (the repository ships no
# release archive; the whole tree is 550 MB, these files 94 MB), into the build tree's _deps — Steam Audio's pattern.
# A failed download is a warning and FSR is left out; configure never fails on it. HE_FSR_SDK_ROOT points at a folder
# laid out like Kits/FidelityFX of the SDK (api/, upscalers/, signedbin/, docs/license.md) and skips the download.
#
# License (docs/license.md of the SDK): binary redistribution free of charge, the notice must go with the software,
# no reverse engineering. The license file is copied next to the DLLs in the editor deploy and the game.
option(HE_ENABLE_FSR "Download the AMD FSR SDK runtime and compile the FSR upscaler (D3D12 / Vulkan)" ON)
set(HE_FSR_SDK_ROOT "" CACHE PATH "Folder laid out like the FSR SDK's Kits/FidelityFX; empty = download at configure")
set(HE_FSR_SDK_VERSION "2.4.0")
set(HE_FSR_SDK_COMMIT "7539ae8b6568ad29ed78a753404351daf3b5cca7")   # tag v2.4.0
set(HE_HAVE_FSR FALSE)
set(HE_FSR_INCLUDE_DIRS "")
set(HE_FSR_RUNTIME_FILES "")   # the DLLs and the license, copied beside the engine

if(HE_ENABLE_FSR AND NOT HE_RUNTIME_FLAVOR STREQUAL "game")
    message(STATUS "HorizonEngine: runtime flavour '${HE_RUNTIME_FLAVOR}' — FSR left out")
elseif(HE_ENABLE_FSR AND WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "(AMD64|x86_64|x64)")
    # path under Kits/FidelityFX, SHA-256
    set(_fsr_files
        "api/include/ffx_api.h"                     4d9dcf243ef5efbf03340a02f16b51a22053243a22e8a9a0b3ce91c26982b4bf
        "api/include/ffx_api_types.h"               53f07c113a9ba493f297656e4481b6539aed0afc9f3cc8da682aa477d270c46c
        "api/include/ffx_api_loader.h"              18d1901dbbc881e8131b91c65ad44bb3b4a1de6c31435d5b75236f16727ba80e
        "api/include/dx12/ffx_api_dx12.h"           c9afbc3c4c673723e9e5369a666f0ff54560543b5a93537f8ff453bf1c3bf530
        "api/include/vk/ffx_api_vk.h"               09ac1855bc4c2f711243d6982909385072ca3f8d30db44d1a82ee5bd637832ad
        "upscalers/include/ffx_upscale.h"           13091e7fe5fd9c82b1fb7c044b9f08f664d9653f599d2ea500738709236fca4d
        "docs/license.md"                           32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb
        "signedbin/amd_fidelityfx_loader_dx12.dll"  e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb
        "signedbin/amd_fidelityfx_loader_vk.dll"    38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9
        "signedbin/amd_fidelityfx_upscaler_dx12.dll" aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace
        "signedbin/amd_fidelityfx_upscaler_vk.dll"  4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7)
    set(_fsr_root "${HE_FSR_SDK_ROOT}")
    if(NOT _fsr_root)
        set(_fsr_root "${CMAKE_BINARY_DIR}/_deps/fsr-sdk-${HE_FSR_SDK_VERSION}")
        set(_fsr_base "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/${HE_FSR_SDK_COMMIT}/Kits/FidelityFX")
        set(_fsr_ok TRUE)
        list(LENGTH _fsr_files _fsr_n)
        math(EXPR _fsr_last "${_fsr_n} - 1")
        foreach(_i RANGE 0 ${_fsr_last} 2)
            math(EXPR _j "${_i} + 1")
            list(GET _fsr_files ${_i} _f)
            list(GET _fsr_files ${_j} _h)
            if(EXISTS "${_fsr_root}/${_f}")
                file(SHA256 "${_fsr_root}/${_f}" _have)
                if(_have STREQUAL _h)
                    continue()
                endif()
            endif()
            if(_f MATCHES "upscaler_")
                message(STATUS "HorizonEngine: downloading FSR SDK ${HE_FSR_SDK_VERSION} ${_f} (once per build tree)")
            endif()
            file(DOWNLOAD "${_fsr_base}/${_f}" "${_fsr_root}/${_f}" EXPECTED_HASH SHA256=${_h} STATUS _st)
            list(GET _st 0 _rc)
            if(NOT _rc EQUAL 0)
                list(GET _st 1 _msg)
                message(WARNING "HorizonEngine: FSR SDK download of ${_f} failed (${_msg}) — FSR left out. "
                                "Configure with -DHE_FSR_SDK_ROOT=<Kits/FidelityFX of the SDK> or -DHE_ENABLE_FSR=OFF.")
                file(REMOVE "${_fsr_root}/${_f}")
                set(_fsr_ok FALSE)
                break()
            endif()
        endforeach()
    endif()
    set(_fsr_runtime
        "${_fsr_root}/signedbin/amd_fidelityfx_loader_dx12.dll" "${_fsr_root}/signedbin/amd_fidelityfx_upscaler_dx12.dll"
        "${_fsr_root}/signedbin/amd_fidelityfx_loader_vk.dll"   "${_fsr_root}/signedbin/amd_fidelityfx_upscaler_vk.dll")
    set(_fsr_complete TRUE)
    foreach(_p "${_fsr_root}/api/include/ffx_api.h" "${_fsr_root}/upscalers/include/ffx_upscale.h" ${_fsr_runtime})
        if(NOT EXISTS "${_p}")
            set(_fsr_complete FALSE)
        endif()
    endforeach()
    if(_fsr_complete)
        set(HE_HAVE_FSR TRUE)
        set(HE_FSR_INCLUDE_DIRS "${_fsr_root}/api/include" "${_fsr_root}/upscalers/include")
        # The license travels with the DLLs, under a name that says whose it is.
        configure_file("${_fsr_root}/docs/license.md" "${_fsr_root}/AMD_FSR_SDK_LICENSE.md" COPYONLY)
        set(HE_FSR_RUNTIME_FILES ${_fsr_runtime} "${_fsr_root}/AMD_FSR_SDK_LICENSE.md")
        message(STATUS "HorizonEngine: AMD FSR SDK ${HE_FSR_SDK_VERSION} enabled (${_fsr_root})")
    else()
        message(WARNING "HorizonEngine: FSR SDK incomplete under '${_fsr_root}' — FSR left out")
    endif()
elseif(HE_ENABLE_FSR)
    message(STATUS "HorizonEngine: FSR needs Windows x64 (D3D12 / Vulkan) — left out")
endif()
