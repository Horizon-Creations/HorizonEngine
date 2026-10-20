# Editor Slice 7 – EditorRewards … EditorTransformGizmo (src/HE_Editor)

Dateien: EditorRewards.{h,cpp}, EditorRowWidgets.cpp, EditorSelection.{h,cpp}, EditorSelfUpdate.{h,cpp}, EditorSettingsCatalog.{h,cpp}, EditorSettingsPanel.{h,cpp}, EditorShortcuts.{h,cpp}, EditorSystemMotion.{h,cpp,mm}, EditorTabs.{h,cpp}, EditorTheme.{h,cpp}, EditorToolbar.{h,cpp}, EditorTransformGizmo.{h,cpp}.

## Zusammenfassung

Der Slice ist insgesamt sauber und gut kommentiert: die offensichtlichen Hot-Path-Fallen sind schon behandelt (Hover-Gate vor der Help-Suche in `EditorRowWidgets`, `favoritesContain` ohne Allokation, Cache der Reduce-Motion-Abfrage, einmaliger `capturePre()` pro Drag). Der einzige echte Skalierungsfehler ist die Selektionslogik (`EditorSelection::roots/contains/addMany`, O(N²) und `roots()` läuft pro Frame im Viewport). Danach folgen die Doppelpflege Settings-Katalog gegen Settings-Panel (Wertebereiche sind bereits gedriftet, Restore Defaults ist eine handgepflegte, lückenhafte Liste), der Gruppen-Gizmo mit pro-Frame-Aufbau über alle Mitglieder, der Footer-Status mit Strings/Vektoren pro Frame und die Shortcut-Lookups über String-IDs. Der Rest sind Kleinigkeiten. Funde: 1 Hoch, 5 Mittel, 6 Niedrig. Messdaten liegen nicht vor, alle Zahlen sind Schätzungen.

---

### EDT-701 – Selektion: `roots()`/`addMany()` sind O(N²), `roots()` läuft pro Frame
- **ID**: EDT-701
- **Kategorie**: D
- **Schweregrad**: Hoch (bei großen Selektionen, z. B. Select All; sonst unauffällig)
- **Typ**: Performance
- **Ort**: `EditorSelection.cpp:75-97` (roots), `:99-102` (contains), `:55-63` (addMany), `:48-53` (setMany). Aufrufer pro Frame: `ViewportPanel.cpp:1535` (`ctx.selection.roots(...)` vor dem Gizmo), `EditorApplication.cpp:12011` (`prune`, pro Frame in `makeContext`).
- **Problem**: `contains()` ist eine lineare Suche über `m_entities`. `roots()` ruft sie für jeden Vorfahren jedes Mitglieds auf (`if (contains(p))`), also O(N · Tiefe · N). Der Viewport ruft `roots()` jeden Frame auf, sobald etwas selektiert ist, und danach noch `erase(remove_if(...))` auf einem frisch allokierten Vektor. Edit ▸ Select All (`EditorUI.cpp:1845`, `ViewportActions::selectAll`) erzeugt Selektionen mit allen Entities. Bei 5 000 Entities mit Hierarchietiefe ~3 sind das größenordnungsmäßig 50–75 Mio. Vergleiche pro Frame (Schätzung, messen). `addMany()` ruft `contains(e)` je Element und ist ebenfalls O(N²), dort aber nur einmalig pro Geste (Hitch beim Select All / Shift-Range).
- **Code-Auszug**:
```cpp
for (Entity e : m_entities) {
    ...
    for (int steps = 0; steps < kMaxDepth; ++steps) {
        ...
        p = h->parent;
        if (contains(p)) { covered = true; break; }   // linear in N
    }
```
```cpp
for (Entity e : range) {
    if (e == entt::null || contains(e)) continue;     // linear in N
    m_entities.push_back(e);
}
```
- **Lösung**:
  1. In `roots()` einmal pro Aufruf ein Lookup bauen und dort fragen: `std::unordered_set<Entity> sel(m_entities.begin(), m_entities.end());` (oder bei kleinem N weiter linear: `if (m_entities.size() <= 16)` Schnellpfad), dann `sel.count(p)`. Komplexität O(N · Tiefe).
  2. `addMany`/`setMany` genauso: lokales Set der vorhandenen Mitglieder, nur neue anhängen. `setMany` braucht das `++m_revision` danach nicht mehr doppelt (der Kommentar "harmless" deutet es an); eine Erhöhung genügt.
  3. Optional: das Set als Member halten und in allen Mutatoren pflegen, dann ist auch `contains()` (vom Outliner pro sichtbarer Zeile genutzt) O(1). Das ist invasiver; Schritt 1+2 reichen.
  4. Optional: das Ergebnis von `roots()` im Viewport pro Selektions-Revision cachen; Vorsicht, Hierarchie-Änderungen ändern `m_revision` nicht, der Cache müsste also auch auf Reparenting reagieren. Ohne Messung nicht empfohlen.
