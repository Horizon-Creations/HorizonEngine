# Core Slice 5 – MaterialGraph, Math, ParticleGraph, Physics (CollisionLayers/WaterWaves), Platform, Project, Renderer (IRenderer/UIFont/…), Scripting

## Zusammenfassung

Der Slice (33 Dateien, ca. 11 000 Zeilen) ist insgesamt sauber, gut kommentiert und defensiv geschrieben. Die meisten Strukturen sind bereits sinnvoll geteilt (GraphCommon, X-Makro für die Grafikfelder, ein gemeinsamer Notify-Helfer). Die relevanten Funde liegen an drei Stellen. Erstens das UI-Text-Pipeline in `UIFont.cpp`, das pro Label und Frame viele Heap-Allokationen und quadratische Messungen macht und 224 Byte pro Glyph erzeugt. Zweitens `Process.cpp` auf Windows, das den Vertrag aus `Process.h` bricht (Timeout, Callback-Thread). Drittens die Material-/Partikel-Auswertung ohne Memoisierung. Zusätzlich ein belegter Logikfehler in `isPathWithin` (Wurzel mit abschließendem Separator). Gefunden: 1 Hoch, 11 Mittel, 10 Niedrig. Messdaten liegen keine vor; alle Performance-Aussagen sind Schätzungen aus dem Code und als solche gekennzeichnet.

---

### CORE-501 – Windows `Proc::run`: Drain-Phase ohne Timeout, ein überlebender Enkelprozess hängt den Worker für immer
- **ID**: CORE-501
- **Kategorie**: E
- **Schweregrad**: Hoch
- **Typ**: Vereinfachung (Robustheit/Korrektheit)
- **Ort**: `src/HE_Core/src/Platform/Process.cpp:340-371` (Windows-Zweig), Vergleich POSIX `:553-616`
- **Problem**: Der Timeout gilt nur für `WaitForSingleObject(pi.hProcess, waitMs)`. Endet der Kindprozess normal, kehrt das Wait mit `WAIT_OBJECT_0` zurück und danach folgt unbedingt `outThread.join()` / `errThread.join()`. Hat das Kind einen Daemon/Enkel gestartet, der die vererbten Schreib-Handles der Pipes hält (klassisch: `adb start-server`, `git fsmonitor`, Build-Server), bekommt `ReadFile` nie EOF und der Aufrufer blockiert unbegrenzt, obwohl `timeoutMs` gesetzt war. Der Job-Object-Kill (`TerminateJobObject`) wird nur im `WAIT_TIMEOUT`-Pfad ausgeführt, und das Job wird erst nach den `join()`s geschlossen. Die POSIX-Variante hat dieses Problem nicht, weil ihre Poll-Schleife die Deadline auch nach dem Kindende noch prüft (und die Prozessgruppe killt). Der Kommentar in `makePipe` (POSIX) beschreibt genau diesen Fehlerfall als real aufgetreten.
- **Code-Auszug**:
```cpp
const DWORD waitMs = o.timeoutMs ? o.timeoutMs : INFINITE;
if (::WaitForSingleObject(pi.hProcess, waitMs) == WAIT_TIMEOUT) { ... TerminateJobObject ... }
outThread.join();
errThread.join();
```
- **Lösung**: Nach dem Prozessende die Reader nur noch bis zur Restdeadline abwarten. Variante ohne zweites Primitive: Reader mit `OVERLAPPED`/`PeekNamedPipe` statt blockierendem `ReadFile`, oder nach `WAIT_OBJECT_0` ein zweites `WaitForSingleObject` auf ein Event, das die Reader beim EOF setzen, mit Restdeadline; bei Ablauf `TerminateJobObject(job, 1)` (schließt die Pipe-Enden der Enkel) und dann `join()`. Skizze:
```cpp
const ULONGLONG deadline = o.timeoutMs ? GetTickCount64() + o.timeoutMs : 0;
// ... nach dem Prozess-Wait:
if (deadline && WaitForMultipleObjects(2, readerDone, TRUE, remaining(deadline)) == WAIT_TIMEOUT)
{ r.timedOut = true; TerminateJobObject(job, 1); }
outThread.join(); errThread.join();
```
- **Erwarteter Effekt**: Kein dauerhaft hängender Worker (Git/Source-Control, Codegen-Aufrufe). Kein Performance-Effekt im Normalfall.
- **Risiko / Verifikation**: Unsicher/messen: auf Windows mit einem Testprogramm reproduzieren, das einen Enkel mit geerbtem stdout startet und selbst beendet. `tests/test_process_runner.cpp` hat vermutlich nur POSIX-/Kurzläufer-Fälle; ein Fall „Kind endet, Enkel hält Pipe, Timeout 500 ms → `timedOut == true`“ ergänzen. `timedOut` bei sauberem Ende darf nicht fälschlich gesetzt werden.

---

### CORE-502 – Windows: Zeilen-Callbacks laufen auf den Drain-Threads, nicht auf dem Aufrufer-Thread, und unter einem Mutex
- **ID**: CORE-502
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (Korrektheit der Schnittstelle)
- **Ort**: `src/HE_Core/include/Platform/Process.h:31-33`, `src/HE_Core/src/Platform/Process.cpp:261-272, 343-344`
- **Problem**: `Process.h` verspricht: „The line callbacks are invoked on the calling thread, so they may touch the caller's state without locking“. Auf POSIX stimmt das (eine Poll-Schleife). Auf Windows ruft `drainPipe` → `splitter.feed` → `m_sink` den Callback aber auf `outThread`/`errThread` auf, serialisiert durch `mx`. Aufrufer, die sich auf das Versprechen verlassen (UI-Zustand ohne Lock beschreiben), haben auf Windows eine Datenrasse. Zudem hält der Mutex den jeweils anderen Strom an, solange ein Callback blockiert.
- **Code-Auszug**:
```cpp
std::lock_guard<std::mutex> lock(mx);
into.append(buf, got);
splitter.feed(buf, got);   // → o.onStdoutLine(...) auf dem Reader-Thread
```
- **Lösung**: Entweder den Vertrag in `Process.h` ehrlich machen („auf unspezifiziertem Thread, serialisiert“) oder auf Windows die Zeilen über eine Queue an den Aufrufer-Thread übergeben: Reader füllen eine `std::deque<std::pair<bool,std::string>>`, der Aufrufer-Thread wartet in einer Schleife auf `{Prozess beendet, Queue nicht leer}` (Event) und ruft die Callbacks selbst auf. Das passt gut zur Lösung von CORE-501 (gleiche Wartestelle).
- **Erwarteter Effekt**: Plattformgleiches Verhalten; keine versteckten Races in Aufrufern wie `GitService`.
- **Risiko / Verifikation**: Reihenfolge stdout/stderr bleibt wie bisher nicht garantiert. Aufrufer der Callbacks (`rg "onStdoutLine"`) prüfen, ob sie bereits eigene Locks haben. Test mit Callback, der einen nicht-atomaren Zähler hochzählt und `std::this_thread::get_id()` mit dem Aufrufer vergleicht.

---

### CORE-503 – `approxFoldNode` ohne Memoisierung: exponentielle Laufzeit bei Diamanten im Graphen
- **ID**: CORE-503
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/MaterialGraph/MaterialGraph.cpp:2177-2186, 2233-2369`
- **Problem**: Der CPU-Fold (`matGraphApproxSurface`, aufgerufen beim Materialladen in `ContentManager.cpp:749`, im `HpakWriter` und in den MCP-Tools) wertet jede Eingabe rekursiv neu aus. Wird ein Knoten mehrfach genutzt (z. B. `Multiply(x, x)` mit beiden Pins aus derselben Quelle, ein typisches „Quadrat“), wird sein Teilgraph zweimal gefaltet. Eine Kette von n solchen Knoten kostet 2^n Besuche; die einzige Bremse ist `depth > 32`, also bis 2^32. Der Codegen (`emitNode`) memoisiert dagegen korrekt (`c.outVar`), der Fold nicht. Zusätzlich sucht jeder Besuch den Link linear (`approxFindLink`).
- **Code-Auszug**:
```cpp
case MatNodeType::Add: case MatNodeType::Subtract: case MatNodeType::Multiply: case MatNodeType::Divide:
    if (!approxFoldInput(g, *n, 0, depth + 1, a, sw) ||
        !approxFoldInput(g, *n, 1, depth + 1, b, sw)) return false;
