# Landscape im Weltmaßstab: das Zeichnen auf OpenGL, Metal, D3D11 und D3D12 (Schritt 2, Teil B)

Stand 02.11.2026, Basis `release/0.8.0`, Hive-Topic 326, Schritt 2 von 8 ("Quadtree-Renderer, Vulkan zuerst").
Vorher: `landscape-world-scale-renderer-2026-10-31.md` (Teil A: Quadtree, Gitter, Atlas, Laufzeit, Shader-Quelle).

**Teil B ist für OpenGL und Metal (Mac), D3D11 und D3D12 (NN-WS03, RTX 4070, siehe §8) gebaut und ausgeführt: die Backends zeichnen die Patches des Quadtrees, im echten Renderer (Forward und Deferred) und in einem eigenen Prüfwerkzeug.
Vulkan ist nicht gemacht (eigene Aufgabe, Vulkan-SDK nötig).** "Vulkan zuerst" im Schritt-Titel ist eine Reihenfolge, kein Verzicht auf die anderen Backends: der Datenpfad (§2) ist für alle derselbe.
Es wurde nichts gemessen: keine Zeit, kein GPU-Speicher im Betrieb (Stromsparmodus; Plan-§4-Abnahme auf NN-WS03). Schritt 1520 bleibt offen.

## 1. Was gebaut ist

| Datei | Aufgabe |
|---|---|
| `HE_Core/Renderer/LandscapeRenderFrame.h` | der Frame in Typen, die die Backends sehen (sie linken nur `HorizonCore`): Patches, **schon gepackte** Bild-Bytes, Tabellen-Änderungen, die statischen Bilder; `LandscapeProvider` |
| `IRenderer::SetLandscapeProvider` | nicht virtuell (wie `SetWaterRipples`: kein Verschieben der vtable), Member zuletzt |
| `LandscapeRuntime::buildRenderFrame / renderProvider` | `LandscapeFrame` + die Bytes aus `landscapePackTile` / `landscapePackCoarse`; die statischen Bilder einmal je Landschaft (`generation`) |
| `OpenGLLandscape.*` (`Backends/OpenGL`) | Programm, Gitter, Atlas-Bilder, Uploads, Zeichnen; GL 4.1 Core |
| `MetalLandscape.*` (`Backends/Metal`) | dasselbe in Metal; reines C++-Header |
| `kLandscapeMSL` in `LandscapeShaderSource.h` | die MSL-Fassung der beiden Shader, von Hand, neben dem GLSL |
| `OpenGLRenderer::DrawLandscape`, `MetalRenderer::EncodeLandscape` | der Haken im Szenen-Pass |
| `GameApplication`, `EditorApplication` | `LandscapeRuntime` je Frame, Provider einmal gesetzt; im Editor im Bearbeiten-Modus um die Editor-Kamera (`update(..., cameraOverride)`) |
| `tools: landscape_gpu_check` | Prüfwerkzeug (§6) |
| Tests | `test_landscape_runtime` (Render-Frame), `test_landscape_shader` (MSL gegen GLSL) |

## 2. Der Vertrag zwischen Anwendung und Backend

Das Backend ruft, einmal je Szenen-Frame (Metal: ganz oben in `EncodeScene`, vor den Sky-only-Abkürzungen; GL: im Haken), `provider(viewProj, zeroToOne, frame)` mit der Matrix, mit der es **gleich selbst rastert** (so entfällt "die Anwendung rechnet Editor-Kamera und Seitenverhältnis nach"), und führt den Frame aus, bevor es zeichnet: **jeder gebaute Frame muss ausgeführt werden**, die Laufzeit zählt die Uploads als erledigt.
Der Ursprung (`world.origin()`) ist Sache des Providers: der Render-World trägt nur ursprungsrelative Positionen, das Backend kennt ihn nicht. Der Provider ruft die Laufzeit auf dem Hauptthread; nur der Haupt-Szenenpass fragt, nie eine Vorschau oder ein Thumbnail.

