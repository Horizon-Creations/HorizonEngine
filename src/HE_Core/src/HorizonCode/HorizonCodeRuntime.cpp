#include "HorizonCode/HorizonCodeRuntime.h"
#include <cstdint>
#include <Diagnostics/Logger.h>
#include <algorithm>
#include <unordered_set>
#include <vector>

namespace HorizonCode {

namespace {
// A HorizonCode runtime error (null reference, missing member, …). Logged at
// Error so it surfaces in the game log AND the editor's post-PIE report — the
// equivalent of Unreal's "Accessed None". Called only while a graph is executing
// (PIE / the shipped game), never at edit time.
void hcError(const std::string& msg)   { HE_LOG_ERROR(HorizonCode, "%s",   ("HorizonCode: " + msg).c_str()); }
void hcWarn (const std::string& msg)   { HE_LOG_WARN(HorizonCode, "%s", ("HorizonCode: " + msg).c_str()); }

// The compiled counterpart to Graph::findVariable — locals never appear in
// varInfos, so (unlike the interpreted path) no scope check is needed here.
const CompiledVarInfo* findVarInfo(const CompiledInstance& c, const std::string& name)
{
    for (const auto& vi : c.varInfos())
        if (name == vi.name) return &vi;
    return nullptr;
}

// A variable's declaration anywhere in the instance's chain, searched LEAF
// FIRST: a derived declaration is the one that answers, and only when nothing
// derived declares it does a base class's reach the caller. Null = no level
// declares it.
const Variable* findVarInLevels(const std::vector<Graph>& levels, const std::string& name)
{
    for (auto lv = levels.rbegin(); lv != levels.rend(); ++lv)
        if (const Variable* v = lv->findVariable(name)) return v;
    return nullptr;
}
}

void Runtime::purgeDoomed()
{
    for (const InstanceId id : m_doomed) m_insts.erase(id);
    m_doomed.clear();
}

Runtime::Inst* Runtime::find(InstanceId id)
{
    if (m_doomed.count(id)) return nullptr;      // logically gone, body pending free
    auto it = m_insts.find(id);
    return it != m_insts.end() ? &it->second : nullptr;
}
const Runtime::Inst* Runtime::find(InstanceId id) const
{
    if (m_doomed.count(id)) return nullptr;      // logically gone, body pending free
    auto it = m_insts.find(id);
    return it != m_insts.end() ? &it->second : nullptr;
}

InstanceId Runtime::add(Graph graph, HostBindings bindings, ClassIdentity cls)
{
    std::vector<Graph> one;
    one.push_back(std::move(graph));
    return addLevels(std::move(one), std::move(bindings), std::move(cls));
}

InstanceId Runtime::addLevels(std::vector<Graph> levels, HostBindings bindings, ClassIdentity cls)
{
    const InstanceId id = m_next++;
    Inst inst;
    inst.levels = std::move(levels);
    if (inst.levels.empty()) inst.levels.emplace_back();   // always at least one
    inst.host   = std::move(bindings);
    inst.cls    = std::move(cls);
    // Seed the private variable store from the graph's declared defaults so
    // GetVariable reads a valid value even before the first SetVariable.
    // Function-locals (scope != 0) live in the Runner's call frames, not here.
    //
    // Seeded ROOT FIRST, so a derived declaration of the same name overwrites the
    // base's — which is what the leaf-most rule means for a starting value.
    for (const Graph& g : inst.levels)
        for (const auto& var : g.variables)
            if (var.scope == 0) inst.vars[var.name] = variableDefaultValue(var);
    m_insts.emplace(id, std::move(inst));
    return id;
}

InstanceId Runtime::addCompiled(CompiledPtr inst, HostBindings bindings, ClassIdentity cls)
{
    if (!inst) return 0;
    // A generated class already carries its own identity, so a caller that has
    // nothing to add gets it filled in here rather than having to restate what
    // the compiled backend would answer anyway. An explicitly passed value wins
    // — the level script and the GameInstance are keyed by the host, not by the
    // asset path the codegen happened to use.
    if (cls.key.empty())       cls.key       = inst->classKey()      ? inst->classKey()      : "";
    if (cls.baseClass.empty()) cls.baseClass = inst->baseClassKey()  ? inst->baseClassKey()  : "";
    if (cls.chain.empty())
        for (const char* a : inst->classChain())
            if (a) cls.chain.emplace_back(a);

    const InstanceId id = m_next++;
    Inst rec;
    rec.compiled = std::move(inst);
    rec.host     = std::move(bindings);
    rec.cls      = std::move(cls);
    auto [it, ok] = m_insts.emplace(id, std::move(rec));
    // No var seeding: the generated constructor initializes its members to the
    // declared defaults. Wire the same Context the interpreter would get.
    it->second.compiled->bindContext(makeContext(id));
    return id;
}

bool Runtime::instanceIsA(InstanceId id, const std::string& classKey) const
{
    const Inst* inst = find(id);
    if (!inst || classKey.empty()) return false;
    // The exact class, then the HorizonCode ancestry, then the engine
    // taxonomy. The middle one is what makes `Cast To Enemy` succeed on a
    // Goblin — and it is a plain list rather than a walk because resolving it
    // needs the content system, which this layer deliberately cannot reach.
    if (!inst->cls.key.empty() && inst->cls.key == classKey) return true;
    for (const std::string& ancestor : inst->cls.chain)
        if (ancestor == classKey) return true;
    return engineClassIsA(inst->cls.baseClass, classKey);
}

const std::string& Runtime::classKeyOf(InstanceId id) const
{
    static const std::string kNone;
    const Inst* inst = find(id);
    return inst ? inst->cls.key : kNone;
}

const std::string& Runtime::baseClassOf(InstanceId id) const
{
    static const std::string kNone;
    const Inst* inst = find(id);
    return inst ? inst->cls.baseClass : kNone;
}

void Runtime::setOwnedEntity(InstanceId id, uint32_t entity)
{
    if (Inst* inst = find(id)) inst->ownedEntity = entity;
}

uint32_t Runtime::ownedEntity(InstanceId id) const
{
    const Inst* inst = find(id);
    return inst ? inst->ownedEntity : 0u;
}

void Runtime::remove(InstanceId id)
{
    // The instance disappears LOGICALLY here (find() hides doomed ids, so no
    // event/call/alive reaches it any more), but the Inst itself — its graphs
    // and, for compiled classes, the object whose member function may be on
    // the stack RIGHT NOW — must outlive the current run: "Destroy Widget
    // (Get Self)" reaches this mid-execution, and runExecChain keeps reading
    // the graph after the node returns. purgeDoomed() frees it once nothing
    // can be executing (top of update(), clear, level GC).
    m_doomed.insert(id);
    m_listeners.erase(id);                       // dispatchers this instance owned
    for (auto& [owner, events] : m_listeners)    // its subscriptions to others
        for (auto& [ev, vec] : events)
            vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
    // Its scheduled Delay continuations die with it (never resume a ghost).
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
        [&](const PendingResume& p){ return p.id == id; }), m_pending.end());
    if (id == m_gameInstance) { m_gameInstance = 0; m_gameInstanceCompiled = nullptr; }
}
void Runtime::destroy(InstanceId id)
{
    // Symmetric teardown counterpart to Create Object / widget create (which fire
    // "Construct"): let the instance run its destructor before it's unregistered.
    if (!find(id)) return;
    // Guard re-entrancy: a Destruct handler that destroys this same instance
    // (e.g. Destroy Widget on Get Self) would otherwise re-fire Destruct forever.
    if (!m_destructing.insert(id).second) return;
    fireDestruct(id);
    remove(id);
    m_destructing.erase(id);
}
bool Runtime::alive(InstanceId id) const { return find(id) != nullptr; }
void Runtime::clear()
{
    m_insts.clear();
    m_doomed.clear();
    m_listeners.clear();
    m_pending.clear();
    m_gameInstance = 0;
    m_gameInstanceCompiled = nullptr;
}

