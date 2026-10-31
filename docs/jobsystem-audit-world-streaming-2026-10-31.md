# Job-System-Audit im Hinblick auf mehr World-Streaming, Map-Bauten, Shader- und Mesh-Upload

Stand 31.10.2026. Reiner Code-Audit (nichts gemessen, nichts gebaut, kein Engine-Code geändert).
Gelesen: `JobSystem.h/.cpp` komplett, `ContentManager.cpp` (Load-Pfad, `pollAsyncResults`,
`parseAndRegisterAsset`), `CellStreamer.cpp:470-540`, `SceneJsonParse.cpp:240-310`,
`D3DShaderPrefetch.h`, die `WarmupMaterials`-Stellen der Backends, Vulkans Upload-Batches,
Jolt-/SkyNoise-Threads, sowie die Handoffs der Docs `cpu-threading-*` und
`world-streaming-baseline-2026-10-06.md`. Was dort schon gelöst ist, steht hier nicht noch einmal.

## 1. Urteil

Der Pool selbst ist für seine heutige Last solide: drei Prioritäten, Low-Obergrenze, Abbruch
(Token, `stale`, `onCancelled`), Abhängigkeiten, `wait()` ohne Deadlock, `parallel_for`, in dem der
Aufrufer mitrechnet. Für größere Streaming-Lasten fehlen **nicht** mehr Threads, sondern vier Dinge:

1. **Die Prioritäten sind zu grob und zu starr.** Drei Lanes, FIFO darin, und ein eingereihter Job
   wechselt die Lane nie (§3, A1/A2).
2. **Die teuren Stufen der Pipeline laufen auf dem Main-/Render-Thread**, nicht im Pool: Asset-Parse
   mit Kopien, Material-/PSO-Aufbau (§3, B1/B2).
3. **Lesen (IO) und Rechnen (CPU) teilen sich dieselben Worker**, und das fertige Pipeline-Feature
   `JobDesc::after` hat keinen einzigen Produktions-Nutzer (§3, C1/C2).
4. **Es fehlt Gegendruck und Beobachtbarkeit der Wartezeit** (§3, D1/D2).

## 2. Was schon gut ist (nicht anfassen)

- Priority-Lanes + Cap `threads-2` für Low (`JobSystem.cpp:441`): hält zwei Worker für den Frame frei.
- `parallel_for` mit Mindest-Chunk und dynamischem Claim (`JobSystem.h:272-370`): degradiert bei
  gesättigtem Pool auf „Aufrufer macht alles“, nie Deadlock.
- Abbruch wirkt zwischen 4-MiB-Blöcken (`ContentManager.cpp:1382`, `readEntry`) und räumt über
  `onCancelled` den Coalesce-Key frei; abgebrochene, wieder gewünschte Loads starten neu (Poll, ~1507).
- Vulkan hat gebatchte Uploads über einen Staging-Ring mit Fence statt `vkQueueWaitIdle` je Upload
  (`VulkanRenderer.cpp:7683-7830`; Sync-Variante nur per `HE_VK_UPLOAD_SYNC`).
- D3D hat einen Shader-Compile-Prefetch im Pool (`D3DShaderPrefetch.h:144`).

## 3. Befunde, nach Hebel geordnet

Aufwand: S = Stunden, M = 1-2 Tage, L = Woche+. „Beleg“ = gelesen, nicht gemessen.

### A. Prioritäten und Reihenfolge

**A1. Kein Hochstufen eines wartenden Jobs.** `LoadInterest::join` senkt zwar `who.priority`
(`ContentManager.cpp:1224`), aber `desc.priority` wird nur beim Start gelesen (`:1346`, `:1749`) und
`loadPathAsync` koalesziert danach nur noch (`:1292`). Ein Asset, das ein Vorlade-Zoll (Low) angestoßen
hat und das jetzt die sichtbare Kamera braucht, bleibt in der Low-Lane, hinter der Obergrenze.
Standardpriorität aller Asset-Loads ist ohnehin Low (`ContentManager.h:36`, `:736`); nur Zellen und
Szenen-Parse nutzen High/Normal (`CellStreamer.cpp:483`, `SceneJsonParse.cpp:294`).
*Maßnahme (S-M):* `JobHandle::promote(JobPriority)`: Priorität atomar setzen und den Job **ein
zweites Mal** in die höhere Lane schieben; der alte Eintrag ist danach ein toter Eintrag (der
`claim(Queued)` des Workers scheitert dort schon heute, `JobSystem.cpp:475`). O(1), kein Umbau der
Deques. `LoadInterest` hält dafür das `JobHandle`. Billigere Notlösung ohne Pool-Änderung:
`handle.cancel()` und über den vorhandenen Neustart-Pfad (`onCancelled` → Poll) in der neuen Lane
neu starten.

