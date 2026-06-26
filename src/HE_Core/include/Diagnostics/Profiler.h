#pragma once

// HorizonEngine profiling macros.
//
//  HE_PROFILE_FRAME()       — mark end of a rendered frame (Tracy FrameMark only)
//  HE_PROFILE_SCOPE()       — auto-named scope timer for the enclosing function
//  HE_PROFILE_SCOPE_N(name) — named scope timer (string literal)
//
// HE_PROFILE_SCOPE/SCOPE_N drive BOTH:
//   • the built-in runtime EngineProfiler (always compiled in; zero cost unless a
//     capture is recording — one bool check), and
//   • Tracy zones when built with  cmake -DHE_ENABLE_TRACY=ON.
// The frame boundary (begin/endFrame) is driven explicitly from Application::Run,
// so HE_PROFILE_FRAME stays Tracy-only.

// HE_PROFILING_ENABLED gates the in-engine EngineProfiler scopes at COMPILE time.
// It is defined for dev/editor builds (CMake option HE_PROFILING, default ON) and
// left undefined for a shipping game build → the scope macros become true no-ops
// (not even the recording bool check), so the shipped game carries ZERO profiler
// overhead. Tracy zones are orthogonal (TRACY_ENABLE).
#include "Diagnostics/EngineProfiler.h"

#define HE_PROF_CONCAT_(a, b) a##b
#define HE_PROF_CONCAT(a, b)  HE_PROF_CONCAT_(a, b)

#if defined(HE_PROFILING_ENABLED)
#  define HE_PROF_SCOPE_IMPL(name)   ::ProfileScope HE_PROF_CONCAT(_heProf_, __LINE__){name}
#else
#  define HE_PROF_SCOPE_IMPL(name)   ((void)0)   // compiled out — zero overhead
#endif

#ifdef TRACY_ENABLE
#  include <tracy/Tracy.hpp>
#  define HE_PROFILE_FRAME()         FrameMark
#  define HE_PROFILE_SCOPE()         ZoneScoped;            HE_PROF_SCOPE_IMPL(__FUNCTION__)
#  define HE_PROFILE_SCOPE_N(name)   ZoneScopedN(name);     HE_PROF_SCOPE_IMPL(name)
#else
#  define HE_PROFILE_FRAME()
#  define HE_PROFILE_SCOPE()         HE_PROF_SCOPE_IMPL(__FUNCTION__)
#  define HE_PROFILE_SCOPE_N(name)   HE_PROF_SCOPE_IMPL(name)
#endif
