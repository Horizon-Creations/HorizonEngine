# Core-Rendering-Baseline: Partikel, Material/Shader, Texturen (Thema 349, Schritt 1), 18.11.2026

Mess- und Planungsschritt, **kein Engine-Code geändert** (`git diff --stat`: nur `scripts/perf/` und `docs/perf-audit/`).
Stand: Zweig `claude/performance-audit-core-rendering-partikel-materialien-shader` auf `3b2be9658` (= main), Metal, Release,
`HE_PROFILING=ON`, eigener Build dieses Worktrees (`cmake-build-release`, Ninja, ccache).

**Der Schritt ist `partial`**: die Königin hat den Lauf gestoppt, während die CPU-Wiederholung (Wiederholung 3) lief; die
CPU-Tabelle hat 2 statt 3 Wiederholungen je Zelle (`core_ctrl-cpu` 3). Alles andere ist vollständig (3 Wiederholungen).

## 0. Voraussetzungen, die nicht erfüllt sind

| Punkt | Stand |
|---|---|
| Streaming-Zweig Thema 326 (`claude/landscape-weltmassstab-100km`) | **nicht in diesem Zweig und nicht in `release/0.8.0`** (`git merge-base --is-ancestor 17df422a9 HEAD` = nein). Darum **keine Landscape-Szene mit großem Streaming**; die Landscape-Zelle ist die 512-m-Szene (`land512`) wie im GPU-Audit. Sobald 326 gemergt ist, fehlt eine Zelle (siehe Abschnitt 6). |
| Thema 345 „Set up the performance audit“ | offen, ohne Posts und ohne Zweig. Der Messaufbau ist deshalb der vorhandene: `scripts/he_perf_capture.py` + `scripts/perf/gpu_audit_matrix.py` (Detailed, RenderScale 2, Tonemap als Takt-Zeuge, `*.conditions.txt`). |
| `HIVE_PERF.json` | existiert nicht. |

## 1. Schon gemessen / noch ungemessen (mit Quelle)

| System | Schon gemessen | Quelle | Noch ungemessen (Stand vor diesem Schritt) |
|---|---|---|---|
| **Partikel** | nur Zähler „Partikel 0“ in der Baseline; Metal-Bucket „Shadow“ enthält die GPU-Wetterpartikel-Simulation, nie einzeln | `baseline-2026-09-27.md` §2, Tabelle D | `ParticleSystem::update`, `extractParticleBatches` (hat **keinen** Profiler-Scope, `RenderExtractor.cpp:1611`), Draw/Encode je Batch, Partikelkollision (Raycast je Partikel), Backend-Kosten außer Metal |
| | statisch (nur Lesen, „nicht gemessen“): Metal `newBufferWithBytes` je Partikel-Batch `MetalRenderer.mm:18286-18290`; GL `glGetUniformLocation` 4× je Batch `OpenGLRenderer.cpp:13920-13926` | `gpu-audit-2026-10-27.md` §2.2 Metal/GL | |
| **Material/Shader** | Wasser +7,8 ms Scene (Metal); `If` als echter Zweig; Layer-Meshes, Variation V=1/3 | `gpu-audit-2026-10-27.md` §3.3; `material-branching-2026-10-26.md` §2; `auto-landscape-material-2-audit-2026-10-21.md` §2-§4; `perf-water-cost-2026-10-21.md` | CPU je Graph-Material-Draw (Kopie+Hash des GLSL, `std::string`-Textur-Key: GPU-Audit-Funde 1 und 2, **„strukturell, nicht gemessen“**), Variantenzahl, Shader-Kompilierung/Cache-Treffer, Kaltstart je Backend |
| | Kaltstart des Editors 6,7 s, Ursache offen (`WarmupWorldMaterials`-Scope eingebaut, nie ausgewertet) | `cpu-threading-optimization-2026-10-22.md:69` | |
| **Texturen** | Formate (RGBA8/ASTC/BC7/BC3), Laden, RSS, Encoder-Zeit; BCn-Array-Dekodierung 440 ms/Frame (behoben); 3D-Rauschtextur 10,7 s → 0,45 s Erzeugung, ~1,5 ms Sampling | `auto-landscape-material-2-audit-2026-10-21.md` §5; `noise-texture-b7-2026-09-30.md` | Upload je Backend außer Metal, Mip-Erzeugung CPU, UI-Texturen, Texturen-Cache je Draw (Fund 2), **Streaming/Residency: es gibt keine Textur-Mip-Streaming-Schicht** (nur asset-weises Lazy-Laden aus dem Pak, `ContentManager.h:381-388`); das ist ein Befund, keine Lücke |
| **Landscape (Streaming)** | Terrain-LOD/-Chunking statisch, D3D11-Zähler | `landscape-streaming-lod-audit-2026-10-27.md`; `world-streaming-baseline-2026-10-06.md` | große Landscapes (Thema 326), siehe oben |
| **Nicht-Metal-Backends** | nichts gemessen (kein Windows/Vulkan/GL-Live auf diesem Mac) | `gpu-audit-2026-10-27.md` §6 | OpenGL live läuft hier in den Timeout (400 s), nur der Dump-Pfad geht; D3D11/D3D12/Vulkan brauchen NN-WS03 |

