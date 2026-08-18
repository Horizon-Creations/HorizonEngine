#pragma once
#include "Scripting/IScriptBackend.h"

class HorizonWorld;

// ── ABI of the runtime-loaded CPython backend ────────────────────────────────
// The Python backend used to be compiled into HorizonScene, which linked
// libpython. That made the interpreter a LOAD-TIME dependency of the scene
// library: every shipped game had to carry libpython (5-9 MB depending on the
// platform) and would not even start without it, whether or not a single line
// of Python existed in the project.
//
// It is a plugin now. HorizonScene loads it on demand and talks to it purely
// through IScriptBackend, so nothing but this file knows the two are related.
//
// The contract is deliberately two C functions and nothing else:
//
//   * C linkage, so the symbol names are stable and dlsym/GetProcAddress can
//     find them without name mangling entering the picture.
//   * A matching destroy function rather than `delete` on the caller's side.
//     The object is allocated by the plugin's allocator and, on Windows, the
//     plugin can be linked against a different CRT than its host — freeing it
//     across that line is undefined. Whoever made it, unmakes it.
//   * Everything else travels over the IScriptBackend vtable, which is why the
//     two optional setters live on that interface rather than on the concrete
//     class (see IScriptBackend).
//
// Keep the symbol names and signatures in sync with the definitions at the
// bottom of HE_Python/src/PyScriptBackend.cpp.

extern "C"
{
	// Returns nullptr if the interpreter could not be brought up.
	using HePythonCreateFn  = IScriptBackend* (*)(HorizonWorld*);
	using HePythonDestroyFn = void (*)(IScriptBackend*);
}

namespace HE::PythonPlugin
{
	inline constexpr const char* kCreateSymbol  = "heCreatePythonBackend";
	inline constexpr const char* kDestroySymbol = "heDestroyPythonBackend";

	// The plugin sits next to the executable, like the other engine libraries.
	inline const char* fileName()
	{
#if defined(_WIN32)
		return "HorizonPython.dll";
#elif defined(__APPLE__)
		return "libHorizonPython.dylib";
#else
		return "libHorizonPython.so";
#endif
	}
}