| Teil | Inhalt | Wann neu |
|---|---|---|
| `LandscapeRenderStatic` (geteilt, `shared_ptr`) | Zahlen (Kachelgröße, Weltrand, Höhenskala, Fenster, Kappen), Mip-Rechtecke, Grobwelt-Höhen (gestapelt) und -Gewichte, Ebenenfarben, das 65²-Gitter als Indizes samt Quadranten-Bereichen | wenn `generation` wechselt: alle Bilder neu, Slot-Tabelle auf 65535 |
| `uploads[]` | je Slot: `heightW x heightH` `uint16` und `weightPages` Seiten `weightW x weightH x 4` Bytes | je Frame, höchstens `maxUploadsPerFrame` (8) |
| `tableUpdates[]` | Eintrag `(z & (D-1)) * D + (x & (D-1))`, Slot oder 65535 | je Frame, in Reihenfolge, **nach** den Uploads |
| `patches[]` | Stufe, Quadranten, Grobwelt ja/nein und Raster, ganzzahlige Eck-Zelle, Ecke und Größe ursprungsrelativ, Morph-Bereich | je Frame |

Die Kacheln selbst bleiben im Store: die Bytes sind gepackt, bevor der Frame das Haus verlässt, der `TilePtr` wird danach losgelassen (Test: `use_count() == 2`).

## 3. Bilder, je Backend

| Bild | Format | OpenGL | Metal |
|---|---|---|---|
| Kachel-Höhen | R16UI, eine Schicht je Slot | `GL_TEXTURE_2D_ARRAY` (`glTexImage3D`: `glTexStorage3D` fehlt unter macOS 4.1), Einheit 21 | `texture2d_array<ushort>`, Textur 20 (Vertex) |
| Slot-Tabelle | R16UI, `D x D` | `GL_TEXTURE_2D`, 22 | `texture2d<ushort>`, 21 (Vertex) / 20 (Fragment) |
| Grobwelt-Höhen | R16UI, gestapelt | `GL_TEXTURE_2D`, 23 | `texture2d<ushort>`, 22 (Vertex) |
| Kachel-Gewichte, 3 Seiten | RGBA8 | 3 Arrays, 24 bis 26 | `texture2d_array<float>`, 21 bis 23 (Fragment) |
| Grobwelt-Gewichte, 3 Seiten | RGBA8 | 3 Texturen, 27 bis 29 | `texture2d<float>`, 24 bis 26 (Fragment) |

Alles wird mit ganzzahligen Abrufen gelesen (`texelFetch` / `read`), nichts gefiltert; in GL bekommt jede Textur trotzdem `NEAREST`, sonst ist sie unvollständig. Die Einheiten liegen **über allem, was der Renderer nutzt** (GL: ab 21, der Renderer geht bis 20; Metal: ab 20, der Renderer bis 16),
damit der Haken keine Bindung des Renderers überschreibt, die später im Pass noch gebraucht wird. Das Metal-Struct `LsGlobals` (768 Bytes, Puffer 20) und `LsPatch` (48 Bytes, Puffer 21 je Draw) haben ein C++-Spiegelbild mit `static_assert` auf die Größe.

## 4. Wo gezeichnet wird, und die Konventionen