- **Erwarteter Effekt**: Schätzung: Selektion mit einigen tausend Einträgen von zweistelligen Millisekunden pro Frame auf unter 1 ms; kleine Selektionen unverändert.
- **Risiko / Verifikation**: Reihenfolge der Ausgabe (Selektionsreihenfolge) und die Zyklus-Kappung `kMaxDepth` müssen erhalten bleiben. `tests/test_editor_selection.cpp` deckt `roots` ("leaves out a member whose ancestor is selected") und Ranges ab. Ergänzen: Test mit 5 000 Mitgliedern und tiefer Kette plus ein Zyklus-Fall; Revisionszähler-Erwartung für `setMany` prüfen (Tests dürfen nicht auf den doppelten Increment bauen).

---

### EDT-702 – Gruppen-Gizmo: pro Frame O(N·Tiefe) Neuaufbau, pro Drag-Frame N Inversen
- **ID**: EDT-702
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `EditorTransformGizmo.cpp:223-261` (Neuaufbau ohne Drag), `:302-316` (Anwenden pro Member), `:339-361` (`movable`-Vektor), `:71-78` (`parentWorldOf`).
- **Problem**: Solange eine Mehrfachselektion existiert und nicht gezogen wird, läuft jeden Frame für jedes Mitglied `HE::worldMatrixOf(world, e)` (komponiert die Hierarchie bis zur Wurzel, laut Kommentar bewusst nicht aus dem Cache gelesen) nur für den Zentroid. Dazu kommt in `manipulate(...)` pro Frame ein neuer `movable`-Vektor, und im ViewportPanel noch `roots()` plus ein weiterer Vektor. Während des Drags wird für jedes Mitglied jeden Frame `parentWorldOf` (wieder eine Hierarchiekomposition) berechnet und `glm::inverse` (allgemeine 4x4-Inverse) darauf angewendet, obwohl sich die Eltern der Wurzel-Mitglieder während des Drags nicht ändern (Mitglieder, deren Vorfahre selektiert ist, sind per `roots()` ausgeschlossen).
- **Code-Auszug**:
```cpp
for (const Entity e : members)
    if (registry.all_of<TransformComponent>(e)) {
        primaryWorld = HE::worldMatrixOf(world, e);   // jeden Frame, N mal
        centroid += glm::vec3(primaryWorld[3]);
```
```cpp
const glm::mat4 local = glm::inverse(parentWorldOf(world, m.entity)) * worldNow;   // jeden Drag-Frame, N mal
```
- **Lösung**:
  1. `GroupMember` um `glm::mat4 invParentStart` erweitern, beim Drag-Start (Zeile 272-278) einmal füllen; im Drag-Loop nur noch multiplizieren. Spart N Hierarchiekompositionen und N Inversen pro Frame (Verhalten identisch, solange kein Elternteil während des Drags von außen bewegt wird; ein Peer-Edit mitten im Drag wäre der einzige Unterschied, vorher wurde er "live" nachgeführt).
  2. Den Zentroid im Nicht-Drag-Fall nur neu berechnen, wenn sich Selektion oder eine Transform geändert hat (Revision der Selektion + Dirty-Flag der Mitglieder). Ohne Messung zunächst nur Schritt 1.
  3. `movable` als wiederverwendeten `static thread_local std::vector<Entity>` oder Member halten statt pro Frame zu allokieren (Niedrig).
- **Erwarteter Effekt**: Schätzung: bei 1 000 Mitgliedern Drag-Frame von mehreren ms auf unter 1 ms. Nicht-Drag-Frames: nur bei großen Gruppen messbar.
- **Risiko / Verifikation**: Das Verhalten bei sich während des Drags ändernden Elternmatrizen (Collab, Skripte) ändert sich; entscheiden, ob das gewollt ist. Tests: `tests/test_viewport_gizmo.cpp`, `tests/test_gizmo_pick.cpp` (Gruppen-Drag mit verschachtelter Hierarchie ergänzen und gegen die bisherige Ausgabe vergleichen).

---

