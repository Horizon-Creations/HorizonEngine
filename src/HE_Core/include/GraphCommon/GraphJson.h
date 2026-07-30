// Shared JSON scaffolding for the engine's graph assets (MaterialGraph,
// ParticleGraph, HorizonCode::Graph, UIWidgetTree, AnimatorStateMachineGraph).
//
// *** EVERYTHING BELOW IS ON-DISK FORMAT. ***
// These graphs are USER PROJECT ASSETS. A file written by any shipped editor
// must still load. So this header unifies the CODE, not the format: where the
// five systems write different shapes, the difference is preserved and
// documented here rather than "cleaned up".
#pragma once

#include <Types/UUID.h>
#include <nlohmann/json.hpp>

#include <string>

namespace HE::graph
{

// ── Parse guard ──────────────────────────────────────────────────────────────
// Every graph loader starts identically: parse WITHOUT exceptions (a corrupt or
// hand-edited asset must not throw out of a load — see the config.json crash
// loop that motivated non-throwing parses engine-wide) and reject anything that
// is not a JSON object.
inline bool parseGraphObject(const std::string& json, nlohmann::json& out)
{
    out = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    return !out.is_discarded() && out.is_object();
}

// ── nextId repair ────────────────────────────────────────────────────────────
// A saved graph carries its own nextId, but a hand-edited or hand-merged file
// can carry node ids at or above it. Loading such a file unrepaired would hand
// the next added node an id that already exists, silently aliasing two nodes
// (and every link pointing at either). Every loader therefore raises nextId past
// every id it reads. Equivalent to the `std::max(nextId, id + 1)` spelling some
// call sites used before.
inline void bumpNextId(int& nextId, int id) { if (id >= nextId) nextId = id + 1; }

// ── Links ────────────────────────────────────────────────────────────────────
// *** TWO ON-DISK LINK FORMATS EXIST, DELIBERATELY. ***
//   OBJECT  {"sn":s,"sp":p,"dn":d,"dp":p}  — MaterialGraph, ParticleGraph
//   ARRAY   [s, sp, d, dp]                 — HorizonCode::Graph
// They cannot be converged: converging the writers would make every existing
// .hasset unreadable by shipped editors, and converging the readers alone buys
// nothing. Each system reads and writes exactly what it always did; the shared
// helpers below just remove the copy-paste.
inline nlohmann::json linkToObject(int sn, int sp, int dn, int dp)
{
    return { { "sn", sn }, { "sp", sp }, { "dn", dn }, { "dp", dp } };
}
// Missing keys read as 0 — the historical behaviour of `j.value("sn", 0)`. A link
// entry that is not an object at all reads as all-zero instead of throwing
// type_error.306 out of the loader (node id 0 is never allocated, so the result
// is an inert dangling link rather than a crash on a corrupt asset).
inline void linkFromObject(const nlohmann::json& j, int& sn, int& sp, int& dn, int& dp)
{
    sn = sp = dn = dp = 0;
    if (!j.is_object()) return;
    sn = j.value("sn", 0); sp = j.value("sp", 0);
    dn = j.value("dn", 0); dp = j.value("dp", 0);
}

inline nlohmann::json linkToArray(int sn, int sp, int dn, int dp)
{
    return nlohmann::json::array({ sn, sp, dn, dp });
}
// Returns false for anything that is not a 4+ element array (a truncated entry
// is skipped rather than read as zeros — HorizonCode's historical behaviour).
inline bool linkFromArray(const nlohmann::json& j, int& sn, int& sp, int& dn, int& dp)
{
    if (!j.is_array() || j.size() < 4) return false;
    sn = j[0].get<int>(); sp = j[1].get<int>();
    dn = j[2].get<int>(); dp = j[3].get<int>();
    return true;
}

// ── Asset references ─────────────────────────────────────────────────────────
// UUIDs serialize as {"hi":…,"lo":…} (ParticleGraph mesh/material slots,
// AnimatorStateMachine clip ids). A missing/!object value reads as the null
// UUID, so an absent key and an explicitly-zero one are the same on load —
// which is why writers are free to omit null references (ParticleGraph does,
// AnimatorStateMachine does not; both load identically).
inline nlohmann::json uuidToJson(const HE::UUID& u)
{
    return { { "hi", u.hi }, { "lo", u.lo } };
}
inline HE::UUID uuidFromJson(const nlohmann::json& j)
{
    HE::UUID u;
    if (j.is_object()) { u.hi = j.value("hi", uint64_t(0)); u.lo = j.value("lo", uint64_t(0)); }
    return u;
}

// ── Pretty vs compact dumps ──────────────────────────────────────────────────
// Also an on-disk difference that is deliberately NOT unified:
//   compact  j.dump()   — MaterialGraph, ParticleGraph, AnimatorStateMachine
//   pretty   j.dump(2)  — HorizonCode::Graph, UIWidgetTree
// Whitespace is part of the bytes on disk, and the incremental packer reuses
// pack entries by hashing the asset blob (.hpak.manifest), so flipping a dump
// style would silently invalidate every affected asset in every project's pack
// cache. Left exactly as each system had it.

} // namespace HE::graph