* **Position:** im Opaque-Pass nach den Skinned-Meshes, vor dem Himmel (GL: vor `GpuTimerEndPass("Opaque")`, Metal: zwischen `"Skinned"` und `drawSky()`), mit Tiefentest und Tiefenschreiben, ohne Culling, ohne Blending. Der Himmel (`LEQUAL`, z = 1, kein Schreiben) füllt nur, was die Landschaft übrig lässt. Auf beiden Pfaden (Forward, Deferred) läuft derselbe Haken, nach dem Resolve.
* **Metal-Tiefe (Fund):** der Metal-Szenenpass rastert mit der **unkorrigierten GL-Matrix** (`u.mvp = viewProj * model`, `HE::kMetalClipFix` wird dort nicht angewandt; der G-Buffer legt die GL-NDC-Tiefe ab). Damit Landschaft, Objekte und Himmel sortiert zusammenpassen, nimmt `MetalLandscape::encode` dieselbe Matrix und der Frustum-Aufruf `zeroToOne = false`. Folge, wie für jeden Szenen-Draw: Geometrie näher als etwa das Doppelte der Near-Ebene wird abgeschnitten.
* **GL-Zustand:** `StateGuard` setzt Programm, VAO, aktive Einheit, Tiefenzustand, Culling, Blending und Unpack-Ausrichtung zurück. Metal setzt Pipeline und Tiefenzustand selbst; jeder folgende Pass des Renderers setzt seine wieder. **Shader und Pipeline entstehen erst beim ersten aktiven Frame**: eine Szene ohne Landschaft (Provider gesetzt, `frame.active` falsch) übersetzt nichts, zeichnet nichts und lässt die Sky-only-Abkürzungen des Metal-Renderers wie sie waren.
* **Beleuchtung:** erste Richtungslicht-Farbe mal Intensität, `sunDirection`, `ambient` der Render-World; Ebenenfarben flach (Material-Graph später, Welt-UVs nötig, siehe Teil-A-Dokument §8).

## 5. Bekannte Grenzen (nicht gelöst)

| Grenze | Folge |
|---|---|
| keine Schatten, kein SSAO, kein GI, kein Nebel, keine SSR | die Landschaft ist nicht im G-Buffer und nimmt an keinem dieser Passes teil; auf dem GL-Deferred-Pfad ist sie reiner Forward-Draw |
| **TAA-Geschwindigkeit** | die Landschaft schreibt keine Bewegungsvektoren: bei Kamerabewegung mit TAA Geisterbilder zu erwarten (nicht untersucht) |
| Metal-Uploads per `replaceRegion` | ein noch laufender GPU-Frame könnte einen eben neu belegten Slot lesen; betrifft nur Kacheln, die die geladene Scheibe verlassen haben, also höchstens einen Frame einer fernen Kachel (Ring-Puffer wären die Lösung) |
| je Patch ein `glUniform`-Block bzw. `setVertexBytes` | rund 180 Patches in 203 Draws im Test; **ob das reicht, ist nicht gemessen** |
| Draw-Aufrufe je Patch, kein Instancing, kein Frustum-Test auf der GPU | bewusst einfach |
| GL: 9 Texturen-Einheiten ab 21 | ein Kontext mit weniger als 30 kombinierten Einheiten schaltet die Landschaft ab (Warnung, einmal) |
| der Renderer ruft den Provider **zweimal**, wenn Viewport-Frame und Screenshot im selben App-Frame laufen | der zweite bekommt keine Uploads mehr (sie sind beim ersten ausgeführt), die Bilder bleiben im selben Backend; ein Screenshot ohne vorherigen Viewport-Frame liefert die Uploads selbst |
| `m_landscapeBound` (Spiel, Editor) ist einmalig | ein neu erzeugter Renderer müsste den Provider neu setzen, und die Laufzeit `reset()` (sonst hält sie Uploads für Bilder, die der neue Renderer nicht hat) |
| Physik, Navigation, Foliage und Wasser (Schritt 3) hängen **nicht** an Spiel und Editor | Spiel und Editor rufen `update` (Store, Atlas), aber `setPhysics`, `setNavigation`, `setFoliageLayers` und die Wasser-Zeichnung sind Aufrufer-Sache und noch nicht verdrahtet |

## 6. Das Prüfwerkzeug `landscape_gpu_check`

Kein Test (braucht Fenstersystem und GPU, nicht in der CI; `src/HE_Tools/src/LandscapeGpuCheck`). Zwei Schichten je Backend:

