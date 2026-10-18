# ─── HorizonSelfUpdate ─────────────────────────────────────────────────────────
# The Editor updating ITSELF: ask a remote manifest which build is current,
# compare it with the running one and, when it is newer, download that build into
# a staging directory and verify it. Taking the staged build over (replacing the
# install without losing an open project) is a later step and lives elsewhere —
# this module ends at "a verified file is waiting in <userDataDir>/SelfUpdate".
#
# Editor-only, same reasoning as HorizonContentSync and HorizonSourceControl: not
# in HE_Game's or hc_codegen's deploy/copy lists — a player's binary has no
# business carrying an SSH client or a way to replace itself.
#
# The SFTP transfer is HorizonContentSync's SftpClient (it takes the endpoint as
# an argument, so nothing in that module is touched or shared with this one). The
# account is a SECOND, separate one and is never the EngineContent account — see
# SelfUpdateEndpoint.h. It is compiled in the same way HorizonContentSync's is
# (the block below), so an installed editor can update itself without anyone
# setting anything; HE_SELFUPDATE_* in the process environment at run time
# replaces it as a whole (tests, CI's live leg, a developer's own server).
#
# Added from the root CMakeLists inside `if(HE_HAVE_LIBSSH2)`, right after
# HorizonContentSync, because it needs that target.
add_library(HorizonSelfUpdate SHARED
	src/SelfUpdateVersion.cpp
	src/SelfUpdateManifest.cpp
	src/SelfUpdateEndpoint.cpp
	src/SelfUpdateTransport.cpp
	src/SelfUpdateCheck.cpp
	src/SelfUpdateProcess.cpp
	src/SelfUpdateInstall.cpp
	src/SelfUpdateApplyCore.cpp
	src/Takeover.cpp
)

# HorizonCore for the engine log and HE::Crypto::Sha256. HorizonContentSync is
# PUBLIC because SelfUpdateEndpoint.h hands out a HE::Cs::SftpEndpoint, so a
# consumer needs that header (and, on Windows, its HE_CS_DLL import definition).
target_link_libraries(HorizonSelfUpdate
	PUBLIC  HorizonContentSync
	PRIVATE HorizonCore nlohmann_json::nlohmann_json)

