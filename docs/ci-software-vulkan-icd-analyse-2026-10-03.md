# CI: Software-Vulkan-ICD für echte Bildtests — Analyse (Thema 122, Schritt 1)

Stand 03.10.2026, Zweig `claude/ci-software-vulkan-icd-lavapipe-swiftshader-fuer-echte-bildt`,
Basis `9a1cc950` (main). **Nur Analyse und Vorschlag, kein Workflow und kein Engine-Code geändert.**

## Kurzfassung

- **Empfehlung: Mesa lavapipe auf dem Linux-Runner, als eigener, paralleler CI-Job.**
  Ubuntu 24.04 (`ubuntu-latest`) liefert über noble-updates Mesa 25.2.x
  (`mesa-vulkan-drivers`), und lavapipe kann seit Mesa 24.1 `VK_KHR_acceleration_structure` +
  `VK_KHR_ray_query`. Damit läuft auf der CPU nicht nur Nebula und Clustered Lighting, sondern
  sogar der **Hardware-RT-Pfad** der GI (`gi_*_hw.comp`), den heute nur NN-WS03 (RTX 4070) je
  gesehen hat. (Quellenangaben, hier **nicht gemessen**: auf diesem Mac gibt es weder Container
  noch lavapipe.)
- **Kein Xvfb nötig, vermutlich.** Das gepinnte SDL 3.2.14 hat im Offscreen-Videotreiber einen
  Vulkan-Weg über `VK_EXT_headless_surface` (`src/video/offscreen/SDL_offscreenvulkan.c`, im
  FetchContent-Checkout selbst nachgelesen). `SDL_VIDEO_DRIVER=offscreen` + lavapipe ergibt
  Surface und Swapchain, die der `VulkanRenderer` zwingend braucht. Rückfall: `xvfb-run` mit
  X11-WSI.
- **Die Bildmaschine gibt es schon.** Der Editor-Dump (`HE_DUMP_PATH` + `HE_DUMP_RHI=Vulkan`,
  `IRenderer::CaptureViewport`) und die Zeugen-Szenen `SKYTEST`/`NEBULA`, `MANYLIGHTS`, `GI`,
  `GIREFLTEST` sind backend-neutral. Es fehlen: zwei CMake-Korrekturen im Editor (Linux), ein
  Linux-Treiberskript statt `he_shot.py` (das braucht `sips` und Metal), A/B-Bewertung in Python,
  eine Validierungs-Allowlist.
- **Deferred ist auf Vulkan kein Bildtest.** Der VulkanRenderer hat keinen Deferred-Composite
  (`VulkanRenderer.cpp:898`); `RENDERPATH=Deferred` läuft dort als Forward. Einen echten
  Deferred-Bildtest gibt dieselbe Mesa-Installation über **llvmpipe (OpenGL 4.5)** her. Das ist
  der GL-Renderer, der heute in CI gebaut, aber nie ausgeführt wird. Als Beigabe empfohlen, nicht
  als Kern.
- **Aufwand Schritt 2 (Schätzung):** 1,5–3 Arbeitstage bis ein Job mit Nebula + Clustered + GI
  grün und aussagekräftig läuft; die größte Unbekannte ist die CPU-Laufzeit pro Bild. Erster
  Unterschritt deshalb: **ein einziger SKYTEST-Schuss auf lavapipe messen**, bevor Weiteres gebaut
  wird.

## 1. Ist-Stand

### 1.1 CI (`.github/workflows/ci.yml`)

