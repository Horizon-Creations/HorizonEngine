#pragma once
#include "Types/Defines.h"
#include "Scripting/ScriptTypes.h"
#include "HorizonCode/HorizonCode.h"   // HorizonCode::Value, for callOnRep
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Language-agnostic per-entity gameplay-script backend (Lua, Python, …).
//
// One backend embeds one language runtime. Scripts are named source-text units
// (ScriptAsset.sourceCode); instances bind a script to an entity and receive the
// lifecycle callbacks below. All methods are main-thread only.
//
// Implementations: ScriptEngine (Lua, HE_Core) and PyScriptBackend (CPython,
// shipped as the runtime-loaded HorizonPython plugin). ScriptContext in
// HE_Scene hosts the backends and routes calls per script language, tagging the
// language into the high byte of the public InstanceId (Lua == 0, so Lua-only
// ids are unchanged).
//
// This interface is also the plugin ABI: the CPython backend crosses a module
// boundary as an IScriptBackend*, so anything a host needs to call on it has to
// be declared here rather than on the concrete class.
class PhysicsWorld;
class ContentManager;

class HE_API IScriptBackend
{
public:
    using InstanceId = uint64_t;
    static constexpr InstanceId kInvalidInstance = 0;

    virtual ~IScriptBackend() = default;

    // Compile + store a named script from source. False on compile error
    // (see lastError()).
    virtual bool loadScript(const std::string& name, const std::string& source) = 0;
    // Remove a script and destroy all instances created from it.
    virtual void unloadScript(const std::string& name) = 0;
    virtual bool   isScriptLoaded(const std::string& name) const = 0;
    virtual size_t loadedScriptCount() const = 0;
    virtual size_t instanceCount() const = 0;

    // Create an instance of a named script bound to an entity (the script sees
    // the id, e.g. self.entityId in Lua / self.entity_id in Python).
    virtual InstanceId createInstance(const std::string& scriptName, uint32_t entityId) = 0;
    virtual void       destroyInstance(InstanceId id) = 0;

    // Lifecycle callbacks — no-ops (returning true) when the script does not
    // define the handler; false + lastError() on script runtime errors.
    virtual bool callOnStart(InstanceId id) = 0;
    virtual bool callOnUpdate(InstanceId id, float dt) = 0;
    virtual bool callOnCollisionEnter(InstanceId id, uint32_t otherEntityId) = 0;
    virtual bool callOnCollisionExit(InstanceId id, uint32_t otherEntityId) = 0;
    // Trigger contacts (ColliderComponent::isTrigger on either side), the pair
    // HorizonCode surfaces as OnBeginOverlap / OnEndOverlap. NOT pure: a backend
    // that predates them keeps compiling and simply never delivers one, which is
    // the same "script does not define the handler" answer as above.
    //
    // The blocking pair above still fires for BOTH kinds of contact, so no
    // existing script loses an event by this being added.
    virtual bool callOnBeginOverlap(InstanceId id, uint32_t otherEntityId)
    { (void)id; (void)otherEntityId; return true; }
    virtual bool callOnEndOverlap(InstanceId id, uint32_t otherEntityId)
    { (void)id; (void)otherEntityId; return true; }
    // Animation notifies: an event the artist put on a clip's timeline, arriving
    // as the playhead sweeps over it. The name is the whole payload. Begin/End
    // are the two edges of a notify STATE (a hit window, an invulnerability);
    // the plain one is a point event (a footstep, a sound).
    //
    // Defaulted like the overlap pair above, and for the same reason: a backend
    // written before these existed keeps compiling and simply never delivers one,
    // which is indistinguishable from a script that defines no handler.
    virtual bool callOnAnimationNotify(InstanceId id, const std::string& name)
    { (void)id; (void)name; return true; }
    virtual bool callOnAnimationNotifyBegin(InstanceId id, const std::string& name)
    { (void)id; (void)name; return true; }
    virtual bool callOnAnimationNotifyEnd(InstanceId id, const std::string& name)
    { (void)id; (void)name; return true; }
    // UI pointer event on the instance's own entity (click / hover enter/exit).
    virtual bool callOnUIEvent(InstanceId id, UIScriptEvent ev) = 0;
    // Input actions: the events a PlayerController graph receives as
    // Input.<Action>.Pressed / .Released / .Axis / .Axis2D, delivered to every
    // text-script instance as well (PlayerHost pumps both). `action` is the
    // logical action name (the InputAction asset's stem). The two axis forms
    // fire once per frame while the session runs, the button pair on the edge.
    //
    // Defaulted like the pairs above, for the same reason: a backend built
    // before these existed keeps compiling and simply delivers none of them.
    virtual bool callOnInputPressed(InstanceId id, const std::string& action)
    { (void)id; (void)action; return true; }
    virtual bool callOnInputReleased(InstanceId id, const std::string& action)
    { (void)id; (void)action; return true; }
    virtual bool callOnInputAxis(InstanceId id, const std::string& action, float value)
    { (void)id; (void)action; (void)value; return true; }
    virtual bool callOnInputAxis2D(InstanceId id, const std::string& action, float x, float y)
    { (void)id; (void)action; (void)x; (void)y; return true; }
    // A timer started with horizon.timer.after / .every came due. Every
    // instance hears every timer — the handle is how a script tells its own
    // from the rest, exactly as a HorizonCode graph does with OnTimer.
    virtual bool callOnTimer(InstanceId id, int handle)
    { (void)id; (void)handle; return true; }
    // The host's anti-cheat made a report, or — on a client — the host sent a
    // notice about this player (docs/anti-cheat-plan.md §5.4). `reportId` is
    // the ticket the horizon.anticheat.report* readers take. Every instance
    // hears every report, like a timer; the readers say whom it concerns.
    //
    // Defaulted like callOnTimer, for the same reason: a backend from before
    // this existed keeps compiling and simply never delivers one.
    virtual bool callOnCheatDetected(InstanceId id, int reportId)
    { (void)id; (void)reportId; return true; }

