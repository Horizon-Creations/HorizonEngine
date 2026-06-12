#pragma once

#ifdef _WIN32
  #ifdef HE_CORE_BUILD_DLL
    #define HE_API __declspec(dllexport)
  #else
    #define HE_API __declspec(dllimport)
  #endif
#else
  #define HE_API __attribute__((visibility("default")))
#endif
