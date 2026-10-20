# Editor Slice 1a – AnimatorStateMachineEditorPanel, AppMetadataRows, AssetAutosave, AssetRecoveryDialog, AssetStubWriter, AssetThumbnailCache, AudioEditorPanel, AudioMixerPanel

## Zusammenfassung

Die 16 Dateien (5283 Zeilen) sind insgesamt sauber und gut kommentiert; es gibt keine Funde mit Schwere Hoch. Die relevantesten Punkte sind (a) ein Logikfehler im Budget-Pfad von `AssetThumbnailCache::get`, der trotz vorhandener Disk-Cache-Datei eine synchrone GPU-Render-Runde auslösen kann, (b) ein nie invalidierter statischer Graph-Cache, der dem eigenen Kommentar ("Non-caching") widerspricht, und (c) mehrere Draw-/Frame-Pfade mit Dateisystemaufrufen oder Allokationen pro Frame (`AudioEditorPanel` Import-Zeile, `AppMetadataRows`, Asset-Autosave-Sammlung). Funde: 0 Hoch, 7 Mittel, 11 Niedrig. Alle Zeilenangaben wurden gegen die gelesenen Dateien geprüft; Messdaten liegen nicht vor, Schätzungen sind als solche gekennzeichnet.

---

### EDT-101 – Thumbnail-Cache: bei erschöpftem Upload-Budget wird die vorhandene Disk-Cache-Datei übergangen und gerendert
- **ID**: EDT-101
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/AssetThumbnailCache.cpp:662-678, 722-729
- **Problem**: Level 1 (Disk-Cache lesen) läuft nur, wenn `s_uploadsLeft > 0` (8 pro Frame). Ist das Budget aufgebraucht, fällt `get()` ohne Prüfung der Datei in Level 2 und rendert — sofern `s_rendersLeft > 0` (2 pro Frame) — den Tile neu per `RenderAssetThumbnail` (synchroner GPU-Readback bzw. Asset-Load), obwohl eine gültige `.hthumb` existiert. Beim Öffnen eines Ordners mit z.B. 300 bereits gecachten Assets rendern also in den ersten Frames bis zu 2 Tiles pro Frame unnötig neu, und `writeFile` überschreibt danach die vorhandene Datei. Für `Scene` hat der Autor genau diese Falle erkannt (Zeilen 684-693), für alle anderen Typen nicht.
- **Code-Auszug**:
```cpp
if (!file.empty() && s_uploadsLeft > 0) { ... readFile(...) ... return e.texture; }
// Level 2: render it.
if (s_rendersLeft <= 0) return nullptr;
...
--s_rendersLeft;
const bool produced = ... s_renderer->RenderAssetThumbnail(*s_content, kind, renderId, kThumbSize, pixels);
```
- **Lösung**: Ist `file` gesetzt, aber `s_uploadsLeft <= 0`, sofort `return nullptr` (Tile versucht es im nächsten Frame wieder, Icon-Fallback bleibt). Alternativ billig prüfen, ob die Datei existiert, und nur dann warten. Skizze:
```cpp
if (!file.empty())
{
    if (s_uploadsLeft <= 0) return nullptr;      // retry next frame; never render past a cache we did not consult
    ... readFile ...
}
```
  Der Szenen-Sonderfall (689-693) vereinfacht sich dadurch (der Zweig `s_uploadsLeft > 0` entfällt).
- **Erwarteter Effekt**: Schätzung: vermeidet bis zu 2 GPU-Renders/Frame während der ersten Frames eines gecachten Ordners; spürbar bei Ordnern mit vielen Meshes/Materialien (ms-Bereich pro Render, abhängig vom Backend).
- **Risiko / Verifikation**: Tiles ohne Cache-Datei werden weiter gerendert, aber erst, wenn das Upload-Budget im Frame nicht erschöpft ist; die Füllzeit eines uncached Ordners kann sich leicht verlängern. Test: `tests/` hat vermutlich Content-Browser-Thumbnail-Tests (nicht geprüft); neuen Test mit Fake-Renderer (Zähler `RenderAssetThumbnail`) und >8 vorab geschriebenen `.hthumb` ergänzen.

### EDT-102 – `loadFunctionGraph`: statischer Graph-Cache wird nie invalidiert (Kommentar widerspricht Code)
- **ID**: EDT-102
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (Korrektheit/Speicher)
- **Ort**: src/HE_Editor/AssetThumbnailCache.cpp:152-169
- **Problem**: Der Kommentar nennt den Loader "Non-caching" und grenzt ihn vom Cache des Material-Editors ab, "whereas a tile is generated once". Tatsächlich ist `graphs` eine Funktions-`static std::map`, aus der nie gelöscht wird (kein Eintrag bei `invalidate()`, `clear()`, `setContext()`/Projektwechsel). Folgen: (1) Wird eine verschachtelte Material-Funktion `F` bearbeitet, nutzt jeder später gerenderte Tile einer Funktion `G`, die `F` aufruft, weiter die alte `F`-Version bis zum Editor-Neustart; (2) bei einem Projektwechsel liefert derselbe `relPath` im neuen Projekt den Graphen des alten; (3) der Cache wächst monoton. Zusätzlich lädt `s_content->loadAsset(relPath)` die Funktion und entlädt sie nie wieder.
- **Code-Auszug**:
```cpp
static std::map<std::string, HE::MaterialGraph> graphs;
if (auto it = graphs.find(relPath); it != graphs.end()) return &it->second;
const HE::UUID id = s_content->loadAsset(relPath);
...
return &(graphs[relPath] = std::move(g));
```
- **Lösung**: Den Cache aus der Funktion in den Dateiscope heben (`s_fnGraphs`, `std::unordered_map`, Zeiger bleiben bei Node-Containern stabil), und (a) in `makeMaterialFunctionScratch` zu Beginn jeder Tile-Erzeugung leeren (Tile wird nur einmal pro Version erzeugt, der Cache muss nur während eines Aufrufs die Zeiger stabil halten), oder (b) in `clear()`/`setContext()`/`invalidate()` leeren. Variante (a) entspricht dem Kommentar ("non-caching") und ist die einfachste.
- **Erwarteter Effekt**: Behebt veraltete Tiles und unbegrenztes Wachstum; Performance neutral (Tile-Erzeugung ist selten).
- **Risiko / Verifikation**: `materialFunctionScratch` ist öffentlich und "für Tests" gedacht; vorhandene Tests (vermutlich in `tests/test_material_graph.cpp`, nur nach Namen zu suchen, nicht ganz lesen) laufen lassen. Neuer Test: Funktion F ändern (neuer JSON), Tile von G erneut erzeugen, generierten GLSL vergleichen.