## 2. Messszenen und Aufrufe

Gemeinsam: privates Projekt, **nie** `~/HorizonEngineProjects/Catania` direkt. Einmalig:

```
mkdir -p /tmp/pa349/proj && rsync -a --exclude Export --exclude Exports --exclude Saved ~/HorizonEngineProjects/Catania/ /tmp/pa349/proj/Catania/
rm -rf /tmp/pa349/proj/Catania/Content/Engine        # sonst überschreibt die Projektkopie von Water.hasset das ausgelieferte
```

Basis aller Partikel-/Material-Zellen ist `docs/perf-audit/scenes/autoland128.hescene` (neu: `gen_autoland_scene.py --extent 128 --meshes none`,
Himmel ohne Wolken, Auto-Landscape-Terrain). **Warum nicht ein nacktes Terrain:** der erste Durchlauf (`raw-core-rendering-run1-bare-terrain/`)
hatte nur eine billige Szene; die GPU takten dann herunter, der Tonemap-Zeuge lag bei 0,8 bis 1,2 ms statt 0,35 ms und alle GPU-Zahlen waren
nicht vergleichbar (die bekannte Falle). Auf dem Auto-Landscape-Terrain (Scene-Pass ~13 ms bei RenderScale 2) liegt der Zeuge bei 0,34 bis 0,41.

Der Treiber ist `scripts/perf/gpu_audit_matrix.py` mit der neuen Zellgruppe `--group core`; die Szenen erzeugt er selbst
(`scripts/perf/gen_reference_world.py`, neu: `--particles`, `--material`).

| Zelle | Szene | Zweck |
|---|---|---|
| `core_ctrl` | `autoland128` allein | Kontrolle für Partikel und Material |
| `part_100` | + 100 Emitter (je 100/s, Leben 1..2 s, Pool 400) = **15 000** lebende Partikel | Partikel, klein |
| `part_1000` | + 1000 Emitter = **150 000** Partikel (Zähler `particles` im Dump als Zeuge) | Partikel, groß |
| `mat_pbr` | 1500 Würfel (150 m), eingebautes PBR | Kontrolle Material |
| `mat_disp` | dieselben mit `DisplacementDemo` (`{0x601,1}`, Graph, billiges Fragment) | Graph-Material-Overhead |
| `mat_water` | dieselben mit `Water` (`{0x600,1}`, Graph, teures Fragment) | Graph-Material, teuer |
| `land512-tex128` | 512-m-Landscape, ausgelieferte 128-px-Platzhalter-Arrays | Textur, klein |
| `land512-tex2k` | dieselbe Szene, Projekt-Override mit 3 Arrays 2048² × 5 Slices RGBA8, 12 Mips (335 MB) aus `landscape_tex_gen <proj>/Content/Engine/Textures/Landscape --pack <leeres Verzeichnis> --size 2048` (synthetischer Inhalt, nichts im Repo) | Textur, groß |

