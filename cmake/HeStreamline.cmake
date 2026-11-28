# ─── NVIDIA DLSS through Streamline: headers + signed runtime DLLs ──────────────────
# docs/upscalers-fsr-dlss-xess-plan-2026-11-27.md (step 15). Streamline 2.14.16 (NVIDIA-RTX/Streamline) is consumed as NVIDIA
# ships it: C++ headers to compile against and NVIDIA-signed DLLs loaded at RUN time (sl.interposer.dll by LoadLibrary after
# sl::security::verifyEmbeddedSignature, manual hooking), so nothing is linked. Windows x64, D3D12 and Vulkan.
#
# Downloaded at CONFIGURE time: the release ZIP (276 MB, pinned SHA-256; the repository holds no binaries), once per build
# tree, into _deps; only the files below are extracted and each is checked against its own hash, then the ZIP is deleted.
# A failed download is a warning and DLSS is left out; configure never fails on it. HE_SL_SDK_ROOT points at a folder laid
# out like the ZIP (include/, bin/x64/, license.txt) and skips the download.
#
# Licences: Streamline itself is MIT (license.txt); nvngx_dlss.dll is under the NVIDIA RTX SDK licence
# (nvngx_dlss.license.txt): an application using DLSS must attribute it and show the NVIDIA marks on its splash screen,
# about box and credits, and DLSS is for systems with NVIDIA GPUs. Both texts travel with the DLLs (editor and game).
option(HE_ENABLE_DLSS "Download NVIDIA Streamline + DLSS and compile the DLSS upscaler (D3D12 / Vulkan)" ON)
set(HE_SL_SDK_ROOT "" CACHE PATH "Folder laid out like the Streamline SDK ZIP; empty = download at configure")
set(HE_SL_SDK_VERSION "2.14.16")
set(HE_HAVE_STREAMLINE FALSE)
set(HE_SL_INCLUDE_DIRS "")
set(HE_SL_RUNTIME_FILES "")   # the DLLs and the licences, copied beside the engine

if(HE_ENABLE_DLSS AND NOT HE_RUNTIME_FLAVOR STREQUAL "game")
    message(STATUS "HorizonEngine: runtime flavour '${HE_RUNTIME_FLAVOR}' — DLSS left out")
