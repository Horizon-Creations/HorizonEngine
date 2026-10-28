# Editor Slice 1b

Modul Editor (Kürzel EDT), Slice 1b: 16 Dateien aus `docs/audit/_slices/Editor_1b.txt` (AudioMixView, AudioWaveformView, BlendSpacePanel, BoneMaskPanel, BuildProgressDialog, CameraBookmarks, CinematicPanel, CinematicPreview, jeweils .cpp/.h).

## Zusammenfassung

Der Code ist durchgehend sauber kommentiert und die Datenmodelle sind gut getrennt (AudioWave/AudioMix ohne AppContext, Peak-Pyramide, Bracket-Muster im Cinematic-Preview). Die Funde liegen vor allem in per-Frame-Arbeit ohne Änderungserkennung (Cinematic-Preview, EQ-Kurve, Bone-Mask-Baum, Build-Log), in dupliziertem Panel-Boilerplate (BlendSpace/BoneMask/Cinematic) und in einer sehr langen Funktion (`AudioWave::draw`, ca. 540 Zeilen). Ein echter Lesefehler auf einen ungültigen Zeiger steckt in `BoneMaskPanel::jointRow`. Anzahl Funde: 1 Hoch, 7 Mittel, 6 Niedrig. `CameraBookmarks.*` ist unauffällig (kleine, geschlossene Datenklasse, Encode/Decode robust). Nichts davon wurde gemessen; Schweregrade sind Schätzungen aus dem Code.

---

### EDT-2001 – Cinematic-Preview rendert und schreibt/restauriert die Welt jeden Frame, auch im Stillstand
- **ID**: EDT-2001
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/CinematicPanel.cpp:704-773` (drawPreview), `src/HE_Editor/CinematicPreview.cpp:130-242` (apply/restore)
- **Problem**: Solange der Cinematic-Tab vorne ist, läuft pro Frame: `Bracket::apply` (resolveBindings, evaluate, Save aller gebundenen Akteure inkl. Kopie von `boneMatrices`, Pose-Berechnung aller Skeletal-Sektionen), `RenderWorldPreview` (laut Header ein synchroner Round-Trip mit Extraktion der ganzen Welt), `Bracket::restore` (schreibt alles zurück, markiert Transform/Skelett/Material dirty und ruft `HE::propagateTransforms(world)` auf). Das geschieht auch, wenn nichts abgespielt wird, der Playhead stillsteht, nichts bearbeitet wird und die Fenstergröße gleich bleibt. Durch die Dirty-Markierungen in `restore` muss auch der Renderer Transform-/Skelett-/Material-Caches jedes Frame neu hochladen.
- **Code-Auszug**:
```cpp
s_preview.apply(*ctx.world, *ctx.contentManager, seq, st.view.playhead);
...
tex = ctx.renderer->RenderWorldPreview(*ctx.contentManager, *ctx.world, w, h, ov, ...);
s_preview.restore(*ctx.world, *ctx.contentManager);
// restore(): ... tc->dirty = true; smc->dirty = true; mc.dirty = true; ... HE::propagateTransforms(world);
```
- **Lösung**: Das Bild nur neu erzeugen, wenn sich eine Eingabe geändert hat: Playhead, Sequenz-Änderungszähler (`commit`/`st.dirty`-Edits/Undo/Restore erhöhen einen `uint32_t revision` im PanelState), Zielgröße, `throughCamera`, Editor-Kamera-Pose, Welt-Strukturepoche (`world.structureEpoch()` gibt es bereits). Sonst die zuletzt erhaltene Textur weiterzeichnen. Voraussetzung: Die Textur von `RenderWorldPreview` (Slot 0) bleibt gültig, bis jemand anderes Slot 0 benutzt; falls andere Panels Slot 0 überschreiben, den Cache dann verwerfen (Slot-Besitzer-Token oder „Frame-Zähler des letzten fremden Aufrufs“). Zusätzlich in `restore` die Re-Propagation nur ausführen, wenn mindestens ein `hasTransform` gesichert wurde.
- **Erwarteter Effekt**: Im Stillstand fast die gesamte Preview-Kosten weg (Schätzung: eine ganze Szenen-Extraktion + Render pro Frame; messen mit `HE_PROFILE_SCOPE`). Bei laufender Wiedergabe unverändert.
- **Risiko / Verifikation**: Hoch genug, um Tests zu brauchen. Gefahr: veraltetes Bild, wenn sich Welt oder Material von außen ändert (Kollaboration, anderer Tab). Kein bestehender UI-Test für den Preview-Pfad bekannt (`tests/test_*cinematic*` prüfen); Test: Bracket-Roundtrip unverändert lassen, neuen Test „zweiter Aufruf bei gleicher Revision ruft RenderWorldPreview nicht erneut auf“ mit Mock-Renderer.

### EDT-2002 – Build-Dialog: Mutex wird beim Zeichnen des Logs gehalten, Logzeilen pro Frame über alle Schritte gescannt
- **ID**: EDT-2002
- **Kategorie**: E (und D)
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/BuildProgressDialog.cpp:830-877`, Kommentar dazu `:700-702`
- **Problem**: Der Kommentar sagt, das Modell werde unter dem Lock kopiert und sofort freigegeben, „holding a worker's mutex across [ImGui calls] is how a UI ends up waiting on a compiler“. Genau das passiert aber im Log-Child: `std::lock_guard<std::mutex> lk(s_mutex)` bleibt über `drawLine`, den ListClipper und `SetClipboardText` bestehen. Der Export-Worker (`Build::log`, `stepProgress`, `setActivity`) blockiert so für die Dauer des Zeichnens. Außerdem wird jeden Frame `s_log` komplett durchlaufen, um die Zeiger der gewählten Schritte zu sammeln (`lines`), obwohl der Pack-Schritt laut Kommentar „thousands of lines“ erzeugt und auch Zeilen aller anderen Schritte mitgescannt werden.
- **Code-Auszug**:
```cpp
std::lock_guard<std::mutex> lk(s_mutex);
...
std::vector<const LogLine*> lines;
for (const LogLine& l : s_log)
    if (l.step == s_selected) lines.push_back(&l);
```
- **Lösung**: Pro Schritt einen eigenen Logvektor führen (`std::vector<std::vector<LogLine>>` oder `Step::log`), dann entfällt der Scan. Beim Zeichnen nur kurz sperren: Anzahl lesen, `ListClipper` ohne Lock laufen lassen und pro sichtbarer Zeile (≈40) unter Lock kopieren, oder `std::deque<LogLine>` (stabile Referenzen bei `push_back`) verwenden und nur beim Größenabruf sperren. Kopieren für „Copy“ unter Lock in einen String, dann Lock lösen und erst danach `SetClipboardText`.
- **Erwarteter Effekt**: Kein UI-bedingtes Stocken des Export-Workers mehr; pro Frame O(sichtbare Zeilen) statt O(Gesamtlog). Schätzung; bei 10k Zeilen ein Scan von Zehntausenden Struct-Zugriffen pro Frame.
- **Risiko / Verifikation**: Gering bis mittel (Thread-Sicherheit). `snapshot()` und `McpToolsBuild` hängen am Log-Typ; bestehende Tests für das Modell (suchen nach `BuildProgressDialog` in `tests/`) müssen weiter grün sein.

