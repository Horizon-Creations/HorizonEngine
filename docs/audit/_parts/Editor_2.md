# Editor Slice 2 – CinematicTimeline.*, Collab* (ActivityBar, Controller, DocSync, EntityOps, Panel)

## Zusammenfassung

Der Slice (11 Dateien, ca. 10.000 Zeilen) ist funktional sorgfältig und sehr gut kommentiert, aber `CollabController` ist ein 4667-Zeilen-Gott-Objekt (davon 1215 Kommentarzeilen in der .cpp und 908 von 1682 Zeilen in der .h), und an mehreren Stellen laufen pro Frame Operationen, die mit der Szenen- bzw. Dokumentgröße skalieren. Die schwersten Funde sind die lineare Netz-ID-Tabelle (`m_netIds`: O(N²) beim Seeding, lineare Suche pro Frame und pro Remote-Nachricht, Negativ-Treffer scannen die ganze Registry) und die Voll-Serialisierung des Level-Script-/GameInstance-Graphen pro Frame in einer Session. Dazu kommen blockierende Datei-I/O auf dem UI-Thread, die Datei vor dem Typ-Gate liest, Syscalls pro Frame im Collaboration-Fenster und viel Duplikation (Adapter, Batch-Approve/Deny, Teilnehmer-Lookup). Der Cinematic-Streifen ist unauffällig (Eigenlogik sauber getrennt und testbar); nur Row-Clipping fehlt und `draw()` ist sehr lang. Funde: 2 Hoch, 6 Mittel, 7 Niedrig. Nichts davon wurde gemessen; Schätzungen sind als solche gekennzeichnet.

---

