# Metal: Material-Pipelines beim Szenenöffnen, Messung (2026-10-31)

Folge von Punkt 4 aus `docs/jobsystem-mac-handoff-2026-10-31.md` (Branch `claude/jobsystem-audit-world-streaming`,
PR #215). Dort steht: `WarmupMaterials` baue Pipeline und Blend-Zwilling synchron, 0,4–0,7 s je Material, Plan
„auf den Pool“. Erst kamen Profiler-Scopes und eine Messung (unten), dann der Umbau (Abschnitt „Umbau“). Entscheidung
des Menschen: ein Material, dessen Pipeline noch baut, wird **nicht gezeichnet**, dafür gibt es einen sichtbaren
Hinweis „Compiling shaders“ und beim Start des Editors einen eigenen Schritt mit Fortschritt.

## Neue Scopes
`Metal::WarmupMaterials`, `Metal::MaterialPipelineBuild` (nur ein echter Build, ein Cache-Treffer kehrt davor
zurück) mit den Phasen `::CrossCompile` (glslang → SPIRV-Cross, nur ohne `precompiled`), `::Libraries`
(2× `newLibraryWithSource`) und `::Pipeline` (`newRenderPipelineStateWithDescriptor`).

## Messung
Foliage-Testprojekt (`~/HorizonEngineTestAssets/foliage-test-project`, Klon je Lauf), Release, Metal, Editor mit
`HE_HIDDEN_WINDOW=1`, `scripts/he_perf_capture.py --warmup 0 --frames 200 --no-counters`. Kalt = der Ordner
`$(getconf DARWIN_USER_CACHE_DIR)/com.apple.metal` (Metals Shader-Cache für Prozesse ohne Bundle-ID) war weg,
warm = er war aus dem Vorlauf gefüllt. Gemessen ist der Szenenstart (Frame 1 des Dumps); in der Summe der
Tabelle stecken 8 Builds (die Log-Zeile „warmed up“ meldet 6, zwei weitere kommen im ersten Draw-Frame dazu).

| Phase | kalt | warm (2 Läufe) |
|---|---|---|
| `WarmupMaterials` insgesamt (Frame 1) | **2829 ms** | 87 / 85 ms |
| `Libraries` (alle 8 Builds) | **2516 ms**, größter Einzelaufruf 1157 ms | 1,4 / 0,7 ms |
| `Pipeline` | 310 ms (max 77) | 2,4 / 1,6 ms |
| `CrossCompile` | 88 ms (max 19) | 86 / 85 ms (max 19) |

Lesart:
- Der kalte Stall ist die **MSL-Kompilierung in `newLibraryWithSource`** (89 %), nicht der Cross-Compile und nicht
  die PSO. Er tritt beim ersten Start auf einer Maschine auf und nach jeder Änderung des erzeugten MSL (neue
  Engine-Version, geänderte Shader), danach liefert Metals Cache.
- Der warme Rest (≈ 86 ms pro Szenenstart) ist fast nur der **Cross-Compile auf dem Aufrufer-Thread**.
- Im Spiel ruft `GameApplication.cpp:3234` `WarmupMaterials` für jedes beim Streaming neu registrierte Material
  auf: dort wirkt dieselbe Aufteilung pro Material (kalt etwa 0,3 s je Build, warm ≈ 14 ms Cross-Compile).

## Was die Frame-Spitze NICHT ist
Der erste Draw-Frame kostet in allen Läufen, kalt wie warm, 500–730 ms CPU, davon `Metal::EncodeScene`
350–540 ms und `Metal::RefineBounds` 140–180 ms. Dort laufen nur 2 ms (warm) bis 85 ms (kalt)
Material-Pipeline-Builds. Der Log nennt zu der Zeit die ersten Mesh-Uploads (12 texturierte Meshes, „shadow pass
instanced (first run)“, „SSAO pre-pass instanced (first run)“). Das ist ein anderes Thema (Upload-Budget,
Audit B3) und wird von einem asynchronen Material-Build nicht kleiner. Nicht weiter untersucht.

## Umbau (umgesetzt)

**Drei Zustände statt zwei.** `m_materialPipelineCache` kennt weiter nur „fertig“ und „gescheitert“ (null); ein
gescheiterter Build fällt an jeder Aufrufstelle wie bisher zurück (Standard-Material, eingebaute Blend-PSO,
Forward-Umleitung). Neu ist `m_materialPipelinePending` (Schlüssel = der fertig gesalzene Cache-Schlüssel) für
„läuft noch“. `GetOrBuildMaterialPipeline(…, bool* pending)` meldet das; ohne `pending` (Material-Vorschau) oder
mit `HE_MATERIAL_PIPELINE_ASYNC=0` bleibt es der blockierende Build.

**Aufteilung.** Der Cross-Compile bleibt auf dem Aufrufer (`MaterialShaderLibrary` hat keinen Mutex, siehe unten).
`newLibraryWithSource` (Vertex, dann Fragment, mit dem Baked-Plain-Rückfall) und `newRenderPipelineState…` laufen
über ihre Completion-Handler auf Metals eigenen Compiler-Threads, mehrere Materialien gleichzeitig. Ein Handler
fasst den Renderer nicht an: er legt `{Schlüssel, PSO oder null, Meldungen}` in einen Briefkasten
(`MatPipelineMailbox`, Mutex, per `shared_ptr` in den Blöcken gehalten, bei `Shutdown` geschlossen), und
`DrainMaterialPipelines()` leert ihn auf dem Main-Thread (Anfang von `EncodeFrame`, in `PumpShaderBuilds`). Log-
Zeilen und das Binary-Archive-Schreiben passieren erst dort.

**Nicht gezeichnet.** Aufrufstellen mit `pending`: Forward-Szene und Deferred-Replay (ganzes Material, auch wenn nur
eine der Varianten — opak, Blend, G-Buffer — noch baut; alle starten gleichzeitig), instanzierte Zwillinge
(der Batch wird übersprungen, nicht als Tausende Einzel-Draws gezeichnet) und Ribbon-Trails (die Spur). Der Skip ist
ein `continue` vor allem, was die Zeichnung braucht; `cMaterialInstTried` wird bei „pending“ zurückgesetzt, damit der
nächste Batch erneut fragt. **Schatten und Tiefen-Pässe laufen weiter**: ein noch nicht gezeichnetes Objekt wirft
bis zu seiner Pipeline einen Schatten.

**Hinweis.**
- Editor: `drawShaderBuildHint` (`ShaderBuildHint.h`) zeichnet „Compiling shaders  3 / 8“ unten links in den
  Scene-Viewport, solange `IRenderer::PumpShaderBuilds().pending > 0`. UI-Shot: `ui shot: the Compiling shaders hint
  over the viewport` (`HE_UI_DUMP_DIR`, `scripts/he_uishot.py`).
- Startup-Schritt: `warmupWorldMaterials(true)` beim Szenenstart des Editors wartet hinter dem Splash auf die Builds
  („Compiling shaders  3 / 8“, Fortschrittsbalken 0,90–0,99, Fenster bleibt ansprechbar, 120 s Obergrenze).
  `openScene` und das Streaming warten nicht, dort zeigt der Viewport den Hinweis. `HE_SHADER_BUILD_NOWAIT=1`
  schaltet das Warten am Start aus (um den Fall „Szene füllt sich“ zu beobachten).
- Spiel (Metal, direkt ins Fenster): `Render()` hängt dieselbe Karte als UI-Quads an den UI-Pass, solange Builds laufen
  (`emitUITextGlyphs`). **Nicht angesehen** — kein Spiel-Build in diesem Lauf.

## Messung nach dem Umbau
Gleiche Szene und Bedingungen wie oben, **kalter** Metal-Cache, Bildschirm **gesperrt** (nur CPU-Werte), ein Lauf je
Zeile außer wo zwei stehen. `sync` ist derselbe Build mit `HE_MATERIAL_PIPELINE_ASYNC=0`.

| | `WarmupWorldMaterials` gesamt | davon auf dem Main-Thread (`Metal::WarmupMaterials`) | Frames mit 24 Draws ab |
|---|---|---|---|
| sync (vorher) | 2839 / 2837 ms | 2839 / 2837 ms | Frame 2 |
| async, Startup-Schritt (Standard) | 1988 / 1452 ms (hinter dem Splash) | 137 / 104 ms | Frame 3 |
| async, ohne Warten (`HE_SHADER_BUILD_NOWAIT=1`, wie eine Szene mitten in der Sitzung) | — | 83 ms | Frame 33 (12 von 24 Draws davor) |

- Der Main-Thread blockiert nicht mehr 2,8 s, sondern ≈ 0,1 s (der Cross-Compile). Im Startup-Schritt wird die Zeit bis zur
  fertigen Szene wegen der parallelen Builds kürzer (≈ 1,5–2,0 s statt 2,8 s), und der Splash zeigt Fortschritt statt
  eines stehenden Fensters.
- Ohne Warten gibt es keinen Frame über 200 ms außer dem ersten Draw-Frame (601 ms, das Upload-Thema aus „Was die
  Frame-Spitze NICHT ist“, unverändert). Die zweite Hälfte der Szene (die instanzierten Zwillinge, erst beim ersten
  Batch angestoßen) erscheint ≈ 3 s später, wenn der kalte Cache sie noch compilieren muss.
- Warm (Cache gefüllt) nicht neu gemessen: der Cross-Compile von ≈ 85 ms pro Szenenstart bleibt auf dem Main-Thread.

## Offen / nicht belegt
- Kein Lauf mit entsperrtem Bildschirm: Frame-/GPU-Zeiten sind nicht verglichen.
- **Im Spiel gibt es keinen Startup-Schritt**, nur die Karte: `GameApplication` ruft `WarmupMaterials` nur im Streaming
  (`pollAsyncResults`), nicht nach dem Laden der Startszene. Dort erscheinen Grafik-Materialien also nach dem ersten
  Sehen, mit der Karte darüber. Ein Schritt hinter dem Splash (nach `setWorld`: `WarmupMaterials(collectAssetRefs(...))`
  plus dieselbe Warteschleife) wäre möglich, ohne Spiel-Build hier aber nicht zu prüfen.
- **Auch warm ist es ein Frame später**: eine Pipeline, die früher in ≈ 1 ms synchron fertig war, kommt jetzt über den
  Briefkasten und liegt einen Frame später im Cache. Jedes `openScene` mitten in der Sitzung lässt Grafik-Materialien
  deshalb einen Frame lang aus und zeigt kurz „Compiling shaders  0 / N“.
- Spiel-Hinweis (Metal-UI-Quads) und Deferred-Pfad nicht angesehen; Ribbons nicht getestet. Eine Szene, die zur
  Laufzeit per `openScene` oder Streaming neue Materialien bekommt, wurde nur über `HE_SHADER_BUILD_NOWAIT=1`
  nachgestellt.
- Die Zwillinge werden nicht vorgewärmt (nur beim ersten instanzierten Batch gebaut); sie kosten kalt ≈ 1,2 s im
  Hintergrund, in der Zeit fehlen diese Objekte.
- Der Cross-Compile ist weiter auf dem Aufrufer: `MaterialShaderLibrary` ist nicht als thread-sicher belegt
  (`D3DShaderPrefetch.h` sagt dasselbe). Ihn zusätzlich vom Frame zu nehmen wäre der nächste Schritt (≈ 14 ms je Material).

## Handoff
- **Topic / step:** Metal-Teil (Punkt 4) der Job-System-Übergabe, Schritt „Umbau“.
- **Branch:** `claude/metal-material-pipeline-async`, Basis `origin/release/0.8.0` (`83d00783`), PR #216.
- **Done:** Scopes, Messung, async Libraries + PSO über Completion-Handler, Drei-Zustände-Cache, Briefkasten,
  Skip an den Aufrufstellen, Editor-Hinweis + Startup-Schritt, Spiel-Hinweis, UI-Shot-Test, Messung nach dem Umbau.
- **Verified:** Release-Build Editor + Tests, Kaltläufe sync/async (siehe Tabelle), UI-Shot des Hinweises, Suite siehe PR.
- **Not verified:** siehe „Offen / nicht belegt“.
- **Traps:** `HE_MATERIAL_PIPELINE_ASYNC=0` stellt das alte Verhalten her; `HE_SHADER_BUILD_NOWAIT=1` überspringt den
  Startup-Schritt. Der Cache-Ordner für kalte Läufe ist `$(getconf DARWIN_USER_CACHE_DIR)com.apple.metal` (beiseite
  schieben, danach zurücklegen). Ein Handler darf den Renderer nie anfassen, nur den Briefkasten.
- **Next steps:** 1. mit entsperrtem Bildschirm den Viewport-Hinweis live und den Spiel-Hinweis ansehen. 2. Zwillinge
  im Startup-Schritt mitwärmen, falls das Nachladen kalt stört. 3. Cross-Compile thread-sicher machen und ebenfalls
  vom Frame nehmen. 4. Den ersten Draw-Frame (EncodeScene/RefineBounds) weiter aufschlüsseln.