// The GameInstance is keyed by the HOST, not by a file: it is the one class the
// codegen already names "__game_instance__", and the loose GameInstance.hcode it
// comes from is not a content asset with a path.
static const ClassIdentity kGameInstanceIdentity{ "__game_instance__", "Object" };

InstanceId Runtime::setGameInstance(Graph graph, HostBindings bindings)
{
    if (m_gameInstance) remove(m_gameInstance);
    m_gameInstance = add(std::move(graph), std::move(bindings), kGameInstanceIdentity);
    m_gameInstanceCompiled = nullptr;   // interpreted
    return m_gameInstance;
}

InstanceId Runtime::setGameInstanceCompiled(CompiledPtr inst, HostBindings bindings)
{
    if (!inst) return m_gameInstance; // don't drop a working GameInstance for a null one
    if (m_gameInstance) remove(m_gameInstance);
    m_gameInstance = addCompiled(std::move(inst), std::move(bindings), kGameInstanceIdentity);
    const Inst* gi = find(m_gameInstance);
    m_gameInstanceCompiled = gi ? gi->compiled.get() : nullptr;
    return m_gameInstance;
}

void Runtime::retainOnlyReachableFrom(InstanceId root)
{
    // Mark root + everything reachable through its Ref-typed variables.
    std::unordered_set<InstanceId> keep;
    std::vector<InstanceId>        stack;
    if (find(root)) { keep.insert(root); stack.push_back(root); }
    while (!stack.empty())
    {
        const InstanceId id = stack.back(); stack.pop_back();
        const Inst* i = find(id);
        if (!i) continue;
        // i->vars covers the interpreted store — and, for compiled instances,
        // the overflow store (undeclared names can hold Refs too).
        auto mark = [&](const HorizonCode::Value& v)
        {
            if (v.ref != 0 && !keep.count(v.ref) && find(v.ref))
            { keep.insert(v.ref); stack.push_back(v.ref); }
        };
        for (const auto& [name, val] : i->vars)
        {
            // A map's KEYS are typed separately from its values, so an object
            // held only as a key has to be marked through keyType — otherwise
            // the sweep frees it and the map keeps a key to nothing.
            if (val.keyType == PinType::Ref && val.kind() == HorizonCode::ContainerKind::Map)
                for (const auto& k : val.keys) mark(k);
            if (val.type != PinType::Ref) continue;
            mark(val);
            for (const auto& item : val.items) mark(item);   // Ref arrays / sets / map values
        }
        if (i->compiled)
        {
            std::vector<uint32_t> refs;
            i->compiled->collectRefs(refs);
            for (const uint32_t r : refs)
                if (r != 0 && !keep.count(r) && find(r))
                { keep.insert(r); stack.push_back(r); }
        }
    }
    // Sweep the unmarked (widgets + the level script are already gone by now, so
    // this only drops scene-scoped Create-Object instances the root doesn't hold).
    std::vector<InstanceId> doomed;
    for (const auto& [id, inst] : m_insts)
        if (!keep.count(id)) doomed.push_back(id);
    for (const InstanceId id : doomed) destroy(id); // fire "Destruct" before GC
    // Level GC runs from the (queued) level-switch request, never from inside a
    // runner — the deferred bodies can be freed for real right here.
    purgeDoomed();
}