### EDT-2003 – Asset-Tab-Panels: ca. 80 Zeilen Boilerplate pro Panel dreifach kopiert, Cinematic fehlt bei den Crash-Snapshots
- **ID**: EDT-2003
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `BlendSpacePanel.cpp:50-68, 230-307, 309-331`; `BoneMaskPanel.cpp:44-62, 237-277, 279-301`; `CinematicPanel.cpp:83-96, 777-828, 830-855`; Header `BlendSpacePanel.h`, `BoneMaskPanel.h`, `CinematicPanel.h`
- **Problem**: Die Funktionen `saveState`, `isXxxAsset`, `isDirty`, `reloadFromDisk`, `appendDirtyPaths`, `save`, `forget`, `stateByContentPath`, `isDirtyByContentPath`, `reloadByContentPath`, `appendSnapshots`, der Lade-Prolog in `render` (`toContentRelativePath`, `loadAsset`, Name aus Stem) und der identische `ImGui::Begin(("##x_" + assetPath)…)`-Flaggenblock sind in drei Dateien Zeile für Zeile gleich (die Kommentare sagen selbst „the same shape InputAssetPanel's and TypeAssetPanel's have“, also sind es mindestens fünf Kopien). Dabei ist schon eine Drift entstanden: `CinematicPanel` hat kein `appendSnapshots` (nicht im Header, nicht in `EditorUI.cpp:1063-1077` aufgerufen), ein ungespeicherter Cinematic-Tab ist also nicht in den Crash-Recovery-Kopien. Das ist vermutlich keine Absicht, sondern Vergessen (unsicher, bitte mit dem Panel-Autor bestätigen).
- **Code-Auszug**:
```cpp
bool BlendSpacePanel::reloadByContentPath(const std::string& contentPath)
{
    PanelState* st = stateByContentPath(contentPath);
    if (!st) return false;
    st->loaded = false;
    st->dirty  = false;
    return true;
}
```
- **Lösung**: Ein Template in `EditorPanelState.h` (neben `AssetPanelState`), z. B. `AssetTabOps<PanelState, Traits>`, mit Traits `assetType`, `getMutable(cm, id)`, `toJson(state)`, `fromJson`. Daraus entstehen die zehn Standardfunktionen einmal. Die Panel-Namespaces rufen nur noch `return Ops::isDirty(path);` o. Ä. oder, einfacher, die Panels exportieren ein `constexpr`-Interface-Objekt, das `EditorUI` in einer Tabelle statt in mehreren handgepflegten Aufruflisten (`EditorUI.cpp:992-1211`, `4161`) durchläuft. Zunächst klein anfangen: nur `stateByContentPath`/`reloadByContentPath`/`isDirtyByContentPath` als Member von `AssetPanelState` (`findByContentPath(relPath)` setzt voraus, dass `PanelState::relPath` in einem Basis-Struct liegt).
- **Erwarteter Effekt**: Ca. 150-250 Zeilen weniger, eine Stelle für künftige Änderungen (z. B. neue Pflichtfunktion wie `appendSnapshots`); keine Laufzeitwirkung.
- **Risiko / Verifikation**: Niedrig bis mittel; rein mechanisch, aber viele Aufrufer. Absichern über bestehende Dirty-/Reload-Tests (Suche `tests/` nach `isDirtyByContentPath`, `reloadFromDisk`); Build von `HorizonEditor` und `he_tests`.

