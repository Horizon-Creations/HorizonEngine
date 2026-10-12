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
# account is a SECOND, separate one: it is read from HE_SELFUPDATE_* in the
# process environment at run time and is never the EngineContent account — see
# SelfUpdateEndpoint.h. Nothing is baked into the binary, so this CMake file has
# none of the credential plumbing HorizonContentSync needs.
#
# Added from the root CMakeLists inside `if(HE_HAVE_LIBSSH2)`, right after
# HorizonContentSync, because it needs that target.
add_library(HorizonSelfUpdate SHARED
	src/SelfUpdateVersion.cpp
	src/SelfUpdateManifest.cpp
	src/SelfUpdateEndpoint.cpp
	src/SelfUpdateTransport.cpp
	src/SelfUpdateCheck.cpp
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

he_deploy_dll(HorizonSelfUpdate)