const Graph& Runtime::graphOf(InstanceId id) const
{
    static const Graph kEmpty;
    const Inst* i = find(id);
    // The MOST DERIVED level — the class this instance is. Callers that want
    // the whole object's surface (which handlers it answers to) ask
    // eventBindingsOf, which unions the chain; this one is for inspecting the
    // class itself.
    return (i && i->hasGraph()) ? i->leaf() : kEmpty;
}

Value Runtime::getVariable(InstanceId id, const std::string& name) const
{
    const Inst* i = find(id);
    if (!i) return {};
    if (i->compiled && findVarInfo(*i->compiled, name))
        return i->compiled->getVariable(name);
    auto it = i->vars.find(name);
    return it != i->vars.end() ? it->second : Value{};
}

void Runtime::setVariable(InstanceId id, const std::string& name, const Value& v)
{
    Inst* i = find(id);
    if (!i) return;
    // Compiled: declared members through reflection; unknown names fall through
    // to the overflow store — Set on an undeclared name still creates an entry.
    if (i->compiled && i->compiled->setVariable(name, v)) return;
    i->vars[name] = v;
}

void Runtime::reseedVariables(InstanceId id)
{
    Inst* i = find(id);
    if (!i) return;
    i->vars.clear();
    i->nodeState.clear();   // DoOnce/FlipFlop start over with the fresh state
    if (i->compiled) { i->compiled->reseedVariables(); return; }
    // Root first, like addLevels: a derived declaration of the same name is the
    // one whose default survives.
    for (const Graph& g : i->levels)
        for (const auto& var : g.variables)
            if (var.scope == 0) i->vars[var.name] = variableDefaultValue(var);
}

const std::unordered_map<std::string, Value>& Runtime::variablesOf(InstanceId id) const
{
    static const std::unordered_map<std::string, Value> kEmpty;
    const Inst* i = find(id);
    return i ? i->vars : kEmpty;
}

std::unordered_map<std::string, Value> Runtime::variablesSnapshot(InstanceId id) const
{
    const Inst* i = find(id);
    if (!i) return {};
    std::unordered_map<std::string, Value> out = i->vars; // full store / overflow
    if (i->compiled)
        for (const auto& vi : i->compiled->varInfos())
            out[vi.name] = i->compiled->getVariable(vi.name);
    return out;
}