### EDT-703 – Settings-Katalog und Settings-Panel sind zwei Wahrheiten; Wertebereiche sind bereits gedriftet
- **ID**: EDT-703
- **Kategorie**: A (Duplikation) / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `EditorSettingsCatalog.cpp:66-463` gegen `EditorSettingsPanel.cpp:303-949`; Behauptung im Header `EditorSettingsCatalog.h:10-11` und `McpToolRegistry.h:1370-1371` ("the ranges the widgets enforce by construction").
- **Problem**: Jede Einstellung ist doppelt beschrieben (Katalog für MCP, Panel für die UI) und ein drittes Mal in `EditorHelp.cpp` plus die Hilfetexte im Katalog. Die Bereiche widersprechen sich bereits: die MCP-Seite lässt Werte zu, die die UI-Slider nie erlauben, und die Doku behauptet das Gegenteil. Beispiele (Katalog / Panel): Render Scale 0.25–2.0 (`:354-356`) / 0.5–2.0 (`Panel:383`), Bloom Threshold 0–10 / 0–4 (`:365`, `Panel:416`), AO Radius 0.01–10 / 0.05–2 (`:373`, `:425`), AO Intensity 0–1 / 0–2 (`:376`, `:426`), DoF Focus Distance 0.1–1000 / 0.1–200 (`:385`, `:438`), Focus Range 0–1000 / 0–100, Camera Speed 0.1–100 / 1–50 (`:86`, `Panel:868`), Stick/Trigger Deadzone 0–0.9 / 0–0.5 (`:96-101`, `Panel:901-902`), SSR Intensity 0–2 / 0–1 (`:407`, `Panel:475`), GI Light Radius 0–10 / 0.05–3 (`:426`, `Panel:499`), GI Indirect Intensity 0–5 / 0–3 (`:423`, `Panel:498`), Content-Browser-Refresh 1–600 / ≥0 (`:108`, `Panel:948`). Auch inhaltlich: Katalog "Keep CPU Assets … saves a re-read" (`:104-107`) gegen Panel "Not implemented — CPU asset data is always kept" (`Panel:935-944`), Render Scale ohne Metal-Hinweis im Katalog.
- **Code-Auszug**:
```cpp
// Katalog
t.push_back(floatRow("postProcess.bloomThreshold", "Bloom Threshold", ..., 0.0, 10.0, ...));
// Panel
Row::sliderFloat("Bloom Threshold", &cfg.BloomThreshold, 0.0f, 4.0f, "%.2f");
```
- **Lösung**:
  1. Katalog als einzige Quelle der Bereiche/Labels/Hilfetexte behandeln: im Panel eine kleine Hilfe `const SettingDesc& S(const char* key)` und `Row::sliderFloat(S("postProcess.bloomThreshold").label, &cfg.BloomThreshold, S(...).minValue, S(...).maxValue, "%.2f")`. Dadurch verschwinden die Literale im Panel; Zeilen mit Backend-Gates bleiben im Panel.
  2. Alternativ (kleiner): nur einen Test ergänzen, der alle Katalog-Bereiche gegen die im Panel verwendeten prüft (z. B. über einen Test-Hook, der die Slider-Aufrufe aufzeichnet).
  3. Vorher entscheiden, welche Seite recht hat (die Panel-Werte sind die geprüften UI-Grenzen, die Katalog-Werte sind vermutlich "großzügig geraten").
- **Erwarteter Effekt**: keine Laufzeit; weniger Pflegeaufwand und ein ehrlicher MCP-Vertrag. Löst das Muster aus `EditorRewards.h:383-390` ("in six places") zu einem Teil.
- **Risiko / Verifikation**: Strengere MCP-Bereiche brechen bestehende Clients, die Werte jenseits der UI-Grenzen setzen (`tests/test_mcp_tools_settings.cpp` "out-of-range number is refused" prüfen). Hilfetexte stehen im Audit-Skript `scripts/editor_help_audit.py --check`; Label-Änderungen müssen dort bestehen.

---

### EDT-704 – "Restore Defaults" ist eine handgepflegte, lückenhafte Kopie der Defaults
- **ID**: EDT-704
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `EditorSettingsPanel.cpp:2405-2480`
- **Problem**: ~45 Zuweisungen mit hartkodierten Werten (`UiFontScale = 1.0f`, `EditorCameraSpeed = 6.0f`, `CollabMaxAssetMB = 64`, …), die `EditorConfig.h` spiegeln und bei jedem neuen Feld nachgezogen werden müssen. Der Button steht auf jeder Settings-Seite (auch Viewport Quality, Editor Renderer), setzt aber u. a. `RenderPath`, `OcclusionCulling`, `AntiAliasing`, `AASharpness`, `RenderScale`, `SpecularAA*`, DoF, Motion Blur, SSR, GI/GI-Refl, Gamepad-Deadzones, `PointerInput`, `MaxFps` nicht zurück, obwohl diese auf den Seiten stehen, auf denen der Button erscheint. Ob das Absicht ist, ist nicht dokumentiert (unsicher). Zusätzlich: `EditorConfig.h` ist die eigentliche Default-Quelle (z. B. `AntiAliasing = 1`, `RenderScale = 1.0f`).
- **Code-Auszug**:
```cpp
cfg.UiFontScale       = 1.0f;
cfg.EditorCameraSpeed = 6.0f;
...
cfg.BloomThreshold    = 1.0f;
```
- **Lösung**: Defaults aus einer frischen `EditorConfig const defaults{};` lesen und über den Katalog (`SettingDesc::pb/pi/pf/ps`, Zeilen mit `row == seitenzeilen`) zurücksetzen: `for (d : editorSettingCatalog()) if (d.storage==Config && d.writable && onThisPage(d)) copyMember(cfg, defaults, d);`. Sonderfälle (`CollabSyncLargeAssets` bei gesperrter Session, `McpServerEnabled=false`, `CollabRelayAddress.clear()`, Kamera-Flugspeed anwenden) bleiben als wenige explizite Zeilen. Das beseitigt auch die Doppelpflege mit `EditorConfig.h`.
- **Erwarteter Effekt**: keine Laufzeit; ~50 Zeilen weniger, kein Driften mehr, und die Seiten verhalten sich konsistent.
- **Risiko / Verifikation**: Beobachtbares Verhalten ändert sich (mehr Felder werden zurückgesetzt). Daher Produktentscheidung nötig, ob nur die sichtbare Seite oder alles zurückgesetzt wird. Absicherung: neuer UI-/Logiktest "Restore Defaults auf Seite X setzt exakt die Felder der Seite X auf `EditorConfig{}`", plus die Sonderfälle (Session-Sperre, MCP aus).