```
- **Lösung**: Pro `matGraphApproxSurface`-Aufruf einen Cache `std::unordered_map<int, std::optional<std::array<float,4>>>` (Knoten-id → Ergebnis, `nullopt` = nicht faltbar) mitführen und in `approxFoldNode` vor dem `switch` prüfen. Das Ergebnis ist pro Knoten deterministisch (Output-Pin spielt bei der Faltung keine Rolle), das Verhalten bleibt also identisch. Dazu optional einen `dstNode*64+dstPin → Link`-Index (siehe CORE-513).
```cpp
struct FoldCtx { const MaterialGraph& g; const std::map<std::string,bool>* sw;
                 std::unordered_map<int, std::optional<std::array<float,4>>> memo; };
```
- **Erwarteter Effekt**: Von exponentiell auf linear in Knoten+Links. Bei normalen Graphen (< 50 Knoten) vermutlich nicht messbar, bei Diamant-Ketten der Unterschied zwischen Millisekunden und Hängern (Schätzung).
- **Risiko / Verifikation**: Gering. Test in `tests/test_material_graph.cpp`: Kette aus 28 `Multiply(x,x)` gegen ein Zeitlimit; bestehende Fold-Tests müssen bit-gleich bleiben (der Cache ändert keine Werte). Zyklen: weiterhin über `depth`-Guard abfangen (Cache erst nach erfolgreichem Fold schreiben).

---

### CORE-504 – Text-Emission: pro Label ein `vector<string>` mit zeichenweiser Kopie, Messung und Zeichnung layouten doppelt
- **ID**: CORE-504
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Renderer/UIFont.cpp:459-488` (`layoutUITextLines`), `:1113-1125` (`measureUIText`), `:1133-1194` (`emitUITextGlyphs`)
- **Problem**: `emitUITextGlyphs` (43 Aufrufstellen in Core/Scene/Editor) ruft `layoutUITextLines`, das auch für einen einzeiligen Text ohne Umbruch den ganzen String Zeichen für Zeichen in `cur` kopiert (`cur.push_back(c)`), dann in `lines` kopiert und zurückgibt. Danach ruft `emitUITextGlyphs` `lineWidth` pro Zeile (zweiter UTF-8-Durchlauf) und dann die Glyphenschleife (dritter Durchlauf). Auto-sizende Elemente rufen zusätzlich `measureUIText`, das dasselbe Layout noch einmal berechnet. Wird der Text pro Frame neu extrahiert, entstehen pro Label mindestens zwei Heap-Strings plus ein Vektor pro Frame.
- **Code-Auszug**:
```cpp
std::string cur;
for (char c : text)
{
    if (c == '\n') { lines.push_back(cur); cur.clear(); }
    else if (c != '\r') cur.push_back(c);
}
lines.push_back(cur);
```
- **Lösung**: Schnellpfad: enthält der Text weder `'\n'` noch `'\r'` und ist `!wrap` (häufigster Fall), `std::string_view`/Byte-Ranges verwenden, wie es `uiTextLineRanges` schon tut, statt Strings zu erzeugen. Intern auf `std::vector<std::string_view>` (oder Ranges) umstellen und `lineWidth` für `string_view` anbieten; nur die Wrap-Pfade, die wirklich Text verändern (Trimmen), brauchen eigene Strings. Die öffentliche Funktion `layoutUITextLines` kann für Aufrufer erhalten bleiben und die interne Variante nutzen.
- **Erwarteter Effekt**: Schätzung: 2–3 Allokationen weniger pro Label und Frame; bei Hunderten Labels im HUD spürbar im UI-Extract. Messen: Frame-Profil von `WidgetManager::extract`/`UISystem::extract` mit einem Text-lastigen Widget.
- **Risiko / Verifikation**: Ausgabe muss byte-identisch bleiben (CRLF-Behandlung, entfernte Schluss-Leerzeile, UTF-8). Tests: `tests/test_ui_font_weight.cpp` und Widget-Tests (`tests/test_ui_widgets.cpp`) laufen lassen; neuen Test für `"a\r\nb\n"`, `""`, `"a\n\n"` gegen die alte Implementierung (Golden-Vergleich).

---

### CORE-505 – Wrap-Algorithmen sind quadratisch und erzeugen pro Wort Kopien
- **ID**: CORE-505
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Renderer/UIFont.cpp:498-541` (`layoutUITextLines`), `:1029-1030, 1054, 1069-1075` (`uiTextWrapRanges`)
- **Problem**: Beim Umbruch wird für jedes Wort die gesamte bisherige Zeile neu vermessen (`lineWidth(font, trimmed(acc + word), scale)`: ein Temporärstring plus eine Kopie in `trimmed`, dann O(Zeilenlänge)). Bei `uiTextWrapRanges` erzeugt `widthOf` pro Aufruf ein `substr` und misst von `segStart` neu; das Hard-Break-Innere (`while (k < wEnd) { widthOf(segStart, k) ...}`) ist O(k²) mit k Allokationen. Ein einzelnes langes Wort (URL, Pfad im Eingabefeld) kostet damit Millisekunden. `uiTextWrapRanges` läuft für editierbare Textfelder bei jeder Änderung bzw. Zeichnung.
- **Code-Auszug**:
```cpp
auto widthOf = [&](std::size_t from, std::size_t to)
{ return lineWidth(font, text.substr(from, to - from), scale); };
...
if (widthOf(segStart, k) > wrapWidth) break;   // pro Zeichen neu vom Zeilenanfang
```
- **Lösung**: Breite inkrementell mitführen: `float rowW` (Breite bis `visEnd`) und Wortbreite `wordW` einmal per Durchlauf über `[cur, wEnd)` berechnen; Entscheidung `rowW + gapW + wordW > wrapWidth` ohne Neumessung. Für den Hard-Break das Prefix zeichenweise akkumulieren (`acc += glyph->xadvance * scale`), bis die Grenze überschritten ist. `lineWidth` bekommt eine `string_view`-/Range-Überladung (kein `substr`). Ergebnis (Zeilengrenzen) bleibt identisch, weil dieselben Advance-Summen verwendet werden (Gleitkomma-Summation in gleicher Reihenfolge beibehalten, sonst Randfälle bei exakt passender Breite).
- **Erwarteter Effekt**: Von O(n²) auf O(n) pro Zeile, keine Allokationen pro Wort. Schätzung: bei Absätzen mit > 1000 Zeichen Faktor 10+; bei kurzen Labels vernachlässigbar.
- **Risiko / Verifikation**: Gleitkommareihenfolge: Summe muss genauso gebildet werden wie `lineWidth` (links nach rechts). Differenztest alt/neu mit zufälligen Texten (ASCII, Umlaute, lange Wörter, Leerzeichenfolgen) und mehreren `wrapWidth`; vorhandene Tests zu Textfeldern (`tests/test_ui_*`).

---

### CORE-506 – `UIRenderObject` ist 224 Byte groß und wird pro Glyph erzeugt; `push_back` ohne `reserve`
- **ID**: CORE-506
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/Renderer/UIRenderObject.h:7-100`, `src/HE_Core/src/Renderer/UIFont.cpp:958-967, 1181-1190`
- **Problem**: Jeder Glyph-Quad (`type == 2`) trägt alle Felder eines Rechtecks: zwei `UUID` (32 B), Border, Gradient, Radii, Innenschatten, `clipRect`, Rotation, `uiState` usw. `sizeof(UIRenderObject)` ist 224 Byte (nachgemessen mit g++/glm). Ein Bildschirm mit 5000 Zeichen erzeugt pro Frame über 1 MB Quad-Daten, die danach (Sortierung nach `uiSortKey`, Batching nach Clip/Atlas, Übergabe an `RenderWidgetThumbnail`/Backends) mehrfach kopiert bzw. verschoben werden. Dazu wächst `out` bei jedem Label durch `push_back` ohne `reserve` mehrfach.
- **Code-Auszug**:
```cpp
UIRenderObject ro;            // 224 B, alle Felder default-initialisiert
ro.position = ...; ro.type = 2; ...
out.push_back(std::move(ro));
```
- **Lösung**: Zwei kleine Schritte ohne Formatwechsel: (1) in den Emit-Funktionen `out.reserve(out.size() + text.size())` (obere Schranke Glyphen = Zeichen); (2) mittelfristig Glyphen als eigene kompakte Struktur (`position, size, uvMin, uvMax, color, layer, atlasKey, clipRect` ≈ 72 B) führen oder rare Felder (`borderColor`, `gradient*`, `innerShadow*`, `uiState`, beide UUIDs) in ein optionales, selten genutztes Sub-Struct auslagern. Sortieren per Index-Array statt Objekte zu verschieben.
- **Erwarteter Effekt**: Schätzung: 2–3× weniger Speichertraffic im UI-Pfad bei textlastigen Szenen; `reserve` allein spart die Realloc-Kaskade. Messen: `sizeof` × Anzahl Quads im UI-Extract, Zeit für `std::stable_sort` der UI-Objekte.
- **Risiko / Verifikation**: Teil (2) ändert das Backend-Interface (6 Backends, `MetalRenderer.mm` etc.) und ist nur nach Messung sinnvoll. `reserve` ist risikolos. Backend-Bildvergleich (`HE_UI_DUMP_DIR`) nach Umbau.

