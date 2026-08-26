#pragma once
#include <string>
#include <string_view>

// ── What every HorizonCode node does ─────────────────────────────────────────
// One sentence or two for every callable thing a graph can hold: each of the
// engine-API calls in HE::api::registry(), and the built-in node types (those
// come from HorizonCode::nodeTooltip, which already had them).
//
// Until now twenty of the API calls had a description and the rest showed their
// registry id and nothing else — "physics.addImpulse", take it or leave it. The
// id is the one thing a reader can already see on the node.
//
// ── What an entry is for ─────────────────────────────────────────────────────
// The pin list under the tooltip is rendered from the LIVE signature, so an
// entry must not restate the parameters; it would only drift from them. What it
// carries is what a signature cannot say:
//
//   * units and ranges — degrees or radians, seconds or frames, 0..1 or metres;
//   * what happens when the input is wrong — an entity that does not exist, a
//     save that was never created, a handle that has stopped;
//   * the preconditions — play mode only, needs a Camera Rig, needs a physics
//     body, overridden by a Weather component;
//   * why the pure/exec split falls where it does, where that is surprising.
//
// ── Where it shows up ────────────────────────────────────────────────────────
// Hovering the node in a graph, hovering its row in the add-node palette, and
// the "HorizonCode Node Reference" page in the manual — which is BUILT from
// this table plus the registry, so a call added to the engine appears in the
// documentation without anyone writing the page (tests/test_hc_node_docs.cpp
// fails if it appears without a description).

namespace HE::Ed::NodeDocs
{

// The description for an engine-call registry id ("transform.setPosition").
// Empty for an id that is not in the registry. The env.* rows are generated
// from the same field list the registry is built from, so a new sky property
// cannot slip in undocumented.
std::string engineCall(std::string_view id);

// Whether `id` has a description without building one — for the coverage test
// and for callers that want to fall back rather than show an empty line.
bool hasEngineCall(std::string_view id);

// The ids the hand-written table carries, for the drift test: an entry for a
// call that no longer exists is as much a defect as a call with no entry, and
// only this direction can find it. (The env.* rows are generated from the
// engine's own field list and cannot drift.)
int         explicitCount();
const char* explicitId(int i);

} // namespace HE::Ed::NodeDocs