---

### EDT-705 – Footer-Status: Strings, Vektoren und Datumsparsing in jedem Frame
- **ID**: EDT-705
- **Kategorie**: C
- **Schweregrad**: Mittel (absolut klein, aber garantiert pro Frame; messen)
- **Typ**: Performance
- **Ort**: `EditorRewards.cpp:1450-1470` (`composeIdle`), `:1610-1724` (`drawFooterStatus`), `:696-707` (`progressOf` → `dayBefore` `:510-521` → `parseDay` `:490-500`), `:220-232` (`Feed::look` → `lineFor`), `:1424-1431` (`todayCached`). Aufruf `EditorUI.cpp:3601`.
- **Problem**: Der Editor zeichnet jeden Frame (siehe `EditorRewards.h:181-183`). Pro Frame entstehen: ein `IdleText` mit `std::vector<Piece>` aus bis zu 7 `std::string`s plus `whole` (Konkatenation, `std::to_string` für beide Zähler), bei zu schmalem Footer ein zweites `composeIdle`, 3–5 `CalcTextSize` über diese Strings, `progressOf(...)` mit `dayBefore(today)` (`substr`/`stoi`/`snprintf`, ein String), und `Feed::look` erzeugt `lk.line` per `lineFor` (bei "Committed and pushed" Heap, sonst SSO). Das passiert auch, wenn Rewards deaktiviert sind (`composeIdle` läuft trotzdem für "Ready"). Der Text ändert sich aber nur bei Änderung von Zählerstand, Tag, Idle-Text oder Footerbreite.
- **Code-Auszug**:
```cpp
IdleText idle = composeIdle(idleTextIn, showCounters ? pr : Progress{}, ...);
if (idle.counters && ImGui::CalcTextSize(idle.whole.c_str()).x > winW / 3.0f)
    idle = composeIdle(idleTextIn, Progress{}, plain[0], plain[1]);
```
- **Lösung**:
  1. `todayCached()` zusätzlich `dayBefore(day)` mitcachen (ein-Sekunden-Cache existiert schon), `progressOf` bekommt `yesterday` übergeben.
  2. `IdleText` als `static` zwischenspeichern mit Schlüssel `(idleTextIn-Zeiger/Inhalt, p.any, builds.shown, streak.shown, showCounters)`; neu bauen nur bei Schlüsseländerung; `idleW` und der Überlauf-Test (`> winW/3`) nur bei Schlüssel- oder Breitenänderung neu messen.
  3. `Feed::look` liefert `const std::string&`/`std::string_view` auf einen im Feed gehaltenen `m_line`-Cache, der bei `take()` gesetzt wird, statt pro Aufruf zu bauen.
- **Erwarteter Effekt**: Schätzung: ~10–15 Heap-Allokationen und einige `CalcTextSize` pro Frame weniger; absolut wenige µs. Wert liegt eher in der Disziplin "idle Frame allokiert nichts".
- **Risiko / Verifikation**: Cache-Invalidierung bei Schriftgröße/DPI-Änderung (`CalcTextSize`-Ergebnis) und beim Sprachwechsel des Idle-Texts. `tests/test_editor_rewards.cpp` (ImGui-freie Kern-Funktionen) bleibt gültig; für die Pixelausgabe ein UI-Frame-Dump mit `HE_UI_DUMP_DIR` vergleichen.

---