### EDT-103 – `AudioEditorPanel`: Import-Zeile eines Raw-Tabs ruft pro Frame `std::filesystem::relative` auf
- **ID**: EDT-103
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/AudioEditorPanel.cpp:524-546, 1102-1115 (zusätzlich 735-736)
- **Problem**: Solange ein Tab mit einer rohen .wav/.ogg geöffnet ist, ruft `render()` jeden Frame `writeTargetFor()` auf. Darin läuft `std::filesystem::relative(src.parent_path(), t.root, ec)` (intern `weakly_canonical` auf beiden Pfaden, also Dateisystem-Syscalls) sowie zweimal `isEngineDefaultPath`, und danach wird `target` (String mit `relDir / stem`) neu gebaut. Das Ergebnis ändert sich nur, wenn sich der Pfad oder die Projektwurzel ändert.
- **Code-Auszug**:
```cpp
const WriteTarget wt = writeTargetFor(ctx, assetPath);   // pro Frame
...
t.relDir = std::filesystem::relative(src.parent_path(), t.root, ec);
```
- **Lösung**: `WriteTarget` samt `target`-String im `State` cachen (neues Feld `WriteTarget wt; std::string wtKey;`), Schlüssel = `contentRoot()` + `engineContentRoot()` + Dev-Mode-Flag; nur bei Schlüsseländerung neu berechnen. `runExtract` ruft `writeTargetFor` weiter direkt (einmalige Aktion).
- **Erwarteter Effekt**: Schätzung: spart pro Frame 2-4 Syscalls plus ca. 4-6 String-Allokationen, solange ein Raw-Tab sichtbar ist; spürbar v.a. auf Windows/Netzlaufwerken.
- **Risiko / Verifikation**: Gering. Die Import-Zeile muss nach Projektwechsel korrekt bleiben (Schlüssel enthält die Roots). Manuell: Raw-.wav öffnen, Projekt wechseln, Zielpfad prüfen.

### EDT-104 – `AppMetadataRows`: Stat-Syscalls und String-Schlüssel pro Frame im Draw
- **ID**: EDT-104
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/AppMetadataRows.cpp:121-131, 135-146, 198-207
- **Problem**: Solange Project Settings > Application oder der Export-Dialog sichtbar ist, laufen pro Frame: `fs::is_regular_file(resolveProjectFile(...))` für Icon-Datei und Splash-Bild (je 1 Syscall plus Pfadkonstruktion), und `iconPreviewTexture` baut bei jedem Aufruf (pro Vorschau-Größe, also zweimal bei gleichzeitigem Settings+Export) einen Schlüssel aus 5 Strings, `std::to_string` und `fs::last_write_time` (weiterer Syscall), nur um zu erkennen, dass sich nichts geändert hat. Der Thumbnail-Cache derselben Slice (`kStatIntervalSec = 1.5`) löst genau dieses Problem mit gedrosseltem Re-Stat.
- **Code-Auszug**:
```cpp
const auto mtime = file.empty() ? fs::file_time_type{} : fs::last_write_time(file, ec);
const std::string key = p.appIconName + "|" + p.appIconColor + "|" + file.string() + "|"
                      + std::to_string(static_cast<long long>(mtime.time_since_epoch().count())) + "|" + std::to_string(px);
```
- **Lösung**: Pro Zeile/Slot den Dateistatus ("exists", "mtime") nur alle ~1 s neu abfragen (`ImGui::GetTime()` als Takt, Werte im `Preview`-Slot bzw. in einer kleinen Struct `FileProbe{ std::string path; double at; bool exists; time mtime; }` halten). Den Schlüssel nur neu bilden, wenn eine der Eingaben (Name/Farbe/Pfad-String/mtime) gewechselt hat — Name/Farbe/Pfad lassen sich billig als einzelne Felder statt als zusammengesetzter String vergleichen.
- **Erwarteter Effekt**: Schätzung: ca. 3-4 Syscalls und ca. 8 String-Allokationen pro Frame weniger, solange die Seite offen ist. Absolut klein, aber exakt die Klasse "Dateisystem im Draw" aus BRIEF_EDITOR.
- **Risiko / Verifikation**: Eine extern ersetzte Icon-Datei erscheint bis zu 1 s später. Kein UI-Test nötig; Sichtprüfung. Hinweis: `Preview::handle` und `tex` sind redundant (`tex` ist aus `handle` ableitbar, Zeilen 100-101, 173-174).

