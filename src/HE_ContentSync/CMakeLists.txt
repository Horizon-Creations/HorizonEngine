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
# instead. Three ways in, first one wins:
#
#   1. -DHE_ENGINE_CONTENT_HOST=… on the configure line, or the environment
#      variable of the same name. CI uses the environment form, so a password
#      never has to survive a shell quoting round-trip — one containing a $ or
#      a backtick would not.
#   2. cmake/EngineContentCredentials.cmake — gitignored, for a local build that
#      has to actually reach the server (publishing content, testing a sync).
#      It just set()s the same variables; copy the .template beside it.
#   3. Nothing. Host and username stay empty, SftpProbe reports "not configured"
#      and the Editor behaves exactly as it did before the remote library
#      existed. This is what anyone cloning the repo gets, and it is fine.
#
# The include goes first so a -D on the command line — whose cache entry already
# exists by this point — wins over the local file, and the empty defaults below
# only fill in whatever neither supplied.
if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/EngineContentCredentials.cmake")
	include("${CMAKE_SOURCE_DIR}/cmake/EngineContentCredentials.cmake")
endif()
set(HE_ENGINE_CONTENT_HOST     ""   CACHE STRING "SFTP host for the shared EngineContent library (empty = feature off)")
set(HE_ENGINE_CONTENT_PORT     "22" CACHE STRING "SFTP port for the EngineContent library")
set(HE_ENGINE_CONTENT_USER     ""   CACHE STRING "SFTP username for the EngineContent library")
set(HE_ENGINE_CONTENT_PASSWORD ""   CACHE STRING "SFTP password for the EngineContent library")
set(HE_ENGINE_CONTENT_BASEPATH ""   CACHE STRING "Remote base path ('' = the SFTP account's own root)")

# The environment form of route 1 — only for what nothing else supplied, so a
# -D and the local file both still win. A plain (non-cache) set() shadows the
# cache entry for the rest of this directory, which is all the definitions below
# need, and keeps the secret out of CMakeCache.txt on the CI runner.
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