std::vector<Runtime::EventBinding> Runtime::eventBindingsOf(InstanceId id) const
{
    std::vector<EventBinding> out;
    const Inst* i = find(id);
    if (!i) return out;
    if (i->compiled)
    {
        for (const auto& e : i->compiled->eventInfos())
            out.push_back({ e.name, e.elem });
        return out;
    }
    // Every level's handlers: a base class's Event is just as firable on this
    // instance as the derived class's own. A name declared on both appears once,
    // because the derived one replaced it (leaf-most wins) — searched leaf first
    // so that is the entry reported.
    for (auto lv = i->levels.rbegin(); lv != i->levels.rend(); ++lv)
        for (const auto& n : lv->nodes)
        {
            auto report = [&](const std::string& name, int elem)
            {
                const bool have = std::any_of(out.begin(), out.end(),
                    [&](const EventBinding& b) { return b.name == name && b.elem == elem; });
                if (!have) out.push_back({ name, elem });
            };
            // An Input Action node handles one event per exec-out, so it reports
            // all of them: this list is what says an instance cares about an
            // event at all, and a handler nobody knows about is never fired.
            if (n.type == NodeType::InputAction)
            {
                for (const std::string& ev : inputActionEventNames(n)) report(ev, 0);
                continue;
            }
            if (n.type != NodeType::Event) continue;
            report(n.s, n.elem);
        }
    return out;
}