**A2. FIFO in der Lane, kein Sortierschlüssel.** Ein weit entfernter Chunk, der früher eingereiht
wurde, läuft vor einem nahen, der später kam (alle Streaming-Jobs sind Low/Normal, FIFO:
`JobSystem.cpp:48,465`). Bei Teleport oder schnellem Flug ist das genau der falsche Fall.
*Maßnahme (M):* `JobDesc::order` (uint32, kleiner = zuerst, z. B. quantisierte Distanz) und je Lane
ein Heap mit Sequenznummer als Tie-Breaker statt `std::deque`. Dazu 4-5 statt 3 Lanes (z. B.
`Critical` für „Main-Thread blockiert jetzt“, `Idle` für Thumbnails/Content-Sync, damit sie nie
Streaming verdrängen). Die Cap-Logik (`pick`, `JobSystem.cpp:64`) bleibt.

### B. Teure Arbeit auf dem Main-/Render-Thread

**B1. Der Parse-Anteil der Asset-Registrierung kopiert auf dem Main-Thread.** Der Worker liest und
zerlegt in Chunks (`splitChunks`, `:1313`), aber `parseAndRegisterAsset` kopiert jeden Chunk noch
einmal in das Asset: `readVec` für Meshes, `a.data = c->data` für Texturen (`ContentManager.cpp:312`,
auch `:583`), und Materialien bauen ihr GLSL beim Laden aus dem Graphen neu (laut Kommentar in
`ContentManager.h:772`; den Aufruf selbst habe ich nicht gelesen, also nur ein Hinweis).
Das Zeitbudget (`:1486`) prüft nur **zwischen** Assets, und `toRegister > 0` lässt immer mindestens
eines durch: eine 64-MB-Textur überschreitet das 2-ms-Budget um ein Vielfaches.
*Maßnahme:* (S) Buffer aus dem `Reader` **verschieben** statt kopieren (der Reader gehört dem
`AsyncResult` und stirbt danach; dafür ein nicht-`const` `takeChunk`). (M-L) Danach
`parseAndRegisterAsset` teilen in `parse(reader) → Asset` (Worker) und `register(Asset)` (Main:
Slot einfügen, Frontier, Callback). Der Main-Thread macht dann pro Asset nur noch O(1).

**B2. Material-/PSO-Aufbau ist synchron im Render-Thread.** Metals `WarmupMaterials` baut Pipeline
und Blend-Zwilling inline (`MetalRenderer.mm:9911-9951`; der Code-Kommentar nennt 0,4-0,7 s auf einer
E-Core pro Material), D3D12 drainiert `m_pendingMatWarmup` im `DrawScene` synchron
(`D3D12Renderer.cpp:4716`), Vulkan reiht nur ein (`VulkanRenderer.cpp:13993`) und baut im Drain. Der
D3D-Prefetch deckt nur die festen Start-Shader ab (`D3D11Renderer.cpp:6182`, `D3D12Renderer.cpp:2904`),
nicht Materialien, die mit einer Zelle eintreffen. Bei Zell-Streaming ist das die wahrscheinlichste
Quelle von Frame-Spikes: jede neue Zelle bringt neue Materialien.
*Maßnahme (L, größter Hebel für Streaming):* zweistufig. Worker: Quelltext auflösen, cross-compilen,
Bytecode/Library bauen (`MTLLibrary`-Compile, `vkCreateGraphicsPipelines` mit Pipeline-Cache,
`CreateGraphicsPipelineState`, FXC sind thread-sicher; GL braucht Shared-Context oder
`KHR_parallel_shader_compile`). Render-Thread: nur noch das fertige Objekt in den Cache heben. Bis
„ready“ zeichnet der Frame das Objekt nicht oder mit Platzhalter-Material. `schedule` mit `cancel`
(Material-Interesse) und `onCancelled` (Key freigeben) passt 1:1; `d3dprefetch::take` ist die
Vorlage für das Abholen.