```
# GPU je Pass (Detailed, RenderScale 2), 3 Wiederholungen, Reihenfolge wechselt
python3 scripts/perf/gpu_audit_matrix.py run --group core --scenes /tmp/pa349/scenes2 --out docs/perf-audit/raw-core-rendering \
    --reps 3 --load-limit 5 --proj /tmp/pa349/proj/Catania/HEProject.heproj --only core_ctrl,part_100,part_1000,mat_pbr,mat_disp,mat_water
# CPU-Scopes eines normalen Frames (kein Detailed, RenderScale 1)
python3 scripts/perf/gpu_audit_matrix.py run ... --only <wie oben> --cpu --scale 1 --suffix=-cpu
# Textur-Zellen: zweimal, mit je einem Projekt (Catania bzw. Catania2k mit dem Override)
python3 scripts/perf/gpu_audit_matrix.py run --group core ... --only land512 --suffix=-tex128   # --proj .../Catania
python3 scripts/perf/gpu_audit_matrix.py run --group core ... --only land512 --suffix=-tex2k    # --proj .../Catania2k
# Tabellen
python3 scripts/perf/gpu_audit_matrix.py table --group core --out docs/perf-audit/raw-core-rendering --scopes ParticleSystem,RenderExtractor::extract,Metal::EncodeScene,Metal::EncodeShadowMap,SceneSystemsTick,Render
python3 scripts/perf/gpu_audit_matrix.py table ... --match '*-cpu' --scopes <wie oben>
```

Kamera `0,25,90,0,-0.25`, Fenster 2840×1528, vsync aus, Warm-up 300 + 400 Frames, Editor-Voreinstellungen (Forward, SSAO und Bloom an, GI aus).

## 3. Bedingungen

| Punkt | Wert |
|---|---|
| Gerät | MacBook Air `Mac17,4`, Apple M5, macOS 27.0, Metal (`session.backend`) |
| Build | `3b2be9658`, Release, `HE_PROFILING=ON`, Ninja, eigener Worktree-Build (nicht das Deploy des Haupt-Checkouts, das ist `release/0.8.0`) |
| Energie | Netzteil, Stromsparmodus **aus** (`pmset -g`, je Lauf in `*.conditions.txt`) |
| Bildschirm | nicht gesperrt (`CGSSessionScreenIsLocked` fehlt in `ioreg`), jeder Lauf unter `caffeinate -u` |
| Last | 1-Minuten-Load vor jedem Lauf 2,2 bis 4,9 (`load before start` je Datei); der Treiber wartet bis < 5. Fremdlast: andere Bienen auf demselben Mac, GPU-Auslastung vor dem Start 36 % bei einer Stichprobe, sonst nicht protokolliert |
| Takt-Zeuge | Tonemap p50 über die 24 Detailed-Läufe der Hauptzellen: **0,34 bis 0,47 ms**, Median je Zelle 0,35 (`core_ctrl`, `mat_water`), 0,37 (`tex128`), 0,39 bis 0,46 (Partikel/`mat_pbr`/`mat_disp`/`tex2k`: **bis +20 % über 0,35, siehe Lesart**) |
| Modus | Detailed (GPU je Pass, exklusiv): `Render`/`cpuMs` sind dort durch die Pass-Waits aufgebläht. CPU-Zahlen stammen aus den `-cpu`-Läufen (kein Detailed), außer wo „Detailed“ dabeisteht |

## 4. Baseline-Zahlen