---

### CORE-507 – `uiLayoutRichText`: pro Zeichen vier Mal `advanceAt` (inkl. `resolveFace`-Hash-Lookup), tote Variable
- **ID**: CORE-507
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/Renderer/UIFont.cpp:768-791, 813-843, 857-876, 899-905`
- **Problem**: `advanceAt(b)` löst bei jedem Aufruf `runAt` (Cursor), `faceAt` → `resolveFace` und `uiUtf8Decode` + `glyph()` auf. Für Nicht-Base-Faces (`<b>`, Icons) bedeutet `resolveFace` jedes Mal `uiEngineFaceKey` (statische Guard) und `UIFontCache::find` (unordered_map-Lookup). Die Funktion wird pro Zeichen im Umbruch-Scan, in der Zeilenhöhen-Schleife (zweimal: Summe `w` und getrimmte Breite), und in der Piece-Schleife aufgerufen. Die Summe `w` der Höhenschleife wird nie benutzt (`(void)w;`), ist also tote Arbeit. `runAt` bewegt den Cursor außerdem rückwärts (`while (runCursor > 0 && b < ...) --runCursor`), was bei den wiederholten Zeilendurchläufen jedes Mal zurücklaufen muss.
- **Code-Auszug**:
```cpp
float mx = 0.0f, w = 0.0f;
...
w += advanceAt(b);
...
(void)w;
```
- **Lösung**: Einmal pro Zeichen die Advance-Werte in ein `std::vector<float> adv(text.size())` (am Zeichenanfang-Byte) füllen und überall nachlesen; `faceAt` je Run vorab in `std::vector<ResolvedFace>` auflösen. Die tote Summe `w` entfernen.
- **Erwarteter Effekt**: Schätzung: 3–4× weniger Arbeit im Rich-Text-Layout; relevant nur, wenn das Layout pro Frame läuft (Cache in `UIElement.cpp:1395` gilt für das Parsen, nicht nachgewiesen für das Layout – messen).
- **Risiko / Verifikation**: Gleiche Werte, gleiche Summationsreihenfolge → bit-gleich. Es gibt laut Kommentar einen Test, der „plain == rich“ pinnt; zusätzlich Test mit `<b>`/`<icon=…>` und Mischgrößen.

---

### CORE-508 – `isPathWithin` weist jedes Kind einer Wurzel mit abschließendem Separator ab (toter `"."`-Zweig)
- **ID**: CORE-508
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (Korrektheit)
- **Ort**: `src/HE_Core/include/Platform/PathSafety.h:29-49`
- **Problem**: Der Kommentar behauptet, `lexically_normal` hinterlasse bei „a/“ ein abschließendes `"."`. Tatsächlich liefert `lexically_normal` für `"/proj/Content/"` und `"/proj/Content/."` einen leeren letzten Pfadbestandteil (`""`), kein `"."`. Der Zweig `if (*ri == ".")` wird dadurch nie erreicht; der leere Bestandteil wird mit dem Kandidatenelement verglichen und `isPathWithin("/proj/Content/", "/proj/Content/a.txt")` ergibt `false`. Nachgemessen (g++ -std=c++20):
```
/proj/Content   | /proj/Content/a.txt -> 1
/proj/Content/  | /proj/Content/a.txt -> 0     (falsch)
/proj/Content/. | /proj/Content/a.txt -> 0     (falsch)
/proj/Content/  | /proj/Content       -> 0     (falsch)
```
  Aktuell fail-closed (kein Sicherheitsloch), aber ein Aufrufer, der einen Pfad aus Nutzereingabe/Dialog mit Slash am Ende übergibt, lehnt dann alles ab. Aufrufer heute: `EditorApplication.cpp:10165` (baut über `/ "Source"`, daher unkritisch).
- **Code-Auszug**:
```cpp
if (*ri == ".") { ++ri; break; }
if (ci == c.end()) return false;
if (*ci != *ri)    return false;
```
- **Lösung**: Einfacher und korrekt über `lexically_relative` oder durch Entfernen des leeren Schlussbestandteils:
```cpp
std::filesystem::path r = root.lexically_normal(), c = candidate.lexically_normal();
if (!r.has_filename()) r = r.parent_path();     // "/proj/Content/" -> "/proj/Content"
if (!c.has_filename()) c = c.parent_path();
auto rel = c.lexically_relative(r);
return !rel.empty() && *rel.begin() != "..";
```
  (`lexically_relative` liefert `"."` für gleiche Pfade, das ist erlaubt; ein leeres Ergebnis bedeutet „nicht darstellbar“ → false, z. B. verschiedene Laufwerke.) Den toten Zweig samt falschem Kommentar entfernen.
- **Erwarteter Effekt**: Korrekte Semantik, weniger Code.
- **Risiko / Verifikation**: Sicherheitsrelevant: alle bestehenden Fälle aus `tests/test_collab_controller.cpp:1776-1785` müssen weiter bestehen (Geschwisterverzeichnis `Content-backup`, `..`-Ausbruch), neu: Wurzel mit `/` am Ende, `"."` am Ende, Windows-Laufwerksbuchstaben (`C:/a` vs `D:/a`), Groß-/Kleinschreibung unter Windows bleibt wie bisher (case-sensitiver Vergleich, ggf. separat entscheiden).

---

### CORE-509 – Lua: jede Instanz kopiert alle Modul-Schlüssel; Hot-Reload muss alle Instanzen patchen
- **ID**: CORE-509
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Core/src/Scripting/ScriptEngine.cpp:154-172` (`createInstance`), `:591-613` (`hotReloadScript`)
- **Problem**: `createInstance` legt pro Entity eine Tabelle an und kopiert per `lua_next`/`lua_rawset` jeden Schlüssel des Modulstabelle (alle Funktionen plus `properties` usw.) hinein. Bei 1000 Entities mit einem 15-Funktionen-Skript sind das 15 000 Tabelleneinträge und Hash-Inserts beim Spawn, plus Speicher pro Instanz. `hotReloadScript` muss dafür für jede Instanz die ganze Modultabelle erneut durchlaufen (O(Instanzen × Schlüssel)). Das übliche Lua-Muster (Instanztabelle mit Metatabelle `__index = module`) vermeidet beides.
- **Code-Auszug**:
```cpp
while (lua_next(m_L, -3) != 0)
{
    lua_pushvalue(m_L, -2);
    lua_insert(m_L, -2);
    lua_rawset(m_L, -4);   // instance_table[key] = value
}
```
- **Lösung**: Pro Skript eine Metatabelle `{ __index = moduleTable }` einmal in der Registry halten (zweite Ref in `ScriptRef`), `createInstance` = `lua_newtable` + `lua_rawgeti(meta)` + `lua_setmetatable`. Hot-Reload ersetzt dann nur den Inhalt der Modultabelle (oder tauscht `__index`), Instanzdaten bleiben unberührt. `pushInstanceMethod` funktioniert unverändert, da `lua_getfield` den `__index` auswertet.
- **Erwarteter Effekt**: Schätzung: Spawnkosten pro Instanz von O(Funktionen) auf O(1), weniger Speicher; Hot-Reload O(Schlüssel) statt O(Instanzen × Schlüssel). Messen: Zeit für 5000 `createInstance`.
- **Risiko / Verifikation**: Verhaltensunterschiede für Skripte, die `pairs(self)`, `rawget(self, "onUpdate")` oder `next(self)` verwenden, und für Skripte, die Modulfunktionen nach dem Spawn ändern (`M.foo = ...` würde jetzt live wirken, vorher nicht). `ScriptContext` (HE_Scene) und HE_Python prüfen (`rg "lua_next|rawget" src/HE_Scene`). Tests: `tests/test_script_*` (Lua), insbesondere Hot-Reload mit erhaltenen Datenfeldern und `injectProperties`. Als Entscheidung dokumentieren; nicht still einführen.