// The Context binds variable access to the instance's private store and
// property/show/hide to its host. Captures (this, id) and looks the instance up
// on each call, so it tolerates concurrent add()/remove() of other instances.
Context Runtime::makeContext(InstanceId id, size_t level)
{
    Context ctx;
    ctx.getVariable = [this, id](const std::string& var) -> Value
    { return getVariable(id, var); };
    ctx.setVariable = [this, id](const std::string& var, const Value& v)
    { setVariable(id, var, v); };
    ctx.getProperty = [this, id](int elem, const std::string& prop) -> Value
    {
        const Inst* i = find(id);
        return (i && i->host.getProperty) ? i->host.getProperty(id, elem, prop) : Value{};
    };
    ctx.setProperty = [this, id](int elem, const std::string& prop, const Value& v)
    {
        const Inst* i = find(id);
        if (i && i->host.setProperty) i->host.setProperty(id, elem, prop, v);
    };
    ctx.showSelf = [this, id]
    {
        const Inst* i = find(id);
        if (i && i->host.showSelf) i->host.showSelf(id);
    };
    ctx.hideSelf = [this, id]
    {
        const Inst* i = find(id);
        if (i && i->host.hideSelf) i->host.hideSelf(id);
    };
    // Reference-based delegation.
    ctx.emitEvent = [this, id](const std::string& event, const Value& arg)
    { emitEvent(id, event, arg); };
    ctx.emitEventId = [this, id](EventId ev, const Value& arg)
    { dispatchToListeners(id, ev, eventName(ev), arg); };
    ctx.bindEventId = [this, id](uint32_t target, EventId ev)
    {
        if (!find(target)) { hcError("null reference — Bind Event '" + eventName(ev) +
                                     "' on a null/destroyed object"); return; }
        if (!find(id)) return;
        auto& vec = m_listeners[target][ev];
        if (std::find(vec.begin(), vec.end(), id) == vec.end()) vec.push_back(id);
    };
    ctx.bindEvent = [this, id](uint32_t target, const std::string& event)
    {
        if (!find(target)) { hcError("null reference — Bind Event '" + event + "' on a null/destroyed object"); return; }
        bindEvent(target, event, id);
    };
    ctx.callExternal = [this](uint32_t target, const std::string& fn,
                              const std::vector<Value>& args) -> std::vector<Value>
    {
        if (!find(target)) { hcError("null reference — Call Function '" + fn + "' on a null/destroyed object"); return {}; }
        std::vector<Value> out;
        if (!callFunction(target, fn, /*requirePublic=*/true, args, &out))
            hcWarn("function '" + fn + "' not found or not public on the target object");
        return out;
    };
    // Read/write a variable on another instance — only if it's declared public.
    ctx.getExternal = [this](uint32_t target, const std::string& var) -> Value
    {
        const Inst* i = find(target);
        if (!i) { hcError("null reference — Get '" + var + "' on a null/destroyed object"); return {}; }
        if (i->compiled)
        {
            const CompiledVarInfo* vi = findVarInfo(*i->compiled, var);
            if (!vi || vi->access != 0)
            { hcWarn("variable '" + var + "' not found or not public on the target object"); return {}; }
            return i->compiled->getVariable(var);
        }
        const Variable* v = findVarInLevels(i->levels, var);
        if (!v || v->access != 0 || v->scope != 0) // locals are never externally visible
        { hcWarn("variable '" + var + "' not found or not public on the target object"); return {}; }
        auto it = i->vars.find(var);
        return it != i->vars.end() ? it->second : Value{};
    };
    ctx.setExternal = [this](uint32_t target, const std::string& var, const Value& val)
    {
        Inst* i = find(target);
        if (!i) { hcError("null reference — Set '" + var + "' on a null/destroyed object"); return; }
        if (i->compiled)
        {
            const CompiledVarInfo* vi = findVarInfo(*i->compiled, var);
            if (!vi || vi->access != 0)
            { hcWarn("variable '" + var + "' not found or not public on the target object"); return; }
            i->compiled->setVariable(var, val);
            return;
        }
        const Variable* v = findVarInLevels(i->levels, var);
        if (!v || v->access != 0 || v->scope != 0) // locals are never externally visible
        { hcWarn("variable '" + var + "' not found or not public on the target object"); return; }
        i->vars[var] = val;
    };
    // Compiled-to-compiled shortcut: hand the object over so generated code can
    // call it directly. Interpreted instances answer null, which is exactly what
    // makes the generated fast path fall back to the seam below.
    ctx.resolveCompiled = [this](uint32_t target) -> CompiledInstance*
    {
        const Inst* i = find(target);
        return i ? i->compiled.get() : nullptr;
    };
    ctx.getSelf = [id] { return Value::ofRef(id); };
    ctx.getGameInstance = [this] { return Value::ofRef(m_gameInstance); };
    // No lookup for the one reference whose class is fixed at generation time.
    ctx.gameInstanceCompiled = [this] { return m_gameInstanceCompiled; };
    // World-level services (shared by every instance) — forwarded through the
    // runtime at CALL time, never copied. A copy would freeze whatever was
    // bound at context creation: interpreted instances rebuild their Context
    // every fire so they never noticed, but a COMPILED instance binds its
    // Context once at addCompiled — registering it before setServices (the
    // GameInstance boot order) would leave it with dead services forever.
    ctx.createWidget  = [this](const std::string& path) -> int
    { return m_services.createWidget ? m_services.createWidget(path) : 0; };
    ctx.showWidget    = [this](int id_) { if (m_services.showWidget) m_services.showWidget(id_); };
    ctx.hideWidget    = [this](int id_) { if (m_services.hideWidget) m_services.hideWidget(id_); };
    ctx.destroyWidget = [this](int id_) { if (m_services.destroyWidget) m_services.destroyWidget(id_); };
    // Straight passthrough, including the nullptr-means-"as authored" pointers:
    // the call is synchronous, so the caller's vec3 locals outlive it and there
    // is nothing here to own or copy.
    ctx.createObject  = [this](const std::string& path, const float* pos,
                               const float* rot) -> uint32_t
    { return m_services.createObject ? m_services.createObject(path, pos, rot) : 0u; };
    ctx.destroyObject = [this](uint32_t ref) { if (m_services.destroyObject) m_services.destroyObject(ref); };
    ctx.callApi       = [this, id](const std::string& apiId, const std::vector<Value>& args) -> std::vector<Value>
    { return m_services.callApi ? m_services.callApi(id, apiId, args) : std::vector<Value>{}; };
    // Latent flow + liveness + per-node state (all runtime-side).
    ctx.scheduleResume = [this, id, level](int nodeId, float seconds, bool realTime)
    {
        if (!find(id)) return;
        // Retriggering a pending Delay is ignored (Unreal semantics) — the
        // running timer wins; a tick-driven Delay can't stack continuations.
        // Compared per LEVEL too: the same node id in a base and a derived
        // graph are two different Delays.
        for (const auto& p : m_pending)
            if (p.id == id && p.level == level && p.node == nodeId) return;
        m_pending.push_back({ id, level, nodeId, seconds > 0.0f ? seconds : 0.0f, realTime });
    };
    // An inherited call: resolved across the instance's OTHER levels, and only
    // to public members — a base class's private function stays its own.
    ctx.callOwn = [this, id](const std::string& fn, const std::vector<Value>& args,
                             std::vector<Value>* results) -> bool
    {
        Inst* i = find(id);
        if (!i) return false;
        const int lv = levelWithFunction(*i, fn, /*publicOnly=*/true);
        if (lv < 0) return false;
        // Same cross-runner recursion guard as Runtime::callFunction — this is
        // the one call edge that builds its Runner without going through it.
        if (m_callDepth >= kMaxCallDepth)
        {
            hcError("call depth limit hit calling inherited '" + fn +
                    "' — recursive call chain aborted");
            return false;
        }
        ++m_callDepth;
        Runner r(i->levels[lv], makeContext(id, (size_t)lv));
        const bool ok = r.callFunction(fn, /*requirePublic=*/false, args, results);
        --m_callDepth;
        return ok;
    };
    ctx.isValid = [this](uint32_t target) { return find(target) != nullptr; };
    ctx.isA = [this](uint32_t target, const std::string& classKey)
    { return instanceIsA(target, classKey); };
    ctx.getNodeState = [this, id, level](int nodeId) -> Value
    {
        const Inst* i = find(id);
        if (!i || level >= i->nodeState.size()) return {};
        const auto& m = i->nodeState[level];
        auto it = m.find(nodeId);
        return it != m.end() ? it->second : Value{};
    };
    ctx.setNodeState = [this, id, level](int nodeId, const Value& v)
    {
        if (Inst* i = find(id)) i->stateAt(level)[nodeId] = v;
    };
    return ctx;
}

