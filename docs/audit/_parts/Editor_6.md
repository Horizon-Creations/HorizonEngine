# Editor Slice 6 – EditorHelp, EditorInput(+Mac), EditorMarquee, EditorMultiEdit, EditorPanelState, EditorReference

## Zusammenfassung
Der Slice ist insgesamt sauber und gut kommentiert. `EditorHelp.cpp` besteht zu ~9350 von 9800 Zeilen aus reinen Daten (1776 `Entry`, 155 `Area`, 16 `PanelTopic`, 41 Komponenten-Scopes); der Code-Teil (ab Zeile 9618) ist klein und bereits einmal auf einen Hash-Index umgestellt (`findKey`, Zeilen 9634-9651). Lookups sind hover-gated (`helpForLabel`/`helpForKey`), es gibt keine doppelten Schlüssel (per Skript geprüft: 1776 eindeutig, alle Einträge haben eine `Area`). Die relevanten Funde liegen bei `EditorMultiEdit` (Vollserialisierung pro Drag-Frame und pro Member, Row-Marks/Summary jeden Frame neu gebaut) und bei `EditorInput::detectTrackpad` (SDL-Aufruf mit Allokation pro Frame). Dazu ein sichtbarer Defekt in `EditorReference` (`##wx`-Suffix landet im Seitentitel). Insgesamt 12 Funde: 0 Hoch, 4 Mittel, 8 Niedrig.

## Funde