### EDT-706 – Shortcut-Lookup über String-IDs: doppelte lineare Suche + Heap pro Aufruf; `conflicts()` O(n²) pro Frame
- **ID**: EDT-706
- **Kategorie**: D
- **Schweregrad**: Mittel (Niedrig für die reinen `pressed()`-Kosten, Mittel wegen der Shortcuts-Seite)
- **Typ**: Performance
- **Ort**: `EditorShortcuts.cpp:134-139` (`find`), `:141-147` (`chord`), `:161-195` (`pressed`), `:206-220` (`conflicts`); Aufrufer: 32 `pressed`-Stellen und 45 `label`-Stellen im Editor, `ShortcutsPage.cpp:147` (`conflicts(a.id)` pro Zeile pro Frame).
- **Problem**: `pressed(id)` ruft `find(id)` (lineare Suche über 31 Einträge, je `string_view`-Konstruktion über `strlen`) und danach `chord(id)`, das `std::string(id)` für die `unordered_map` baut (Heap bei IDs > 15 Zeichen, z. B. `"viewport.snapToGround"`, `"file.saveSceneAs"`) und intern erneut `find(id)` ausführt. Alle ~32 Shortcuts werden jeden Frame abgefragt. `conflicts(id)` ruft für jede Tabellenzeile erneut `chord(other.id)` auf; die Shortcuts-Seite macht das für jede der 31 Zeilen in jedem Frame: ~1 000 `find` + Hash + Allokationen pro Frame, solange die Seite offen ist. `chordToText`/`chordFromText` (`:222-251`) laufen dagegen nur beim Laden/Speichern (150 Tasten × `GetKeyName` pro Chord, okay).
- **Code-Auszug**:
```cpp
ImGuiKeyChord chord(std::string_view id)
{
    const auto& ov = overrides();
    if (const auto it = ov.find(std::string(id)); it != ov.end()) return it->second;
    const Action* a = find(id);
    return a ? a->defaultChord : ImGuiKey_None;
}
```
- **Lösung**:
  1. `pressed(const Action&)`/`chord(const Action&)`-Overloads; die zentrale Form arbeitet auf dem Index in der Tabelle. Aufrufer lösen die ID einmal auf (`static const Action* kSave = EditorShortcuts::find("file.save");`) oder nutzen eine `enum class Id`.
  2. Effektive Chords in einem `std::array<ImGuiKeyChord, N>` parallel zur Tabelle halten (bei `setChord/reset/decode` aktualisieren); `overrides()` wird zur Persistenzliste. Dann ist `chord()` ein Array-Zugriff.
  3. `conflicts()` auf dem Array: O(n) pro Aufruf ohne Allokation; alternativ die Konflikte der Seite einmal pro Revision (Änderung der Overrides) berechnen.
  4. Mindestens: `unordered_map` mit transparentem Hash (`std::string_view`-Lookup), um `std::string(id)` zu sparen.
- **Erwarteter Effekt**: Schätzung: `pressed()`-Summe pro Frame von einigen µs auf < 1 µs; Shortcuts-Seite pro Frame von ~1 000 Lookups auf ~31.
- **Risiko / Verifikation**: Die API (`string_view`-IDs, die Strings in Configs/Menüs) bleibt kompatibel, wenn die Overloads nur ergänzt werden. `tests/test_editor_shortcuts.cpp` und `tests/test_shortcuts_ui.cpp` prüfen Rebinding, Konflikte, Encode/Decode.

---

### EDT-707 – Toolbar-Zelle: Help-Lookup und Textmessung pro Zelle und Frame, drei Kopien der Zellen-Layoutlogik
- **ID**: EDT-707
- **Kategorie**: D / A
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `EditorToolbar.cpp:47-97` (`cellImpl`, Zeile 56 `findKey`, Zeile 86 `CalcTextSize`), `:37-41` (`cellWidth` misst dasselbe Label bereits), `:352-392` (`Bar::item`, `itemTinted`, `readout` mit identischem Cursor-/Breiten-/Zentrier-Code).
- **Problem**: Jede Zelle mit `helpKey` macht pro Frame `Help::findKey(helpKey)` (die Tabelle in `EditorHelp.cpp:9634` ist groß; Suchkosten unbekannt, siehe "Unsicher"), obwohl das Ergebnis konstant ist. Das Label wird pro Zelle zweimal gemessen: in `Bar::item → cellWidth` und in `cellImpl` (`labelW`). `item`/`itemTinted`/`readout` wiederholen den Block `w = cellWidth; if(!m_first) m_cursor += kSegGap; x = ...; m_cursor += w; m_first=false;` und die Mittellinie-Berechnung aus `cellImpl`.
- **Code-Auszug**:
```cpp
const HE::Ed::Help::Entry* help = helpKey ? HE::Ed::Help::findKey(helpKey) : nullptr;
...
const float labelW   = label ? ImGui::CalcTextSize(label).x : 0.0f;
```
- **Lösung**: (1) `Bar::advance(label) → {x, w}` als private Hilfe für alle drei Zellenarten. (2) `cellImpl` bekommt die bereits berechnete Labelbreite (oder `w` und `labelW` aus `Bar`) übergeben. (3) `findKey` nur auslagern, wenn die Messung zeigt, dass es relevant ist (z. B. `static` Zeiger-Cache pro Aufrufstelle, aber `helpKey` ist ein Zeiger auf Literale; ein `unordered_map<const char*, Entry*>` könnte genügen).
- **Erwarteter Effekt**: Schätzung: ~20–40 Zellen pro Frame; Gewinn im niedrigen µs-Bereich; Hauptnutzen ist weniger Code.
- **Risiko / Verifikation**: Pixelgenaue Layoutänderungen ausschließen (Rundung `std::floor` in der gleichen Reihenfolge lassen); UI-Tests, die die Toolbars rendern (`tests/test_*_ui.cpp`) vor/nach per `HE_UI_DUMP_DIR` vergleichen.