Alle Zahlen: Median über die Wiederholungen, in Klammern min..max. Rohdaten: `raw-core-rendering/<zelle>-r<n>.summary.json`
(Pass-Perzentile, Scopes, Zähler), `.log`, `.conditions.txt`; fertige Tabellen `tables-detailed.md` und `tables-cpu.md` (je Lauf und Median).
Die 3-5-MB-Profile liegen gzip-komprimiert außerhalb von git in `out/perf-audit/2026-11-18-core-rendering/`.

### 4.1 Partikel (CPU: `-cpu`-Läufe, 2 Wiederholungen; GPU: Detailed, 3 Wiederholungen)

| Zelle | Emitter / Partikel | `ParticleSystem` ms | `RenderExtractor::extract` ms | `Metal::EncodeScene` ms | `Render` ms | fps (normal) | GPU Scene / Σ Pässe ms |
|---|---|---|---|---|---|---|---|
| core_ctrl | 0 / 0 | 0,00 | 0,09 | 0,09 | 0,83 | 60,0 | 13,26 / 16,02 |
| part_100 | 100 / 15 000 | 0,17 | 0,25 | 0,98 | 1,84 | 60,1 | 13,95 [13,76..22,93] / 16,84 |
| part_1000 | 1000 / 150 000 | **1,72** | **1,59** | **10,24** | **12,66** | **43,3** | 16,52 / 19,85 |

Lesart (gemessen): bei 150 000 Partikeln kostet die CPU pro Frame rund **13 ms mehr** (`ParticleSystem` 1,7 + Extract 1,5 + `EncodeScene` +10,2 ms),
die fps fallen von 60 auf 43. Der größte Posten ist das **Encodieren**: 1000 Batches (ein Draw je Emitter, Draw-Zähler 1004 gegen 4) = ~10 µs je Batch.
Die Extraktion der Partikel hat keinen eigenen Scope; ihr Anteil steckt in `RenderExtractor::extract` (Differenz zur Kontrolle, +1,5 ms). GPU: +3,3 ms Scene
bei 150 000, +0,7 ms bei 15 000 (dort ein Ausreißer 22,9 in einer Wiederholung). Eine Aussage über **Kollision** ist nicht möglich: das Inline-Szenenformat kennt
`collisionEnabled` nicht (nur über ein Partikel-Graph-Asset erreichbar).

### 4.2 Material / Shader

| Zelle | Draws | `Metal::EncodeScene` ms (CPU) | `RenderExtractor::extract` ms | `Render` ms | GPU Scene ms (Detailed) | Σ Pässe | fps |
|---|---|---|---|---|---|---|---|
| mat_pbr | 6 | 0,16 | 0,32 | 1,30 | 12,57 [12,51..12,94] | 16,20 | 59,9 |
| mat_disp | 6 | 0,17 | 0,32 | 1,33 | 13,01 [12,26..16,39] | 16,78 | 59,7 |
| mat_water | **1344** | **2,57** | 0,52 | 4,07 | **23,03** [21,93..24,15] | 26,41 | 59,8 (Detailed 29,4) |

- Ein billiges Graph-Material (`DisplacementDemo`) kostet gegenüber dem eingebauten PBR **nichts messbar** (Scene +0,4 ms im Streubereich 12,3..16,4; CPU gleich).
- `Water` auf 1500 Würfeln: **+10,5 ms Scene** (das ist Fragmentarbeit, vgl. `gpu-audit-2026-10-27.md` §3.3) und **1344 statt 6 Draws**: die Würfel werden nicht gebündelt, `EncodeScene` steigt von 0,16 auf 2,57 ms (~1,8 µs je Draw). Das Bild ist ein Extremfall, aber es zeigt, dass transparente Graph-Materialien je Objekt einen Draw kosten.
- **Kaltstart / Shader-Kompilierung** (`SceneOpenTiming` im Log, Spalte `warmup`): `mat_water` Erstlauf **3688 ms** (`raw-core-rendering-run1-bare-terrain/mat_water-r1.log`; „warmed up 4 material pipeline(s)“), danach 513 bis 575 ms in den Wiederholungen 2 und 3 (System-Shader-Cache des Betriebssystems, die Engine schaltet ihr eigenes Metal-Archiv ab, `MetalRenderer`-Log „on-disk material pipeline archive disabled“). `mat_pbr`/`mat_disp`: 31 bis 67 ms. **Variantenzahl** wurde nicht gezählt: der Log nennt nur die vorab gebauten Pipelines („warmed up N“, 2 bis 4 hier).

