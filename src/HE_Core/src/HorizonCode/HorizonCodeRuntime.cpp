#include "HorizonCode/HorizonCodeRuntime.h"

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

void Runtime::remove(InstanceId id) { m_insts.erase(id); }
bool Runtime::alive(InstanceId id) const { return find(id) != nullptr; }
void Runtime::clear() { m_insts.clear(); }

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
    return ctx;
}

void Runtime::fireEvent(InstanceId id, const std::string& event, int elem, const Value& arg)
{
    Inst* i = find(id);
    if (!i) return;
    Runner runner(i->graph, makeContext(id));
    runner.fireEvent(event, elem, arg);
}

bool Runtime::callFunction(InstanceId id, const std::string& fn, bool requirePublic)
{
    Inst* i = find(id);
    if (!i) return false;
    Runner runner(i->graph, makeContext(id));
    return runner.callFunction(fn, requirePublic);
}

} // namespace HorizonCode
