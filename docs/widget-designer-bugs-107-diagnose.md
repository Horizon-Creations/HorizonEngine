# Widget-Designer: vier Befunde, Diagnose (Thema 107, Schritt 1)

Stand 29.09.2026, Basis `9c4aeeb1` (main). Nur Diagnose, **kein Fix**. Belege:
Code-Stellen unten, die echten Assets des Menschen im Projekt
`~/HorizonEngineProjects/Catania` (`UI/Startup.hasset`, `UI/Source/HE_Logo.hasset`),
zwei Repro-Tests in `tests/test_widget_designer_ui.cpp` und Bilder unter
`docs/img/widget-designer-bugs-107/`.

## Kurzfassung

| # | Befund | Ursache | Pfad | Hängt zusammen mit |
|---|--------|---------|------|--------------------|
| 1 | Karo statt Transparenz | Leinwand zeichnet das Content-Browser-Tile, in das das Karo **eingebacken** ist (Alpha auf 255 gezwungen) | Editor, Designer-Leinwand | **4** (gleiche Ursache) |
| 4 | Vorschau niedrig aufgelöst | dasselbe Tile: 128 × 128 px, aus 1024 × 1024 herunterskaliert | Editor, Designer-Leinwand | **1** |
| 2 | Logo orange (Designer) vs. rot (Spiel) | Spiel-UI-Pass tastet eine `_sRGB`-Textur ab (Hardware dekodiert nach linear) und schreibt in ein **Unorm**-Ziel, ohne zurückzukodieren. **Falsch ist das Spiel**, der Designer zeigt die richtigen Farben | Laufzeit-Renderer (Metal **und** GL) | unabhängig |
| 3 | Opacity-Animation poppt | keine fehlende Interpolation: der zweite Key des Menschen liegt bei **t = 0,0503 s** in einem 1-s-Clip, und ein Clip endet an seinem letzten Key → Blende nach 3 Frames fertig | Autorendaten + Timeline-UX (Editor), Laufzeit verhält sich gleich | unabhängig |

Also: **1 + 4 haben eine Wurzel**, **2** und **3** sind voneinander und von 1/4 unabhängig.
Die Nähe von 2 zum „Spiegel-Bug“ aus Thema 92 ist nur thematisch. Jener war die
Zeilenreihenfolge (Kopfstand), das hier ist der Farbraum.

---

## (1) + (4) Die Leinwand malt das Content-Browser-Tile

**Wo.** `src/HE_Editor/UIEditorPanel.cpp:4141-4142` (`drawElementIn`) holt das Bild eines
Elements über `AssetThumbnailCache::get(contentRoot + "/" + n.texture)`. Das ist derselbe
Cache, der die Kacheln im Content Browser füllt. Genutzt für die Seite selbst (`:4507`) und für
eingebettete Widgets (`:4302`). Gezeichnet wird mit `dl->AddImage(..., uv 0..1, tint)` in
`drawElementPreview` (`:3489`, `:3493`).

**Was das Tile ist.** `src/HE_Editor/AssetThumbnailCache.cpp:201-265` (`makeTextureThumbnail`):
- `kThumbSize = 128` (`:35`): das 1024er-Logo kommt mit 1/8 der Kantenlänge an → **Befund 4**.
  Box-Filter mit höchstens 4 × 4 Abtastungen pro Zielpixel.
- `:254-261`: jedes Pixel wird **über ein Karo komponiert** (`bg = 90 / 130`, 16-px-Felder),
  danach `dst[3] = 255` → **Befund 1**. Die Transparenz ist beim Erzeugen des Tiles schon
  „verbraucht“, die Leinwand kann nicht mehr durchscheinen.
- Letterbox (`:212-218`): ein nicht quadratisches Bild liegt mittig in einem quadratischen Tile
  mit leeren Rändern (Alpha 0). Da `drawElementPreview` das **ganze** Tile mit UV 0..1 auf das
  Element legt (`UIEditorPanel.cpp:3493`), wird ein nicht quadratisches Bild im Designer
  zusätzlich **gestaucht und mit Rand** gezeigt. Nebenbefund, gleiche Ursache.