void Runtime::update(float dt, float unscaledDt)
{
    // Nothing is executing between frames — free the bodies of instances that
    // destroyed themselves (or each other) mid-run since the last tick.
    purgeDoomed();
    if (m_pending.empty()) return;
    // A caller with no notion of a time scale passes one number and means it
    // for every Delay — the behaviour this had before Real Time existed.
    const float realDt = unscaledDt < 0.0f ? dt : unscaledDt;
    // Decrement first, then snapshot the expired set: a resumed chain may
    // schedule NEW delays (they must wait at least one tick), destroy
    // instances (remove() prunes m_pending), or re-enter update indirectly.
    std::vector<PendingResume> expired;
    for (auto& p : m_pending) p.remaining -= p.realTime ? realDt : dt;
    for (const auto& p : m_pending)
        if (p.remaining <= 0.0f) expired.push_back(p);
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
        [](const PendingResume& p){ return p.remaining <= 0.0f; }), m_pending.end());

    for (const auto& p : expired)
    {
        Inst* i = find(p.id);
        if (!i) continue;   // destroyed while waiting
        if (i->compiled)
            i->compiled->resumeFrom(p.node);
        else
        {
            // Resume in the graph the Delay was scheduled from. The node id
            // alone would not say which — a base and a derived graph each have
            // their own node 7, and resuming in the wrong one would run a
            // different chain than the one that paused.
            if (p.level >= i->levels.size()) continue;   // that level is gone
            Runner runner(i->levels[p.level], makeContext(p.id, p.level));
            runner.resumeFrom(p.node);
        }
    }
}

// ── the engine's own events ─────────────────────────────────────────────────
// One shape, twenty-odd entry points. `hook` is the CompiledInstance method for
// events whose only payload is the element; the ones that carry a value spell
// their own dispatch out below, because their hook signatures differ.
//
// The listener pass is NOT optional: fireEvent reaches everyone bound to this
// instance after the owner's own handlers (§3.5), and skipping it here would
// break dispatcher patterns silently — nothing would report it.
// The element-only pointer/focus events.
#define HE_HC_POINTER_EVENT(fn, name, hookFn)                                   \
    void Runtime::fn(InstanceId id, int elem)                                   \
    {                                                                           \
        Inst* i = find(id);                                                     \
        if (!i) return;                                                         \
        if (i->compiled) i->compiled->hookFn(elem);                             \
        else runEventOnLevel(*i, id, name, elem, {}); \
        static const EventId ev = eventId(name);                                \
        dispatchToListeners(id, ev, name, {});                                  \
    }
HE_HC_POINTER_EVENT(fireOnClicked,    "OnClicked",    onClicked)
HE_HC_POINTER_EVENT(fireOnPressed,    "OnPressed",    onPressed)
HE_HC_POINTER_EVENT(fireOnReleased,   "OnReleased",   onReleased)
HE_HC_POINTER_EVENT(fireOnHovered,    "OnHovered",    onHovered)
HE_HC_POINTER_EVENT(fireOnUnhovered,  "OnUnhovered",  onUnhovered)
HE_HC_POINTER_EVENT(fireOnMouseEnter, "OnMouseEnter", onMouseEnter)
HE_HC_POINTER_EVENT(fireOnMouseLeave, "OnMouseLeave", onMouseLeave)
HE_HC_POINTER_EVENT(fireOnFocused,    "OnFocused",    onFocused)
HE_HC_POINTER_EVENT(fireOnUnfocused,  "OnUnfocused",  onUnfocused)
#undef HE_HC_POINTER_EVENT

// The no-payload lifecycle events — their hooks take nothing, so they cannot
// share the element-carrying helper above.
#define HE_HC_PLAIN_EVENT(fn, name, hookFn)                                     \
    void Runtime::fn(InstanceId id)                                             \
    {                                                                           \
        Inst* i = find(id);                                                     \
        if (!i) return;                                                         \
        if (i->compiled) i->compiled->hookFn();                                 \
        else runEventOnLevel(*i, id, name, 0, {}); \
        static const EventId ev = eventId(name);                                \
        dispatchToListeners(id, ev, name, {});                                  \
    }
HE_HC_PLAIN_EVENT(fireConstruct,       "Construct",       onConstruct)
HE_HC_PLAIN_EVENT(fireDestruct,        "Destruct",        onDestruct)
HE_HC_PLAIN_EVENT(fireBeginPlay,       "BeginPlay",       onBeginPlay)
HE_HC_PLAIN_EVENT(fireOnInit,          "OnInit",          onInit)
HE_HC_PLAIN_EVENT(fireOnShutdown,      "OnShutdown",      onShutdown)
HE_HC_PLAIN_EVENT(fireOnLevelLoaded,   "OnLevelLoaded",   onLevelLoaded)
HE_HC_PLAIN_EVENT(fireOnLevelUnloaded, "OnLevelUnloaded", onLevelUnloaded)
#undef HE_HC_PLAIN_EVENT

