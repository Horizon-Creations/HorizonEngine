#pragma once
#include <cstdint>
#include <Types/Defines.h>
#include "HorizonCode.h"
#include <memory>
#include <string>
#include <vector>

// ── HorizonCode::CompiledInstance ────────────────────────────────────────────
// The compiled counterpart to Graph+Runner: one instance of a HorizonCode class
// that was ahead-of-time generated to C++ at export (see
// docs/horizoncode-cpp-codegen-implementation-plan.md). Compiled instances
// register in the SAME Runtime as interpreted ones — same InstanceId handles,
// same Context (services, delegation), so Refs, Bind/Emit, callExternal, GC and
// mixed compiled/interpreted populations work identically by construction.
//
// The Runtime reads a Graph in a handful of places (declared variables, Event
// nodes); CompiledInstance mirrors exactly those reads as static metadata
// (varInfos/eventInfos) plus reflection over the generated typed members
// (getVariable/setVariable/collectRefs/reseedVariables).

namespace HorizonCode {

// Static per-class metadata, backed by static tables in the generated class.
struct CompiledVarInfo
{
    const char* name;
    PinType     type;
    bool        isArray;
    int         access;   // 0 = public, 1 = private — mirrors Variable::access
};
struct CompiledEventInfo
{
    const char* name;     // one entry per Event node in the source graph
    int         elem;     // widget element filter (0 = any), mirrors Node::elem
};

class HE_API CompiledInstance
{
public:
    virtual ~CompiledInstance();

    // Everything below classKey has a default that says "this class has none of
    // that", so a generated class only spells out what its graph actually
    // contains — a graph with no functions carries no callFunction, one with no
    // variables no variable reflection. An empty answer here is already the
    // Runtime's "not mine" case, so the defaults are the same behaviour the
    // emitted-but-empty overrides had.

    // ── class metadata ───────────────────────────────────────────────────────
    virtual const char* classKey() const = 0;   // canonical registry key
    // Identity for the checked downcast generated code uses before calling
    // another compiled class directly (hc::as). A per-class address, compared by
    // pointer — NOT dynamic_cast: the generated library is built with hidden
    // visibility, and RTTI across that boundary is the kind of thing that breaks
    // on one platform and nowhere else. Default null = "not any generated class".
    virtual const void* classTag() const { return nullptr; }
    // The engine base class this one derives from (HorizonCode.h engineClasses):
    // "Entity", "PlayerCharacter", "PlayerController", … The Runtime needs it to
    // answer a Cast to a BASE class, which classTag cannot — that one is an
    // exact per-class address and knows nothing about a chain.
    //
    // The default is "" = Object, which is what a class without a taxonomy row
    // has always effectively been. Overriding it is therefore purely additive:
    // a hand-written CompiledInstance keeps behaving exactly as before.
    virtual const char* baseClassKey() const { return ""; }
    // The HorizonCode classes this one derives from, nearest first — the same
    // list ClassIdentity::chain carries, baked in by the generator. Needed for
    // the same reason baseClassKey is: a Cast to a PARENT class cannot be
    // answered by classTag, which is one exact address per class.
    virtual const std::vector<const char*>& classChain() const
    { static const std::vector<const char*> kNone; return kNone; }
    virtual const std::vector<CompiledVarInfo>& varInfos() const
    { static const std::vector<CompiledVarInfo> kNone; return kNone; }
    virtual const std::vector<CompiledEventInfo>& eventInfos() const
    { static const std::vector<CompiledEventInfo> kNone; return kNone; }