1. **Offscreen**: die Backend-Klasse (`OpenGLLandscape` im eigenen GL-Kontext, `MetalLandscape` in eigenem Pass, RGBA16F + Depth32F wie der Renderer) zeichnet eine **Rampe** `y = 600 + 0.1 · (x − 3072)` über 32 x 32 Kacheln, alles Farbe der Ebene 0. Eine Ebene wird auf jeder Stufe exakt wiedergegeben, also ist die Höhe an jedem Pixel bekannt. Je Pixel: Strahl gegen Ebene, dann
   *Loch/Zuviel* (gezeichnet, wo die Ebene ist, bis 5 km), *Tiefe* (Abstand zur gezeichneten Fläche gegen Abstand zur Ebene, Toleranz 0,35 m + 3e-4 · Abstand) und *Farbe* (Ebenenfarbe mal Ambient + Sonne · n·l, 4/255, bis 5 km, also auch über die Grobwelt-Gewichte).
   Danach springt die Kamera 10 km (der Atlas fasst rund 190 der 1024 Kacheln: Slots werden frei und wiederverwendet, `releases > 0`), und dieselben Prüfungen laufen auf denselben Bildern. Eine zweite Szene (die fBm-Hügel der Engine) wird nur auf "Boden da" geprüft und als PNG geschrieben.
2. **Engine**: der **echte Renderer** (`RendererFactory`, verstecktes Fenster, Szenen-Pass des Backends, **Forward und Deferred**) zeichnet dieselbe Welt über `SetLandscapeProvider` und `RenderSceneImage`; geprüft wird, ob dort Boden ist, wo die Ebene ist (>= 97 % grün), und nichts Grünes darüber (<= 3 %). Das Bild ist tone-gemappt, also keine Farbgenauigkeit.

3. **OpenGL gegen Metal**: beide Backends zeichnen dieselben Frames; mehr als 0,1 % der Pixel mehr als 2/255 auseinander ist ein Unterschied zwischen den beiden Portierungen, kein Rundungsfehler. Das ist die einzige Stelle, an der ein Morph- oder Abruf-Fehler in nur **einer** der beiden Fassungen auffällt: eine Ebene morpht auf sich selbst, die Hügel sind es, die zählen.

Ergebnis dieses Laufs (Mac, Apple-Silikon), **gezählt, nicht gemessen**:

| | OpenGL | Metal |
|---|---|---|
| Rampe, erste Position | 180 Patches in 203 Draws; Löcher 0, Zuviel 0; Tiefe schlecht 6 von 80 176 px; Farbe schlecht 0 von 81 143 (größte Abweichung 1/255) | gleiche Patches und Draws; Löcher 0, Zuviel 0; Tiefe schlecht 0 von 80 176; Farbe schlecht 0 |
| Rampe, nach 10 km | 175 Patches in 196 Draws; Löcher 1; Tiefe schlecht 4 von 62 608; Farbe 0 | wie links, Tiefe schlecht 0 |
| Atlas | 382 Uploads = 382 der Laufzeit; 191 Slots freigegeben; 0 abgelehnt | gleich |
| Echter Renderer, Forward / Deferred | Boden 100 % grün, Himmel 1 % grün; beide OK | gleich |
| GL gegen Metal | Rampe: 0 von 129 600 Pixeln über 2/255 verschieden; Hügel: 18 von 518 400 (9 davon über 16/255, Kanten) | |

**Absichtlich eingebaute Fehler** werden auf beiden Backends gefunden: ohne `- uOrigin.y` im Höhenshader stimmt jede Tiefe nicht mehr (größte Abweichung 2392 m); ohne die Grobwelt-Gewichte werden ferne Pixel grau (857 Pixel falsch, bis 65/255); ohne Morph nur in der MSL-Fassung weichen 14 791 von 518 400 Hügel-Pixeln ab (der Vergleich unter 3 schlägt an). Zwei weitere Fehler dieser Art hatte schon Teil A in den CPU-Tests gefunden.

