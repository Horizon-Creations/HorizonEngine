# Horizon Engine

A cross-platform 3D game engine and editor, written from scratch in C++20. It is
built to power our own game **Catania**, which is why the feature list leans
towards what an actual production needs rather than towards a demo reel.

**Current release: 0.3.0 "Aurora"** — [Downloads](https://horizoncreations.dev/HorizonEngine/HE.html) ·
[Documentation](https://horizoncreations.dev/HorizonEngineDocs/) · [Website](https://horizoncreations.dev)

---

## What it does

**Five rendering backends.** Metal (macOS), OpenGL, Direct3D 11, Direct3D 12 and
Vulkan. Metal and OpenGL are the full-featured pair; the D3D and Vulkan backends
are close behind and gaining the remaining parity work release by release (see
[Status](#status)).

- Physically based shading, a deferred renderer with clustered lighting, and
  cascaded shadow maps plus point/spot shadow atlases
- Procedural sky: atmospheric scattering, day/night, volumetric clouds, moon
  phases, stars, milky way, nebulae and aurora — all driven from the editor
- HDR, bloom, FXAA, SSR, three ambient-occlusion methods (SSAO/HBAO/GTAO) and
  ray-traced global illumination on hardware that supports it
- GPU instancing, LOD, frustum culling, foliage scattering, and terrain with
  chunked LOD and in-editor sculpting

**A real editor.** Scene outliner, inspector, content browser, viewport gizmos,
a profiler, and node-graph editors for materials, particles, animation state
machines and visual scripting. Several people can edit the same scene live over
the network, with an authoritative lock table and per-user undo.

**Four ways to write gameplay.** Visual scripting (HorizonCode), Lua, Python and
native C++ — chosen per project, with the editor adapting to the choice.

**What a shipped game needs.** Jolt-backed physics, skeletal animation and
blending, navmesh generation and agents, audio, particles, an in-game UI widget
system with its own designer, packaging into compressed and optionally encrypted
`.hpak` archives with on-demand streaming, and an export pipeline that produces
a runnable build for Windows, macOS or Linux.

Source control (git + LFS, with GitHub/GitLab/Azure DevOps sign-in) is built
into the editor, so a project can be versioned without leaving it.

## Status

The engine is feature-complete for its planned scope on macOS and Windows, and
in daily use on Catania. It is **not at 1.0**, and the honest gaps are:

| Area | State |
|---|---|
| Metal, OpenGL | Complete |
| Direct3D 11/12, Vulkan | Close to parity — the material node-graph shaders and the newest sky work are still Metal/OpenGL only |
| Linux | Builds and runs, but has had far less real-world use than macOS and Windows |
| Texture compression | ASTC on Apple hardware; BCn encoders are not written yet |

The full plan, including everything already finished, lives in
[`CopilotDocs/MASTERPLAN.md`](CopilotDocs/MASTERPLAN.md).

## Building

You need **CMake 3.21+**, a **C++20 compiler**, and git. Everything else (SDL3,
glm, nlohmann/json, …) is fetched during configure.

```bash
git clone https://github.com/Horizon-Creations/HorizonEngine.git
cd HorizonEngine
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The editor lands in `build/src/HE_Editor/`. Run the test suite with
`./build/tests/he_tests`.

**Building binaries for other machines?** Add `-DHE_PORTABLE_BUILD=ON`. Without
it the texture encoder is compiled for the CPU doing the building, and will die
with an illegal instruction on an older one.

Linux additionally needs the X11/Wayland, OpenGL, audio and udev development
headers — the `Install Linux dependencies` step in
[`.github/workflows/ci.yml`](.github/workflows/ci.yml) has the exact package
list.

## Contributing

Issues and pull requests are welcome. The editor has **Help ▸ Report Issue**,
which fills in your engine version, machine and the relevant part of the log for
you.

Please read [`docs/coding-conventions.md`](docs/coding-conventions.md) before
opening a pull request — the codebase has opinions, about comments in
particular.

## Licence

MIT — see [`LICENSE`](LICENSE).