target_include_directories(HorizonSelfUpdate
	PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(HorizonSelfUpdate PUBLIC cxx_std_20)

# Explicit export macro (HE_SU_API), same discipline as HE_CS_API / HE_SC_API.
target_compile_definitions(HorizonSelfUpdate
	PRIVATE HE_SU_BUILD_DLL
	PUBLIC  HE_SU_DLL
)

# ─── the self-update SFTP account ─────────────────────────────────────────────
# The same plumbing as HorizonContentSync's EngineContent endpoint (see
# src/HE_ContentSync/CMakeLists.txt for the long version of every "why" below),
# with its own names and its own file. Never a value in a tracked file: this
# repository is public. Three ways in, in this order of precedence:
#
#   1. cmake/SelfUpdateCredentials.cmake — gitignored, copied from the .template
#      beside it. Plain set(), not set(CACHE), so it also wins in a build
#      directory that was configured before.
#   2. -DHE_SELFUPDATE_HOST=… on the configure line, or the environment variable
#      of the same name at CONFIGURE time. CI uses the environment form (secrets
#      SELFUPDATE_HOST/USER/PASSWORD), which keeps the password off the command
#      line and out of CMakeCache.txt. PORT is not on the environment route, as
#      for ContentSync: its cache default (22) is never empty.
#   3. Nothing. The editor then has no compiled-in account and Help ▸ Check for
#      Updates… says "not configured" — unless HE_SELFUPDATE_* is set in its
#      environment at RUN time. That run-time override is not this file's
#      business: it is read by loadSelfUpdateConfig() and replaces whatever was
#      compiled in as a whole (SelfUpdateEndpoint.h).
#
# The account compiled in here ends up readable in every shipped editor binary,
# so it must be READ-ONLY on the server — the whole design rests on that.
#
# Cache entries BEFORE the include, and that order is load-bearing (CMP0126 OLD,
# selected by the root's cmake_minimum_required(3.20)): the first configure of a
# build directory writes the entries, and writing one deletes a normal variable
# of the same name — the local file's values would be thrown away on exactly the
# fresh build directory a new machine starts with.
set(HE_SELFUPDATE_HOST     ""   CACHE STRING "SFTP host of the editor's self-update account (empty = none compiled in)")
set(HE_SELFUPDATE_PORT     "22" CACHE STRING "SFTP port of the self-update account")
set(HE_SELFUPDATE_USER     ""   CACHE STRING "SFTP username of the self-update account (read-only login)")
set(HE_SELFUPDATE_PASSWORD ""   CACHE STRING "SFTP password of the self-update account")
set(HE_SELFUPDATE_BASEPATH ""   CACHE STRING "Remote base path holding packages/ and preview-packages/ ('' = the account's own root)")

# A GLOB rather than an if(EXISTS), so the file's ARRIVAL triggers a reconfigure
# (CONFIGURE_DEPENDS); later edits are covered by the include() itself.
file(GLOB _hesu_local_credentials CONFIGURE_DEPENDS
	"${CMAKE_SOURCE_DIR}/cmake/SelfUpdateCredentials.cmake")
if(_hesu_local_credentials)
	include("${_hesu_local_credentials}")
endif()

# The environment form — for whatever the two routes above left empty.
foreach(_hesu_var HOST USER PASSWORD BASEPATH)
	if(HE_SELFUPDATE_${_hesu_var} STREQUAL "" AND DEFINED ENV{HE_SELFUPDATE_${_hesu_var}})
		set(HE_SELFUPDATE_${_hesu_var} "$ENV{HE_SELFUPDATE_${_hesu_var}}")
	endif()
endforeach()

# PORT goes into the source as a bare number, so anything else would surface as
# a compile error in SelfUpdateEndpoint.cpp pointing at a line that looks fine.
if(NOT HE_SELFUPDATE_PORT MATCHES "^[0-9]+$" OR HE_SELFUPDATE_PORT LESS 1 OR HE_SELFUPDATE_PORT GREATER 65535)
	message(FATAL_ERROR "HE_SELFUPDATE_PORT must be a number from 1 to 65535, not '${HE_SELFUPDATE_PORT}'")
endif()

# Escaped once, here, because these end up inside a C string literal (a
# generated password may contain a backslash or a double quote).
foreach(_hesu_var HOST USER PASSWORD BASEPATH)
	set(_hesu_val "${HE_SELFUPDATE_${_hesu_var}}")
	string(REPLACE "\\" "\\\\" _hesu_val "${_hesu_val}")
	string(REPLACE "\"" "\\\"" _hesu_val "${_hesu_val}")
	set(_hesu_esc_${_hesu_var} "${_hesu_val}")
endforeach()
unset(_hesu_var)
unset(_hesu_val)

# PRIVATE: only SelfUpdateEndpoint.cpp reads them (builtInSelfUpdateEndpoint()).
# HorizonUpdater does not compile that file and gets none of this.
target_compile_definitions(HorizonSelfUpdate PRIVATE
	HE_SELFUPDATE_HOST="${_hesu_esc_HOST}"
	HE_SELFUPDATE_PORT=${HE_SELFUPDATE_PORT}
	HE_SELFUPDATE_USER="${_hesu_esc_USER}"
	HE_SELFUPDATE_PASSWORD="${_hesu_esc_PASSWORD}"
	HE_SELFUPDATE_BASEPATH="${_hesu_esc_BASEPATH}"
)
foreach(_hesu_var HOST USER PASSWORD BASEPATH)
	unset(_hesu_esc_${_hesu_var})
endforeach()
unset(_hesu_var)

# The only place the state is visible at build time — each way of being
# half-configured says which one it is. The password is never echoed.
if(HE_SELFUPDATE_HOST STREQUAL "" AND HE_SELFUPDATE_USER STREQUAL "" AND HE_SELFUPDATE_PASSWORD STREQUAL "")
	message(STATUS "HorizonSelfUpdate: no update account compiled in — Check for Updates needs HE_SELFUPDATE_* at run time")
	if(_hesu_local_credentials)
		message(WARNING "cmake/SelfUpdateCredentials.cmake exists but leaves HE_SELFUPDATE_HOST, _USER and _PASSWORD empty — no update account is compiled in.")
	else()
		message(STATUS "  → to compile one in: copy cmake/SelfUpdateCredentials.cmake.template to cmake/SelfUpdateCredentials.cmake and fill it in")
	endif()
elseif(HE_SELFUPDATE_HOST STREQUAL "" OR HE_SELFUPDATE_USER STREQUAL "" OR HE_SELFUPDATE_PASSWORD STREQUAL "")
	message(WARNING "HorizonSelfUpdate: the compiled-in update account is incomplete (HE_SELFUPDATE_HOST, _USER and _PASSWORD are all required) — the editor will report it as such and never connect.")
else()
	message(STATUS "HorizonSelfUpdate: update account ${HE_SELFUPDATE_USER}@${HE_SELFUPDATE_HOST}:${HE_SELFUPDATE_PORT}")
endif()
unset(_hesu_local_credentials)

he_deploy_dll(HorizonSelfUpdate)

# ─── HorizonUpdater ─────────────────────────────────────────────────────────────
# The separate, tiny program the takeover needs: the editor cannot replace its
# own files while it is running (the Windows loader holds every DLL and the
# exe open), so whatever does the swap has to run from somewhere else, as its
# own process. See SelfUpdateApplyPlan.h / SelfUpdateApplyCore.h / Takeover.cpp.
#
# HE_SU_STANDALONE (SuCommon.h) makes HE_SU_API a no-op here: this links no
# engine library at all, not even HorizonCore, because Takeover.cpp copies this
# executable alone to a temp directory and runs it from there — it must be able
# to start with nothing beside it but the OS and the C++ runtime.
add_executable(HorizonUpdater
	updater/main.cpp
	src/SelfUpdateProcess.cpp
	src/SelfUpdateApplyCore.cpp
)
target_include_directories(HorizonUpdater PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(HorizonUpdater PRIVATE cxx_std_20)
target_compile_definitions(HorizonUpdater PRIVATE HE_SU_STANDALONE)
target_link_libraries(HorizonUpdater PRIVATE nlohmann_json::nlohmann_json)

# Deployed beside HorizonEditor the same way HorizonSelfUpdate's own .dll/.so is
# — he_deploy_dll only copies $<TARGET_FILE:target> to DEPLOY_EDITOR, which
# works for an executable exactly as well as for a DLL. This subdirectory is
# added BEFORE src/HE_Editor (see the root CMakeLists.txt), so there is no
# HorizonEditor target yet to depend on here; src/HE_Editor/CMakeLists.txt adds
# `add_dependencies(HorizonEditor HorizonUpdater)` on its own side instead, so a
# stale HorizonUpdater is never what an editor Build picks up — the same lesson
# that applies to every DLL beside the editor's tool executables.
he_deploy_dll(HorizonUpdater)
