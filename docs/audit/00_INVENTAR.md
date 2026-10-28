# Phase 0 – Inventar (Audit-Lauf 2026-10-16, Branch `release/0.7.0`)

Rein lesendes Audit. Es entsteht ausschließlich Dokumentation unter `docs/audit/`.
Annahmen zum Projektkontext (im Prompt waren Platzhalter): **C++20, CMake, Windows/macOS/Linux, fünf+1 Render-Backends** (Metal, OpenGL, Vulkan, D3D11, D3D12, Software), entt-ECS, Jolt-Physik, ImGui-Editor.

## Modulliste und Prüfreihenfolge

LOC = Zeilen in `.cpp/.h/.hpp/.mm/.inl/.vert/.frag` ohne `vendor/`, `glm/`, `glad/`. Die Aufteilung in Teilstücke ("Slices") steht in `_slices/<Modul>.txt` (exakte Dateilisten), die Teilberichte in `_parts/`.

| # | Modul | Pfad | Dateien | LOC | Hängt ab von | Slices |
|---|---|---|---|---|---|---|
| 1 | **Core** (HorizonCore: Application/Window/Input, ContentManager/Hpak, UIWidget, HorizonCode, MaterialGraph, ParticleGraph, Physics, Audio, Scripting, JobSystem, Math, Project, Crypto) | `src/HE_Core` | 167 | 75 159 | he_crypto (extern: Jolt, miniaudio, …) | 8 |
| 2 | **Net** (Sockets, Kollaborations-Transport) | `src/HE_Net` | 42 | 17 339 | Core | 2 |
| 3 | **SourceControl** (git/LFS-Wrapper, GitService) | `src/HE_SourceControl` | 24 | 7 867 | Core, Net | 1 |
| 4 | **ContentSync** (EngineContent-SFTP) | `src/HE_ContentSync` | 15 | 1 922 | Core, Net | 1 |
| 5 | **SelfUpdate** (Updater, Editor-Selbstupdate) | `src/HE_SelfUpdate` | 21 | 3 335 | Core, ContentSync | 1 |
| 6 | **Python** (Scripting-Host) | `src/HE_Python` | 2 | 1 821 | Core, Scene | 1 |
| 7 | **Scene** (HorizonWorld/entt, Components, Systeme, SceneSerializer, EngineApi) | `src/HE_Scene` | 226 | 72 590 | Core | 8 |
| 8 | **Rendering** (Frontend + 6 Backends, RenderGraph, Culling, Sky, Material-Shader) | `src/HE_Rendering` | ≈180 (ohne glm/glad) | 93 291 | Core, Scene | 10 |
| 9 | **Tools** (Importer, asset_compiler, hpak_packer, hc_codegen, *_gen) | `src/HE_Tools` | 44 | 13 815 | Core, Scene | 2 |
| 10 | **Game** (Player-Executable) | `src/HE_Game` | 9 | 4 641 | Core, Scene, Rendering | 1 |
| 11 | **Editor** (ImGui-Editor, MCP-Bridge) | `src/HE_Editor` | 368 | 186 150 | alles | 19 |
| – | Tests (`tests/`, 311 Dateien, ≈229 k LOC, ein doctest-Binary) | `tests/` | 311 | ≈229 500 | alle | **nicht auditiert** (nur als Verifikationsnetz referenziert) |
| – | Build-System (`CMakeLists.txt`, `cmake/`, `scripts/`) | root | – | – | – | Querschnitt, in AUDIT.md |

Abhängigkeitsrichtung (aus `target_link_libraries`): `Core → {Net → {SourceControl, ContentSync → SelfUpdate}}`, `Core → Scene → Rendering → Game`, `Core/Scene → Python`, `Core/Scene → Tools`, `Editor → alles`.
Vendored und **nicht** auditiert: `src/HE_Core/vendor`, `src/HE_Rendering/glm`, `src/HE_Rendering/glad`, EditorDeps, ImGui/entt/etc. (FetchContent).

## Prüfreihenfolge
Core → Net → SourceControl → ContentSync → SelfUpdate → Python → Scene → Rendering → Tools → Game → Editor → Gesamtauswertung (`AUDIT.md`).

## Vorgehen / Einschränkungen
* Pro Slice liest ein Subagent alle Dateien vollständig; Ergebnis → `_parts/<Modul>_<n>.md`; ich fasse sie zu `modul_<name>.md` zusammen.
* Es wurde nichts gebaut oder profiliert: Aussagen zu Laufzeit sind Schätzungen oder als "unsicher / messen" markiert.
