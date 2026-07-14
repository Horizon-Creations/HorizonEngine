# Windows-GPU-Verifikations-Checkliste — Block A (D3D11 / D3D12 / Vulkan)

Alles unter „Block A" wurde **blind auf macOS** entwickelt (GL+Metal sind die Referenz; D3D11/D3D12/Vulkan
laufen nur auf Windows). Die CI verifiziert **nur, dass es kompiliert** — NICHT, dass es korrekt rendert.
Diese Liste ist der **B3-Schritt**: die tatsächliche GPU-Prüfung auf deiner Windows-Hardware.

Stand: A1 ✅, A2 ✅, A3 ✅ (alle compile-grün + adversariell reviewt); A4/A5 noch offen (siehe unten).

---

## 0. Setup (einmalig)

1. **Editor bauen/holen:** entweder das CI-Artefakt `HorizonEditor-windows-x64` (Actions → letzter grüner Run
   → Artifacts) oder lokal `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build`.
2. **Backend umschalten:** im Editor die **RHI-Auswahl** (Settings/Preferences → „Renderer/RHI"; intern
   `GlobalState::getSelectedRHI()`), Editor neu starten. Alternativ headless per Umgebungsvariable
   `HE_DUMP_RHI=D3D11|D3D12|Vulkan` (+ `HE_DUMP_PATH=out.bmp HE_DUMP_QUIT=1` für einen Screenshot-Dump).
3. **Referenz:** dieselbe Szene zusätzlich unter **OpenGL** (läuft auch auf Windows) rendern — das ist die
   „richtige" Optik. Jede D3D/Vulkan-Ausgabe muss **pixel-nah zu OpenGL** aussehen.
4. **Validierungs-Layer einschalten** (fängt die meisten blinden Fehler ab, auch ohne sichtbaren Artefakt):
   - **D3D12/D3D11:** Debug-Build nutzen (aktiviert den D3D-Debug-Layer) und das **Visual Studio Output-
     Fenster / DebugView** auf `D3D12 ERROR`/`WARNING` beobachten.
   - **Vulkan:** Vulkan-SDK installiert → Validation-Layer via `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`
     oder vkconfig; auf `VUID-…`-Meldungen in der Konsole achten.
   - **Keine Validation-Errors** ist genauso wichtig wie ein korrektes Bild.

**Vorgehen pro Feature:** Testszene laden → nacheinander D3D11 / D3D12 / Vulkan → mit dem OpenGL-Bild
vergleichen → auf die unten genannten „richtig/falsch"-Symptome achten → pro Backend 1 Screenshot.

---

## A1 — Basecolor-Texturen (gebackene Mesh-Textur)

**Setup:** eine Szene mit mindestens einem Mesh, das eine **Basecolor-/Albedo-Textur** hat (z. B. ein
importiertes texturiertes Modell; kein Node-Graph-Material nötig).

- ✅ **Richtig:** das Mesh zeigt seine Textur — identisch zu OpenGL (Farben, UV-Ausrichtung, Kachelung).
- ❌ **Falsch-Symptome:** flache/graue Fläche (Textur nicht gebunden), falsche Farbe (sRGB/Format-Bug),
  vertauschte/gespiegelte UVs, schwarz (Sampler/SRV nicht gesetzt), Flackern.
- **Alle drei Backends** müssen die Textur zeigen.

## A2 — MaterialComponent-Override + Live-Invalidate

**Setup:** ein texturiertes Mesh (wie A1). Eine `MaterialComponent` mit einem Override-Material zuweisen.

- ✅ **Richtig:** das Override-Material **ersetzt** die gebackene Textur **vollständig** — exakt wie GL:
  hat das Override keine Textur → Mesh wird **flach/weiß** (NICHT Rückfall auf die gebackene Textur).
- ✅ **Live-Update:** Material im Editor ändern (Farbe/Textur) → das Mesh aktualisiert sich **sofort**
  (Invalidate-Pfad). Besonders: **Terrain sculpten** (feuert `InvalidateMesh` pro Chunk/Frame) darf **nicht
  crashen und nicht langsam leaken** (D3D12 Slot-Free-List / Vulkan `vkFreeDescriptorSets`).
- ❌ **Falsch-Symptome:** Override wirkt nicht; altes Material „klebt" nach Änderung; Crash/Ruckeln beim
  Sculpten; Descriptor-Heap-Erschöpfung nach längerem Sculpten (D3D12).

