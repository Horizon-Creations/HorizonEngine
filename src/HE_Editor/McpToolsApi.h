#pragma once

// ─── The engine's own API, as MCP tools ──────────────────────────────────────
// The three tool files before this one were written by hand: somebody decided
// `entity_set_transform` should exist, spelled its schema and wrote its handler.
// This one is not. `HE::api::registry()` already describes every gameplay
// function the engine has — id, category, typed params, typed results, the
// exec/pure split and a thunk that calls it — and five places already walk that
// table to build something (Lua's `horizon.<group>.<fn>`, Python's twin, the
// graph editor's add menu, the node reference, the palette). This is the sixth
// reader, and it turns a row into an `McpTool`: name from `id`, description from
// `NodeDocs::engineCall`, inputSchema from `params`, handler = JSON → Values →
// `ApiFn::invoke` → Values → JSON.
//
// So a function added to the engine becomes callable by an external client
// without anybody touching this file — which is the whole reason the registry is
// machine-readable in the first place.
//
// ── What is NOT callable, and why ────────────────────────────────────────────
// Only PURE rows (`isExec == false`) get a tool. That is the rule
// docs/mcp-editor-integration-plan.md §2.7 settled on before any of this was
// built, and it is the same rule the whole theme exists for: an exec row writes
// straight into the component (plus a physics teleport) and knows nothing about
// undo, about locks or about publishing to a collaboration session. Handing one
// to a client would be exactly the fourth mutation path that `EditorCommands`
// was built to prevent — so `transform.setPosition` is not a tool here, while
// `entity_set_transform`, which does the same thing through the gateway, is.
//
// Two groups of pure rows are refused on top of that (`kDeniedGroups`), because
// they read something that is not the project: the human's clipboard and the
// machine's PATH.
//
// A refused row is still LISTED (`api_list` carries `callable: false` and the
// reason), because "why can I not call this" is a question the client will have
// and an absence cannot answer it.
//
// ── Addressing entities ──────────────────────────────────────────────────────
// The registry speaks raw entt handles; every other tool on this interface
// speaks uuids (see McpToolsEntity.cpp). So an Int parameter accepts EITHER: a
// number is a handle, a string is a uuid and is resolved through the same index
// the gateway uses. An unresolvable uuid fails `not_found` — never falls back to
// 0, which is a perfectly valid handle and would silently act on a stranger.
// Int results named `entity` carry the uuid alongside, so a raycast hit can be
// handed straight to `entity_get`.
//
// Deliberately free of ImGui and of EditorApplication, like its three siblings:
// what the editor contributes is a `Ctx` and two uuid lookups, so "does
// api_transform_getPosition return where the entity actually stands" is a
// question the test binary can put to a `HorizonWorld` on the stack.

#include "McpToolRegistry.h"

#include <HorizonScene/EngineApi.h>

#include <cstdint>
#include <functional>
#include <string>

namespace HE::Ed
{

struct McpApiHooks
{
	// The host half of a call. Built by the editor's own `apiCtx()`, so a row
	// reached from here finds exactly what it finds when Lua, Python or a
	// HorizonCode graph reaches it. Unset = every tool refuses with
	// `no_context`; a Ctx assembled here would be one with half its fields
	// defaulted, which is the failure EngineApi.h's own comment warns about.
	std::function<HE::api::Ctx()> makeCtx;

	// uuid → raw entt handle, and back. -1 = no entity with that uuid (0 cannot
	// mean that: it is the first handle entt hands out). Unset = a uuid string
	// is refused rather than guessed at.
	std::function<std::int64_t(const std::string& uuid)> entityByUuid;
	std::function<std::string(std::uint32_t entity)>     uuidOf;
};

// The MCP tool name for a registry id: `api_` + the id with its dots turned into
// underscores ("transform.getPosition" → "api_transform_getPosition"). Dots are
// not spellable — see `McpToolRegistry::enforceNameRule`.
std::string apiToolName(const std::string& id);

// May this row be handed to a client at all? `outReason` is filled with the
// wire code ("exec", "denied_group", "unsupported_signature") when not.
bool apiRowCallable(const HE::api::ApiFn& fn, std::string* outReason = nullptr);

// The row as an MCP tool: schema, description, handler. Only meaningful for a
// row `apiRowCallable` accepts — the caller checks, this builds.
McpTool apiRowTool(const HE::api::ApiFn& fn, const McpApiHooks& hooks);

// One tool per callable row, plus `api_list` — the catalogue of the whole
// registry, callable or not, so a client can find out what exists and why the
// rest is missing.
void registerApiTools(McpToolRegistry& registry, McpApiHooks hooks);

} // namespace HE::Ed
