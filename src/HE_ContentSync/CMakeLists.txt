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

he_deploy_dll(HorizonContentSync)