// The value-carrying ones: same order, own signatures.
#define HE_HC_VALUE_EVENT(fn, name, argType, hookCall, boxed)                   \
    void Runtime::fn(InstanceId id, int elem, argType v)                        \
    {                                                                           \
        Inst* i = find(id);                                                     \
        if (!i) return;                                                         \
        if (i->compiled) i->compiled->hookCall;                                 \
        else runEventOnLevel(*i, id, name, elem, boxed); \
        static const EventId ev = eventId(name);                                \
        dispatchToListeners(id, ev, name, boxed);                               \
    }
HE_HC_VALUE_EVENT(fireOnTextChanged,   "OnTextChanged",   const std::string&,
                  onTextChanged(elem, v),   Value::ofString(v))
HE_HC_VALUE_EVENT(fireOnTextCommitted, "OnTextCommitted", const std::string&,
                  onTextCommitted(elem, v), Value::ofString(v))
HE_HC_VALUE_EVENT(fireOnValueChanged,  "OnValueChanged",  float,
                  onValueChanged(elem, v),  Value::ofFloat(v))
HE_HC_VALUE_EVENT(fireOnCheckChanged,  "OnCheckChanged",  bool,
                  onCheckChanged(elem, v),  Value::ofBool(v))
HE_HC_VALUE_EVENT(fireOnSelectionChanged, "OnSelectionChanged", int,
                  onSelectionChanged(elem, v), Value::ofInt(v))
#undef HE_HC_VALUE_EVENT

// The physics contacts: one Int argument (the other entity), no element. Same
// shape as fireTick rather than the value events, so they get their own macro
// instead of four near-identical hand-written copies.
#define HE_HC_ENTITY_EVENT(fn, name, hook)                                      \
    void Runtime::fn(InstanceId id, uint32_t other)                             \
    {                                                                           \
        Inst* i = find(id);                                                     \
        if (!i) return;                                                         \
        const Value arg = Value::ofInt((int)other);                             \
        if (i->compiled) i->compiled->hook((int)other);                         \
        else runEventOnLevel(*i, id, name, 0, arg); \
        static const EventId ev = eventId(name);                                \
        dispatchToListeners(id, ev, name, arg);                                 \
    }
HE_HC_ENTITY_EVENT(fireOnBeginOverlap, "OnBeginOverlap", onBeginOverlap)
HE_HC_ENTITY_EVENT(fireOnEndOverlap,   "OnEndOverlap",   onEndOverlap)
HE_HC_ENTITY_EVENT(fireOnHit,          "OnHit",          onHit)
HE_HC_ENTITY_EVENT(fireOnHitEnd,       "OnHitEnd",       onHitEnd)
#undef HE_HC_ENTITY_EVENT

void Runtime::fireTick(InstanceId id, float dt)
{
    Inst* i = find(id);
    if (!i) return;
    if (i->compiled) i->compiled->onTick(dt);
    else runEventOnLevel(*i, id, "Tick", 0, Value::ofFloat(dt));
    static const EventId ev = eventId("Tick");
    dispatchToListeners(id, ev, "Tick", Value::ofFloat(dt));
}

void Runtime::fireOnWindowFocusChanged(InstanceId id, bool focused)
{
    Inst* i = find(id);
    if (!i) return;
    if (i->compiled) i->compiled->onWindowFocusChanged(focused);
    else runEventOnLevel(*i, id, "OnWindowFocusChanged", 0, Value::ofBool(focused));
    static const EventId ev = eventId("OnWindowFocusChanged");
    dispatchToListeners(id, ev, "OnWindowFocusChanged", Value::ofBool(focused));
}

void Runtime::fireEvent(InstanceId id, const std::string& event, int elem, const Value& arg)
{
    Inst* i = find(id);
    if (!i) return;
    if (i->compiled)
        i->compiled->fireEvent(event, elem, arg);
    else
        runEventOnLevel(*i, id, event, elem, arg);
    // An event firing on an instance also reaches everyone bound to it, so
    // another class holding a reference can react (Unreal-style dispatchers).
    dispatchToListeners(id, event, arg);
}

void Runtime::emitEvent(InstanceId owner, const std::string& event, const Value& arg)
{
    // Dispatcher semantics: notify listeners only, not the owner's own nodes.
    dispatchToListeners(owner, event, arg);
}