### EDT-201 – Level-Script-/GameInstance-Graph wird in einer Session jeden Frame vollständig serialisiert
- **ID**: EDT-201
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/CollabDocSync.cpp:127-219` (diffInto, hasLocalChanges); Aufrufer `src/HE_Editor/EditorApplication.cpp:10241-10248`, `10339-10353` (fremder Slice, nur zur Belegung)
- **Problem**: `updateAssetCollabSync` läuft pro Frame, sobald eine Session aktiv ist. Für die Tabs `LevelScriptPanel::kTabPath` und `GameInstancePanel::kTabPath` gibt es kein Dirty-Flag. Gehalten: `publishDocDeltas` setzt `alwaysDiff` und ruft jeden Frame `diffInto`. Nicht gehalten: `hasLocalChanges` wird jeden Frame aufgerufen. Beide Pfade rufen `capture()` für alle 7 `Kind`s, also `nodeToJson` für jeden Node, jede Variable, jeden Kommentar, und vergleichen/ersetzen anschließend die Strings. Der Kommentar bei `publishDocDeltas` (EditorApplication.cpp:10319-10322) sagt selbst, dass das "a few hundred-node graphs ... milliseconds of pure JSON" kostet und deshalb nicht pro Frame laufen darf; `alwaysDiff` tut genau das für diese zwei Tabs. Solange der Tab offen ist, ist es reine Verschwendung (Idle-Frames).
- **Code-Auszug**:
```cpp
// CollabDocSync.cpp:205-207
for (int k = 0; k < kKindCount; ++k)
{
    const auto current = capture(doc, static_cast<Kind>(k));
// EditorApplication.cpp:10339-10342
const bool alwaysDiff = (key == LevelScriptPanel::kTabPath || key == GameInstancePanel::kTabPath);
const bool dirty = alwaysDiff || EditorUI::tabHasUnsavedEdits(absPath);
```
- **Lösung**: (1) Billigen Änderungszähler an den zwei Dokumenten einführen: `HorizonCode::Graph` bekommt ein `uint64_t revision`, das die Editier-Funktionen (GraphEditor/`HcGraphHost::Host::onEdit`, MCP `hc.endEdit`) erhöhen; alternativ hängt `EditorApplication` einen Zähler an `onEdit`. (2) `hasLocalChanges`/`diffInto` nur ausführen, wenn sich der Zähler seit dem letzten Diff geändert hat (Zähler im `DocMirror` merken: `uint64_t seenRevision`). (3) Ohne Zähler als Zwischenlösung: Diff nur alle ~250 ms drosseln (`nowMs - lastDiffMs`), da Deltas ohnehin gebündelt werden. 
- **Erwarteter Effekt**: Schätzung: bei einem Graphen mit einigen hundert Nodes spart das pro Frame etwa 0,1-2 ms CPU plus Tausende kurzer String-Allokationen im Idle; nur in Sessions mit offenem Tab. Messen mit Scoped-Timer um `diffInto`.
- **Risiko / Verifikation**: Die Revisionszähler müssen an allen Mutationsstellen erhöht werden (MCP-Tools, Undo/Redo, Remote-Apply); ein vergessener Pfad würde Edits nicht senden. Das Throttling-Variante ist risikoärmer (nur Latenz). Absichern über `tests/test_collab_docsync.cpp` plus einen Test "Edit ohne Zähler-Bump wird trotzdem gefunden" (bei Throttling) bzw. je Mutationspfad.

---

### EDT-202 – Netz-ID-Tabelle ist ein Vektor: O(N²) beim Seeding, lineare Suche pro Frame, Registry-Scan bei jedem Fehltreffer
- **ID**: EDT-202
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Editor/CollabController.cpp:2702-2783` (netIdFor, entityForNetId, forgetNetId, seedNetIds), `2801-2812` (publishDestroy); `CollabController.h:1590` (`m_netIds`)
- **Problem**: `m_netIds` ist `std::vector<std::pair<uint64_t,uint32_t>>`. `netIdFor(handle)` durchsucht linear nach dem Handle, `entityForNetId(net)` linear nach der Net-ID. Konsequenzen: (a) `seedNetIds()` ruft für jede Entity `netIdFor`, das jedes Mal alle bisher eingetragenen Paare vergeblich durchläuft: N²/2 Vergleiche (10k Entities: ca. 5·10⁷; 100k: ca. 5·10⁹, also ein mehrsekündiger Freeze beim Join/Hosten). (b) Jeder Frame mit selektierter Entity in einer Session ruft `subjectFor` und `publishComponents` (beide `netIdFor`) linear auf (EditorApplication.cpp:5010, 5066), dazu `entityForNetId` je Remote-Transform (Callback `onTransform`, bis 30 Hz) und je fremder Selektion in der Presence-Schleife (EditorApplication.cpp:4666). (c) Bei einem Fehltreffer (unbekannte Entity, z. B. Presence-Selektion auf eine Entity, die lokal noch nicht existiert) scannt `entityForNetId` die gesamte Registry (`reg.view<EntityIdComponent>().each`) und cached das Negativergebnis nicht, also wiederholt sich das jeden Frame. (d) `forgetNetId` ist `remove_if` über alles.
- **Code-Auszug**:
```cpp
std::uint64_t CollabController::netIdFor(std::uint32_t entityHandle)
{
    for (const auto& [net, handle] : m_netIds)
        if (handle == entityHandle) return net;
...
void CollabController::seedNetIds()
{
    m_netIds.clear();
    m_world->registry().view<entt::entity>().each([&](auto e) {
        netIdFor(static_cast<std::uint32_t>(entt::to_integral(e)));
    });
```
- **Lösung**: (1) Zwei Hash-Maps statt Vektor: `std::unordered_map<uint32_t,uint64_t> m_netByHandle; std::unordered_map<uint64_t,uint32_t> m_handleByNet;`, Einfügen mit `emplace` (erster Eintrag gewinnt, wie bisher die lineare Suche). (2) `seedNetIds` reserviert (`registry.size()`) und trägt direkt ein, ohne vorherige Suche. (3) Die UUID-zu-NetId-Ableitung (`lo & ~(1<<63)`, Zeilen 2735-2736 und 2755-2756 sind dupliziert) in eine freie Funktion `netIdOf(const EntityIdComponent&)` ziehen. (4) Fehltreffer: statt Registry-Scan pro Aufruf einmalig pro Frame/Registry-Änderung eine komplette `handleByNet`-Aktualisierung (z. B. über `registry.on_construct<EntityIdComponent>()`-Observer, der neue Entities einträgt) oder zumindest ein kurzlebiges Negativ-Cache (Net-ID, Frame-Nummer). (5) `forgetNetId` über beide Maps.
- **Erwarteter Effekt**: Schätzung: Seeding von O(N²) auf O(N) (Sekunden → Millisekunden bei 50k+ Entities); pro-Frame-Lookups von O(N) auf O(1). Bei Szenen unter ca. 2k Entities nicht spürbar.
- **Risiko / Verifikation**: Heutiges Verhalten bei doppelten Einträgen (gleiche Net-ID, anderes Handle nach Handle-Recycling) muss bewusst festgelegt werden: `forgetNetId` entfernt aktuell alle Einträge mit dieser Net-ID. Tests: `tests/test_collab_controller.cpp` (seedNetIds/entityForNetId bereits genutzt) um Fälle "Entity gelöscht und Handle recycelt", "unbekannte Net-ID, die später entsteht" (Negativ-Cache darf nicht veralten) und einen Seed-Zeitbudget-Test mit 20k Entities ergänzen.

---

### EDT-203 – CollabDocSync::diffInto: doppelte JSON-Kopien, O(n²)-Suche, tote Bedingung, Vollserialisierung pro Remote-Batch
- **ID**: EDT-203
- **Kategorie**: A / C / D
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Editor/CollabDocSync.cpp:127-200`, `221-283`
- **Problem**: (1) `const auto current = capture(...)` ist const, daher kopiert `for (...) known.emplace(id, json)` (Zeile 197) jede JSON-Zeichenkette ein zweites Mal und baut die Map jedes Mal komplett neu (`known.clear()`), auch wenn sich nur ein Item geändert hat. (2) Die Entfernungserkennung `std::find(ids.begin(), ids.end(), id)` (Zeile 161) und das `remove_if` mit `find` (Zeilen 186-188) sind O(n²); bei UI-Widgets mit mehreren tausend Elementen sind das Millionen Vergleiche pro Diff. (3) Tote Bedingung: innerhalb von `if (ids != mirror.order[k])` ist `ids == mirror.order[k]` immer falsch, `!(sameSet && ids == mirror.order[k])` also immer wahr; `sameSet` ist zudem nur ein Größenvergleich und irreführend benannt. Außerdem wird `Delta d` samt `joinIds(ids)` (String-Bau) vor der Entscheidung gebaut und bei `onlyAppended` verworfen. (4) `applyDeltas` ruft nach jedem erfolgreichen Batch `capture()` für den ganzen Dokumentbestand (Zeilen 269-281), d. h. ein einzelner Remote-Knoten-Drag (bis 30 Hz) kostet O(Dokument) JSON-Serialisierung.
- **Code-Auszug**:
```cpp
// :161
if (std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
// :192
if (!(sameSet && ids == mirror.order[k]) && !onlyAppended)
    out.push_back(std::move(d));
// :196-198
known.clear();
for (const auto& [id, json] : current) known.emplace(id, json);
```
- **Lösung**: `current` nicht-const lassen; nach der Schleife `known` per `std::move(json)` befüllen (`known.reserve(current.size())`). Für die Entfernungsprüfung die bereits vorhandene Map nutzen: zuerst alle `current`-IDs in ein `std::unordered_set` (oder markiere in `known` mit einem Generationszähler), dann `mirror.order[k]` gegen das Set prüfen. `onlyAppended` berechnen, bevor `Delta d`/`joinIds` gebaut wird; die tote Teilbedingung und `sameSet` entfernen. Für `applyDeltas`: Mirror nur für die tatsächlich angewendeten Items nachziehen (JSON neu aus dem Dokument holen, weil `upsert` normalisieren kann) statt das gesamte Dokument.
- **Erwarteter Effekt**: Schätzung: halbiert Allokationen/Kopien pro Diff, macht Entfernungsprüfung linear; pro Remote-Batch O(geänderte Items) statt O(Dokument). Wirkung bei Graphen ab einigen hundert Items messbar.
- **Risiko / Verifikation**: Die Serialisierung nach `upsert` kann vom empfangenen JSON abweichen; deshalb Mirror aus dem Dokument neu lesen und nicht aus `d.json` übernehmen (sonst "Ping-Pong"-Diffs). `tests/test_collab_docsync.cpp` deckt Diff/Apply/Reorder ab; ergänzen: reine Reorder-Fälle, "nur angehängt", "Item entfernt und neu eingefügt", Echo-Freiheit nach Apply.

---

### EDT-204 – publishAsset liest die ganze Datei, bevor das Typ-Gate entscheidet; `istreambuf_iterator`, Kopie statt Move; dreifach dupliziert
- **ID**: EDT-204
- **Kategorie**: C / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/CollabController.cpp:3136-3181` (publishAsset), `4417-4421` (publishAssetCreate), `4587-4604` (catchUpJoiner)
- **Problem**: (1) Der Hook feuert laut Kommentar (3143-3147) bei jedem `ContentManager::saveAsset`, auch bei Importen von Meshes/Texturen. `publishAsset` liest die komplette Datei in einen Vektor (3138-3141) und prüft erst danach `assetTypeTravels(assetTypeOf(fullPath))` (3149-3150). Bei einer Session ohne Large-Asset-Sync (Normalfall) wird jede große `.hasset`-Datei also auf dem UI-Thread komplett gelesen und anschließend verworfen. `publishAssetCreate` macht es richtig (Gate vor dem Lesen, Zeilen 4414-4415). (2) `std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {})` liest Byte für Byte ohne Vorab-Reserve (typisch deutlich langsamer als ein einziger `read`) und reallokiert exponentiell. (3) `up.bytes = bytes;` (Zeile 3178) kopiert die Daten, `bytes` wird danach nicht mehr gebraucht; der Header (CollabController.h:196-199) rechnet ohnehin mit dreifacher Residenz einer Datei. (4) `catchUpJoiner` liest für jeden Joiner alle angefassten Dateien synchron im Frame (Callback `onParticipantJoined`, aus `update()`).
- **Code-Auszug**:
```cpp
std::ifstream f(fullPath, std::ios::binary);
if (!f) return;
std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
...
up.bytes    = bytes;
```
- **Lösung**: Hilfsfunktion `static bool readFileBytes(const std::string& path, std::vector<uint8_t>& out)` (seekg/tellg + `out.resize` + ein `read`) einführen und an den drei Stellen verwenden. In `publishAsset` den Typ-Gate vor das Lesen ziehen (`isSyncableAsset` → Typ → dann lesen). `up.bytes = std::move(bytes)`; `size` vorher merken (steht bereits danach, deshalb `const size_t size = bytes.size()` vor dem Move).
- **Erwarteter Effekt**: Schätzung: bei einem 100-MB-Import in einer Session ohne Large-Asset-Sync entfällt ein Lesevorgang von einigen hundert ms bis Sekunden auf dem UI-Thread; Einlesen mit einem `read` ist typischerweise um ein Mehrfaches schneller als der Iterator-Weg. Messen mit einem großen Mesh.
- **Risiko / Verifikation**: Reihenfolge `!f`/`bytes.empty()` vs. Gate ändert nur den Zeitpunkt stiller `return`s, kein beobachtbares Verhalten. `tests/test_collab_controller.cpp` (Asset-Publish-Fälle) laufen lassen; neuen Test ergänzen: "nicht reisender Typ → Datei wird nicht geöffnet" (z. B. über einen nicht lesbaren Pfad, der trotzdem lautlos zurückkehrt).

---

### EDT-205 – Collaboration-Fenster: Interface-Enumeration und HMAC bei jedem Frame
- **ID**: EDT-205
- **Kategorie**: D / E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/CollabPanel.cpp:917-931` und `967-972`; `CollabController.cpp:2557-2568`; `src/HE_Net/src/Socket.cpp:1577-1609`, `1521-1575`; `src/HE_Net/src/SecureTransport.cpp:654-677`
- **Problem**: Solange das Fenster als Host offen ist, ruft `DrawCollabWindow` jeden Frame `HE::Net::socketOwnsAddress(seen)` auf. Das ruft `socketLocalAddresses()`, das `getifaddrs` (Linux/macOS) bzw. `GetAdaptersAddresses` (Windows, mit 16-KB-Puffer; auf Windows im Bereich mehrerer Millisekunden üblich, unsicher/messen) ausführt und für jede Adresse `inet_pton` plus String-Bau macht. Zusätzlich `sessionFingerprint()` pro Frame: bis 16 `m_peers.find` plus HMAC-Berechnung plus String-Bau. Beide Ergebnisse ändern sich während der Session praktisch nicht.
- **Code-Auszug**:
```cpp
const bool ours   = HE::Net::socketOwnsAddress(seen);
...
if (const std::string fp = collab->sessionFingerprint(); !fp.empty())
```
- **Lösung**: Panel-lokal cachen: `static std::string s_cachedSeen; static bool s_cachedOurs;` und neu berechnen, wenn `seen != s_cachedSeen` oder alle ~5 s. Fingerprint im Controller beim Wechsel auf `Status::Joined/Hosting`/Peer-Established einmal berechnen und als `std::string m_fingerprint` ablegen (leer bis Handshake fertig; deshalb bei leerem Cache jeden Frame erneut versuchen, danach nicht mehr).
- **Erwarteter Effekt**: Schätzung: entfernt einen Syscall (Linux/macOS einige µs bis zehn µs, Windows potenziell ms) und einige Allokationen pro Frame, nur bei offenem Fenster.
- **Risiko / Verifikation**: Änderung der lokalen Interfaces (VPN an/aus) wird erst mit dem Cache-Intervall sichtbar; rein kosmetisch. Kein Test nötig; per Profiler-Zähler (Aufrufe pro Sekunde von `socketLocalAddresses`) prüfen.

---

### EDT-206 – Request-Zeilen der Host-Queue bauen pro Frame Pfade, Strings und durchsuchen Teilnehmer/Entity-Teilbäume
- **ID**: EDT-206
- **Kategorie**: D / C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Editor/CollabPanel.cpp:1040-1126`, `1128-1235`, `1253-1296`; `CollabController.cpp:3940-3985` (opDisplayName, opHeldBy)
- **Problem**: Bis zu 128 offene Anfragen (`kMaxPendingOps`, Zeile 3875) werden jeden Frame ungeclippt gezeichnet. Pro Zeile: `opDisplayName` (bei Entity: `entityForNetId` inkl. möglichem Registry-Scan, siehe EDT-202; bei Datei `std::filesystem::path(...).filename().string()` mit Allokationen), `opHeldBy` (für Entities ein Stack-DFS über den ganzen Teilbaum mit `netIdFor` + `lockFor` je Knoten; für Dateien `assetLockInfo` mit FNV über den Pfad), `std::filesystem::path(op.newPath).filename().string()` (Zeile 1074), ein Teilnehmer-Linearscan je Requester (1104-1115) und je Gruppe (1184). Zusätzlich pro Frame `std::vector<char> drawn`, `std::vector<size_t> group` je Zeile, `std::string headline`/`title`. Die Ergebnisse (Anzeigenamen, Halter) ändern sich nur bei Lock-/Welt-Änderungen. Das widerspricht der Konvention "Row-heavy panels draw only visible rows" (CLAUDE.md Abschnitt 4).
- **Code-Auszug**:
```cpp
const std::string name = collab->opDisplayName(op);
...
const std::string heldBy = collab->opHeldBy(op);
...
std::filesystem::path(op.newPath).filename().string().c_str()
```
- **Lösung**: (1) `PendingAssetOp` um gecachte Felder `displayName`/`newDisplay` erweitern, beim Einreihen (`queueAssetOp`) berechnen. (2) `opHeldBy` nur für sichtbare Zeilen (`ImGui::IsRectVisible`/`ImGuiListClipper`, Zeilenhöhe konstant machen) auswerten und höchstens alle ~250 ms pro Zeile auffrischen. (3) Bündel-Gruppierung einmal beim Ändern von `m_pendingOps` berechnen statt jeden Frame. (4) Teilnehmer-Lookup über Hilfsfunktion `participantById` (siehe EDT-211).
- **Erwarteter Effekt**: Schätzung: bei wenigen Anfragen kaum messbar; bei einer Bündel-Anfrage mit 100 Dateien (typischer Multi-Delete) entfallen pro Frame ca. 100× Pfad-/Lock-Lookups und viele kleine Allokationen. Nur Host mit offenem Fenster.
- **Risiko / Verifikation**: Gecachter Name einer Entity kann veralten (Umbenennung); beim Rendern nur neu auflösen, wenn `entityForNetId` greift. UI-Test nach Muster `tests/test_*_ui.cpp` mit `HE_UI_DUMP_DIR` für Bündel-Zeilen ergänzen.

---

### EDT-207 – CollabController ist ein Gott-Objekt mit 280- und 610-Zeilen-Funktionen
- **ID**: EDT-207
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/CollabController.cpp:196-477` (startHosting mit ca. 150-Zeilen-Lambda, 327-474), `1053-1661` (wireCallbacks), gesamte Datei 4667 Zeilen; `CollabController.h:46-1682`
- **Problem**: Eine Klasse vereint: Netzwerk-Stack-Aufbau, Port-Mapping/Directory-Worker, LAN-Discovery, lokale Identität/Avatar (Config + Datei), Asset-Locking, Asset-Operationen mit Host-Inbox, Revisionsordnung, Netz-ID-Tabelle, Transform-/Komponenten-Throttling, Settle/Catch-up und einen Per-Peer-Rate-Limiter (~100 Zeilen plus 40 Zeilen Doku). `startHosting` enthält den kompletten Router-/Directory-Registrierungsablauf als Inline-Lambda (Mapping-Ergebnisse → Texte → Pinhole → HTTPS-Registrierung); `wireCallbacks` ist eine einzige Funktion mit 20 Lambdas, in denen `onLockChanged`/`onLockDenied` jeweils über 100 Zeilen mit mehrfachen Early-Returns und verschachtelten Schleifen haben. Das erschwert Tests (der Limiter z. B. ist nur über den Controller prüfbar) und Lesbarkeit.
- **Code-Auszug**:
```cpp
m_registerFuture = std::async(std::launch::async, [endpoint, sid, port16, displayName] {
    RegisterResult r;
    ...  // ca. 150 Zeilen bis Zeile 474
```
- **Lösung**: Schrittweise, ohne Verhaltensänderung: (1) Das Lambda als freie Funktion `RegisterResult registerWithDirectory(endpoint, sid, port, displayName)` in `CollabDirectory.cpp` (zusammen mit `remedyFor`, `releaseNetworkResources`, `pumpDirectory` und den Konstanten). (2) `RemoteNoteLimiter` (budget/summary/mutex, 5 Funktionen) als eigene kleine Klasse mit Unit-Test. (3) Identity (`identityStorage`, `setLocal*`, `resampleSquareRgba`) in `CollabIdentity.cpp`. (4) `wireCallbacks` pro Callback in benannte Member (`onLockChangedImpl`, `onLockDeniedImpl`, `onAssetUpdatedImpl`) zerlegen; das gemeinsame Muster "Pfad in m_heldAssetLocks / m_pendingLockReleases suchen" in Helfer (`bool contains(vec, path)`, `bool eraseFirstMatching(...)`).
- **Erwarteter Effekt**: Rein strukturell: bessere Lesbarkeit/Testbarkeit, kleinere TUs, schnellere inkrementelle Builds. Keine Laufzeitänderung.
- **Risiko / Verifikation**: Mechanisches Verschieben; `tests/test_collab_controller.cpp`, `test_net_collab.cpp`, `test_mcp_tools_collab.cpp` müssen unverändert grün bleiben. Public-API der Klasse bleibt (Header-Kompatibilität).

---

### EDT-208 – CollabController.h wird über EditorApplication.h in ca. 100 Übersetzungseinheiten gezogen
- **ID**: EDT-208
- **Kategorie**: F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/CollabController.h:17-36` (Includes), `src/HE_Editor/EditorApplication.h:10`
- **Problem**: `CollabController.h` inkludiert `Net/CollabSession.h` (1211 Zeilen), `Net/LanBeacon.h`, `Net/PortMapper.h`, `Net/RelayFanout.h`, `Net/RelayTransport.h`, `<future>`, `<mutex>`, `<unordered_map>`. `EditorApplication.h` inkludiert den Controller direkt; `EditorApplication.h` selbst wird von rund 100 Dateien in `src/` und `tests/` eingebunden (grep). Jede dieser TUs parst den Netzwerk-Stack mit, obwohl die meisten nur `ctx.collab->inSession()` brauchen. Der Header hat selbst Pimpl-Ansätze (`class SceneStateProvider`, `unique_ptr<NetSession>`), hält aber `LanBeacon::Announcer/Browser`, `PortMapper::MappingHandle`, `std::future<RegisterResult>` und `RemoteNoteBudget` per Wert.
- **Code-Auszug**:
```cpp
#include <Net/CollabSession.h>
#include <Net/LanBeacon.h>
#include <Net/PortMapper.h>
#include <Net/RelayFanout.h>
#include <Net/RelayTransport.h>
```
- **Lösung**: In `EditorApplication.h` `class CollabController;` vorwärts deklarieren und `m_collab` als `std::unique_ptr<CollabController>` halten (oder die Member hinter ein Pimpl `struct Impl` legen: Directory-/LAN-/Limiter-Zustand). Die 18 Dateien, die `CollabController` wirklich benutzen, inkludieren den Header selbst (tun sie bereits per `#include "CollabController.h"`). Alternativ Teilung in `CollabController.h` (kleines Interface) und `CollabControllerImpl.h`.
- **Erwarteter Effekt**: Schätzung: spürbar kürzere Inkrementalbuilds des Editors, wenn `CollabSession.h`/Net-Header geändert werden (ca. 80 TUs weniger betroffen). Kein Laufzeiteffekt. Messen mit `-ftime-trace` bzw. `/d1reportTime`.
- **Risiko / Verifikation**: `m_collab` wird per Wert verwendet (`m_collab.update(...)` etc. in EditorApplication.cpp); Umstellung auf Zeiger ändert viele Aufrufstellen (mechanisch). `AppContext::collab` ist bereits ein Zeiger. Build aller Targets (Editor, `he_tests`) prüfen.

---

### EDT-209 – CollabDocSync-Adapter duplizieren Link-/Upsert-Logik; `applyOrder` ist O(n²); zwei handgeschriebene Zahlen-Parser
- **ID**: EDT-209
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/CollabDocSync.cpp:493-514`, `581-601`, `625-634`, `661-665`, `683-684`, `697-708`, `721-730` (Link-Code ×3), `305-326` (applyOrder), `41-60`, `526-550` (Parser)
- **Problem**: `HcAdapter`, `MatAdapter` und `ParticleAdapter` implementieren dasselbe Link-Handling dreimal: `linkJson` (String aus 4 Ints; in `ParticleAdapter::enumerate` inline), Upsert "parse vier Ints, `find_if` gleiche Endpunkte, sonst `push_back`", Remove "`find_if` über `linkKey`". `applyOrder` sucht für jede gewollte ID linear im Vektor (O(n²)); bei UI-Bäumen mit tausenden Elementen und jedem Reorder-Delta spürbar. `splitIds` und `parseFourInts` sind zwei Eigenbau-Parser für Ganzzahlen, die `std::from_chars` ersetzen könnte.
- **Code-Auszug**:
```cpp
for (std::size_t i = 0; i < items.size(); ++i)
{
    if (taken[i] || idOf(items[i]) != want) continue;
```
- **Lösung**: Template-Hilfsfunktionen `template<class L> std::string linkJson(const L&)`, `template<class V,class L> bool upsertLink(V& links, const L&)`, `template<class V> bool removeLinkByKey(V& links, int64 id)` (alle drei Link-Typen haben `srcNode/srcPin/dstNode/dstPin`). `applyOrder`: `unordered_map<int64,size_t>` einmal aufbauen. Parser per `std::from_chars` mit den bestehenden Obergrenzen.
- **Erwarteter Effekt**: ca. 60-80 Zeilen weniger; Reorder-Apply von O(n²) auf O(n). Gering.
- **Risiko / Verifikation**: Reihenfolge-Semantik von `applyOrder` (Unerwähnte ans Ende, relative Reihenfolge erhalten) muss identisch bleiben; Randfälle: doppelte IDs im Reorder-Delta, unbekannte IDs. Absichern mit den Reorder-Tests in `tests/test_collab_docsync.cpp`.

---

### EDT-210 – Cinematic-Streifen: kein Row-Clipping, pro Frame String-/Vektor-Allokationen, `draw()` mit 416 Zeilen
- **ID**: EDT-210
- **Kategorie**: D / A
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/CinematicTimeline.cpp:532-948` (draw), `354-391` (buildRows), `606-850`
- **Problem**: (1) Die Zeilenschleife übergibt jede Zeile an ImGui (`PushID`, `Selectable`, `BeginPopupContextItem`, Items), unabhängig davon, ob die Zeile im sichtbaren Bereich des Child-Fensters liegt; CLAUDE.md verlangt für Row-heavy-Panels Clipping. Bei vielen Aktoren × Tracks (hunderte Zeilen) wächst der Aufwand linear. (2) Pro Frame und Zeile: `std::vector<Row>` in `buildRows`, `std::string name` mit `+= "  (missing)"` (Zeile 627-629), `trackLabel(tr) + "##cin_track"` (Zeile 662), pro Section `assetLabel` → `std::function` → `std::string`-Kopie (Zeile 750). (3) `buildRows` ist O(B·T): für jede Binding `findBinding` (Zeile 364, quadratisch in B) und eine Schleife über alle Tracks, danach für jeden Track erneut `findBinding` (Zeile 382); `draw` ruft `findBinding` pro Gruppenzeile ein weiteres Mal (Zeile 616). (4) `draw()` ist eine 416-Zeilen-Funktion mit bis zu sechs Verschachtelungsebenen (Zeilen → Items → Section/Key/Flag → Hit-Behandlung) und gemischter Eingabe-, Zeichen- und Drag-Logik.
- **Code-Auszug**:
```cpp
for (int r = 0; r < static_cast<int>(rows.size()); ++r)
{
    const Row& row = rows[r];
    const float rowY = rowsTop + M.rowH * static_cast<float>(r);
    ...
    if (ImGui::Selectable((trackLabel(tr) + "##cin_track").c_str(), selected, 0, ...
```
- **Lösung**: Zeilenhöhe ist konstant (`M.rowH`): `firstRow/lastRow` aus `ImGui::GetScrollY()` und Child-Höhe berechnen und nur diese Zeilen zeichnen (Playhead/Lane-Hintergrund unverändert). `buildRows` mit einer einmal gebauten `slot → bindingIndex`-Map (kleiner `unordered_map` oder sortierter Vektor) auf O(B+T). Labels in einem kleinen Per-Frame-Puffer (`char[]` + `snprintf`) oder gecacht pro Track; `assetName` nur für sichtbare Sections. `draw()` in `drawGroupRow`, `drawTrackRow`, `drawItem` und `applyDrag` zerlegen.
- **Erwarteter Effekt**: Schätzung: bei üblichen Sequenzen (unter 30 Zeilen) nicht messbar; bei großen Cinematics weniger ImGui-Submits und Allokationen. Hauptnutzen ist Lesbarkeit.
- **Risiko / Verifikation**: Item-Selektion/Drag darf nicht von Clipping abhängen (der Drag-Block ab Zeile 855 arbeitet mit Indizes und bleibt unberührt). `tests/test_cinematic_timeline.cpp` (headless über `buildRows`/`draw`) und `tests/test_ui_shot.cpp` als Regression; bildliche Prüfung per `HE_UI_DUMP_DIR`.

---

### EDT-211 – Duplizierte Muster im Controller: Batch-Approve/Deny, Pending-Op-Ersetzen, Teilnehmer-Lookup, FNV-1a
- **ID**: EDT-211
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `CollabController.cpp:4176-4214` (approveAssetOpBatch/denyAssetOpBatch), `3921-3929` und `4028-4035` (OurOp ersetzen/anhängen), Teilnehmer-Lookups `481-488`, `1227-1232`, `3286-3287`, `3322-3327`, `3612-3617`, `3749-3750`, `3804-3809`, `4384-4385`, `CollabPanel.cpp:1106, 1184, 1259`; FNV-1a `134-136`, `2316-2318`, `2849-2855`, `CollabDocSync.cpp:63-68`
- **Problem**: `approveAssetOpBatch` und `denyAssetOpBatch` sind bis auf das letzte `approveOne`/`denyOne` identisch (Herausnehmen der Batch-Zeilen). Das Muster "gleiche (op,path) ersetzen requestId sonst anhängen" steht zweimal (`requestEntityOp`, `requestOrPerformAssetOp`). "Teilnehmer per ID suchen" steht ca. elfmal als Schleife mit leicht unterschiedlichem Fallback. FNV-1a ist viermal handgeschrieben.
- **Code-Auszug**:
```cpp
std::vector<PendingAssetOp> taken;
for (auto it = m_pendingOps.begin(); it != m_pendingOps.end();)
{
    if (it->batchId == batch && it->batchOwner == owner) { taken.push_back(*it); it = m_pendingOps.erase(it); }
    else ++it;
}
```
- **Lösung**: `std::vector<PendingAssetOp> takeBatch(owner, batch)` privat (`stable_partition` + move) und die zwei Methoden darauf; `void trackOurOp(op, path, id, display)` für das Ersetzen; `const Participant* participantById(ParticipantId) const` im Controller (und von den Panels nutzbar); `constexpr uint64_t fnv1a(string_view)` in einem kleinen Header (Net- oder Editor-Util).
- **Erwarteter Effekt**: ca. 70 Zeilen weniger, ein Ort für Teilnehmer-Fallbacks. Keine Laufzeitänderung.
- **Risiko / Verifikation**: `assetSubject` normalisiert `\\` zu `/` innerhalb der Schleife (andere FNV-Varianten nicht), also die Hash-Funktion generisch und die Normalisierung separat halten; Hash-Werte sind Teil des Wire-Formats und dürfen sich nicht ändern. Tests: `test_collab_controller.cpp` (Batch-Fälle), `test_net_collab.cpp`.

---

### EDT-212 – CollabActivityBar: redundanter Zweig, doppelte String-Bildung pro Frame, implizite Aufrufreihenfolge, UTF-8-Schnitt
- **ID**: EDT-212
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Editor/CollabActivityBar.cpp:41-46`, `64-100`, `105-127`, `141-166`, `168-220`
- **Problem**: (1) `if (s_batch.count == 0) { s_batch = {}; s_batch.since = now; } else s_batch.since = now;` setzt in beiden Zweigen `since = now`; `s_batch = {}` ist redundant (der Zustand ist bei `count == 0` bereits Standard). (2) `FooterWidth` und `DrawFooter` rufen jeweils `queueText` (baut `std::to_string`-Text, zwei Vektor-`size()`-Aufrufe) und `batchText`/`elide` (Teilstring-Kopien) pro Frame, also doppelt; im Gegensatz dazu hält `BuildProgressDialog.cpp:546` ein gemeinsames Modell pro Frame. (3) `collect()` (Ableeren der Controller-Puffer) läuft nur innerhalb von `FooterWidth`; wer nur `DrawFooter` aufruft, sammelt nie. Die Abhängigkeit steht nirgends dokumentiert. (4) `elide()` schneidet nach Bytes (`substr(0, 33)`) und kann ein UTF-8-Mehrbytezeichen (Umlaute in Datei-/Personennamen) in der Mitte teilen, was ungültiges UTF-8 an ImGui gibt (Darstellung als "?").
- **Code-Auszug**:
```cpp
if (s_batch.count == 0) { s_batch = {}; s_batch.since = now; }
else                     s_batch.since = now;
...
s = s.substr(0, kMaxLabelChars - 1) + "\xE2\x80\xA6";  // U+2026
```
- **Lösung**: Zweig zu `s_batch.since = now;` vereinfachen. Einen `struct Line { std::string text; bool personal; float alpha; }` pro Frame einmal in `FooterWidth` bauen und in `DrawFooter` wiederverwenden (oder `collect()` + Text-Bau in eine `prepare(ctx)`-Funktion mit Frame-Zähler). `elide` am letzten UTF-8-Lead-Byte (`(c & 0xC0) == 0x80` rückwärts überspringen) kürzen.
- **Erwarteter Effekt**: vernachlässigbar bei Laufzeit (ein paar kleine Allokationen pro Frame, nur in Session); Korrektheit bei Nicht-ASCII-Namen.
- **Risiko / Verifikation**: `elide` ändert sichtbaren Text nur bei Mehrbyte-Zeichen; kleiner Unit-Test mit "Jörg"-Länge am Limit.

---

### EDT-213 – Lineare Vektoren und String-Schlüssel auf Nachrichtenpfaden
- **ID**: EDT-213
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `CollabController.cpp:4477-4479` (noteSessionTouched), `2883-2887`, `1496-1501`, `1533`, `1546-1548` (m_heldAssetLocks/m_pendingLockReleases `std::find`), `2992-2996`, `3020` (revisionKey), `4272-4281` (acceptRemoteCreate); `CollabController.h:1527`, `1591`, `1604`
- **Problem**: `noteSessionTouched` (pro Doc-Delta-Publish und -Empfang, bis ca. 30 Hz) sucht linear in `m_sessionTouched` (wächst mit jeder angefassten Datei der Session). `m_heldAssetLocks`/`m_pendingLockReleases` sind kleine Vektoren; unkritisch, aber mehrfach linear durchsucht. `acceptRevision` baut pro empfangenem Frame `to_string(from) + '\n' + path` als Schlüssel (eine Allokation je Delta). `acceptRemoteCreate` testet bei vergebenem Namen bis zu 998 Kandidaten per `filesystem::exists` auf dem Host-UI-Thread; ein Peer, der in einer Schleife denselben Namen anlegt, löst das wiederholt aus (es existiert ein serverseitiger Rate-Limit-Grund `RateLimited`; Wirkung nicht geprüft).
- **Code-Auszug**:
```cpp
if (std::find(m_sessionTouched.begin(), m_sessionTouched.end(), relativePath) == m_sessionTouched.end())
    m_sessionTouched.push_back(relativePath);
```
- **Lösung**: `m_sessionTouched` als Vektor behalten (Reihenfolge wird beim Catch-up gebraucht) plus `std::unordered_set<std::string> m_sessionTouchedSet` für den Existenztest. `m_seenRevision` mit Key `std::pair<ParticipantId,std::string>` (eigener Hash) oder `unordered_map<ParticipantId, unordered_map<string,uint32_t>>` (löscht auch `forgetRevisionsFrom` ohne Präfixscan). `acceptRemoteCreate`: Kandidatenzahl auf ca. 50 begrenzen oder einmal das Verzeichnis listen.
- **Erwarteter Effekt**: gering (Zeichenketten-Vergleiche bei hunderten Einträgen, eine Allokation je Delta). Nur Sauberkeit/Skalierung.
- **Risiko / Verifikation**: `acceptRemoteCreate`-Begrenzung ändert das Verhalten bei vielen vergebenen Namen (Vorschlag fehlt dann) und ist daher optional. Tests `test_collab_controller.cpp` (Revisionsordnung, Catch-up).

---

### EDT-214 – Kleinigkeiten im Cinematic-Teil: Magic-Character-Vergleich, doppelte Dauer-Initialisierung
- **ID**: EDT-214
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Editor/CinematicTimeline.cpp:447-453`, `108-121`
- **Problem**: `kindColour` entscheidet über den ersten Buchstaben des Gruppennamens (`g[0] == 'T'` / `'M'`), eine unsichtbare Kopplung an die Benennung in `Sequencer::targetGroup` (Umbenennung "Transform" → "Position" bricht die Farbe still). In `addTrack` steht `if (seq.duration <= 0.0f) seq.duration = kDefaultDuration;` in beiden Zweigen (Zeilen 116 und 120).
- **Code-Auszug**:
```cpp
const char* g = Sequencer::targetGroup(tr.channel.target);
if (g[0] == 'T') return IM_COL32(120, 180, 255, 255);
if (g[0] == 'M') return IM_COL32(190, 130, 255, 255);
```
- **Lösung**: Farbe direkt am `PropTarget` bestimmen (kleine `switch`-Funktion oder `Sequencer::targetGroupId(target)` als Enum) bzw. `targetGroup` in einen Enum-Rückgabewert umstellen. Den Dauer-Fallback einmal vor dem `if (kind == CameraCut)`-Zweig setzen.
- **Erwarteter Effekt**: Robustheit, 2 Zeilen weniger.
- **Risiko / Verifikation**: `Sequencer::targetGroup` wird im SequencerTimeline-Teil weiterverwendet (anderer Slice); dort nur ergänzen, nicht ändern. `tests/test_cinematic_timeline.cpp`.

---

### EDT-215 – Kommentare erzählen Fehlerhistorie statt Absicht
- **ID**: EDT-215
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `CollabController.cpp` (1215 von 4667 Zeilen sind Kommentare, neunmal "used to"), z. B. `3085-3096`, `2172-2175`, `1663-1670`; `CollabController.h` (908 von 1682 Zeilen), z. B. `79-96`, `182-199`, `1153-1165`; `CollabPanel.cpp:371-392`
- **Problem**: Viele Kommentare dokumentieren, was früher falsch war ("This list used to spell out `.hmat`, ...", "used to be one line of small red text", Anekdoten wie "418 stale forwards"). Das ist wertvoll als Commit-/Doc-Information, verdeckt aber im Code den eigentlichen Vertrag und macht die Datei schwer überfliegbar; die Funktionen selbst sind dadurch teils nur noch mit Scrollen verständlich. CLAUDE.md fordert "explains the why", nicht die Chronik.
- **Code-Auszug**:
```cpp
// This list used to spell out `.hmat`, `.huiw`, `.hcode`, `.hpart`, `.hasm`,
// `.hinput`; the engine writes none of those, so it matched NOTHING in a real
// project ...
```
- **Lösung**: Pro Block den heute gültigen Vertrag in 2-4 Zeilen belassen und die Fehlergeschichte in `docs/` (z. B. `docs/collab-sync-audit-*.md`, existiert laut EditorApplication.cpp:10237) bzw. Commit-Nachricht verweisen. Nur dort kürzen, wo ohnehin angefasst wird (keine Massenänderung).
- **Erwarteter Effekt**: Lesbarkeit; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Keines für Verhalten; keine Information löschen, die nirgends anders steht.

---

## Cross-Modul-Hinweise

- `src/HE_Editor/EditorApplication.cpp:5061-5070`: Pro Frame mit selektierter Entity in einer Session wird `TerrainSync::serializeWithoutGrids(...)` ausgeführt (Heap-Vektor aller Komponenten), dann in `publishComponents` FNV-gehasht (CollabController.cpp:2665) und `netIdFor` (EDT-202) aufgerufen. Besser: nur bei geänderter Entity (Dirty-Flag/Revisionszähler der Komponenten oder `EditorCommands`-Hook) serialisieren. Unsicher/messen: Kosten hängen von Komponentenzahl ab.
- `src/HE_Editor/EditorApplication.cpp:4663-4667`: Presence-Selektion fremder Peers ruft pro Frame `entityForNetId` je selektierter Entity (siehe EDT-202, Negativ-Treffer scannen die Registry).
- `src/HE_Editor/EditorApplication.cpp:10241-10248, 10339-10343`: Belegstellen für EDT-201; dort ist auch die richtige Stelle für einen Revisionszähler/ein Throttling.
- `src/HE_Net/src/Socket.cpp:1577-1609`: `socketOwnsAddress` enumeriert bei jedem Aufruf alle Interfaces; für Aufrufer mit Frame-Frequenz fehlt ein Cache (siehe EDT-205). `SecureTransport::sessionFingerprint` (SecureTransport.cpp:654) rechnet bei jedem Aufruf einen HMAC.
- `src/HE_Editor/EditorApplication.h:10`: bindet `CollabController.h` ein (siehe EDT-208).
- `src/HE_Editor/BuildProgressDialog.cpp:546-615`: nutzt das Muster "ein Modell pro Frame, von `FooterWidth` und `DrawFooter` geteilt", das `CollabActivityBar` und vermutlich `CollabPresenceBar` (anderer Slice, nicht gelesen) nicht nutzen.
- `src/HE_Net/include/Net/CollabSession.h`: `sendAsset(const AssetUpdate&)` nimmt die Bytes per const-Referenz; ob intern nochmals kopiert wird (Frame-Aufbau/Chunking), wurde nicht geprüft.

## Unsicher / Messung nötig

- Tatsächliche Kosten von `diffInto`/`hasLocalChanges` pro Frame für Level Script/GameInstance (EDT-201): Scoped-Timer in `publishDocDeltas`, Graphen mit 50/500/2000 Nodes.
- Ab welcher Entity-Zahl `seedNetIds` (EDT-202) spürbar wird: Test mit 10k/50k/100k Entities; Schätzung oben ist reine Rechnung (N²/2 Paarvergleiche).
- `GetAdaptersAddresses`-Dauer auf Windows (EDT-205); Linux/macOS vermutlich im µs-Bereich.
- Wie oft `m_pendingOps` im Alltag groß wird (EDT-206); die 128er-Grenze ist nur eine Obergrenze.
- Zeitgewinn durch Datei-Lesen mit einem `read` statt `istreambuf_iterator` (EDT-204): Implementierungsabhängig (libstdc++/libc++/MSVC), mit großem Mesh messen.
- Ob `Sequencer::targetName` (SequencerTimeline.h, anderer Slice) pro Aufruf einen `std::string` baut; falls ja, ist der Label-Anteil in EDT-210 größer.
- Wirkung der Row-Clipping-Änderung im Cinematic-Streifen (EDT-210) bei realen Sequenzen; wahrscheinlich nur bei Extremfällen relevant.

## Gelesen

Ja, alle 11 Dateien des Slice vollständig gelesen: `CinematicTimeline.cpp/.h`, `CollabActivityBar.cpp/.h`, `CollabController.cpp` (Zeilen 1-4667) und `.h` (1-1682), `CollabDocSync.cpp/.h`, `CollabEntityOps.h`, `CollabPanel.cpp` (1-1334) und `.h`; zur Belegung zusätzlich gezielt `EditorApplication.cpp` (Zeilen 1040-1080, 4640-4700, 4995-5085, 10195-10400), `Socket.cpp` (1500-1610), `SecureTransport.cpp` (640-680), `EditorRowWidgets.cpp` (490-540) und `CinematicPanel.cpp` (880-938).
