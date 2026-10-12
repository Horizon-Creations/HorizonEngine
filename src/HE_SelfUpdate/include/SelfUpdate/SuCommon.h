#pragma once

// ─── HorizonSelfUpdate — module export macro ──────────────────────────────────
// Same discipline as HorizonContentSync's HE_CS_API and HorizonNet's HE_NET_API:
// explicit dllexport while building, dllimport for consumers.
//
// The module is editor-only. It is deliberately NOT copied into the packaged
// game: a player's binary has no business carrying an SSH client or a way to
// replace itself.
#ifdef _WIN32
  #ifdef HE_SU_BUILD_DLL
    #define HE_SU_API __declspec(dllexport)
  #else
    #define HE_SU_API __declspec(dllimport)
  #endif
#else
  #define HE_SU_API __attribute__((visibility("default")))
#endif