### EDT-2004 – EQ-Graph berechnet die komplette Frequenzgangkurve jeden Frame neu (komplexe Arithmetik, 3 Heap-Vektoren)
- **ID**: EDT-2004
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/AudioMixView.cpp:195-233` (drawGraph), `src/HE_Core/src/Audio/AudioEdit.cpp:307-316` (magnitudeDb, außerhalb des Slice, nur zur Belegung)
- **Problem**: Pro Frame und Pixelspalte wird `sumAt(f)` über alle Bänder berechnet, und `magnitudeDb` nutzt `std::polar`, komplexe Division, `std::abs` und `log10`. Bei 600 Spalten × bis zu `kMaxBands` Bändern sind das einige Tausend solcher Aufrufe pro Frame, plus die zweite Kurve für das gewählte Band (`own`). `freqOfX` ruft pro Spalte `std::pow` (zweimal pro Spalte, wenn ein Band gewählt ist). Dazu drei Heap-Allokationen pro Frame (`coeffs`, `line`, `own`). Der Graph ändert sich nur, wenn EQ, Samplerate, Breite oder Auswahl sich ändern.
- **Code-Auszug**:
```cpp
for (int i = 0; i <= cols; ++i)
{
    const double f = freqOfX(float(i), w);
    line.emplace_back(o.x + float(i), o.y + yOfDb(sumAt(f), h));
}
```
- **Lösung**: (1) Frequenztabelle `std::vector<double> freq` pro Breite einmal berechnen (Member in `EqView`, gebaut wenn `cols` wechselt). (2) Die Kurven (`line` und `own` Y-Werte in dB) in `EqView` zwischenspeichern, mit einem Schlüssel aus (eq-Bänder, enabled, rate, cols, selBand). Schlüssel billig: ein FNV-Hash über `sizeof` der Bänder-POD-Felder oder ein vom Aufrufer gelieferter Änderungszähler. (3) Die Vektoren als Member wiederverwenden, nicht neu allokieren. Alternativ ohne Cache: `magnitudeDb` mit reellem Ausdruck (cos/sin von ω und 2ω einmal pro Frequenz für alle Bänder teilen).
- **Erwarteter Effekt**: Schätzung 0,3-1 ms pro Frame weniger solange das Audio-Editor-/Mixer-Fenster sichtbar ist; messen.
- **Risiko / Verifikation**: Gering; die Anzeige muss pixelgleich bleiben. Hinweis: der Header sagt, `AudioMix` sei von den Tests angesteuert (`graphMin/graphSize`, `xOfFreq`); die vorhandenen Tests (`tests/` nach `AudioMix`) decken die Mapping-Funktionen, ein Test „Kurve bleibt gleich nach Cache-Treffer“ ist zu ergänzen.

### EDT-2005 – `AudioWave::draw` ist eine einzige Funktion mit ca. 540 Zeilen
- **ID**: EDT-2005
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/AudioWaveformView.cpp:471-1011`
- **Problem**: Eingabe (Wheel, Presses, Drags, Cursor), Layout, Lineal, Selektion, Kanalspuren, Trim, Kurve, Overview und Cursorrückgabe stehen in einer Funktion mit Lambdas (`frameUnderPointer`, `overviewFrameAt`, `xAt`, `yOfGain`, `ox`) und rund 35 lokalen Variablen. Die Fallunterscheidung der Presses (`rulerActivated` / `lanesPressed && curveOn` / `lanesPressed` / `overActivated`) und der Drag-`switch` hängen über Zustände in `View` zusammen, die beim Lesen quer über 300 Zeilen verfolgt werden müssen. Die zwei Tick-Schleifen (Zeilen 770-777 und 788-797) wiederholen denselben Linien-/Label-Block.
- **Code-Auszug**:
```cpp
Result draw(const Clip& clip, const Peaks& peaks, View& view, const ImVec2& size, bool trackpad,
            HE::AudioEnvelope* curve, bool curveEditable)
{ ... // 540 Zeilen
```
- **Lösung**: Aufteilen in `struct Layout` (origin, width, height, rulerBottom, overTop, lanesBottom, lanesH, rate, curveOn) und private Funktionen in einem `namespace`: `handleWheel`, `handlePresses`, `handleDrags`, `drawRuler` (mit einem Lambda `tick(x, label)` für beide Modi), `drawLanes`, `drawTrim`, `drawCurve`, `drawOverview`. `draw` bleibt die Reihenfolge-Steuerung (≈40 Zeilen). Reine Umordnung, keine Verhaltensänderung.
- **Erwarteter Effekt**: Lesbarkeit und Testbarkeit (z. B. `handlePresses` ließe sich mit synthetischem `ImGuiIO` einzeln treiben); keine Laufzeitänderung.
- **Risiko / Verifikation**: Gering; die vorhandenen headless Draw-Tests (Audio-Editor-UI-Tests, `tests/test_*audio*`) sind das Sicherheitsnetz; zusätzlich BMP-Frames vor/nach vergleichen (`HE_UI_DUMP_DIR`).