## 7. Verifikation

Verifiziert (Mac, ohne Zeitmessung): `landscape_gpu_check` auf OpenGL und Metal wie in §6 (offscreen und echter Renderer, Forward und Deferred); `he_tests`: `test_landscape_*` 115 Testfälle (darunter der Render-Frame und die MSL-gegen-GLSL-Prüfung), `test_foliage*`, `test_physics*`, `test_world_scale*`, `test_scene_serializer*`, `test_cell_*`, `test_navigation*`, `test_sky*` grün;
`validate_embedded_shaders.py`: 60 GLSL-Shader übersetzen (der MSL-Teil wird hier übersprungen, weil dem Mac die Metal-Toolchain fehlt; `kLandscapeMSL` ist dafür ergänzt, und die Laufzeit-Übersetzung per `newLibraryWithSource` hat sie bestanden). `HorizonGame` linkt; `EditorApplication.cpp` übersetzt.
**Nicht verifiziert:** jede Zeit; GPU-Speicher im Betrieb (die Rechnung steht im Teil-A-Dokument: rund 494 KB je Slot); Verhalten mit TAA in Bewegung; ein Editor- oder Spiel-Lauf mit einer echten Szene (der Engine-Modus des Werkzeugs nimmt die Renderer-Schnittstelle, nicht die Anwendung); Windows- und Linux-Lauf des Prüfwerkzeugs (es wird dort nur gebaut); Intel-Macs (Metal `Managed`-Speicher ist vorgesehen, nicht gelaufen);
Vulkan. (D3D11 und D3D12: §8.)

## 8. D3D11 und D3D12 (Windows)

Ein Shader für beide: `kLandscapeHLSL` in `LandscapeShaderSource.h`, von Hand aus der MSL (SM 5.0, fxc). Vertex-ID statt Vertex-Puffer (`SV_VertexID` ist im Indexed-Draw der Indexwert: `(id % 65, id / 65)`, kein Input-Layout), zwei Konstantenblöcke (`LsGlobals` b12 = 768 Bytes, `LsPatch` b13 = 48 Bytes; `patch` ist in HLSL ein Schlüsselwort, das Mitglied heißt `lsPatch`), neun Bilder `t32`-`t40` nur mit `Load()` (kein Sampler). Die SRVs liegen über `t31`, weil der D3D11-Renderer und die Graph-Materialien `t0`-`t26` belegen und nach dem Landscape-Draw nicht neu binden. Das C++-Spiegelbild beider Blöcke samt `static_assert` und die Füllfunktionen stehen in `D3D_Shared/LandscapeD3D.h` (geteilt).

| Datei | Aufgabe |
|---|---|
| `D3D11Landscape.{h,cpp}` | Shader, Zustände, Index-Puffer, Atlas-Bilder (`UpdateSubresource` je Slot), Slot-Tabelle als CPU-Spiegel (ein `UpdateSubresource` je geänderter Tabelle); `draw` setzt alles, was es anfasst (Shader, IA, Raster, Tiefe, Blend, b12-b13, t32-t40), über einen `StateGuard` zurück |
| `D3D12Landscape.{h,cpp}` | Root-Signatur (b12 Root-CBV, b13 zwölf Root-Konstanten, Tabelle t32-t40), eigener Descriptor-Heap, ein PSO je Farbformat (RGBA16F / RGBA8, Tiefe D32F), Upload-Ring (4 Puffer, Barrieren SRV -> COPY_DEST -> SRV), CBV-Ring (32 Slots); Wechsel von Bildern oder Wachsen des Rings ruft `waitIdle` (= `waitForAllFrames`) |
| `D3D11Renderer.cpp`, `D3D12Renderer.cpp` | Haken (siehe unten) |
| `tools: D3D11Backend.cpp, D3D12Backend.cpp` | Offscreen-Backends des Prüfwerkzeugs; `main.cpp` vergleicht jedes Paar (OpenGL ist die Referenz, dazu D3D11 gegen D3D12) |