elseif(HE_ENABLE_DLSS AND WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "(AMD64|x86_64|x64)")
    # path inside the ZIP, SHA-256 (the DLLs are NVIDIA-signed; production, not development builds)
    set(_sl_files
        "include/sl.h"                    ""
        "include/sl_appidentity.h"        ""
        "include/sl_consts.h"             ""
        "include/sl_core_api.h"           ""
        "include/sl_core_types.h"         ""
        "include/sl_device_wrappers.h"    ""
        "include/sl_dlss.h"               ""
        "include/sl_dlss_g.h"             ""
        "include/sl_reflex.h"             ""
        "include/sl_pcl.h"                ""
        "include/sl_helpers.h"            ""
        "include/sl_helpers_vk.h"         ""
        "include/sl_hooks.h"              ""
        "include/sl_matrix_helpers.h"     ""
        "include/sl_result.h"             ""
        "include/sl_security.h"           ""
        "include/sl_struct.h"             ""
        "include/sl_template.h"           ""
        "include/sl_version.h"            ""
        "bin/x64/sl.interposer.dll"       b1d7395aebd7b9722841d64dcf7433680020b6a8e421f851c16d5ba0c2182edf
        "bin/x64/sl.common.dll"           38eea3b4209afc455636ea372b5933d65fcc13becec6c84d101dcfc4d8648f75
        "bin/x64/sl.dlss.dll"             18871eea87c90345da97d8109faa81ea7d5840e8d99b51b55b0e46cfa177f84e
        "bin/x64/nvngx_dlss.dll"          d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e
        # DLSS Frame Generation (D3D12, the game's swapchain) and the Reflex + PC Latency markers it requires.
        "bin/x64/sl.dlss_g.dll"           3ed9538df724993bea8a050195a6a78b9c8c8a566d5b05ade477aab3c390e90f
        "bin/x64/nvngx_dlssg.dll"         8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7
        "bin/x64/sl.reflex.dll"           443577e869352436ff7ec8f4d9c90418d3e95368436ce219261e0f4f07a38122
        "bin/x64/sl.pcl.dll"              5698c38e722318681f215c9fe68e45b9bedd51378acc699d8eee26e961e57770
        "bin/x64/nvngx_dlss.license.txt"  3027f23ca5a46dd9cb8183fbd522983a86f64d7daac5982912bf9f214671f294
        # sl.common loads it on Vulkan (NVIDIA's low-latency layer; logs an error without it). Its licence is Reflex's.
        "bin/x64/NvLowLatencyVk.dll"      2a77dc3e1c724b7eea5755be0ae7423752e79a2459fae72181a9f00e3507e5d6
        "bin/x64/reflex.license.txt"      ebf83c07fb3b2939908c3795d887afde3161c89a28ba391724efc784ce1bdabe
        "license.txt"                     7b6f23e7d6f3ad6292f9308d2b42cdc3d82ae4e9b2abb55f230279d83bedd43d
        "3rd-party-licenses.md"           cb251639994465f31d2178c16acedf1ff4f9cef1c370a09782ed36d4c86c1dca)
    set(_sl_root "${HE_SL_SDK_ROOT}")
    if(NOT _sl_root)
        set(_sl_root "${CMAKE_BINARY_DIR}/_deps/streamline-${HE_SL_SDK_VERSION}")
        # Complete already (every pinned file present with its hash)?
        set(_sl_have TRUE)
        list(LENGTH _sl_files _sl_n)
        math(EXPR _sl_last "${_sl_n} - 1")
        foreach(_i RANGE 0 ${_sl_last} 2)
            math(EXPR _j "${_i} + 1")
            list(GET _sl_files ${_i} _f)
            list(GET _sl_files ${_j} _h)
            if(NOT EXISTS "${_sl_root}/${_f}")
                set(_sl_have FALSE)
            elseif(_h)
                file(SHA256 "${_sl_root}/${_f}" _got)
                if(NOT _got STREQUAL _h)
                    set(_sl_have FALSE)
                endif()
            endif()
        endforeach()
        if(NOT _sl_have)
            set(_zip "${_sl_root}/streamline-sdk-v${HE_SL_SDK_VERSION}.zip")
            message(STATUS "HorizonEngine: downloading NVIDIA Streamline ${HE_SL_SDK_VERSION} (276 MB, once per build tree)")
            file(DOWNLOAD "https://github.com/NVIDIA-RTX/Streamline/releases/download/v${HE_SL_SDK_VERSION}/streamline-sdk-v${HE_SL_SDK_VERSION}.zip"
                 "${_zip}" EXPECTED_HASH SHA256=32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b STATUS _st)
            list(GET _st 0 _rc)
            if(_rc EQUAL 0)
                set(_members "")
                foreach(_i RANGE 0 ${_sl_last} 2)
                    list(GET _sl_files ${_i} _f)
                    list(APPEND _members "${_f}")
                endforeach()
                execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf "${_zip}" -- ${_members}
                                WORKING_DIRECTORY "${_sl_root}" RESULT_VARIABLE _xrc)
                if(NOT _xrc EQUAL 0)
                    message(WARNING "HorizonEngine: Streamline ZIP extraction failed (${_xrc}) — DLSS left out")
                endif()
            else()
                list(GET _st 1 _msg)
                message(WARNING "HorizonEngine: Streamline download failed (${_msg}) — DLSS left out. "
                                "Configure with -DHE_SL_SDK_ROOT=<unpacked SDK> or -DHE_ENABLE_DLSS=OFF.")
            endif()
            file(REMOVE "${_zip}")
        endif()
    endif()
    # Every pinned file present with its hash (also for HE_SL_SDK_ROOT: the same binaries or none).
    set(_sl_complete TRUE)
    list(LENGTH _sl_files _sl_n)
    math(EXPR _sl_last "${_sl_n} - 1")
    foreach(_i RANGE 0 ${_sl_last} 2)
        math(EXPR _j "${_i} + 1")
        list(GET _sl_files ${_i} _f)
        list(GET _sl_files ${_j} _h)
        if(NOT EXISTS "${_sl_root}/${_f}")
            set(_sl_complete FALSE)
        elseif(_h)
            file(SHA256 "${_sl_root}/${_f}" _got)
            if(NOT _got STREQUAL _h)
                message(WARNING "HorizonEngine: ${_sl_root}/${_f} has an unexpected SHA-256 — DLSS left out")
                set(_sl_complete FALSE)
            endif()
        endif()
    endforeach()
    if(_sl_complete)
        set(HE_HAVE_STREAMLINE TRUE)
        set(HE_SL_INCLUDE_DIRS "${_sl_root}/include")
        # The licences travel with the DLLs, under names that say whose they are.
        configure_file("${_sl_root}/license.txt" "${_sl_root}/NVIDIA_STREAMLINE_LICENSE.txt" COPYONLY)
        configure_file("${_sl_root}/3rd-party-licenses.md" "${_sl_root}/NVIDIA_STREAMLINE_3RD_PARTY_LICENSES.md" COPYONLY)
        configure_file("${_sl_root}/bin/x64/nvngx_dlss.license.txt" "${_sl_root}/NVIDIA_DLSS_LICENSE.txt" COPYONLY)
        configure_file("${_sl_root}/bin/x64/reflex.license.txt" "${_sl_root}/NVIDIA_REFLEX_LICENSE.txt" COPYONLY)
        set(HE_SL_RUNTIME_FILES
            "${_sl_root}/bin/x64/sl.interposer.dll" "${_sl_root}/bin/x64/sl.common.dll" "${_sl_root}/bin/x64/sl.dlss.dll"
            "${_sl_root}/bin/x64/nvngx_dlss.dll" "${_sl_root}/bin/x64/NvLowLatencyVk.dll"
            "${_sl_root}/bin/x64/sl.dlss_g.dll" "${_sl_root}/bin/x64/nvngx_dlssg.dll"
            "${_sl_root}/bin/x64/sl.reflex.dll" "${_sl_root}/bin/x64/sl.pcl.dll"
            "${_sl_root}/NVIDIA_STREAMLINE_LICENSE.txt" "${_sl_root}/NVIDIA_STREAMLINE_3RD_PARTY_LICENSES.md"
            "${_sl_root}/NVIDIA_DLSS_LICENSE.txt" "${_sl_root}/NVIDIA_REFLEX_LICENSE.txt")
        message(STATUS "HorizonEngine: NVIDIA Streamline ${HE_SL_SDK_VERSION} + DLSS enabled (${_sl_root})")
    else()
        message(WARNING "HorizonEngine: Streamline SDK incomplete under '${_sl_root}' — DLSS left out")
    endif()
elseif(HE_ENABLE_DLSS)
    message(STATUS "HorizonEngine: DLSS needs Windows x64 (D3D12 / Vulkan) — left out")
endif()
