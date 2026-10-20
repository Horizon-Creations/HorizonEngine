# Editor Slice 12 – MaterialEditorPanel.h, McpBridge*, McpCameraGizmos*, McpClaudeProbe*, McpClientCameras.h, McpClientSetup*, McpLockBook.h, McpStatusBar*, McpToolCommon*, McpToolRegistry*, McpToolsAnimator/Api/Asset/Batch/Build/Clip/Collab/Entity/Hc

## Zusammenfassung

Der Slice ist die MCP-Bridge des Editors (Socket, JSON-RPC, Tool-Registry, 11 Tool-Familien) plus ein paar Footer-/Viewport-Gizmos. Nichts davon läuft pro Frame im Draw-Pfad außer `McpBridge::update` (leer, solange niemand verbunden ist), `McpStatusBar` und die Kamera-Gizmos; der Hot-Path ist hier stattdessen der *Tool-Aufruf auf dem UI-Thread* (Bridge.h: „one call, on the main thread"). Die Qualität ist hoch (gute Fehlertexte, saubere Trennung ImGui-frei/testbar), die Probleme sind (a) zwei O(N)-Scans pro Aufruf in den Entity-Tools, die mit Batch/Szenengröße multiplizieren, (b) mehrfach duplizierte Gate-/Helferlogik über ≥11 Tool-Dateien und ein 1592-Zeilen-Header, der über `EditorApplication.h` in 78 Übersetzungseinheiten gezogen wird, (c) Frame-Thread-Dateisystemwalks, die entgegen den Kommentaren nicht durch das `limit` begrenzt sind. Funde: 2 Hoch, 8 Mittel, 7 Niedrig. Alle Funde sind statisch aus dem Code belegt; es wurde nichts gemessen oder gebaut.

---

### EDT-1201 – `entityByUuid` ist ein linearer Scan mit `snprintf` + `std::string` pro Entity, obwohl `HorizonWorld::findByEntityId` O(1) existiert
- **ID**: EDT-1201
- **Kategorie**: D (auch C)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: Aufrufer in diesem Slice: `McpToolsEntity.cpp:206`, `:443`, `:573`; `McpToolsApi.cpp:199` (über `hooks.entityByUuid`); Implementierung (Cross-Slice) `src/HE_Editor/EditorCommands.cpp:419-429`; vorhandener Index `src/HE_Scene/include/HorizonScene/HorizonWorld.h:53` (`findByEntityId`, `m_idIndex` Z. 300)
- **Problem**: Jede uuid-Auflösung eines MCP-Tools formatiert die ID *jeder* Entity der Welt als Hex-String (`snprintf` + `std::string`-Allokation, wenn > SSO) und vergleicht Strings; es gibt keinen Early-Exit (`view.each` läuft nach dem Treffer weiter, der Lambda kehrt nur sofort zurück). Jeder `entity_*`-Aufruf löst 1–2 uuids auf, `api_*`-Tools lösen *jeden* String-Int-Parameter so auf, und `batch` (bis 100 Operationen) multipliziert das: 100 Operationen × E Entities × ein `snprintf`. Das läuft auf dem UI-Thread. Die Welt führt bereits einen `unordered_multimap<UUID, Entity>`-Index (`findByEntityId`), der hier nicht genutzt wird.
- **Code-Auszug**:
```cpp
// EditorCommands.cpp:424
reg.view<const EntityIdComponent>().each([&](auto e, const EntityIdComponent&) {
    if (found != entt::null) return;
    if (uuidOf(world, e) == uuid) found = e;
});
```
- **Lösung**: Den Hex-String einmal parsen (`uuidFromHex` existiert in `TaskBoard.h:88`; sonst 2×`strtoull` über je 16 Zeichen, Länge 32 prüfen) und `world.findByEntityId(id)` aufrufen; `entt::null` bei Parsefehler. Kein neuer Cache nötig. Skizze:
```cpp
Entity entityByUuid(HorizonWorld& w, const std::string& s) {
    HE::UUID id; if (!parseHex32(s, id)) return entt::null;
    return w.findByEntityId(id);
}
```
- **Erwarteter Effekt**: Schätzung: von O(E) mit Allokation/`snprintf` je Entity auf O(1). Bei 50 000 Entities grob 5–20 ms je Auflösung → < 1 µs; ein 100er-Batch fiele von Sekunden auf Mikrosekunden. (Schätzung, nicht gemessen.)
- **Risiko / Verifikation**: Bei doppelten IDs (beschädigter Merge) lieferte der alte Scan den ersten in View-Reihenfolge, `findByEntityId` „einen von beiden" (HorizonWorld.h:50-52 dokumentiert das) – für MCP-Adressierung unkritisch. Groß-/Kleinschreibung: `uuidOf` gibt Kleinbuchstaben aus; der alte String-Vergleich war case-sensitiv, der Parser sollte beides akzeptieren oder ebenfalls streng sein (Verhalten erhalten = streng). Der Index wird über Registry-Signale gepflegt: Test in `tests/` für „Entity in demselben Frame erzeugt, dann `entity_get`" ergänzen (Index wird synchron über Signale gepflegt, aber das ist der Fall, an dem der Scan nie scheitern konnte).

---

### EDT-1202 – `entity_list` serialisiert jede Entity vollständig (CBOR→JSON) *vor* Filter und Limit und weiter nach Erreichen des Limits
- **ID**: EDT-1202
- **Kategorie**: D (auch C)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `McpToolsEntity.cpp:145-174` (`componentsOf`, `entitySummary`), `:331-362` (Schleife); Folgeaufrufe `:391`, `:685`, `:710`
- **Problem**: `entitySummary` ruft pro Entity `SceneSerializer::serializeEntityComponents` (alle Komponententypen via `try_get`), dann `json::from_cbor`, dann ein JSON-Array der Schlüssel – nur um danach `name` zu vergleichen oder `components` nach einem Schlüssel zu durchsuchen. Der Namensfilter (`nameContains`) bräuchte nur `NameComponent`; selbst ohne Filter wird nach Erreichen von `limit` (Z. 360 `++truncated; continue;`) weiter für *alle* restlichen Entities komplett serialisiert, nur um `truncated` zu zählen. Bei einer Szene mit 20 000 Entities × ~8 Komponenten sind das 20 000 CBOR-Encodes + Decodes + JSON-Bäume pro Aufruf auf dem UI-Thread, bei Standardlimit 200 liefert der Aufruf aber nur 200 Zeilen. Zusätzlich: `entity_set_components` serialisiert die Zielentity zweimal (Z. 685 und 710), `entity_get` einmal – das ist okay, aber die Rückgabe von `entity_set_components` ist ein weiterer voller Roundtrip.
- **Code-Auszug**:
```cpp
// McpToolsEntity.cpp:337-361
for (const Entity e : all) {
    if (e == world->rootEntity()) continue;
    json summary = entitySummary(*world, e);          // volle Serialisierung
    ...
    if (!needle.empty()) { std::string name = summary["name"]...; tolower... }
    if (static_cast<int>(out.size()) >= limit) { ++truncated; continue; }
```
- **Lösung**: (1) Reihenfolge umdrehen: zuerst billige Prädikate (uuid leer? `NameComponent` lowercase-Substring) direkt am Registry prüfen, erst für Treffer `entitySummary` bauen. (2) Für den `component`-Filter eine billige Prüfung „hat Entity Komponente mit Schlüssel K" anbieten (z. B. `SceneSerializer::hasComponentKey(world, e, key)` neben `isKnownComponentKey`, `SceneSerializer.cpp:3011`) statt Voll-Serialisierung; solange es sie nicht gibt, nur für den Fall `component` nicht leer voll serialisieren. (3) Nach `limit` für `truncated` nur noch die billigen Prädikate auswerten (Zähler), nie `entitySummary`. (4) `std::vector<Entity> all` (Z. 331) mit `reserve(registry.storage<entt::entity>().size())` bauen, oder direkt iterieren – der Kommentar begründet die Kopie mit Pool-Erzeugung durch `try_get`; die entfällt, wenn die billigen Prädikate vorab laufen und nur Treffer serialisiert werden (dann aber `all` beibehalten, solange `entitySummary` im Loop läuft).
- **Erwarteter Effekt**: Schätzung: Kosten fallen von O(E · Komponenten) auf O(E) billige Vergleiche + O(limit) Serialisierungen; bei großen Szenen Faktor 10–100 (nicht gemessen).
- **Risiko / Verifikation**: Ausgabeformat (`entities`, `truncated`) muss bit-identisch bleiben, inkl. dass Entities ohne uuid übersprungen werden und `truncated` nur gefilterte Treffer zählt. `tests/` hat vermutlich MCP-Entity-Tests (nach `entity_list` greppen); einen Test ergänzen, der `nameContains` + `limit` + `component` kombiniert und `truncated` prüft. Gefahr: Die `components`-Liste in der Zusammenfassung stammt aus dem Serializer; wer sie billiger baut, muss dieselbe Schlüsselmenge liefern (sonst driftet `entity_list` von `entity_get` ab) – deshalb die Voll-Serialisierung für Treffer beibehalten.

---

### EDT-1203 – Rekursive Verzeichniswalks auf dem UI-Thread laufen komplett (Sammeln + Sortieren) bevor `limit` greift; zwei Kopien der Walk-Logik
- **ID**: EDT-1203
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `McpToolCommon.cpp:197-241` (`walkContentAssets`), `McpToolsAsset.cpp:299-333` (`asset_list`, rekursiv); Kommentar-Behauptung `McpToolCommon.h:72-75`, `McpToolsAsset.cpp:39-41`
- **Problem**: Beide Funktionen laufen den *gesamten* Baum, sammeln alle Pfade (`absPaths.push_back`, `found.emplace_back`), sortieren alles und wenden erst danach das Limit an. Der Header begründet die Kappung ausdrücklich damit, dass „MCP handlers run on the editor's frame thread – a full walk of a large project would stall it", aber das Limit begrenzt nur die Ergebnisgröße, nicht den Walk. Bei `asset_list` mit `type`-Filter wird zudem pro Datei `typeNameOf` (Header-Sniff, bei Cache-Miss Dateizugriff) bis zum Limit aufgerufen. Außerdem sind die Dotfile-/Disable-Recursion-Regeln dreimal ausgeschrieben (Common:215-222, Asset:307 + :317-322).
- **Code-Auszug**:
```cpp
// McpToolCommon.cpp:213-228
for (; !ec && it != end; it.increment(ec)) { ... absPaths.push_back(...); }
std::sort(absPaths.begin(), absPaths.end());
// danach erst: if (found.size() >= limit) { truncated = true; break; }
```
- **Lösung**: (a) `asset_list` die Walk-Logik von `walkContentAssets` mitbenutzen lassen (eine Funktion `collectEntries(root, recursive, includeFolders)`), (b) die Walk-Ergebnisse pro Projekt cachen und über den vorhandenen Datei-Watcher/„Content refresh" invalidieren (der Editor kennt `onAssetAppeared/onAssetGone/onAssetMoved`, Hooks in `McpAssetHooks`), (c) wenn ein Cache nicht gewünscht ist: Sortierung ist nötig für Determinismus, also bleibt der Walk – dann wenigstens `reserve` und dokumentieren, dass das Limit keine Stall-Garantie ist. Mindestlösung: Kommentar korrigieren und `walkContentAssets`/`asset_list` zusammenführen.
- **Erwarteter Effekt**: Schätzung: Mehrfach-Aufrufe (`animator_info`, `clip_info`, `material_info`, `asset_list` …) auf großen Projekten (10⁴–10⁵ Dateien) vom 10–100-ms-Stall auf ~0 mit Cache; Vereinfachung: eine Walk-Implementierung statt zwei.
- **Risiko / Verifikation**: Determinismus (sortierte Reihenfolge) muss bleiben. Cache-Invalidierung bei Änderungen von außen (Git-Pull, externer Editor) – ohne Watcher bleibt nur der ungecachte Walk; „unsicher / messen". Tests: `tests/` zu `asset_list`/`walkContentAssets` mit Dotfiles, verschachtelten Ordnern, Limit.

---

### EDT-1204 – `tools/call`: Ergebnis wird kopiert, mit `dump(2)` pretty-gedruckt und zusätzlich als `structuredContent` ein zweites Mal gesendet
- **ID**: EDT-1204
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `McpBridge.cpp:444-477`, `:494-500` (`sendJson`); `McpToolsBatch.cpp:244-254`
- **Problem**: `const ToolResult r` verhindert Move: `json payload = r.isError ? ... : r.content;` kopiert den ganzen Ergebnisbaum (z. B. `hc_get` mit 200 Knoten samt Pins, `entity_get`). Danach `payload.dump(2)` (eingerückt, ~30–40 % größer als kompakt – und für ein Modell Tokens), dann `structuredContent = std::move(payload)`, und `sendJson` serialisiert das gesamte Objekt noch einmal und escaped dabei den Text-String ein zweites Mal. Auf der Leitung liegt jedes Ergebnis also zweimal (Pretty-Text + kompaktes Struct). Das drückt gegen `kMaxFrameBytes` (4 MiB, Bridge.h:63) früher als nötig. Im Batch: `slot["result"] = r.content;` (Z. 254) kopiert ebenfalls, `r` ist `const`.
- **Code-Auszug**:
```cpp
// McpBridge.cpp:454-460
json payload = r.isError ? json{ {"code", r.errorCode}, {"message", r.errorMessage} } : r.content;
json content = json::array({ json{ {"type","text"}, {"text", payload.dump(2)} } });
...
{ "structuredContent", std::move(payload) },
```
- **Lösung**: `ToolResult r = ...` (nicht const) und `json payload = r.isError ? ... : std::move(r.content);` — im Batch `slot["result"] = std::move(r)`-Variante entsprechend (r nicht const machen). Optional: `dump()` statt `dump(2)` (kompakt; MCP-Clients/Modelle lesen beides, spart Bytes und Tokens) – das ändert den sichtbaren Text, daher separat entscheiden.
- **Erwarteter Effekt**: Schätzung: ein Deep-Copy weniger pro Aufruf (O(Ergebnisgröße)); mit kompaktem `dump` ~25–40 % kleinere Antworten. Wirkt nur bei großen Antworten spürbar.
- **Risiko / Verifikation**: Der Move ist verhaltensneutral, solange `r` danach nicht mehr gelesen wird (`r.isError`, `r.imageBytes` werden nach dem Payload-Bau gelesen – `isError` vor dem Move zwischenspeichern; `imageBytes` sind eigene Member). `dump(2)`→`dump()` ändert den Text-Block; Tests, die den Text vergleichen (Bridge-Tests über echten Socket, `tests/` nach `structuredContent`/`"text"` greppen), vorher prüfen.

---

### EDT-1205 – Nicht authentifizierte Verbindungen haben kein Timeout; `Client::connectedMs` wird nie gelesen
- **ID**: EDT-1205
- **Kategorie**: E / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `McpBridge.h:134-138`, `McpBridge.cpp:237`, `:227-236`
- **Problem**: Jeder Client bekommt `connectedMs = nowMs`, das Feld wird aber nirgends ausgewertet (`grep connectedMs` trifft nur Deklaration und die eine Zuweisung). Eine Verbindung, die nie `auth` sendet, bleibt für immer in `m_clients`; vier solche Sockets (lokaler Prozess, kein Token nötig) füllen `kMaxClients = 4` und sperren damit alle echten Clients aus, bis der Editor die Bridge aus-/einschaltet. Das Token schützt die Befehle, nicht die Slots. Zugleich ist das Feld toter Code.
- **Code-Auszug**:
```cpp
// McpBridge.cpp:237
m_clients[ev.conn] = Client{ /*authed=*/false, nowMs };
// McpBridge.h:137  std::uint64_t connectedMs = 0;   // nie gelesen
```
- **Lösung**: In `update()` nach der Event-Schleife über `m_clients` iterieren (max. 4 Einträge) und unauthentifizierte Verbindungen älter als z. B. 5 s trennen (`disconnect` + `erase` + `notifyClientGone`, derselbe Pfad wie bei falschem Token). Alternativ das Feld entfernen, wenn man den Zustand bewusst akzeptiert.
- **Erwarteter Effekt**: Kein Performance-Gewinn; Robustheit (Slot-Verhungern) bzw. weniger toter Zustand.
- **Risiko / Verifikation**: Ein Test über echten Socket (Bridge ist ImGui-frei, laut Header testbar): verbinden, nichts senden, `update(now + 6 s)` → `connectionCount()==0`. Achtung: Frame-Zeit steht nur still, wenn der Editor idle-throttled; `nowMs` ist Frame-Zeit, ein sehr langsamer Start könnte legitime Clients treffen – großzügiges Timeout wählen.

---

### EDT-1206 – `project_build_status` kopiert den kompletten Build-Log (unter dem Lock des Build-Fensters) und filtert erst danach
- **ID**: EDT-1206
- **Kategorie**: C / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `McpToolRegistry.h:1296-1349` (`McpBuildStatus`, `buildStatus`), `McpToolsBuild.cpp:378-404`
- **Problem**: Der Hook liefert `McpBuildStatus` *by value* inklusive `std::vector<McpBuildLogLine> log` (Kommentar Z. 1297-1298: „a copy taken under the window's own lock"). Die Beschreibung des Tools sagt selbst, ein Toolchain-Lauf drucke „thousands of lines". Der Aufruf ist als Polling gedacht (`running`/`finished`), also wird bei jedem Poll auf dem UI-Thread der gesamte Log (alle Strings) kopiert, währenddessen der Lock gehalten wird, den der Build-Worker zum Schreiben braucht; danach filtert das Tool auf die letzten ≤ 2000 Zeilen (`kept` ist ein zweiter Vektor, nochmals Kopien).
- **Code-Auszug**:
```cpp
// McpToolsBuild.cpp:378, 389-402
McpBuildStatus st = hooks.buildStatus();
...
std::vector<McpBuildLogLine> kept;
for (const McpBuildLogLine& l : st.log) { if (...) continue; kept.push_back(l); }
if (kept.size() > maxLines) kept.erase(kept.begin(), kept.end() - maxLines);
```
- **Lösung**: Hook-Signatur um die Filter erweitern: `buildStatus(const McpBuildLogFilter& f)` (`withLog`, `maxLines`, `minSeverity`, `step`), der Editor-Hook filtert unter dem Lock rückwärts vom Ende und kopiert nur die ≤ maxLines Treffer. Das Tool behält `logLinesMatched`, indem der Hook die Trefferzahl mitliefert.
- **Erwarteter Effekt**: Schätzung: Kopieraufwand von O(Loggröße) auf O(maxLines) pro Poll, kürzere Lock-Haltezeit; bei Logs mit 10⁴–10⁵ Zeilen merklich.
- **Risiko / Verifikation**: Änderung der Hook-Signatur berührt die Verdrahtung in `EditorApplication::setupMcpTools` und Tests mit synthetischem `McpBuildStatus`. `logLinesMatched`/`logTruncated` müssen identisch bleiben (Test mit mehr Zeilen als `maxLines`, mit `minSeverity`, mit `step`).

---

### EDT-1207 – Uuid→Pfad-Auflösung durch Vollwalk mit Datei-Öffnung pro Asset (`RefNames`), dasselbe Muster in Prefab/Sequence
- **ID**: EDT-1207
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `McpToolsAnimator.cpp:91-121` (`RefNames`), genutzt `:456`, `:608`, `:980`, `:1132`; gleiches Muster außerhalb des Slice: `McpToolsPrefab.cpp:711-712`, `McpToolsSequence.cpp:145-148`
- **Problem**: Um eine gespeicherte uuid (Clip/BlendSpace einer State/Sample) in einen Pfad zurückzuübersetzen, läuft `RefNames::build` `walkContentAssets` (bis 2000 Assets, Walk + Sort + Header-Sniff je Datei) und öffnet danach *jede* Datei, um die uuid im META-Chunk zu lesen (`assetUuidOfFile`), und baut daraus eine `std::map` mit Pair-Key. Das passiert bei jedem Detail-Aufruf (`animator_info`, `blendspace_info`) und bei jedem Set-Tool, dessen Ergebnis einen Zustand mit Referenzen zurückgibt (Z. 608-620, 1132-1137), auf dem UI-Thread. „Built ONCE per call" bedeutet: einmal pro Aufruf, nicht einmal pro Projekt.
- **Code-Auszug**:
```cpp
// McpToolsAnimator.cpp:109-115
for (const ContentAsset& a : walkContentAssets(*m_content, {AnimationClip, BlendSpace}, 2000, truncated)) {
    const HE::UUID id = HE::AssetRefs::assetUuidOfFile(a.abs);   // Datei öffnen
    if (!(id == HE::UUID{})) m_byId.emplace(std::make_pair(id.hi, id.lo), a.rel);
}
```
- **Lösung**: Ein projektweiter, lazy gebauter Index `uuid → relPath` neben `EditorAssetTypeCache` (gleiche Invalidierung wie `EditorAssetTypeCache::invalidate`, gespeist von `onAssetAppeared/Gone/Moved`), den Animator/Prefab/Sequence/… teilen. Für die Set-Tools zusätzlich: die gerade aus den Argumenten aufgelösten Pfade direkt zurückgeben statt den Index zu befragen (der Aufrufer kennt `clip`-Pfad schon).
- **Erwarteter Effekt**: Schätzung: Detailaufruf von O(Assets + Dateiöffnungen) auf O(1) nach dem ersten Aufruf; ein weniger Walk-Duplikat in drei Dateien.
- **Risiko / Verifikation**: Veraltete Einträge (Datei extern verschoben) → `missing: true` muss weiterhin erscheinen; Tests mit gelöschtem Clip. Cache-Invalidierung sorgfältig (Asset-Move über Collab-Broadcast). „Unsicher / messen", wie groß typische Projekte sind.

---

### EDT-1208 – Gleiche Tor-Logik (Pfad, Engine-readonly, Play-Mode, Lock, Dirty-Tab) in ≥ 11 Tool-Dateien und 15 Hook-Structs mit denselben vier `std::function`s
- **ID**: EDT-1208
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `McpToolsAnimator.cpp:206-235`, `McpToolsClip.cpp:177-204`, `McpToolsHc.cpp:422-478`, `McpToolsAsset.cpp:146-154`; ebenso (nicht Teil des Slice) Input, Material (2×), Particle, Prefab, Scene, Sequence, Type, Widget (`grep '"play_mode"\|"locked_by_other"'`: je 1–2 Treffer in 12 Dateien). Hook-Structs: `McpToolRegistry.h:358-1169` (`McpAssetHooks`, `McpWidgetHooks`, `McpInputHooks`, `McpMaterialHooks`, `McpPrefabHooks`, `McpTypeHooks`, `McpParticleHooks`, `McpAnimatorHooks`, `McpClipHooks`, `McpSequenceHooks`, …)
- **Problem**: Jede Familie hat ihre eigene `Doc`/`openDoc`-Kopie mit denselben Prüfungen in derselben Reihenfolge und nur leicht abgewandelten Fehlertexten, und jedes Hook-Struct deklariert wieder `isPlaying`, `lockedByOther`, `isDirty`, `reloadFromDisk` (50 Treffer dieser Namen in der Header-Datei) samt wiederholtem Kommentar „Same question, same shape and same optimistic asset policy as McpHcHooks::lockedByOther". `McpToolCommon.h` begründet `checkPath` ausdrücklich mit „two copies of a boundary are two boundaries" – bei den Gates gilt dasselbe, nur wurde dort nicht zusammengeführt.
- **Code-Auszug**:
```cpp
// Animator:206-235 ≈ Clip:177-204 ≈ Hc:425,464 ...
if (p.engine) { d.failure = failEngineReadOnly(p.rel); return d; }
if (h.isPlaying && h.isPlaying()) { d.failure = ToolResult::fail("play_mode", ...); return d; }
if (h.lockedByOther && h.lockedByOther(p.rel)) { ... "locked_by_other" ... }
if (h.isDirty && h.isDirty(p.rel)) { ... "dirty" ... }
```
- **Lösung**: (1) `struct McpAssetGate { std::function<bool()> isPlaying; std::function<bool(const std::string&)> lockedByOther, isDirty, reloadFromDisk; }` in `McpToolCommon.h`; die Familien-Hooks erben/enthalten es (`struct McpAnimatorHooks : McpAssetGate {…}`). (2) `std::optional<ToolResult> gateWrite(const McpAssetGate&, const PathCheck&, GateText)` in `McpToolCommon.cpp`, wobei `GateText` die familienspezifische Begründung für `play_mode`/`dirty` liefert (die Texte unterscheiden sich fachlich). (3) Dann pro Familie nur noch Parsen + `gateWrite`. Die Verdrahtung in `EditorApplication::setupMcpTools` füllt das Gate einmal statt zehnmal.
- **Erwarteter Effekt**: Vereinfachung: grob 300–400 Zeilen weniger über 12 Dateien, ein Ort für die Reihenfolge der Prüfungen. Keine Laufzeitwirkung.
- **Risiko / Verifikation**: Fehlercodes und -texte sind ein Teil der Tool-Schnittstelle (Modelle lesen sie): Codes exakt erhalten, Texte pro Familie parametrisieren. Tests, die `play_mode`/`locked_by_other`/`dirty` pro Familie prüfen (`tests/` nach diesen Codes greppen), laufen unverändert. Reihenfolge der Prüfungen (Engine → Play → Lock → Dirty; bei Hc: Play → Key → Lock) darf nicht kippen, sonst ändern sich Refusal-Prioritäten.

---

### EDT-1209 – `McpToolRegistry.h` (1592 Zeilen, schwere Includes) wird über `McpBridge.h` → `EditorApplication.h` in 78 Übersetzungseinheiten gezogen
- **ID**: EDT-1209
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `EditorApplication.h:46` (`#include "McpBridge.h"`), `McpBridge.h:41` (`#include "McpToolRegistry.h"`), `McpToolRegistry.h:24-36` (Includes `Renderer/IRenderer.h`, `Scripting/ScriptTypes.h`, `Types/*`, `<nlohmann/json.hpp>`)
- **Problem**: `EditorApplication.h` wird von 78 Dateien eingebunden (`grep -l`), und bringt damit `nlohmann/json.hpp`, `IRenderer.h` (nur für `EditorCameraOverride` in den Screenshot-Hooks) und ca. 1100 Zeilen reine Hook-Struct-Deklarationen in jede davon, obwohl nur `EditorApplication.cpp` (`setupMcpTools`) und die `Mcp*.cpp`-Dateien sie brauchen. Der Header ist Prosa-schwer (Block-Kommentare über Hunderte Zeilen), was Lesbarkeit hilft, aber Parsezeit kostet.
- **Code-Auszug**:
```cpp
// EditorApplication.h:46
#include "McpBridge.h"
// McpBridge.h:41
#include "McpToolRegistry.h"
```
- **Lösung**: (1) `McpToolRegistry.h` in `McpTool.h` (ToolResult, McpTool, McpToolRegistry, base64 – ohne Renderer/Scripting/Hook-Structs) und `McpToolHooks.h` (alle `Mcp*Hooks`, Registrierungsfunktionen) aufteilen; (2) `EditorApplication` hält `std::unique_ptr<HE::Ed::McpBridge>` mit Forward-Declaration statt Wert-Member (Bridge-Kommentar `EditorApplication.h:838-861` verlangt nur die Destruktionsreihenfolge relativ zu `m_mcpCameras` – ein `unique_ptr` an derselben Stelle behält sie); `McpClaudeProbe.h`/`McpLockBook.h` bleiben leicht. (3) Prosa-Blöcke in `docs/` verschieben und im Header per Verweis kürzen.
- **Erwarteter Effekt**: Schätzung: weniger Parse-Arbeit in 78 TUs (json.hpp dominiert); Größenordnung 1–3 % der Editor-Gesamtbuildzeit bei inkrementellen Änderungen am Header, nicht gemessen.
- **Risiko / Verifikation**: Reines Include-/Ownership-Refactoring, aber der `unique_ptr` verlagert die Konstruktion nach `EditorApplication.cpp`; Tests, die `McpBridge` direkt nutzen (Socket-Tests), sind unberührt. Mit `-ftime-trace` an einem typischen Panel-TU messen (unsicher / messen).

---

### EDT-1210 – `checkPath` prüft die Konfinierung nur lexikalisch; Symlinks innerhalb der Content-Wurzel werden nicht aufgelöst
- **ID**: EDT-1210
- **Kategorie**: F (Korrektheit/Sicherheit, außerhalb des Perf-Rasters)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `McpToolCommon.cpp:162-180` (`lexically_normal`, `isUnder`), Kommentar `:165-167`; Gegenbeispiel `McpClientSetup.cpp:195` nutzt `weakly_canonical`
- **Problem**: Der Kommentar nennt die Prüfung „the check that actually keeps the promise: resolution is symbolic, so a path can come back looking fine and still point outside". Implementiert ist aber nur `std::filesystem::path(absRaw).lexically_normal()` und ein Präfixvergleich der Pfadelemente. Ein Symlink (oder Junction) im Content-Ordner, der aus dem Projekt zeigt, besteht die Prüfung; `asset_delete`/`asset_move`/`asset_list` wirken dann auf Dateien außerhalb des Projekts (soweit das Ziel `.hasset`-Namen/Header-Sniffs durchlässt). Das Versprechen „no file access" in `McpToolRegistry.h:5-7` ist damit schwächer als dokumentiert.
- **Code-Auszug**:
```cpp
// McpToolCommon.cpp:163
std::filesystem::path abs = std::filesystem::path(absRaw).lexically_normal();
...
if (!isUnder(abs, contentRoot) && !(engineRoot.empty() ? false : isUnder(abs, engineRoot)))
```
- **Lösung**: Nach dem lexikalischen Test `weakly_canonical(abs, ec)` bilden (und ebenso für die Roots einmalig) und den Präfixvergleich auf die kanonischen Pfade wiederholen; `p.abs` als kanonische Form zurückgeben. Kosten: wenige `lstat`-Aufrufe pro Tool-Aufruf.
- **Erwarteter Effekt**: Keine Performance; schließt die Lücke zwischen Kommentar und Verhalten.
- **Risiko / Verifikation**: Auf macOS ist `/var` ein Symlink auf `/private/var`: die Roots müssen *ebenfalls* kanonisiert werden, sonst lehnt jeder Pfad ab. Windows: Junctions/8.3-Namen testen. Unsicher: ob Projekte legitime Symlinks im Content nutzen (dann Verhalten ändern, gegebenenfalls Option). Test: Symlink in temporärem Content auf Verzeichnis außerhalb → `asset_resolve` muss `invalid_path` liefern.

---

### EDT-1211 – Lokale Kopien gemeinsamer Hilfen (`strArg`, `objectSchema`, `lowered`, `pinTypeName`, `uuidJson`)
- **ID**: EDT-1211
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `McpToolsEntity.cpp:95-100`, `:234-243` und `McpToolsHc.cpp:363-368`, `:379-388` (Kopien von `McpToolCommon.cpp:18`, `:51`); `lowered` in `McpToolsApi.cpp:44`, `McpToolsBuild.cpp:29`, `McpToolsHc.cpp:203` (+ `McpToolsSettings.cpp:43`); `pinTypeName` in `McpToolsApi.cpp:51` und `McpToolsHc.cpp:92` (identische Tabelle für `HorizonCode::PinType`; weitere in Material:707, Type:76); `uuidJson` in `McpToolsAnimator.cpp:79`, `McpToolsAsset.cpp:78` (+ Material:97, Particle:236, Prefab:113)
- **Problem**: Entity und Hc binden `McpToolCommon.h` nicht ein und definieren `strArg`/`objectSchema` selbst (anonymer Namespace, identische Semantik); `lowered` existiert viermal, `uuidJson` fünfmal (Animator nutzt `static_cast<uint64_t>`, Asset nicht – gleiches JSON). Zwei Pin-Namenstabellen für denselben Enum. Divergenz-Risiko genau der Art, die `McpToolCommon.h` für Grenzfälle beschreibt.
- **Code-Auszug**:
```cpp
// McpToolsEntity.cpp:95 und McpToolsHc.cpp:363 – identisch zu McpToolCommon.cpp:18
std::string strArg(const json& args, const char* key) { if (!args.is_object()) return {}; ... }
```
- **Lösung**: `McpToolCommon.h` einbinden, lokale Kopien löschen; `lowered`, `uuidJson`, `pinTypeName(HorizonCode::PinType)` nach `McpToolCommon` (bzw. `HcNodeDocs`/Hc-Header) heben.
- **Erwarteter Effekt**: Vereinfachung, ca. 60–80 Zeilen; kein Laufzeiteffekt.
- **Risiko / Verifikation**: Reines Löschen/Umhängen; Kompilierung prüft (Mehrdeutigkeit zwischen lokaler und Common-Variante tritt nur auf, solange beide sichtbar sind). Bestehende Tests genügen.

---

### EDT-1212 – Kleine O(n²)-Stellen und Allokationen in den HorizonCode-/API-Tools
- **ID**: EDT-1212
- **Kategorie**: D / C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `McpToolsHc.cpp:533-545` (`droppedLinks`), `:996`, `:1073`, `:1077-1079` + `:1104-1106` (`idsBefore`), `:353-359` + `:739` (`apiAllowed` je Zeile), `:722-725`/`:741-744` (`lowered` je Kandidat); `McpToolsApi.cpp:586-589` (`lowered` je Zeile)
- **Problem**: `droppedLinks` prüft jeden alten Link per `any_of` gegen alle neuen Links: O(L²) plus vollständige Kopie von `d.g->links` vor jeder Mutation (`before`). `hc_connect` kopiert außerdem alle Knoten-IDs und sucht jeden Knoten mit `std::find` in dieser Liste (O(N²)). `apiAllowed` baut für jede Registry-Zeile einen neuen `std::vector<const char*>`; `hc_node_types`/`api_list` allokieren pro Kandidat 2–3 lowercase-Strings.
- **Code-Auszug**:
```cpp
// McpToolsHc.cpp:538
const bool still = std::any_of(g.links.begin(), g.links.end(), [&b](const HC::Link& l){ ... });
// :1105
if (std::find(idsBefore.begin(), idsBefore.end(), n.id) == idsBefore.end())
```
- **Lösung**: Links nach dem Muster „sort + `set_difference`" oder über `unordered_set<uint64>` (Schlüssel `CollabDocSync::linkKey`) vergleichen; für neue Knoten reicht die Differenz `nextId` vorher/nachher (Ids werden fortlaufend vergeben, `n.id = d.g->nextId` Z. 873) oder `unordered_set`. `apiAllowed`: Gruppenvektor einmal pro Dokument bauen; `lowered`-Treffer vorberechnen oder `strcasestr`-artigen Vergleich ohne Allokation.
- **Erwarteter Effekt**: Schätzung: nur bei Graphen mit mehreren tausend Links/Knoten merkbar; sonst im Rauschen (Editor-only, pro Tool-Aufruf, nicht pro Frame).
- **Risiko / Verifikation**: Ausgabe (`replacedLinks`, `spawnedNodes`) muss gleiche Reihenfolge behalten (Reihenfolge der alten `links` bzw. `nodes`). Unsicher / messen, ob es relevante Graphgrößen gibt.

---

### EDT-1213 – Registry: `tools/list` wird bei jedem Aufruf neu gebaut, lineares `find`, doppelte Handler-Felder, Hooks-Kopie je API-Zeile
- **ID**: EDT-1213
- **Kategorie**: A / C
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `McpToolRegistry.cpp:96-101` (`find`), `:113-125` (`listPayload`), `McpToolRegistry.h:109-117` (`handler` + `handlerCtx`, `invoke`), `McpToolRegistry.cpp:69-77`; `McpToolsApi.cpp:462` (`[row, hooks]`, Kopie von `McpApiHooks` pro Zeile), `:633-637`
- **Problem**: Neben den ~120 handgeschriebenen Tools registriert `registerApiTools` eine Tool-Instanz je pure `HE::api`-Zeile (Anzahl nicht gemessen, vermutlich hunderte). `listPayload` kopiert dann pro Aufruf alle Schemas in ein neues JSON; `find` vergleicht linear Strings (in `batch` bis 100×). Jedes API-Tool trägt eine eigene Kopie von drei `std::function`s (`McpApiHooks`). Zwei Handler-Felder pro Tool (`handler`, `handlerCtx`) erfordern den Wrapper in `add`, obwohl nur Batch/Screenshot (und `add` selbst) `handlerCtx` nutzen (`grep`: 3 Dateien).
- **Code-Auszug**:
```cpp
// McpToolRegistry.cpp:96
for (const auto& t : m_tools) if (t.name == name) return &t;
// McpToolsApi.cpp:462
t.handler = [row, hooks](const json& args) -> ToolResult { ...
```
- **Lösung**: `unordered_map<std::string, size_t>` neben `m_tools` (Registrierung ist einmalig); `listPayload` einmal bauen und cachen (Invalidierung in `add`/`clear`); `McpApiHooks` als `shared_ptr` capturen; langfristig nur `handlerCtx` (alle Lambdas bekommen den ungenutzten ctx-Parameter) – niedriger Nutzen, nur bei Gelegenheit.
- **Erwarteter Effekt**: Schätzung: marginal (Mikrosekunden–niedrige Millisekunden pro `tools/list`); der eigentliche Hebel ist die Antwortgröße (siehe „Unsicher").
- **Risiko / Verifikation**: Cache muss bei `registry.add` nach dem ersten `tools/list` (z. B. spätes `registerBatchTool`) invalidiert werden; Test „Reihenfolge der Tools in tools/list" beibehalten (Batch als letztes).

---

### EDT-1214 – Kommentar `McpTool::mutates` beschreibt ein Verhalten, das es nicht gibt
- **ID**: EDT-1214
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `McpToolRegistry.h:99-101`; tatsächliche Verwendung `McpBridge.cpp:478`, `McpToolsBatch.cpp:257`
- **Problem**: Der Kommentar sagt „Mutating tools are refused while play-in-editor runs, and are the ones written to the console log". `grep '\->mutates\|\.mutates'` findet nur die Log-Zeile in der Bridge und im Batch; ein Play-Mode-Refuse existiert dort nicht (jede Familie prüft selbst, siehe EDT-1208, und die Entity-Tools über das Gateway). Wer sich auf den Kommentar verlässt, geht von einer zentralen Sperre aus, die fehlt.
- **Code-Auszug**:
```cpp
// McpToolRegistry.h:99
// Mutating tools are refused while play-in-editor runs, and are the ones
// written to the console log. Reading tools are neither.
bool mutates = false;
```
- **Lösung**: Kommentar auf „wird im Konsolen-Log vermerkt" kürzen — oder die zentrale Sperre tatsächlich in `McpBridge::dispatch` implementieren und die zehn Familien-Prüfungen aus EDT-1208 darauf stützen (Verhaltensänderung: Entity-Tools lieferten dann denselben Code, aber Hc/Asset-Familien hätten ihre eigenen Texte nicht mehr).
- **Erwarteter Effekt**: Weniger Irreführung.
- **Risiko / Verifikation**: Nur Kommentar: keins. Zentrale Sperre: Tests der einzelnen `play_mode`-Texte.

---

### EDT-1215 – `McpClaudeProbe.cpp` und `McpClientSetup.cpp` duplizieren `commandLine` und Trim-Helfer
- **ID**: EDT-1215
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `McpClaudeProbe.cpp:12-17` ↔ `McpClientSetup.cpp:15-20` (`commandLine`, zeichengleich); `McpClaudeProbe.cpp:19-25` (`trimmed`) ↔ `McpClientSetup.cpp:50-59` (`firstLine` mit eigener Trim-Logik)
- **Problem**: Die Probe-Datei inkludiert bereits `McpClientSetup.h`, definiert aber `commandLine` neu (mit dem Kommentar „Same transcript shape the 'Add to Claude' button prints"). `trimmed` und das Trim in `firstLine` sind dieselbe Operation.
- **Code-Auszug**:
```cpp
std::string commandLine(const std::filesystem::path& exe, const std::vector<std::string>& args)
{ std::string line = exe.string(); for (const std::string& a : args) { line += ' '; line += a; } return line; }
```
- **Lösung**: `commandLine` (und `trimmed`) in `McpClientSetup.h` als `inline`/freie Funktion deklarieren und in der Probe wiederverwenden.
- **Erwarteter Effekt**: ~15 Zeilen weniger; keine Laufzeitwirkung (läuft einmal pro Klick/Probe, Worker-Thread).
- **Risiko / Verifikation**: Keins; Tests `McpClientSetup`/`McpClaudeProbe` mit synthetischen `Proc::Result` bleiben gültig.

---

### EDT-1216 – Statusleiste und Kamera-Labels bauen pro Frame Strings, solange die Bridge läuft
- **ID**: EDT-1216
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `McpStatusBar.cpp:38-55`, `:77-87` (`FooterWidth`), `:89-97` (`DrawFooter`); `McpCameraGizmos.cpp:38-43`, `:127` (`labelFor` je Kamera je Frame)
- **Problem**: `FooterWidth` und `DrawFooter` rufen je `stateOf` + `textFor` (neuer `std::string`, im Busy-Fall `snprintf`) – dasselbe Ergebnis zweimal pro Frame, nur solange die Bridge läuft (Standard: aus, `State::Off` kehrt vorher zurück). `drawViewportLabels` baut pro Kamera pro Frame `labelFor(id)` (`snprintf` + `std::string`) und ruft `CalcTextSize`. Beides ist sehr klein (≤ 4 Clients) und nur aktiv, wenn die Funktion benutzt wird.
- **Code-Auszug**:
```cpp
// McpCameraGizmos.cpp:127
const std::string label = labelFor(id);
const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
```
- **Lösung**: Optional: Footer-Text pro Frame einmal cachen (statische Struktur mit `count`/`state` als Schlüssel) und `labelFor` in einen kleinen `char[16]`-Puffer schreiben lassen. Nur sinnvoll, wenn man ohnehin hier arbeitet.
- **Erwarteter Effekt**: Nicht messbar; nur der Vollständigkeit halber (Schätzung: < 1 µs pro Frame).
- **Risiko / Verifikation**: Keins; die Kommentare nennen `FooterWidth` und `DrawFooter` als synchron zu haltende Paarung (McpStatusBar.cpp:34-37) – ein Cache würde genau das sicherstellen.

---

### EDT-1217 – `MaterialEditorPanel.h` zieht `EditorUI.h`, `imgui.h`, `CollabDocSync.h`, `AssetAutosave.h` für Vorwärtsdeklarierbares
- **ID**: EDT-1217
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `MaterialEditorPanel.h:2-7`
- **Problem**: Das Interface braucht `AppContext&` (Forward-Declaration `struct AppContext;` steht bereits in `EditorUI.h:9`), `ImVec2` (nur in `render`), `CollabDocSync::DocBindings` (ein `std::vector<DocBinding>`, Rückgabe *by value* → vollständiger Typ nötig) und `HE::Ed::AssetSnapshotSource` (nur als Vector-Referenz → Forward-Declaration reicht). Es hat nur vier Includer (`ContentBrowserPanel.cpp`, `EditorApplication.cpp`, `EditorUI.cpp`, `MaterialEditorPanel.cpp`), daher geringer Hebel.
- **Code-Auszug**:
```cpp
#include "CollabDocSync.h"
#include "EditorUI.h"
#include <imgui.h>
...
#include "AssetAutosave.h"
```
- **Lösung**: `struct AppContext;`, `struct ImVec2;`/`<imgui.h>` bleibt wegen Wertparameter `const ImVec2&` (Referenz → Forward-Declaration genügt), `namespace HE::Ed { struct AssetSnapshotSource; }`; nur `CollabDocSync.h` bleibt. Auch das Paar `isDirty`/`isDirtyByContentPath` und `reload…` existiert wortgleich in `InputAssetPanel` (laut Kommentar Z. 30-34) – siehe Cross-Modul-Hinweise.
- **Erwarteter Effekt**: Vernachlässigbar (4 TUs).
- **Risiko / Verifikation**: Prüfen, dass die vier Includer die Typen anderweitig einbinden (Compile-Fehler zeigen es sofort).

---

## Cross-Modul-Hinweise

- `src/HE_Editor/EditorCommands.cpp:419-429` (`entityByUuid`): siehe EDT-1201; auch `TaskBoardPanel.cpp:547,550,579,603` ruft `HE::Ed::entityByUuid` in Panel-Draw-Code (Zeilen 547/550 vermutlich pro sichtbarer Zeile/Link) — dort wird derselbe O(E)-Scan pro Frame ausgeführt; mit dem Fix von EDT-1201 entfällt auch dieses Problem (Draw-Pfad, daher höher zu gewichten als im MCP-Fall).
- `src/HE_Editor/McpToolsInput.cpp:327`, `McpToolsMaterial.cpp:546,1643`, `McpToolsParticle.cpp:402`, `McpToolsPrefab.cpp:322,711`, `McpToolsSequence.cpp:145,592`, `McpToolsType.cpp:522`: weitere Aufrufer von `walkContentAssets` (EDT-1203) und eigene uuid→Pfad-Walks (EDT-1207); Gate-Duplikate (EDT-1208) in Input, Material, Particle, Prefab, Scene, Sequence, Type, Widget; lokale `pinTypeName`/`uuidJson`/`lowered` in Material, Particle, Prefab, Settings, Type (EDT-1211).
- `src/HE_Editor/InputAssetPanel.*`, `TypeAssetPanel.*`, `ParticleGraphEditorPanel.*`, `AnimatorStateMachineEditorPanel.*`, `BlendSpacePanel.*`, `SkeletalMeshEditorPanel.*`: jedes Panel liefert dasselbe Quartett `isDirty / isDirtyByContentPath / reloadByContentPath / appendDirtyPaths` (MaterialEditorPanel.h:35-40 verweist ausdrücklich auf „InputAssetPanel's pair"). Ein gemeinsames Interface (`IAssetTabHost`) würde die Verdrahtung in `setupMcpTools` und die Bestätigungsdialoge beim Beenden vereinheitlichen.
- `src/HE_Scene/src/SceneSerializer.cpp:3011` (`isKnownComponentKey`): für EDT-1202 wäre eine billige `hasComponentKey`-Prüfung die passende Ergänzung.
- `src/HE_Editor/EditorApplication.h:46-48`: Include-Kette für EDT-1209; Destruktionsreihenfolge-Kommentar bei `:838-861`.
- `scripts/he_mcp.py` (Shim, nicht gelesen): Rahmengröße 4 MiB wird in Bridge.h/Registry.h erwähnt; EDT-1204 sollte mit dem Shim abgestimmt werden, wenn `dump(2)` entfällt.

## Unsicher / Messung nötig

- Größe der `tools/list`-Antwort: wie viele `api_*`-Tools werden aus `HE::api::registry()` erzeugt (Z. 633-637) und wie viele Bytes/Tokens kostet das pro Client-Kontext? Wenn das Hunderte Tools sind, wäre eine gestufte Liste („api_list" statt je ein Tool) der größte Hebel bei den Modell-Kosten; hier nicht bewertbar.
- Typische Szenen-/Projektgrößen für EDT-1202/1203/1207 (Entities, `.hasset`-Dateien) und reale Dauer eines `entity_list`, `animator_info`-Details und `asset_list` rekursiv.
- Wirkung von EDT-1209 auf die Buildzeit: `-ftime-trace` an einem typischen Panel-TU (Anteil `nlohmann/json.hpp` und `IRenderer.h`).
- EDT-1205: realistische Timeout-Wahl, weil `nowMs` Frame-Zeit ist und der Editor bei Idle-Throttling selten tickt.
- EDT-1210: ob Projekte legitim Symlinks im Content-Verzeichnis haben (macOS `/var`↔`/private/var`, Windows-Junctions).
- Build-Log-Größe in EDT-1206 (nur aus der Tool-Beschreibung „thousands of lines" geschlossen).

## Gelesen

Ja, alle 27 Dateien des Slice 12 vollständig gelesen (MaterialEditorPanel.h, McpBridge.cpp/.h, McpCameraGizmos.cpp/.h, McpClaudeProbe.cpp/.h, McpClientCameras.h, McpClientSetup.cpp/.h, McpLockBook.h, McpStatusBar.cpp/.h, McpToolCommon.cpp/.h, McpToolRegistry.cpp/.h (beide Hälften), McpToolsAnimator.cpp, McpToolsApi.cpp/.h, McpToolsAsset.cpp, McpToolsBatch.cpp, McpToolsBuild.cpp, McpToolsClip.cpp, McpToolsCollab.cpp, McpToolsEntity.cpp, McpToolsHc.cpp); zusätzlich gezielt gelesen: `EditorCommands.cpp:400-430`, `EditorApplication.cpp:11355-11398`, `EditorSettingsPanel.cpp:762-791`, `HorizonWorld.h` (Id-Index).