    // The multiplayer session's lifecycle (docs/gameplay-replication-plan.md
    // §7.4): onPlayerJoined / onPlayerLeft / onConnected / onDisconnected /
    // onSessionStarted / onSessionEnded, and their snake_case twins in Python.
    // Every instance of the session hears every one, like a timer; `arg` is the
    // PlayerId, the disconnect reason, or 0 (see NetScriptEvent).
    //
    // Defaulted like callOnCheatDetected, for the same reason: a backend from
    // before this existed keeps compiling and simply never delivers one.
    virtual bool callOnNetEvent(InstanceId id, NetScriptEvent ev, int arg)
    { (void)id; (void)ev; (void)arg; return true; }

    // A replicated variable this entity owns arrived from the authority
    // (docs/gameplay-replication-plan.md §6.4): onRep_<name>(self, old) in Lua,
    // on_rep_<name>(self, old) in Python. `old` is the value this machine held
    // before; the NEW one is already in the variable and is read with
    // horizon.net.getVar*.
    //
    // Unlike the six lifecycle events above, this goes to the ONE instance on
    // the entity whose property changed, not to every instance of the session:
    // a property belongs to an entity, and telling every script in the game
    // that somebody else's door opened would be noise nobody asked for.
    //
    // A HorizonCode::Value rather than the four-type ScriptPropValue, because a
    // replicated variable may be a struct, an enum or a map, and a callback that
    // silently dropped those would be worse than none. The Lua side does NOT go
    // through here — ScriptContext pushes the value itself with the marshaller
    // it already owns (ScriptEngine::callInstanceMethod) — so this is the
    // Python/plugin path, where the ABI boundary is real.
    //
    // Defaulted like callOnNetEvent, for the same reason.
    virtual bool callOnRep(InstanceId id, const std::string& varName,
                           const HorizonCode::Value& oldValue)
    { (void)id; (void)varName; (void)oldValue; return true; }

    // A remote call arrived for this instance (plan §7.2): run the method of
    // that name with `args`. The name is used VERBATIM — a graph's `Open` is a
    // script's `Open`, because the two sides have to agree on one spelling and
    // the one the caller wrote is the only one both know.
    //
    // THE RETURN VALUE MEANS SOMETHING ELSE HERE than in every hook above.
    // False is not a failure: it is "this instance has no such method", and
    // the router then asks the next frontend (NetEvents::dispatchRpc). A hook
    // answers true for a missing method because nothing went wrong; a remote
    // call has to know, or a Lua script would swallow a call meant for the
    // native module.
    //
    // Defaulted to false, so a backend that predates RPC simply never claims a
    // call — which is the truth about it.
    virtual bool callRpc(InstanceId id, const std::string& fn,
                         const std::vector<HorizonCode::Value>& args)
    { (void)id; (void)fn; (void)args; return false; }

    // Declared properties of a loaded script (editor inspector surface) and
    // per-instance override injection (before callOnStart).
    virtual std::vector<ScriptPropDef> getScriptProperties(const std::string& name) const = 0;
    virtual void injectProperties(InstanceId id,
                                  const std::unordered_map<std::string, ScriptPropValue>& props) = 0;

    // Recompile a loaded script and patch behaviour of live instances while
    // preserving their data fields. False (state unchanged) on compile error.
    virtual bool hotReloadScript(const std::string& name, const std::string& source) = 0;

    virtual const std::string& lastError() const = 0;

    // ── Optional wiring ──────────────────────────────────────────────────────
    // Defaulted to nothing, because only some backends want them: Lua reaches
    // physics and content through the shared ScriptApi, the CPython backend
    // holds its own pointers. They sit here rather than on the concrete class
    // so a host can own that backend purely as an IScriptBackend* — which is
    // exactly what lets it be loaded at runtime instead of linked.
    virtual void setPhysicsWorld(PhysicsWorld* pw)     { (void)pw; }
    virtual void setContentManager(ContentManager* cm) { (void)cm; }
};
