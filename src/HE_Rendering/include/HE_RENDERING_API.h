#pragma once

#ifdef _WIN32
#ifdef HE_RENDERING_BUILD_DLL
#define HE_RENDERING_API __declspec(dllexport)
#else
#define HE_RENDERING_API __declspec(dllimport)
#endif
#else
#define HE_RENDERING_API
#endif
