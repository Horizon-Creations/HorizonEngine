# Landscape im Weltmaßstab: das Zeichnen auf OpenGL und Metal (Schritt 2, Teil B)

Stand 01.11.2026, Basis `release/0.8.0`, Hive-Topic 326, Schritt 2 von 8 ("Quadtree-Renderer, Vulkan zuerst").
Vorher: `landscape-world-scale-renderer-2026-10-31.md` (Teil A: Quadtree, Gitter, Atlas, Laufzeit, Shader-Quelle).

**Teil B ist für OpenGL und Metal gebaut und auf diesem Mac ausgeführt: beide Backends zeichnen die Patches des Quadtrees, im echten Renderer (Forward und Deferred) und in einem eigenen Prüfwerkzeug.
Vulkan, D3D11 und D3D12 sind nicht gemacht (Reihenfolge laut Plan, Vulkan-SDK und D3D-Rechner nötig).** "Vulkan zuerst" im Schritt-Titel ist eine Reihenfolge, kein Verzicht auf die anderen Backends: der Datenpfad (§2) ist für alle derselbe.
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
Vulkan, D3D11, D3D12.

## Handoff

1. **Vulkan, D3D11, D3D12:** `LandscapeRenderFrame` ist der Eingang, `OpenGLLandscape` die kürzeste Vorlage (Bilder, Gitter, Uploads, ein Draw je Patch). Vulkan/D3D bekommen ihr Shader-Vorspiel wie der Himmel (`kLandscapeVulkanPrelude`: Block statt loser Uniforms, `LsGlobals` ist die Vorlage); ihre Clip-Konvention (`kVulkanClipFix`, y gedreht) gehört in den Provider-Aufruf (`zeroToOne = true`) und in die Matrix des Backends.
2. Schritt 3 an die Anwendung hängen: `setPhysics`, `setNavigation`, `setFoliageLayers`, Foliage- und Wasser-Zeichnung (Draw-Verträge im Foliage/Wasser-Dokument).
3. Fallen: Metal-Tiefe ist die GL-NDC-Tiefe (§4); die Einheiten und Indizes über den Renderer-eigenen (§3); jeder gebaute Frame muss ausgeführt werden; ein Push bricht die laufende CI ab.
4. Danach: Material mit Welt-UVs (Teil-A-§8), Schatten und GI (Schritt 4), Messungen auf NN-WS03.
