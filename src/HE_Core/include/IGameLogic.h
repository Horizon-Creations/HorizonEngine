#pragma once
#include <cstdint>

class HorizonWorld;   // forward — GameLogic gets the world injected

// Export decoration for the game DLL's factory functions. extern "C" alone does
// NOT export a symbol from a Windows DLL — without __declspec(dllexport) the
// GameLogicLoader's GetProcAddress finds nothing and load() fails. No-op elsewhere.
#ifdef _WIN32
#  define HE_GAME_API __declspec(dllexport)
#else
#  define HE_GAME_API
#endif

// Every game DLL must export a C-compatible factory function:
//   extern "C" HE_GAME_API IGameLogic* HE_CreateGameLogic();
//   extern "C" HE_GAME_API void        HE_DestroyGameLogic(IGameLogic*);
//
// Using C linkage prevents name-mangling issues across DLL boundaries.
class IGameLogic {
public:
    virtual ~IGameLogic() = default;

    // Called once after the DLL is loaded and the world is ready.
    virtual void onStart(HorizonWorld& world) = 0;

    // Called every frame. deltaTime is in seconds.
    virtual void onUpdate(HorizonWorld& world, float deltaTime) = 0;

    // Called once before the DLL is unloaded (hot-reload or shutdown).
    virtual void onStop(HorizonWorld& world) = 0;
};

// Typedefs for the DLL export function pointers
using FnCreateGameLogic  = IGameLogic*(*)();
using FnDestroyGameLogic = void(*)(IGameLogic*);