### 4.3 Texturen (512-m-Landscape, Detailed, 3 Wiederholungen)

| Zelle | Arrays im Speicher | GPU Scene ms | Σ Pässe ms | Tonemap-Zeuge ms | `RenderExtractor::extract` | Szene öffnen total ms (warmup) |
|---|---|---|---|---|---|---|
| land512-tex128 (ausgeliefert) | ~0,05 MB je Array (128², Platzhalter) | **16,34** [16,18..16,34] | 19,24 | 0,37 | 0,07 | 379..491 (354..453) |
| land512-tex2k | 3 × 106,7 MiB = 335 MB RGBA8, 12 Mips | **13,44** [13,39..13,49] | 16,39 | 0,41 | 0,13 | 386..389 (363..371) |

**Zwei nicht erklärte Befunde, beide für den Textur-Detailschritt:**
1. Die 2K-Arrays sind **schneller** als die 128-px-Platzhalter (Scene 13,4 gegen 16,3 ms; Tonemap-Zeuge 0,41 gegen 0,37, also gleicher Takt innerhalb ±10 %). Mögliche, **ungeprüfte** Ursache: Platzhalter ohne Mip-Kette (Minification, Cache-Fehlzugriffe).
2. `land512` mit Platzhaltern kostet heute 16,3 ms Scene; der GPU-Audit vom 27.10. (`91e2c849`) maß für dieselbe Szene **12,88 ms** bei Zeuge 0,34. Das sind +3,4 ms bei gleichem Takt zwischen `91e2c849` und `3b2be9658` oder durch das private Projekt. **Nicht untersucht**, kein Rückschluss auf eine Regression ohne A/B.
RSS (`rssMB.max`) schwankt zwischen 1044 und 2141 MB in denselben drei Platzhalter-Läufen: die 1-Hz-Messung sieht unter macOS nur nicht komprimierte Seiten, **nicht verwertbar** (wie schon in `auto-landscape-material-2-audit` §5).

## 5. Was an dieser Baseline nicht belastbar ist

- Die CPU-Tabelle hat 2 statt 3 Wiederholungen (Lauf gestoppt); Streuung innerhalb der Paare ist klein (≤ 0,5 ms), aber ein Ausreißer wäre nicht gefangen.
- Takt-Zeuge: Partikel-, `mat_pbr`-, `mat_disp`- und `tex2k`-Zellen liegen 10 bis 30 % über dem Zeugenmittel 0,35 (Tonemap 0,39 bis 0,46). GPU-Differenzen unter ~1 ms in diesen Zeilen sind **nicht** belastbar; die großen (Partikel +3,3 ms, Wasser +10,5 ms) sind es. CPU-Scopes sind vom Takt unabhängig.
- Fremdlast durch andere Bienen auf demselben Mac (Load 2,2 bis 4,9, GPU-Auslastung nicht je Lauf mit Tonemap belegt).
- Detailed-Läufe verändern `Render` und `cpuMs` (Pass-Waits); darum die getrennten `-cpu`-Läufe.
- Nur Metal. OpenGL live nicht möglich (Timeout), Vulkan/D3D11/D3D12 brauchen NN-WS03: Windows-Zeilen **offen**, nicht als erledigt melden.
- Der erste Durchlauf (`raw-core-rendering-run1-bare-terrain/`) bleibt als Beleg für die Takt-Falle im Repo; seine GPU-Zahlen sind nicht zu verwenden, seine Kaltstart-Zeiten (`SceneOpenTiming`) sind es.

