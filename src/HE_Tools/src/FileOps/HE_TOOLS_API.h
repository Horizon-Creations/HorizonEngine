#pragma once

#ifdef _WIN32
  #ifdef HE_TOOLS_BUILD_DLL
	#define HE_TOOLS_API __declspec(dllexport)
  #else
	#define HE_TOOLS_API __declspec(dllimport)
  #endif
#else
  #define HE_TOOLS_API __attribute__((visibility("default")))
#endif