---

### CORE-510 – ~16 fast identische `callOn…`-Hooks in Interface und Lua-Backend
- **ID**: CORE-510
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/include/Scripting/IScriptBackend.h:52-146`, `src/HE_Core/src/Scripting/ScriptEngine.cpp:217-407`
- **Problem**: Jeder Hook wiederholt den gleichen Rumpf: Instanz suchen, Fehlermeldung setzen, `pushInstanceMethod`, Argumente pushen, `pcall(n, 0)`. Es gibt bereits drei Abkürzungen (`callNotifyMethod`, `callInstanceMethod` mit `std::function`, und die Enum-Variante für UI/Net), aber `callOnStart/Update/CollisionEnter/Exit/Overlap×2/Timer/CheatDetected/InputAxis/InputAxis2D` sind weiter handkopiert. Im Interface kommen pro neuem Event zwei Stellen (Interface + Python-Plugin + Lua) dazu; die Autoren haben das für UI/Net selbst als Problem erkannt („six near-identical virtuals would only spread it out“), aber nicht für die übrigen Paare.
- **Code-Auszug**:
```cpp
bool ScriptEngine::callOnTimer(InstanceId id, int handle)
{
    auto it = m_instances.find(id);
    if (it == m_instances.end()) { m_lastError = "Invalid instance id"; return false; }
    if (!pushInstanceMethod(m_L, it->second.luaRef, "onTimer")) return true;
    lua_pushinteger(m_L, static_cast<lua_Integer>(handle));
    return pcall(2, 0);
}
```
- **Lösung**: Einen privaten Template-Helfer einführen (kein `std::function`, kein Heap):
```cpp
template <class Push>
bool ScriptEngine::callHook(InstanceId id, const char* fn, int nargs, Push&& push)
{
    auto it = m_instances.find(id);
    if (it == m_instances.end()) { m_lastError = "Invalid instance id"; return false; }
    if (!pushInstanceMethod(m_L, it->second.luaRef, fn)) return true;
    push(m_L);
    return pcall(1 + nargs, 0);
}
bool ScriptEngine::callOnTimer(InstanceId id, int h)
{ return callHook(id, "onTimer", 1, [&](lua_State* L){ lua_pushinteger(L, h); }); }
```
  Damit schrumpfen ca. 150 Zeilen auf ca. 60. Das öffentliche Interface (ABI zum Python-Plugin) bleibt unverändert; eine Zusammenfassung der Hooks zu einem `callEvent(id, Kind, payload)` wäre ein ABI-Bruch und nur bei ohnehin geplantem Plugin-Update sinnvoll.
- **Erwarteter Effekt**: Weniger Code, ein Ort für Fehlermeldung und Stack-Disziplin. Laufzeit unverändert.
- **Risiko / Verifikation**: Gering, rein mechanisch. Bestehende Lua-Skript-Tests laufen lassen. Fehlermeldung „Invalid instance id“ und das Nicht-Setzen von `m_lastError` bei fehlendem Handler beibehalten.

---

### CORE-511 – `ProjectSettings`: dieselbe Feldliste in vier handgepflegten Funktionen
- **ID**: CORE-511
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Project/ProjectSettings.cpp:164-224` (`operator==`), `:226-334` (`clamp`), `:336-435` (`toJson`), `:437-567` (`fromJson`); Vorbild: `ProjectSettings.h:213-252` (`HE_PROJECT_GRAPHICS_FIELDS`)
- **Problem**: Für `graphics` ist die Feldliste ein einziges X-Makro (laut Header „a field is added in exactly one place“). Für alle anderen Abschnitte (shadows, physics, anticheat, multiplayer, …, ca. 60 Felder) steht jedes Feld in `operator==`, `toJson`, `fromJson` und teilweise `clamp` einzeln. Ein neues Feld braucht vier Edits; vergisst man `operator==`, erkennt `isDefault()`/der Panel-Dirty-Vergleich die Änderung nicht, vergisst man `toJson`, wird sie nicht gespeichert – beides still. Zusätzlich wird in `operator==` für die Float-Felder `nearlyEqual` von Hand gesetzt, während der Rest `==` benutzt (Inkonsistenz-Risiko).
- **Code-Auszug**:
```cpp
&& nearlyEqual(shadows.distance, o.shadows.distance)
&& shadows.cascadeCount == o.shadows.cascadeCount
&& shadows.resolution == o.shadows.resolution
```
- **Lösung**: Pro Abschnitt ein kleines X-Makro oder (C++20) eine `fields()`-Tupel-Funktion `auto fields() { return std::tie(distance, cascadeCount, ...); }` je Struct; `operator==` iteriert über beide Tupel mit der vorhandenen `fieldEqual`-Überladung, `fromJson` über `readField`. Schlüsselnamen bleiben als Textliste im Makro, damit das Dateiformat byte-gleich bleibt. Nur die Clamp-Regeln (Wertebereiche, die teils vom Feld abhängen) bleiben handgeschrieben.
- **Erwarteter Effekt**: Ein Edit pro Feld, keine stillen „vergessen“-Fehler. Kein Laufzeiteffekt.
- **Risiko / Verifikation**: JSON-Ausgabe muss byte-gleich bleiben (Reihenfolge der Schlüssel in `toJson`, verschachtelte Objekte `score`/`policy`/`prediction`/`discovery`, `sourceControl.mode` als String). `tests/test_project_settings.cpp`: Golden-Vergleich der Ausgabe vor/nach dem Umbau; Test, der per Reflection prüft, dass jedes Feld im Roundtrip und im `==` vorkommt.

---

