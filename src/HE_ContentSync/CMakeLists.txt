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
#      PORT is deliberately not on the environment route: its cache default (22)
#      is never empty, so the "only fill in what is still empty" test below can
#      never fire for it. A non-default port goes in the local file or a -D.
#   3. Nothing. Host and username stay empty, SftpProbe reports "not configured"
#      and the Editor behaves exactly as it did before the remote library
#      existed. This is what anyone cloning the repo gets, and it is fine.
#
# The cache entries are declared BEFORE the local file is included, and that
# order is load-bearing. A set(<var> <value> CACHE …) that actually writes its
# entry — i.e. the first configure in a build directory — ALSO deletes a normal
# variable of the same name from the current scope. That is CMP0126's OLD
# behaviour, which the root CMakeLists' cmake_minimum_required(VERSION 3.20)
# selects for the whole project. With the include() above the cache block, a
# fresh build directory therefore read the local file and threw its values away
# five lines later: empty definitions, "not configured", no warning. Only a
# SECOND configure of the same directory kept them (entries already there → no
# write → no deletion), which is exactly why that order looked verified.
# Declared first, the plain set()s in the local file shadow the cache entries
# for the rest of this directory, on every configure, fresh or not.
set(HE_ENGINE_CONTENT_HOST     ""   CACHE STRING "SFTP host for the shared EngineContent library (empty = feature off)")
set(HE_ENGINE_CONTENT_PORT     "22" CACHE STRING "SFTP port for the EngineContent library")
set(HE_ENGINE_CONTENT_USER     ""   CACHE STRING "SFTP username for the EngineContent library")
set(HE_ENGINE_CONTENT_PASSWORD ""   CACHE STRING "SFTP password for the EngineContent library")
set(HE_ENGINE_CONTENT_BASEPATH ""   CACHE STRING "Remote base path ('' = the SFTP account's own root)")

# A GLOB rather than an if(EXISTS), for the file's ARRIVAL. CMake re-runs the
# configure step for files it knows about, and a file that did not exist at
# configure time is not one of them — so copying the template into place and
# hitting Build would produce an incremental build that never re-reads anything,
# the same silent "I filled it in and nothing happened" this feature has already
# produced once. CONFIGURE_DEPENDS re-checks the glob at build time and triggers
# a reconfigure when the file appears (or is deleted again). Later EDITS to it
# are covered by the include() itself: an included file is a configure
# dependency automatically.
file(GLOB _hecs_local_credentials CONFIGURE_DEPENDS
	"${CMAKE_SOURCE_DIR}/cmake/EngineContentCredentials.cmake")
if(_hecs_local_credentials)
	include("${_hecs_local_credentials}")
endif()

# The environment form — for whatever the two routes above left empty.
foreach(_hecs_var HOST USER PASSWORD BASEPATH)
	if(HE_ENGINE_CONTENT_${_hecs_var} STREQUAL "" AND DEFINED ENV{HE_ENGINE_CONTENT_${_hecs_var}})
		set(HE_ENGINE_CONTENT_${_hecs_var} "$ENV{HE_ENGINE_CONTENT_${_hecs_var}}")
	endif()
endforeach()

# Escaped once, here, because these end up inside a C string literal. A hoster
# generates the password, and nothing stops it containing a backslash or a
# double quote — both of which would reach the compiler verbatim and break
# SftpCredentials.cpp with a syntax error pointing at a line that looks fine.
foreach(_hecs_var HOST USER PASSWORD BASEPATH)
	set(_hecs_val "${HE_ENGINE_CONTENT_${_hecs_var}}")
	string(REPLACE "\\" "\\\\" _hecs_val "${_hecs_val}")
	string(REPLACE "\"" "\\\"" _hecs_val "${_hecs_val}")
	set(_hecs_esc_${_hecs_var} "${_hecs_val}")
endforeach()
unset(_hecs_var)
unset(_hecs_val)

target_compile_definitions(HorizonContentSync PRIVATE
	HE_ENGINE_CONTENT_HOST="${_hecs_esc_HOST}"
	HE_ENGINE_CONTENT_PORT=${HE_ENGINE_CONTENT_PORT}
	HE_ENGINE_CONTENT_USER="${_hecs_esc_USER}"
	HE_ENGINE_CONTENT_PASSWORD="${_hecs_esc_PASSWORD}"
	HE_ENGINE_CONTENT_BASEPATH="${_hecs_esc_BASEPATH}"
)
foreach(_hecs_var HOST USER PASSWORD BASEPATH)
	unset(_hecs_esc_${_hecs_var})
endforeach()
unset(_hecs_var)

# What this reports is the ONLY place the state is visible at build time, and
# an Editor whose Engine folder just stays local-only is not a symptom anyone
# traces back on their own — so each way of being half-configured says which
# one it is, instead of one flat "not configured" for all of them.
if(HE_ENGINE_CONTENT_HOST STREQUAL "" OR HE_ENGINE_CONTENT_USER STREQUAL "")
	message(STATUS "HorizonContentSync: EngineContent endpoint not configured — the remote library stays off")
	if(_hecs_local_credentials)
		message(WARNING "cmake/EngineContentCredentials.cmake exists but leaves HE_ENGINE_CONTENT_HOST and/or _USER empty — the EngineContent library stays off.")
	else()
		message(STATUS "  → to switch it on: copy cmake/EngineContentCredentials.cmake.template to cmake/EngineContentCredentials.cmake and fill it in")
	endif()
elseif(HE_ENGINE_CONTENT_PASSWORD STREQUAL "")
	message(WARNING "HorizonContentSync: EngineContent host and user are set but the password is empty — every connection will fail authentication.")
else()
	# Host and user only — the password is never echoed, not even into a local log.
	message(STATUS "HorizonContentSync: EngineContent endpoint ${HE_ENGINE_CONTENT_USER}@${HE_ENGINE_CONTENT_HOST}:${HE_ENGINE_CONTENT_PORT}")
endif()
unset(_hecs_local_credentials)

he_deploy_dll(HorizonContentSync)
