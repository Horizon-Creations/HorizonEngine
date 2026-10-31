# Metal: Material-Pipelines beim Szenenöffnen, Messung (2026-10-31)

Folge von Punkt 4 aus `docs/jobsystem-mac-handoff-2026-10-31.md` (Branch `claude/jobsystem-audit-world-streaming`,
PR #215). Dort steht: `WarmupMaterials` baue Pipeline und Blend-Zwilling synchron, 0,4–0,7 s je Material, Plan
„auf den Pool“. Bevor das umgebaut wird, steht hier, was gemessen wurde. Dieser Zweig enthält **nur die
Profiler-Scopes** (`MetalRenderer.mm`), keinen Umbau.

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

## Entwurf für den Umbau (nicht umgesetzt)
- Die Metal-Phasen brauchen den Pool nicht: `newLibraryWithSource:options:completionHandler:` und
  `newRenderPipelineStateWithDescriptor:completionHandler:` laufen auf Metals eigenen Compiler-Threads. Das
  deckt 2,8 von 2,83 kalten Sekunden und umgeht die QoS-Frage.
- Der Cross-Compile bleibt auf dem Aufrufer: `MaterialShaderLibrary` (`src/HE_Rendering/src/material/`) hat
  keinen Mutex, `D3DShaderPrefetch.h` hält sie ebenfalls für nicht belegt nebenläufig. Ihn auf den Pool zu
  legen heißt, sie erst thread-sicher zu machen (oder pro Job eine eigene Instanz).
- Der Draw braucht einen „noch nicht fertig“-Zustand je Cache-Schlüssel (Eintrag = `building`), und jede
  Aufrufstelle von `GetOrBuildMaterialPipeline` (Szene, Vorschau, Deferred, instanziert) muss damit umgehen.
  Das ist eine sichtbare Verhaltensänderung: ein Material erscheint erst, wenn seine Pipeline da ist.
- Preis/Nutzen: gewonnen wird der **kalte** Fall (erster Start, Engine-Update) und ≈ 14 ms je neues Material
  nur, wenn auch der Cross-Compile weg vom Frame kommt. Warm ist es ein 85-ms-Stall beim Szenenstart.

## Handoff
- **Topic / step:** Metal-Teil (Punkt 4) der Job-System-Übergabe, Schritt „erst mit Scopes belegen“.
- **Branch:** `claude/metal-material-pipeline-async`, Basis `origin/release/0.8.0` (`83d00783`).
- **Done:** Scopes (siehe oben), Messung kalt/warm, Aufschlüsselung der Phasen.
- **Verified:** Release-Build des Editors, drei Capture-Läufe (kalt, 2× warm), Dumps ausgewertet. Keine Funktionsänderung.
- **Not verified:** jede Aussage über Frame-/GPU-Zeit (Bildschirm war bei allen Läufen **gesperrt**; die Werte
  hier sind CPU-seitig und davon nicht betroffen). Nur eine Maschine (Apple Silicon, 10 Kerne). Kein Lauf mit
  `precompiled` (Pak-Material), dort entfällt der Cross-Compile.
- **Decisions taken:** nur Scopes, kein Umbau; Cross-Compile bleibt im Aufrufer.
- **Decisions suggested, not taken:** async Libraries/Pipeline per Completion-Handler mit `building`-Zustand
  (siehe Entwurf), nur wenn der kalte Erststart die Mühe wert ist.
- **Traps:** der Cache-Ordner heißt `com.apple.metal` im `DARWIN_USER_CACHE_DIR` und wird von allen Prozessen
  ohne Bundle-ID geteilt; für einen kalten Lauf beiseite schieben und danach zurücklegen. Das erste Draw-Frame-
  Problem (EncodeScene 350–540 ms) ist nicht dieses Thema.
- **Next steps:** 1. entscheiden, ob der kalte Erststart den Umbau trägt. 2. Falls ja: erst `GetOrBuildMaterialPipeline`
  aufteilen (Cross-Compile / Build), dann den `building`-Zustand je Aufrufstelle. 3. Den ersten Draw-Frame
  (EncodeScene/RefineBounds) mit Scopes weiter aufschlüsseln.