### EDT-105 – Asset-Autosave: Dirty-Liste samt `std::function`/Strings wird jeden Frame neu gebaut
- **ID**: EDT-105
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/AssetAutosave.cpp:240-290 (Aufrufer EditorApplication.cpp:13704-13709, EditorUI.cpp:1061-1077), Panel-Seite z.B. AnimatorStateMachineEditorPanel.cpp:149-163, AudioEditorPanel.cpp:619-633
- **Problem**: `updateAssetAutosave` läuft laut Header "one call per frame". Pro Frame ruft `EditorUI::appendAssetSnapshots` 15 Panels ab; jedes Panel iteriert seine States und legt für jede schmutzige Datei einen `AssetSnapshotSource` an: `resolveSavePath(relPath)` (String-Allokation), ein `std::function` mit Capture (`[cm,&st]`, 16 Byte: je nach STL im SBO oder auf dem Heap), `out` ohne `reserve`. Danach baut `update()` jeden Frame `keyed` (reserve), ein `unordered_set<std::string>` (Allokationen je Key), ruft `keyFor` (liefert `std::string` per Wert, d.h. Kopie des gecachten Keys) und iteriert `m_live`. Die eigentliche Arbeit (Schreiben) passiert nur alle 60 s; das Prunen ist die einzige Frame-pflichtige Aufgabe, und die muss nicht 60x pro Sekunde laufen. Bei vielen offenen schmutzigen Tabs sind das einige zig Heap-Allokationen/Frame.
- **Code-Auszug**:
```cpp
std::vector<HE::Ed::AssetSnapshotSource> dirty;
EditorUI::appendAssetSnapshots(ctx, dirty);   // pro Frame
m_assetAutosave.update(nowMs, dirty);
...
std::unordered_set<std::string> dirtyKeys;     // pro Frame in update()
```
- **Lösung**: In `updateAssetAutosave` drosseln: Sammlung/Prune nur, wenn seit dem letzten Lauf >= ~1000 ms vergangen sind (oder eine Tab-Dirty-Generation sich geändert hat), `m_live.empty()` und kein Panel dirty ist der billige Early-out. Zusätzlich `keyFor` als `const std::string&` zurückgeben und `normalised(m_root)` in `configure()` einmal berechnen (derzeit pro `relativeOf`-Aufruf, also pro Datei und Tick, 2x `weakly_canonical`, Zeile 156).
- **Erwarteter Effekt**: Schätzung: Allokationen pro Frame bei N schmutzigen Tabs von ca. 3N auf ca. 0 im Durchschnitt; bei 0 schmutzigen Tabs bleibt es bei 15 leeren Panel-Iterationen (vernachlässigbar).
- **Risiko / Verifikation**: Das Prunen verzögert sich bis ~1 s: Stürzt der Editor in dieser Sekunde nach einem Save ab, bietet er eine bereits gespeicherte Kopie als "ungesichert" an (das Recovery-Dialog meldet `changedSince`). Alternativ ohne Verhaltensänderung: nur SBO-sicher machen (kleinere Captures) und `reserve`. Tests: vorhandene AssetAutosave-Tests (Fake-Uhr, laut Header) um den gedrosselten Pfad erweitern.

### EDT-106 – `AudioEditorPanel::appendSnapshots` kopiert beim Autosave das komplette PCM
- **ID**: EDT-106
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/AudioEditorPanel.cpp:625-631
- **Problem**: Der Schreiber legt `AudioAsset copy = *a;` an und übergibt ihn an `writeAssetTo(RuntimeAsset&, ...)`. `copy` enthält `audioData` (laut Kommentar des Panels "tens of megabytes"), d.h. jede Autosave-Runde eines schmutzigen Audio-Tabs kopiert die gesamte Audio-Payload im UI-Thread, obwohl sich nur der kleine Edit-Chunk (`AudioEdit`) geändert hat. (Das ASM-Panel kopiert dagegen ein kleines JSON-Asset, unkritisch.)
- **Code-Auszug**:
```cpp
AudioAsset copy = *a;
return cm->writeAssetTo(copy, dest);
```
- **Lösung**: `ContentManager::writeAssetTo` auf `const RuntimeAsset&` umstellen (Signatur in ContentManager.h:89 ist non-const; vermutlich nur aus Bequemlichkeit) und `*a` direkt übergeben. Falls der Writer intern mutiert: Edit-Chunk-Pfad (`saveAsset` schreibt "samples exactly as loaded") nutzen. Als Zwischenlösung die Nutzdaten kurz verschieben (`std::swap`) statt kopieren ist riskanter, weil der Zeiger im ContentManager lebt.
- **Erwarteter Effekt**: Schätzung: spart je Autosave-Tick einen memcpy der Clip-Größe (10-100 MB, einige bis einige 10 ms Frame-Hitch) pro schmutzigem Audio-Tab.
- **Risiko / Verifikation**: Const-Korrektheit von `writeAssetTo` und seinen Unterklassen-Writern prüfen (unsicher, nicht gelesen); Test: Asset mit Edit speichern, Snapshot schreiben und byte-identisch zu `saveAsset` vergleichen.

### EDT-107 – `AudioEditorPanel`: Dekodierung, Peaks und Analyse laufen synchron im ersten Frame des Tabs
- **ID**: EDT-107
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: src/HE_Editor/AudioEditorPanel.cpp:159-183, 222-297, 695-722
- **Problem**: Beim ersten Frame eines Tabs werden im UI-Thread nacheinander `AudioEngine::decodeToPcm16` (Vorbis, ganze Datei), `AW::buildPeaks` und `analyze()` (Vollpass über alle Samples) ausgeführt. Der Kommentar in `analyze` benennt selbst "tens of millions of samples per clip". Für lange Ambience-Loops (der Zweck des Tabs) ist das ein spürbarer Freeze beim Öffnen des Tabs; die Tab-Ansicht kann während dieser Zeit weder neu zeichnen noch reagieren. Peaks und Analyse sind reine Funktionen über unveränderliche PCM-Daten und eignen sich für den Job-System-Worker.
- **Code-Auszug**:
```cpp
const AudioAsset* clip = clipOf(ctx, st);                 // decodeToPcm16 on first call
if (!st.peaksDone)    { st.peaks    = AW::buildPeaks(wclip); st.peaksDone    = true; }
if (!st.analysisDone) { st.analysis = analyze(*clip);        st.analysisDone = true; }
```
- **Lösung**: Decode+Peaks+Analyse als ein Job auf dem JobSystem (`std::future`/Atomic-Flag im State), Panel zeigt "Analysing…" bis fertig. PCM ist unveränderlich (siehe Kommentar 156-158), also kein Locking der Samples nötig; State muss nur den Tab-Abbruch (`forget`) abfangen (Handle auf Job halten, Ergebnis verwerfen). Als Minimalschritt: Peaks und Analyse in einem Durchlauf fusionieren (zwei Vollpässe → einer).
- **Erwarteter Effekt**: Schätzung/Messung nötig: Dekodierung eines 60 s-Stereo-Ogg liegt im Bereich 10-50 ms, Analyse+Peaks einer 10-Minuten-Datei zig ms; mit Job entfällt der Frame-Hitch.
- **Risiko / Verifikation**: Moderat (Nebenläufigkeit im Editor). Vorher messen (siehe "Unsicher"). Test: `analyze` ist nach Auslagerung in Header/anonymen Namespace testbar; vorhandene Audio-Panel-Tests (falls vorhanden) mit synchronem Stub-Executor laufen lassen.

