#pragma once

// ─── HorizonContentSync — module export macro ─────────────────────────────────
// Same discipline as HorizonNet's HE_NET_API, HorizonCore's HE_API and
// HorizonSourceControl's HE_SC_API: explicit dllexport while building,
// dllimport for consumers.
//
// The module is editor-only. It is deliberately NOT copied into the packaged
// game: a player's binary has no business carrying an SSH client.
#ifdef _WIN32
  #ifdef HE_CS_BUILD_DLL
    #define HE_CS_API __declspec(dllexport)
  #else
    #define HE_CS_API __declspec(dllimport)
  #endif
#else
  #define HE_CS_API __attribute__((visibility("default")))
#endif