### CORE-512 – `AABB::transformed`: 8 volle Matrix-Vektor-Multiplikationen statt Arvo-Methode
- **ID**: CORE-512
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/Math/AABB.h:44-58`; Aufrufer im Frame-Pfad: `src/HE_Rendering/src/RenderExtractor.cpp:466, 614, 636, 798`, `FoliageExtract.cpp:158`, `RenderObject.h:145`
- **Problem**: Für jedes sichtbare Objekt und jeden Frame werden 8 Ecken mit `m * vec4(corner, 1)` (je 16 Mul + 12 Add) transformiert und per `min/max` zusammengeführt. Für affine Matrizen genügt die Center/Extent-Methode (Arvo): ca. 18 Mul, ohne Verzweigung auf `isValid` in der Schleife.
- **Code-Auszug**:
```cpp
for (int i = 0; i < 8; ++i) {
    const glm::vec3 corner{ (i & 1) ? max.x : min.x, ... };
    out.expand(glm::vec3(m * glm::vec4(corner, 1.0f)));
}
```
- **Lösung**:
```cpp
const glm::vec3 c = center(), e = extents();
const glm::vec3 nc = glm::vec3(m[3]) + glm::vec3(m[0]) * c.x + glm::vec3(m[1]) * c.y + glm::vec3(m[2]) * c.z;
const glm::vec3 ne = glm::abs(glm::vec3(m[0])) * e.x + glm::abs(glm::vec3(m[1])) * e.y + glm::abs(glm::vec3(m[2])) * e.z;
out.min = nc - ne; out.max = nc + ne;
```
  Ergebnis ist mathematisch identisch (für Matrizen mit letzter Zeile (0,0,0,1); die bestehende Funktion nimmt das ebenfalls an, `w` wird nicht dividiert), kann sich aber in den letzten Float-Bits unterscheiden.
- **Erwarteter Effekt**: Schätzung: 3–4× weniger Arithmetik pro Aufruf; bei 10 000 Objekten im Extract im Bereich 0,1 ms. Nur bei einem Frame-Profil relevant, daher vor Umsetzung messen.
- **Risiko / Verifikation**: Leicht andere letzte Bits können Tests mit exaktem Vergleich brechen (`rg "transformed" tests/`) und das Culling um ein ULP verschieben. Test: zufällige affine Matrizen, Vergleich mit der 8-Ecken-Version mit Toleranz 1e-5; Nicht-affine Matrizen (Projektion) dürfen nie hier landen.

---

### CORE-513 – MaterialGraph: lineare Registry- und Link-Suchen im Codegen und beim JSON-Laden
- **ID**: CORE-513
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/src/MaterialGraph/MaterialGraph.cpp:327-339` (`matNodeDesc`, `matNodeDescByName`), `:1430, 1479, 1511` (Aufrufer im Codegen), `:1487-1494, 1521-1526, 2155-2160` (Link-Suche), `:1253, 1470-1475, 1505` (Funktionsschnittstelle)
- **Problem**: (a) `matNodeDesc(type)` ist eine lineare Suche über ca. 95 Registry-Einträge und wird pro Knoten mehrfach aufgerufen (`emitNode`, `outputPinType`, `inputExpr`, JSON-Schreiben); `matNodeDescByName` vergleicht pro geladenem Knoten mit bis zu 95 Namen (`std::string == const char*`). (b) `inputExpr`, `hasInput` und `uvInput` scannen die komplette Linkliste pro Pin → O(Knoten × Links); `uvInput` scannt zweimal. (c) `fnInterfaceNodes` (alloziert + sortiert) wird für jeden `FnInput`-Knoten und für jeden Pin eines `FunctionCall` neu gebaut, `outputPinType` baut dazu `matFunctionPins` (zwei Vektoren) pro Link. Alles läuft bei jeder Shader-Generierung (Editor ruft `generateFragment` an ca. 7 Stellen), nicht pro Frame; bei dem 151-Knoten-Landscape-Graph vernachlässigbar.
- **Code-Auszug**:
```cpp
const MatNodeDesc& matNodeDesc(MatNodeType type)
{
    for (const auto& d : registry())
        if (d.type == type) return d;
    return registry().front();
}
```
- **Lösung**: (a) Beim ersten Zugriff ein `std::array<const MatNodeDesc*, 256>` nach `uint8_t`-Typ und eine `unordered_map<string_view,const MatNodeDesc*>` für Namen aufbauen. (b) Pro `Scope` einen Index `unordered_map<uint64_t(dstNode<<32|dstPin), const MatGraphLink*>` beim ersten Zugriff füllen. (c) Funktionsschnittstellen pro Aufruf von `generateFragment` im `EmitCtx` cachen (Schlüssel = Funktionsgraph-Zeiger).
- **Erwarteter Effekt**: Nur bei sehr großen Graphen messbar (Schätzung: Codegen-Zeit O(N·L) → O(N+L)). Niedrige Priorität, erst messen (Editor-Latenz beim Ziehen eines Parameterknotens in einem großen Graphen).
- **Risiko / Verifikation**: Gering. Erzeugter GLSL-Text muss byte-gleich bleiben (Pipeline-Cache-Schlüssel!). `tests/test_material_graph.cpp` enthält Codegen-Vergleiche; mit Golden-Text für den Landscape-Graph prüfen.

---

### CORE-514 – MaterialGraph: StaticSwitch-Auflösung dreifach dupliziert, kleine Codegen-Reste
- **ID**: CORE-514
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `MaterialGraph.cpp:1228-1236` (Codegen), `:2267-2272` (`approxFoldNode`), `:2386-2392` (`approxSource`); `:1584` und `:1599` (doppelte `uiDomain`-Berechnung); `:1948-1962` (leerer Zweig); `:1914-1926` (große String-Kopien)
- **Problem**: Der Code für „Name des Schalters, Override aus der Map, Node-Default“ steht dreimal wörtlich da (`swName = n.s.empty() ? "switch_" + id : n.s`). Weicht eine Kopie ab, folgen Codegen und GI-Fold verschiedenen Zweigen. In `generateFragment` wird `uiDomain` zweimal mit demselben Ausdruck berechnet (ein Kommentar erklärt die Reihenfolge, aber `c.uiDomain` ließe sich einfach später wiederverwenden), `if (uiDomain) { /* no G-buffer tail */ }` ist ein leerer Zweig nur zur Kettenbildung, und `common` (der gesamte Shader-Prolog) wird in `src = withArrayMask(header) + common` und `withArrayMask(headerGB) + common` zweimal kopiert, obwohl Header kurz und `common` groß ist.
- **Code-Auszug**:
```cpp
const std::string swName = n->s.empty() ? ("switch_" + std::to_string(n->id)) : n->s;
bool on = n->p[0] > 0.5f;
if (sw) if (auto it = sw->find(swName); it != sw->end()) on = it->second;
```
- **Lösung**: Freie Funktion `bool resolveSwitch(const MatGraphNode&, const std::map<std::string,bool>*, std::string* nameOut)` und an den drei Stellen verwenden; `uiDomain` einmal berechnen; Tail-Aufbau als `if (!uiDomain) { if (lit) ... else ...; gb += ... }`. Die Kopie von `common` lässt sich vermeiden, indem man `src`/`gb` direkt mit `reserve(header.size()+common.size()+tail)` bildet; Gewinn gering.
- **Erwarteter Effekt**: Weniger Divergenzrisiko, ca. 20 Zeilen weniger. Kein messbarer Laufzeiteffekt.
- **Risiko / Verifikation**: Rein mechanisch; Codegen-Text byte-gleich (Golden-Test), insbesondere Switch-Vergleich mit Override und doppeltem Schalternamen (der erste gewinnt, `c.switches`).

---