**Ist das Karo Absicht?** Ja, **für die Kachel im Content Browser**: Kommentar
`AssetThumbnailCache.h:78-82` („otherwise a transparent texture reads as half-missing on the
dark tile“). Dort ist es richtig. Der Bug ist, dass die Designer-Leinwand sich **dieses Tile
ausleiht**, statt die Textur selbst zu zeigen. Der Textur-Viewer aus Thema 92 macht es richtig:
Karo nur *hinter* dem Bild, volle Auflösung (`TextureViewerPanel.cpp`, `toDisplayRgba`).

**Reproduziert.**
- Test `repro 107: designer canvas draws the thumbnail tile, checker and all` (standardmäßig
  `skip`, weil er den Bug festnagelt). Echter `UIEditorPanel::render` mit einem Stub-Renderer,
  dessen `CreateImGuiTexture` in den Software-Rasterizer lädt. Der Cache lädt also *wirklich* hoch,
  und die Leinwand zeigt genau sein Bild. Prüft: hochgeladen wurde 128 × 128, nichts ≥ 1024. Die
  Karo-Grautöne 90/130 tauchen auf der Leinwand auf und fehlen in der Negativkontrolle (gleicher
  Designer ohne Renderer).
  `HE_UI_DUMP_DIR=/tmp/ui HE_REPRO107_LOGO=~/HorizonEngineProjects/Catania/Content/UI/Source/HE_Logo.hasset ./build/tests/he_tests -tc="repro 107*" --no-skip`
  Ergebnis am 29.09. (Debug, macOS, selbst gelesen): Upload **128x128** (einziger), Karo-Grau
  90/130 auf **12533/12748** Pixeln, in der Kontrolle **0/0**. 10/10 Assertions grün, ebenso mit
  dem generierten 1024er-Bild ohne `HE_REPRO107_LOGO`.
- Bilder (echter `UIEditorPanel::render`, headless):
  `designer-canvas-catania-logo.png` (ganzes Panel, echtes Catania-Logo),
  `designer-canvas-catania-logo-zoom.png` (Ausschnitt 3x: Karo, Treppenkanten des 128er-Tiles),
  `designer-canvas-generiertes-bild.png`, `designer-canvas-kontrolle-ohne-textur.png`.
- `logo-designer-vs-soll-vs-spiel.png` (links) und `logo-aufloesung-ausschnitt.png`: CPU-Nachbildung
  aus den echten Logo-Bytes. Das Tile wurde exakt nach `makeTextureThumbnail` nachgebaut und auf
  550 px (Elementgröße in Catania) hochskaliert.

**Hinweis für den Fix (nicht umgesetzt).** Die Leinwand braucht eine Textur in voller Auflösung
mit echtem Alpha, getrennt vom Tile-Cache. Das Tile muss für den Content Browser bleiben, wie es ist.
Ein Karo, falls überhaupt gewünscht, gehört höchstens *hinter* die Leinwandfläche (wie im
Textur-Viewer), nicht ins Bild. Vorsicht Speicher: eine 4K-Textur pro Bildelement als ImGui-Textur.

## (2) Farben: Designer orange, Spiel rot

**Asset.** `Catania/Content/UI/Source/HE_Logo.hasset`, TXMI: 1024 × 1024, RGBA8, 1 Mip,
**`srgb = 1`**. (Die Kopie in `CollabTest/Content/UI/Images/HE_Logo.hasset` hat `srgb = 0` und
zeigt den Fehler deshalb nicht.) Der Import-Dialog (`TextureColourSpaceDialog`) schlägt für Bilder
per Dateiname sRGB vor (`ImporterCommon.cpp:190` `suggestTextureSrgb`, `:727`). Jedes seit dem Dialog
importierte UI-Bild ist also betroffen, ältere nicht. Deshalb wirkt der Fehler neu.

