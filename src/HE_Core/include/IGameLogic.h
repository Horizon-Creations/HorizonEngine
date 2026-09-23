#pragma once
#include <cstdint>

class HorizonWorld;   // forward — GameLogic gets the world injected

// Export decoration for the game DLL's factory functions. extern "C" alone does
// NOT export a symbol from a Windows DLL — without __declspec(dllexport) the
// GameLogicLoader's GetProcAddress finds nothing and load() fails.
//
// Elsewhere this is normally redundant, because a shared library exports
// everything by default — but only until someone builds their GameLogic with
// -fvisibility=hidden (CMake's CXX_VISIBILITY_PRESET hidden), which is common
// enough advice that it must not silently hide the two symbols the loader looks
// for. Spelling the visibility out makes the decoration mean the same thing on
// every platform: this one is exported, whatever the default is.
#ifdef _WIN32
#  define HE_GAME_API __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#  define HE_GAME_API __attribute__((visibility("default")))
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

    // The host's anti-cheat made a report about a connection, or — on a client
    // — the host sent a notice about this player (docs/anti-cheat-plan.md §5.4).
    // `reportId` is a ticket; he::anticheat::reportLevel/Rule/Player/… in
    // <HorizonGameServices.h> open it, and he::anticheat::respond(reportId, …)
    // inside THIS call replaces what the host would otherwise do at the end of
    // the frame. Defaulted to nothing, so a module from before it existed keeps
    // building and simply lets the host's policy stand.
    virtual void onCheatDetected(int reportId) { (void)reportId; }

    // The multiplayer session's lifecycle (docs/gameplay-replication-plan.md
    // §7.4), in the plan's order. `player` is the PlayerId; `reason` is the
    // disconnect reason (0 Leave, 1 Timeout, 2 Kicked, 3 Rejected,
    // 4 VersionMismatch, 5 WrongProject). The player pair fires on the HOST,
    // the connect pair on the CLIENT, and a kick arrives as onCheatDetected
    // first and onDisconnected(2) after.
    //
    // Appended at the END and defaulted to nothing, like onCheatDetected: a
    // module built before these existed keeps loading and simply never hears
    // one. New hooks go after these, for the same reason.
    virtual void onPlayerJoined(int player) { (void)player; }
    virtual void onPlayerLeft(int player)   { (void)player; }
    virtual void onConnected()              {}
    virtual void onDisconnected(int reason) { (void)reason; }
    virtual void onSessionStarted()         {}
    virtual void onSessionEnded()           {}
};

// Typedefs for the DLL export function pointers
using FnCreateGameLogic  = IGameLogic*(*)();
using FnDestroyGameLogic = void(*)(IGameLogic*);