### EDT-2006 – Bone-Mask-Baum: O(J²) Kindersuche, `vector`-Allokation je Knoten und Linearsuchen je Frame, kein Row-Clipping
- **ID**: EDT-2006
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/BoneMaskPanel.cpp:66-71, 144-195, 201-233, 340-341`
- **Problem**: (a) `jointTree` durchsucht für jeden Knoten das gesamte Skelett nach Kindern und legt dafür pro Knoten und Frame einen `std::vector<int>` an, das ist O(J²) und J Heap-Allokationen pro Frame. (b) `jointRow` ruft für jeden gezeichneten Joint `entryFor` (lineare String-Vergleiche über alle Einträge), also O(J·E). (c) `strayNames` vergleicht für jeden Eintrag Strings mit allen Gelenken (O(E·J)) und baut jeden Frame einen `vector<string>` mit Kopien. (d) Alle Knoten sind `DefaultOpen`, also werden bei einem Rig mit 150-300 Knochen jeden Frame alle Zeilen mit Checkbox + Text + Slider gebaut, ohne Clipper; CLAUDE.md Abschnitt 4 verlangt für zeilenlastige Panels Row-Clipping.
- **Code-Auszug**:
```cpp
std::vector<int> children;
for (size_t i = 0; i < mesh.skeleton.size(); ++i)
    if (mesh.skeleton[i].parent == index) children.push_back(static_cast<int>(i));
```
- **Lösung**: Pro Referenz-Mesh einmal `children` als CSR-Struktur (`std::vector<int> firstChild/next`, oder `vector<vector<int>>`) in `PanelState` aufbauen, Schlüssel `referenceMeshId` und `skeleton.size()`. Pro Frame (oder nur bei `dirty`/Mesh-Wechsel) ein `std::vector<int8_t>` / `std::vector<float*>` `entryOfJoint[J]` über eine einmal erzeugte `unordered_map<string,int>` (Name → Gelenk) füllen; `strayNames` leitet sich aus demselben Durchlauf ab (Einträge, die auf kein Gelenk abgebildet wurden). Für Clipping die Baumdarstellung in eine flache Liste „sichtbare Zeilen mit Tiefe“ (je Frame aus dem Aufklapp-Zustand) überführen und per `ImGuiListClipper` zeichnen; dafür braucht `TreeNode` einen eigenen Aufklapp-Zustand (z. B. `std::vector<bool> collapsed`).
- **Erwarteter Effekt**: Von O(J²) und ~J Allokationen pro Frame auf O(J) bzw. O(sichtbare Zeilen). Bei J≈300 vermutlich dominiert der ImGui-Widgetaufbau (≈900 Widgets); gemessen werden sollte vorher mit einem großen Rig (Schätzung: 0,5-2 ms/Frame).
- **Risiko / Verifikation**: Mittel für den Clipper-Teil (Tree-Verhalten ändert sich); Teil (a)-(c) risikoarm. Test mit `tests/test_*bone_mask*` (suchen), und mit synthetischem Skelett 500 Gelenke.

### EDT-2007 – Bone-Mask: nach `eraseEntry` wird `e->weight` aus einem ungültigen Zeiger gelesen
- **ID**: EDT-2007
- **Kategorie**: C (Dangling)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (Korrektur)
- **Ort**: `src/HE_Editor/BoneMaskPanel.cpp:110-136`
- **Problem**: `e` wird vor der Checkbox per `entryFor` gesucht. Schaltet die Checkbox auf „aus“, ruft der Code `eraseEntry(st.mask, j.name)` auf, das den Vektor verschiebt, `e` zeigt aber weiter auf den (jetzt verschobenen oder zerstörten) Slot. Direkt danach prüft `if (e)` auf einen nicht-null Zeiger und liest `float w = e->weight`. Der Kommentar in Zeile 131-133 sagt zwar, man suche den Eintrag neu, weil „a pointer into a vector that just grew is the oldest bug in this codebase“, tut das aber erst beim Schreiben (Zeile 134), nicht beim Lesen in Zeile 128. Praktisch zeigt der Slider für ein Frame ein Gewicht eines anderen Gelenks oder zeigt für ein frisch abgewähltes Gelenk noch einen Slider; formal ist es undefiniertes Verhalten.
- **Code-Auszug**:
```cpp
if (EditorWidgets::checkbox("Joint", &on))
{
    if (on) setEntry(st.mask, j.name, 1.0f);
    else    eraseEntry(st.mask, j.name);
    st.dirty = true;
}
...
if (e) { ... float w = e->weight; ... }
```
- **Lösung**: Nach dem Umschalten `e = on ? entryFor(st.mask, j.name) : nullptr;` setzen (eine Zeile), danach entfällt auch die erneute Suche in Zeile 134 (`e->weight = w; st.dirty = true;`).
- **Erwarteter Effekt**: Korrektheit (kein UB, kein Flackern); weniger Code.
- **Risiko / Verifikation**: Sehr gering. Test: UI-Test, der einen Joint abwählt und prüft, dass im selben Frame kein Slider gezeichnet wird (`tests/test_*_ui.cpp`-Muster).

### EDT-2008 – Cinematic-Tab: wiederkehrende Per-Frame-Allokationen und -Scans (Refs, Labels, Akteurzeile)
- **ID**: EDT-2008
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/CinematicPanel.cpp:204-220, 407-427, 437-440, 896-909`
- **Problem**: Jeden Frame: (a) `ensureRefsResident` baut einen neuen `std::vector<HE::UUID> refs` per `sequenceAssetRefs` über alle Tracks, macht je Ref zwei Hash-Lookups und eine Linearsuche in `triedRefs`; die Sequenz ändert sich aber nur bei einem Commit/Undo/Reload. (b) `drawActors` baut je Akteur drei temporäre Strings (`shown`, Label mit `std::to_string` + Konkatenation, `CalcTextSize`), `entityOf` wird dabei pro Akteur aufgerufen; dieselbe Auflösung (+ `all_of<CameraComponent>`) passiert dann nochmals für `Labels` (Zeilen 897-902) und `Add at Playhead`/`cameraCombo` (Zeilen 363-368, 506). (c) `fresh` wird pro Frame neu gefüllt mit `std::any_of` über alle Bindings (O(S·B)). (d) `Labels` baut `missing`/`camera`-`vector<bool>` ohne `reserve`.
- **Code-Auszug**:
```cpp
std::vector<HE::UUID> refs;
HE::sequenceAssetRefs(*seq, refs);
for (const HE::UUID& id : refs)
{
    if (ctx.contentManager->getAnimationClip(id) || ctx.contentManager->getAudio(id)) continue;
    if (std::find(st.triedRefs.begin(), st.triedRefs.end(), id) != st.triedRefs.end()) continue;
```
- **Lösung**: `refs` als Member der PanelState mit `refsRevision`; neu berechnen nur nach `commit`/`restoreSnapshot`/Load (ein Zähler `st.revision`, derselbe wie in EDT-2001 vorgeschlagen). Die Auflösung `Binding → entity/isCamera` einmal pro Frame in einen `struct BindingInfo { entt::entity e; bool camera; }`-Vektor (Member, `clear()` + `push_back`) legen und in drawActors/Labels/Combo wiederverwenden (ein Helfer `isCameraBinding` entfernt auch die dreifach duplizierte Zeile). Label-Strings nur bei Namensänderung bauen oder `ImGui::PushID(b.slot)` statt `##cin_actor<slot>`-Konkatenation nutzen.
- **Erwarteter Effekt**: Einige Dutzend kleine Allokationen und einige Hundert Lookups pro Frame weniger; vermutlich < 0,1 ms; echter Gewinn ist eher Klarheit. Bei sehr vielen Akteuren (Zeile läuft nicht um, siehe Unsicher) wächst es linear.
- **Risiko / Verifikation**: Gering; `triedRefs` muss bei Reload zurückgesetzt bleiben (Zeile 842).