---

### EDT-708 – `Row::`-Helfer: Feldschlüssel-String pro Zeile und Frame bei Mehrfachselektion
- **ID**: EDT-708
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `EditorRowWidgets.cpp:205-218` (`fieldKey`), `:429-444` (`MixedScope`, `mixedMask`), `:238` + `:301`-`:338` (jede Zeile ruft `mixedMask` zwei- bis dreimal), `:377-383` (`comboZ` baut Vektor).
- **Problem**: `mixedMask(label)` springt bei fehlendem Scope billig zurück, sobald aber ein `MixedScope` mit Feldern aktiv ist (Details-Panel bei Mehrfachselektion), baut jeder Aufruf `fieldKey(label)` (ein `std::string`, Zeichen für Zeichen mit `tolower`) und sucht in einer `unordered_map`. Die Wrapper (`sliderFloat` usw.) rufen `mixedMask` einmal selbst und `row()` einmal; `dragFloat3` sogar zweimal. Der `MixedScope`-Konstruktor baut zusätzlich pro Feld pro Frame einen Schlüssel. `comboZ` allokiert im gemischten Fall pro Frame einen `std::vector`.
- **Code-Auszug**:
```cpp
const unsigned m = mixedMask(label);
return row(label, [&]{ return axisDragN("##v", v, 3, speed, min, max, fmt, m); });
```
- **Lösung**: `mixedMask` einmal in `row()` auswerten und in den `body` durchreichen (z. B. `row(label, [&](unsigned mixed){...})`); `fieldKey` auf `thread_local std::string` ohne Neuallokation umstellen oder einen Hash über die normalisierten Zeichen berechnen (Schlüssel `uint64_t`) statt Strings in der Map.
- **Erwarteter Effekt**: Schätzung: bei Mehrfachselektion ~100+ Kleinallokationen pro Frame weniger; Einzelselektion unverändert.
- **Risiko / Verifikation**: `fieldKey`-Normalisierung (Teil vor `##`, nur alnum, lowercase) muss identisch bleiben (Hash-Kollisionen vermeiden, Schlüssel 64 Bit). Tests: Details-Panel-/Multi-Edit-UI-Tests vorhanden (`tests/test_*_ui.cpp`), Mixed-Anzeige "—" und "(mixed)" prüfen.

---

### EDT-709 – Ton-Synthese: Duplikate, Neuberechnung in `previewDragCues`, Erstberechnung im Save-Frame
- **ID**: EDT-709
- **Kategorie**: A / D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `EditorRewards.cpp:824-848` (`importPopPcm16`) gegen `:951-964` (`glide`); `:1000-1024` (`chimePcm16`) gegen `:745-759` (`toPcm16`); `:1221-1245` (`previewDragCues`) gegen `:1137-1147` (`playDragCue`-Cache); `:1122-1135` (`playTone` baut PCM beim ersten Abspielen).
- **Problem**: `importPopPcm16` hat die Gleit-Synthese ein zweites Mal von Hand geschrieben (nur mit `min(t, kGlideSec)`); `chimePcm16` normalisiert nicht über `toPcm16` (bewusst, aber als eigener Schleifenkörper). `previewDragCues` synthetisiert alle 5 Cues bei jedem Klick neu und mixt sie in einen neuen Puffer, obwohl `playDragCue` dieselben Cues cached. `playTone` erzeugt beim ersten Abspielen eines Tons die Samples synchron im Frame der Aktion (z. B. Chime: ~18 500 Samples × 2 Noten mit `sin`+`exp`; Schätzung 1–3 ms), obwohl die Header-Doku (`EditorRewards.h:313-318`) verspricht, dass die Rückmeldung "nie die Aktion verzögert" (dort bezogen auf das Öffnen des Geräts).
- **Code-Auszug**:
```cpp
if (pcm[i].empty()) pcm[i] = tonePcm16(t, kRate);   // im Frame des Speicherns
```
- **Lösung**: `importPopPcm16` über `glide(...)` ausdrücken (mit angepasster Gleitdauer-Parameter); `previewDragCues` aus dem Cache der Cues bedienen; Töne in `keepUiAudio` (läuft schon im eigenen Frame, wenn der Ton "möglich" wird) vorab erzeugen, sobald `uiSoundPossible` und das Gerät offen ist.
- **Erwarteter Effekt**: Schätzung: einmalig 1–3 ms weniger beim ersten Save/Build-Ton; 4 Duplikat-Schleifen weniger.
- **Risiko / Verifikation**: Ausgabe muss bitidentisch bleiben (`tests/test_editor_rewards.cpp` pinnt Eigenschaften der Töne; zusätzlich Hash der PCM-Bytes vor/nach Refactoring vergleichen). Glide-Parameter von `importPop` nicht ändern.

---