### EDT-108 – Thumbnail-Cache: Re-Stat aller Tiles im selben Frame und doppelte Typ-/Hash-Lookups pro Tile und Frame
- **ID**: EDT-108
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/AssetThumbnailCache.cpp:623-656, 903-916, 566-586
- **Problem**: (1) `lastStat` startet bei `-1e9`, alle sichtbaren Tiles stat'en im ersten Frame und danach synchron alle 1.5 s im selben Frame: bei 300 sichtbaren Tiles etwa 600 Syscalls (`last_write_time` + `file_size` in `sourceStampOf`, Zeilen 906-908) in einem Frame, regelmäßig. (2) Jeder `get()`-Aufruf (pro sichtbarem Tile und Frame) macht `thumbnailKindOf` (EditorAssetTypeCache-Lookup, Pfad-Hash) *und* `s_entries[absPath]` (zweiter Pfad-Hash) auch für bereits fertige Tiles, bevor `state == Ready` geprüft wird.
- **Code-Auszug**:
```cpp
ThumbnailKind kind;
if (!thumbnailKindOf(absPath, kind)) return nullptr;   // Hash 1 (+ evtl. Disk-Sniff)
Entry& e = s_entries[absPath];                          // Hash 2
```
- **Lösung**: (a) Beim Anlegen eines Eintrags `lastStat = now - jitter` (z.B. `hash(absPath) % 1.5s`) setzen, damit die Re-Stats über das Intervall verteilt werden; zusätzlich ein einziger Syscall statt zwei (`stat`-basiert, z.B. plattformneutral nicht trivial → mtime+size via `fs::directory_entry` wäre ebenfalls 1 Syscall; sonst belassen). (b) Zuerst `s_entries.find(absPath)`; nur wenn nicht gefunden die Typfrage stellen und einen Eintrag anlegen. Dann kostet ein fertiger Tile einen Lookup und keinen Typ-Lookup.
- **Erwarteter Effekt**: Schätzung: glättet einen periodischen Syscall-Burst im ms-Bereich (Windows/Netzwerklaufwerke stärker); spart ca. 1 Hash pro Tile und Frame. Ohne Messdaten klein.
- **Risiko / Verifikation**: Kein Verhaltensunterschied außer zeitlicher Streuung der Aktualisierung (max. 1.5 s). Der Kommentar bei 627-633 (Typ vor Eintrag, um Map-Einträge für Skripte zu vermeiden) bleibt erfüllt, wenn bei Nicht-Fund zuerst der Typ geprüft wird.

### EDT-109 – State-Machine-Canvas: Modell-Lambdas bauen pro Frame Vektoren und Strings
- **ID**: EDT-109
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/AnimatorStateMachineEditorPanel.cpp:450-485, 525-600
- **Problem**: Pro Frame: `m.nodeIds` legt einen neuen `vector<int>` an, `m.pins` liefert pro Aufruf einen neuen `vector<Pin>` mit zwei Elementen (identisch für alle Knoten), `m.links` ruft pro Transition zweimal `findStateByName` (lineare String-Vergleiche, O(T·S)), und `drawNodeBody` baut pro Knoten zwei `std::string` (`clipState`, `spaceState`) nur zum Ausgeben. Außerdem werden `findStateById` pro Callback linear gesucht (`getPos`, `setPos`, `title`, `drawNodeBody` je Knoten: O(S²) pro Frame). Bei typischen Graphen (10-50 Zustände) unkritisch; mit 200 Zuständen aber messbar.
- **Code-Auszug**:
```cpp
m.pins = [](int) -> std::vector<GraphEditor::Pin> { return { {0,"In",...}, {0,"Out",...} }; };
const HE::AnimationState* from = findStateByName(st.graph, t.fromState);
const std::string clipState = (s->clipId == HE::UUID{}) ? std::string("(no clip)") : ...
```
- **Lösung**: `clipState`/`spaceState` als `const char*` (kein `std::string`); `pins` liefert eine `static const std::vector`-Kopie bzw. (API-Änderung in GraphEditor) einen `span`; in `links` einmal pro Frame eine `unordered_map<string,int>` name→id aufbauen (oder Index-Lookup per `reserve`d Vector); `findStateById` per `unordered_map<int, size_t>` oder über Index, den GraphEditor schon liefert.
- **Erwarteter Effekt**: Schätzung: wenige Allokationen/Frame bei kleinen Graphen, O(S²)→O(S) bei großen; erst ab ca. 100 Zuständen relevant.
- **Risiko / Verifikation**: Gering; `GraphEditor::Model` ist gemeinsame API (siehe Cross-Modul). UI-Test in `tests/test_*_ui.cpp` für den Panel-Pfad (falls vorhanden) laufen lassen.