### EDT-2009 – Volumenkurve: pro Pixelspalte vier transzendente Aufrufe und neuer Vektor je Frame
- **ID**: EDT-2009
- **Kategorie**: D / A
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/AudioWaveformView.cpp:237-259, 880-910`
- **Problem**: `curveFracOfGain` berechnet bei jedem Aufruf `std::pow(10.0, kCurveFloorDb/20.0)` (Konstante) und ruft `curveTopDb()` zweimal (`20*log10(kMaxGain)`, ebenfalls Konstante), dazu `log10(gain)`. Dieselbe Funktion wird pro Pixelspalte der Kurve (Zeile 908 über `yOfGain`) und für jeden Punkt in `curvePointAt` aufgerufen, also bei ca. 1500 Spalten ≈ 6000 `pow/log10` pro Frame. Das ist genau das „constexpr statt Laufzeitberechnung“-Muster; `kMaxGain` ist `static constexpr float = 4.0f` (`AudioEdit.h:100`). Außerdem wird `std::vector<ImVec2> line` jeden Frame neu allokiert und `evalGain` (Binärsuche) pro Spalte aufgerufen, auch wenn die Kurve nur als ruhige Linie (`!curveOn`) dasteht.
- **Code-Auszug**:
```cpp
double curveTopDb() { return 20.0 * std::log10(double(HE::AudioEnvelope::kMaxGain)); }
...
const double floorGain = std::pow(10.0, kCurveFloorDb / 20.0);
```
- **Lösung**: `constexpr double kCurveTopDb = 12.0411998265592478;` (mit `static_assert(std::abs(kCurveTopDb - 20*log10(4.0)) < 1e-9)` zur Absicherung falls `std::log10` nicht `constexpr` ist; GCC erlaubt es, MSVC/Clang nicht) und `constexpr double kCurveFloorGain = 0.00398107170553497;` (10^-48/20 = 10^-2.4). Den Pixelvektor als `thread_local`/Member wiederverwenden (`line.clear()`).
- **Erwarteter Effekt**: Schätzung 50-100 µs pro Frame im Curve-Modus; eher Vereinfachung als Gewinn.
- **Risiko / Verifikation**: Sehr gering; Tests für `curveFracOfGain` und `curveGainOfFrac` Rundlauf existieren vermutlich (`tests/` nach `curveFracOfGain`).

### EDT-2010 – Waveform/Overview: Spaltenextrema pro Frame neu berechnet
- **ID**: EDT-2010
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/AudioWaveformView.cpp:843-860, 962-983`
- **Problem**: Für jeden Pixel und jeden Kanal ruft `draw` `columnExtent` (Spuren, Zeilen 843-860) und für die Overview ein weiteres Mal pro Spalte und Kanal (962-983), obwohl sich die Overview nur bei neuem Clip oder neuer Breite ändert. Die Pyramide begrenzt jeden Aufruf auf wenige Dutzend Bucket-Lesezugriffe (laut Header ≈ 2×16), das ist also keine Katastrophe, aber ≈ `width × channels × ~20` Reads pro Frame und `width × channels` einzelne `AddLine`-Aufrufe mit Anti-Aliasing.
- **Code-Auszug**:
```cpp
for (int c = 0; c < clip.channels; ++c)
{ int l = 0, h = 0; columnExtent(clip, peaks, f0, f1, c, l, h); ... }
```
- **Lösung**: Overview-Extrema (`std::vector<int16_t> lo, hi`) in `View` ablegen und nur bei Wechsel von (`peaks.frames`, `width`) neu bauen. Für die Spuren optional dasselbe mit Schlüssel (viewStart, framesPerPx, width). Zusätzlich messen, ob `AddRectFilled` für 1-px-Säulen billiger ist als `AddLine` mit AA-Pfad (siehe EDT-2004: dort wurde absichtlich auf Rechtecke gewechselt).
- **Erwarteter Effekt**: Unsicher; nur nach Messung (ImGui-Zeichenkosten dominieren vermutlich die Berechnung).
- **Risiko / Verifikation**: Gering; Cache-Invalidierung bei Edits, die Samples ändern (Clip-Wechsel liefert neue `Peaks`).

