#pragma once
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

    // ── class metadata ───────────────────────────────────────────────────────
    virtual const char* classKey() const = 0;   // canonical registry key
    virtual const std::vector<CompiledVarInfo>&   varInfos()   const = 0;
    virtual const std::vector<CompiledEventInfo>& eventInfos() const = 0;

    // ── execution (mirrors Runner's entry points) ───────────────────────────
    virtual void fireEvent(const std::string& name, int elem, const Value& arg) = 0;
    virtual bool callFunction(const std::string& name, bool requirePublic,
                              const std::vector<Value>& args,
                              std::vector<Value>* results) = 0;

    // ── variable reflection (Get/SetExternal, GC, reseed, tooling) ──────────
    // getVariable is only meaningful for declared names (see varInfos);
    // setVariable returns false for an unknown name so the Runtime can route it
    // to its per-instance overflow store (undeclared-Set semantics).
    virtual Value getVariable(const std::string& name) const = 0;
    virtual bool  setVariable(const std::string& name, const Value& v) = 0;
    virtual void  reseedVariables() = 0;   // back to declared defaults
    // Every live reference held in Ref-typed members (incl. Ref arrays) — the
    // compiled equivalent of the GC's var-store scan.
    virtual void  collectRefs(std::vector<uint32_t>& out) const = 0;

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
