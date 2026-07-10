#include "HorizonCode/HorizonCodeRuntime.h"
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
void hcError(const std::string& msg)   { Logger::Log(Logger::LogLevel::Error,   ("HorizonCode: " + msg).c_str()); }
void hcWarn (const std::string& msg)   { Logger::Log(Logger::LogLevel::Warning, ("HorizonCode: " + msg).c_str()); }

// The compiled counterpart to Graph::findVariable — locals never appear in
// varInfos, so (unlike the interpreted path) no scope check is needed here.
const CompiledVarInfo* findVarInfo(const CompiledInstance& c, const std::string& name)
{
    for (const auto& vi : c.varInfos())
        if (name == vi.name) return &vi;
    return nullptr;
}
}

Runtime::Inst* Runtime::find(InstanceId id)
{
    auto it = m_insts.find(id);
    return it != m_insts.end() ? &it->second : nullptr;
}
const Runtime::Inst* Runtime::find(InstanceId id) const
{
    auto it = m_insts.find(id);
    return it != m_insts.end() ? &it->second : nullptr;
}

InstanceId Runtime::add(Graph graph, HostBindings bindings)
{
    const InstanceId id = m_next++;
    Inst inst;
    inst.graph = std::move(graph);
    inst.host  = std::move(bindings);
    // Seed the private variable store from the graph's declared defaults so
    // GetVariable reads a valid value even before the first SetVariable.
    // Function-locals (scope != 0) live in the Runner's call frames, not here.
    for (const auto& var : inst.graph.variables)
        if (var.scope == 0) inst.vars[var.name] = variableDefaultValue(var);
    m_insts.emplace(id, std::move(inst));
    return id;
}

InstanceId Runtime::addCompiled(CompiledPtr inst, HostBindings bindings)
{
    if (!inst) return 0;
    const InstanceId id = m_next++;
    Inst rec;
    rec.compiled = std::move(inst);
    rec.host     = std::move(bindings);
    auto [it, ok] = m_insts.emplace(id, std::move(rec));
    // No var seeding: the generated constructor initializes its members to the
    // declared defaults. Wire the same Context the interpreter would get.
    it->second.compiled->bindContext(makeContext(id));
    return id;
}

void Runtime::remove(InstanceId id)
{
    m_insts.erase(id);
    m_listeners.erase(id);                       // dispatchers this instance owned
    for (auto& [owner, events] : m_listeners)    // its subscriptions to others
        for (auto& [ev, vec] : events)
            vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
    if (id == m_gameInstance) m_gameInstance = 0;
}
void Runtime::destroy(InstanceId id)
{
    // Symmetric teardown counterpart to Create Object / widget create (which fire
    // "Construct"): let the instance run its destructor before it's unregistered.
    if (!find(id)) return;
    // Guard re-entrancy: a Destruct handler that destroys this same instance
    // (e.g. Destroy Widget on Get Self) would otherwise re-fire Destruct forever.
    if (!m_destructing.insert(id).second) return;
    fireEvent(id, "Destruct", 0);
    remove(id);
    m_destructing.erase(id);
}
bool Runtime::alive(InstanceId id) const { return find(id) != nullptr; }
void Runtime::clear()
{
    m_insts.clear();
    m_listeners.clear();
    m_gameInstance = 0;
}

InstanceId Runtime::setGameInstance(Graph graph, HostBindings bindings)
{
    if (m_gameInstance) remove(m_gameInstance);
    m_gameInstance = add(std::move(graph), std::move(bindings));
    return m_gameInstance;
}

InstanceId Runtime::setGameInstanceCompiled(CompiledPtr inst, HostBindings bindings)
{
    if (!inst) return m_gameInstance; // don't drop a working GameInstance for a null one
    if (m_gameInstance) remove(m_gameInstance);
    m_gameInstance = addCompiled(std::move(inst), std::move(bindings));
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
        for (const auto& [name, val] : i->vars)
        {
            if (val.type != PinType::Ref) continue;
            if (val.ref != 0 && !keep.count(val.ref) && find(val.ref))
            { keep.insert(val.ref); stack.push_back(val.ref); }
            for (const auto& item : val.items)   // Ref arrays
                if (item.ref != 0 && !keep.count(item.ref) && find(item.ref))
                { keep.insert(item.ref); stack.push_back(item.ref); }
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
}

const Graph& Runtime::graphOf(InstanceId id) const
{
    static const Graph kEmpty;
    const Inst* i = find(id);
    return i ? i->graph : kEmpty;
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
    if (i->compiled) { i->compiled->reseedVariables(); return; }
    for (const auto& var : i->graph.variables)
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
    for (const auto& n : i->graph.nodes)
        if (n.type == NodeType::Event)
            out.push_back({ n.s, n.elem });
    return out;
}

// The Context binds variable access to the instance's private store and
// property/show/hide to its host. Captures (this, id) and looks the instance up
// on each call, so it tolerates concurrent add()/remove() of other instances.
Context Runtime::makeContext(InstanceId id)
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
        const Variable* v = i->graph.findVariable(var);
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
        const Variable* v = i->graph.findVariable(var);
        if (!v || v->access != 0 || v->scope != 0) // locals are never externally visible
        { hcWarn("variable '" + var + "' not found or not public on the target object"); return; }
        i->vars[var] = val;
    };
    ctx.getSelf = [id] { return Value::ofRef(id); };
    ctx.getGameInstance = [this] { return Value::ofRef(m_gameInstance); };
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
    ctx.createObject  = [this](const std::string& path) -> uint32_t
    { return m_services.createObject ? m_services.createObject(path) : 0u; };
    ctx.destroyObject = [this](uint32_t ref) { if (m_services.destroyObject) m_services.destroyObject(ref); };
    ctx.callApi       = [this](const std::string& apiId, const std::vector<Value>& args) -> std::vector<Value>
    { return m_services.callApi ? m_services.callApi(apiId, args) : std::vector<Value>{}; };
    return ctx;
}

void Runtime::fireEvent(InstanceId id, const std::string& event, int elem, const Value& arg)
{
    Inst* i = find(id);
    if (!i) return;
    if (i->compiled)
        i->compiled->fireEvent(event, elem, arg);
    else
    { Runner runner(i->graph, makeContext(id)); runner.fireEvent(event, elem, arg); }
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
    auto& vec = m_listeners[owner][event];
    if (std::find(vec.begin(), vec.end(), listener) == vec.end())
        vec.push_back(listener);
}

void Runtime::dispatchToListeners(InstanceId owner, const std::string& event, const Value& arg)
{
    if (m_dispatchDepth >= 32) return; // guard cross-instance event cycles (DEPTH)
    auto oit = m_listeners.find(owner);
    if (oit == m_listeners.end()) { if (m_dispatchDepth == 0) m_dispatchFires = 0; return; }
    auto eit = oit->second.find(event);
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

bool Runtime::callFunction(InstanceId id, const std::string& fn, bool requirePublic,
                           const std::vector<Value>& args, std::vector<Value>* results)
{
    Inst* i = find(id);
    if (!i) return false;
    if (i->compiled)
        return i->compiled->callFunction(fn, requirePublic, args, results);
    Runner runner(i->graph, makeContext(id));
    return runner.callFunction(fn, requirePublic, args, results);
}

} // namespace HorizonCode