### EDT-601 – Multi-Edit serialisiert pro Drag-Frame die ganze Entity N+2 mal
- **ID**: EDT-601
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorMultiEdit.cpp:10-19`, `:75-104`; Aufrufer `src/HE_Editor/InspectorPanel.cpp:276-279`, `:326-327`
- **Problem**: `state()` serialisiert JEDE Komponente der Entity per `SceneSerializer` nach CBOR und parst sie sofort wieder zu `json`. Bei einem Drag (Maus gedrückt, `inputMayEditThisFrame()`) passiert das jeden Frame für den Primary zweimal (vor/nach den Widgets) und in `propagate()` zusätzlich einmal pro Member (`state(world, member)`), nur um zu prüfen, ob Komponente und Pfad existieren. Bei Strg+A in einem großen Level (Kommentar InspectorPanel:141-148 nennt "tausend") sind das ~1000 Voll-Serialisierungen samt JSON-Parse pro Frame, solange ein Wert gezogen wird. Zusätzlich wird eine `SceneSerializer`-Instanz pro Aufruf angelegt (Kosten der Konstruktion unsicher).
- **Code-Auszug**:
```cpp
for (Entity member : members) {
    ...
    json mine = state(world, member);          // ALLE Komponenten, CBOR -> json
    json patch = json::object();
    for (const Change& c : changes) {
        const auto comp = mine.find(c.component);
        if (comp == mine.end()) continue;
        if (!comp->contains(c.path)) continue;
        (*comp)[c.path] = c.value;
        patch[c.component] = *comp;
    }
```
- **Lösung**: (1) `propagate` nur dann ausführen, wenn `changes` nicht leer ist (ist bereits so) und die Member-Zustände nur für die betroffenen Komponenten holen: eine `state(world, e, componentNames)`-Variante, falls `SceneSerializer` einen Komponentenfilter anbietet oder ergänzt bekommt (Cross-Modul, siehe unten). (2) Während des Drags `propagate` höchstens einmal pro Frame, aber die "after"-Aufnahme des Primary nur wenn der Frame tatsächlich ein Widget geändert hat (ImGui-`changed`-Rückgabe von `renderFor`) statt bei jedem Frame mit gedrückter Maus. (3) Alternativ nur beim Loslassen anwenden und während des Drags auf einem Teil der Member vorschauen (Verhaltensänderung, daher nicht ohne Abstimmung).
- **Erwarteter Effekt**: (Schätzung) bei 100+ gewählten Entities spürbar flüssigeres Ziehen im Details-Panel; bei 1-5 Entities kaum messbar. Messen: Frame-Zeit beim Ziehen mit 200/1000 Members.
- **Risiko / Verifikation**: Funktioniert nur, wenn die Teil-Serialisierung dasselbe JSON liefert wie `state()`; `tests/test_editor_multi_edit.cpp` deckt `diff`/`propagate` ab, ergänzen um Fall "Member ohne Komponente / kürzeres Array". Die "changed"-Variante (2) ändert Verhalten, wenn ein Widget den Wert ohne `changed`-Flag modifiziert (z. B. Gizmo-Buttons, Strukturaktionen) – daher mit `structural` aus `renderFor` kombinieren.

### EDT-602 – `rowMarks`/`describe` werden jeden Frame für jede gemeinsame Komponente neu gebaut
- **ID**: EDT-602
- **Kategorie**: C / D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorMultiEdit.cpp:110-117`, `:183-198`, `:200-248`; Aufrufer `src/HE_Editor/InspectorPanel.cpp:292-302`
- **Problem**: `s_multi.mixed` wird sauber gecacht (Refresh alle 30/240 Frames), aber die daraus abgeleiteten Strukturen nicht: pro Frame und pro Komponente läuft `rowMarks` (Neuaufbau einer `unordered_map<string,unsigned>`) und `describe` (Neuaufbau eines Strings samt `spelled`/`elementName`-Temporären). Beide rufen für jeden `Mixed` der betreffenden Komponente `tokensOf()` auf (kopiert den `json_pointer`, baut ein `vector<string>` durch `back()/pop_back()` + `reverse`), und beide iterieren pro Komponente über die komplette `mixed`-Liste (O(Komponenten × Leaves) pro Frame). Alles ist reine Funktion von (`mixed`, `component`) und ändert sich nur beim Refresh.
- **Code-Auszug**:
```cpp
const Row::MixedScope mixedRows(
    key ? EditorMultiEdit::rowMarks(s_multi.mixed, key)
        : std::unordered_map<std::string, unsigned>{});
...
const std::string differs = EditorMultiEdit::describe(s_multi.mixed, key);
```
- **Lösung**: Im `MultiSelectionCache` pro Komponente ein Paar `{marks, differs}` ablegen, beim Refresh (`refreshMultiSelection`) einmal aus `c.mixed` berechnen (`std::unordered_map<std::string, Cached>`), und im Frame nur noch nachschlagen. `MixedScope` auf eine Referenz auf die gecachte Map umstellen statt Kopie/Move einer frischen Map.
- **Erwarteter Effekt**: (Schätzung) pro Frame mehrere hundert kleine Allokationen weniger bei vielen abweichenden Feldern; bei wenigen `mixed`-Leaves vernachlässigbar.
- **Risiko / Verifikation**: Gering; Ergebnis ist identisch, solange der Cache bei `stale`/Revision/Periode zusammen mit `mixed` invalidiert wird (derselbe Pfad). Bestehende Tests für `rowMarks`/`describe` unverändert nutzbar.

### EDT-603 – `detectTrackpad()` ruft auf Win/Linux jeden Frame `SDL_GetTouchDevices` auf (Heap-Allokation + Lock)
- **ID**: EDT-603
- **Kategorie**: C / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorInput.cpp:9-45`, `:59-63`; Aufrufer `src/HE_Editor/EditorUI.cpp:605` (jeden Frame) plus 7 weitere Stellen (`ViewportPanel.cpp:1150`, `EditorViewportNav.cpp:82`, `StaticMeshEditorPanel.cpp:168/515/548`, `AudioEditorPanel.cpp:1185/1203`)
- **Problem**: Auf einem Desktop ohne Akku und ohne Touch-Gerät (der Normalfall bei Windows/Linux-Workstations) bleibt `sawTrackpad` dauerhaft `false`, also läuft die Probe bei JEDEM `trackpadPointer`-Aufruf neu: `SDL_GetTouchDevices` liefert ein frisch allokiertes Array (`SDL_free` danach). Das passiert mindestens einmal pro Frame (EditorUI) und zusätzlich in jedem Panel-Aufruf. Der Kommentar rechtfertigt das Dauerprobieren nur für macOS (Lazy-Registrierung beim ersten Touch).
- **Code-Auszug**:
```cpp
static bool sawTrackpad = false;
if (!sawTrackpad)
{
    int count = 0;
    if (SDL_TouchID* ids = SDL_GetTouchDevices(&count))
    { ... SDL_free(ids); }
}
```
- **Lösung**: Die Probe drosseln (z. B. nur wenn sich `SDL_GetTicks()` um >= 1000 ms geändert hat) oder ereignisgesteuert machen (bei `SDL_EVENT_FINGER_DOWN`/`SDL_EVENT_FINGER_MOTION` bzw. Touch-Device-Hotplug einmal neu prüfen). Das Ergebnis des Frames außerdem in `beginFrame()` einmal berechnen und die Panels nur `detail::g_trackpadActive` lesen lassen (siehe `trackpadActive()`); `trackpadPointer(ctx)` müsste dann nur noch `resolveTrackpad` mit dem Cache aufrufen.
- **Erwarteter Effekt**: (Schätzung) ein paar Mikrosekunden und eine Allokation pro Aufruf gespart; klein, aber ein Dauerkostenpunkt ohne Nutzen. Messen mit SDL-Profil unter Windows.
- **Risiko / Verifikation**: Gedrosselte Erkennung verzögert das automatische Umschalten um bis zu eine Sekunde; Test: `resolveTrackpad` ist bereits rein testbar, Drosselung per Hand prüfen (Magic Trackpad anstecken, Auto-Modus).

### EDT-604 – `EditorReference`: "##"-Suffix landet im Seitentitel (sichtbarer Defekt)
- **ID**: EDT-604
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/EditorReference.cpp:38-47` (labelOf), `:99-101`; betroffene Daten `src/HE_Editor/EditorHelp.cpp:1195`, `:1199`, `:1202`, `:1206`, `:1211`
- **Problem**: `labelOf` nimmt bei leerem `title` den Teil nach dem letzten `/` des Schlüssels – inklusive ImGui-ID-Suffix. Fünf Einträge haben `title == ""` und ein `##wx`-Suffix (`Weather/Fog Density##wx`, `Wind Speed##wx`, `Rain##wx`, `Snow##wx`, `Wetness##wx`); ihre Abschnitte heißen in der generierten Referenz daher "Fog Density##wx" usw., ebenso fließt es in `sec.text` (Suche) ein. Gleiche Behandlung fehlt in `Help::find`-Gegenstück `visible()` (EditorHelp.cpp:9627), das nur im Anonymous-Namespace existiert.
- **Code-Auszug**:
```cpp
const std::size_t slash = key.rfind('/');
return slash == std::string::npos ? key : key.substr(slash + 1);
```
- **Lösung**: Nach dem Slash-Split den Teil ab `##` abschneiden (`label.substr(0, label.find("##"))`) – am besten über eine geteilte Funktion `Help::visibleLabel()` (die vorhandene `visible()` exportieren), damit Lookup und Referenz dieselbe Regel nutzen. Der Anker (`sec.id`) darf das Suffix behalten, er muss mit `referenceTopic` übereinstimmen.
- **Erwarteter Effekt**: Korrekte Titel im Handbuch; keine Performance-Wirkung.
- **Risiko / Verifikation**: Gering. `tests/test_editor_help.cpp` um Prüfung erweitern: kein `sec.title` enthält "##". Anker-Auflösung (F1) bleibt unverändert.

### EDT-605 – `referenceTopic()` wird für jedes angezeigte Tooltip-Frame neu berechnet
- **ID**: EDT-605
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorRowWidgets.cpp:603-607`; `src/HE_Editor/EditorHelp.cpp:9738-9792`
- **Problem**: Solange ein Tooltip sichtbar ist, läuft pro Frame `referenceTopic(e->key)`: `areaOf` scannt linear alle 155 Präfixe (String-View-Vergleich), danach bei Komponenten-Keys zusätzlich bis zu 41 Scopes, dann zwei `std::string`-Allokationen (`anchor`, Rückgabe). Das Ergebnis wird nur gebraucht, wenn F1 gedrückt wird bzw. um den Hinweis "F1 documentation" anzuzeigen.
- **Code-Auszug**:
```cpp
static std::string s_topic;
s_topic = HE::Ed::Help::referenceTopic(e->key);
```
- **Lösung**: Pro Tooltip-Wechsel (Vergleich `e != s_lastQueued`) einmal berechnen und `s_topic` behalten; oder `bool hasReference(key)` ohne String-Bau für die Anzeige und `referenceTopic` erst bei `IsKeyPressed(F1)`.
- **Erwarteter Effekt**: (Schätzung) wenige Mikrosekunden/Frame, nur während ein Tooltip steht. Niedrig.
- **Risiko / Verifikation**: Keines; `s_lastQueued` existiert schon (RowWidgets:475).

### EDT-606 – `findKey` ohne Hover-Gate in Toolbar, `helpMarker` und Asset-Slot (Doppel-Lookup)
- **ID**: EDT-606
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorToolbar.cpp:56`; `src/HE_Editor/EditorRowWidgets.cpp:524-531`; `src/HE_Editor/EditorWidgets.cpp:135-136`
- **Problem**: Drei Stellen hashen jeden Frame den Schlüssel, ohne dass das Item gehovert ist: Toolbar-Zelle (`findKey(helpKey)` pro Zelle), `helpMarker` (jede Marker-Zeile) und der Asset-Slot, der erst `findKey(helpKey)` aufruft und dann `helpForKey(helpKey)` nochmals (zweiter Hash, der erste nur für die Verzweigung). Bei ~60 Zellen und ~25 ns/Hash ist das ~1,5 µs/Frame – das Performance-Audit (Kommentar EditorHelp.cpp:9637-9641) hat den Hauptteil bereits beseitigt.
- **Code-Auszug**:
```cpp
if (helpKey && HE::Ed::Help::findKey(helpKey))
    helpForKey(helpKey);
else if (ImGui::IsItemHovered())
```
- **Lösung**: Asset-Slot: einmal `const Entry* e = findKey(...)` und `helpForEntry(e)`. Toolbar/`helpMarker`: Entry-Zeiger pro Call-Site cachen (`static const Entry* e = findKey("...")`), da die Tabelle konstant ist; oder Lookup erst bei `hovered`, wobei die Weiche "Help- vs. Plain-Tooltip" nur beim Hover relevant ist.
- **Erwarteter Effekt**: Mikrosekunden; reine Konsolidierung. Nur zusammen mit anderem Umbau lohnend.
- **Risiko / Verifikation**: `helpMarker` zeigt "(?)" nur bei existierendem Eintrag – bei Hover-Gating darf diese Entscheidung nicht verschoben werden. Gering.

### EDT-607 – Hash-Index von `findKey` wird lazy beim ersten Hover gebaut; Startkosten ungemessen
- **ID**: EDT-607
- **Kategorie**: D / F
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Editor/EditorHelp.cpp:9643-9648`, Tabelle `:25-9372`
- **Problem**: Die Tabelle `kEntries` ist `constexpr` (71 KB Zeigerdaten in `.data.rel.ro`, mit ~8,9 k Relokationen unter PIE/macOS-Rebase, plus ~370 KB Strings in `.rodata`). Der Index (`unordered_map<string_view, const Entry*>`, 1776 Knoten-Allokationen, jeder Key wird gehasht) entsteht beim ersten `findKey`, also im ersten Frame mit Hover bzw. in der ersten Toolbar-Zelle – ein Mini-Hitch im ersten Frame, plus ~1776 kleine Heap-Knoten über die gesamte Laufzeit. Die Struktur ist ansonsten richtig (kein linearer Scan, `try_emplace` erhält Ersttreffer-Semantik).
- **Code-Auszug**:
```cpp
static const std::unordered_map<std::string_view, const Entry*> s_index = [] {
    std::unordered_map<std::string_view, const Entry*> m;
    m.reserve(std::size(kEntries));
    for (const Entry& e : kEntries) m.try_emplace(e.key, &e);
    return m;
}();
```
- **Lösung**: Nur falls Messung (Startup-Zeitstempel, erster Frame) eine nennenswerte Größe zeigt: statt Hash-Map ein `std::vector<const Entry*>` einmal nach Key sortieren (`stable_sort`, damit der Ersttreffer gewinnt) und per `lower_bound` suchen (1 Allokation, cache-freundlicher), oder den Index zur Build-Zeit erzeugen (gperf/constexpr-Sortierung in einem `consteval`-Block). Alternativ den Index im Editor-Start im Hintergrund-Job aufbauen.
- **Erwarteter Effekt**: (Schätzung, unsicher) Index-Aufbau ~100-300 µs einmalig, ~100 KB Heap weniger bei vector-Variante. Nicht ohne Messung umsetzen.
- **Risiko / Verifikation**: Binärsuche ist pro Lookup langsamer als Hash (log2 1776 ≈ 11 String-Vergleiche), also nur wenn Hover-Gating gilt; `tests/test_editor_help.cpp` prüft Eindeutigkeit/Ersttreffer bereits.

### EDT-608 – `EditorReference::install`: unnötige Temporäre und Duplikat-Strings beim Aufbau
- **ID**: EDT-608
- **Kategorie**: C / F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/EditorReference.cpp:52-55` (`whereOf`), `:87-90`, `:99-103`, `:147-148`; Aufruf `src/HE_Editor/DocsPanel.cpp:236`
- **Problem**: (a) Der Komparator der `stable_sort` baut pro Vergleich zwei `std::string(a.area->group)`/`std::string(a.entry->key)`-Temporäre (Gruppen > 15 Zeichen und Keys > 15 Zeichen allokieren): bei 1776 Zeilen O(n log n) Vergleiche. (b) `whereOf(e, area)` ignoriert `e` (ungenutzter Parameter) und gibt nur `area.group` zurück. (c) Der Body liegt dreimal vor (`kEntries`-Literal, `Paragraph`-Run, `sec.text`), `sec.text` hängt zusätzlich title/eyebrow/key an: ~330 KB Body-Text → ~1 MB Heap in der Docs-Library. (d) `pageTitle[a->page] = a->title` wird je Eintrag überschrieben statt je Seite einmal.
- **Code-Auszug**:
```cpp
const int g = std::string(a.area->group).compare(b.area->group);
return g != 0 ? g < 0 : std::string(a.entry->key) < std::string(b.entry->key);
```
- **Lösung**: `std::string_view` im Komparator; `whereOf` entfernen und `area->group` direkt nutzen; `pageTitle.try_emplace`. Für (c) den Suchtext nur aus Titel + Body bilden, falls die Suche sonst nichts braucht (`sec.eyebrow` und Key sind für Treffer beim Nutzer nützlich – Absicht prüfen) oder `Docs::Section::text` lazy beim ersten Suchen füllen.
- **Erwarteter Effekt**: (Schätzung) einmaliger Aufbau beim Öffnen der Docs, ~ms und wenige hundert KB; Niedrig.
- **Risiko / Verifikation**: Sortierreihenfolge muss identisch bleiben (string_view-Vergleich ist lexikografisch wie `std::string`). Tests: `test_editor_help` (Seitenstruktur).

### EDT-609 – Kleine Ineffizienzen in `EditorMultiEdit` (`propagate`, `mixed`, `diffInto`)
- **ID**: EDT-609
- **Kategorie**: A / D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/EditorMultiEdit.cpp:28`, `:78`, `:85-92`, `:175-177`
- **Problem**: (a) `patch[c.component] = *comp;` kopiert den gesamten Komponenten-JSON für JEDEN Change, auch wenn mehrere Changes in dieselbe Komponente gehen (letzte Kopie enthält alle, die früheren sind Verschwendung). (b) `std::find(skip.begin(), skip.end(), member)` pro Member: O(n·|skip|); in einer Collab-Session mit vielen gehaltenen Entities quadratisch. (c) `mixed()` prüft `seen` mit linearem `std::any_of` über das wachsende Ergebnis: O(L²) für L Leaves. (d) `diffInto` beginnt jede Rekursionsebene mit `before == after` (tiefer Vergleich) – für jeden Unterbaum, der sich unterscheidet, wird der Vergleich auf jeder Ebene wiederholt (O(Tiefe·Größe)).
- **Code-Auszug**:
```cpp
(*comp)[c.path] = c.value;
patch[c.component] = *comp;
```
- **Lösung**: (a) Schreibzugriffe auf `*comp` sammeln und `patch[c.component] = std::move(*comp)` nach der Change-Schleife je Komponente einmal setzen (Changes nach Komponente gruppieren oder `patch.contains` prüfen und auf `patch[c.component]` direkt schreiben). (b) `skip` einmal in ein sortiertes Vektor/`unordered_set` kopieren (oder dem Aufrufer erlauben, es sortiert zu liefern). (c) `std::set<std::pair<std::string,std::string>>`/Hash-Set über (component, path.to_string()) oder zuerst sortieren + `unique`; Reihenfolge "erste Sichtung" beachten. (d) Gleichheitstest nur an Blättern, Objekt/Array-Zweig ohne Vorab-Vergleich (Ergebnis identisch, da gleiche Teilbäume keine Changes erzeugen).
- **Erwarteter Effekt**: Nur bei sehr großen Selektionen/Leave-Mengen messbar (Schätzung); sonst Lesbarkeit.
- **Risiko / Verifikation**: (d) ändert die Aufrufzahl, nicht das Ergebnis – `tests/test_editor_multi_edit.cpp` deckt `diff`/`mixed` ab; Reihenfolge der `mixed`-Ausgabe muss erhalten bleiben (Header-Vertrag "in der Reihenfolge des Referenz-Zustands").

### EDT-610 – `EditorHelp`: tote Area-Regeln und handgepflegte Parallellisten
- **ID**: EDT-610
- **Kategorie**: F / A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/EditorHelp.cpp:9411`, `:9486`, `:9521`, `:9538`, `:9540`, `:9556`, `:9557`, `:9578`, `:9582` (tote Regeln); `:9604-9615` (`kComponentScopes`); `:9378-9395` (`kPanelTopics`)
- **Problem**: Neun `kAreas`-Präfixe (`"hub."`, `"settings."`, `"material."`, `"input."`, `"hc."`, `"terrain."`, `"env."`, `"profiler."`, `"collab."`) treffen keinen einzigen der 1776 Schlüssel (per Skript über den Tabellen-Quelltext geprüft; im Code sind Strings mit denselben Präfixen nur Settings-Ids (`EditorSettingsCatalog.cpp:96/261`) und HorizonCode-Node-Namen (`HcNodeDocs.cpp`), keine Help-Keys). Sie kosten jeden `areaOf`-Aufruf einen unnötigen Vergleich und wecken den Eindruck, es gäbe diese Einträge. `kComponentScopes` (41 Namen) muss manuell mit den `"<Komponente>/…"`-Keys in sync gehalten werden – eine neue Komponente ohne Eintrag landet in keiner Area (der Test meldet das, aber erst nach dem Build). Zudem baut `areaOf` einen separaten statischen `vector<Area>` (Zeilen 9764-9770) nur um die Scopes in ein `Area`-Format zu bringen.
- **Code-Auszug**:
```cpp
{ "hub.",      "editor-interface", "Editor Interface", "Project Hub" },
...
{ "collab.",   "editor-collab",    "Collaboration & Source Control", "Collaboration" },
```
- **Lösung**: Tote Regeln löschen (nach Gegenprobe `grep` auf `"hub."` usw. in `src/HE_Editor` und in `tests/`). Optional: `kComponentScopes` als `constexpr Area`-Tabelle direkt anlegen statt `const char*[]` + Laufzeit-Vector (ein Datenblock weniger, kein `static`-Init).
- **Erwarteter Effekt**: Weniger Rauschen in Reviews; Mikrovorteil bei `areaOf`.
- **Risiko / Verifikation**: Gering; `tests/test_editor_help.cpp` (prüft Area je Key) muss weiter grün sein. Falls Plugins/Projekte (nicht im Repo) diese Präfixe verwenden, vorher mit dem Team klären.

### EDT-611 – Doppelt gepflegte Help-Texte über Scopes
- **ID**: EDT-611
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/EditorHelp.cpp` – Schlüsselpaare mit byte-gleichem Body: `Viewport Menu/Unlock` = `World Outliner/Unlock`; `Project General/{Show a splash while starting, Splash image, Subtitle, Browse...}` = `Export/…`; `Application/Browse...` = `Export/Browse...` = `Project General/Browse...`; `Application/Icon file` = `Export/Icon file`; `Sequencer/Delete Key` = `UI Timeline/Delete Key`; `UI Variable/Rotation##vdef` = `Script Variable/Rotation##vdef`
- **Problem**: Acht Gruppen identischer Texte werden an mehreren Stellen gepflegt; bei Änderungen driftet eine Kopie (die Datei warnt selbst vor Drift Tooltip/Handbuch). Die Literale werden vom Linker meist zusammengeführt, kosten also keinen Speicher, nur Wartung.
- **Code-Auszug**: (Zitat nicht möglich ohne ganze Einträge; Belege per Duplikat-Suche, siehe Ort)
- **Lösung**: Nur wenn die Texte auch dauerhaft gleich bleiben sollen: `constexpr const char*`-Konstanten im Anonymous-Namespace (`kSplashBody` …) und in beiden Einträgen referenzieren. Alternativ einen Alias-Mechanismus in `find()` (Scope A fällt auf Scope B zurück) – mehr Komplexität, nicht empfohlen.
- **Erwarteter Effekt**: Pflege; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Gering; Wortlaut-Regressionen werden von `test_editor_help` nicht erfasst, daher beim Zusammenführen Text exakt belassen.

### EDT-612 – `scopes()` ist ein prozessweiter Vektor, `find()` nutzt aber `thread_local` für den Puffer
- **ID**: EDT-612
- **Kategorie**: E / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/EditorHelp.cpp:9620-9624`, `:9667`
- **Problem**: Der Kommentar bei `thread_local std::string scoped` begründet, dass ein Lookup "von anderswo als dem UI-Thread" den Puffer nicht überschreiben soll. Der Scope-Stack, den derselbe Aufruf liest, ist aber ein gewöhnliches `static std::vector` ohne Synchronisation: ein Lookup aus einem Nicht-UI-Thread (z. B. MCP-Bridge, Report-Dialog-Worker) während der UI-Thread `Scope` pusht/poppt wäre ein Data Race. Die Absicht des Kommentars wird so nur halb erfüllt.
- **Code-Auszug**:
```cpp
std::vector<const char*>& scopes()
{
    static std::vector<const char*> s;
    return s;
}
```
- **Lösung**: Entweder `static thread_local` (jeder Thread hat seinen Stack, Lookups ohne Scope fallen auf unscoped zurück – bisheriges Verhalten im UI-Thread unverändert) oder den Kommentar anpassen ("UI-Thread only", wie `EditorPanelState.h` es schreibt) und das `thread_local` am Puffer streichen. Ein Gegencheck, ob irgendwo außerhalb des UI-Threads `Help::find` aufgerufen wird, steht aus (311 Aufrufstellen in `src/HE_Editor`, nicht einzeln geprüft).
- **Erwarteter Effekt**: Konsistenz/Klarheit; kein Performance-Effekt (`thread_local` bei `scopes()` kostet einen TLS-Zugriff pro Lookup).
- **Risiko / Verifikation**: `thread_local` verändert Verhalten, wenn ein Hintergrund-Thread bisher absichtlich die Scopes des UI-Threads gelesen hat (unwahrscheinlich). Test: vorhandene UI-Tests (`test_*_ui.cpp`).

## Cross-Modul-Hinweise
- `src/HE_Scene/include/HorizonScene/SceneSerializer.h:135-136`: `serializeEntityComponents` hat keinen Komponentenfilter; für EDT-601 wäre eine Variante `(world, entity, span<const char*> componentKeys)` die eigentliche Lösung (hier außerhalb des Slice).
- `src/HE_Editor/InspectorPanel.cpp:~232-233` (Mehrfachauswahl): `const std::vector<std::string> common = s_multi.common;` und `partial` werden pro Frame kopiert, obwohl der Cache nur über `refreshMultiSelection` verändert wird und `renderFor` ihn nicht anfasst (eine Referenz würde reichen, muss aber gegen Refresh während der Iteration abgesichert werden – `s_multi.stale = true` wird erst nach der Schleife wirksam).
- `src/HE_Editor/ViewportPanel.cpp:1688-1712` (Marquee-Rahmen): pro Frame ein neues `std::unordered_set<uint32_t> seen` und `std::vector<Entity> found`, `meshBounds`-Lookup je Objekt; `EditorMarquee::encloses` (EditorMarquee.cpp:27-48) multipliziert pro Ecke `model` und dann `viewProj` (zwei Matrix-Vektor-Produkte). Vorab `mvp = viewProj * model` wäre ein Produkt je Ecke, ändert aber Float-Rundung minimal (Randfälle im Rahmen); nicht ohne Messung. Allokationen per `reserve` bzw. wiederverwendeten statischen Puffer vermeiden.
- `src/HE_Editor/EditorPanelState.h`: `AssetPanelState::operator[]/find` nehmen `const std::string&`; Aufrufer mit `const char*`/`string_view` bauen pro Frame ein temporäres `std::string`. Heterogener Lookup (`std::map<std::string, T, std::less<>>`) würde das vermeiden – Panels in anderen Slices prüfen.

## Unsicher / Messung nötig
- EDT-601: tatsächliche Kosten von `state()` je Entity (Serialisierung + CBOR→JSON) und Konstruktionskosten von `SceneSerializer`; Frame-Zeit beim Drag mit 200/1000 Members.
- EDT-603: Kosten von `SDL_GetTouchDevices` pro Aufruf unter Windows (Win32-Pfad evtl. mit Enumeration).
- EDT-607: Index-Aufbau und Relokations-Startkosten von `kEntries` (Startup-Profil, erster Hover-Frame).
- EDT-612: Ob `Help::find`/`Scope` jemals außerhalb des UI-Threads aufgerufen wird.

## Gelesen
Ja, vollständig gelesen: `EditorHelp.h`, `EditorInput.h/.cpp`, `EditorInputMac.mm`, `EditorMarquee.h/.cpp`, `EditorMultiEdit.h/.cpp`, `EditorPanelState.h/.cpp`, `EditorReference.h/.cpp` sowie der Code-Teil von `EditorHelp.cpp` (Zeilen 1-40 und ab 9372 bis Ende, d. h. Lookup, Scope, Area, PanelTopic). Von den Dateneinträgen von `EditorHelp.cpp` habe ich Zeilen 1-1960 gelesen; Zeilen 1960-9372 (reine `Entry`/`Area`-Datenzeilen) habe ich nur skriptgestützt ausgewertet (Eindeutigkeit der Keys, Area-Abdeckung, Duplikat-Bodies, Titel mit "##", Textlängen, tote Präfixe), nicht Zeile für Zeile gelesen.