### EDT-110 – State-Machine-Seitenleiste: feste `char`-Puffer kürzen Namen still, Transitionsliste ist nicht geclippt
- **ID**: EDT-110
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Editor/AnimatorStateMachineEditorPanel.cpp:534-549, 633-638, 650-678, 700-706
- **Problem**: Alle Namensfelder werden über `char buf[64/128]` + `snprintf` in ein Eingabefeld gespiegelt: pro Transition und Frame drei `snprintf` (From/To/Param). Ist ein Name länger als der Puffer (63 bzw. 127 Zeichen), wird er beim ersten Tastendruck still auf die Pufferlänge gekürzt und in den Graph zurückgeschrieben (Datenverlust). Außerdem werden alle Transitionen gezeichnet (7 Widgets + `PushID` je Zeile), ohne Row-Clipping aus CLAUDE.md Abschnitt 4, d.h. 100 Transitionen = 700 Widgets pro Frame.
- **Code-Auszug**:
```cpp
char fb[64], tb[64], pb[64];
std::snprintf(fb, sizeof(fb), "%s", t.fromState.c_str());
...
if (ImGui::InputText("From##t", fb, sizeof(fb)))  { t.fromState = fb; structuralEdit = true; }
```
- **Lösung**: `ImGui::InputText(label, &t.fromState)` via `misc/cpp/imgui_stdlib.h` (wird in AppMetadataRows/AudioMixerPanel bereits genutzt) — keine Puffer, keine Kürzung, keine Kopien. Zeilenhöhe der Transitionszeile konstant halten und mit `ImGuiListClipper` zeichnen (Konvention aus `OutlinerPanel.cpp`); bei `IsItemDeactivatedAfterEdit` für Textfelder ist die `structuralEdit`-Semantik unverändert.
- **Erwarteter Effekt**: Behebt stilles Kürzen; spart pro Transition und Frame 3 `snprintf`; Clipping begrenzt Widgetzahl bei großen Maschinen.
- **Risiko / Verifikation**: Das Eingabefeld "From/To" ist Freitext und kann auf nicht existierende Zustände zeigen (siehe "Unsicher"); Verhalten ändert sich dadurch nicht. Test: Zustandsname > 64 Zeichen setzen, Edit auslösen, Name bleibt vollständig.

### EDT-111 – State-Machine-Panel: duplizierte Reload-Logik und doppelter Asset-Lookup
- **ID**: EDT-111
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/AnimatorStateMachineEditorPanel.cpp:73-86, 186-202, 230-242
- **Problem**: `reloadFromDisk` und `reloadByContentPath` setzen identisch `loaded=false; dirty=false; collabMirror={}; collabSyncMirror={}` (der Kommentar in 234 nennt sie "the same three lines", es sind vier). Kommt ein dritter Mirror hinzu, muss man beide Stellen ändern. In `stateFor` wird `getAnimatorStateMachine(st.assetId)` zweimal aufgerufen (75, 81), mit zwei getrennten `if`-Initialisierungen.
- **Code-Auszug**:
```cpp
st->loaded = false; st->dirty = false; st->collabMirror = {}; st->collabSyncMirror = {};   // 2x
```
- **Lösung**: `static void markForReload(State& st)` einführen und aus beiden Funktionen aufrufen; in `stateFor` `const auto* asset = ...getAnimatorStateMachine(...)` einmal holen und beide JSONs daraus lesen. `reloadByContentPath` kann zudem `reloadFromDisk` nicht ersetzen (anderer Schlüssel), nur den Teil nach dem Lookup teilen.
- **Erwarteter Effekt**: Weniger Duplikat, kein messbarer Laufzeitgewinn.
- **Risiko / Verifikation**: Praktisch null; vorhandene MCP-Animator-Tests (Hooks `McpAnimatorHooks`) laufen lassen.

### EDT-112 – `AudioEditorPanel::render` ist eine ca. 630 Zeilen lange Funktion
- **ID**: EDT-112
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/AudioEditorPanel.cpp:653-1286
- **Problem**: `render()` vereint Lazy-Load, Voice-Verfolgung, Toolbar (ca. 150 Zeilen), linke Info-Spalte (ca. 215 Zeilen), Wellenform-Pane, Tastaturkürzel und den verzögerten Extract. Die Info-Spalte besteht aus Abschnitten (Format/Levels/Silence/Loop seam/Cut/Volume curve/Mixer bus/Preview/Import), die nur `clip`, `an`, `editable`, `st` lesen. Hohe Zyklomatik, schwer testbar; Nebenabhängigkeiten (z.B. `syncTrimView` zweimal, 745 und 928, weil Reihenfolge implizit wichtig ist) sind nur über Kommentare dokumentiert.
- **Code-Auszug**:
```cpp
void render(AppContext& ctx, const std::string& assetPath, const ImVec2& pos, const ImVec2& size)
{ ... }   // Zeilen 653-1286
```
- **Lösung**: In anonyme Funktionen zerlegen: `followVoice(st, ...)`, `drawToolbar(ctx, st, ...)`, `drawInfoColumn(ctx, st, ...)` (mit Unterfunktionen je `SeparatorText`), `drawWavePane(...)`, `handleShortcuts(...)`. Ein gemeinsames Kontext-Struct (`struct Frame { AppContext& ctx; State& st; const AudioAsset& clip; AudioAsset* editable; bool canEdit, editsLocked, audioReady; }`) vermeidet lange Parameterlisten.
- **Erwarteter Effekt**: Lesbarkeit/Testbarkeit; keine Laufzeitänderung.
- **Risiko / Verifikation**: Rein mechanisch; ImGui-ID-Stack und `WrapText`-Scopes (CLAUDE.md: vor `EndChild()` freigeben) beim Verschieben erhalten. Test: bestehende UI-Tests mit Pixel-Dump (`HE_UI_DUMP_DIR`) vor/nach vergleichen.

### EDT-113 – `AudioEditorPanel`: Footer-Texte werden jeden Frame formatiert und mit Umbruch vermessen
- **ID**: EDT-113
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/AudioEditorPanel.cpp:1168-1197, 949-1088 (viele `formatTime/formatFrames/formatGain`-Aufrufe), 1066
- **Problem**: Pro Frame: `AW::readout(...)` (String), `snprintf` in einen 320-Byte-Hint, zwei `ImGui::CalcTextSize(..., wrap_width)` über den langen Hinttext (Umbruchsberechnung ist O(Textlänge) pro Aufruf), mehrere `AW::formatTime(...)` Temp-Strings in der Infospalte, `std::string bus = editable->edit.bus;` (Kopie). Der Hint hängt nur von (Dauer, framesPerPx·canvasW, curveMode, trackpadPointer) ab.
- **Code-Auszug**:
```cpp
const float whereH = ImGui::CalcTextSize(where.c_str(), nullptr, false, wrapW).y;
const float hintH  = ImGui::CalcTextSize(hint, nullptr, false, wrapW).y;
```
- **Lösung**: `hint` und `hintH` im State cachen, Schlüssel = (`canvasW`, `fpp`, `curveMode && canEdit`, `trackpad`, `durationSec`). `where` ändert sich mit Hover/Playhead — das bleibt pro Frame. `formatTime`-Strings der statischen Spalten (`an.durationSec`) einmal beim Analysieren erzeugen.
- **Erwarteter Effekt**: Schätzung: 1-2 Textlayouts und ca. 5 String-Allokationen pro Frame weniger; im Mikrosekundenbereich, nur der Vollständigkeit wegen (Tab ist nur im Vordergrund aktiv).
- **Risiko / Verifikation**: Gering; Layoutunterschiede sichtbar im UI-Dump. Nur umsetzen, wenn ein Profiler Editor-UI-Zeit auf diesem Tab zeigt.