**Spiel-Pfad (falsch).**
- Metal: `EncodeUIPass` bindet für ein Bild-Quad `ResolveGraphTexture(obj.textureAssetId)`
  (`MetalRenderer.mm` ≈ `:12443-12449`) → `uploadMetalTexture` → `metalTexPixelFormat` wählt bei
  `tex->srgb` **`MTLPixelFormatRGBA8Unorm_sRGB`** (`:8482-8490`). Der Sampler dekodiert dadurch nach
  linear. `uiFragment` Modus 2 (`:1344-1352`) gibt `color.rgb * t.rgb` unverändert aus. Die UI-Pipeline
  schreibt nach `kSwapchainFormat = BGRA8Unorm` (`:176`, `:6586`), also **ohne** sRGB-Kodierung beim Schreiben.
  Ergebnis: linear dekodierte Werte landen als wären sie sRGB → Mitteltöne viel dunkler und satter.
- OpenGL: gleich. `ResolveGraphTexture` (`OpenGLRenderer.cpp:7215-7217`) → `GL_SRGB8_ALPHA8`
  (`:7684`), während des UI-Passes ist `GL_FRAMEBUFFER_SRGB` aus (`:11345`).
- Einfarbige UI-Flächen, Text und Farbverläufe sind **nicht** betroffen: ihre Farben gehen roh als
  sRGB-Zahlen durch, genau wie im Designer. Nur **texturierte** Quads mit `srgb = 1`.
- D3D11/D3D12/Vulkan zeichnen UI-Bild-Quads laut Thema 92 gar nicht texturiert (nur Vollfarbe), dort
  zeigt sich der Fehler deshalb nicht.

**Designer-Pfad (richtig).** Das Tile wird aus den rohen Bytes gebaut (`makeTextureThumbnail`
ignoriert `srgb`) und per `CreateImGuiTexture` als **`RGBA8Unorm`** hochgeladen
(`MetalRenderer.mm:16782`). ImGui schreibt roh ins Unorm-Ziel. Bytes rein = Bytes raus.

**Zahlen (echte Logo-Pixel).** Orange (216,128,24) → im Spiel (175,55,2), Gelb (240,176,40) →
(222,111,5), Creme (253,242,215) → (250,226,173). Genau „orange wird rot“.
Bild: `logo-designer-vs-soll-vs-spiel.png`, rechts. CPU-Nachbildung der Dekodierung aus den echten
Bytes, **kein** GPU-Screenshot (headless gibt es keinen Metal-UI-Pass).

**Hinweis für den Fix (nicht umgesetzt).** `ResolveGraphTexture` / `m_graphTexCache` wird **auch vom
Material-Graphen** benutzt. Dort ist die lineare Dekodierung einer sRGB-Farbtextur **richtig**
(Beleuchtung rechnet linear, der Tonemapper kodiert am Ende). Man darf deshalb nicht global das
Upload-Format umstellen. Möglichkeiten: im UI-Pass eine Unorm-Sicht der Textur benutzen (Metal
`newTextureViewWithPixelFormat:` braucht `MTLTextureUsagePixelFormatView`; GL `glTextureView` erst
ab 4.3, macOS-GL ist 4.1, also dort eher `GL_TEXTURE_SRGB_DECODE_EXT`/Sampler-Parameter), oder
im `uiFragment` Modus 2 zurückkodieren (billig, aber doppelte Rundung in 8 Bit), oder ein eigener
UI-Upload ohne sRGB-Flag. UI-Materialien (Domain UI) prüfen: die tasten ebenfalls Graph-Texturen ab.

## (3) Render-Opacity-Animation poppt

**Daten des Menschen.** `Catania/Content/UI/Startup.hasset`, Clip „Blend“: `duration = 1.0`, eine
Spur `Render Opacity` an Element 1 (dem Logo), Keys **(0,0 s → 0,0)** und
**(0,050314 s → 1,0)**. Der Graph spielt ihn beim `Construct` vorwärts, nach `Delay 4 s`
rückwärts (`widget.playAnimation`, `direction Forward/Backward`).