## A3 — Echtes GPU-Instancing (opaker Geometrie-Pass)

**Setup:** eine Szene mit **vielen Instanzen desselben Meshes + Material** — am einfachsten **Foliage**
(Gras/Bäume) oder viele Kopien eines Prefabs, sodass ein instanzierter Batch entsteht.

- ✅ **Richtig:** **exakt dasselbe Bild wie vor A3 / wie unter OpenGL** — alle Instanzen an den korrekten
  Positionen/Rotationen/Skalierungen, korrekt beleuchtet & texturiert. A3 ist eine **reine Perf-Optimierung**;
  das Bild darf sich **nicht** ändern.
- ✅ **Perf (der eigentliche Gewinn):** Draw-Call-Zahl sinkt drastisch. Im **Profiler (F9)** die
  „Render/Draws"-Zähler vor/nach vergleichen, ODER via GPU-Capture (PIX für D3D12, RenderDoc für
  Vulkan/D3D11): ein instanzierter Batch = **1 DrawIndexedInstanced statt N**.
- ❌ **Falsch-Symptome** (die kritischen, blind nicht gefundenen Risiken):
  - **Verschobene/rotierte/„explodierte" Instanzen** → Matrix-Major-ness-Bug (mvp/model transponiert).
  - **Nur 1 Instanz** oder fehlende Instanzen → Instanz-Puffer/Offset falsch.
  - **Falsche Beleuchtung/Farbe** auf Instanzen → Normalen (model-Matrix) oder Param-Cbuffer falsch.
  - **HDR-Pfad** (PostFX an): Instanzen im HDR-Zielformat prüfen — Bild darf nicht kaputt/„device removed"
    sein (Format-Mismatch-Fallback ist abgesichert, aber real prüfen).
  - **Transparente instanzierte Objekte** (Opacity < 1): sollen weiterhin korrekt sortiert/geblendet sein
    (die laufen bewusst über den alten Schleifen-Pfad).
- **Wichtig:** in **allen drei** Backends **und** mit **PostFX/HDR an UND aus** prüfen (LDR + HDR-Pipelines).

---

## A4 — Material-Node-Graph-Shader auf D3D/Vulkan — ⏳ NOCH NICHT IMPLEMENTIERT

Aktuell rendert **jedes über den Material-Node-Graph gebaute Material auf D3D/Vulkan gar nicht** wie gebaut
(die Renderer rufen `MaterialShaderLibrary` nie auf — nur GL+Metal tun das). Die Cross-Compilation
(GLSL→HLSL/SPIR-V via glslang/SPIRV-Cross) existiert bereits; es fehlt der Per-Material-Pipeline-Pfad in den
drei Backends. **Sobald implementiert, hier prüfen:** ein Graph-Material (z. B. mit Emissive/Fresnel/Textur-
Nodes) an ein Mesh hängen → muss auf D3D11/D3D12/Vulkan **identisch zu GL/Metal** aussehen; Material-Parameter
live ändern → sofortiges Update; Graph-Texturen (heTexP0..3) korrekt.

## A5 — Sky/Nebula v2–v3.4 + physikalische Atmosphäre auf D3D/Vulkan — ⏳ NOCH NICHT IMPLEMENTIERT

`sky.frag` (HLSL/Vulkan) hat noch **Nebula v1 + altes Gradient-`skyColor`**. GL/Metal haben Nebula v2→v3.4,
Rayleigh/Mie/Ozon-Streuung, 22°-Halo/Mond-Corona, God-Rays, Regenbogen. **Sobald portiert, hier prüfen:**
Himmel unter D3D/Vulkan gegen GL vergleichen — Nebula-Struktur, Tag-/Nacht-Atmosphärenfarbe, Halo um Sonne/
Mond, God-Rays, Regenbogen müssen matchen (Environment-Tab „Night Sky"-Regler durchspielen).

---

## Rückmeldung an mich

Pro Feature × Backend hilfreich: **(1)** Screenshot (idealerweise D3D neben OpenGL), **(2)** ob
Validation-/Debug-Layer-Fehler kamen (Text), **(3)** Crashes (der `$TMPDIR`/`%TEMP%`-`he_crash_*`-Report bzw.
Callstack). Bei A3 zusätzlich die Draw-Call-Zahl aus dem Profiler (vorher/nachher). Damit kann ich blinde
Bugs gezielt fixen.
