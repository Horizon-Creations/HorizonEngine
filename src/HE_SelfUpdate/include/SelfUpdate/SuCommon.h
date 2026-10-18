#pragma once

// ─── HorizonSelfUpdate — module export macro ──────────────────────────────────
// Same discipline as HorizonContentSync's HE_CS_API and HorizonNet's HE_NET_API:
// explicit dllexport while building, dllimport for consumers.
//
// The module is editor-only. It is deliberately NOT copied into the packaged
// game: a player's binary has no business carrying an SSH client or a way to
// replace itself.
//
// HE_SU_STANDALONE: a few source files are ALSO compiled straight into the
// HorizonUpdater executable (SelfUpdateProcess.cpp and the updater's own core),
// which links no engine library at all — there is no DLL boundary for them to
// cross, so the macro is empty there.
#if defined(HE_SU_STANDALONE)
  #define HE_SU_API
#elif defined(_WIN32)
  #ifdef HE_SU_BUILD_DLL
    #define HE_SU_API __declspec(dllexport)
  #else
    #define HE_SU_API __declspec(dllimport)
  #endif
#else
  #define HE_SU_API __attribute__((visibility("default")))
#endif