## 6. Nächste Schritte (nicht in diesem Schritt)

1. Partikel-Detailschritt: `Metal::EncodeScene` je Batch (10 µs), Batch-Zusammenlegung statt Draw je Emitter; `extractParticleBatches` einen Scope geben; Kollisionszelle über ein Partikel-Graph-Asset anlegen.
2. Material-Detailschritt: CPU je Graph-Draw (GPU-Audit-Funde 1 und 2) mit `mat_disp`/`mat_water` als Zellen; Shader-Varianten und Kompilierzeit je Backend zählen; `WarmupWorldMaterials`-Scope des Editor-Kaltstarts auswerten.
3. Textur-Detailschritt: die beiden Rätsel aus 4.3 (Mips der Platzhalter, +3,4 ms gegenüber 27.10.) per A/B klären; BCn-Variante des 2K-Satzes; Windows-Backends.
4. Sobald Thema 326 gemergt ist: Landscape-Zelle mit großem Streaming ergänzen (`gen_reference_world.py`/`gen_autoland_scene.py` um die Pack-Landscape erweitern; die Zelle in `CORE_CELLS`).

## Handoff

- **Topic / step:** 349, Schritt 1 (Messszenen und Baseline). Status **partial** (Königin hat gestoppt).
- **Branch / commit:** `claude/performance-audit-core-rendering-partikel-materialien-shader`, Commit siehe `git log` (nicht gepusht).
- **Done:** Liste gemessen/ungemessen (§1); Szenen + Aufrufe (§2); Baseline Partikel/Material/Textur auf Metal mit Rohdaten (§4); `gen_reference_world.py --particles/--material`, `gpu_audit_matrix.py --group core/--cpu/--scopes/--load-limit`, Median-Tabelle; Szene `docs/perf-audit/scenes/autoland128.hescene`.
- **Files:** `scripts/perf/gen_reference_world.py`, `scripts/perf/gpu_audit_matrix.py`, `docs/perf-audit/core-rendering-baseline-2026-11-18.md`, `docs/perf-audit/raw-core-rendering/`, `docs/perf-audit/raw-core-rendering-run1-bare-terrain/`, `docs/perf-audit/scenes/autoland128.hescene`. Kein `src/`.
- **Verified:** Lauf mit dem dokumentierten Treiber (rund 60 Läufe, alle `rc=0`); `py_compile` beider Skripte; Zähler-Zeuge `emitters`/`particles` (100 / 15 000, 1000 / 150 000); Tonemap-Zeuge je Zelle.
- **Not verified:** Wiederholung 3 der CPU-Läufe (gestoppt); jede Nicht-Metal-Zahl; Ursachen der zwei Textur-Rätsel; Variantenzahl; Partikelkollision; Thema 326 nicht im Zweig, keine Streaming-Zelle.
- **Decisions taken:** Basis `autoland128` statt nacktem Terrain (Takt-Falle); Inline-Emitterformat statt Asset (keine Projektdateien nötig); Textur-Override über `landscape_tex_gen --pack <leer> --size 2048` im privaten Projekt.
- **Pending:** CPU-Wiederholung 3; Windows-Zeilen; Zelle für 326.
- **Traps:** `--suffix=-x` (argparse); Szenenordner `--scenes` cacht Szenen nach Dateiname (bei neuer Basis neuen Ordner nehmen, sonst alte Szene); Projektkopie muss `Content/Engine` verlieren; der Treiber überschreibt die Startszene des Projekts (nur private Kopie benutzen); `landscape_tex_gen` und Editor teilen `out/deploy` nicht, aber ein Neubau des Editors überschreibt das Deploy während Läufen.
- **Next steps:** §6.
