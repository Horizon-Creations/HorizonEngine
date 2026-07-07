#include "HorizonCode/HorizonCodeRuntime.h"
#include <algorithm>

namespace HorizonCode {

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
    for (const auto& var : inst.graph.variables)
        inst.vars[var.name] = variableDefaultValue(var);
    m_insts.emplace(id, std::move(inst));
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
    auto it = i->vars.find(name);
    return it != i->vars.end() ? it->second : Value{};
}

void Runtime::setVariable(InstanceId id, const std::string& name, const Value& v)
{
    if (Inst* i = find(id)) i->vars[name] = v;
}

const std::unordered_map<std::string, Value>& Runtime::variablesOf(InstanceId id) const
{
    static const std::unordered_map<std::string, Value> kEmpty;
    const Inst* i = find(id);
    return i ? i->vars : kEmpty;
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
    { bindEvent(target, event, id); };
    ctx.callExternal = [this](uint32_t target, const std::string& fn)
    { callFunction(target, fn, /*requirePublic=*/true); };
    ctx.getSelf = [id] { return Value::ofRef(id); };
    ctx.getGameInstance = [this] { return Value::ofRef(m_gameInstance); };
    return ctx;
}

void Runtime::fireEvent(InstanceId id, const std::string& event, int elem, const Value& arg)
{
    Inst* i = find(id);
    if (!i) return;
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
    if (m_dispatchDepth >= 32) return; // guard cross-instance event cycles
    auto oit = m_listeners.find(owner);
    if (oit == m_listeners.end()) return;
    auto eit = oit->second.find(event);
    if (eit == oit->second.end()) return;

    const std::vector<InstanceId> listeners = eit->second; // copy: fireEvent may re-bind
    ++m_dispatchDepth;
    for (InstanceId l : listeners)
        if (l != owner && find(l))
            fireEvent(l, event, 0, arg); // fires the listener's own Event node of that name
    --m_dispatchDepth;
}

bool Runtime::callFunction(InstanceId id, const std::string& fn, bool requirePublic)
{
    Inst* i = find(id);
    if (!i) return false;
    Runner runner(i->graph, makeContext(id));
    return runner.callFunction(fn, requirePublic);
}

} // namespace HorizonCode