### EDT-2011 – Build-Dialog: doppelte Strukturen (Step/StepView, LogLine/LogView) und volle Modellkopie pro Frame
- **ID**: EDT-2011
- **Kategorie**: A / C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/BuildProgressDialog.cpp:31-45, 281-285, 578, 708-731`; `BuildProgressDialog.h:119-132`
- **Problem**: `Step`/`StepView` und `LogLine`/`LogView` sind Feld für Feld identisch. `snapshot()` und `brief()` kopieren Felder von der einen in die andere Form. `render()` kopiert pro Frame `s_steps` (mit Strings), `message`, `activity`, `exePath` und `s_hcInterpreted` (Vektor von Notices mit Strings), obwohl sich das Modell meist nur beim Wechsel des Fortschritts ändert. `brief()` baut zusätzlich pro Frame (nur im versteckten Zustand) einen `vector<StepView>` mit leeren Strings, nur um `overallProgress` zu füttern.
- **Code-Auszug**:
```cpp
steps    = s_steps;
...
hcInterpreted = s_hcInterpreted;
```
- **Lösung**: Ein einziger Typ `StepView`/`LogView` (die Zustandskonstanten `Pending…Failed` als `enum` im Header), `s_steps`/`s_log` direkt von diesem Typ. Einen `uint64_t s_version` im Modell bei jeder Schreibfunktion erhöhen; `render()` kopiert nur, wenn sich die Version seit dem letzten Frame geändert hat, und hält das Ergebnis in `static` UI-State. `overallProgress` auf `std::span<const StepView>` bzw. direkt eine Schleife unter dem Lock in `brief()`.
- **Erwarteter Effekt**: ca. 40 Zeilen weniger, Kopierkosten nur bei Änderungen (während eines Builds ändert sich das Modell mehrmals pro Sekunde, also eher gering).
- **Risiko / Verifikation**: Gering; `McpToolsBuild` konsumiert `Snapshot`, Typnamen bleiben erhalten.

### EDT-2012 – Cinematic-Undo: bis zu 64 vollständige JSON-Strings pro Tab
- **ID**: EDT-2012
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/CinematicPanel.cpp:55-56, 99-108`
- **Problem**: Jeder Commit serialisiert die gesamte Sequenz (`HE::sequenceToJson`), vergleicht sie mit dem obersten Snapshot und legt sie im Vektor ab (max. 64, `erase(begin())` verschiebt 64 Strings). Bei langen Sequenzen mit vielen Keys sind das schnell einige MB pro Tab und je Edit eine ganze Serialisierung. Außerdem wird während einer `DragFloat`-Interaktion nicht committet (nur beim Loslassen), das ist richtig; die Kosten fallen also pro Gesten-Ende an, nicht pro Frame.
- **Code-Auszug**:
```cpp
std::string snap = HE::sequenceToJson(seq);
if (st.undoPos >= 0 && ... && st.undo[st.undoPos] == snap) return;
st.undo.resize(static_cast<size_t>(st.undoPos + 1));
st.undo.push_back(std::move(snap));
if (st.undo.size() > 64) st.undo.erase(st.undo.begin());
```
- **Lösung**: Ring (`std::deque<std::string>`) statt `erase(begin())`; optional einen Byte-Budget-Deckel (z. B. 16 MB pro Tab) zusätzlich zur Anzahl 64. Delta-Snapshots lohnen sich vermutlich nicht.
- **Erwarteter Effekt**: Speicherbegrenzung; Laufzeit unwesentlich.
- **Risiko / Verifikation**: Gering. Aufpassen auf `undoPos`-Index bei Deque.