void Runtime::bindEvent(InstanceId owner, const std::string& event, InstanceId listener)
{
    if (!find(owner) || !find(listener)) return;
    auto& vec = m_listeners[owner][eventId(event)];
    if (std::find(vec.begin(), vec.end(), listener) == vec.end())
        vec.push_back(listener);
}

void Runtime::dispatchToListeners(InstanceId owner, const std::string& name, const Value& arg)
{ dispatchToListeners(owner, eventId(name), name, arg); }

void Runtime::dispatchToListeners(InstanceId owner, EventId ev, const std::string& event,
                                  const Value& arg)
{
    if (m_dispatchDepth >= 32) return; // guard cross-instance event cycles (DEPTH)
    auto oit = m_listeners.find(owner);
    if (oit == m_listeners.end()) { if (m_dispatchDepth == 0) m_dispatchFires = 0; return; }
    auto eit = oit->second.find(ev);
    if (eit == oit->second.end()) { if (m_dispatchDepth == 0) m_dispatchFires = 0; return; }

    const std::vector<InstanceId> listeners = eit->second; // copy: fireEvent may re-bind
    ++m_dispatchDepth;
    for (InstanceId l : listeners)
    {
        if (l == owner || !find(l)) continue;
        // TOTAL budget per top-level cascade: the depth guard alone doesn't
        // bound work — each fireEvent can spawn TWO dispatch subtrees (an
        // EmitEvent in the handler + fireEvent's own trailing dispatch), so a
        // bind CYCLE of re-emitting listeners branches into ~2^32 fires. Cut
        // the whole cascade once the budget is spent, loudly.
        if (++m_dispatchFires > kMaxDispatchFires)
        {
            if (m_dispatchFires == kMaxDispatchFires + 1) // warn once per cascade
                hcError("event dispatch budget exceeded while dispatching '" + event +
                        "' — Bind/Emit cycle? aborting the event cascade");
            break;
        }
        fireEvent(l, event, 0, arg); // fires the listener's own Event node of that name
    }
    --m_dispatchDepth;
    if (m_dispatchDepth == 0) m_dispatchFires = 0; // cascade over — fresh budget
}

int Runtime::levelHandlingEvent(const Inst& i, const std::string& event, int elem) const
{
    for (size_t lv = i.levels.size(); lv-- > 0; )
        for (const Node& n : i.levels[lv].nodes)
        {
            if (n.type == NodeType::Event && n.s == event && (n.elem == 0 || n.elem == elem))
                return (int)lv;
            // An Input Action node handles the events its action produces, so a
            // derived class binding one overrides the base's for that action —
            // the same leaf-first rule every other handler follows.
            if (n.type == NodeType::InputAction && inputActionChainFor(n, event) >= 0)
                return (int)lv;
        }
    return -1;
}

int Runtime::levelWithFunction(const Inst& i, const std::string& fn, bool publicOnly) const
{
    for (size_t lv = i.levels.size(); lv-- > 0; )
        for (const Node& n : i.levels[lv].nodes)
            if (n.type == NodeType::FunctionEntry && n.s == fn)
            {
                if (publicOnly && n.access != 0) continue;   // private to its own class
                return (int)lv;
            }
    return -1;
}

void Runtime::runEventOnLevel(Inst& i, InstanceId id, const std::string& event,
                              int elem, const Value& arg)
{
    const int lv = levelHandlingEvent(i, event, elem);
    if (lv < 0) return;
    Runner r(i.levels[lv], makeContext(id, (size_t)lv));
    r.fireEvent(event, elem, arg);
}

bool Runtime::callFunction(InstanceId id, const std::string& fn, bool requirePublic,
                           const std::vector<Value>& args, std::vector<Value>* results)
{
    Inst* i = find(id);
    if (!i) return false;
    // Cross-runner recursion guard. kMaxDepth/kMaxSteps live PER RUNNER, but
    // every Call Function (Ref) edge builds a fresh one with a fresh budget —
    // F calling itself via Get Self, or A.f ↔ B.g ping-pong, would otherwise
    // recurse natively until the process stack blows, in PIE and in packaged
    // builds alike. 64 nested cross-instance calls is far beyond any
    // legitimate graph.
    if (m_callDepth >= kMaxCallDepth)
    {
        hcError("call depth limit hit calling '" + fn +
                "' — recursive Call Function chain aborted");
        return false;
    }
    ++m_callDepth;
    bool ok = false;
    if (i->compiled)
        ok = i->compiled->callFunction(fn, requirePublic, args, results);
    else if (const int lv = levelWithFunction(*i, fn, /*publicOnly=*/false); lv >= 0)
    {
        Runner runner(i->levels[lv], makeContext(id, (size_t)lv));
        ok = runner.callFunction(fn, requirePublic, args, results);
    }
    --m_callDepth;
    return ok;
}

} // namespace HorizonCode