### CORE-515 – Partikel-Graph: Auswertung ohne Memo, `RandomRange` würfelt pro Verbraucher, UB bei min > max
- **ID**: CORE-515
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Core/src/ParticleGraph/ParticleGraph.cpp:242-313` (insbesondere `:277-283`), Header `ParticleGraph.h:13-17`
- **Problem**: (a) Wie in CORE-503 wird ein mehrfach genutzter Teilgraph bei jedem Verbraucher neu ausgewertet (bis `depth <= 32`, also 2^32 bei einer Diamantkette). Die Knotenzahl der Partikelgraphen ist klein, praktisch kaum relevant. (b) `RandomRange` ruft bei jeder Auswertung `dist(rng)`: speist ein Knoten zwei Pins (Start- und Endgröße), erhält jeder Pin einen anderen Zufallswert. Der Header sagt „Resolves once per evaluate() call“, was man als „ein Wert pro Knoten“ liest; der Code liefert „ein Wert pro Verbraucher“. (c) `std::uniform_real_distribution<float>(p0, p1)` ist undefiniert, wenn `p0 > p1` oder nicht endlich (aus Datei/Eingabe möglich).
- **Code-Auszug**:
```cpp
case ParticleNodeType::RandomRange:
{
    std::uniform_real_distribution<float> dist(n->p[0], n->p[1]);
```
- **Lösung**: `lo = std::min(p0,p1)`, `hi = std::max(p0,p1)` und bei Nicht-Endlichkeit auf `lo` zurückfallen. Für (a)/(b) pro `evaluateParticleGraph` eine Memo-Map `nodeId → vec3` führen; das ändert für fan-out-genutzte `RandomRange` das Ergebnis (gleicher Wert an beiden Pins) und ist damit eine bewusste Verhaltensentscheidung, die man mit dem Autor des Headers klären muss. Nur (c) ist risikolos.
- **Erwarteter Effekt**: (c) Robustheit; (a)/(b) kein messbarer Gewinn, aber dokumentierte Semantik.
- **Risiko / Verifikation**: `tests/test_particle_graph.cpp`: Fall `RandomRange` mit zwei Verbrauchern (aktuelles Verhalten festschreiben oder ändern), Fall min > max. Seed-abhängige Ergebnisse bei Reihenfolgeänderungen der rng-Aufrufe prüfen.

---

### CORE-516 – `sampleWaves`: Richtungs-Trigonometrie und `k` pro Aufruf statt pro Wellenzug
- **ID**: CORE-516
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/Physics/WaterWaves.h:78-98` (Zeilen 86-88: `cos(rad)`, `sin(rad)`, `k`)
- **Problem**: Für jeden Auftriebs-/Query-Punkt und jede Physik-Teilschritt werden pro Wellenzug `cos(rad)`, `sin(rad)` und `k` neu berechnet, obwohl sie nur von `WaveTrain` abhängen. Pro Punkt kommen dann `sin(ph)` und `cos(ph)` hinzu. Bei zwei Trains also 4 überflüssige Trig-Aufrufe pro Punkt (von 8).
- **Code-Auszug**:
```cpp
const double rad = static_cast<double>(w.directionDeg) * (kWaveTwoPi / 360.0);
const double dx  = std::cos(rad), dz = std::sin(rad);
const double k   = kWaveTwoPi / static_cast<double>(w.length);
```
- **Lösung**: Eine `PreparedTrain { double dx, dz, k, speed, steep; }`-Struktur einmal pro Wasserkörper und Schritt aufbauen (z. B. in `WaterQuery`, HE_Scene), `sampleWaves(const PreparedTrain*, int, x, z, t)` als Überladung; `sin`/`cos(ph)` über `sincos` (GCC/Clang) bzw. gemeinsamen Aufruf. Die bisherige Funktion bleibt als Wrapper und bleibt für die Material-Generatoren gültig.
- **Erwarteter Effekt**: Schätzung: ca. 40–50 % weniger Trig-Aufrufe in der Wasserabfrage. Wegen `double` und weniger Punkten pro Frame kaum messbar; erst mit `test_water_buoyancy`/Profil mit vielen Schwimmkörpern prüfen.
- **Risiko / Verifikation**: Rundung identisch, solange `dx,dz,k` mit denselben Ausdrücken berechnet werden (Bit-gleich). `tests/test_water_query.cpp`, `test_water_buoyancy.cpp`.

---

### CORE-517 – `UIFontCache::keyFor`: fehlgeschlagener Bake wird nicht gemerkt, 32-Bit-Hash ohne Kollisionsprüfung, globale Zustände ohne Synchronisation
- **ID**: CORE-517
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Core/src/Renderer/UIFont.cpp:221-224, 366-387`; Aufrufer `UIWidget/WidgetManager.cpp:218`, `HE_Editor/AssetThumbnailCache.cpp:298`
- **Problem**: (a) Schlägt `bakeInto` fehl, `return 0` ohne Eintrag im Cache; jeder weitere Aufruf mit derselben defekten TTF backt erneut (bis zu drei Pack-Versuche mit Atlasverdopplung auf 4096², also Dutzende MB Allokation). (b) `key = (uint32_t)(h ^ (h >> 32))` und danach `c.find(key) == c.end()`: zwei verschiedene Fonts mit gleichem 32-Bit-Key teilen sich stillschweigend das Atlas des ersten. (c) `g_scripts`, `g_weightBold`, `g_sharedBaked` und die `cache()`-Map sind nicht atomar/gelockt, obwohl `keyFor` aus Editor-Hintergrund-Pfaden (Thumbnails) erreichbar sein kann – unsicher, ob jemals von mehreren Threads genutzt.
- **Code-Auszug**:
```cpp
if (c.find(key) == c.end())
{
    BakedUIFont f; ...
    bakeInto(ttf.data(), f, uiFontScripts());
    if (!f.ok) return 0;      // nichts gemerkt
    c.emplace(key, std::move(f));
}
```
- **Lösung**: Fehlschlag als `BakedUIFont` mit `ok=false` cachen (die Anrufer prüfen bereits `ok`); Key als 64 Bit halten und intern auf `uint64` indizieren, nach außen nur den (zufällig kollisionsarmen) 32-Bit-Wert, oder bei Kollision hochzählen (`while (c.count(key) && c[key].stableId != id) ++key`). Für (c) einen `std::mutex` um `cache()`-Schreibzugriffe oder in der Dokumentation „nur Main-Thread“ festschreiben.
- **Erwarteter Effekt**: Vermeidet Wiederholtes Backen defekter Fonts; Kollisionssicherheit.
- **Risiko / Verifikation**: Gering. Test: kaputte TTF zweimal, zählt Bakes; zwei Font-IDs mit erzwungenem Hash-Konflikt. `tests/test_ui_font_weight.cpp`.

---

### CORE-518 – `IRenderer.h`: schwerer Header und 12 Einzel-Setter, drei handgepflegte Abbildungen der Umgebungseinstellungen
- **ID**: CORE-518
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/include/Renderer/IRenderer.h:1-14, 376-613` ; `EnvironmentSettings.h:16-26`
- **Problem**: (a) Der Header zieht `glm/gtc/matrix_transform.hpp` (nur für `worldPreviewProjection`), `UIRenderObject.h` (224-Byte-Struktur nur für `RenderWidgetThumbnail`), `DebugDraw`, `<functional>`, und wird von `GpuPassAccumulator.h`, `RendererFactory.h` u. a. transitiv eingebunden (Kompilierzeit). (b) Zwölf `Set…Settings`-Virtuals (Bloom, AA, SSAO, Shadow, GI, SSR, Occlusion, DoF, MotionBlur, GIRefl, …) mit jeweils leerem Default; die Kommentare nennen sie „pushed every frame“. Das sind ca. 12 virtuelle Aufrufe pro Frame (vernachlässigbar), aber jedes Backend muss jedes Setter-Paar einzeln auf Änderung vergleichen. (c) `EnvironmentSettings` liegt im globalen Namespace (Konvention: `namespace HE`) und muss laut Kommentar an drei Stellen von Hand synchron gehalten werden (`EnvironmentPush.cpp`, `SkyFrameParams.cpp`, OpenGL-Sky-Block); ein neues Feld, das in einer fehlt, wirkt auf einem Backend stillschweigend nicht.
- **Code-Auszug**:
```cpp
// KEEP IN SYNC — a new field needs BOTH of these, and on OpenGL a third:
```
- **Lösung**: (a) `worldPreviewProjection`/`worldPreviewVerticalFov` in einen eigenen Header (`Renderer/WorldPreview.h`) auslagern, `UIRenderObject.h` per Vorwärtsdeklaration (`struct UIRenderObject;`) einbinden – Signaturen mit `const std::vector<UIRenderObject>&` benötigen nur die Deklaration. (b) Ein `struct RenderSettings { BloomSettings bloom; AntiAliasingSettings aa; ... }` plus ein einziges `virtual void SetRenderSettings(const RenderSettings&)` mit `operator==` zur Änderungserkennung; die ABI-Reihenfolge der Vtable darf nicht verändert werden (Kommentar im Header beachten), daher nur anfügen. (c) Test, der per Reflection/`sizeof`-Check meldet, wenn `EnvironmentSettings` wächst, ohne dass die Abbildungstabelle aktualisiert wurde.
- **Erwarteter Effekt**: Kürzere Kompilierzeit (schwer zu schätzen, Messung mit `-ftime-trace`), weniger Wartungsfehler.
- **Risiko / Verifikation**: Mittel für (b) wegen Vtable-ABI und sechs Backends – nur umsetzen, wenn ohnehin Backend-Umbau ansteht. (a) risikoarm: Build aller Ziele.

---

### CORE-519 – `GpuPassAccumulator`: drei Kopien pro abgeschlossenem Frame, `std::map`, GC-Scan pro Report
- **ID**: CORE-519
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Core/include/Renderer/GpuPassAccumulator.h:75-125` (Zeilen 93-110)
- **Problem**: Beim Abschluss eines Frames wird der Pass-Vektor dreimal kopiert (`fs.passes = a.passes`, `m_last = fs`, `outDone = fs`), die in-flight-Frames liegen in einem `std::map`, und jeder `report()` scannt die komplette Map nach alten Frames. Das läuft nur im Profiler-Detail-Capture, im Completion-Handler von Metal-Command-Buffern (Hintergrund-Thread, unter Mutex). Der Aufwand ist klein (≈ 20–40 Pässe), der Mutex wird aber vom Hauptthread in `latest()` geteilt. Zudem bindet der Header `IRenderer.h` nur wegen zweier kleiner Structs ein (siehe CORE-518).
- **Code-Auszug**:
```cpp
fs.passes = a.passes;
...
m_last = fs;
...
outDone = fs;
```
- **Lösung**: `fs.passes = std::move(a.passes)`, danach `m_last = fs;` (eine Kopie) und `outDone = std::move(fs)`; `m_inflight` als kleiner `std::vector<std::pair<uint64_t,Accum>>`; GC nur, wenn `frame > m_newestSeen`. `GpuPassTime`/`FrameGpuStats` in einen kleinen Header `Renderer/GpuStats.h` verschieben.
- **Erwarteter Effekt**: Vernachlässigbar außerhalb des Profilers; nur Aufräumen.
- **Risiko / Verifikation**: Gering; es gibt laut Header einen direkten Unit-Test des Akkumulators (Reihenfolge, Straggler).

---

### CORE-520 – Rich-Text-Parser und Glyphen-Emission: unnötige Kopien, doppelte `stof`, doppelter Emit-Code
- **ID**: CORE-520
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Renderer/UIFont.cpp:602-606, 647, 711` (Parser), `:958-967` vs. `:1181-1190` (Glyphen-Quads), Umbruch in drei Varianten (`:459-543`, `:813-843`, `:1022-1092`)
- **Problem**: (a) `readTag` prüft `size=` mit `std::stof` in einem `try/catch`, danach parst `uiParseRichText` denselben Wert noch einmal (`std::stof(value)`). Ungültige Eingaben („size=abc“) kosten eine Exception; die `std::stof`-Konvertierung ist außerdem locale-abhängig (Komma-Dezimaltrenner nach `setlocale`), `std::from_chars` wäre locale-frei und werfend-frei. (b) `attrs()` liefert `RichScope` per Wert (zwei `std::string`-Kopien) und wird in `flush` und an mehreren Stellen aufgerufen. (c) Der Block, der aus `BakedGlyph` ein `UIRenderObject` baut, steht in `uiEmitRichText` und `emitUITextGlyphs` je einmal vollständig; änderte jemand ein Quad-Feld, müsste er beide anfassen. (d) Es gibt drei Greedy-Umbruch-Implementierungen (Label-Strings, Byte-Ranges für Eingabefelder, Rich-Layout) mit absichtlich unterschiedlichen Randregeln (dokumentiert); jede neue Regel (z. B. Tab-Behandlung: nur `uiTextWrapRanges` kennt `\t`) muss dreifach gepflegt werden.
- **Code-Auszug**:
```cpp
try { const float v = std::stof(outValue); return v > 0.0f && v < 100.0f; }
catch (...) { return false; }
...
else if (name == "size") a.sizeScale = std::stof(value);
```
- **Lösung**: `readTag` gibt den geparsten `float` mit heraus (`outSize`), `std::from_chars` statt `stof`; `attrs()` als `const RichScope&` mit statischer Leer-Instanz; ein `static UIRenderObject makeGlyphQuad(const BakedGlyph&, float scale, ...)` für beide Emitter. Die drei Umbruchregeln nur zusammenführen, wenn die Unterschiede (Trimmen, `\r`, Tab) bewusst vereinheitlicht werden – sonst nur dokumentieren.
- **Erwarteter Effekt**: Weniger Code, kein Exception-Pfad; Laufzeit vernachlässigbar.
- **Risiko / Verifikation**: `from_chars` für `float` ist bei libc++ erst ab neueren Versionen vorhanden (macOS-Toolchain prüfen), sonst `strtof` mit C-Locale. Parser-Regeln laut Header sind „pinned by tests“ → diese Tests müssen unverändert bestehen.

---

### CORE-521 – `Proc::run` / `augmentToolPath`: kleine Reste (POSIX-`cwd`, Zeilensplitter, Wartepolling, `setenv`)
- **ID**: CORE-521
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Core/src/Platform/Process.cpp:33-41` (Splitter), `:107` (`setenv`), `:489-494` (`cwd`), `:627-634` (Kill-Wartezeit)
- **Problem**: (a) Ist `posix_spawn_file_actions_addchdir_np` nicht verfügbar (Bedingung `__APPLE__ || _POSIX_C_SOURCE >= 200809L`), wird `Options::cwd` stillschweigend ignoriert und der Prozess läuft im falschen Verzeichnis. Die Bedingung prüft ein POSIX-Makro, aber `_np` ist eine glibc/macOS-Erweiterung (glibc ≥ 2.29). (b) `LineSplitter::feed` hängt jedes Zeichen einzeln per `push_back` an; `memchr` auf `'\n'` und Blockanhang wäre kürzer und schneller (relevant bei Megabyte-Ausgaben wie `git status`/`cmake`). (c) Nach `SIGTERM` pollt die Schleife mit `usleep(10000)` bis zu 2 s. (d) `augmentToolPath` ruft `setenv` in einem Thread, während andere Threads `getenv` ausführen können (nicht thread-sicher); einmalig und früh, daher nur ein Risiko, wenn der erste Aufruf aus einem Worker kommt.
- **Code-Auszug**:
```cpp
#if defined(__APPLE__) || (defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200809L)
    ::posix_spawn_file_actions_addchdir_np(&actions, o.cwd.c_str());
#endif
```
- **Lösung**: Mit `#else` bei nicht vorhandener Funktion `r.launchFailed = true; return r;` (oder `chdir`-Wrapper über `/usr/bin/env -C`), statt still weiterzulaufen; `feed` mit `memchr`; `augmentToolPath()` in `Application`-Start (Main-Thread) vorab aufrufen.
- **Erwarteter Effekt**: Klare Fehler statt falschem Verzeichnis; schnelleres Splitten großer Ausgaben.
- **Risiko / Verifikation**: Gering. `tests/test_process_runner.cpp`: Fall mit `cwd` (Ausgabe von `pwd`) und mehrzeilige Ausgabe mit `\r\n`, Zeile ohne Abschluss.

---

### CORE-522 – Lua-Fehlerbehandlung: `lua_pcall` ohne Message-Handler (kein Traceback), `atoi` ohne Bereichsprüfung
- **ID**: CORE-522
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Core/src/Scripting/ScriptEngine.cpp:454-464` (`pcall`), `:70-72` (`parseScriptErrorLocation`)
- **Problem**: `lua_pcall(m_L, nargs, nresults, 0)` liefert nur die nackte Fehlermeldung `name:7: message`; der Aufrufstapel geht verloren, was bei verschachtelten Skriptfunktionen die Fehlersuche erschwert (Konsole kann nur die innerste Stelle öffnen). `std::atoi` in `parseScriptErrorLocation` hat bei Zeilennummern über `INT_MAX` undefiniertes Verhalten (Eingabe stammt aus einer Fehlermeldung, also nicht vertrauenswürdig, z. B. aus Skripttext `error("x:99999999999:")`).
- **Code-Auszug**:
```cpp
if (lua_pcall(m_L, nargs, nresults, 0) != LUA_OK)
...
out.line = std::atoi(message.c_str() + colon + 1);
```
- **Lösung**: Einen C-Message-Handler (`luaL_traceback(L, L, msg, 1)`) einmal in der Registry halten, per `lua_pushcfunction`/`lua_insert` unter die Funktion legen und als `msgh` übergeben, die Fehlerposition weiterhin aus der ersten Zeile lesen (`parseScriptErrorLocation` findet das erste `<name>:<zahl>:`, bleibt also stabil). `atoi` durch `std::from_chars` mit Bereichsprüfung ersetzen.
- **Erwarteter Effekt**: Bessere Fehlerdiagnose; geringe Mehrkosten nur im Fehlerfall (Handler läuft nur bei Fehler). Der Handler-Slot auf dem Stack kostet je Aufruf zwei Stack-Operationen im Hot Path `callOnUpdate` – vor Einführung messen oder den Handler nur im Editor-/Debug-Build aktivieren.
- **Risiko / Verifikation**: `lastError()`-Text ändert sich (länger, mehrzeilig); Tests, die `lastError()` exakt vergleichen, und die Konsole (`parseScriptErrorLocation`-Tests) anpassen. `ScriptContext` loggt die Meldung 1:1.

---

## Cross-Modul-Hinweise

- `src/HE_Rendering/src/RenderExtractor.cpp:466, 614, 636, 798`: `AABB::transformed` pro Objekt und Frame (CORE-512); dort außerdem prüfen, ob für statische Objekte (Transform unverändert) die Weltbox gecacht werden kann (Dirty-Flag am Transform) – das wäre der größere Hebel als die Methode selbst.
- `src/HE_Core/src/UIWidget/WidgetManager.cpp` / `src/HE_Scene` (UISystem::extract): Aufrufer von `emitUITextGlyphs`/`measureUIText` (43 Stellen). Klären, ob Text-Quads pro Frame neu gebaut oder bei unverändertem Text/Rect gecacht werden; davon hängt die Gewichtung von CORE-504 bis 506 ab.
- `src/HE_Scene/src/PhysicsWorld.cpp:153`: `PhysicsWorld` cacht `CollisionLayerConfig::collides` bereits in `m_matrix`; die Funktion in `CollisionLayers.cpp` ist daher nicht im Jolt-Filterpfad (kein Fund).
- `src/HE_Scene` `ScriptContext`: „Jede Instanz hört jeden Timer/Net-Event/Cheat-Report“ (Kommentar in `IScriptBackend.h:99-123`) heißt O(Instanzen) Aufrufe pro Ereignis über die virtuelle Schnittstelle; bei vielen Instanzen ohne passenden Handler wäre ein „Hat-Handler“-Bitset pro Skript (einmal bei `loadScript` ermittelt) ein Gewinn. Dort prüfen.
- `src/HE_Rendering/*Renderer*`: Die `Set…Settings`-Push-Aufrufe pro Frame (CORE-518) führen in den Backends zu Vergleichen/Uploads; prüfen, dass Änderungen (z. B. Schattenauflösung) nur bei echtem Delta Ressourcen neu anlegen.
- `src/HE_Editor/EditorApplication.cpp` (7 Stellen `generateFragment`): prüfen, ob die Shader-Generierung beim Ziehen eines Parameter-Sliders neu läuft (Parameteränderungen sollen laut Header nie neu kompilieren).
- `src/HE_Tools` / `HE_Editor` Aufrufer von `Proc::run` (GitService): Verhalten bei Windows-Timeout (CORE-501, 502) prüfen.

## Unsicher / Messung nötig

- Wie oft pro Frame `emitUITextGlyphs`, `measureUIText` und `uiLayoutRichText` tatsächlich laufen (CORE-504 bis 507): Frame-Profil mit einem textreichen Widget.
- Reale Kosten des 224-Byte-`UIRenderObject` im Sort/Batching (CORE-506): `perf`/Instruments auf `UISystem::extract` und Backend-UI-Pass.
- Reproduktion des Windows-Hängers (CORE-501) mit `adb start-server` oder einem eigenen Testprogramm, das einen Enkel mit geerbtem stdout startet.
- Laufzeit von `matGraphApproxSurface` bei Diamant-Graphen (CORE-503) – ob ein vorhandener Content-Graph die Fälle schon erreicht (Content-Scan über alle `.hasset`-Material-Graphen).
- Spawn-Kosten pro Lua-Instanz (CORE-509): Benchmark 5000 `createInstance` mit realistischem Skript; Kompatibilität mit `pairs(self)` in Projekt-Skripten.
- Ob `UIFontCache::keyFor` jemals von Nicht-Main-Threads erreicht wird (CORE-517).
- Gewinn von Arvo-`AABB::transformed` (CORE-512): Mikrobenchmark plus Extract-Zeit bei 10 000 Objekten.
- Kompilierzeitgewinn durch schlankeren `IRenderer.h` (CORE-518): `-ftime-trace`.

## Gelesen

Ja, alle 33 Dateien des Slice 5 vollständig gelesen (MaterialGraph.cpp/.h, WeatherMaterialFunctions.cpp/.h, AABB.h, Math.h, ParticleGraph.cpp/.h, CollisionLayers.cpp/.h, WaterWaves.h, DynLib.cpp/.h, PathSafety.h, Process.cpp/.h, ProjectWriteGate.h, ProjectSettings.cpp/.h, EnvironmentSettings.h, GpuPassAccumulator.h, IRenderer.cpp/.h, RendererFactory.h, UIFont.cpp/.h, UIRenderObject.h, WaterRipples.h, IScriptBackend.h, PythonPluginAbi.h, ScriptEngine.cpp/.h, ScriptTypes.h); keine Datei ausgelassen. `isPathWithin` (CORE-508) und `sizeof(UIRenderObject)` (CORE-506) wurden zusätzlich mit g++ nachgemessen; alle übrigen Angaben stammen aus dem Quelltext.
