#pragma once

// HorizonEngine profiling macros — zero cost when Tracy is disabled (default).
// Enable with:  cmake -DHE_ENABLE_TRACY=ON
//
//  HE_PROFILE_FRAME()       — mark end of a rendered frame (FrameMark)
//  HE_PROFILE_SCOPE()       — auto-named zone for the enclosing scope
//  HE_PROFILE_SCOPE_N(name) — named zone (string literal)

#ifdef TRACY_ENABLE
#  include <tracy/Tracy.hpp>
#  define HE_PROFILE_FRAME()         FrameMark
#  define HE_PROFILE_SCOPE()         ZoneScoped
#  define HE_PROFILE_SCOPE_N(name)   ZoneScopedN(name)
#else
#  define HE_PROFILE_FRAME()
#  define HE_PROFILE_SCOPE()
#  define HE_PROFILE_SCOPE_N(name)
#endif
