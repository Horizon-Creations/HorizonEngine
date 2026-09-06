# Backend-Parität — D3D11 / D3D12 / Vulkan an Metal heranführen

Stand: 2026-08-11. Grundlage: belegter Feature-Abgleich aller fünf Backends (60 Features,
240 Einzelprüfungen mit `Datei:Zeile`, plus adversarische Gegenprobe).

Metal ist heute mit Abstand das vollständigste Backend (13 211 Zeilen gegenüber 8 286 auf
Vulkan, 7 202 auf D3D12, 4 247 auf D3D11). Dieses Dokument sagt, **was genau** fehlt, **was
davon Portierung statt Neuentwurf ist** und **in welcher Reihenfolge** es sich lohnt.

---

## 0. Methode — warum diesen Zahlen zu trauen ist

Header-Signaturen und Code-Kommentare beweisen nichts: ein `SetSSAOSettings`-Override kann
eine Struct wegspeichern, die niemand liest, und der D3D12-Header behauptet Dinge über
andere Backends, die nicht stimmen. Der Abgleich hat deshalb eine harte Beweisregel benutzt:

> Ein Feature gilt nur als **vorhanden**, wenn für alle drei Punkte eine `Datei:Zeile`
> vorliegt: (a) Shader-Quelltext existiert, (b) Pipeline/PSO wird erzeugt, (c) der Pass wird
> im Frame-Pfad tatsächlich encodiert. Zwei von drei = **teilweise**. Alles, was nur aus
> einer Override-Signatur, einem Kommentar oder der Dateigröße abgeleitet ist = **fehlend**.

Eine zweite, adversarische Runde hat jede Behauptung zu widerlegen versucht — in beide
Richtungen: falsche Lücken (Backend löst es anders) und falsche „vorhanden" (toter
Codepfad). Das hat 13 Statuskorrekturen ergeben, darunter der schwerwiegendste Befund
dieses Dokuments (§1.1).

**Die vier Befunde in §1 habe ich anschließend selbst nachvollzogen** — mit `fxc` bzw. einem
eigens gegen `he_shadercompiler` gebauten Testprogramm. Sie sind nicht abgeleitet, sondern
reproduziert. Alles andere stammt aus dem Abgleich und trägt seine Belege im Text.

---

## 1. Vier Befunde, die den Plan bestimmen

### 1.1 Der gesamte GI-Stack auf D3D11 **und** D3D12 ist zur Laufzeit tot

Das ist kein fehlendes Feature. Der Code ist vollständig da — er läuft nur nie.

