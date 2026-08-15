#pragma once
#include <Types/Defines.h>
#include "HorizonCode.h"
#include <functional>
#include <string>
#include <vector>

class ContentManager;

// ── HorizonCode class inheritance ────────────────────────────────────────────
// A class asset's `baseClass` may name another CLASS ASSET instead of an engine
// taxonomy row, which is what lets `Cast To Enemy` succeed on a Goblin and what
// lets a Goblin use what Enemy already defines.
//
// Inheritance is resolved by FLATTENING, once, when the class is loaded: the
// ancestors' graphs are merged into one, nearest-wins, and the runtime then
// executes a single ordinary graph. Everything downstream — the variable store,
// event dispatch, Get/Set/Call External, the garbage collector, the C++ codegen
// — is untouched, because there is still exactly one graph and one object per
// instance. The alternative (a parent instance behind every child, with misses
// routed upward) would have needed a second answer for every one of those.
//
// What "nearest wins" means, matching what an override does in C++:
//   • A derived Event or Function whose name the base also has REPLACES it —
//     the base's version does not run at all, for events exactly as for
//     functions.
//   • A derived variable of the same name replaces the base's declaration.
// A base member is only offered for overriding when it is marked `overridable`
// (Node::overridable) — the same opt-in `virtual` is.
namespace HorizonCode {

struct ResolvedClass
{
    // One graph per class in the chain, ROOT FIRST, the class itself last —
    // what the runtime executes. Kept apart rather than merged so a base class
    // stays a base class at run time: an override is resolved by asking the
    // levels leaf-first, and a node id keeps meaning something only inside its
    // own graph.
    std::vector<Graph>       levels;
    // The same chain merged into ONE graph, nearest declaration winning. Not
    // what runs — it is what the EDITOR reads to show a class's whole member
    // surface in one list, and what the C++ codegen compiled before it learned
    // real inheritance.
    Graph                    graph;
    // Ancestor class keys, NEAREST FIRST, excluding the class itself. This is
    // what Runtime::instanceIsA answers a Cast to a parent class with — resolved
    // here, at load time, because the runtime has no way to read an asset and a
    // class's ancestry cannot change under a running instance anyway.
    std::vector<std::string> chain;
    // The engine taxonomy row at the root of the chain ("" = plain Object).
    std::string              engineBase;
    bool                     ok = false;
};

// Yields a class's stored graph JSON and its `baseClass` string. False = no such
// class. Passed in rather than taken as a ContentManager so this layer stays
// free of the content system — and so the merge can be tested without one.
using ClassLoader = std::function<bool(const std::string& key, std::string& graphJson,
                                       std::string& baseClass)>;

// Walk `key`'s ancestry and flatten it. A cycle (A derives from B derives from A)
// stops the walk and is logged rather than hanging; the classes resolved up to
// that point are kept, which is the most useful thing that can still be true.
HE_API ResolvedClass resolveClass(const std::string& key, const ClassLoader& load);

// The same over the content system — what every spawn site actually calls.
HE_API ResolvedClass resolveClassAsset(ContentManager& cm, const std::string& classPath);

// Merge `derived` INTO `base`, applying the override rules above. Exposed for
// the tests; resolveClass is the way to use it.
HE_API void mergeDerivedInto(Graph& base, Graph derived);

// Every overridable member the ancestors of `key` declare — what a derived
// class's add menu offers as overrides. Nearest ancestor first, and a name that
// appears twice in the chain is reported once (the nearest declaration wins,
// exactly as the flattening resolves it).
struct OverridableMember
{
    NodeType    kind;        // Event or FunctionEntry
    std::string name;
    std::string fromClass;   // which ancestor declared it (for the menu's hint)
    // The declaration itself, so inserting an override starts from the same
    // signature instead of an empty stub the author has to re-type.
    Node        prototype;
};
HE_API std::vector<OverridableMember> overridableMembers(const std::string& key,
                                                         const ClassLoader& load);
HE_API std::vector<OverridableMember> overridableMembersOf(ContentManager& cm,
                                                           const std::string& classPath);

// ── What a derived class inherits by NAME ────────────────────────────────────
// An ancestor's variable is reachable from the derived class under the same name
// (there is one variable store per instance, searched leaf-first), and so is an
// ancestor's public function (Runtime::callFunction escalates across the levels).
// The editor needs both: to OFFER them, and — for variables — to refuse a new
// declaration that would take a name the chain already uses. That refusal has to
// include the ancestors' PRIVATE variables too: private or not, they occupy a
// slot in the same store, so a same-named declaration in the child would not
// shadow the base's state, it would overwrite it.
struct InheritedVariable
{
    Variable    var;
    std::string fromClass;   // which ancestor declared it (for the editor's hint)
};
struct InheritedFunction
{
    Node        proto;       // the FunctionEntry, so a call node mirrors its pins
    std::string fromClass;
};
// Both read a resolved chain rather than loading one: every caller already has
// the ResolvedClass in hand, and taking it keeps these testable without a
// ContentManager. Nearest ancestor first; a name declared twice in the chain is
// reported once, by its nearest declaration. The class ITSELF contributes
// nothing — its own members are not "inherited".
HE_API std::vector<InheritedVariable> inheritedVariables(const ResolvedClass& rc);
HE_API std::vector<InheritedFunction> inheritedFunctions(const ResolvedClass& rc);

} // namespace HorizonCode
