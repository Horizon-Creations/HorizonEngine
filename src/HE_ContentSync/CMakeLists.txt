# ─── HorizonContentSync ────────────────────────────────────────────────────────
# On-demand SFTP download of the Editor's shared EngineContent asset library, so
# an Editor install does not have to ship every default asset up front.
#
# Editor-only, same reasoning as HorizonSourceControl: not added to HE_Game's or
# hc_codegen's deploy/copy lists — a player's binary has no business carrying an
# SSH client.
#
# Unlike HorizonSourceControl (which shells out to the `git` CLI), this links
# libssh2 directly: the configured SFTP account uses password auth, and the
# plain OpenSSH `sftp` CLI cannot do that non-interactively in batch mode. Only
# built when the root CMakeLists resolved a usable libssh2 (HE_HAVE_LIBSSH2).
add_library(HorizonContentSync SHARED
	src/SftpCredentials.cpp
	src/SftpClient.cpp
	src/SftpProbe.cpp
	src/EngineContentManifest.cpp
	src/EngineContentSync.cpp
	src/EngineContentPublish.cpp
)

# HorizonCore for the engine log. HorizonNet for HE::Net::Socket (Layer 0 —
# libssh2 handshakes directly on the raw socket handle it hands over).
target_link_libraries(HorizonContentSync PRIVATE HorizonCore HorizonNet nlohmann_json::nlohmann_json)

if(TARGET libssh2)
	target_link_libraries(HorizonContentSync PRIVATE libssh2)
else()
	target_link_libraries(HorizonContentSync PRIVATE Libssh2::libssh2)
endif()

target_include_directories(HorizonContentSync
	PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(HorizonContentSync PUBLIC cxx_std_20)

# Explicit export macro (HE_CS_API), same discipline as HorizonCore/HorizonNet/
# HorizonSourceControl — keeps the ABI surface deliberate.
target_compile_definitions(HorizonContentSync
	PRIVATE HE_CS_BUILD_DLL
	PUBLIC  HE_CS_DLL
)

# ─── the EngineContent SFTP endpoint ──────────────────────────────────────────
# Never a value in a tracked file: this repository is public, and a credential
# committed once stays greppable in every clone and every fork, whether or not
# it is rotated afterwards. It reaches the binary as a compile definition
# instead. Three ways in, in this order of precedence:
#
#   1. cmake/EngineContentCredentials.cmake — gitignored, for a local build that
#      has to actually reach the server (publishing content, testing a sync).
#      Copy the .template beside it and fill it in. It uses PLAIN set(), not
#      set(CACHE): a cache set without FORCE is a no-op once the entry exists,
#      so on any build directory that had already been configured the file would
#      silently do nothing. A normal variable shadows the cache entry for the
#      rest of this directory instead, which is all the definitions below read.
#   2. -DHE_ENGINE_CONTENT_HOST=… on the configure line, or the environment
#      variable of the same name. CI uses the environment form, so a password
#      never has to survive a shell quoting round-trip — one containing a $ or
#      a backtick would not — and nothing lands in CMakeCache.txt on the runner.
#   3. Nothing. Host and username stay empty, SftpProbe reports "not configured"
#      and the Editor behaves exactly as it did before the remote library
#      existed. This is what anyone cloning the repo gets, and it is fine.
if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/EngineContentCredentials.cmake")
	include("${CMAKE_SOURCE_DIR}/cmake/EngineContentCredentials.cmake")
endif()
# The cache entries are where a -D lands; they also give cmake-gui a knob. A
# normal variable from the local file above shadows them from here on.
set(HE_ENGINE_CONTENT_HOST     ""   CACHE STRING "SFTP host for the shared EngineContent library (empty = feature off)")
set(HE_ENGINE_CONTENT_PORT     "22" CACHE STRING "SFTP port for the EngineContent library")
set(HE_ENGINE_CONTENT_USER     ""   CACHE STRING "SFTP username for the EngineContent library")
set(HE_ENGINE_CONTENT_PASSWORD ""   CACHE STRING "SFTP password for the EngineContent library")
set(HE_ENGINE_CONTENT_BASEPATH ""   CACHE STRING "Remote base path ('' = the SFTP account's own root)")

# The environment form — for whatever the two routes above left empty.
foreach(_hecs_var HOST USER PASSWORD BASEPATH)
	if(HE_ENGINE_CONTENT_${_hecs_var} STREQUAL "" AND DEFINED ENV{HE_ENGINE_CONTENT_${_hecs_var}})
		set(HE_ENGINE_CONTENT_${_hecs_var} "$ENV{HE_ENGINE_CONTENT_${_hecs_var}}")
	endif()
endforeach()
unset(_hecs_var)

target_compile_definitions(HorizonContentSync PRIVATE
	HE_ENGINE_CONTENT_HOST="${HE_ENGINE_CONTENT_HOST}"
	HE_ENGINE_CONTENT_PORT=${HE_ENGINE_CONTENT_PORT}
	HE_ENGINE_CONTENT_USER="${HE_ENGINE_CONTENT_USER}"
	HE_ENGINE_CONTENT_PASSWORD="${HE_ENGINE_CONTENT_PASSWORD}"
	HE_ENGINE_CONTENT_BASEPATH="${HE_ENGINE_CONTENT_BASEPATH}"
)

if(HE_ENGINE_CONTENT_HOST STREQUAL "" OR HE_ENGINE_CONTENT_USER STREQUAL "")
	message(STATUS "HorizonContentSync: EngineContent endpoint not configured — the remote library stays off")
else()
	# Host and user only — the password is never echoed, not even into a local log.
	message(STATUS "HorizonContentSync: EngineContent endpoint ${HE_ENGINE_CONTENT_USER}@${HE_ENGINE_CONTENT_HOST}")
endif()

he_deploy_dll(HorizonContentSync)