`src/Backends/D3D_Shared/HlslSources.h:630` öffnet einen `[unroll]`-Loop, der `break`
(`:632`) und `continue` (`:635`, `:636`, `:638`) enthält und zweimal dynamisch in eine
Vektor-Komponente schreibt (`localVis[i] = 0.0`, `:638` und `:640`). Diese Kombination ist
unter `cs_5_0` nicht übersetzbar. Der Kommentar direkt darüber (`:628-629`) behauptet
ausdrücklich das Gegenteil („so the dynamic vector-component write `localVis[i]` stays
FXC/SM5.0-safe") — er ist falsch.

Selbst reproduziert, Kernel exakt so zusammengesetzt wie zur Laufzeit
(`kGiTraversalHLSL` + `kGiShadowCSHLSL`):

```
error X3500: array reference cannot be used as an l-value; not natively addressable  (localVis[i] = 0.0)
error X3511: forced to unroll loop, but unrolling failed                             ([unroll] for (int i = 0; i < 4; ++i))
compilation failed; no code produced
```

`GiProbeCS` aus derselben Datei kompiliert dagegen sauber — der Fehler sitzt isoliert im
Local-Light-Loop des Shadow-Kernels.

**Die Folgen sind viel größer als der Fehler.** Beide Backends werten den Compile-Erfolg als
Gesamt-Gate:

- D3D11: `D3D11Renderer.cpp:1677` schlägt fehl → `:1706 giSupported = false`
- D3D12: `D3D12Renderer.cpp:4069` schlägt fehl → `:4204-4213 giSupported = false; return;`

Damit sind auf beiden Backends tot: DDGI-Probe-Update, DDGI-Feldsampling im Shading,
ray-getracte Direktionalschatten, die lokale RT-Schattenmaske, der Software-RT-Fallback über
`HE::GiBvh` und das Multi-Bounce-Feedback.

**Und das Bitterste:** Auf D3D12 steht `createGiHwPipelines()` (`D3D12Renderer.cpp:4220`)
**hinter** diesem `return`. Der komplette DXR-1.1-Pfad — separat mit `dxc -T cs_6_5`
offline gebaut, per CMake deployt, und der sauber kompiliert — ist deshalb **unerreichbar**.
Es gibt keine Hardware, auf der er je gelaufen wäre.

Der naheliegende Fix ist klein und trägt: den Loop durch vier Aufrufe einer Hilfsfunktion
mit literalem Index ersetzen, die einen Skalar zurückgibt (kein dynamischer Lvalue, kein
`break`/`continue`). Ich habe die gepatchte Variante gebaut — sie kompiliert unter `cs_5_0`
sauber (nur noch eine Registerdruck-Warnung X4714).

### 1.2 Warum das überhaupt passieren konnte

Die eingebetteten HLSL-Strings werden **zur Laufzeit** von `D3DCompile` übersetzt. Kein
Build-Schritt fasst sie an. Ein kaputter Shader bricht deshalb nichts sichtbar — er schaltet
sein Feature still ab, und auf macOS, wo entwickelt wurde, fällt das nie auf.

Genau dieselbe Klasse gilt für die MSL-Strings in `MetalRenderer.mm` und die GLSL-Strings in
`OpenGLRenderer.cpp`. **Ausgenommen sind die beiden Backends mit Datei-Shadern**: Vulkans 30
`.glsl` (via `glslc`) und D3D12s zwei `gi_*_hw.hlsl` (via `dxc`) werden zur Buildzeit
geprüft — und sind entsprechend heil.

Das ist der Grund, warum P0b eine Build-Prüfung enthält und nicht nur P0 einen Bugfix.

Zur Beruhigung: Ich habe **alle** eingebetteten HLSL-Shader durch `fxc` geschickt — 21
Strings, 29 Entry-Point/Profil-Kombinationen. Genau einer schlägt fehl, der aus §1.1. Es
liegt also kein zweiter stiller Ausfall dieser Art herum. Für die MSL- und GLSL-Strings ist
das nicht geprüft (auf Windows nicht sinnvoll ausführbar für MSL); dort bleibt das Risiko
bestehen, bis P0b die GLSL-Seite mitnimmt.

### 1.3 SSR ist **keine** Metal-Exklusivität — die Shader sind schon portabel

Der naheliegende Schluss aus dem Code („SSR gibt es nur auf Metal, also muss es dreimal neu
entworfen werden") ist falsch. Alle vier SSR-Shader liegen als kanonisches GLSL 450 im
geteilten Cross-Compiler, und jeder Accessor hat neben dem Metal-Zweig einen generischen
`else`-Zweig auf `toTarget(backend)`:

| Shader | Quelle | Accessor |
|---|---|---|
| `kSSRTraceFS` | `MaterialShaderLibrary.cpp:1116` | `:1554` (generisch `:1569`) |
| `kSSRCompositeFS` | `:1369` | `:1573` (generisch `:1595`) |
| `kSSRBlurFS` | `:1479` | `:1599` (generisch `:1610`) |
| `kSSRRoughMixFS` | `:1509` | `:1537` (generisch `:1550`) |

Dieser `else`-Zweig wurde für SSR allerdings **nie aufgerufen** — einziger Aufrufer heute ist
`MetalRenderer.mm:10812-10815`. Ob er trägt, war damit unbewiesen. Ich habe die vier Quellen
extrahiert und gegen `he_shadercompiler` durch alle drei Zielformate geschickt:

```
kSSRTraceFS       HLSL SM5.0 OK / SPIR-V OK / GLSL 410 OK
kSSRCompositeFS   HLSL SM5.0 OK / SPIR-V OK / GLSL 410 OK   (mit injectPreamble)
kSSRBlurFS        HLSL SM5.0 OK / SPIR-V OK / GLSL 410 OK
kSSRRoughMixFS    HLSL SM5.0 OK / SPIR-V OK / GLSL 410 OK
```

**SSR auf D3D/Vulkan ist damit Verdrahtungsarbeit, keine Shader-Entwicklung.** Das ist die
größte Einzelkorrektur an der Aufwandsschätzung in diesem Dokument.

Für den *Forward*-SSR kommt hinzu: der Konsument ist bereits in jedes Zielbackend
einkompiliert (`heSSRFwd`, `MaterialShaderLibrary.cpp:257`, Mix-Zweig `:504-508`), er ist nur
beweisbar unerreichbar, weil `heLight.ssr` außerhalb von Metal nirgends geschrieben wird. Die
Library sagt das selbst (`:496-497`: Gates sind 0, „dead code after constant folding").

### 1.4 Der Deferred-Resolve ist teilweise schon portabel — die Sperre ist eine Zeile

`compileResolveVariant` (`MaterialShaderLibrary.cpp:1024`) übersetzt den **nicht-tile,
nicht-clustered** Resolve für *jedes* Backend. Tile- und Clustered-Variante werden dagegen
hart abgewiesen:

```cpp
if (backend != Backend::Metal) {
    // GL keeps the sampled non-clustered resolve (no SSBOs in GL 4.1, no
    // framebuffer fetch); the tile/clustered variants are Metal-only.
    if (tile || clustered) return {};
```

Die Begründung ist **GL-4.1-spezifisch** — und gilt für D3D11/D3D12/Vulkan nicht: die haben
Structured Buffers bzw. SSBOs. Die Clustered-Variante ist für sie also nicht technisch
gesperrt, sondern nur von dieser Zeile. Für die Tile-Variante gilt das nur eingeschränkt
(siehe §4).

---

## 2. Paritätsmatrix

`JA` = alle drei Belege vorhanden · `~` = teilweise · `--` = fehlend.
**OpenGL ist nicht Ziel dieses Plans, sondern Referenz**: wo GL `JA` steht, existiert bereits
eine nicht-Apple-Implementierung — Portierung statt Neuentwurf.

### Post-Processing & Basispässe — weitgehend erreicht

| Feature | GL | D3D11 | D3D12 | Vulkan |
|---|:--:|:--:|:--:|:--:|
| HDR-Tonemap / PostProcessPass | JA | JA | JA | JA |
| Bloom | JA | JA | JA | JA |
| SSAO / HBAO / GTAO + Blur | JA | JA | JA | JA |
| FXAA | JA | JA | JA | JA |
| Transparenz-Pfad | JA | JA | JA | JA |
| Skinned-Geometry-Pass | JA | JA | JA | JA |
| Debug-Linien | JA | JA | JA | JA |
| ImGui-Anbindung | JA | JA | JA | JA |
| Viewport-Target + `CaptureViewport` | JA | JA | JA | JA |
| `FrameGpuStats` (Whole-Frame) | JA | JA | JA | JA |
| Lens-Flare-Overlay | JA | -- | -- | -- |
| SSAO über G-Buffer-Tiefenrekonstruktion | JA | -- | -- | -- |

### Schatten

| Feature | GL | D3D11 | D3D12 | Vulkan |
|---|:--:|:--:|:--:|:--:|
| CSM-Depth-Pass (3 Kaskaden) | JA | ~ | ~ | ~ |
| CSM-Sampling + 3×3-PCF (Forward) | JA | ~ | ~ | ~ |
| Normal-Offset + Slope-Scaled Bias | JA | ~ | ~ | ~ |
| Punkt-/Spot-Schatten (16-Layer-Atlas) | JA | -- | -- | -- |
| Shadow-Cascade-Debug (`SetShadowDebug`) | JA | -- | -- | -- |
| CSM + Atlas für Graph-Materialien (`heLitP`) | ~ | -- | -- | -- |

### Global Illumination — hier wirkt §1.1

Die `~` auf D3D11/D3D12 sind **ausschließlich** Folge des toten Kernels. Der Code ist
geschrieben; er läuft nicht.

| Feature | GL | D3D11 | D3D12 | Vulkan |
|---|:--:|:--:|:--:|:--:|
| Ray-getracte Schatten ersetzen CSM | JA | JA | JA | JA |
| DDGI-Probe-Update | JA | ~ | ~ | JA |
| DDGI-Feldsampling im Shading | JA | ~ | ~ | JA |
| RT-Shadow-Pass (Direktional) | JA | ~ | ~ | JA |
| RT-Schattenmaske Punkt/Spot | JA | ~ | ~ | JA |
| Software-RT-Fallback (`HE::GiBvh`) | JA | ~ | ~ | JA |
| Multi-Bounce-Probe-Feedback | JA | ~ | ~ | JA |
| HW-RT-Acceleration-Structures | -- | -- | ~ | JA |
| RT-GI-Reflections | JA | -- | -- | -- |
| Painted-Landscape-Tabelle | JA | -- | -- | -- |
| Mehrfach-Spiegel-Bounces | -- | -- | -- | -- |

### Deferred / Lighting

| Feature | GL | D3D11 | D3D12 | Vulkan |
|---|:--:|:--:|:--:|:--:|
| Deferred G-Buffer-Pass (4 MRT) | JA | -- | -- | -- |
| Zwei-Pass-Resolve (gesampelt) | JA | -- | -- | -- |
| `heLitP` Lighting-ABI-Fill | JA | ~ | ~ | ~ |
| G-Buffer-Variante für Graph-Materials | JA | -- | -- | -- |
| RenderPath-Umschaltung Forward/Deferred | ~ | -- | -- | -- |
| Tile-Memory-Resolve (Framebuffer-Fetch) | -- | -- | -- | -- |
| Deferred Decals | JA | -- | -- | ~ ¹ |
| Clustered-Lighting-Build | -- | -- | -- | -- |
| SSR deferred / SSR forward | -- | -- | -- | -- |

¹ **Vulkan zeichnet Decals, aber nicht deferred.** Vulkan hat keinen G-Buffer, also
gibt es dort kein Base-Color-Ziel, in das ein Decal *vor* der Beleuchtung blenden
könnte. Statt auf den Deferred-Port (P4) zu warten, zeichnet Vulkan
**Forward-Screen-Space-Decals**: Kamera-Tiefen-Vorpass, Weltposition rekonstruieren,
Box clippen, in die bereits beleuchtete Farbe blenden. Der Projektor bringt seine
eigene, viel kleinere Beleuchtung mit (ein Richtungslicht + Ambient, Normale aus
`ddx/ddy` der rekonstruierten Position) — **keine Schatten, keine Punkt-/Spotlichter,
kein GI auf dem Decal**. Das ist eine bewusste, dokumentierte Abweichung derselben
Art wie Single-Map statt CSM bei den Schatten. Details und Grenzen:
`docs/decals-cross-backend-plan.md` §6. D3D11/D3D12 bekommen denselben Shader
(`MaterialShaderLibrary::decalFragmentForward`), sobald ihre Schritte laufen.

### Himmel & Wetter

| Feature | GL | D3D11 | D3D12 | Vulkan |
|---|:--:|:--:|:--:|:--:|
| Skybox mit Rayleigh/Mie/Ozon-Atmosphäre | JA | ~ | ~ | ~ |
| Nachthimmel (Sterne, Nebel, 3D-Aurora, Meteore) | JA | ~ | ~ | ~ |
| Mond (Phase, Corona, prozedurale Oberfläche) | JA | ~ | ~ | ~ |
| Wolken-Raymarch (Dome + 3D-Volumen) | JA | ~ | ~ | ~ |
| Low-Res-Cloud-Pass + Reprojektion | JA | -- | -- | -- |
| Cirrus, Kondensstreifen, Regenbogen, God-Rays | JA | -- | -- | -- |
| GPU-Wetter-Partikel | JA | -- | -- | -- |
| Sky-Env-Bake (IBL-Cubemap) | ~ | -- | -- | -- |

### Editor-Integration — der geschlossenste Block

| Feature | GL | D3D11 | D3D12 | Vulkan |
|---|:--:|:--:|:--:|:--:|
| Material-Vorschau | JA | -- | -- | -- |
| Skeletal-Vorschau (Bone-Overlay) | JA | -- | -- | -- |
| Partikel-Vorschau | JA | -- | -- | -- |
| Asset-Thumbnails | JA | -- | -- | -- |
| Partikel-Thumbnails | JA | -- | -- | -- |
| Widget-Thumbnails | JA | -- | -- | -- |
| `WarmupMaterials` | JA | -- | -- | -- |
| `InvalidateMaterial/Mesh/Texture` | JA | ~ | ~ | ~ |
| Multi-Window | JA | -- | -- | JA |
| Per-Pass-GPU-Timing | JA | -- | -- | -- |
| Detailed-Capture (1 Command-Buffer/Pass) | JA | ~ | -- | -- |

**Keine Lücke, anders gelöst:** D3D12 hat kein `GetViewportTexture`-Override, löst dasselbe
aber über `GetViewportD3DResource` + `HasViewportResourceChanged`. Nicht nachziehen.

---

## 3. D3D11 — die Obergrenze offen ausgesprochen

„D3D" meint hier zwei sehr verschiedene Backends. D3D11 ist auf **CS 5.0** festgelegt und hat
**keine Hardware-Raytracing-API** — kein DXR, kein `RayQuery`, kein `ID3D11Device5`
(Grep nach DXR/RayQuery/`ACCELERATION_STRUCTURE` in `D3D11Renderer.cpp`: null Treffer).

Für D3D11 gilt deshalb dauerhaft:

- GI läuft **nur** über den Software-RT-Pfad (`HE::GiBvh`). Das ist kein Defizit des Plans —
  es ist derselbe Pfad, der auf Metal ohne HW-RT greift, und er ist der portabelste.
- Clustered Lighting ist über Structured Buffers machbar, Tile-Memory-Resolve nicht.
- Alles unter P0/P2/P3/P5 ist ohne Einschränkung erreichbar.

Wo im Folgenden „D3D" steht und D3D11 abweicht, ist es benannt.

---

## 4. Was **nicht** portiert wird — und warum

Ein Plan, der Metal-Exklusives als Lücke führt, erzeugt Arbeit, die nie fertig wird.

**Tile-Memory-Deferred-Resolve** (`MetalRenderer.mm:10620`) liest den G-Buffer per
Framebuffer-Fetch aus Tile-Speicher, statt gespeicherte Texturen zu sampeln. Das ist ein
Apple-GPU-Merkmal (MSL 2.3, `[[color(n)]]`).
→ **D3D12: kein Äquivalent.** Der ehrliche Zielzustand ist der Zwei-Pass-Resolve.
→ **Vulkan: eingeschränkt möglich** über Subpass-Input-Attachments innerhalb eines
Render-Passes — aber das ist ein eigener Entwurf, kein Port. Nicht in diesem Plan.

**Deferred Decals** (`:10693`) und **Clustered-Lighting-Build** (`:10454`) hängen im Metal-Code
am offenen Tile-G-Buffer-Pass. Der *Algorithmus* ist portabel (Clustered braucht nur
Structured Buffers/SSBOs, siehe §1.4), die *Einbettung* nicht. Sie stehen deshalb erst nach
P4 an, nicht darin.
→ **Decals sind seit 06.09.2026 vorgezogen** (`docs/decals-cross-backend-plan.md`): GL hat
den echten Port bekommen, Vulkan einen Forward-Ersatz mit eigener kleiner Beleuchtung.
Nur der *deferred* Decal-Pfad wartet auf P4; Decals als Feature nicht mehr.

**Echte Hit-Normalen über Tier-2-Argument-Buffer** (`refl-true-hit-normals-argbuffer`) ist
ein Metal-Ressourcenmodell-Merkmal. Auf D3D12/Vulkan wäre das Bindless — anderer Entwurf.

---

## 5. Phasenplan

Konvention wie in `docs/ssr-plan.md`, `docs/deferred-renderer-plan.md` und
`docs/gi-reflections-plan.md`: jede Phase ist einzeln commit- und verifizierbar, nach jeder
Phase läuft die Engine, jede Phase endet mit einem konkreten Abnahme-Gate.

Reihenfolge nach Nutzen pro Aufwand — nicht nach Feature-Größe.

### P0 — Den toten GI-Stack auf D3D reparieren (Bugfix, kein Feature)

> **Stand 2026-08-11: ERLEDIGT und auf Hardware abgenommen.**
>
> Der Shader-Fix sitzt in `HlslSources.h` (`giLocalVisOne` + vier literale Aufrufe).
> Zusätzlich wird `createGiPipelines()` jetzt **eifrig aus `Initialize`** aufgerufen
> (`D3D11Renderer.cpp`, `D3D12Renderer.cpp`) statt erst im ersten GI-Draw: der Compile
> gehört in die Initialisierung, nicht in einen Frame, und ein Fehlschlag muss
> `giSupported` löschen, *bevor* `GetCapabilities()` zum ersten Mal gelesen wird. Der
> alte Aufruf in `runGiShadow` bleibt als idempotentes Sicherheitsnetz stehen.
>
> Belege: `fxc`-Sweep über alle eingebetteten HLSL-Shader von **1 von 29 Fehlschlägen auf 0**;
> `x64-release`-Build grün; und auf echter Hardware loggt die Engine jetzt
>
> ```
> D3D11Renderer: GI pipelines built (compute ray tracing active)
> D3D12Renderer: GI pipelines built (compute ray tracing active)
> D3D12Renderer: GI hardware ray tracing available (DXR 1.1 inline RayQuery)
> ```
>
> — vorher stand dort „GI pipeline build failed — GI disabled", und die DXR-Zeile war
> strukturell unerreichbar. Sie erscheint hier zum ersten Mal.
>
> **Zwei Dinge bleiben offen** (kleiner als P0, eigene Aufgaben):
> 1. Ob der GI-**Draw** pro Frame auf D3D tatsächlich läuft, ist derzeit **nicht
>    beobachtbar**: D3D hat keinerlei GI-Laufzeit-Logs, GL dagegen schon
>    (`OpenGLRenderer.cpp:5785`, „GI probe grid NxMxK"). Ein Gegenstück auf D3D wäre
>    die billigste Abhilfe und Voraussetzung für jede spätere GI-Verifikation.
> 2. Die Bleed-Zeugenszene taugt nicht als Gate — siehe Verifikations-Notiz unten.

Die mit Abstand höchste Rendite im ganzen Dokument: sechs Features auf zwei Backends plus der
komplette DXR-Pfad auf D3D12 sind bereits geschrieben und werden durch ~14 Zeilen HLSL
blockiert.

1. `HlslSources.h:626-641`: den `[unroll]`-Loop durch vier Aufrufe einer Hilfsfunktion
   `giLocalVisOne(int idx, float3 P, float3 N)` mit **literalem** Index ersetzen, die einen
   Skalar zurückgibt. Kein dynamischer Vektor-Lvalue, kein `break`/`continue`.
   Den falschen Kommentar `:628-629` mitkorrigieren.
2. Prüfen, ob D3D11 nach dem Fix `giSupported` tatsächlich behält (`:1677` → `:1706`), und
   ob auf D3D12 der DXR-Zweig (`D3D12Renderer.cpp:4220`) jetzt erreicht wird. Er sollte —
   sobald der SW-Compile durchläuft, ist `ok` wahr, das frühe `return` (`:4204-4213`)
   entfällt und `createGiHwPipelines()` läuft. **Für P0 ist keine Umstellung nötig.**

> **Nicht tun: `createGiHwPipelines()` vor den `if (!ok) return`-Block ziehen.** Der
> Blockkommentar `:4218-4219` sagt, es sei ein „optional DXR 1.1 upgrade **on top of** the
> working SW pipelines" — der Fehlerpfad hat unmittelbar davor `giShadowPSO`/`giProbePSO`
> und die Heaps `Reset()`. Vorziehen hieße, den HW-Aufbau auf abgeräumten Ressourcen laufen
> zu lassen. Ob die Kopplung *dauerhaft* bestehen bleiben soll (heute reißt ein künftiger
> SW-Shader-Bruch den DXR-Pfad wieder mit), ist eine **offene Frage** — sie verlangt zu
> prüfen, was `createGiHwPipelines()` tatsächlich aus dem SW-Aufbau liest. Das ist bewusst
> nicht Teil von P0.

**Verifikation — Teil 1 (statisch, erledigt):**
`fxc /T cs_5_0 /E GiShadowCS` auf dem konkatenierten `kGiTraversalHLSL + kGiShadowCSHLSL`
läuft fehlerfrei durch (vorher: X3500 + X3511). Der Sweep über alle 29
Entry-Point/Profil-Kombinationen ist grün.

**Verifikation — Teil 2 (GPU, erledigt):** Abgenommen über das Init-Log (siehe Statusblock
oben). Der *naheliegende* Weg — ein Bild-A/B — funktioniert dagegen **nicht**; hier steht,
was ich probiert habe, damit es niemand ein zweites Mal probiert:

- Ein Headless-A/B `HE_DUMP_RHI=D3D11|D3D12|OpenGL` × `HE_DUMP_GI=1|0` mit der
  Bleed-Zeugenszene (`HE_DUMP_MATERIALTEST=1 HE_DUMP_GIBLEED=1`, bis `HE_DUMP_FRAMES=60`)
  erzeugt zwar auf allen Backends Bilder, aber GI-an und GI-aus sind praktisch identisch
  (max. 1 LSB) — **auch auf OpenGL, wo im selben Lauf „GI probe grid 7x6x7 (294 probes)"
  geloggt wird, GI also nachweislich lief.** Der Dump bildet GI somit auf *keinem* Backend
  ab; das ist ein Mangel von Szene/Kamera, nicht von D3D. Wer es erneut versucht, muss
  zuerst Kamera und Tageszeit (`HE_DUMP_CAMX/Y/Z`, `HE_DUMP_YAW/PITCH`, `HE_DUMP_TOD`) so
  setzen, dass die Sphären-Unterseite über dem roten Boden im Bild liegt.
- Eine Sackgasse beim Loggen: den Editor live laufen zu lassen und abzuschießen verliert
  das gepufferte Log (bricht reproduzierbar bei ~8 kB ab, egal ob PowerShell-Pipe,
  `Start-Process -RedirectStandardOutput` oder `cmd`-Umleitung). Nur der saubere Exit über
  `HE_DUMP_QUIT=1` liefert ein vollständiges Log — und genau deshalb muss alles, was man
  beobachten will, **bis zum Ende von `Initialize`** passiert sein. Das ist der zweite
  Grund für die eifrige Pipeline-Erzeugung.

> **Widerlegte Vermutung** (steht hier, weil sie naheliegt und falsch ist): Der GI-Block
> hängt hinter `if (io.output.id != kBackbufferTarget) return;` (`D3D11Renderer.cpp:3550`),
> was wie ein Ausschluss der Offscreen-Viewport-Ansicht aussieht. Ist es nicht.
> `kBackbufferTarget = 0` heißt „aktives Ausgabeziel" (`RenderTarget.h:28`), und der
> `GeometryPass` trägt diese id **immer** (`RenderPass.cpp:160`) — in beiden Pfaden. Der
> Gate trennt nur Geometrie- von Shadow-Pass.

### P0b — Shader-Prüfung in den Build ziehen

> **Stand 2026-08-11: ERLEDIGT.**
> `scripts/validate_embedded_shaders.py` + CMake-Ziel `HeValidateShaders` (in `ALL`,
> abschaltbar über `-DHE_VALIDATE_SHADERS=OFF`). Deckt **75 Shader** ab: 36 HLSL-Jobs
> über `fxc` (SM 5.0) und 39 GLSL-Stages über `glslangValidator` — deutlich mehr als die
> 29, die ich für §1.1 von Hand geprüft hatte. Laufzeit ~3 s.
>
> Beide Eigenschaften sind nachgewiesen, nicht angenommen:
> * Stellt man den Original-`[unroll]`-Loop wieder her, meldet das Skript Exit 1 mit
>   X3500 + X3511 auf genau `kGiShadowCSHLSL`.
> * Ein absichtlich fehlendes Semikolon lässt den **Build** abbrechen
>   (`FAILED: … HeValidateShaders` → `ninja: build stopped`).
>
> Fehlt fxc oder glslangValidator, wird die betreffende Hälfte mit Meldung übersprungen,
> statt fehlzuschlagen — ein Checkout ohne Windows- bzw. Vulkan-SDK baut weiterhin.

P0 repariert einen Fall. P0b sorgt dafür, dass der nächste überhaupt auffällt (§1.2).

Ein CMake-/CI-Schritt, der die eingebetteten Shader-Strings extrahiert und übersetzt:
HLSL gegen `fxc` (SM 5.0), GLSL aus `OpenGLRenderer.cpp` gegen `glslangValidator`. Die
beiden Datei-Shader-Pfade (Vulkan `.glsl`, D3D12 `gi_*_hw.hlsl`) sind bereits abgedeckt und
brauchen nichts.

Der HLSL-Umfang ist bekannt und klein: **21 Strings, 29 Entry-Point/Profil-Kombinationen**,
verteilt auf drei Dateien — `HlslSources.h` (geteilt) sowie je eine eigene Kopie von
Szene/Sky/FXAA/Bloom/UI in `D3D11Renderer.cpp:65-725` und `D3D12Renderer.cpp:397`. Zwei
Strings sind reine Präludien ohne eigenen Entry (`kGiTraversalHLSL`, `kSkyFuncHLSL`); der
Extraktor muss die Konkatenationen der Aufrufstellen nachbilden — `kSkyFuncHLSL + kSceneHLSL`
(`D3D11Renderer.cpp:2309`), `kSkyFuncHLSL + kSkyPSHLSL` (`:2802`), `kGiTraversalHLSL +
kGiShadowCSHLSL` (`:1677`) und `+ kGiProbeCSHLSL` (`:1679`). Ohne das erzeugt er
Falschmeldungen (`undeclared identifier 'skyColor'`).

**Verifikation:** erledigt, siehe Statusblock oben. Zwei Fallstricke, die beim Bau der
Extraktion aufgefallen sind und die jede spätere Erweiterung wieder treffen werden:

* **`main` ist nicht immer ein Pixel-Shader.** `kFSTriangleVS` wird mit Entry `main` als
  `vs_5_0` übersetzt (`D3D12Renderer.cpp:1295`, `:3040`, `:4079`). Deshalb gibt es im
  Skript eine explizite Ausnahmetabelle statt einer Namens-Heuristik.
* **D3D12 hat eigene Kopien unter eigenen Namen** — `kSkyPSHLSL12` (`:95`) braucht dasselbe
  `kSkyFuncHLSL`-Präludium wie D3D11s `kSkyPSHLSL`. Wer nur nach dem D3D11-Namen sucht,
  hält die D3D12-Kopie fälschlich für kaputt.

Gegen genau dieses Verrotten hat das Skript eine **Abdeckungsprüfung**: existiert ein
Shader-String, den kein Job erfasst, schlägt es mit „not covered by any job" fehl. Ein
Validator, der neue Shader stillschweigend überspringt, ist schlechter als keiner — er
liest sich wie ein bestandener Test.

### P1 — Editor-Integration nachziehen (D3D11, D3D12, Vulkan)

Der geschlossenste Block der Matrix: elf Features, auf allen drei Zielbackends fehlend, und
**GL ist überall die portable Referenz** — kein Apple-Mechanismus, kein neuer Shader.
Nutzerwirkung ist unmittelbar: heute bleiben Content-Browser-Thumbnails und Vorschaufenster
leer, sobald jemand das Backend umstellt.

- P1a — `InvalidateTexture` vervollständigen (`~` in allen dreien), `WarmupMaterials`.
- P1b — Thumbnails: Asset, Partikel, Widget (Offscreen-Target + Readback existiert überall
  schon, siehe `CaptureViewport` = `JA`).
- P1c — Vorschauen: Material, Skeletal (Bone-Overlay), Partikel.
- P1d — Multi-Window auf D3D11/D3D12 (`AttachWindow`/`DetachWindow`/`RenderWindow`).
  Vulkan hat es bereits. Achtung: Metals Variante legt nur die Swapchain an und rendert
  **keinen Inhalt** — Zielzustand ist GL, nicht Metal.
- P1e — `SetShadowDebug` (Cascade-Tint) und Per-Pass-GPU-Timing. Letzteres ist auf D3D12
  Timestamp-Queries pro Pass, auf Vulkan `vkCmdWriteTimestamp` — Whole-Frame-Timing steht
  überall schon, die Slot-Verwaltung ist die eigentliche Arbeit.

**Verifikation:** Editor unter jedem Backend starten, Content-Browser öffnen — Thumbnails
müssen erscheinen und wie unter GL aussehen. `HE_DUMP_THUMB` / `HE_DUMP_PREVIEW` /
`HE_DUMP_PREVIEWMESH` headless gegen die GL-Ausgabe. Terrain-Sculpting (feuert
`InvalidateMesh` pro Chunk und Frame) darf weder crashen noch Deskriptoren leaken.

### P2 — SSR verdrahten (D3D11, D3D12, Vulkan — GL optional mit)

Nach §1.3 ist die Shader-Seite fertig und bewiesen übersetzbar. Zu bauen ist nur der
C++-Rahmen.

**Scope-Hinweis:** Gefragt waren D3D und Vulkan. GL steht hier trotzdem, weil die Matrix
zeigt, dass GL SSR ebenfalls nicht hat, und weil der Cross-Compiler auch GLSL 410 liefert —
die Verdrahtung ist dieselbe. Das ist ein Vorschlag, keine stillschweigende
Scope-Erweiterung: wer nur D3D und Vulkan will, lässt P2 für GL einfach weg.

- P2a — `SetSSRSettings`-Override in allen drei Zielbackends (heute nirgends vorhanden,
  `IRenderer.h:243` bleibt der No-Op-Default).
- P2b — MRT-Reflection-Prepass: Metals Variante braucht drei Attachments (View-Position,
  oktaedrische Weltnormale, NDC-Tiefe). Heute hat D3D11 ein einzelnes Positions-RT
  (`:1311`), D3D12 genau drei RTVs im `ssaoRtvHeap` (`:2322`), Vulkan einen
  Ein-Attachment-`m_ssaoPosRenderPass`. **Das ist der eigentliche Aufwand der Phase.**
- P2c — Trace/Blur/RoughMix/Composite-Pässe an die vorhandenen
  `MaterialShaderLibrary::ssr*(backend)`-Accessoren hängen.
- P2d — `heLight.ssr` im Lighting-ABI-Fill schreiben, damit der bereits einkompilierte
  Forward-Konsument (`heSSRFwd`) aufhört, wegkonstantengefaltet zu werden.

**Verifikation:** `HE_DUMP_SSR=1`, `HE_DUMP_SSRTEST`, `HE_DUMP_SSRTESTWALL`,
`HE_DUMP_SSRQUALITY`, `HE_DUMP_SSRTESTROUGH` — die Testszenen existieren bereits und sind
auf Metal eingefahren. Pro Backend gegen den Metal-Referenzshot.

### P3 — Himmel und Wetter nachziehen

Größter reiner Shader-Portierungsblock — und der am besten vorbereitete: `sky.frag:1-50`
listet **selbst** auf, was gegenüber GL fehlt, und GL ist ausdrücklich die Referenz
(`sky.frag:6-8`; Metal spiegelt GL, nicht umgekehrt).

Wichtig: die Audit-1a-Entscheidung dort lautete „DOCUMENT the drift, do not port". Diese
Phase kehrt sie um. Das ist eine bewusste Änderung, keine Übersehung.

- P3a — Atmosphäre: `atmoScatter`/`atmoRaySphere` (Rayleigh/Mie/Ozon) ersetzt den alten
  Gradienten `skyColor()`. **Größter optischer Einzelunterschied.** Laut Abgleich kein
  infrastruktureller Blocker in allen drei Backends — Pass, PSO/Pipeline, CB/UBO und die
  gemeinsame `BuildSkyFrameParams`-Übersetzung stehen. Reine Shader-Portierung aus
  `OpenGLRenderer.cpp:2465-2526`. Da `kSkyFuncHLSL` zwischen D3D11 und D3D12 geteilt ist,
  zahlt sich die HLSL-Portierung doppelt aus.
- P3b — CB/UBO-Erweiterungen. Die tatsächliche Fehlerquelle dieser Phase: `SkyFrameParams`
  ist 336 Bytes, der Vulkan-Block 160 (`sky.frag:35-45`). `SkyUBOData`
  (`VulkanRenderer.cpp:61-69`) und der std140-Block müssen **offset-synchron** wachsen.
  D3D12 hat einige Slots bereits, D3D11 nicht — die Erweiterung ist von D3D12 nach D3D11
  1:1 abschreibbar.
- P3c — Mond (Phase, Corona, prozedurale Oberfläche), Nachthimmel v2→v3.4 (3-Farb-Nebel,
  Milchstraßen-Rift, 3D-Aurora, Meteore).
- P3d — Wolken: `applyClouds` auf `out float outT` erweitern (Transmittanz wird intern
  bereits berechnet), dann 3D-Volumen-Raymarch und die Zusatz-Layer (Cirrus,
  Kondensstreifen, Regenbogen, God-Rays, Sonnenscheibe/Glare).
- P3e — Low-Res-Cloud-Pass (Viertelauflösung + Reprojektion). Braucht ein zusätzliches
  RGBA16F-Target samt Sicht-Paar; ein HDR-Target ist **nicht** der Blocker, das existiert.
- P3f — GPU-Wetter-Partikel. **Compute ist nicht der Blocker** — alle drei haben
  nachweislich Compute (D3D11 `:1678-1680`/`:1986`, D3D12 `:4109-4113`/`:4643`, Vulkan
  `:5762-5763`). Es fehlen Partikelpool-Buffer, Simulations-Kernel und der
  Vertex-Pull-Billboard-Pfad.

**Verifikation:** Die `HE_DUMP_*`-Familie deckt diesen Bereich am dichtesten ab —
`HE_DUMP_SKYTEST`, `HE_DUMP_TOD`/`HE_DUMP_SKYTIME`, `HE_DUMP_CLOUDMODE`,
`HE_DUMP_LOWRESCLOUDS`, `HE_DUMP_NEBULA`, `HE_DUMP_AURORA`, `HE_DUMP_MOONPHASE`,
`HE_DUMP_GODRAYS`, `HE_DUMP_CIRRUS`, `HE_DUMP_CONTRAILS`, `HE_DUMP_METEORS`,
`HE_DUMP_RAIN`. Tageszeit-Sweep pro Backend gegen GL. Nach P3b zusätzlich ein UBO-Offset-Test:
ein Feld setzen und prüfen, dass **genau** das im Shader ankommt.

### P4 — Deferred-Pfad (D3D12, Vulkan; D3D11 nach Maßgabe)

Erst hier, weil es das erste ist, das echte neue Infrastruktur braucht: heute wertet **kein**
Zielbackend `SetRenderPath` aus (`grep -c RenderPath` = 0 in allen dreien).

- P4a — MRT-G-Buffer-Target (4 Attachments + Tiefe) und der eigene Extract/Cull/Sort-Zweig.
- P4b — Zwei-Pass-Resolve. Der Shader dafür wird **schon heute** für jedes Backend übersetzt
  (§1.4) — nur der Pass fehlt.
- P4c — G-Buffer-Variante für Node-Graph-Materialien inkl. Forward-Routing-Fallback.
- P4d — `SetRenderPath`-Gate scharf schalten.
- P4e — *Optional, danach:* die Clustered-Sperre in `compileResolveVariant:1030` für
  D3D11/D3D12/Vulkan öffnen (die GL-4.1-Begründung trägt für sie nicht) und den
  CPU-Scatter-Build portieren.

**Verifikation:** `HE_DUMP_RENDERPATH` + `HE_DUMP_GBUFFER` pro Backend; Forward- und
Deferred-Shot derselben Szene müssen einander entsprechen und beide dem GL-Forward-Shot.

### P5 — GI-Reflections (D3D12, Vulkan)

Zuletzt, weil hier als einzigem Block **kein Shader existiert**, den man portieren könnte:
`HlslSources.h` hat im GI-Bereich (`:405-875`) kein `kGiRefl*`, und
`src/HE_Rendering/shaders/` kein `gi_refl*`. GL hat einen Kernel — aber mit ausdrücklich
dokumentierten Einschränkungen (`gi-reflections-plan.md:316-327`: kein Bounce-Loop, kein SSR
in der Kaskade, kein Sky-Cubemap-Sampling, Terrain und skinned Meshes nicht in der BVH).

Voraussetzung, die vor dem Kernel steht: `GiInst` (`HlslSources.h:455`,
`gi_probe.comp:19`) trägt heute weder Landschaftsindex noch Metallic/Roughness pro Instanz.
Ohne diese Felder sind Painted-Landscape-Tabelle und Multi-Bounce-Spiegelkette nicht
darstellbar. Das ist der eigentliche erste Schritt, nicht der Kernel.

D3D11 nimmt an dieser Phase nicht teil (§3).

**Verifikation:** `HE_DUMP_GIREFL`, `HE_DUMP_GIREFLTEST`, `HE_DUMP_GIREFLQUALITY`,
`HE_DUMP_GIREFLROUGH`, `HE_DUMP_GIREFLBLUR`, `HE_DUMP_GIREFLBOUNCES`,
`HE_DUMP_GIREFLLANDSCAPE` gegen die Metal-Referenz.

---

## 6. Veraltete Dokumentation (nicht stillschweigend korrigiert)

Beim Abgleich sind drei Stellen aufgefallen, die heute etwas Falsches behaupten. Sie sind
hier festgehalten statt einfach umgeschrieben, weil das Absicht sein könnte:

1. **`CopilotDocs/windows-gpu-verification-checklist.md`, Abschnitt A4** — „Aktuell rendert
   jedes über den Material-Node-Graph gebaute Material auf D3D/Vulkan gar nicht wie gebaut
   (die Renderer rufen `MaterialShaderLibrary` nie auf — nur GL+Metal tun das)."
   Das stimmt nicht mehr: D3D11 (`:2458`), D3D12 (`:5414`) und Vulkan (`:1977`) rufen die
   Library alle auf, und CMake linkt `he_materialshader` in jedes Backend. A4 ist
   implementiert und wartet nur noch auf die Hardware-Abnahme.

2. **`CopilotDocs/ROADMAP.md:20`** — „Backends (GL 4.1/4.6, Metal, Vulkan*, D3D11/12*)
   🟡 Clear + ImGui-Overlay, keine Draw-Calls". Stand Juni 2026 und lange überholt; alle
   fünf Backends zeichnen Geometrie.

3. **`HlslSources.h:627-628`** — der Kommentar behauptet FXC/SM5.0-Sicherheit für genau die
   Konstruktion, die FXC ablehnt (§1.1). Wird in P0 mitkorrigiert.

Ebenfalls erwähnenswert, ohne Fehler zu sein: `docs/deferred-renderer-plan.md:34-48` ist ein
überholter Vorgänger-Statusblock, der `:8-32` desselben Dokuments widerspricht. Maßgeblich
ist `:8-32`.

---

## 7. Was dieser Plan nicht leistet

Er ist **blind gegenüber der tatsächlichen Optik**. Es gibt keine GPU-Paritätstests — die
Suite deckt nur die CPU-Seite ab (`test_rendergraph`, `test_gi_bvh`, `test_culling`), und die
CI verifiziert ausschließlich, dass kompiliert wird. Jede Phase hier endet deshalb mit einem
`HE_DUMP_*`-A/B gegen OpenGL bzw. Metal auf echter Hardware; „kompiliert grün" ist in diesem
Repo nachweislich kein Beleg dafür, dass ein Feature überhaupt läuft — §1.1 ist der Beweis.