**B3. Mesh-/Textur-Upload je Backend ungleich.** Vulkan: gebatcht (gut). Metal: mehrere
`waitUntilCompleted` im Renderer (`MetalRenderer.mm:10321…11604`, Großteil Thumbnails/Capture, nicht
verifiziert, welche im Streaming-Pfad liegen). D3D12: Kommentare nennen einen QueueWaitIdle-Ansatz
für seltene Builds (BLAS) und Upload je `uploadTexture2D` im Command-List des Frames
(`D3D12Renderer.cpp:4464`). Es gibt kein einheitliches **Upload-Budget pro Frame** über die Backends.
*Maßnahme (M je Backend):* gemeinsame Upload-Queue (Bytes/Frame-Budget, Priorität = Sichtbarkeit),
Fortschritt als „ready“-Flag; Staging-Kopie (CPU-seitig) im Pool, nur Submit im Render-Thread.
Eigene Copy-Queue (D3D12 Copy-Queue, Vulkan Transfer-Queue, Metal Blit auf eigener Queue) ist der
Schritt danach.

### C. Verteilung auf die Worker

**C1. IO und CPU teilen sich Worker.** Pak-/Pfad-Load-Jobs sind ein Block aus blockierendem Lesen
plus zstd-Dekodierung (`ContentManager.cpp:1363-1416`, `:1767-1800`). Low darf `threads-2` Worker
belegen; auf einer langsamen Platte, einem Netzlaufwerk oder einem Cloud-Sync-Ordner sind das
lauter wartende Threads, während Dekodier-/Parse-Arbeit keinen Platz findet.
*Maßnahme (M):* Generalisieren der Cap von „je Priorität“ auf „je Ressourcenklasse“
(`JobDesc::kind = Cpu | Io`, `limit[kind]`, z. B. Io = 2-4). Das ist im bestehenden
`pick`/`running`-Schema ein kleiner Schritt.

**C2. `JobDesc::after` hat null Produktions-Nutzer.** Die Suche nach `desc.after`/`.after` findet es
nur in `JobSystem.cpp` und den Tests. „Lesen → Dekodieren → Parsen“ wird weiter als ein Monolith
geschedult. Eine Zerlegung bringt: Abbruchprüfung zwischen den Stufen, getrennte Prioritäten/Caps
(C1), und Tasks, die kürzer als ein Frame sind. Zusammen mit B1 die natürliche Pipeline:
`Read(Io) → Decode+Parse(Cpu) → Register(Main)`.

**C3. Keine Thread-QoS.** Weder `QOS_CLASS`, `SetThreadPriority`/EcoQoS noch Affinität kommen im Code
vor (Grep über HE_Core/Scene/Rendering). Auf Apple Silicon und Intel-Hybrid entscheidet der OS-Scheduler
allein, ob Streaming-Arbeit P-Cores belegt oder Frame-Helfer auf E-Cores landen (der Metal-Kommentar
oben zeigt, dass das vorkommt).
*Maßnahme (S-M):* zwei Worker-Gruppen statt eines Caps: „Frame“-Worker mit normaler/hoher QoS und
„Background“-Worker (macOS `QOS_CLASS_UTILITY`, Windows EcoQoS, Linux `nice`) für Low/Idle. Wartet
ein Frame-Job inline auf einen Low-Job (`wait()`), läuft dieser ohnehin auf dem Aufrufer, es gibt also
keine Prioritätsinversion.

**C4. Oversubscription durch Nebenpools.** Zusätzlich zum Pool mit `hardware_concurrency` Threads
laufen Jolts `JobSystemThreadPool` (`PhysicsWorld.cpp:836-838`, bis 4 Worker), ad-hoc
`std::thread`s in `SkyNoise3D.cpp:75` (bis 8, je Bake neu gestartet) und `std::async`/`std::thread`
im Editor. `SkyNoise3D` ist trivial auf `parallel_for` umzustellen (S). Jolt auf den Engine-Pool zu
legen (eigenes `JPH::JobSystem`) ist L und erst nach einer Messung der Überbelegung sinnvoll. Außerdem:
`hardware_concurrency()` Worker plus Main plus Render-Thread ist mehr als die Kernzahl; `N-1` wäre
mit „Aufrufer rechnet mit“ üblich. Messen, bevor man es ändert.

