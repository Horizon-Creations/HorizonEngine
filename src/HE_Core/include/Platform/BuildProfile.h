#pragma once

// ─── Build profile: dev | release, and the developer feature switches ────────
// Whether developer features EXIST in a binary is decided by the compiler, not
// at runtime: a release game must not contain the F9 capture or the headless
// hooks at all, and a runtime flag can only hide code, never remove it.
//
// The definitions come from CMake: he_target_build_profile() in
// cmake/HorizonBuildProfile.cmake, tree-wide choice HE_BUILD_PROFILE=dev|release.
// Rules and names: docs/coding-conventions.md section 9. In short:
//
//   #if HE_BUILD_PROFILE_DEV ... #endif        a block that only dev builds have
//   #if HE_FEATURE_TEST_HOOKS ... #endif       a block behind one feature switch
//   HE_DEV_ONLY(statement;)                    one line that only dev builds have
//
// Always `#if`, never `#ifdef`: every macro here is defined, as 0 or 1, so a
// misspelt name is a -Wundef warning instead of a silently missing feature.
//
// Macros only, no engine includes: tests compile this header under several
// profiles in one binary (tests/build_profile_probe.cpp), and step 5's SDK
// hands it to game code as is.

// ── Profile ────────────────────────────────────────────────────────────────
// A target CMake never gave a profile is a dev build. That is what every tree
// built before the profile existed was, and it keeps all existing code (the
// editor, he_tests, the prebuilt runtimes) compiling exactly as before.
// HE_BUILD_PROFILE_DEFAULTED says that it happened, so a release pipeline can
// assert it did not.
#if !defined(HE_BUILD_PROFILE_DEV) && !defined(HE_BUILD_PROFILE_RELEASE)
#  define HE_BUILD_PROFILE_DEV 1
#  define HE_BUILD_PROFILE_RELEASE 0
#  define HE_BUILD_PROFILE_DEFAULTED 1
#elif !defined(HE_BUILD_PROFILE_DEV)
#  if HE_BUILD_PROFILE_RELEASE
#    define HE_BUILD_PROFILE_DEV 0
#  else
#    define HE_BUILD_PROFILE_DEV 1
#  endif
#elif !defined(HE_BUILD_PROFILE_RELEASE)
#  if HE_BUILD_PROFILE_DEV
#    define HE_BUILD_PROFILE_RELEASE 0
#  else
#    define HE_BUILD_PROFILE_RELEASE 1
#  endif
#endif
#ifndef HE_BUILD_PROFILE_DEFAULTED
#  define HE_BUILD_PROFILE_DEFAULTED 0
#endif

#if (HE_BUILD_PROFILE_DEV) + (HE_BUILD_PROFILE_RELEASE) != 1
#  error "Exactly one of HE_BUILD_PROFILE_DEV and HE_BUILD_PROFILE_RELEASE must be 1"
#endif

// ── Feature switches ───────────────────────────────────────────────────────
// Each one defaults to "on in dev, off in release". This is the only place
// those defaults live; he_target_build_profile(... FEATURES NAME=0|1) can
// override one per target (a release build for CI that keeps the test hooks,
// say). A new switch goes here AND into HE_BUILD_FEATURES in the CMake module,
// which rejects names it does not know.

// The game's V (VSync) and F10 (stats card) keys.
#ifndef HE_FEATURE_DEV_HOTKEYS
#  define HE_FEATURE_DEV_HOTKEYS HE_BUILD_PROFILE_DEV
#endif
// The F9 profiler capture and the HE_PROFILE_* / HE_CAPTURE_* / HE_PERF_*
// environment automation around it.
#ifndef HE_FEATURE_PROFILER_CAPTURE
#  define HE_FEATURE_PROFILER_CAPTURE HE_BUILD_PROFILE_DEV
#endif
// Headless and image-test hooks: HE_HIDDEN_WINDOW, HE_EXIT_AFTER_FRAMES,
// HE_DUMP_*, HE_BACKGROUND_FPS, HE_NO_SPLASH, HE_SPLASH_DUMP.
#ifndef HE_FEATURE_TEST_HOOKS
#  define HE_FEATURE_TEST_HOOKS HE_BUILD_PROFILE_DEV
#endif
// Environment variables that override shipped settings: HE_CONFIG_DIR, HE_LOG,
// the physics tuning (HE_PHYSICS_*, HE_PROPAGATE_FULL).
#ifndef HE_FEATURE_ENV_OVERRIDES
#  define HE_FEATURE_ENV_OVERRIDES HE_BUILD_PROFILE_DEV
#endif

// ── One-line helpers ───────────────────────────────────────────────────────
// For a statement or an argument that is not worth an #if block. Commas inside
// are fine (variadic). Anything longer than a line belongs in `#if`.
#if HE_BUILD_PROFILE_DEV
#  define HE_DEV_ONLY(...) __VA_ARGS__
#  define HE_RELEASE_ONLY(...)
#else
#  define HE_DEV_ONLY(...)
#  define HE_RELEASE_ONLY(...) __VA_ARGS__
#endif

#ifdef __cplusplus
namespace HE {

// For values, such as the profile in the start-up log. Not for removing code:
// outside a template the branch `if constexpr` discards must still compile, with
// every name it uses declared, so code that should not exist in release (and
// the headers it needs) goes behind `#if`.
//
// Deliberately `constexpr` without `inline`: internal linkage, so translation
// units compiled under different profiles (the test probes) never define the
// same entity twice with different values.
constexpr bool kDevBuild = HE_BUILD_PROFILE_DEV != 0;
constexpr bool kReleaseBuild = HE_BUILD_PROFILE_RELEASE != 0;
constexpr const char* kBuildProfileName = HE_BUILD_PROFILE_DEV ? "dev" : "release";

} // namespace HE
#endif
