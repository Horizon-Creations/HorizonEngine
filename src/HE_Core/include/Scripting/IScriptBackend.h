#pragma once
#include "Types/Defines.h"
#include "Scripting/ScriptTypes.h"
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
    // UI pointer event on the instance's own entity (click / hover enter/exit).
    virtual bool callOnUIEvent(InstanceId id, UIScriptEvent ev) = 0;

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