### D. Gegendruck, Wartezeit, Betrieb

**D1. Kein Gegendruck auf fertige, nicht registrierte Ergebnisse.** Die Worker laufen so schnell wie
Lesen und Dekodieren erlauben, der Main-Thread registriert höchstens 16 Assets pro 2 ms
(`EditorApplication.cpp:3497-3500`; Spiel `GameApplication.cpp:3228`). Das Ergebnis-Queue
(`m_asyncSink->results`) wächst unbegrenzt, jedes Element hält die zerlegten Chunks eines ganzen
Assets im RAM. Bei einem Welt-Sprung kann das den Speicher-Peak bestimmen.
*Maßnahme (S-M):* Zähler „dekodierte, nicht registrierte Bytes“; über Schwelle (z. B. 256 MB)
kehren Low-Jobs vor dem Lesen zurück in die Queue (oder `stale`-ähnliches „später“). Das Budget für
Main sollte außerdem nach Frame-Last statt konstant sein (Editor 2 ms, Spiel `kStreamBudgetMs -
spentMs` sind getrennte Konstanten).

**D2. Keine Messung der Wartezeit in der Queue.** `ThreadPoolStats` kennt Laufzeit (`busyNs`),
Zähler und Länge, aber nicht „eingereiht → gestartet“. Für Streaming ist genau das die Latenz, die
zählt. *Maßnahme (S):* Zeitstempel im `PoolTask`/`JobState`, je Lane Summe und Maximum der Wartezeit
(plus Histogramm je `name`, der Name ist ein statischer Zeiger), im Streaming-Tab anzeigen. Das ist
auch die Messgrundlage für alle anderen Punkte hier.

**D3. Herunterfahren arbeitet die Queue ab.** `stop` lässt die Worker leerlaufen, bis alle Queues
leer sind (`JobSystem.cpp:464`, Destruktor `:536`); die Warnung nennt die Anzahl nur. Mit hunderten
Low-Loads in der Queue ist das ein spürbar langer Exit. *Maßnahme (S):* ein `stopping`-Flag, das
`runClaimed` wie `cancelWanted` behandelt (Job endet `Cancelled`, `onCancelled` läuft).

**D4. Kleinigkeiten.** `wait()` auf einen Waiting-Job pollt mit 1 ms (`JobSystem.cpp:411`), das wird
erst relevant, wenn `after` produktiv genutzt wird. `counted` wird beim Pop festgelegt, ein
`setConcurrencyLimit` mitten im Betrieb lässt `running[]` kurz falsch zählen (Cap kann kurz
überschritten sein, nie negativ). `post()` mit Exception ist `std::terminate` (dokumentiert).
`queued` in den Stats zählt tote Einträge nach Massenabbruch mit.

### E. Skalierung des Pools selbst (erst nach Messung)

Ein Mutex und eine Condition-Variable für alle Lanes; je Task ein `std::function` plus meist ein
`shared_ptr<JobState>`; `notify_one` je Kopie. Bei den heutigen Raten (zig bis hunderte Tasks pro
Frame) unkritisch. Mit Zehntausenden kleiner Tasks pro Sekunde (Chunk-Meshing, Instancing je Zelle)
wird die Lock-Konkurrenz sichtbar. *Erst* einen Durchsatz-Benchmark in `test_jobsystem.cpp`
(Tasks/s bei 1/4/16 Threads), dann entscheiden: Stealing-Deques nur für die High-Lane
(`parallel_for`-Helfer), die geordnete globale Queue für Low/Normal behalten. Ein kurzer Spin
der Worker nach High-Arbeit (20-50 µs) senkt die Aufwachlatenz der Helfer (Audit 2026-10-22 sah
8,7 ms p50 unter Last; der Mindest-Chunk hat das entschärft, die Ursache bleibt); Energie dabei
mitmessen.

## 4. Empfohlene Reihenfolge