**Haken, anders als auf GL/Metal:** `DrawScene` zeichnet den Himmel **zuerst** und verlässt die Funktion früh, wenn die Szene keine Objekte hat. Eine Szene nur aus Landschaft würde nie in den Render-Graph kommen. Darum: der Provider wird **direkt nach dem Himmel** gefragt (vor beiden frühen Rückkehren, mit der Matrix, die auch die Objekte bekommen), die Uploads werden sofort ausgeführt (jeder gebaute Frame muss ausgeführt werden), gezeichnet wird im Opaque-Pass nach den Skinned-Meshes und vor TAA-Geschwindigkeit/Decals (Forward und Deferred, nach dem G-Buffer-Resolve), und auf den beiden frühen Rückkehren sofort. D3D12 baut nach dem Draw den Szenen-Zustand wieder auf (`restoreSceneRoot()` + `scenePso`, wie der Skinned-Pass).

**Tiefe und Matrix:** der Szenen-Pass beider D3D-Backends rastert wie Metal die **unkorrigierte GL-Matrix** (`kD3DClipFix` wird dort nicht angewandt, nur in der Welt-Vorschau); die Tiefe im Puffer ist die GL-NDC-Tiefe der nicht abgeschnittenen Hälfte. Der Haken reicht dieselbe Matrix und `zeroToOne = false` durch, die Prüfwerkzeug-Backends melden `depthIsNdc() = true`. Folge wie auf Metal: Geometrie näher als etwa das Doppelte der Near-Ebene wird abgeschnitten.

**Ergebnis** (NN-WS03, Release, RTX 4070 für alle drei APIs, gezählt, nicht gemessen):

| | OpenGL | D3D11 | D3D12 |
|---|---|---|---|
| Rampe, erste Position | 180 Patches in 203 Draws; Löcher 0, Zuviel 0; Tiefe schlecht 3 von 80 176; Farbe 0 (max. 1/255) | gleich, Tiefe schlecht **0** | gleich, Tiefe schlecht **0** |
| Rampe, nach 10 km | 175 in 196; Löcher 1; Tiefe schlecht 2 von 62 608 | gleich, Tiefe schlecht 0 | gleich, Tiefe schlecht 0 |
| Atlas | 382 Uploads = 382 der Laufzeit | gleich | gleich |
| Echter Renderer, Forward / Deferred | Boden 100 %, Himmel < 1 % grün | gleich | gleich |
| gegen OpenGL | | Rampe 0 von 129 600, Hügel 5 von 518 400 Pixeln über 2/255 | dasselbe |
| D3D12 gegen D3D11 | | | Rampe 0, Hügel 0 Pixel über 2/255 |

D3D12 mit Debug-Layer und `HE_GPU_DEBUG=1` (Werkzeug und echter Renderer, Forward und Deferred): keine Meldung. Dafür liest `RenderSceneImage` die InfoQueue jetzt auch (`drainGpuDebugMessages`, vorher nur `Render()`). Auf D3D11 gibt es nur Bildbelege (kein Debug-Layer im Renderer).
**Absichtlich eingebaute Fehler** werden auf beiden Backends gefunden (HLSL geändert, nur diese Quelle, dann zurück): ohne `- origin.y` stimmt jede Tiefe nicht mehr (größte Abweichung 2392 m, wie auf GL/Metal), ohne Morph weichen D3D11 und D3D12 von OpenGL bei 14 788 von 518 400 Hügel-Pixeln ab (6287 über 16/255).
`validate_embedded_shaders.py --hlsl-only` übersetzt `kLandscapeHLSL` (vs_5_0, ps_5_0; es läuft auch im CMake-Build-Schritt); `test_landscape_shader` prüft die HLSL-Fassung gegen die GLSL-Uniforms, Register und die Reihenfolge `ddx` vor `discard`.

