# In-Engine Performance-Profiler (Selbstdiagnose) — Design

> Stand: 2026-06-24 · Ziel: ein **startbares/stoppbares** Laufzeit-Profiling-Tool, das aus möglichst vielen Systemen genaue CPU/GPU-Metriken erfasst und als strukturierten Dump in `<deploy>/dumps/` schreibt. Grundlage für die spätere Himmel/Wetter-Optimierung (man optimiert gegen echte Messungen, nicht Schätzungen).

## Designprinzipien
1. **Zero-cost wenn aus.** Nicht-aufnehmend = ein `bool`-Check pro Scope. Keine Allokationen, keine I/O.
2. **In HE_Core/Diagnostics**, kein neues Modul → keine DLL-Zirkel; nutzbar von Editor *und* Shipped-Game.
3. **Backend-agnostischer Kern** (CPU + Zähler). GPU-Timing pro Backend, mit ehrlichem Fallback (Metal zuerst — Dev-Plattform; D3D/Vulkan „blind", später auf echter HW).
4. **Koexistenz mit Tracy.** Bestehende `HE_PROFILE_*`-Makros bleiben; der Laufzeit-Profiler ist unabhängig von `TRACY_ENABLE`.
5. **Single-Writer-Scope-Modell, NICHT single-thread.** Frame-Schleife + entt-Systeme laufen seriell auf dem Main-Thread, ABER der Renderer nutzt einen JobSystem-ThreadPool (FrustumCuller/RenderExtractor `parallel_for`), und dessen Worker-Loop wickelt jede Task in `HE_PROFILE_SCOPE_N("Job::Execute")`. Scopes werden also auf Worker-Threads geöffnet. → `begin/endScope` zeichnen **nur vom Capture-Thread** (Main) auf und sind auf jedem anderen Thread No-op; `m_recording` ist `std::atomic<bool>`. Sonst korrumpieren gleichzeitige `push_back` auf den geteilten Scope-Stack den Heap (real beobachteter Crash, s. u.). Tracy-Zonen bleiben thread-aware. (Diese „single-threaded by design"-Annahme war ursprünglich FALSCH und wurde durch einen Laufzeit-Crash widerlegt.)

## Komponenten

### 1. `EngineProfiler` (neu: `HE_Core/include/Diagnostics/EngineProfiler.h` + `src/.../EngineProfiler.cpp`)
Singleton (`EngineProfiler::instance()`), `HE_API`-exportiert. Datenmodell:
```
struct ScopeSample { const char* name; double ms; uint32_t depth; };  // CPU, verschachtelt
struct GpuPass     { const char* name; double ms; };                  // GPU pro Pass
struct RenderStats { uint32_t drawCalls, triangles, visibleObjects, totalObjects,
                     entities, lights, particles, gpuParticles;
                     double vramUsedMB, vramBudgetMB; };
struct FrameRecord { uint64_t index; double wallMs, cpuFrameMs, deltaMs;
                     double gpuFrameMs = -1.0;          // Σ Pässe bzw. cmdbuf-Total; -1 unbekannt
                     std::vector<GpuPass> gpuPasses;    // ← KERN: per-Pass-GPU-Zeit (Sky/Clouds/Post/IBL)
                     RenderStats stats; std::vector<ScopeSample> scopes; };
```
> **Warum per-Pass-GPU zwingend (Advisor):** Der Profiler existiert, um die **GPU-gebundenen** Sky/Wolken-Kosten zu finden. CPU-Scopes takten den Himmel mit ~µs („ein Fullscreen-Triangle zeichnen"), die echten Millisekunden liegen im Fragment-Shader. Ein einzelner `gpuFrameMs`-Skalar beantwortet „ist der Frame langsam?", nicht „**was** ist langsam?". Darum `vector<GpuPass>`, nicht ein Skalar.

API:
- `start(const SessionInfo&, size_t maxFrames = 0)` — `0` = wachsend bis Stop, sonst Ringpuffer. Legt `dumps/` an. `SessionInfo{backend, gpuName, os, width, height, vsync, captureVsyncOff}` → in den Dump-Metadaten.
- `std::string stopAndDump()` — schreibt JSON nach `<dumps>/profile_<YYYYMMDD_HHMMSS>.json`, gibt Pfad zurück.
- `requestStart()/requestStop()/requestToggle()` — vom Hotkey/UI **mitten im Frame** aufrufbar; die Umschaltung wird **erst am nächsten `beginFrame` angewandt** (Frame ist immer ganz oder gar nicht aufgenommen → keine Stack-Imbalance).
- `bool isRecording()`
- `beginFrame(double deltaMs)` / `endFrame()` — Rahmen; wendet zuerst pending Start/Stop an; `endFrame` finalisiert `cpuFrameMs`, schiebt Record.
- `beginScope(const char*)` / `endScope()` — verschachtelt via Stack; misst inklusive Zeit, Tiefe getaggt.
- `setRenderStats(const RenderStats&)` / `setGpuFrame(double total, std::vector<GpuPass>)` — Zähler/GPU für den laufenden (bzw. via Frame-Index zugeordneten) Frame.
- `const FrameRecord* lastFrame()` / `std::vector<FrameRecord> snapshot()` — für die Live-UI.
- `recordedFrames()`, `dumpsDir()`.

Timing: `SDL_GetTicksNS()` (engine-konsistent, ns-genau). Kein `std::chrono`.
Dump-Format: nlohmann::json (`j.dump(2)`), Metadaten (Engine/OS/RHI/GPU-Name, Start-/Endzeit, Framezahl, Auflösung), `frames[]`, plus `summary` = pro Scope-Name min/avg/max/count über das Fenster.

### 2. `GlobalState::getDumpsDir()` (neu)
Spiegelt `setLogFile`: `fs::path(startupPath).parent_path() / "dumps"`, `create_directories`. Selbe Deploy-Wurzel wie `HorizonEngine.log` → robust, nicht CWD-abhängig.

### 3. `Profiler.h` — RAII + Makros
`Profiler.h` bekommt eine leichte `ProfileScope`-RAII-Klasse (global, `HE_API`) und kombiniert die Makros:
- `HE_PROFILE_SCOPE_N(name)` → (Tracy-Zone falls an) **und** `ProfileScope{name}`.
- `HE_PROFILE_SCOPE()` → `ProfileScope{__FUNCTION__}`.
- `HE_PROFILE_FRAME()` → bleibt Tracy-`FrameMark` (Frame-Rahmen kommt explizit aus `Application::Run`).
`ProfileScope` **latcht den Zustand**: ctor speichert `m_opened = isRecording()` und ruft dann ggf. `beginScope`; dtor ruft `endScope` **nur wenn `m_opened`**. So kann ein F9-Toggle mitten in einem Scope nie eine Stack-Imbalance (beginScope ohne endScope) erzeugen. Bestehende Tests (`test_profiler.cpp`) bleiben grün (Scopes sind No-ops ohne Aufnahme).

### 4. Frame-Schleife — `Application::Run` (`Application.cpp:113–152`)
- `beginFrame(dt*1000)` am Schleifenanfang (wendet pending Start/Stop an); `endFrame()` vor `}`.
- Scopes: `PollEvents`, `OnRender`, `Render` (um `m_renderer->Render()`), `GameLogicTick` (um `m_loop.tick`), `SwapBuffers`.
- Nach `Render()`: GPU-Stats vom Renderer ziehen (`m_renderer->GetFrameGpuStats()` → `gpuFrameMs` + `passes[]`) → `setRenderStats` + `setGpuFrame`. GPU-Zeiten sind 1–N Frames versetzt → Zuordnung per Frame-Index, nicht Wall-Clock.
- **Hotkey** (cross-build): F9 = `Application::toggleProfilerCapture()` (Stop → Dump + Log-Zeile mit Pfad). Im Event-Callback der Base-`Application`. Der Toggle ist dort **ausgeschrieben** (`isRecordingOrPending()` → `requestStop()`, sonst `requestStart()`) statt `requestToggle()` zu rufen, weil derselbe Pfad zusätzlich den VSync-Zustand sichern und beim Stop wiederherstellen muss (nächster Punkt).
- **Benchmark-Capture / VSync (Advisor):** VSync blockiert `SwapBuffers` auf die Bildwiederholrate → CPU-Frametime/FPS sind bei VSync an **bedeutungslos** (immer 8,3/16,6 ms, egal wie teuer der Himmel). Beim Start einer Aufnahme optional **VSync aus** (`captureVsyncOff`, Default an für den Benchmark), alten Zustand merken, beim Stop wiederherstellen. Der tatsächliche VSync-Zustand wandert in die Dump-Metadaten, damit ein gecappter Lauf nicht als „langsam" fehlgelesen wird. (GPU-Timestamps sind VSync-immun — zweiter Grund, warum Punkt 6 die verlässlichen Zahlen liefert.)

### 5. Szene-Systeme — `SceneSystems::tick` (`SceneSystems.cpp:59–82`)
Je ein `HE_PROFILE_SCOPE_N` pro System: `Terrain`, `Animation`, `AnimationBlend`, `AnimationStateMachine`, `PropertyAnimation`, `Navigation`, `Weather`, `GpuParticleParams`, `ParticleSystem`, `Foliage`, `LOD`. **Breite zählt (User-Vorgabe „möglichst viele Systeme", ultracode):** Physics-Step, Lua-`onUpdate`, Audio-Spatial-Update und Collision-Dispatch sind **nicht optional** — je ein Scope in `EditorApplication::OnRender` (bzw. `GameApplication`), da sie reale Frame-Kosten tragen und CPU-Timing dort seinen Wert beweist. Async-Streaming/ContentManager-I/O: In-Flight-Zähler als RenderStats-Feld.

### 6. Renderer-GPU-Stats — `IRenderer` (`HE_Core/include/Renderer/IRenderer.h`)
Neu (per-Pass-GPU im Modell, nicht nur ein Skalar):
```
struct GpuPassTime  { const char* name; double ms; };   // name = statisches Literal
struct FrameGpuStats {
    double gpuFrameMs = -1.0;                 // cmdbuf-Total bzw. Σ Pässe; -1 unbekannt
    std::vector<GpuPassTime> passes;          // ← per-Pass (Shadow/SSAO/Scene=Sky+Clouds/Bloom/Tonemap/Fxaa)
    uint32_t drawCalls=0, triangles=0, visibleObjects=0, totalObjects=0;
    double vramUsedMB=0, vramBudgetMB=0;
};
virtual FrameGpuStats GetFrameGpuStats() const { return {}; }
```
- **Metal (v1-Pflicht, per-Pass):** `MTLCounterSampleBuffer` an `sampleBufferAttachments` jeder Encode-Pass (EncodeShadowMap/SSAO/Scene/Bloom/Tonemap/Fxaa), gated auf `device.supportsCounterSampling`; Auflösung im `addCompletedHandler`, 1 Frame versetzt (Ring + Frame-Index-Zuordnung). Fallback (kein Counter-Support): `GPUEndTime-GPUStartTime` als `gpuFrameMs`, leere `passes`. In leerer Welt ist **`EncodeScene ≈ Sky+Wolken`** — exakt die Baseline, an der der frühere Sky-Plan hing. (Wolken-innerhalb-`sky.frag` ist ein feinerer Timer für später; per-Encode-Pass ist die v1-Granularität.)
- **OpenGL**: `glQueryCounter(GL_TIMESTAMP)` pro Pass; auf macOS unzuverlässig → Warn-Log, `gpuFrameMs=-1` + leere `passes` als ehrlicher Default zunächst. Draw/Tri-Zähler trotzdem.
- **D3D11/D3D12/Vulkan**: Default `{}` (−1), Schnittstelle steht; echte Timestamp-Queries (Disjoint / QueryHeap+Resolve / vkCmdWriteTimestamp+timestampPeriod) später auf realer HW (blind-Windows-Constraint).

### 7. Editor-UI — Profiler-Panel (`EditorUI.cpp`, `HE_IMGUI_ENABLED`)
`DrawProfilerWindow(bool& open)` (Stil wie `DrawPreferencesWindow`): Start/Stop-Button, Live-Frame-Balken (CPU-Gesamt, GPU-Gesamt), pro-Scope-Balken des letzten Frames (Tiefe eingerückt), Zähler (Draw/Tris/VRAM/Entities), „Dump"-Button + Pfadanzeige, „Open Dumps Folder". Liest `EngineProfiler::instance()` direkt — keine `AppContext`-Änderung nötig. Toggle über View-Menü + F9.

### 8. Tests (`tests/test_profiler.cpp` erweitern, doctest)
- Start → N×(beginFrame/Scopes/endFrame) → stopAndDump: Datei existiert, JSON parsebar, Framezahl stimmt, Scope-Summary plausibel.
- Zero-cost-Pfad: Scopes ohne `start()` ändern keinen State.
- Ring-Modus: `maxFrames` begrenzt die gehaltenen Records.
- Bestehende Makro-Kompiliertests bleiben.

## Reihenfolge der Umsetzung
1. `EngineProfiler` + `getDumpsDir()` + `Profiler.h`-Makros + CMake-Eintrag. → kompiliert, Unit-Tests grün.
2. Frame-Schleife + SceneSystems-Instrumentierung + F9-Hotkey. → erzeugt echte Dumps.
3. `IRenderer::GetFrameGpuStats` + Metal-Impl + Draw/Tri-Zähler.
4. Editor-Panel.
5. Tests + `he_tests` grün + ein realer Dump zur Sichtprüfung.

## Umsetzungsstatus (v1, 2026-06-24)
**Implementiert & getestet (473/473 Tests grün, alle Backends gebaut):**
- `EngineProfiler` (HE_Core/Diagnostics) — CPU-Scopes, Ring/Wachstum, Start/Stop (pending→beginFrame), JSON-Dump mit Summary (min/avg/max je Scope+GPU-Pass). 4 neue doctest-Fälle.
- `GlobalState::getDumpsDir()` → `<exeDir>/dumps` (verifiziert: spiegelt Log-Pfad).
- `Profiler.h`-Makros (`HE_PROFILE_SCOPE/_N`) treiben Laufzeit-Profiler + Tracy; `ProfileScope` latcht `m_opened`.
- Frame-Schleife instrumentiert (`Application::Run`): PollEvents/OnRender/Render/GameLogicTick/SwapBuffers + F9-Hotkey (vsync-off-Benchmark, Dump+Restore) + GPU-Stats-Pull.
- `SceneSystems::tick` — 11 per-System-Scopes. Editor-Play-Mode + Game: SceneSystemsTick/PhysicsStep/AudioSpatial/ScriptUpdate/CollisionDispatch/EnvironmentPush.
- `IRenderer::GetFrameGpuStats()` + **Metal GPU-Timing, vsync-immun:**
  - **Per-Pass** (`MTLCounterSampleBuffer`, Stage-Boundary): **Scene** (= Sky+Wolken+Opaque+Skinned+Partikel+Debug), **Tonemap**, **Present** (FXAA+UI+ImGui). Capability-gated (`supportsCounterSampling`, macOS 11+), nur während Aufnahme aktiv, CPU/GPU-Timestamp-Korrelation pro Frame, async im completion-handler, by-value/shared_ptr gegen UAF. **In leerer Welt isoliert „Scene" die Sky/Wolken-Kosten von Post-FX — genau die Stufe-0-Baseline des Sky-Plans.**
  - **Whole-Frame-Fallback** (`GPUStartTime/EndTime`) wenn Counter-Sampling fehlt.
  - Capture via F9 auf 20000 Frames (Ring) gedeckelt; Counter-Set wird in Shutdown freigegeben.
- Editor-Panel „Performance Profiler" (View-Menü): Start/Stop, Live-CPU/GPU-Breakdown-Balken (inkl. per-Pass-GPU), Zähler, Dump-Now, Open-Folder.

**Bekannte Grenzen / nächste Schritte:**
- Metal per-Pass deckt **Scene/Tonemap/Present** ab; **SSAO & Bloom** (eigene Encoder in Helfern) sind noch nicht einzeln getimt → stecken in der Differenz `gpuFrameMs − Σ Pässe`. Feinere Timer (z. B. Wolken-innerhalb-`sky.frag`, SSAO, Bloom) als Folgeschritt.
- **GL/D3D/Vulkan GPU-Timing**: Interface steht, Default −1. GL-macOS unzuverlässig; D3D/Vulkan blind-Windows.
- Draw/Tri/VRAM/Entity-Zähler: Felder vorhanden, Befüllung pro Backend folgt.

**Laufzeit-Verifikation OFFEN (Sandbox ohne Display):** Build + 473 Unit-Tests grün beweisen NICHT die laufende App, und die per-Pass-GPU-Zahlen (Korrelation, Counter-Resolve) konnten hier NICHT auf echter HW geprüft werden. User-Smoke-Test nötig: Editor starten → View ▸ Performance Profiler → F9 → ein paar Sekunden bewegen → F9 → prüfen, dass `out/deploy/Editor/dumps/profile_*.json` erscheint, öffnet, nonzero `cpuMs` + `gpuMs` hat UND `gpu`-Pässe (Scene/Tonemap/Present) mit plausiblen ms (Σ Pässe ≲ gpuFrameMs). Im Log erscheint „Metal: per-pass GPU timing enabled" wenn Counter-Sampling läuft.

## Erweiterung v2 (2026-06-25) — feinere Sub-Frame-Granularität

> Ziel (User): „sub-frame infos wie lange jedes Element der Welt gebraucht hat zu rendern". v1 timte den Frame in **3** GPU-Pässe (Scene/Tonemap/Present) und ließ die Render-Zähler leer. v2 schließt beides.

**1. Mehr exakte per-Encoder-GPU-Pässe (Metal, stage-boundary).** Shadow, SSAO und Bloom sind eigene Encoder und werden jetzt einzeln getimt — von 3 → **6** ehrliche GPU-Buckets: `Shadow`, `SSAO`, `Scene`, `Bloom`, `Tonemap`, `Present`. SSAO/Bloom sind Multi-Encoder (Pos/Occlusion/Blur bzw. Bright + 10 Blur) → Start-Sample am ersten, End-Sample am letzten Sub-Encoder spannt das ganze Feature. Diese Zahlen sind **exakt** (Encoder-GPU-Span) und summieren zum Frame.

**2. Dynamische Slot-Allokation.** Das hartkodierte `sampleCount = 6` ist weg. Ein über-allokierter Sample-Buffer (`kMaxGpuSamples = 32`) wird pro Frame angelegt; jeder Pass holt sich Slots zur Encode-Zeit (`ftPair`/`ftPoint`), der Completion-Handler resolved nur `[0, next)`. Bedingte Pässe (Shadow nur mit Schatten, SSAO/Bloom nur wenn an) belegen nur Slots, wenn sie wirklich encodieren.

**3. Intra-Scene-Element-Split (Metal, draw-boundary).** Der „Scene"-Encoder wird via `sampleCountersInBuffer:atSampleIndex:withBarrier:` an Element-Grenzen abgetastet: `Opaque`, `Skinned`, `Sky+Clouds`, `Transparent`, `Particles`, `Debug`. Element[i] = sample[i] − sample[i−1]. **Gated** auf die separate Capability `MTLCounterSamplingPointAtDrawBoundary` (≠ stage-boundary); fehlt sie, bleibt es bei dem einen exakten „Scene"-Pass. Der authoritative `Scene`-Gesamtwert kommt weiterhin vom Stage-Boundary-Paar — der Split ist ein **Breakdown davon**, nicht additiv.
> **TBDR-Caveat (Advisor):** Apple-Silicon ist Tile-Deferred — Fragment-Arbeit (genau die teuren Sky/Wolken-ms) läuft tile-verzögert, **nicht** in Draw-Submit-Reihenfolge. Draw-Boundary-Deltas mit `withBarrier:NO` sind daher **Näherungen**, keine exakten Pass-Kosten (ein `withBarrier:YES` würde serialisieren und genau das Gemessene verfälschen). Darum tragen alle Intra-Scene-Einträge das Flag **`approx`** — im Dump (`"approx": true` pro Frame + im Summary) und im Editor (eingerückt, gedimmt, mit „~"). **HW-Verifikation gegen Metal System Trace steht aus**, bevor diese Zahlen als Grundwahrheit gelten.

**4. Render-Zähler befüllt (Metal + GL).** `drawCalls`/`triangles`/`visibleObjects`/`totalObjects` waren immer 0; jetzt an den echten Draw-Sites gezählt (opaque + transparent + skinned; instanced GL = 1 Draw, Tris × Instanzen). `visible/total` = gecullte vs. extrahierte statische Objekte. Zähler sind **current-frame** (Main-Thread, in `GetFrameGpuStats` mit den 1–2 Frames verspäteten GPU-Zeiten gemischt). GL liefert die Zähler trotz `gpuFrameMs = −1` (macOS-GL-Timestamps bleiben bewusst aus).

**Warum kein per-Objekt-GPU-Timing?** Nicht praktikabel und auf TBDR nicht einmal sinnvoll: ein Timestamp pro Draw-Call ist zu teuer und überlappende, tile-verzögerte GPU-Arbeit macht per-Draw-Deltas ohne Barrieren bedeutungslos. Die plausible Lesart von „jedes Element der Welt" ist **per-Kategorie/per-Pass** — das ist das Deliverable. Per-Objekt-Heatmaps bräuchten einen anderen Mechanismus (z. B. Visibility-Buffer-Statistik), nicht Timestamps.

**Status v2:** Build grün (alle Backends), 473 Tests grün (Profiler-Test deckt visible/total + `approx`-Round-Trip ab, läuft order-unabhängig). Adversariale Diff-Review (4 Dimensionen, je verifiziert) → 2 low-sev-Befunde gefunden+gefixt (Zähler-Reset vor Early-Returns; order-unabhängiger Test).

**v2.1 Hotfix (Laufzeit-Crash gefunden + behoben):** Erster echter F9-Lauf crashte sofort (`malloc: pointer being freed was not allocated`, SIGABRT). Ursache: der JobSystem-ThreadPool wickelt jede Task in `HE_PROFILE_SCOPE_N("Job::Execute")` (`JobSystem.cpp`), also riefen mehrere Worker-Threads `EngineProfiler::beginScope` gleichzeitig auf dem geteilten `m_stack` auf → Heap-Korruption. Fix: `begin/endScope` zeichnen nur vom Capture-Thread auf (`m_captureThread` + `std::atomic<bool> m_recording`), Worker-Scopes sind No-op. Regressionstest (`test_profiler.cpp`: 8 Threads × 5000 Scopes während Aufnahme) deckt es ab; 474 Tests grün. **Nebenbefund — F9 auf macOS:** F9 ist standardmäßig eine Medientaste → die App sieht den Tastendruck nur mit **fn+F9**; alternativ die Editor-Panel-Buttons (View ▸ Performance Profiler) nutzen, die keinen Hotkey brauchen.

**Real-HW-Capture (2026-06-25, Mac17,4 Apple Silicon, 4102 Frames @ 2520×1324, vsync off) — Ergebnisse:**
- ✅ **Kein Crash, Dump geschrieben, Thread-Fix bestätigt** (nur Main-Thread-CPU-Scopes, kein „Job::Execute").
- ✅ **CPU-Scopes + Zähler verlässlich** (`tris≈522k`, draws/visible/total korrekt). **Whole-Frame `gpuFrameMs` verlässlich** (Median 17,8ms). GPU-bound-Verdikt klar: GPU 17,8ms > CPU 9,4ms Median. (`deltaMs` 12,6 < `gpuFrameMs` 17,8 ⇒ GPU-Arbeit pipelined über mehrere Frames — beide Zahlen unterschiedlich lesen.)
- ⚠️ **Intra-Scene-Draw-Boundary-Split NICHT aktiv:** diese GPU meldet `supportsCounterSampling:MTLCounterSamplingPointAtDrawBoundary == false`. Fallback griff sauber (nur die 6 Stage-Pässe, kein Crash) — aber das per-Element-Headline-Feature liefert auf dieser HW keine Daten. (Startup-Log bestätigt: „stage-boundary only".)
- 🔴 **Per-Encoder-Stage-Zeiten sind auf TBDR NICHT verlässlich/additiv:** Σ(Pässe) ≈ **3,07× gpuFrameMs**; einzelne Werte physikalisch unplausibel (Tonemap/Present je ~23ms in einem 25,8ms-Frame). Ursache: tile-deferred Fragment-Arbeit drainiert gebündelt ans Frame-Ende → jedes `[startVertex,endFragment]`-Fenster dehnt sich auf ≈ ganzen Frame, Fenster verschachteln statt zu kacheln. **Nicht durch andere Sample-Punkte rettbar** (architektonisch). → Dump kennzeichnet `summary.gpuPassesOverlap=true` + `gpuPassNote`; Editor warnt statt zu summieren. **Verlässliche Per-Pass-GPU-Zeiten auf Apple Silicon: Xcode Metal System Trace** (Option C, empfohlen), oder ein in-Engine „serialized detailed capture" (Option B, 1 Command-Buffer/Pass, perturbiert Perf) — offene Produktentscheidung.
- Per-Encoder-Plumbing bleibt erhalten (korrekt; liefert auf D3D/Vulkan-Timestamp-Queries bzw. künftigen Apple-GPUs mit Draw-Boundary echte Zahlen).

> **✅ VERIFIZIERT (echte HW, 2026-06-25, Dump 18:28):** `gpuTimingModes={detailed:1743}`, **Σ/gpuMs = 1.000**, Tonemap 0.18ms / Present 0.47ms (vorher 10–23ms Overlap-Müll). Per-Pass-GPU-Timing in-Engine ist auf Apple Silicon jetzt verlässlich + exklusiv. Metal-Teil abgeschlossen. Cross-Backend (D3D/Vulkan/GL) ist der nächste Schritt — **Constraint: auf einem Mac nicht verifizierbar** (kein Windows; macOS-GL hat dasselbe TBDR-Overlap).

**v2.2 / v2.3 — verlässliches Per-Pass-GPU in-Engine („Detailed Capture", 2026-06-25):** Reaktion auf den TBDR-Befund (Counter-Spans überlappen ~3×). Neuer Modus: jeder Render-Pass wird als **eigener Command-Buffer** committet (Shadow/SSAO/Scene/Bloom/Tonemap/Present = 6); dessen `GPUStartTime/GPUEndTime` liefern **exklusive, additive** Per-Pass-GPU-Zeit.
> **v2.3-Korrektur (empirisch):** Die v2.2-Annahme „Cross-Cmdbuf-Hazard-Tracking serialisiert die Pässe" war FALSCH — Command-Buffer in Commit-Reihenfolge **überlappen** trotzdem auf der GPU-Timeline (echter Dump: Σ weiterhin ~3×). Fix: `[cmdBuf waitUntilCompleted]` zwischen den Pässen erzwingt Nicht-Überlappung **garantiert** (CPU blockiert bis GPU fertig, bevor der nächste Pass startet → `GPUStart(N+1) > GPUEnd(N)`). Detailed-Total = **Σ(Pässe)** (nicht Span — der enthielte die CPU-Stall-Lücken). Capture wird dadurch langsam (6 CPU↔GPU-Roundtrips/Frame) — akzeptabel für ein Diagnose-Capture.
> **Executed-Mode-Marker (Advisor):** jeder publizierende Pfad stempelt `FrameGpuStats.gpuTimingMode` = „detailed"/„counter"/„whole-frame" → Dump-Feld `frames[].gpuMode` + `summary.gpuTimingModes` (Frames pro Modus) + einmaliges Log „detailed GPU capture ENGAGED". Sagt, **was lief**, nicht was die Checkbox wollte (ein Request-Flag verfehlt einen Engage-Bug). Damit ist der nächste Dump eindeutig: `detailed` + Σ≈gpuMs → funktioniert; `counter` trotz Checkbox → Engage-Bug; `detailed` + Σ~3× → waitUntilCompleted half nicht (quasi unmöglich).
Umsetzung:
- `EngineProfiler::setDetailedGpuCapture(bool)` (atomic) + Editor-Checkbox „Detailed GPU pass timing (serializes GPU — capture only)". **Opt-in**, weil es die GPU serialisiert → Frame-Pacing/FPS während der Aufnahme bedeutungslos; die Per-Pass-Zahlen sind Kosten **unter Serialisierung** (verlässliches Ranking + Obergrenze), nicht die Shipping-Single-Cmdbuf-Kosten.
- `MetalRenderer::EncodeFrame`: `flushPass()`-Grenzen committen je einen Cmdbuf + hängen einen Timing-Handler an; im Normalmodus **No-op** → schneller Single-Cmdbuf-Pfad unverändert. Counter-Sampling im Detailed-Modus aus. `m_gpuTimer` wird jetzt immer erstellt (trägt Whole-Frame + den Akkumulator).
- **`GpuPassAccumulator`** (`HE_Core/include/Renderer/GpuPassAccumulator.h`, header-only, reines C++): sammelt die 6 async/out-of-order/verspäteten Completions pro Frame-Index, publiziert `FrameGpuStats` wenn alle erwarteten Pässe da sind (neuester Frame gewinnt; verlorene Completions werden GC't). 4 Unit-Tests (in-order/out-of-order/empty-pass/late-straggler/lost-completion). `gpuFrameMs` im Detailed-Modus = Span (maxEnd−minStart) über die 6 Cmdbufs.
- **Selbst-verifizierend (Advisor):** im Detailed-Modus muss Σ(Pässe) ≈ `gpuFrameMs` sein und Tonemap/Present auf sub-ms fallen → der bestehende `gpuPassesOverlap`-Guard liest dann automatisch `false`. Bleibt Σ ≈ 3×, ist das Cross-Cmdbuf-Hazard-Tracking nicht eingetreten (am nächsten Dump sofort sichtbar). **Scene bleibt EIN Bucket** — Per-Element-in-Scene ist in-Engine auf dieser GPU nicht verlässlich (bräuchte per-Element-Cmdbufs mit Tile-Reload). Per-Pass ist die ehrliche Obergrenze.
- **Cross-Backend (offen, NACH Metal-Verifikation):** das `passes[]`-Modell ist backend-agnostisch; D3D11/D3D12/Vulkan/Desktop-GL sind nicht-TBDR → dort sind Timestamp-Query-Per-Pass-Zeiten additiv/verlässlich. Bewusst NICHT in diesem Push gebaut (4 blinde Backends = der Session-Fehler ×4) — erst Metal auf echter HW bestätigen.

**(historisch) Smoke-Test-Reihenfolge (jetzt teils durch obigen Lauf erledigt):**
1. **ZUERST: Metal API Validation Layer AN** (`MTL_DEBUG_LAYER=1`, ggf. `MTL_SHADER_VALIDATION=1`, oder Xcode-Scheme-Diagnostics) → F9-Capture starten und prüfen, dass **kein Validation-Error** ausgelöst wird. Das ist der diskriminierende Check: auf draw-boundary-fähigen Geräten teilen sich am „Scene"-Encoder ein **Stage-Boundary-Timer und `sampleCountersInBuffer`** denselben Buffer — diese Kombination ist im Sandbox NICHT verifiziert; falls Metal sie ablehnt, ist das ein Crash/Assert bei Capture-Start, kein „ungenaue Zahl"-Problem. **Fallback (im Code dokumentiert an `ftAttachPass(...,"Scene")`):** bei aktivem `m_ft.draw` den Scene-Stage-Pair NICHT anhängen, Scene-Total stattdessen aus erstem/letztem Draw-Boundary-Point ableiten.
2. **Dann Plausibilität:** Editor → F9 → bewegen → F9 → `dumps/profile_*.json`: `gpu`-Pässe Shadow/SSAO/Scene/Bloom/Tonemap/Present mit plausiblen ms, `approx`-Einträge (Opaque/Sky+Clouds/…) ⊆ Scene, `stats.draws/tris/visible/total` ≠ 0. Log: „per-pass GPU timing enabled (stage + draw-boundary…)".
3. **Genauigkeit der `approx`-Splits:** gegen Metal System Trace auf einem bekannt-teuren Sky-Frame abgleichen, bevor die Intra-Scene-Zahlen als belastbar gelten.

## Erweiterung v2.4 (2026-06-25) — Editor-Profiling-Fenster

Das „Performance Profiler"-Fenster (View-Menü) hat jetzt drei Tabs:
- **Overview** — always-on Live-HUD (FPS, CPU-, GPU-, Frame-ms, draws/tris/objects) + **Frametime-Graph** (`ImGui::PlotLines` über die letzten ~240 Frames, avg/max-Overlay) + optionaler GPU-Graph. Gespeist durch einen leichten Live-Ring im `EngineProfiler` (keine Scopes), den die App nur füllt, solange das Fenster **sichtbar** ist (`setLiveEnabled`), sonst zero-cost.
- **Capture** — „Start Benchmark Capture (F9)" (vsync-off-Multi-Frame → JSON-Dump), **„Capture Single Frame"** (genau ein Frame, detailed-GPU erzwungen → exklusive Per-Pass-Zeiten same-frame dank `waitUntilCompleted`, kein Dump), detailed-Checkbox, Dump/Open-Folder.
- **Frame Detail** — volle Aufschlüsselung **eines** Frames (Single-Frame-Capture, sonst letzter Benchmark-Frame): GPU-Pässe (mode-aware: „exclusive, serialized" im detailed-Modus, sonst Overlap-Warnung), CPU-Scope-Baum (eingerückt nach Tiefe, ms + Balken), Zähler, `gpuMode`.

Backend: `EngineProfiler` bekam `ProfLiveFrame`-Ring (`pushLive`/`liveSnapshot`/`setLiveEnabled`/`liveEnabled`, cap 240), `requestSingleFrameCapture()`/`singleFrame()` (1-Frame-Aufnahme in `m_singleFrame`, `m_forceDetailed` → `detailedGpuCapture()` true für den Frame, kein Dump), und `lastCpuFrameMs()`. `beginFrame/endFrame` messen `cpuFrameMs` jetzt **immer** (2 Timestamps, vernachlässigbar) für den Live-HUD. Application pusht Live-Frames, wenn `liveEnabled() || isRecording()`. **Editor-UI im Sandbox nicht lauffähig → Sichtprüfung durch User offen.**

## Nutzung (so misst man)
**Starten/Stoppen — zwei Wege, beide identisch:**
- **F9** (Editor *und* Spiel): startet einen Benchmark-Capture (schaltet VSync aus → echte Frametimes), F9 erneut = stoppen + Dump schreiben. Funktioniert auch im ausgelieferten Spiel ohne UI.
- **Editor-Panel** `View ▸ Performance Profiler`: Button „Start Benchmark Capture", Live-Anzeige, „Stop & Dump", „Dump Now" (ohne zu stoppen), „Open Dumps Folder".

**Wo landen die Dumps:** `<deploy>/dumps/profile_<YYYYMMDD_HHMMSS>.json`, also bei dir `out/deploy/Editor/dumps/` (Editor) bzw. `out/deploy/Game/dumps/` (Spiel) — gleicher Ordner wie `HorizonEngine.log`.

**Dump lesen:**
- `session` — Backend, GPU, Auflösung, **vsync** (bei Benchmark = false → Zahlen sind echte Kosten). `captureNote` warnt, falls vsync an war.
- `summary` — der Schnellblick: pro CPU-Scope und pro GPU-Pass `min/avg/max/count` über die ganze Aufnahme. **Hier zuerst schauen.**
- `frames[]` — pro Frame: `cpuMs`, `gpuMs`, `deltaMs`, `cpu[]` (Scopes mit Tiefe `d`), `gpu[]` (Pässe Scene/Tonemap/Present), `stats`.

**Für die Sky-Baseline (leere Welt):** `summary.gpuPasses.Scene.avg` = GPU-Kosten von Atmosphäre + Wolken zusammen (verifiziert: `EncodeScene` zeichnet den Sky auf demselben Encoder). `Tonemap`+`Present` = Post-FX. Differenz `gpuFrameMs − ΣPässe` = SSAO/Bloom/Shadow (noch nicht einzeln getimt). Ist `Scene` GPU-teuer und `cpuFrameMs` klein → GPU-gebunden = Shader-Optimierung (Sky-Plan), nicht CPU.

**Wichtig beim Lesen:**
- VSync-an macht `deltaMs`/FPS bedeutungslos (auf Refresh gepinnt) → für echte Zahlen den F9-Benchmark (vsync off) nutzen. `gpuMs`/`gpu[]` sind immer vsync-immun.
- Die **ersten ~1–2 Frames** einer Aufnahme haben **kein** `gpu[]` (die CPU/GPU-Zeit-Korrelation braucht zwei Messpunkte) — leeres `gpu` auf Frame 0 ist normal, kein Fehler.
- GPU-Zeiten sind 1–2 Frames versetzt (async) und dem Frame zugeordnet, in dem sie ankommen.
- Im Log bestätigt „Metal: per-pass GPU timing enabled", dass Counter-Sampling läuft; sonst nur Whole-Frame-`gpuMs`.

## Offene Punkte / Risiken
- **GPU-Latenz**: GPU-Zeiten sind 1–N Frames versetzt; im Dump per Frame-Index zuordnen, nicht per Wall-Clock.
- **macOS-GL-Timestamps** unzuverlässig → bewusst −1 statt falscher Zahlen.
- **D3D/Vulkan blind** → Interface ja, Impl später mit HW-Verifikation.
- **`startupPath`-Form**: endet mit Separator (Content = `startupPath+"Content"`); `parent_path()` liefert dieselbe Deploy-Wurzel wie der Log → verifizieren beim ersten Dump.

---

# Erweiterung v3 (2026-08-18) — „deutlich ausführlicher und visuell ansprechender"

> Auftrag (User): mehr messen, besser darstellen. Die Bestandsaufnahme unten sagt zuerst, **was schon da ist**, damit die Arbeit dort landet, wo wirklich Lücken sind, statt weitere Subsysteme mit Scopes zu bestreuen, die es längst haben.

## Bestandsaufnahme: was v1/v2 schon misst
- **CPU-Scopes**, verschachtelt, RAII, zero-cost wenn aus: Frame-Schleife (PollEvents/OnRender/Render/GameLogicTick/SwapBuffers), 11 Szene-Systeme, PhysicsStep, AudioSpatial, ScriptUpdate, CollisionDispatch, EnvironmentPush. **Breite ist gut.**
- **GPU pro Pass** (Metal „detailed" verifiziert exklusiv/additiv, GL-Timer-Queries, Whole-Frame-Fallback), modus-gestempelt, mit TBDR-Overlap-Guard.
- **Render-Zähler** draws/tris/visible/total.
- **Dump** (JSON) mit min/avg/max je Scope und Pass, **Live-HUD** + Frametime-Graph, Single-Frame-Capture.

## Die echten Lücken (das ist v3)
1. **Nur der Main-Thread wird aufgezeichnet.** Der JobSystem-Pool wickelt jede Task in `HE_PROFILE_SCOPE_N("Job::Execute")`, aber `begin/endScope` verwerfen alles, was nicht vom Capture-Thread kommt (v2.1-Crashfix). Damit ist FrustumCuller/RenderExtractor-`parallel_for` — die parallele Hälfte des Frames — komplett unsichtbar. **Das ist die Kopf-Lücke von „ausführlicher".**
2. **Keine Statistik.** `min/avg/max` beantworten „wie schnell im Schnitt", nicht „wie schlecht im schlechten Fall". Ruckler leben in p99 und 1%-Low, nicht im Mittelwert.
3. **Keine Self-Time.** Ein Scope-Baum ohne Eigenzeit sagt „Render kostet 9 ms" — nicht, *wo* in Render.
4. **Tote Zähler.** `entities/lights/particles/gpuParticles/streamingInFlight/vram*` stehen in `ProfRenderStats`, aber `Application.cpp` befüllt sie nie → im Dump immer 0.
5. **Darstellung ist eine Textliste.** Zwei `PlotLines` und eingerückte `ImGui::Text`-Zeilen mit `ProgressBar`. Kein Flame-Graph, keine Timeline, kein Histogramm, kein Scrubbing, keine Sortierung, kein Top-N.

## Teil A — was man messen kann (und was davon v3 baut)

### A1. Per-Thread-Timeline (BAUEN)
Der Wechsel vom „Scope-Stack pro Frame" zum **Capture-weiten Ereignisstrom pro Thread**:
```
struct ProfThreadSpan { const char* name; uint64_t startNs, endNs; uint32_t depth; };  // session-relativ
```
- `thread_local` Ring fester Kapazität pro Thread; ein Thread registriert seinen Puffer beim ersten Scope **einmal** unter Mutex in einer Registry. Danach schreibt **nur er** hinein → single-writer ohne Lock im Hot-Path, genau die Eigenschaft, deren Verletzung v2.1 zum Absturz brachte.
- Aufgezeichnet werden **abgeschlossene** Spans (im `endScope`), mit absoluten Start/End-Zeitstempeln. Damit ist die Frage „ein Scope überlappt die Frame-Grenze — zu welchem Frame gehört er?" gar nicht erst zu beantworten: die Timeline ist frame-los, Frames sind nur Marker darauf.
- Der Main-Thread-Pfad (`m_current.scopes`) **bleibt unverändert** — die bestehende Frame-Aufschlüsselung, der Dump und die Tests hängen daran. Die Timeline kommt additiv daneben.
- **Vertragsumkehr:** der Regressionstest „8 Threads × 5000 Scopes" prüft heute, dass Worker-Scopes *ignoriert* werden. v3 kehrt das um: sie werden *aufgezeichnet*, ohne Korruption. Der Test wird bewusst umgeschrieben, nicht gelöscht — er ist der Wächter über den einzigen Laufzeit-Crash, den dieses System je hatte.

### A2. Statistik statt Mittelwert (BAUEN)
Über jede Serie (Frametime, CPU, GPU, jeden Scope, jeden Pass): **p50/p95/p99, 1%-Low und 0.1%-Low FPS, Stddev**, plus **Hitch-Erkennung** (Frame > 2× Median → Liste der Ruckler-Frames mit Index und dem teuersten Scope darin). Das ist die Zahl, die man auf einen Screenshot schreibt, wenn man behauptet, etwas sei schneller geworden.

### A3. Self-Time (BAUEN)
Aus `depth` + Schließreihenfolge rekonstruierbar, ohne die Erfassung anzufassen: Eigenzeit = Inklusivzeit − Σ direkter Kinder. Damit wird die Top-N-Tabelle nach *Eigenzeit* sortierbar — die einzige Sortierung, die auf den Übeltäter zeigt statt auf dessen Elternkette.

### A4. Zähler befüllen (BAUEN)
`entities/lights/particles/gpuParticles/streamingInFlight` aus vorhandenen Quellen (World-Registry, ContentManager) — **nur wo es schon einen Getter gibt**. Keine `malloc`-Hooks in dieser Runde.

### A5. Was bewusst NICHT gebaut wird (Roadmap)
- **Allokations-Profiling** (globale `new/delete`-Hooks, Peak-Bytes/Frame, Allokationen pro Scope). Hoher Wert, aber ein eigener Eingriff mit ABI-Risiko über 10 Module.
- **A/B-Vergleich zweier Captures** (zwei Dumps laden, Δ pro Scope). Reines UI-Feature, sinnvoll erst wenn v3-Dumps existieren.
- **Cross-Backend-GPU-Queries** (D3D11/D3D12/Vulkan). Auf diesem Mac nicht verifizierbar — vier blinde Backends sind der dokumentierte Session-Fehler ×4.
- **Per-Objekt-GPU-Zeit.** Auf TBDR architektonisch nicht sinnvoll (siehe v2).

## Teil B — wie man es darstellt

Kein neues Vendoring: **ImPlot ist nicht im Baum** (`EditorDeps` hat nur EngineContent/Fonts/Images), und eine Charting-Bibliothek für vier Diagramme hereinzuziehen passt nicht zum Rest des Editors. Alles wird mit `ImDrawList` gezeichnet — dieselbe Technik wie der bestehende Graph-Editor.

| Ansicht | Zeigt | Warum diese Form |
|---|---|---|
| **Flame-/Timeline-Graph** | Main-Thread-Lane + eine Lane je Worker + GPU-Pass-Lane, Rechtecke nach Zeit/Tiefe, Zoom+Pan, Hover-Tooltip | Die einzige Darstellung, in der man *Parallelität* sieht: Lücken zwischen Worker-Balken sind unausgelastete Kerne, ein breiter Main-Balken neben leeren Workern ist ein Serialisierungs-Engpass. Eine Liste kann das nicht. |
| **Frametime-Graph mit Ruckler-Färbung** | Balken je Frame, eingefärbt gegen 60/120-FPS-Budget, p95/p99-Linien, Klick = Frame in „Frame Detail" laden | Verwandelt den Graph vom Dekor zum Navigationsmittel: der Ausreißer, den man sieht, ist der Frame, den man untersuchen will. |
| **Histogramm** | Verteilung der Frametimes | Ein Mittelwert von 11 ms bei bimodaler Verteilung (8 ms / 22 ms) fühlt sich an wie 22 ms. Nur das Histogramm zeigt das. |
| **Top-N-Tabelle** | Scope, Eigenzeit, Inklusivzeit, Anzahl, min/avg/p95/max, Sparkline | Der Arbeits-Bildschirm: sortierbar, sofort ranking-fähig. |
| **Budget-Balken** | CPU/GPU gegen Zielframezeit, farbcodiert | „Passt es ins Budget" ist eine andere Frage als „wie viele ms" und verdient eine andere Darstellung. |

**Modus-Ehrlichkeit wandert mit.** Der GPU-Lane wird nur bei `detailed`/`gl-timer` additiv (aneinandergereiht) gezeichnet; im `counter`-Modus zeichnet sie ehrlich **überlappende** Spans plus den bestehenden Warntext. Die v2-Lehre (Σ ≈ 3× auf TBDR) darf nicht durch eine hübschere Ansicht verlorengehen — ein Balkendiagramm, das überlappende Spans nebeneinanderlegt, *behauptet* Additivität.

## Teil C — Umsetzungsreihenfolge
1. **Per-Thread-Timeline** (Erfassung + Merge + Dump-Anteil + Test-Vertragsumkehr).
2. **Statistik + Self-Time** als reine Funktionen (`ProfilerStats.h`) → headless testbar, Dump-`version` auf 2.
3. **Zähler befüllen.**
4. **UI-Überarbeitung** (ImDrawList-Widgets, Layout-Mathe in reine Funktionen ausgelagert und getestet).

## Umsetzungsstatus v3 (2026-08-18) — alle vier Phasen auf dem Branch

**Phase 1 — Per-Thread-Timeline.** `ProfThreadSpan`/`ProfThreadTimeline`/`ProfFrameMark` in `EngineProfiler.h`; `thread_local`-Puffer, per `shared_ptr` in einer Registry gehalten (ein Worker, der mitten im Capture endet, nimmt seine Daten nicht mit), Generation-Stempel gegen Reste alter Captures, per-Puffer-Mutex nur dort, wo der Merge liest. `timelineSnapshot()`, `frameMarks()`, `timelineDroppedSpans()`, `setThreadTimelineEnabled()`. Der Regressionstest (8 Threads × 5000 Scopes) prüft weiter auf Nicht-Korruption, aber mit umgekehrtem Vertrag: Worker-Scopes landen jetzt auf eigenen Lanes.

**Phase 2 — Statistik + Self-Time.** Neuer Header `Diagnostics/ProfilerStats.h`, rein funktional: `computePercentiles` (p50/p95/p99, Stddev, nearest-rank), `computeFpsLows` (1 %/0,1 %-Low nach Benchmark-Konvention, Definition steht im Dump), `findHitches` (gegen den **Median**, mit dem teuersten depth-0-Scope pro Ruckler), `aggregateScopes` (Self-Time aus Tiefe + Schließreihenfolge), `computeHistogram`, `laneOccupancy`. Dump-`version` 2, `summary.stats`, `summary.hitches`, `summary.topScopesBySelfTime`, `summary.threads`, `threads[]`, `frameMarks[]`.

**Phase 3 — Zähler.** `SceneSystems::pushProfilerSceneCounters` füllt entities/lights/particles/emitters/rigidBodies/audioSources/scripts/gpuParticles + `ContentManager::asyncInFlightCount()`. `setRenderStats` merged nur noch die Renderer-Felder — die Wholesale-Zuweisung war die eigentliche Ursache dafür, dass diese Felder seit v1 in jedem Dump 0 waren.

**Phase 4 — Darstellung.** `ProfilerLayout.h` (reine Geometrie: Span→Rechteck, Zoom/Pan/Clamp, Bar-Hit-Test, Tick-Schritte, Namens-Hash) + `ProfilerWidgets.h/.cpp` (ImDrawList: Stat-Tiles, Budget-Bars, Frame-Graph mit Budget-Färbung und p95/p99-Linien, Histogramm, Multi-Lane-Flame-Timeline mit Wheel-Zoom/Drag-Pan/Hover-Tooltip, Sparkline). Panel jetzt fünf Tabs: **Overview** (Tiles, Budget, Frame-Graph, Histogramm, Zähler-Grid), **Timeline**, **Scopes** (Ranking nach Self-Time), **Capture** (+ Hitch-Liste, klickbar → Frame Detail), **Frame Detail** (Scope-Baum jetzt rückwärts gelesen, also Eltern über Kindern).

**Tests:** 1738 Fälle grün, davon 15 neue (`test_profiler_stats.cpp`, `test_profiler_layout.cpp`) plus die erweiterten Profiler-Fälle. Alle Targets bauen.

**Kosten der neuen Ansichten.** `snapshot()` und `timelineSnapshot()` kopieren tief (alle Frames mit ihren Scope-Vektoren, alle Lanes mit bis zu 262144 Spans). Drei Tabs wollen diese Daten, gezeichnet wird mit Editor-Framerate — pro Draw kopiert wären das Megabytes 60×/s, beim Timeline zusätzlich unter dem Lane-Mutex, gegen die schreibenden Worker. Deshalb liegt im Panel ein Cache: fertige Aufnahme einmal holen, laufende alle 250 ms, plus genau ein Refresh beim Stoppen. Der Profiler darf nicht bremsen, was er misst.

**Bekannte Eigenheit (vor-v3, jetzt sichtbar):** „Capture Single Frame" ruft intern `doStart()` und leert damit `m_frames` — die vorherige Benchmark-Aufnahme ist danach weg, also auch Scopes-Tab und Hitch-Liste. Nicht neu, fällt nur jetzt auf. Reihenfolge deshalb: erst Benchmark auswerten, dann Einzelframe.

**Offen — Sichtprüfung durch den User (Sandbox ohne Display):** Editor starten → `View ▸ Performance Profiler`. (1) *Overview* zeigt Tiles/Budget/Graph/Histogramm und die Zähler sind **nicht mehr 0**. (2) *Capture* → „Per-thread timeline" an → Benchmark starten, ein paar Sekunden bewegen, stoppen. (3) *Timeline*: eine „Main"-Lane **und** „Worker N"-Lanes mit `Job::Execute`-Spans; Wheel zoomt unter dem Cursor, Drag pant, „Fit" setzt zurück. (4) *Scopes*: Ranking nach Self-Time plausibel. (5) Der Dump enthält `summary.stats`, `summary.threads`, `frameMarks` und Zähler ≠ 0.

## Teil D — Grenzen dieser Runde
- **Dump-Größe:** Per-Thread-Spans über 20 000 Frames sprengen das JSON. Die Timeline wird im Dump auf ein hartes Ereignisbudget gedeckelt und die Kappung **protokolliert** (Log + `summary`-Feld), nie still abgeschnitten. Der vollständige Strom bleibt der Live-Ansicht vorbehalten.
- **Keine Sichtprüfung in dieser Umgebung** (Sandbox ohne Display). Deshalb liegt die gesamte Layout-Mathematik (Span→Rechteck bei Zoom/Viewport, Histogramm-Binning, Perzentile) in reinen Funktionen mit Unit-Tests, und am Ende steht eine Smoke-Test-Liste für den User — wie schon bei v1/v2.