### EDT-114 – `AudioMixerPanel`: Engine-Abgleich und Voice-Zählung jeden Frame, `std::function`-Parameter, Flag-lastiges `drawStrip`
- **ID**: EDT-114
- **Kategorie**: B
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: src/HE_Editor/AudioMixerPanel.cpp:58-64, 81-91, 103-105, 240, 255, 274-298; src/HE_Scene/src/AudioEngine.cpp:594-612 (Cross-Modul)
- **Problem**: (1) `audio.applyBusConfig(cfg)` läuft bei offenem Fenster jeden Frame (Master- + je Bus `createBus`/`setBusVolume`/`setBusEq` mit jeweiliger Map-Suche und EQ-Vergleich); (2) `audio.busVoiceCount(name)` iteriert pro Strip alle `m_impl->sounds` mit String-Vergleich, d.h. O(Busse × Stimmen) pro Frame; (3) `listenFor` ist ein linearer Suchlauf, wird pro Bus mehrfach aufgerufen (255, 294, in `applyListenState` doppelt): O(B²); (4) `drawStrip` erhält zwei `const std::function&`, die pro Strip pro Frame aus Lambdas konstruiert werden (Heap-Allokation möglich je nach Capture-Größe/STL), plus neun Parameter inkl. nullable `solo` und Flags `removable`/`eqLit` — Master- und Bus-Strip unterscheiden sich in Wahrheit nur durch drei Eigenschaften.
- **Code-Auszug**:
```cpp
audio.applyBusConfig(cfg);                       // jeden Frame
StripResult drawStrip(const char* id, const char* title, float* volume, bool* muted, bool* solo,
                      int voices, bool removable, const std::function<void(float)>& onVolume,
                      const std::function<void()>& onListen, bool eqLit = false)
```
- **Lösung**: `applyBusConfig` nur nach Änderung aufrufen (Projekt gewechselt, Bus hinzugefügt/entfernt, `commit`, MCP-seitig geänderte Konfiguration über eine Versionszähler-Abfrage am `AudioBusConfig`) oder zumindest `busVoiceCount` einmal pro Frame in ein `unordered_map<string,int>`/`vector` füllen (eine Iteration über `sounds`). `onVolume/onListen` als Template-Parameter oder `function_ref`; `StripSpec{ title, id, removable, eqLit }` statt Flag-Argumente. `s_listen` als `unordered_map<string, BusListen>` oder Index parallel zu `cfg.buses`.
- **Erwarteter Effekt**: Schätzung: Mikrosekunden; nur bei offenem Mixer-Fenster. Eher Vereinfachung als Performance.
- **Risiko / Verifikation**: `applyBusConfig` wird laut Kommentar bewusst jeden Frame ausgeführt, damit MCP-seitige Bus-Änderungen sofort wirken; ein Dirty-Mechanismus am `AudioBusConfig` muss alle Schreibpfade abdecken (unsicher, nicht geprüft). Tests: bestehende Mixer-/AudioBusConfig-Tests, plus MCP-Pfad manuell.

### EDT-115 – `AssetRecoveryDialog`: kopiert die komplette Offer-Liste jeden Frame
- **ID**: EDT-115
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: src/HE_Editor/AssetRecoveryDialog.cpp:81, 111-127, 203-219
- **Problem**: `const std::vector<AssetRecoveryEntry> offers = *ctx.assetRecoveryOffers;` kopiert jeden Frame, solange der Dialog offen ist, die Liste (je Eintrag 4 Strings). Die Zeilenschleife selbst sammelt die Aktion in `act`/`actKey` und braucht keine Kopie; nur "Restore All"/"Delete All" (203-219) iterieren, während die Callbacks die Quellliste verändern. Zusätzlich pro Zeile pro Frame: `std::filesystem::path(...).filename().string()` und `savedAtText` (localtime_r + strftime) sowie zwei Durchläufe über die Liste (Höhenberechnung und Zeichnen).
- **Code-Auszug**:
```cpp
// A copy: the buttons remove entries from the vector it points at.
const std::vector<HE::Ed::AssetRecoveryEntry> offers = *ctx.assetRecoveryOffers;
```
- **Lösung**: Mit `const auto& offers = *ctx.assetRecoveryOffers;` zeichnen (Aktionen laufen ohnehin nach der Schleife) und nur in den "All"-Zweigen eine lokale Kopie (oder Key-Liste) anlegen. `name`/`when` je Eintrag beim ersten Zeichnen cachen (z.B. Felder in `AssetRecoveryEntry`, oder `displayName` in `pending()` befüllen). Hinweis: Nach `ctx.restoreAssetRecovery` im "Restore"-Zweig (156-161) wird `offers.size()` nicht mehr benutzt, also kein Dangling.
- **Erwarteter Effekt**: Vernachlässigbar (seltener modaler Dialog); Hauptgewinn ist Einfachheit.
- **Risiko / Verifikation**: Referenz auf `*ctx.assetRecoveryOffers` darf nach einem Callback nicht weiterbenutzt werden (Iterator-Invalidierung); daher Zeilenschleife unverändert lassen und die "All"-Zweige kopieren. Kein bestehender Test sichtbar (Dialog ist ImGui); mit `HE_UI_DUMP_DIR` prüfen.