### EDT-710 – Rewards: unbegrenzt wachsende Tab-Liste, `normalPath` pro Content-Browser-Kachel
- **ID**: EDT-710
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `EditorRewards.cpp:377-402` (`SaveMarks::update`, `m_tabs` wird nie bereinigt), `:1341-1350` (`importHighlight`), `:404-407` (`normalPath`), Aufrufer `ContentBrowserPanel.cpp:1948`.
- **Problem**: `SaveMarks::m_tabs` bekommt für jeden je gezeichneten Tab-Schlüssel einen Eintrag (String + Zustand) und löscht nie; bei vielen geöffneten/geschlossenen Assets wächst die Liste, und die lineare Suche mit String-Vergleich läuft pro Tab und Frame (`EditorUI.cpp:3774`). `importHighlight` ruft, solange Marken existieren (bis 30 s nach einem Import), pro sichtbarer Kachel und Frame `normalPath(fullPath)` (`std::filesystem::path`, `lexically_normal`, `generic_string`: mehrere Allokationen).
- **Code-Auszug**:
```cpp
return s_fresh.strength(normalPath(fullPath), now);   // pro Kachel und Frame
```
- **Lösung**: `SaveMarks::update` durch einen `endFrame(openKeys)`-Schritt ergänzen, der nicht mehr vorhandene Tabs entfernt (oder Einträge ohne Aktivität nach X Sekunden); `importHighlight` zuerst billig prüfen, ob der Dateiname (`filename`) irgendeiner Marke passt, bevor normalisiert wird, oder die Marken beim Import zusätzlich über den rohen Pfad indizieren.
- **Erwarteter Effekt**: Schätzung: vernachlässigbar im Normalfall; begrenzt Wachstum und verhindert ein Hotspot-Szenario mit 1 000 sichtbaren Kacheln nach einem Import.
- **Risiko / Verifikation**: Marken-Zeitfenster (`kImportWindowSec`) und Check-Mark-Dauer (`kTabCheckSec`) nicht verändern. `tests/test_editor_rewards.cpp` deckt `FreshImports`/`SaveMarks` ab; Test für "Tab geschlossen → Eintrag entfernt, neu geöffneter Tab zeigt keinen alten Haken" ergänzen.

---

### EDT-711 – Settings-Panel Kleinkram: `toggleFavorite`, `prefScopeFor`, Status-Seite
- **ID**: EDT-711
- **Kategorie**: A / C
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `EditorSettingsPanel.cpp:84-100` (`toggleFavorite`), `:164-171` (`prefScopeFor`), `:1667-1696` + `:1746-1755` (`statusRow` mit `std::function`-Temporären), `:303-1090` (Row-Lambdas).
- **Problem**: (a) `toggleFavorite` parst die Favoritenliste mit `std::stringstream`/`getline`, obwohl `QuickSettingsFavorites.h` bereits einen allokationsfreien Parser für dieselbe Liste hat; zwei Implementierungen derselben Grammatik (leere Tokens, Doppelkommas). (b) `prefScopeFor` baut pro sichtbarer Zeile und Frame ein temporäres `std::string` (`cache.find(cat)` mit `const char*` auf `std::map<std::string,…>`; für "Global Illumination" (19 Zeichen) Heap) – dabei ist die Kategorie ein Literal. (c) Die Status-Seite baut pro Frame ~15 `std::string`-Details und pro Fix-Zeile ein `std::function`; die Seite ist selten offen.
- **Code-Auszug**:
```cpp
static std::map<std::string, std::string> cache;
auto it = cache.find(cat);          // temporärer std::string pro Aufruf
```
- **Lösung**: (a) `toggleFavorite` in `QuickSettingsFavorites.h` neben `favoritesContain` (mit `string_view`-Tokenizer). (b) Cache als `std::unordered_map<const char*, std::string>` (Zeiger der Literale als Schlüssel) oder `std::map<std::string, std::string, std::less<>>` mit transparenter Suche, oder die Zeilen übergeben direkt den Scope-String. (c) nur bei Messung anfassen.
- **Erwarteter Effekt**: Schätzung: wenige Allokationen pro Frame bei geöffneter Preferences-Seite; Hauptgewinn weniger Doppelcode.
- **Risiko / Verifikation**: Reihenfolge und Format von `QuickSettingsFavorites` bleiben (config.json-Kompatibilität). Vorhandener Test für `favoritesContain` um `toggle` erweitern (leere Tokens, Duplikate).

---