### EDT-2013 – Inkonsistente ImGui-Guards (`__has_include`, `HE_IMGUI_ENABLED`) und streuende `#ifdef`s
- **ID**: EDT-2013
- **Kategorie**: B / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `AudioMixView.h:6-8` vs `:77-78, 90`; `AudioWaveformView.h:8-10, 226, 267` und `.cpp:9-11, 411, 1013`; `BuildProgressDialog.cpp:15-17, 344, 599, 603-612, 617, 671, 676, 944`
- **Problem**: `AudioMixView.h` schützt `#include <imgui.h>` mit `__has_include`, benutzt aber `ImVec2` ohne Guard in `EqResult` und `drawEq`; ohne ImGui bricht der Header also ohnehin. `AudioMixView.cpp` bindet `imgui.h` und `EditorWidgets.h` unbedingt ein. `AudioWaveformView` trennt dagegen sauber ImGui-freien und ImGui-Teil. `BuildProgressDialog.cpp` bindet `EditorWidgets.h`/`EditorHelp.h` unbedingt ein und zerlegt `FooterWidth`, `DrawFooter`, `render` in je eigene `#ifdef HE_IMGUI_ENABLED`-Rümpfe mit `[[maybe_unused]]`-Parametern, obwohl der Editor ohne ImGui nicht baut (CLAUDE.md §2 verwendet `-DHE_IMGUI_ENABLED` als Normalfall).
- **Code-Auszug**:
```cpp
void DrawFooter([[maybe_unused]] AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
    if (!hidden()) return;
```
- **Lösung**: Entscheiden: Entweder AudioMix wie AudioWave in einen ImGui-freien Teil (`busChoice`, Mapping, `newBand`) und einen ImGui-Teil trennen, oder die Guards entfernen. Im Build-Dialog die `#ifdef`-Rümpfe entfernen, wenn kein Target ohne `HE_IMGUI_ENABLED` diese Datei kompiliert (CMake prüfen).
- **Erwarteter Effekt**: Weniger Rauschen; evtl. ehrlichere Include-Struktur.
- **Risiko / Verifikation**: Gering; `HorizonEditor`-Build und `he_tests` (die AudioWave/Mix nutzen) bauen.

### EDT-2014 – Ungenutzte Includes, leere `if`-Rümpfe, doppelte Konstanten und Bandlogik
- **ID**: EDT-2014
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `BlendSpacePanel.cpp:14, 21, 175, 374-375, 380-382`; `BoneMaskPanel.cpp:14`; `CinematicPanel.cpp:27`; `AudioMixView.cpp:123-134, 314-323, 325-330, 449-454` vs `AudioWaveformView.cpp:428`
- **Problem**: (a) `Diagnostics/Log.h` wird in `BlendSpacePanel.cpp` und `BoneMaskPanel.cpp` nicht benutzt (kein `HE_LOG_*` in den Dateien). `<cmath>` in `BlendSpacePanel.cpp` scheint ungenutzt; dafür fehlt `<cstdio>` für `std::snprintf` (Zeile 175, kommt zufällig transitiv). `glm/gtc/matrix_transform.hpp` in `CinematicPanel.cpp` hat keinen sichtbaren Verwender (kein `translate`/`rotate`). `HorizonScene/AnimationPose.h` in `BlendSpacePanel.cpp` ist unklar (per Syntax-Check prüfen). (b) Leere Rümpfe: `if (EditorWidgets::Row::dragFloat("Cursor X", ...)) {}` (Zeilen 380, 382) sind Aufrufe, deren Rückgabe ignoriert wird, als `if` getarnt. (c) `weights` ist ein lokaler `std::vector<float>`, der jeden Frame neu allokiert wird (Zeile 374); `blendSpaceWeights` nimmt einen Out-Parameter, ein Member in `PanelState` genügt. (d) In `AudioMixView.cpp` sind „Band einfügen“ (Zeilen 314-319, Doppelklick) und `newBand` (Zeilen 100-117) zwei Pfade, die beide `type = Peak; q = 1.0f` setzen; „Band entfernen“ steht doppelt (Rechtsklick Zeile 327-329 und `removeAt` Zeilen 449-454). `kBg` ist in `AudioMixView.cpp:123` und `AudioWaveformView.cpp:428` identisch (`IM_COL32(20, 21, 25, 255)`) und könnte in `EditorTheme` stehen.
- **Code-Auszug**:
```cpp
if (EditorWidgets::Row::dragFloat("Cursor X", &st.cursorX, 0.01f, -10000.0f, 10000.0f, "%.3f")) {}
```
- **Lösung**: Includes entfernen (`g++ -fsyntax-only` nach jedem Entfernen), leere `if`-Rümpfe zu reinen Aufrufen machen, `weights` als Member, `bandAt(freq, gain)`/`removeBand(eq, v, i)` als kleine Hilfsfunktionen in `AudioMix`.
- **Erwarteter Effekt**: Kleinere Include-Graphen, weniger Duplikat.
- **Risiko / Verifikation**: Sehr gering. `editor_help_audit.py --check` unverändert, weil keine Labels sich ändern.