    // ── the engine's own events, as methods ─────────────────────────────────
    // These names are not data: each is a string LITERAL at exactly one place in
    // the engine (WidgetManager, GameInstanceHost, HorizonWorld, PlayerHost,
    // Runtime::destroy). A compiled class overrides the ones its graph handles,
    // and its header then shows at a glance which ones those are.
    //
    // The DEFAULT forwards to fireEvent under the canonical name, so overriding
    // is purely an optimization: a class that implements only fireEvent — a
    // hand-written one, or a generated one whose graph does not handle this
    // event — behaves exactly as it did before the hooks existed. Skipping the
    // name is the win; needing to know about it is not a condition.
    //
    // `elem` is the widget element the event happened on (0 = the widget
    // itself); a handler declared for element 0 answers for every element, which
    // is the same rule fireEvent applies. The typed argument is what the engine
    // sends, so nothing has to be boxed into a Value to get here.
    //
    // NOTE: Construct/Destruct are ordinary hooks, NOT the C++ constructor and
    // destructor. The Context is bound AFTER construction (Runtime::addCompiled)
    // and the instance is still registered while Destruct runs — a C++ ctor/dtor
    // sits outside both windows, and graph nodes that touch the world would
    // silently do nothing there.
    virtual void onConstruct() { fireEvent("Construct", 0, Value{}); }
    virtual void onDestruct()  { fireEvent("Destruct", 0, Value{}); }
    virtual void onTick(float dt) { fireEvent("Tick", 0, Value::ofFloat(dt)); }
    virtual void onBeginPlay() { fireEvent("BeginPlay", 0, Value{}); }
    // Widget pointer/focus events.
    virtual void onClicked(int elem)    { fireEvent("OnClicked", elem, Value{}); }
    virtual void onPressed(int elem)    { fireEvent("OnPressed", elem, Value{}); }
    virtual void onReleased(int elem)   { fireEvent("OnReleased", elem, Value{}); }
    virtual void onHovered(int elem)    { fireEvent("OnHovered", elem, Value{}); }
    virtual void onUnhovered(int elem)  { fireEvent("OnUnhovered", elem, Value{}); }
    virtual void onMouseEnter(int elem) { fireEvent("OnMouseEnter", elem, Value{}); }
    virtual void onMouseLeave(int elem) { fireEvent("OnMouseLeave", elem, Value{}); }
    virtual void onFocused(int elem)    { fireEvent("OnFocused", elem, Value{}); }
    virtual void onUnfocused(int elem)  { fireEvent("OnUnfocused", elem, Value{}); }
    // Widget value events — the argument is the new value.
    virtual void onTextChanged(int elem, const std::string& text)
    { fireEvent("OnTextChanged", elem, Value::ofString(text)); }
    virtual void onTextCommitted(int elem, const std::string& text)
    { fireEvent("OnTextCommitted", elem, Value::ofString(text)); }
    virtual void onValueChanged(int elem, float value)
    { fireEvent("OnValueChanged", elem, Value::ofFloat(value)); }
    virtual void onCheckChanged(int elem, bool checked)
    { fireEvent("OnCheckChanged", elem, Value::ofBool(checked)); }
    virtual void onSelectionChanged(int elem, int index)
    { fireEvent("OnSelectionChanged", elem, Value::ofInt(index)); }
    // List rows — the argument is the ITEM index in both cases.
    virtual void onRowBind(int elem, int index)
    { fireEvent("OnRowBind", elem, Value::ofInt(index)); }
    virtual void onRowActivated(int elem, int index)
    { fireEvent("OnRowActivated", elem, Value::ofInt(index)); }
    // GameInstance lifecycle.
    virtual void onInit()     { fireEvent("OnInit", 0, Value{}); }
    virtual void onShutdown() { fireEvent("OnShutdown", 0, Value{}); }
    virtual void onWindowFocusChanged(bool focused)
    { fireEvent("OnWindowFocusChanged", 0, Value::ofBool(focused)); }
    // Level script lifecycle.
    virtual void onLevelLoaded()   { fireEvent("OnLevelLoaded", 0, Value{}); }
    virtual void onLevelUnloaded() { fireEvent("OnLevelUnloaded", 0, Value{}); }
    // Physics contacts on an Entity class; `other` is the other entity's id.
    virtual void onBeginOverlap(int other) { fireEvent("OnBeginOverlap", 0, Value::ofInt(other)); }
    virtual void onEndOverlap(int other)   { fireEvent("OnEndOverlap",   0, Value::ofInt(other)); }
    virtual void onHit(int other)          { fireEvent("OnHit",          0, Value::ofInt(other)); }
    virtual void onHitEnd(int other)       { fireEvent("OnHitEnd",       0, Value::ofInt(other)); }

    // ── execution (mirrors Runner's entry points) ───────────────────────────
    virtual void fireEvent(const std::string& name, int elem, const Value& arg)
    { (void)name; (void)elem; (void)arg; }
    virtual bool callFunction(const std::string& name, bool requirePublic,
                              const std::vector<Value>& args,
                              std::vector<Value>* results)
    { (void)name; (void)requirePublic; (void)args; (void)results; return false; }
    // Resume the exec chain after a Delay node (Runner::resumeFrom's mirror,
    // called by the Runtime when the timer expires). Default no-op — only
    // classes containing Delay nodes override it.
    virtual void resumeFrom(int nodeId) { (void)nodeId; }

    // ── variable reflection (Get/SetExternal, GC, reseed, tooling) ──────────
    // getVariable is only meaningful for declared names (see varInfos);
    // setVariable returns false for an unknown name so the Runtime can route it
    // to its per-instance overflow store (undeclared-Set semantics).
    virtual Value getVariable(const std::string& name) const { (void)name; return Value{}; }
    virtual bool  setVariable(const std::string& name, const Value& v)
    { (void)name; (void)v; return false; }
    virtual void  reseedVariables() {}   // back to declared defaults
    // Every live reference held in Ref-typed members (incl. Ref arrays) — the
    // compiled equivalent of the GC's var-store scan.
    virtual void  collectRefs(std::vector<uint32_t>& out) const { (void)out; }

    // ── wiring (Runtime calls this right after registration) ────────────────
    void bindContext(Context ctx) { m_ctx = std::move(ctx); }

protected:
    Context m_ctx;   // the SAME Context the interpreter gets (makeContext(id))
};

// Ownership across the generated dylib's C ABI: instances are created and
// destroyed by the factory pair in their CompiledClassEntry, so the deleter is
// carried with the pointer. Default-constructed (null fn) deletes nothing.
struct CompiledDeleter
{
    void (*fn)(CompiledInstance*) = nullptr;
    void operator()(CompiledInstance* p) const { if (p && fn) fn(p); }
};
using CompiledPtr = std::unique_ptr<CompiledInstance, CompiledDeleter>;

// In-process construction (tests, hand-written instances): plain delete.
template <typename T, typename... Args>
CompiledPtr makeCompiled(Args&&... args)
{
    return CompiledPtr(new T(std::forward<Args>(args)...),
                       CompiledDeleter{ [](CompiledInstance* p) { delete p; } });
}

// One row of the manifest the generated dylib exports (same pattern as
// IGameLogic's C factory exports):
//
//   extern "C" HE_GAME_API const HorizonCode::CompiledClassEntry*
//       HE_HorizonCodeGenClasses(int* count, const char** engineVersion);
struct CompiledClassEntry
{
    const char*        key;   // canonical class key (asset path / "level:…" / "__game_instance__")
    CompiledInstance* (*create)();
    void              (*destroy)(CompiledInstance*);
};

} // namespace HorizonCode
