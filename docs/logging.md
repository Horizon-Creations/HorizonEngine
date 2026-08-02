# Engine Logging

One log, one file, every subsystem. `HorizonEngine.log` next to the executable is
meant to be enough to reconstruct a run — what was loaded, what was compiled,
what failed, and on which frame and thread.

## Writing a log record

```cpp
#include <Diagnostics/Log.h>

HE_LOG_INFO (Render,  "Backend '%s' initialised (%ux%u)", name, w, h);
HE_LOG_WARN (Physics, "Entity %u has no collider — falling back to the transform box", id);
HE_LOG_ERROR(Asset,   "Cannot load '%s': %s", path.c_str(), err.c_str());
```

Levels: `HE_LOG_TRACE`, `HE_LOG_DEBUG`, `HE_LOG_INFO`, `HE_LOG_WARN`,
`HE_LOG_ERROR`, `HE_LOG_CRIT`. Formatting is printf-style and checked at compile
time on Clang/GCC.

The macros test the filter **before** formatting, so a `HE_LOG_TRACE` left in a
hot path costs one predictable branch. Leave verbose logging in the code; do not
delete it to save time.

A record looks like this:

```
[14:22:07.318] [ INFO] [Render     ] [Main    ] [f    120] Metal device: Apple M2 Pro
[14:22:07.402] [ WARN] [Asset      ] [Worker-0] [f    124] Mesh 'Rock' has no tangents  (MeshImporter.cpp:214)
```

Time · level · category · thread · frame · message · source location. Source
location is appended from Warning upwards by default
(`HE::Log::setSourceLocationLevel`).

## Categories

`HE::Log::Cat` is a fixed enum (`Core`, `Config`, `Platform`, `Window`, `Input`,
`Job`, `Memory`, `Asset`, `Content`, `Pak`, `Export`, `Render`, `RHI`, `Shader`,
`Material`, `Scene`, `World`, `Serialize`, `Physics`, `Collision`, `Animation`,
`Nav`, `Audio`, `Particle`, `Terrain`, `Foliage`, `LOD`, `Weather`, `UI`,
`Widget`, `Script`, `Lua`, `Python`, `HorizonCode`, `GameLogic`, `Net`, `Editor`,
`Tool`, `Profiler`). Adding one means adding an enumerator before `Count` and a
name in `kCategoryNames` — the `static_assert` in `Log.cpp` keeps the two aligned.

An enum rather than registered globals: filtering is an array lookup and the set
is shared across every DLL without exported data, which matters because
`HorizonScene` deliberately never uses `HE_API`.

## Filtering

Every category has its own verbosity; the default is `Info`. `Level::Off`
silences a category completely.

```bash
# One subsystem, one run — no rebuild, no config edit.
HE_LOG=Physics=Trace,RHI=Debug ./HorizonEngine

# Quiet everything except what you are chasing.
HE_LOG=*=Warning,Nav=Trace ./HorizonEngine

# Suppress the console mirror (the file still gets everything).
HE_LOG_NO_CONSOLE=1 ./HorizonEngine
```

The same syntax persists in `config.json` under `logVerbosity`. Precedence is
default → `config.json` → `HE_LOG`, so the environment always wins.

In code: `HE::Log::setVerbosity(Cat::Physics, Level::Trace)` or
`HE::Log::configureFromString("Physics=Trace")`.

## Keeping per-frame code loggable

Per-frame paths need logging most and can afford it least. Three throttles, each
per call site:

```cpp
HE_LOG_ONCE    (Asset, Warning, "Mesh '%s' has no tangents", name);   // once per process
HE_LOG_EVERY_N (Render, Debug, 100, "Draw %d", drawIndex);            // 1st, 101st, 201st …
HE_LOG_THROTTLE(Nav, Warning, 5.0, "Entity %u has no path", id);      // at most every 5 s
```

On top of that, identical consecutive records are collapsed automatically into a
`(previous message repeated N more times)` note, so an error firing every frame
costs two lines, not thousands.

## Scope timing

```cpp
HE_LOG_SLOW_SCOPE(Physics, 8.0, "PhysicsWorld::step");  // silent unless it overruns 8 ms
HE_LOG_SCOPE(Serialize, "Scene load");                  // logs entry + exit with duration
```

`HE_LOG_SLOW_SCOPE` is the one to reach for in per-frame systems: silent when
healthy, loud exactly when something stalls. The main loop has its own hitch
detector (`Application::kHitchSeconds`) for frames that blow past their budget.

## Guard clauses

```cpp
if (!HE_CHECK(asset, Asset, "Script asset %s not found", idStr)) return;
```

Evaluates to the condition and logs when it is false — for the early returns that
used to fail silently.

## Consumers

- **File** — `<exeDir>/HorizonEngine.log`, flushed per record. The three previous
  runs are rotated to `HorizonEngine.1/2/3.log`, so the log of the run that
  crashed survives the restart.
- **Console** — stdout, with Warning and above on stderr and ANSI colour when the
  stream is a terminal. Off in the test runner unless `HE_TEST_LOG_CONSOLE=1`.
- **Ring buffer** — the last 512 formatted lines in a fixed, preallocated buffer.
  `CrashHandler` appends them to the `.crash` report, so a crash report says what
  the engine was doing on the way down, not just where it died.
- **Sinks** — `HE::Log::addSink(fn, user)` for anything else (the editor's
  post-PIE report uses one). Sinks run on the logging thread with the log mutex
  held: keep them short and never log from inside one.

## Startup and shutdown

`HE::Log::logStartupBanner()` writes the header every log starts with — build
configuration, compiler, OS, CPU, RAM, pid, executable, working directory,
command line and the active log filter. `logShutdownSummary()` closes with
uptime, frame count and the warning/error/critical totals for the run.

## Legacy call sites

`Logger::Log(level, msg)` still works and forwards into `HE::Log` (category
`Core`, no source location). `Logger::LogTo(cat, level, msg)` adds a category.
New code should use the macros.