### EDT-712 – Kleinere Beobachtungen (kein eigener Fund)
- **Ort / Problem / Lösung**: kurz gebündelt, alle Niedrig/Vereinfachung:
  - `EditorSelfUpdate.cpp:82` `Session::set(const std::function<void(Snapshot&)>&)` erzeugt für jede Statusänderung ein `std::function`; kalt, kann ein Template sein. Kein Handlungsbedarf.
  - `EditorTabs.cpp:22-43` `sameAsset` macht pro Vergleich zweimal `fs::exists` + `fs::equivalent` (Syscalls), und `find()` ruft das für jeden Tab (`:51-54`); nur beim Öffnen von Tabs, nicht im Frame. Wenn `openOrFocus` einmal pro Frame aus einer Schleife aufgerufen würde (nicht gefunden), O(Tabs) Dateisystemzugriffe; derzeit nur Aktionen.
  - `EditorRewards.cpp:1049-1059` `tallyMoment` schreibt bei jedem Build/Commit synchron `config.json` im UI-Thread (im Frame der Aktion); selten, aber Disk-I/O auf dem Edge, den das Header-Versprechen "never delays the action" betrifft.
  - `EditorRewards.h:1-430`: ~430 Zeilen Designdokumentation im Header; wirkt sich nicht auf Kompilierzeit aus, gehört aber nach `docs/` (laut CLAUDE.md-Konvention für Designaufzeichnungen).

---

## Cross-Modul-Hinweise

- `src/HE_Editor/ViewportPanel.cpp:1535-1560` ruft jeden Frame `ctx.selection.roots(...)`, dann `erase(remove_if(... all_of<EditorLockComponent>))`; zusätzlich Collab-Filter. Siehe EDT-701/702. Der Vektor sollte pro Selektionsrevision gecacht oder wiederverwendet werden.
- `src/HE_Editor/ViewportActions.cpp:88,103,180,244,277` und `EditorApplication.cpp:11740` rufen `roots()` pro Aktion, profitieren von EDT-701 automatisch.
- `src/HE_Editor/ShortcutsPage.cpp:147` (`conflicts(a.id)` pro Zeile pro Frame), siehe EDT-706.
- `src/HE_Editor/SelfUpdateDialog.cpp:180-181,188` ruft `su.snapshot()` mehrfach pro Frame (je ein Mutex-Lock und Kopie von fünf Strings). Einmal pro Frame in eine lokale Variable lesen (Niedrig).
- `src/HE_Editor/EditorUI.cpp:3774` (`tabMark` + `CalcTextSize((tab.label + " *"))` pro Tab pro Frame, baut Strings) und `tabHasUnsavedEdits(tab.assetPath)` pro Tab pro Frame. In Slice des EditorUI prüfen.
- `src/HE_Editor/McpToolRegistry.h:1370` und `EditorSettingsCatalog.h:10-11` behaupten, der Katalog spiegle die Widget-Bereiche; stimmt nicht (EDT-703).
- `src/HE_Editor/EditorHelp.cpp:9634` (`findKey`) wird von jeder Toolbar-Zelle pro Frame aufgerufen (EDT-707); Implementierung (Tabelle sortiert? Hash?) im Slice von EditorHelp bewerten.

## Unsicher / Messung nötig

- EDT-701: tatsächliche Kosten von `roots()` bei Select All (5 000 / 20 000 Entities) im Viewport-Frame messen; ebenso ob Tiefe > 1 in realen Szenen vorkommt.
- EDT-702: Zeit von `worldMatrixOf` pro Mitglied (Tiefe) und `glm::inverse` bei 100/1 000 Mitgliedern im Drag.
- EDT-705: Allokationen pro Frame im idle Footer (Allokations-Zähler/Tracy) bestätigen; absoluter Gewinn vermutlich < 20 µs.
- EDT-706: Zeitanteil von `pressed()`-Summe pro Frame (vermutlich ~2–5 µs).
- EDT-707: Suchstrategie von `Help::findKey` (nicht in diesem Slice) – wenn Hash/Binärsuche, ist der Fund noch kleiner.
- EDT-704: Ist das Nicht-Zurücksetzen der Render-Einstellungen in "Restore Defaults" gewollt? Produktentscheidung.
- EDT-709: Dauer der Erstsynthese des Chime/Tick (geschätzt 1–3 ms) messen.
- `EditorSettingsPanel.cpp` Status-Seite: Allokationen pro Frame nur relevant, falls die Seite Dauer-Hotspot beim Profiling wird.

## Gelesen

Alle Dateien des Slice vollständig gelesen (ja): EditorRewards.cpp (1–1731) und .h (1–902), EditorRowWidgets.cpp, EditorSelection.{h,cpp}, EditorSelfUpdate.cpp und .h (Zeilen 1–60 des Headers nur überflogen, Kopfkommentar), EditorSettingsCatalog.{h,cpp}, EditorSettingsPanel.cpp (1–2490) und .h, EditorShortcuts.{h,cpp}, EditorSystemMotion.{h,cpp,mm}, EditorTabs.{h,cpp}, EditorTheme.{h,cpp}, EditorToolbar.{h,cpp}, EditorTransformGizmo.{h,cpp}. Zusätzlich gezielt gelesen: Aufrufer in ViewportPanel.cpp, EditorApplication.cpp, ShortcutsPage.cpp, SelfUpdateDialog.cpp, EditorUI.cpp, ContentBrowserPanel.cpp, QuickSettingsFavorites.h (nur Zitate, nicht ausgewertet).