### EDT-116 – `AssetStubWriter`: `if`-Kette statt `switch`, `assetTypeFromName` mit hart codierter Obergrenze
- **ID**: EDT-116
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/AssetStubWriter.cpp:62-121, 192-205
- **Problem**: `writeAssetStub` prüft `type` in acht aufeinanderfolgenden `if`-Blöcken (mit gleichem Muster "Konstanter JSON-String → `addChunk`"), während `isCreatableAssetType` im selben File ausdrücklich ohne `default` per `switch` arbeitet, damit neue Typen bewusst klassifiziert werden. Beim Stub-Schreiben fehlt diese Absicherung: ein neuer erstellbarer Typ, der einen Pflicht-Chunk braucht, wird ohne Compilerwarnung ohne Chunk geschrieben. `assetTypeFromName` läuft linear mit `std::string == const char*` bis zum hart codierten `AssetType::Sequence` (Zeile 199); das Enum-Ende wird so nur per Kommentar abgesichert, ein neues Enum-Mitglied nach `Sequence` bricht lautlos (Enums.h:113 ist aktuell das letzte).
- **Code-Auszug**:
```cpp
if (type == HE::AssetType::InputAction) { const char* json = "{\"valueType\":\"Button\"}"; w.addChunk(HAsset::CHUNK_IACT, json, std::strlen(json)); }
...
for (std::uint32_t i = 1; i <= static_cast<std::uint32_t>(HE::AssetType::Sequence); ++i)
```
- **Lösung**: Kleine Hilfsfunktion `addJson(w, chunk, std::string_view)`, dann `switch (type)` ohne `default` mit je einem `case`; leere Fälle explizit (`case Material: ... break;`). Für den Namen→Typ-Lookup einen `AssetType::Count`-Sentinel in `Enums.h` einführen (gemeinsam mit den beiden dort bereits dokumentierten Switches) oder eine einmal gebaute `static const std::unordered_map<std::string, AssetType>` verwenden.
- **Erwarteter Effekt**: Compiler-gestützte Vollständigkeit; Laufzeit unerheblich (MCP/Create-Menü).
- **Risiko / Verifikation**: Das Enum liegt in HE_Core (Cross-Modul). Test: Round-Trip-Test, der für jeden `isCreatableAssetType`-Typ `writeAssetStub` + Laden prüft (laut Header-Kommentar existiert bereits so ein Tool-Test, nicht geprüft); `assetTypeFromName(assetTypeName(t)) == t` für alle `t`.

### EDT-117 – `AssetThumbnailCache`: duplizierte Box-Filter-Schleifen und Dummy-`ThumbnailKind`
- **ID**: EDT-117
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/AssetThumbnailCache.cpp:109-137, 251-284, 781-813, 305-310
- **Problem**: (1) `makeTextureThumbnail` und `captureToTile` enthalten nahezu denselben Box-Filter mit gedeckelten Taps (`sx0/sx1/stepX/acc/taps`), nur Quellformat/Zeilenrichtung/Alpha-Handling unterscheiden sich. (2) `thumbnailKindOf(absPath, kind)` füllt `kind` für sechs Typen (Texture, Font, Particle, Widget, Scene, MaterialFunction→Material) mit einem Platzhalter `ThumbnailKind::Material`, der laut Kommentaren "unused" ist; die Funktion kombiniert so zwei Fragen ("hat der Typ ein Tile?" und "welche Renderer-Art?"). (3) `makeFontThumbnail` legt `static const char kSample[]` an, kopiert es in eine `std::string` und dekodiert es mit `uiUtf8Decode` zeichenweise.
- **Code-Auszug**:
```cpp
case HE::AssetType::Texture: out = ThumbnailKind::Material; return true;   // "kind unused"
static const char kSample[] = "Aa"; ... const std::string sample = kSample;
```
- **Lösung**: Gemeinsamen `boxDownscale(src, srcStride/format, rect, out)`-Helfer extrahieren (Lambda für Pixel-Fetch), Typ-Dispatch als `enum class TileSource { Renderer, Cpu, ViewportCapture }` plus separates `rendererKindOf`, und `std::string_view`/Konstante statt `static`+Kopie.
- **Erwarteter Effekt**: Weniger Code und weniger Fehlerstellen (der Texturpfad musste schon einmal wegen "Thema 92" korrigiert werden); Laufzeit gleich.
- **Risiko / Verifikation**: Tests existieren laut Header für `textureThumbnail`/`captureToTile` ("Public for tests"); Pixel-für-Pixel-Gleichheit vor/nach Refactoring prüfen.

### EDT-118 – Kleinkram: FNV-1a mehrfach kopiert, ungenutzte Includes, schwere Header
- **ID**: EDT-118
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: src/HE_Editor/AssetAutosave.cpp:174-176 und AssetThumbnailCache.cpp:896-898 (gleiche Konstanten), AudioEditorPanel.cpp:24,30-31 (`<cctype>`, `<iterator>`, `<limits>`), AudioEditorPanel.h:2-4, AnimatorStateMachineEditorPanel.h:2-4
- **Problem**: (1) Dieselbe FNV-1a-64-Schleife steht inline in beiden Dateien (Konstanten `1469598103934665603ull`/`1099511628211ull` kommen laut Suche in mind. 14 Dateien vor, u.a. `CollabDocSync.cpp`, `SceneFileStamp.h`, `HpakFormat.h`). (2) In `AudioEditorPanel.cpp` werden `<cctype>`, `<iterator>`, `<limits>` nicht verwendet (kein `isalnum`, `std::distance`, `numeric_limits` im File; per Grep geprüft); `<cfloat>` wird für `FLT_MIN` gebraucht. (3) `AudioEditorPanel.h` und `AnimatorStateMachineEditorPanel.h` ziehen `EditorUI.h` und `<imgui.h>` nur für `AppContext` bzw. `ImVec2` (Rückgabe-/Parametertypen) in jeden Includer; `AssetAutosave.h` für `AssetSnapshotSource`.
- **Code-Auszug**:
```cpp
std::uint64_t h = 1469598103934665603ull;
for (unsigned char c : rel) { h ^= c; h *= 1099511628211ull; }
```
- **Lösung**: Eine gemeinsame `constexpr std::uint64_t fnv1a64(std::string_view)` in einem kleinen Core-Header (HE_Core, `Types/Hash.h` o.ä.; vorher prüfen, ob bereits ein Helfer existiert) und in den Editor-Dateien verwenden. Ungenutzte Includes entfernen. In den Panel-Headern `AppContext` vorwärts deklarieren (`struct AppContext;`) und `ImVec2` über `<imgui.h>` beibehalten oder `struct ImVec2;` nur, wenn der Header nicht inline benutzt; `AssetSnapshotSource` bräuchte ein kleines `AssetSnapshotSource.h`.
- **Erwarteter Effekt**: Schätzung: etwas kürzere Kompilierzeit durch kleinere Header; weniger Duplikate.
- **Risiko / Verifikation**: Hash-Ergebnis muss bitgleich bleiben (Dateinamen im Cache/Autosave hängen daran): Test mit festen Eingaben (`fileNameFor("a/b")`, `AssetAutosave::keyFor`). Include-Bereinigung per Build aller Targets prüfen.