| Job | Vulkan | Grafik zur Laufzeit |
|---|---|---|
| Windows | SDK 1.4.313.0 installiert, `HE_VULKAN_ENABLED=TRUE` erzwungen, **nur kompiliert** | D3D11/D3D12 über **WARP** in `he_tests` |
| Linux | bewusst ohne Vulkan („Vulkan is optional and left out"), baut GL | **keine** |
| macOS | ohne Vulkan | MSL-Offline-Compile, keine Bilder |

Laufzeiten des letzten main-Laufs (Run 37013856695, 02.10.): Linux 42 min (Build 27 min, Tests
1,5 min, App-Runtimes 12 min), macOS 35 min, **Windows 76 min**. Windows ist der lange Pol; ein
paralleler Linux-Job von 30–40 min verlängert den Lauf nicht. Das Repo ist öffentlich, die
Standard-Runner-Minuten kosten nichts.

### 1.2 Vulkan-Prüfungen heute

- C++: MSVC-Kompilat im Windows-Job; lokal clang `-fsyntax-only` gegen MoltenVK.
- Shader: SPIRV-Cross-Reflexion jedes Material-Knotens gegen `HE::vkmat::kBindings`
  (`VulkanMaterialLayout.h`) in `test_material_graph.cpp`, „die eine Vulkan-Prüfung ohne Gerät".
- Laufzeit: nur manuelle Läufe auf NN-WS03 (z. B. `docs/gi-ddgi-material-path-analysis-2026-10-02.md`
  §7.2). Offene Pixel-Zeugen, die genau auf dieses Thema verweisen:
  - Clustered: `docs/clustered-lighting-forward-plan-2026-10-01.md` §3.4 („CI hat kein Vulkan-ICD").
  - GI/DDGI: `docs/gi-ddgi-material-path-analysis-2026-10-02.md` §7.3 („s. Thema 122").
  - Nebula: `docs/nebula-backend-parity-analysis-2026-10-01.md` („nie als Bild geprüft").

### 1.3 Was der VulkanRenderer zum Start braucht

`VulkanRenderer::Initialize` (`VulkanRenderer.cpp:168 ff.`):

1. `createInstance`: Extensions aus `SDL_Vulkan_GetInstanceExtensions`, API 1.2 wenn der Loader
   es kann. **Validation-Layer ist fest an** (`gpuDebug = true`, Z. 966), sobald
   `VK_LAYER_KHRONOS_validation` installiert ist; Meldungen gehen als „Vulkan validation: …" ins Log.
2. `createSurface`: `SDL_Vulkan_CreateSurface`, ohne Surface Abbruch.
3. `pickPhysicalDevice`: **`devs[0]`**, keine Auswahl.
4. `createDevice`: Queue mit Present-Support; RT-Probe (`VK_KHR_ray_query` + AS + deferred host
   ops + BDA, Z. 1076 ff.), abschaltbar mit `HE_GI_FORCE_SW=1` (Z. 1089). Log:
   „GI hardware ray tracing available (VK_KHR_ray_query)" bzw. „GI uses software ray tracing".
5. `createSwapchain`: bevorzugt `B8G8R8A8_UNORM`, Present-Mode **FIFO** (vsync) oder IMMEDIATE,
   nimmt `currentExtent`, wenn gesetzt.

Ein Headless-Gerät ganz ohne Surface ist im Renderer also nicht vorgesehen. Daher der Weg über
eine Headless-Surface (SDL offscreen) oder ein virtuelles X (Xvfb).

### 1.4 Dump-Pfad des Editors

- `HE_DUMP_RHI=Vulkan` wählt das Backend (`EditorBackendChoice.h`); seit `f3899ee1` (Thema 124,
  auf main) bleibt es auch nach `OnInit` dabei, eine `config.json` mit RHI ist nicht nötig.
- `HE_DUMP_PATH=x.bmp` + `HE_DUMP_QUIT=1`: `dumpFrameHeadless()` läuft in `OnInit` **vor** dem
  ersten UI-Frame, rendert `HE_DUMP_FRAMES` (Default 3, max 240) Settle-Frames offscreen und
  schreibt BMP über `CaptureViewport` (Vulkan: `VulkanRenderer.cpp:4488`). Auflösung **fest
  1280×720** (`EditorApplication.cpp:6297`).
- Zeugen-Szenen (alle `HE_DUMP_*`): `SKYTEST` (+ `NEBULA`, `NEBQUALITY`, `NEBCOVER`, `TOD`,
  `PITCH`…), `MANYLIGHTS=N[builtin]`, `GI=0|1`, `GIREFLTEST`, `MATERIALTEST`, `RENDERPATH`.
- `he_shot.py` ist Mac-only (`sips` für BMP→PNG, `HE_DUMP_RHI=Metal` als Default) und sagt selbst:
  der Editor **stürzt beim Dump-Quit-Abbau manchmal nach dem Schreiben ab**. Ein CI-Schritt muss
  nach der Datei urteilen, nicht nach dem Exit-Code.

## 2. Optionen

| | lavapipe, Linux | lavapipe, Windows (mesa-dist-win) | SwiftShader | Xvfb statt Offscreen |
|---|---|---|---|---|
| Beschaffung | `apt install mesa-vulkan-drivers` (Distro, signiert) | inoffizielle Binärpakete (pal1000), SHA-Pin nötig | Quellbuild (CMake + LLVM/Subzero), ~15–25 min oder eigener Cache | `apt install xvfb` |
| Vulkan | 1.3/1.4 (Mesa 25.2) | wie Linux | 1.3 | — (nur WSI-Weg) |
| Ray Query | **ja** (seit Mesa 24.1) | ja | **nein** → HW-GI-Pfad ungetestet | — |
| Headless-Surface | `VK_EXT_headless_surface` (Mesa-WSI) | ja | ja | X11-WSI |
| Validation-Layer | `vulkan-validationlayers` (Distro) | SDK ist im Job schon da | eigener Build/SDK | — |
| Passt zu bestehendem Job | neuer Linux-Job (s. §5) | verlängert den 76-min-Windows-Job | egal wo, teuer | Ergänzung zu Linux |

**Gewählt: lavapipe auf Linux.** Distro-Pakete statt fremder Binaries, Ray Query inklusive, und
der Windows-Job wird nicht noch länger. SwiftShader bringt gegenüber lavapipe nichts außer einer
zweiten Implementierung und kostet einen Quellbuild; als späterer Gegencheck („liegt der Fehler
im Treiber?") denkbar, nicht als erstes. lavapipe auf Windows ist interessant, sobald Windows-
spezifischer Vulkan-Code (Win32-WSI) mitgeprüft werden soll; heute nicht nötig.

## 3. Was Schritt 2 vor dem Workflow anfassen muss (Engine/CMake)

1. **`src/HE_Editor/CMakeLists.txt:32`** gated die ImGui-Vulkan-Brücke auf
   `DEFINED ENV{VULKAN_SDK} OR Vulkan_FOUND`. `Vulkan_FOUND` setzt `find_package` in
   `src/HE_Rendering` und ist dort **verzeichnislokal**; im Editor-Verzeichnis ist es leer. Folge
   auf Linux mit Distro-Paketen: `RendererVulkan` wird gebaut und gelinkt
   (`HE_VULKAN_ENABLED` ist CACHE INTERNAL, Z. 385), die ImGui-Brücke fehlt
   (`HE_IMGUI_VULKAN_ENABLED` aus). Der Dump selbst kommt vor dem ersten UI-Frame und braucht sie
   nicht, aber das ist ein halber Editor. Korrektur: auf `HE_VULKAN_ENABLED` gaten.
2. **`src/HE_Editor/CMakeLists.txt:86`** (und analog `src/HE_Rendering/CMakeLists.txt:89` richtig,
   der Editor aber nicht): mit gesetztem `VULKAN_SDK` wird fest `Lib/vulkan-1.lib` verlinkt, ein
   Windows-Pfad. Auf Linux daher **kein** `VULKAN_SDK` setzen, sondern `libvulkan-dev` +
   `find_package(Vulkan)`. Oder die Zeile plattformabhängig machen.
3. **SPIR-V-Compiler:** `find_program(GLSLC_EXECUTABLE NAMES glslc glslangValidator)`
   (`src/HE_Rendering/CMakeLists.txt:126`). `glslang-tools` liefert `glslangValidator`, das reicht
   (der `_hw.comp`-Zweig mit `--target-env vulkan1.2` ist für beide Werkzeuge da).
4. **Dump-Auflösung schaltbar machen** (z. B. `HE_DUMP_W`/`HE_DUMP_H`, Default 1280×720
   unverändert). 640×360 viertelt die CPU-Pixelarbeit; für A/B-Zeugen reicht das.
5. **Gerätewahl belegen:** Name des gewählten `VkPhysicalDevice` ins Log schreiben (heute steht
   nur „physical device selected"). Dann sieht der CI-Log, dass es wirklich `llvmpipe` war.
6. **Optional:** den `gpuDebug = true`-Kommentar (Z. 963 ff., „TEMPORARILY forced ON while the
   Vulkan backend is still broken") nicht anfassen; für CI ist der Dauer-Layer gerade richtig.

## 4. Bildtests pro Feature

Bewertet wird **relativ (A/B im selben Lauf)**, nicht gegen eingecheckte Referenzbilder. Gründe:
lavapipe-Ausgabe ändert sich mit der Mesa-Version, und LLVM erzeugt je nach CPU-Merkmalen des
Runners (AVX2/AVX-512, Intel/AMD) leicht anderen Code. Mesa-Version und `vulkaninfo --summary`
werden geloggt, damit ein Sprung erklärbar bleibt.

| Feature | Szene | A/B-Zeuge | Erwartung | Aufwand | Risiko |
|---|---|---|---|---|---|
| **Nebula** | `SKYTEST=1 TOD=0 NEBULA=0.6 NEBQUALITY=2 PITCH=60 HE_SKY_TIME=…` | `NEBULA=0` gegen `NEBULA=0.6` | Himmelsbereich: mittlere \|Δ\| deutlich > Rauschen; Log ohne „falling back to the reduced sky.frag" (`VulkanRenderer.cpp:7521`) | klein (½ Tag) | Volumetrische Wolken in SKYTEST sind CPU-teuer → `COVERAGE=0` |
| **Clustered Lighting** | `MANYLIGHTS=16 TOD=0 CAMY=207 CAMZ=2 PITCH=-38` | mit gegen `HE_FORWARD_CLUSTER=0`; Negativkontrolle `MANYLIGHTS=16builtin` | 16 statt ≤8 Lichtpools auf dem Boden (Pool = zusammenhängender heller Fleck, Zählung per Schwelle + Flood-Fill); keine Validation-Meldung zu Bindings 24–26 | mittel (1 Tag inkl. Pool-Zähler) | Pool-Zählung braucht stabile Kamera/Schwelle; erst einmal Bild ansehen |
| **GI (DDGI)** | `GI=1` auf MATERIALTEST- oder MANYLIGHTS-Boden, `HE_DUMP_FRAMES=20–40` | `GI=0` gegen `GI=1`; dazu `GI=1` mit `HE_GI_FORCE_SW=1` (Software-BVH) gegen ohne (Ray Query) | GI an ≠ aus (indirektes Licht hebt Schatten); HW- und SW-Pfad nahe beieinander (Schwellwert aus erstem Lauf); Log meldet HW-RT verfügbar | mittel (1 Tag) | **Laufzeit**: Probe-Compute + 20–40 Frames auf der CPU, unbekannt; Rauschen zwischen zwei GI-Läufen (§7.2 maß 0,25 mittlere \|Δ\| auf HW) |
| **GI-Reflexionen** (Bonus) | `GIREFLTEST=1` | `GIREFL=0` gegen `1` | grüner/rot leuchtender Würfel im Spiegelboden | klein, wenn GI läuft | wie GI |
| **Deferred** | `RENDERPATH=1` | — | Vulkan: nur Log-Zeuge, dass Forward läuft (kein Deferred-Pfad) | — | — |
| **Deferred auf GL** (Beigabe) | `HE_DUMP_RHI=OpenGL`, `SECTIONTEST`/`MANYLIGHTS`, `RENDERPATH=0` gegen `1` | Forward gegen Deferred | nahe beieinander (vgl. `docs/deferred-vs-forward-brightness-2026-10-02.md`) | klein–mittel | llvmpipe-GL unter SDL-Offscreen braucht EGL; sonst Xvfb |

**Validation-Allowlist.** Mit installiertem Layer meldet der Renderer heute schon Bekanntes:
Binding 14 (`heLandscapeWeights`, GI-Doku §7.3) und eine Abbau-Leak-Liste (~10 ImGui-förmige
Objekte, §7.2). „Null Meldungen" wäre sofort rot. Vorschlag: nur Meldungen zwischen
„frame dump armed" und dem BMP-Schreiben zählen, mit einer kleinen Allowlist (Muster + Grund +
Thema), und jede **neue** Meldung macht den Schritt rot.

### Zwei Testebenen

- **(a) In-Process in `he_tests`** (wie die D3D11/D3D12-WARP-Fälle in `test_material_graph.cpp`,
  je ~200–250 Zeilen): eigenes `VkInstance`/`VkDevice` ohne Surface auf lavapipe, ein
  Offscreen-Bild, eine Pipeline aus den echten SPIR-V-Blobs, ein Pixel zurücklesen. Schnell und
  deterministisch, aber: die D3D11-Helfer (`BindDDGIAtlases`, `CreateClusterRawBuffer`) sind freie
  Funktionen, die der Test direkt ruft; die Vulkan-Gegenstücke sind Member des Renderers. Ebene (a)
  prüft also Shader + Layout auf einem echten Gerät, **nicht** den Renderer-Code. Aufwand:
  gemeinsamer Harness ~1 Tag, je Fall ~½ Tag. Guard: Test überspringt mit `MESSAGE`, wenn kein ICD
  da ist (macOS/Windows-Job), wie die WARP-Fälle auf Nicht-Windows.
- **(b) End-to-End-Editor-Dumps** (Tabelle oben): treffen den ganzen VulkanRenderer inklusive
  Descriptor-Writes, Barrieren, Readback. **Das sind die „echten Bildtests" aus dem Thema; damit
  anfangen.** (a) lohnt sich danach für Dinge, die man gezielt einzeln prüfen will.

## 5. Vorschlag CI-Workflow (nicht eingebaut)

Eigener Job statt Erweiterung des Linux-Jobs. Grund: `libvulkan-dev` im bestehenden Job schaltet
über `find_package(Vulkan)` automatisch `HE_VULKAN_ENABLED` ein, und das ausgelieferte
Linux-Editor-Paket enthielte plötzlich `RendererVulkan` + `Shaders/`. Das ist eine
Produktentscheidung, keine CI-Entscheidung. Der eigene Job baut nur `HorizonEditor` und verlängert
den Lauf nicht (Windows 76 min ist der lange Pol).

```yaml
  vulkan-lavapipe:
    name: Linux · Vulkan (lavapipe)
    runs-on: ubuntu-24.04            # gepinnt statt ubuntu-latest: Mesa-Stand bestimmt die Bilder
    steps:
      - uses: actions/checkout@v4
      - uses: seanmiddleditch/gha-setup-ninja@v5
      - name: Install dependencies (+ lavapipe)
        run: |
          sudo apt-get update
          sudo apt-get install -y <dieselben Pakete wie der Linux-Job> \
            mesa-vulkan-drivers libvulkan-dev vulkan-tools \
            vulkan-validationlayers glslang-tools
          dpkg-query -W mesa-vulkan-drivers vulkan-validationlayers
      - name: Software ICD witness
        env:
          VK_DRIVER_FILES: /usr/share/vulkan/icd.d/lvp_icd.x86_64.json
        run: |
          vulkaninfo --summary
          vulkaninfo > vkinfo.txt
          for ext in VK_EXT_headless_surface VK_KHR_ray_query; do
            grep -q "$ext" vkinfo.txt || { echo "::error::lavapipe without $ext"; exit 1; }
          done
      # Derselbe Schluessel wie der Linux-Job: dieselben FetchContent-Checkouts.
      - uses: actions/cache@v4
        with: { path: build/_deps, key: deps-${{ runner.os }}-${{ hashFiles('CMakeLists.txt') }} }
      - name: Configure
        run: cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHE_PORTABLE_BUILD=ON -DHE_PREFER_MBEDTLS=ON
      - name: Assert the Vulkan backend is enabled
        run: grep -qE '^HE_VULKAN_ENABLED:INTERNAL=TRUE$' build/CMakeCache.txt
      - name: Build editor
        run: cmake --build build --target HorizonEditor -j 4
      - name: Vulkan image tests
        env:
          VK_DRIVER_FILES: /usr/share/vulkan/icd.d/lvp_icd.x86_64.json
          SDL_VIDEO_DRIVER: offscreen
          HE_HIDDEN_WINDOW: "1"
          HE_COLLAB_OFFLINE: "1"
          HE_SKY_TIME: "10"
        run: python3 scripts/he_vk_imagetests.py --editor out/deploy/Editor/HorizonEditor --out vk-shots
      - uses: actions/upload-artifact@v4
        if: always()
        with: { name: vulkan-lavapipe-shots, path: vk-shots/ }
```

`scripts/he_vk_imagetests.py` (neu, Schritt 2): pro Fall ein privates `HOME`, Editor mit
`HE_DUMP_RHI=Vulkan`, `HE_DUMP_PATH`, `HE_DUMP_QUIT=1` und den Szenen-Variablen starten,
**nach der BMP urteilen** (Exit-Code nur loggen, s. §1.4), BMP mit reinem Python lesen (kein
`sips`, kein PIL nötig: 24/32-bit BMP ist trivial), A/B-Metriken aus §4 rechnen, Validation-Zeilen
gegen die Allowlist prüfen, Bilder + Log + `metrics.json` ins Artefakt. Timeout pro Fall großzügig
und erst nach der Messung aus Unterschritt 1 festlegen. Wer die Bilder ansehen will, lädt das
Artefakt; Läufe, die rot werden, haben das Bild dabei.

**Rückfall Xvfb**, falls lavapipe die Headless-Surface oder FIFO nicht anbietet:
`sudo apt-get install xvfb` und den Testschritt unter `xvfb-run -a -s "-screen 0 1280x720x24"`
ohne `SDL_VIDEO_DRIVER` laufen lassen (X11-WSI von Mesa).

## 6. Reihenfolge für Schritt 2

1. **Messen:** Job-Gerüst bis „Software ICD witness" + Build + **ein** SKYTEST-Schuss
   (`NEBULA=0.6 COVERAGE=0`) auf lavapipe; Laufzeit, Log, Bild ansehen. Hier zeigt sich, ob
   Offscreen-Surface, Swapchain (FIFO, `currentExtent`), Validation-Rauschen und Dump-Abbau
   funktionieren. Per `gh workflow run CI --ref <zweig>` auf dem Feature-Zweig.
2. CMake-Fixes §3.1/§3.2 (+ §3.4/§3.5), falls Schritt 1 sie braucht oder um den Editor vollständig
   zu machen.
3. `he_vk_imagetests.py` mit Nebula → Clustered → GI (Reihenfolge nach Aufwand und Laufzeit).
4. Validation-Allowlist aus dem ersten grünen Lauf ableiten.
5. Optional: GL/llvmpipe-Deferred-Fall, In-Process-Fälle (Ebene a).

## 7. Nicht verifiziert / offen

- **Alle lavapipe-Aussagen sind Quellenangaben, nicht gemessen** (kein Container, kein Linux auf
  diesem Mac): Mesa 25.2.x in noble-updates, Ray Query seit 24.1, Headless-Surface auf lavapipe,
  FIFO auf der Headless-Swapchain. Selbst geprüft ist nur der Offscreen-Vulkan-Code im gepinnten
  SDL 3.2.14 (`HEADLESS_SURFACE_EXTENSION_REQUIRED_TO_LOAD 0`: die Extension wird nur gemeldet,
  wenn die Instanz sie anbietet). Ebenfalls geprüft: der Offscreen-Treiber ist **mitgebaut**.
  SDL 3.2.14 setzt `SDL_OFFSCREEN` per Default `ON` (`sdl3-src/CMakeLists.txt:382`) und
  `SDL_VULKAN` auf Linux `ON` (Z. 376); das Engine-CMake überschreibt nur
  `SDL_SHARED/STATIC/TEST` (`CMakeLists.txt:67-69`). Die lokale Mac-Build-Config enthält
  `SDL_VIDEO_DRIVER_OFFSCREEN 1` und `SDL_VIDEO_VULKAN 1`; die Linux-Config ist nicht gesehen.
- **CPU-Laufzeit pro Bild unbekannt.** Volumetrische Wolken, Probe-Compute und 20–40 Settle-Frames
  bei 1280×720 auf 4 Runner-Kernen können Sekunden bis Minuten je Fall kosten.
- **Sturz beim Dump-Quit-Abbau** (`he_shot.py`-Docstring) ist auf Vulkan/Linux unbeobachtet;
  sollte er die BMP verhindern, wird er Teil von Schritt 2.
- Ob der Renderer mit der Validation-Ausgabe von `vulkan-validationlayers` (Ubuntu, älter als das
  1.4.313-SDK) andere Meldungen bekommt als auf NN-WS03: im ersten Lauf vergleichen.
- Die Material-Fragment-Stage nutzt genau 16 Combined-Image-Sampler (Spec-Minimum, GI-Doku §7.3).
  lavapipe meldet höhere Grenzen; ein Fehler, der nur auf Geräten am Minimum auftritt, fällt dort
  **nicht** auf.

## 8. Schritt 2: umgesetzt und auf dem Runner gemessen (03.10.2026)

**Eingebaut:**
- Job `vulkan-lavapipe` in `.github/workflows/ci.yml` (ubuntu-24.04 gepinnt, eigener Job wie
  in §5 vorgeschlagen, das Linux-Paket bleibt ohne Vulkan).
- `scripts/he_vk_imagetests.py`: A/B-Fälle, Urteil nach der BMP, Gerätezeuge `llvmpipe`,
  Validation-Prüfung.
- Editor-CMake §3.1/§3.2: Gate auf `HE_VULKAN_ENABLED`, SDK-Bibliothekspfad je Plattform.
- Gerätename im Log (§3.5): `VulkanRenderer: device 0 of N: <Name> (Vulkan x.y.z, driver …)`.

**Gemessen** (Läufe 37112790557 und 37113763253; Lauf 37114795547 mit der fertigen Fassung:
Job grün, 4/4 Fälle, Werte wie unten):

| | Wert |
|---|---|
| Treiber | Mesa 25.2.8-0ubuntu0.24.04.4, `llvmpipe (LLVM 20.1.2, 256 bits)`, Vulkan 1.4.318 |
| Loader / Layer | `libvulkan1` 1.3.275, `vulkan-validationlayers` 1.3.275 |
| ICD-Datei | **`lvp_icd.json`**, nicht `lvp_icd.x86_64.json` (§5 lag falsch). Der Job sucht sie jetzt per Glob |
| Extensions | `VK_EXT_headless_surface`, `VK_KHR_ray_query` und `VK_KHR_acceleration_structure` vorhanden |
| Runner | 4 Kerne. Build nur `HorizonEditor`: **13 min** |
| Zeit pro Bild (1280×720) | 3–6 s mit 3 Frames, 11–19 s mit 40 Frames GI; alle 13 Bilder unter 3 min. §3.4 (`HE_DUMP_W/H`) ist **nicht nötig** |
| WSI | SDL-Offscreen + Headless-Surface **und** Xvfb/X11 funktionieren beide |
| Abbau | Exit-Code 0 bei allen Bildern, der Abbau-Absturz aus `he_shot.py` tritt hier nicht auf |

**Fälle und Messwerte** (mittlere |Δ| in 8-Bit-Stufen; Rauschen = gleiches Bild zweimal):

| Fall | A/B | Signal | Rauschen | Schwelle |
|---|---|---|---|---|
| `nebula` | `NEBULA=0` / `0.6`, Nacht, Blick nach oben | 13,1–13,4 (35 % px > 2) | bis 1,4 zwischen zwei beliebigen Aufnahmen (1,2–1,4 zwischen Läufen, 1,0 im selben Lauf Offscreen gegen Xvfb): das Sternfeld bewegt sich | 5,0 |
| `clustered` | `HE_FORWARD_CLUSTER=0` / an, `MANYLIGHTS=16` | 2,97 (18 % px > 2): **16 Lichtpools gegen 7** | bitgleich zwischen Läufen | 1,5 |
| `gi` | `GI=0` / `1` auf `GIREFLTEST`, `GIREFL=0` | 0,46 (10 % px > 2) | max 1 auf 6 px | 0,3 |
| `gi` HW gegen SW | `HE_GI_FORCE_SW=1` | max 1 auf 20 px: **Ray-Query-Pfad = Software-BVH** | – | nur Bericht |
| `gi_refl` | `GIREFL=0` / `1` | max 1 auf 39 px: **keine GI-Reflexionen auf Vulkan** | – | nur Bericht |

Der HW-RT-Pfad der GI (`gi_*_hw.comp`) ist damit zum ersten Mal außerhalb von NN-WS03 gelaufen,
und er trifft den Software-Pfad praktisch pixelgleich.

**Gefundene Renderer-Fehler** (offen, nicht Teil dieses Schritts):
1. **Bemaltes Terrain stürzt auf lavapipe ab.** `HE_DUMP_LANDSCAPELAYERS=1` endet mit SIGSEGV
   im ersten Draw, auch mit `GI=0`. Validation: `VUID-VkGraphicsPipelineCreateInfo-layout-07988`
   und `vkCmdDrawIndexed … binding #14 is invalid`. Das ist das fehlende `heLandscapeWeights`
   aus GI-Doku §7.3. NVIDIA meldet es nur, ein Treiber, der den Deskriptor wirklich liest,
   stürzt. Der GI-Fall nutzt deshalb `GIREFLTEST` statt des Terrains aus §4.
2. **Material-UBO wird im Render-Pass beschrieben.** Built-in-Material-Draws rufen
   `vkCmdUpdateBuffer` und eine Barriere innerhalb des Szenen-Render-Pass
   (`VulkanRenderer.cpp`, „Update material UBO", in `drawDCVk` und im instanzierten Zwilling).
   Meldungen: `VUID-vkCmdUpdateBuffer-renderpass` und `VUID-vkCmdPipelineBarrier-None-07889`,
   zwei pro Built-in-Draw und Frame. Spec-widrig: alle Draws teilen einen UBO.
3. **Bildlayout mit GI an.** Einmal pro Frame
   `UNASSIGNED-CoreValidation-DrawState-InvalidImageLayout`. Der Engine-Logger kürzt die
   Meldung vor den Layout-Namen; welches Bild es ist, ist offen.

2 und 3 stehen in der Allowlist des Skripts (`ALLOWED_VALIDATION`, je Fall und Variante,
mit Grund). Nebula und Clustered erlauben nichts. Jede neue Meldung färbt den Fall rot. Ist
ein Fehler behoben, wird sein Eintrag gelöscht.

**Offen nach Schritt 2:**
- Pool-Zähler für Clustered (heute: mittlere |Δ|; die Bilder zeigen 16 gegen 7 Pools eindeutig).
- `LANDSCAPELAYERS` als Fall aufnehmen, sobald Fund 1 behoben ist.
- `gi_refl` zur bewerteten Paarung machen, sobald Vulkan GI-Reflexionen zeichnet.
- GL/llvmpipe-Deferred-Beigabe und In-Process-Fälle (Ebene a) aus §4: nicht angefangen.

## Quellen

- Phoronix, „Mesa's CPU-Based Vulkan Driver Now Supports Ray-Tracing":
  https://www.phoronix.com/news/Mesa-Lavapipe-Vulkan-RayTracing
- Phoronix, „Lavapipe CPU-Based Vulkan Driver Implements Ray-Tracing Pipelines" (Mesa 24.1):
  https://phoronix.com/news/Lavapipe-RT-Pipelines-Mesa-24.1
- ubuntuupdates.org, `mesa-vulkan-drivers` noble-updates (25.2.8-0ubuntu0.24.04.2):
  https://ubuntuupdates.org/package/core/noble/main/updates/mesa-vulkan-drivers
- Phoronix, „Lavapipe CPU-Based Vulkan Ported To Windows":
  https://www.phoronix.com/news/Lavapipe-CPU-Vulkan-Windows
- SwiftShader README (Vulkan 1.3, Quellbuild): https://swiftshader.googlesource.com/SwiftShader/
- SDL 3.2.14, `src/video/offscreen/SDL_offscreenvulkan.c` (FetchContent-Checkout `build/_deps/sdl3-src`)