## Handoff

- **Topic / step:** 326, Schritt 1520 (Quadtree-Renderer), Teil B auf D3D11 und D3D12; Schritt 1520 bleibt **offen** bis Vulkan fertig ist.
- **Done:** D3D11 und D3D12 zeichnen die Landschaft im echten Renderer (Forward, Deferred, Szene ohne Objekte) und im Prüfwerkzeug, Bild und Tiefe gleich OpenGL (§8).
- **Files:** `LandscapeShaderSource.h` (kLandscapeHLSL), `D3D_Shared/LandscapeD3D.h`, `D3D11Landscape.*`, `D3D12Landscape.*`, Haken in `D3D11Renderer.cpp`/`D3D12Renderer.cpp` (suche `landscapeFrameLive`), `tools/LandscapeGpuCheck/D3D{11,12}Backend.cpp`, `scripts/validate_embedded_shaders.py`.
- **Verified:** `landscape_gpu_check` (alle Backends dieser Maschine) wie §8; `he_tests` `landscape*` (140 Fälle); `validate_embedded_shaders.py --hlsl-only`; Build von `HorizonEditor`, `HorizonGame`.
- **Not verified:** jede Zeit und GPU-Speicher (Stromsparmodus); TAA in Bewegung (die Landschaft schreibt keine Geschwindigkeit); ein Editor- oder Spiel-Lauf mit echter Szene (nur der Engine-Modus des Werkzeugs); D3D11 mit Debug-Layer; Intel/AMD-Treiber (nur NVIDIA); eine Szene mit Objekten **und** Landschaft (Sortierung gegen Objekte ist wie auf Metal durch dieselbe Matrix gedacht, nicht gesehen).
- **Decisions taken:** ein HLSL für beide D3D-Backends; Provider direkt nach dem Himmel statt im Pass (frühe Rückkehr, §8); eigener Descriptor-Heap und Root-Signatur in D3D12 statt Slots im Szenen-Heap (der Renderer wechselt Heaps ohnehin dauernd); Upload-Ring mit `waitIdle` beim Wachsen statt Fence-Verwaltung in der Klasse.
- **Pending:** Vulkan (die GLSL-Quelle bleibt, sie bekommt ein Vorspiel wie der Himmel, `kLandscapeVulkanPrelude`: Konstantenblock statt loser Uniforms, `LsGlobals` ist die Vorlage; ob Vulkan die GL-Matrix wie D3D/Metal oder `kVulkanClipFix` (y gedreht) bekommt, zuerst am Szenen-Pass nachsehen, wie in §8 für D3D); Schritt 3 an die Anwendung hängen (`setPhysics`, `setNavigation`, `setFoliageLayers`, Foliage/Wasser zeichnen); Material mit Welt-UVs; Schatten/GI (Schritt 4); Messungen auf NN-WS03 (Schritt 0).
- **Traps:** Metal und D3D rastern die GL-Matrix ohne Clip-Korrektur (§4, §8); D3D11 hat nur b0-b13, SRVs ab t32; `patch` ist ein HLSL-Schlüsselwort; jeder gebaute Frame muss ausgeführt werden (auch bei Szenen ohne Objekte); die Upload-Jobs in `D3D12Landscape::upload` zeigen in den Frame, dessen Uploads erst am Ende der Funktion geleert werden dürfen; der Worktree-Build braucht `-DDEPLOY_DIR` und einen kurzen `-B`-Pfad; ein Push bricht die laufende CI ab.
- **Next steps:** 1. Vulkan wie §8 (Vorlage `D3D11Landscape`, Provider-Aufruf an derselben Stelle: nach dem Himmel, vor frühen Rückkehren). 2. Schritt 3 an die Anwendung hängen. 3. Material mit Welt-UVs.