**Auswertung ist in Ordnung.** `uiAnimEvaluate` (`UIWidgetAnim.cpp:153-188`) interpoliert Floats
linear mit Easing. Die Designer-Vorschau (`ScrubPreview`, `UIEditorPanel.cpp:4317-4350`) und die
Laufzeit (`WidgetManager.cpp:2378-2396`) benutzen denselben Auswerter. `uiAnimPlayEnd`
(`UIWidgetAnim.cpp:112-123`) beendet einen Clip an seinem **letzten Key**, nicht an `duration`. Das ist
gewollt und festgenagelt (`tests/test_ui_widgets.cpp:6950`, „the runtime ends one at its last key“).

**Folge.** Die Blende dauert 50 ms = 3 Frames bei 60 Hz, vorwärts wie rückwärts. Sie sieht aus wie
Rein-/Rausspringen, im Designer genauso wie im Spiel.

**Reproduziert.** Test `repro 107: the user's Render Opacity clip is over in three frames`
(läuft normal mit, er prüft Fakten und keinen Bug): gleiche Keys, echter `WidgetManager`,
60-Hz-Ticks. Gemessen am 29.09.:
- Key bei 0,0503 s: `f1=0.33 f2=0.66 f3=0.99 f4=1.00 … f60=1.00`
- Key bei 1,0 s: `f1=0.02 f4=0.07 f10=0.17 f30=0.50 f60=1.00`

**Wie kam der Key auf 0,05 s?** Aus den Daten nicht beweisbar. Zwei Kandidaten im Code:
- Das „Time“-Feld im Key-Editor zieht mit **5 ms pro Pixel** (`UIEditorPanel.cpp:2615`,
  `DragFloat("Time", &key.time, 0.005f, ...)`): 10 px Ziehen = 0,05 s. Wer „ans Ende ziehen“ will,
  bräuchte 200 px.
- Key-Ziehen in der Spur oder Scrubben im Lineal setzt `view.tOf(mouse.x)` (`:2976`, `:3032`), mit
  Schwelle `IsMouseDragging` (ein paar Pixel). Ein kleines Ziehen am ersten Key macht daraus einen Key
  knapp hinter 0.

Dazu kommt ein Darstellungsproblem: Die Timeline zeigt die volle `duration` (1 s). Dass der Clip
schon bei 0,05 s endet, sieht man nicht, obwohl die Laufzeit dort aufhört.
Nebenbefund (nicht die Ursache hier): Das „Value“-Feld für Float-Keys zieht mit 0,5 pro Pixel und
ohne Grenzen (`:2634`). Für Render Opacity (0..1) springt ein Key damit leicht auf −7 oder 12, und
`setPropAny` klemmt (`UIElement.cpp:1028`). Das würde ebenfalls wie ein Pop aussehen.

**Für den Fix-Schritt zu entscheiden (Mensch).** Soll ein Clip bis `duration` laufen
(dann änderte sich festgenagelte Semantik), oder soll die Timeline das Clip-Ende am letzten Key
sichtbar machen und das Setzen eines End-Keys erleichtern (z. B. „Key am Ende“, Time-Feld mit
feinerer Zieh-Geschwindigkeit relativ zur Clip-Länge, Value-Feld mit den Grenzen der Eigenschaft)?

## Unabhängigkeit

- 1 und 4: **eine** Ursache (Leinwand ← `AssetThumbnailCache`). Ein Fix trifft beide, dazu den
  Letterbox-Nebenbefund.
- 2: unabhängig, liegt im **Laufzeit**-Renderer. Ein Fix von 1/4 ändert daran nichts. Umgekehrt: Würde
  der Designer künftig eine GPU-Textur mit sRGB-Format zeigen, müsste er ebenfalls in einer Unorm-Sicht
  abtasten, sonst kippt er auf dieselbe Seite wie das Spiel.
- 3: unabhängig, Autorendaten plus Timeline-UX.