---

## Cross-Modul-Hinweise

- `src/HE_Editor/EditorUI.cpp:1063-1077`: `appendSnapshots`-Liste ist handgepflegt und enthält `CinematicPanel` nicht (siehe EDT-2003); gleiche Pflege-Liste existiert für `isDirty` (`:992`), `save` (`:1048`), `forget` (`:1117`), `reloadFromDisk` (`:1211`) und das Tab-Dispatching (`:4161`). Eine Registrierungstabelle würde alle fünf Listen ersetzen.
- `src/HE_Core/src/Audio/AudioEdit.cpp:307-316`: `BiquadCoeffs::magnitudeDb` verwendet `std::complex<double>`/`std::polar`/`log10` pro Aufruf; für die EQ-Anzeige (EDT-2004) wäre eine Variante mit vorab berechnetem `cos(w)`, `cos(2w)` ohne Komplexzahlen deutlich billiger.
- `src/HE_Editor/AudioEditorPanel.cpp` (nicht Teil dieses Slice): ruft `AudioWave::buildPeaks` auf; bei sehr langen Clips (siehe Unsicher) bitte prüfen, ob der Aufruf synchron im UI-Thread geschieht und ob das Dekodieren/Peak-Bauen über das `JobSystem` laufen könnte.
- `src/HE_Scene/src/TransformHierarchy.cpp:415`: `propagateTransforms` ist dirty-fähig (`structureEpoch`), geht aber im Zweifel auf `propagateTransformsFull`; `CinematicPreview::restore` setzt `tc->dirty = true`, was bei jedem Frame mindestens einen Teilpass auslöst (EDT-2001).

## Unsicher / Messung nötig

- EDT-2001: Realer Frame-Anteil von `RenderWorldPreview` + Extraktion + `propagateTransforms` im Stillstand; ob Slot-0-Textur zwischen Frames stabil bleibt.
- EDT-2004: Kosten von `magnitudeDb` × Spalten × Bänder pro Frame (6 Bänder, 600 Spalten im Kopf geschätzt).
- EDT-2006: Ob bei realen Rigs (150-300 Gelenke) die O(J²)-Kindersuche gegenüber den ImGui-Widgetkosten überhaupt messbar ist.
- EDT-2010: `AddLine` (AA) vs. `AddRectFilled` für die Wellenformspalten; Wirkung des Overview-Caches.
- `AudioWave::buildPeaks` (`AudioWaveformView.cpp:18-84`) läuft in einem Durchgang über alle Samples. Bei einem 20-Minuten-Stereo-Clip (≈ 115 M Frames) sind das einige hundert Millisekunden, wenn der Aufrufer es im UI-Thread macht (nicht in diesem Slice zu sehen).
- `BuildProgressDialog::s_log` wächst unbegrenzt (`:209`, `Build::log`); bei sehr langen Compiler-Ausgaben ist ein Deckel (z. B. letzte 50 000 Zeilen) ratsam, aber Verhalten (Vollständigkeit der MCP-Auskunft über `snapshot()`) wäre betroffen.
- `BoneMaskPanel.cpp:155-165`: Für Knoten mit Kindern zeichnet `TreeNodeEx(..., "%s", j.name)` den Gelenknamen, danach zeichnet `jointRow` ihn nochmals (`TextUnformatted`). Ob das Absicht ist (zweiter Name hinter der Checkbox) oder doppelt angezeigt wird, ließ sich ohne Laufzeit nicht klären (UI-Dump mit `HE_UI_DUMP_DIR` ansehen).
- `CinematicPanel.cpp:407-427`: Die Akteurzeile kennt weder Umbruch noch Clipping; bei vielen Akteuren läuft sie aus dem Fenster. Ob das praktisch vorkommt, ist offen.
- `CinematicPanel` ohne `appendSnapshots` (EDT-2003) ist evtl. gewollt; bitte mit dem Autor bestätigen.

## Gelesen

Alle 16 Dateien der Slice-Liste wurden vollständig mit dem Read-Tool gelesen (jeweils ein Aufruf ohne Offset, keine abgeschnitten): `src/HE_Editor/AudioMixView.cpp` (463 Zeilen) ja, `AudioMixView.h` (92) ja, `AudioWaveformView.cpp` (1015) ja, `AudioWaveformView.h` (268) ja, `BlendSpacePanel.cpp` (423) ja, `BlendSpacePanel.h` (39) ja, `BoneMaskPanel.cpp` (354) ja, `BoneMaskPanel.h` (33) ja, `BuildProgressDialog.cpp` (947) ja, `BuildProgressDialog.h` (222) ja, `CameraBookmarks.cpp` (122) ja, `CameraBookmarks.h` (64) ja, `CinematicPanel.cpp` (937) ja, `CinematicPanel.h` (43) ja, `CinematicPreview.cpp` (265) ja, `CinematicPreview.h` (107) ja. Fremddateien nur punktuell zur Belegung angesehen (`AudioEdit.h/.cpp`, `TransformHierarchy.cpp`, `EditorUI.cpp`, `HorizonWorld.h`).