| # | Schritt | Aufwand | Warum zuerst |
|---|---|---|---|
| 1 | D2 Wartezeit-Messung in Stats + Streaming-Tab | S | Ohne sie sind 2-8 nicht belegbar |
| 2 | A1 `promote()` (oder Cancel+Neustart) | S-M | Sichtbare Assets hängen sonst hinter Vorladen |
| 3 | B1a Chunks verschieben statt kopieren | S | Main-Thread-Spike bei großen Texturen, Null Risiko |
| 4 | D3 Stop-Flag, C4 `SkyNoise3D` auf `parallel_for` | S | billig, räumt Ränder auf |
| 5 | A2 Sortierschlüssel + 5 Lanes | M | Teleport/Flug: nahe Chunks zuerst |
| 6 | C1+C2 IO-Klasse und `after`-Pipeline | M | trennt Plattenwartezeit von Rechenzeit |
| 7 | D1 Gegendruck | S-M | RAM-Peak bei Welt-Sprung |
| 8 | B1b Parse auf den Worker | M-L | Main-Thread pro Asset O(1) |
| 9 | B2 Material-/PSO-Aufbau im Pool | L | größter Frame-Spike-Hebel, braucht „ready“-Zustand im Draw |
| 10 | B3 gemeinsames Upload-Budget, C3 QoS-Gruppen | M | nach Messung von 1 |
| 11 | E Skalierung, Jolt auf den Engine-Pool | L | nur wenn Messung es verlangt |

Jeder Schritt braucht einen Test, der das alte Verhalten weiter prüft (CLAUDE.md §4 für Formate gilt
hier nicht, aber `test_jobsystem.cpp` und `test_async_streaming.cpp` sind die Regression). Für 2, 5, 6, 9
zusätzlich ein Linux-TSan-Lauf der beiden Dateien (das Doc zur Jolt-Umstellung vermerkt: kein TSan-Lauf).

## Handoff

- **Topic / step:** Job-System-Audit (World-Streaming, Map-Bauten, Shader-/Mesh-Upload), Schritt 1 von n.
- **Branch / commit:** Arbeitsbaum, nicht committet, nicht gepusht.
- **Done:** Befundliste mit Beleg und Priorisierung (§3, §4). Kein Engine-Code geändert.
- **Files touched:** nur dieses Doc.
- **Verified (gelesen, nicht ausgeführt):** `JobSystem.h/.cpp` komplett; Load-Pfad und Poll in
  `ContentManager.cpp:187-330, 1220-1305, 1330-1640, 1730-1800`; `CellStreamer.cpp:470-540`;
  `SceneJsonParse.cpp:240-310`; `D3DShaderPrefetch.h:90-170`; `WarmupMaterials` in Metal, Vulkan
  (nur Queue), D3D12 (`drainMaterialWarmup`); Vulkan `noteUpload`; Grep: kein QoS/Thread-Priority-Aufruf,
  `desc.after` nur in JobSystem und Tests, `JobPriority::` nur an den genannten Stellen.
- **Not verified:** keine Messung, kein Build, kein Test gelaufen. Nicht gelesen: der Vulkan-Drain
  der Material-Warmups, Metals Upload-Pfad für Meshes/Texturen im Detail (welche der
  `waitUntilCompleted` im Streaming-Pfad liegen), D3D11/GL-Upload, `expandFrontier`-Kosten,
  `TerrainSystem`-Chunk-Build auf dem Pool (nicht gefunden, vermutlich nicht im Pool).
- **Decisions taken:** keine Wiederholung der Befunde aus `cpu-threading-*` (Foliage, Animation,
  Extract) und `world-streaming-baseline` (Pak-TOC, Szenen-Parse).
- **Decisions suggested, not taken:** Reihenfolge in §4; `promote()` als Doppel-Eintrag statt
  Queue-Umbau; Ressourcenklasse Io/Cpu statt weiterer Prioritäts-Lanes.
- **Traps:** `ContentManager.cpp` und die `*Renderer.*` nie ganz lesen (CLAUDE.md §3); das Poll-Budget
  lässt immer ein Asset durch, nur `budgetMs`-Prüfung allein beweist nichts über Einzelspikes.
- **Next steps:** 1. D2 in `JobSystem.cpp` (Zeitstempel in `PoolTask`, Lane-Wartezeit in
  `ThreadPoolStats`, Anzeige in `StreamingDebugView.cpp`). 2. Mit den Zahlen A1 umsetzen
  (`JobHandle::promote`, Test in `test_jobsystem.cpp`, Verhalten in `test_async_streaming.cpp`).
  3. B1a als eigener kleiner Schritt mit Texturgrößen-Messung vorher/nachher.