---

## Cross-Modul-Hinweise

- `src/HE_Scene/src/AudioEngine.cpp:594-612`: `busVoiceCount` ist O(Stimmen) pro Aufruf, `applyBusConfig` setzt jedes Mal alle Bus-Lautstärken neu (vgl. EDT-114). Ein `voiceCountsByBus()` (eine Iteration) wäre die passende Engine-API.
- `src/HE_Core/include/ContentManager/ContentManager.h:89`: `writeAssetTo(RuntimeAsset&, ...)` nimmt non-const, was EDT-106 (PCM-Kopie) erzwingt. Const-Überladung prüfen.
- `src/HE_Core/include/Types/Enums.h:85-113`: kein `Count`-Sentinel; `assetTypeFromName` und mehrere Switches hängen am letzten Enumerator (EDT-116).
- GraphEditor-Modell (`GraphEditor.h`, nicht Teil dieses Slices): `Model::pins`/`nodeIds`/`links` liefern Vektoren per Wert über `std::function`, was alle Aufrufer (Material/HorizonCode/Particle/ASM) pro Frame zu Allokationen zwingt (vgl. EDT-109). Eine `span`/Callback-basierte API wäre projektweit effizienter.
- FNV-1a-Duplikate in `src/HE_Editor/CollabDocSync.cpp`, `CollabPresenceBar.cpp`, `CollabController.cpp`, `TerrainSync.cpp`, `SceneFileStamp.h`, `src/HE_Core/include/Hpak/HpakFormat.h`, `src/HE_Core/src/Hpak/ProjectExporter.cpp`, `src/HE_Scene/src/Water*.cpp`, `RopeTrailSystem.cpp` (nur über Konstantensuche gefunden, nicht gelesen).
- `AppMetadataRows::previewSlot` (AppMetadataRows.cpp:103-109) hält `Preview::handle` als statische GPU-Textur; ein Projekt-/Renderer-Wechsel oder das Beenden gibt sie nicht frei (siehe Unsicher), ähnlich wie `AssetThumbnailCache::shutdown()` es für Tiles tut.

## Unsicher / Messung nötig

- EDT-107: tatsächliche Dauer von `decodeToPcm16` + `buildPeaks` + `analyze` für typische Ambience-Loops (1-10 Min.) im Release-Build messen, bevor ein Job-Umbau begründet wird.
- EDT-105/106: Frame-Zeit von `updateAssetAutosave` mit z.B. 5 offenen schmutzigen Tabs und 50-MB-PCM messen (Hitch bei Autosave-Tick vs. durchschnittliche Allokationslast).
- EDT-101: Wie oft tritt der Fall "Upload-Budget leer, Render-Budget frei" in der Praxis auf? Mit einem Zähler für `RenderAssetThumbnail`-Aufrufe vs. vorhandene `.hthumb` messen.
- `AssetThumbnailCache::image()` (Zeilen 566-621): Laden + `toDisplayRgba` + Upload eines 4K-Bildes läuft ungedrosselt synchron im Frame (bewusst laut Header). Beim Wechsel zwischen Seiten mit vielen großen Bildern und der 5 s-Räumung (`kImageIdleSec`) mögliche Re-Upload-Hitches; messen, ob ein Budget oder Hintergrund-Decode nötig ist.
- `AppMetadataRows`: `s_previews` gibt die GPU-Textur beim Projekt- oder Renderer-Wechsel nicht frei (nur bei geändertem Schlüssel); ob `DestroyImGuiTexture` am Ende noch einen gültigen Renderer hat, wurde nicht geprüft (potentielles Dangling-Handle bei Renderer-Wechsel).
- ASM-Panel: Transitionen referenzieren Zustände über Namen (Strings). Doppelte Zustandsnamen oder freie Texteingaben in "From/To" (Zeilen 659-662) erzeugen mehrdeutige/baumelnde Links; `findStateByName` liefert dann den ersten Treffer. Ob die Laufzeit (AnimationStateMachineSystem) ebenfalls nach Namen auflöst, wurde nicht geprüft; eine ID-basierte Referenz wäre eine Datenformat-Änderung (Loader-Kompatibilität nötig).
- EDT-114: Ob `AudioEngine`-Aufrufe aus dem UI-Thread intern gelockt sind (kein Lock in `applyBusConfig` gesehen), wurde nicht geprüft.

## Gelesen

Dateien vollständig gelesen (alle 16, ja): AnimatorStateMachineEditorPanel.cpp (858 Z.), AnimatorStateMachineEditorPanel.h, AppMetadataRows.cpp, AppMetadataRows.h, AssetAutosave.cpp, AssetAutosave.h, AssetRecoveryDialog.cpp, AssetRecoveryDialog.h, AssetStubWriter.cpp, AssetStubWriter.h, AssetThumbnailCache.cpp (980 Z.), AssetThumbnailCache.h, AudioEditorPanel.cpp (1288 Z.), AudioEditorPanel.h, AudioMixerPanel.cpp, AudioMixerPanel.h; keine Datei ausgelassen. Fremddateien nur punktuell (EditorApplication.cpp:13690-13715, EditorUI.cpp:1055-1100, AudioEngine.cpp:474-624, Enums.h:85-113, ContentManager.h:89) zur Belegung.
