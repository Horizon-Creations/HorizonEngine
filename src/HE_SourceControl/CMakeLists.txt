# ─── HorizonSourceControl ─────────────────────────────────────────────────────
# Native Git integration for the editor: repository status, staging, commits,
# push/pull, LFS setup and the provider specifics (token shape, size limits).
#
# Editor-only, on purpose. It is NOT added to src/HE_Game's deploy list — a
# player's binary has no business carrying a git client and three provider REST
# clients — and not to src/HE_Tools' hc_codegen copy list either.
#
# Git operations shell out to the `git` CLI through HE::Proc rather than linking
# libgit2: libgit2 has no LFS, no credential helpers and no partial clone, and
# LFS is the entire reason this module exists (see docs/source-control-design.md).
add_library(HorizonSourceControl SHARED
	src/GitProbe.cpp
	src/StatusParser.cpp
	src/GitCli.cpp
	src/GitService.cpp
)

# HorizonCore for HE::Proc (the subprocess runner) and the engine log.
target_link_libraries(HorizonSourceControl PRIVATE HorizonCore)

target_include_directories(HorizonSourceControl
	PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(HorizonSourceControl PUBLIC cxx_std_20)

# Explicit export macro (HE_SC_API), same discipline as HorizonCore and
# HorizonNet — keeps the ABI surface deliberate and satisfies the Windows-CI
# portability rules.
target_compile_definitions(HorizonSourceControl
	PRIVATE HE_SC_BUILD_DLL
	PUBLIC  HE_SC_DLL
)

he_deploy_dll(HorizonSourceControl)
