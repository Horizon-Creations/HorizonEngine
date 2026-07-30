# Horizon Engine — Coding Conventions

> Settled during the 2026-07 rework (`docs/rework-audit-2026-07.md` §5, "Benennungs-
> Inkonsistenzen"). This file is the *decision record*: the audit found the same idea
> spelled three ways in one class, so the conventions below were picked and then applied
> mechanically. Read this before writing new code or starting a rename.

Every rule is grounded in a real place in this tree. Line numbers were verified on
2026-07-31; if one has drifted, grep for the quoted text.

---

## 1. Namespaces — `HE` for anything new

New types, functions and constants go in `namespace HE` (or a nested one such as
`HE::graph`, `HE::api`, `HE::hccg`). `HE::Window`
(`src/HE_Core/include/Window/Window.h:11` + `:26`) is what the target state looks like.

The existing global-namespace stock migrates **gradually** — do not bulk-move it as a
side effect of unrelated work.

### Already moved

| Symbol | Now | Sites updated |
|---|---|---|
| `File`, `Folder` | `HE::File`, `HE::Folder` (`Diagnostics/DiagnosticsStructs.h`) | 53 call sites, 4 files |
| `ScriptLanguage`, `scriptLanguageFromPath` | `HE::…` (`Scripting/ScriptTypes.h`) | 59 call sites, 8 files |
| `toString`, `projectScriptLanguageFromString`, `cppIdentifier` | `HE::tools::…` (`HE_Tools/src/FileOps/ProjectManager.h`) | 12 call sites + 3 decls/3 defs, 3 files |

Notes on those three, because the *reasons* generalise:

- **`File`/`Folder` were the urgent ones.** Maximally generic names at global scope in a
  codebase where nearly every TU also sees `<filesystem>`. They are plain structs with no
  export decoration, so moving them broke no ABI.
- **`ScriptLanguage` was previously recorded here as "pinned to global scope on
  purpose".** That was reversed deliberately. The recorded hazard — "an
  `HE::ScriptLanguage` would silently shadow it for code inside `namespace HE`" — is an
  argument against *duplicating* it, not against *moving* it: the shadowing is only
  possible while both spellings exist, so relocating it is precisely what removes the
  hazard. The enum's *values* are what the `CHUNK_SLNG` byte and script instance ids
  bake in; the *name* reaches no file, so no user data moved. The rename was also
  self-verifying — with only one spelling in the tree, any missed call site is a hard
  compile error rather than a silent misbind.
- **Only `toString`/`cppIdentifier` moved out of `ProjectManager.h`, not the whole
  header.** They went to `HE::tools`, not plain `HE`, on purpose: a generic `toString`
  dropped into the broad `HE` namespace would just re-create a milder version of the same
  problem. `ProjectManager`, `ProjectData`, `ExportProfile`, `ProjectPreset` and
  `ProjectScriptLanguage` are specific enough to stay global for now.
  `projectScriptLanguageFromString` came along because it is `toString`'s documented
  inverse — splitting a documented pair across two namespaces is worse than either end
  state.

### Still outstanding at global scope

All four are **`HE_API`-exported** — i.e. `__declspec(dllexport)` on Windows,
`visibility("default")` elsewhere (`src/HE_Core/include/Types/Defines.h:5–10`). Moving
any of them changes its mangled name, and a hot-loaded `GameLogic` library resolves its
undefined symbols against the host at load time, so out-of-repo game code that touches
them would break at load, not at compile. **That is an API-break decision for the project
owner, not a cleanup** — do not fold it into a rename pass.

| Symbol | Declared at | Footprint |
|---|---|---|
| `ThreadPool` | `JobSystem/JobSystem.h:13` | 3 files, 14 hits |
| `EventBus` | `Events/EventBus.h:58` | 3 files, 40 hits |
| `Input` | `Application/Input.h:12` | 33 files, 119 hits |
| `Logger` | `Diagnostics/Logger.h:6` | 45 files, 478 `Logger::` call sites |

`ThreadPool` and `EventBus` are small enough to do in one sitting the moment the export
question is settled — they are blocked on the decision, not on the work. `Input` is the
one where the collision risk is already real rather than theoretical: `EditorCamera.h:20`
declares a *second* `struct Input` (nested, so it does not currently collide) purely
because the name was taken. `Logger` is the largest by an order of magnitude; if it ever
moves, consider introducing a logging macro first so the call sites stop naming the type
at all.

---

## 2. Data members — `m_`

Private data members use `m_`. **Not** a trailing `_`, **not** bare. The audit's poster
child was `HorizonWorld`, which had `registry_` sitting directly next to
`m_hierarchyDirty`; it now reads consistently
(`src/HE_Scene/include/HorizonScene/HorizonWorld.h:149` + `:151`).

**Public fields are left alone.** Commit `c993482` renamed private members only, because
a public field may belong to a serialised struct or to the game-facing API — renaming it
is an ABI/format change, not a style change. That distinction is the rule, not an
oversight.

Status: **all six modules are clean.** The only trailing-underscore declarations a grep
still finds anywhere in the first-party tree are `ddx_`/`ddy_` at
`src/HE_Rendering/src/Backends/D3D_Shared/HlslSources.h:213–214` — and those are HLSL
locals inside a shader string, not C++ members. See §7.

---

## 3. File-local statics in the editor — `s_`

Editor translation units use `s_` for file-local statics, not `g_` (e.g.
`src/HE_Editor/ContentBrowserPanel.cpp:53`, `static bool s_quietContentRefresh`).
First-party `src/HE_Editor` is now at **zero** `g_` identifiers.

> ⚠️ `grep -rn '\bg_' src/HE_Editor/` still returns ~890 hits. **Every one of them is in
> `src/HE_Editor/vendor/imgui`.** See §8.2 — that tree is off-limits.

---

## 4. Casing

- **Types**: `PascalCase`.
- **Methods**: `camelCase` by default (`EngineProfiler::beginScope`,
  `InputMapping::mapAction`).
- **The renderer backend surface is `PascalCase`** — the `IRenderer` virtuals and every
  backend override of them: `virtual void Initialize(...)`
  (`src/HE_Core/include/Renderer/IRenderer.h:81`), `SetGISettings` (`:195`). Backend
  *private helpers* stay `camelCase`; `VulkanRenderer.h` shows the split cleanly —
  `public:` at `:26` is all PascalCase overrides, `private:` at `:82` onward is
  camelCase helpers.
- **Compile-time constants**: `k` + `PascalCase` (~204 in the tree), e.g.
  `kMatMaxGraphTextures` at `src/HE_Core/include/MaterialGraph/MaterialGraph.h:290`.

---

## 5. Acronyms stay upper-case — `GI`, not `Gi`

`GI`, `UI`, `UUID`, `SSAO`, `HDR`, `LOD` keep all their letters capitalised.

What triggered the rule: the same DDGI constant was spelled `kGIProbeSpacing` in the
Metal backend and `kGiProbeSpacing` in the GL/Vulkan/D3D ones. Class-scoped, so nothing
collided and nothing was broken — but four backends named one value two ways. Now
unified on `kGIProbeSpacing`
(`src/HE_Rendering/include/Backends/Metal/MetalRenderer.h:619`,
`.../OpenGL/OpenGLRenderer.h:696`, `.../Vulkan/VulkanRenderer.h:602`).

**Deferred, deliberately:** the CPU BVH family — `HE::GiBvh`, `GiBvhNode`,
`GiBvhTriangle` (`src/HE_Rendering/include/HorizonRendering/GiBvh.h:40`, `:24`, `:34`) —
is still `Gi`-spelled. Renaming the type wants the *file* renamed too, which drags in
`src/HE_Rendering/CMakeLists.txt:282`, `tests/CMakeLists.txt:70`, `tests/test_gi_bvh.cpp`
and eleven backend sources. Correct call, and the reason it is worth writing down: a
cosmetic rename whose blast radius reaches the build files is not cosmetic.

Watch the shader boundary either way — `kOctSize` at
`src/HE_Rendering/src/Backends/D3D_Shared/HlslSources.h:732` carries a "must match the
host's `kGIProbeOctSize`" comment, so host and shader move together or not at all (§7).

---

## 6. Comments — English, and say *why*

**Language: English.** Two German remnants are known and should be converted when the
surrounding code is next touched: `src/HE_Editor/EditorApplication.cpp:1118` and
`src/HE_Core/include/Diagnostics/GlobalState.h:64`. ("Möller-Trumbore" is a surname, not
a remnant.)

**House style: explain the reason, not the mechanics.** A comment that restates the code
is noise; a comment that records *why the code is shaped this way* — especially when the
shape came from a bug — is the thing that survives. The audit called this the codebase's
strongest asset. Keep it up. Two representative examples:

- `src/HE_Core/include/MaterialGraph/MaterialGraph.h:292–298` — explains why surplus
  material parameters are baked as literals, and states the bug that motivated it: the
  layout used to be clamped only *after* emission, so the generated shader read
  `heParams.v[16]` out of bounds.
- `src/HE_Core/include/UIWidget/UIElements.h:67–70` — explains the auto-size rule and
  closes with "this is why bumping FontSize used to clip the text — the box stayed
  200×30."

When you fix a bug, leave the *why* behind in a comment. When you delete code, don't
leave the comment that described it.

---

## 7. Not covered by any rename convention: external formats

Everything above governs **C++ identifiers**. Strings that reach a file the user keeps
are a **format**. Renaming a symbol is safe; renaming a format breaks user data, usually
silently, usually only noticed weeks later.

Treat as external:

- **Serialised type/node names.** `src/HE_Core/src/HorizonCode/HorizonCode.cpp:281–294`
  is a boxed warning above `nodeDisplayName`: node types are written to disk as their
  *display name*, so renaming one makes every saved graph containing it unreadable — the
  node is dropped on load and its links go with it. `"Array Add"` → `"Array Append"`
  already cost exactly that, and survives only via `kLegacyNodeNames` (`:801`). **If you
  rename a node, add the old string to that table in the same commit.**
- **JSON keys.** `Node::hasArg` (`src/HE_Core/include/HorizonCode/HorizonCode.h:182`) has
  a misleading name — on `EngineCall` nodes it means "isExec". It was deliberately *not*
  renamed in `c993482`, because it is also the JSON key at `HorizonCode.cpp:830` (write)
  and `:911` (read). The double meaning is resolved with local aliases at the use sites
  instead.
- **Persisted config keys.** `src/HE_Editor/EditorApplication.cpp:651–661`: the key
  `"KeepCPUAssetsInfoAcknoleged"` was misspelled. The C++ member was corrected; the read
  path still reads the old spelling first and feeds it in as the default for the new one,
  so users who already dismissed the dialog don't get it back. That fallback is load-
  bearing — the comment says so explicitly.
- **ImGui window titles.** The title string is the docking id in `imgui.ini`. Rename the
  *label* and pin the *id* after `###`: `src/HE_Editor/EditorUI.cpp:1363–1374` renames
  the panel to "Landscape" while keeping `###Quick Settings`, so saved layouts and
  `DockBuilderDockWindow("Quick Settings")` still resolve.
- **Even whitespace.** `src/HE_Core/include/GraphCommon/GraphJson.h:94–101`: some graph
  types dump compact, some pretty, and this is deliberately *not* unified — the
  incremental packer reuses pack entries by hashing the asset blob, so flipping a dump
  style would invalidate every affected asset in every project's pack cache.
- **Shader source strings.** Identifiers inside embedded GLSL/HLSL/MSL are not C++ and
  are not yours to restyle. `HlslSources.h:213–214` declares `ddx_`/`ddy_` inside the
  `kSSAOHLSL` raw string (`:180`) — those are the *only* trailing-underscore
  "members" a naive grep still finds anywhere in the first-party tree, and touching them
  would be editing a shader. Separately, uniform names are string-coupled across the
  host/shader boundary: renaming one means changing the shader text *and* the host lookup
  in the same commit.

**Rule of thumb:** if the string can reach a `.hescene`, `.hasset`, `.hpak`, `.heproj`,
`config.json` or `imgui.ini`, it is a format. Leave it, or migrate it with a compat read.

---

## 8. How to apply a rename safely in this repo

This is the part that repeatedly went wrong. Work the list.

1. **Grep the whole repo, not the module.** Tests, scripts, docs and CMake files bind to
   these names too — `src/HE_Rendering/CMakeLists.txt` and `tests/CMakeLists.txt` both
   list source files by name, so a file rename that stops at `src/` does not build:
   ```sh
   grep -rn '\bOldName\b' src tests scripts docs CopilotDocs \
     --include='*.h' --include='*.cpp' --include='*.mm' \
     --include='*.py' --include='*.md' --include='CMakeLists.txt' \
   | grep -v '/vendor/' | grep -v '/glm/'
   ```
   Two things about that command:
   - **Name the directories; do not grep `.`.** `.claude/worktrees/` holds full checkouts
     of *other branches* (git-excluded, so they are easy to forget). Hits in there are
     stale, must not be edited, and will make a finished rename look half-done — this
     happened while writing this file.
   - In zsh, quote the `--include` globs or the shell tries to expand them.

2. **Never edit vendored trees**, and expect them to pollute your greps with generic
   identifiers: `src/HE_Core/vendor`, `src/HE_Scene/vendor`, `src/HE_Editor/vendor`,
   `src/HE_Tools/vendor`, `src/HE_Rendering/glm`. The `g_` case in §3 is the cautionary
   example — 890 of 890 hits were vendored ImGui.

3. **Search for the name in strings, separately.** `grep -rn '"OldName"'` and check
   whether the hit reaches a file. If it does, go to §7: either don't rename, or rename
   *and* add the compat read in the same commit.

4. **Check whether it is exported.** `HE_API`, `HE_TOOLS_API`, `HE_GAME_API` mean
   out-of-repo game code or tooling may bind to the symbol even when the in-repo call
   sites are all visible. Renaming is then an API break — say so out loud rather than
   assuming the grep was complete.

5. **Syntax-check everything you touched** — do *not* run `cmake --build` or `ctest` for
   this; they serialise on the one shared build tree and take minutes:
   ```sh
   python3 scripts/he_syntax.py <file.cpp> [<file.mm> ...]
   ```
   Every file must print `OK`. A brand-new `.cpp` prints `NO-TARGET` (the build tree
   predates the `CMakeLists` edit) — check those by hand against the owning target's
   `build/.../flags.make`. Headers have no TU: pass a `.cpp` that includes them.

6. **All or nothing.** If a rename turns out larger or riskier than it looked, revert
   *that one rename* and record it as deferred with the reason. A partially applied
   rename — some call sites updated, some not, or the symbol renamed but its serialised
   twin left behind — is the worst possible outcome, worse than not starting.
