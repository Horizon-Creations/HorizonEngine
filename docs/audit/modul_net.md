# Modulbericht Net – HorizonNet

Pfad: `src/HE_Net` · Teilberichte: 2 (`_parts/Net_*.md`)

## Modulzusammenfassung

**41 Funde**: Hoch 5, Mittel 13, Niedrig 23 · Typ: Vereinfachung 20, Performance 19, Beides 2 · Kategorien: A 9, B 1, C 12, D 5, E 6, F 8

| Teilbericht | Hoch | Mittel | Niedrig |
|---|---|---|---|
| Net_1.md | 3 | 6 | 14 |
| Net_2.md | 2 | 7 | 9 |

### Hoch-Funde (Index)

- **NET-101** – BitWriter/BitReader verarbeiten Bulk-Bytes Bit für Bit
- **NET-102** – NetSession::send formt jede Nachricht dreifach um; broadcast pro Peer neu
- **NET-103** – Host serialisiert jedes Asset für jeden Peer erneut (und umgeht damit RelayFanout)
- **NET-201** – UDP: Adress-Strings, `getaddrinfo` und Key-Allokationen pro Datagramm
- **NET-202** – TcpTransport: Ausgangspuffer wächst per exaktem `reserve` und wird per `erase(begin)` verkleinert

## Zusammenfassungen der Teilberichte

**Net_1.md**: Der Code ist durchgehend sorgfältig kommentiert und defensiv (Größenlimits vor Allokation, Identität aus der Verbindung statt aus der Nutzlast, Logging über `HE_LOG_*` mit lazy Argumenten). Die größten Hebel liegen nicht in der Kollaborationslogik, sondern in der untersten Schicht: `BitWriter`/`BitReader` arbeiten Bit für Bit, auch für Bulk-Bytes (Snapshots, Assets), und `NetSession` formt jede Nachricht mehrfach um und pro Empfänger neu. Darauf bauen die Asset-/Snapshot-Pfade auf, die alles auf dem Hauptthread und pro Peer erneut serialisieren. Daneben gibt es einen echten Überlauf-Bug im Snapshot-Chunk-Handler, mehrere „erst `resize`, dann prüfen"-Stellen, nicht aufgeräumte Transfer-Zustände nach Disconnect und einen Log-Fehler in `LanBeacon::Browser::update`. Der Rest sind Vereinfachungen (Handler-Boilerplate, tote Parameter, doppelte Helfer, schwere Header).

Funde: 3 Hoch, 6 Mittel, 14 Niedrig. Alle Zeilenangaben gegen den aktuellen Stand geprüft. Performance-Zahlen sind Schätzungen, keine Messungen.

---

**Net_2.md**: Der Code ist insgesamt sauber, gut kommentiert und fehlerrobust. Die Probleme liegen in der Datenpfad-Mechanik und in Duplikation, nicht in der Protokolllogik. Die gravierendsten Funde betreffen den Transport-Hot-Path. UDP löst pro Datagramm Adressen über Strings und `getaddrinfo` auf. TCP verwaltet den Ausgangspuffer quadratisch (exaktes `reserve` plus `erase` am Vektoranfang). Außerdem gibt es mehrere vermeidbare Kopien/Allokationen pro Frame und ein O(n²)-`poll()` in `SecureTransport`. Auf der Vereinfachungsseite sind Wire-Helfer, der Request/Reply-Client-Zustandsautomat (ScClient/TeamClient) und die Plattform-`#ifdef`-Blöcke in `Socket.cpp` mehrfach kopiert. Funde: 2 Hoch, 7 Mittel, 9 Niedrig. Alle Performance-Aussagen sind Schätzungen ohne Messdaten (siehe "Unsicher / Messung nötig").

---

## Cross-Modul-Hinweise

**Net_1.md**

- `src/HE_Net/src/SecureTransport.cpp:632`: `m_events` ist ein `std::vector<NetEvent>`, `poll()` entfernt mit `m_events.erase(m_events.begin())`, d. h. O(n) pro Event und O(n²) pro Drain, wenn viele (Snapshot-)Chunks gleichzeitig anstehen. Als `std::deque` (wie in RelayFanout/RelayTransport/UdpTransport) wäre es O(1). Deklaration: `include/Net/SecureTransport.h:196`. (Slice 2; hier nur verifiziert, nicht bewertet.)
- `src/HE_Net/src/TcpTransport.cpp:277`, `:307`: `inBuf.erase(begin, …)` und `outBuf.erase(begin, …)` je einmal pro `update()`; bei einem 64-MiB-Snapshot in `outBuf` (siehe NET-104) wird pro Tick der gesamte Rest verschoben. Ein Lese-/Schreib-Offset statt Erase wäre O(1). Messen, bevor man es ändert.
- `src/HE_Net/src/UdpTransport.cpp:72`: Peer-Schlüssel `host + "|" + std::to_string(port)` als `std::string`-Key für jeden eingehenden Datagram-Lookup (`m_byAddress`, `findPeer`) → Heap-Allokation pro Paket. (Slice 2; nicht weiter geprüft.)
- `src/HE_Editor/CollabController.cpp:3182`, `:4443`: Aufrufer von `sendAsset`; relevant für NET-103/108 (Kopien und N-fach-Serialisierung bei jedem Speichern eines Assets).
- `TeamClient.h:84-87` (`TeamMember::avatarRgba`) und `:329-336` (`Profile::avatarRgba`) halten dieselben Pixel doppelt; `mergeProfiles()` kopiert sie laut Kommentar bei jeder Rosteränderung in `m_members` (`TeamClient.cpp`, nicht in diesem Slice geprüft).

**Net_2.md**

- `src/HE_Core/include/Hpak/Aes256Gcm.h:34-45`: `aesGcmEncrypt/Decrypt` liefern immer in einen frisch geleerten Ausgabevektor. Für NET-204 wäre eine "append"-Variante (oder Ausgabe in vorbelegten Puffer) im Core nötig.
- `src/HE_Editor/CollabController.cpp:211, 943, 950`: `RelayTransport::host/connect` und `TcpTransport::connect` laufen synchron (Thread nicht verifiziert), relevant für NET-206. Die Directory-Aufrufe (`:327, :830, :2450`) laufen korrekt off-thread.
- `src/HE_Net/src/LanBeacon.cpp:175-177, 322` und `src/HE_Net/src/PortMapper.cpp:264, 313, 585, 605` nutzen `socketSendTo`/`socketRecvFrom` mit Strings. Sie sind nicht heiß, müssen aber bei NET-201 mit der alten String-API kompatibel bleiben (nicht entfernen).
- `src/HE_ContentSync/src/SftpClient.cpp:64` ist der einzige Aufrufer von `skipCandidates`/`outCandidateCount`; deshalb bleiben diese Parameter von `socketCreateTcpConnecting` erhalten.
- `src/HE_Net/src/HttpClient.cpp:157` ruft ebenfalls `socketCreateTcpConnecting` (blocking DNS), betrifft NET-206 analog.

## Unsicher / Messung nötig

**Net_1.md**

- Reale Kosten von NET-101/102 (Join mit 5/20/60 MiB-Szene, Asset-Save mit 8 Peers): mit `HE_LOG=Net=Trace` und einem Stopwatch um `handleJoinRequest` bzw. `sendAssetChunks` messen. Alle Zahlen oben sind Schätzungen.
- Ob `setLocalPresence` tatsächlich jeden Frame aufgerufen wird und wie groß typische Selektionen sind (NET-109): Aufrufer in `HE_Editor/CollabController.cpp` prüfen.
- Ob `LoopbackTransport` wirklich Multi-Thread-Zugriff sieht (NET-123).
- Heartbeat-Frequenz von `SessionDirectory` und damit der Nutzen von Handle-Wiederverwendung (NET-112, `SessionDirectory.cpp` außerhalb des Slice).
- Wirkung der Redirect-Beschränkung im curl-Backend gegen den echten Verzeichnis-Webserver (NET-112) und der Host-Prüfung in `discover` gegen reale Router (NET-110): nur mit Hardware.
- Windows- und macOS-Backends (`HttpsClient_Win.cpp`, `HttpsClient_Apple.mm`) im Linux-Container nicht übersetzt; Aussagen aus Code-Lesen.
- Ob zusätzlich eine Zeit-/Mengenbegrenzung in `NetSession::pump` das Late-Join-Verhalten ändert (NET-104): Tests nötig.

**Net_2.md**

- NET-201: tatsächlicher Anteil von `getaddrinfo`/`inet_ntop`/String-Keys an der Frame-Zeit bei 16-32 Peers (Profiler: `rawSend`, `handleDatagram`).
- NET-202: Stall-Dauer pro Tick bei 50-200 MB `put`/Snapshot über Loopback und über echtes Netz; Kernel-Sendepuffer-Größe pro Plattform bestimmt, wie viel pro Tick abfließt.
- NET-205: Gewinn durch Stack-Puffer und inkrementelle `ackBits` nur nach NET-201 sinnvoll messbar.
- NET-206: ob die `CollabController`-Aufrufstellen im UI-Thread laufen; Dauer von `getaddrinfo` bei Offline-Betrieb je Plattform.
- NET-207: ob in der Praxis Sockets mit `fd >= 1024` entstehen (Editor mit vielen offenen Dateien); Windows-`WSAPoll`-Verhalten bei fehlgeschlagenem Non-Blocking-Connect.
- `UdpTransport::serviceReliability/ackOne/windowAdmits` (Z. 801-920) laufen O(`inFlight` ≤ 256) pro Paket/Ack; bei Bulk-Transfers zu vielen Peers evtl. messbar, nicht als Fund aufgenommen, weil die Zahlen klein sind.
- `ScClient`/`TeamClient`: Dateigrößenobergrenze eines `put` im Produktivbetrieb (bestimmt, wie relevant NET-204(3) ist).


---
# Funde (vollständig, nach Teilbericht)

## Net Slice 1 – CollabSession, HTTP/HTTPS, LanBeacon, Loopback/Lossy, PortMapper, RelayFanout, NetSession + alle Net-Header

## Zusammenfassung

Der Code ist durchgehend sorgfältig kommentiert und defensiv (Größenlimits vor Allokation, Identität aus der Verbindung statt aus der Nutzlast, Logging über `HE_LOG_*` mit lazy Argumenten). Die größten Hebel liegen nicht in der Kollaborationslogik, sondern in der untersten Schicht: `BitWriter`/`BitReader` arbeiten Bit für Bit, auch für Bulk-Bytes (Snapshots, Assets), und `NetSession` formt jede Nachricht mehrfach um und pro Empfänger neu. Darauf bauen die Asset-/Snapshot-Pfade auf, die alles auf dem Hauptthread und pro Peer erneut serialisieren. Daneben gibt es einen echten Überlauf-Bug im Snapshot-Chunk-Handler, mehrere „erst `resize`, dann prüfen"-Stellen, nicht aufgeräumte Transfer-Zustände nach Disconnect und einen Log-Fehler in `LanBeacon::Browser::update`. Der Rest sind Vereinfachungen (Handler-Boilerplate, tote Parameter, doppelte Helfer, schwere Header).

Funde: 3 Hoch, 6 Mittel, 14 Niedrig. Alle Zeilenangaben gegen den aktuellen Stand geprüft. Performance-Zahlen sind Schätzungen, keine Messungen.

---

### NET-101 – BitWriter/BitReader verarbeiten Bulk-Bytes Bit für Bit
- **ID**: NET-101
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Net/include/Net/BitStream.h:27-39`, `:69-72`, `:111-125`, `:161-167`
- **Problem**: `writeBits` schleift über jedes einzelne Bit und prüft pro Bit auf Byte-Überlauf. `writeBytes` ruft pro Byte `writeByte` → `writeBits(v, 8)` (8 Iterationen), `readBytes` pro Byte `readByte` → `readBits(…, 8)` (8 Iterationen mit Byte/Bit-Index-Berechnung). Das gilt auch dann, wenn der Strom byte-ausgerichtet ist, was bei Snapshot-Chunks (256 KiB), Asset-Chunks, Komponenten-/Terrain-/Struktur-Blobs und dem Payload-Umkopieren in `NetSession::send` der Normalfall ist. Ein 64-MiB-Snapshot (Limit `maxSnapshotBytes`) bedeutet auf dem Schreibpfad allein hier ~0,5 Mrd. Bit-Iterationen, auf dem Lesepfad nochmals so viele.
- **Code-Auszug**:
```cpp
void writeBits(std::uint32_t value, int numBits) {
    ...
    for (int i = 0; i < numBits; ++i) {
        const std::uint32_t bit = (value >> i) & 1u;
        m_current |= static_cast<std::uint8_t>(bit << m_bitInByte);
        if (++m_bitInByte == 8) { m_bytes.push_back(m_current); ... }
    }
}
void writeBytes(const void* data, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i) writeByte(p[i]);
}
```
- **Lösung**: (1) `writeBytes`/`readBytes` bekommen einen Schnellpfad für den ausgerichteten Fall (`m_bitInByte == 0` bzw. `m_bitPos % 8 == 0`): `m_bytes.insert(end, p, p+len)` bzw. `memcpy`. Unausgerichtet bleibt die bestehende Schleife. (2) `writeBits` schreibt byteweise: so viele Bits wie in das aktuelle Byte passen, dann ganze Bytes. (3) `readBits` analog über Byte-Fenster statt Bit-Schleife. (4) Zusätzlich `reserve()`-Möglichkeit und ein `const std::vector<uint8_t>& bytes()`-Zugriff für den ausgerichteten Fall (siehe NET-102). Die Bit-Reihenfolge (LSB-first) ändert sich nicht, das Wire-Format bleibt byte-identisch.
```cpp
void writeBytes(const void* data, std::size_t len) {
    if (m_bitInByte == 0) { auto* p = static_cast<const std::uint8_t*>(data);
                            m_bytes.insert(m_bytes.end(), p, p + len); return; }
    ...bestehende Schleife
}
```
- **Erwarteter Effekt**: Schätzung: Bulk-Pfade (Snapshot, Assets, Blobs) 10-50x schneller; bei 64 MiB von mehreren Sekunden CPU auf wenige Dutzend Millisekunden. Kleine Nachrichten (Presence, Transform) profitieren moderat.
- **Risiko / Verifikation**: Gering, solange das Bitlayout unverändert bleibt. Absichern mit einem Property-Test (zufällige Folge aus `writeBool/Bits/Byte/UInt*/Bytes/String`, ausgerichtet und unausgerichtet, Ergebnis gegen die alte Implementierung als Referenz im Test vergleichen) und den vorhandenen Collab-/Replication-Tests unter `tests/`. Wire-Kompatibilität: Goldene Byte-Folgen für `LanBeacon::encode` und einige Collab-Frames festschreiben.

---

### NET-102 – NetSession::send formt jede Nachricht dreifach um; broadcast pro Peer neu
- **ID**: NET-102
- **Kategorie**: C
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/NetSession.cpp:174-221`, `:229-236`; `src/HE_Net/include/Net/BitStream.h:86-90`
- **Problem**: `send()` ruft `payload.data()` (Kopie 1, `data()` liefert einen Vektor per Wert), schreibt diesen Inhalt über `framed.writeBytes` Byte für Byte in ein neues `BitWriter` (Kopie 2, Bit-Schleife, siehe NET-101) und holt mit `framed.data()` eine weitere Kopie (3). `broadcast()` und `broadcastExcept()` rufen `send()` pro Peer, d. h. die komplette Umformung läuft N-mal für identische Bytes (Presence mit 10 Hz, Transform-Relays pro Drag-Frame, Asset-Chunks mit 256 KiB). `broadcast()` kopiert außerdem `m_connections` pro Aufruf auf den Heap, `broadcastExcept()` tut das nicht, obwohl beide Kommentare dieselbe Begründung hätten.
- **Code-Auszug**:
```cpp
BitWriter framed;
framed.writeUInt16(id);
const std::vector<std::uint8_t> pd = payload.data();
if (!pd.empty()) framed.writeBytes(pd.data(), pd.size());
const std::vector<std::uint8_t> bytes = framed.data();
...
const std::vector<ConnectionId> peers = m_connections;   // broadcast()
for (const ConnectionId conn : peers) send(conn, id, payload, mode);
```
- **Lösung**: Rahmen direkt als Bytevektor bauen: das 16-Bit-Id-Feld ist LSB-first, also zwei Bytes (low, high), danach der Payload verbatim. Der Rahmen ist damit unabhängig vom Empfänger und wird einmal pro `send/broadcast/broadcastExcept/sendTo` gebaut (`frame()` existiert bereits für den Fan-out-Pfad und kann dafür die einzige Implementierung werden). `BitWriter` bekommt dafür `appendTo(std::vector<uint8_t>&)` oder `bytes()` ohne Kopie. Die `m_connections`-Snapshot-Kopie in `broadcast()` entfällt: `ITransport::send` kann keinen Handler aufrufen (Loopback legt nur in die Peer-Queue), das Argument des Kommentars trägt also nicht; sollte es doch nötig sein, dann in beiden Funktionen gleich.
```cpp
std::vector<std::uint8_t> NetSession::frame(MessageId id, const BitWriter& p) const {
    std::vector<std::uint8_t> out; out.reserve(2 + p.byteSize());
    out.push_back(id & 0xFF); out.push_back(id >> 8);
    p.appendTo(out); return out;
}
```
- **Erwarteter Effekt**: Schätzung: pro Nachricht 3 Heap-Allokationen und ein Bit-Pass weniger; bei Broadcasts mit N Peers Faktor ~N weniger Framing-Arbeit. In Kombination mit NET-101 für große Chunks der Unterschied zwischen „spürbarem Hänger" und „unmessbar".
- **Risiko / Verifikation**: Wire-Bytes müssen identisch bleiben: Test, der `frame()` gegen einen aus der alten Implementierung erzeugten Referenzrahmen vergleicht (mit Payload, dessen letztes Byte partiell gefüllt ist, und leer). Vorhandene Loopback/Collab-Tests decken den Rest ab.

---

### NET-103 – Host serialisiert jedes Asset für jeden Peer erneut (und umgeht damit RelayFanout)
- **ID**: NET-103
- **Kategorie**: D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/CollabSession.cpp:1698-1700`, `:1711-1745`, `:2396-2399`
- **Problem**: Beim Publizieren (`sendAsset`) und beim Weiterreichen eines Client-Assets (`kMsgAssetEnd`-Handler) ruft der Host `sendAssetChunks` einmal pro Verbindung. Jeder Aufruf baut Begin/Chunks/End neu auf, obwohl die Frames für alle Empfänger identisch sind (derselbe `from`-Stempel). Bei einem Asset bis 64 MiB und 7 Gästen sind das 7 komplette Serialisierungen (je NET-101-Kosten) im Editor-Thread. Zusätzlich: `NetSession` hat extra `sendTo/broadcastExcept` und `RelayFanout` einen `sendToMany`-Pfad („ein Frame, einmal verschlüsseln"), der hier nie benutzt wird, weil `send()` pro `ConnectionId` aufgerufen wird.
- **Code-Auszug**:
```cpp
if (m_role == NetRole::Host) {
    for (const ConnectionId c : m_net->connections()) sendAssetChunks(c, m_localId, a);
    return true;
}
...
for (const ConnectionId other : m_net->connections()) {
    if (other != conn) sendAssetChunks(other, a.sender, up);
}
```
- **Lösung**: `sendAssetChunks` in zwei Schritte teilen: Frames einmal erzeugen (Begin, Chunk i, End) und dann je Frame `m_net->sendTo(targets, id, writer)` bzw. `broadcastExcept(conn, …)` aufrufen. Die Reihenfolge Begin → Chunks → End bleibt pro Peer erhalten; über Peers hinweg wird sie nicht mehr verschränkt, das ist unkritisch (jeder Peer hat eine eigene geordnete Verbindung). Bei Relay-Sessions wird dadurch das Fan-out überhaupt erst wirksam. Das Einzelziel (`sendAssetTo` für Late-Joiner) bleibt mit einer Ziel-Liste der Länge 1 derselbe Code.
- **Erwarteter Effekt**: Schätzung: CPU-Zeit für Asset-Publish linear in der Peerzahl → konstant; bei 8 Teilnehmern bis zu ~7x weniger Serialisierung, bei Relay zusätzlich ~Nx weniger verschlüsselte/versandte Bytes.
- **Risiko / Verifikation**: Mittel-gering. Tests: Host + 2-3 Loopback-Clients, Asset von Host und von Client A senden, Inhalt/Intent/Revision bei allen anderen vergleichen; ein Test mit `RelayFanout` (Fake-Relay aus den vorhandenen Relay-Tests) prüft, dass genau ein Broadcast-Frame pro Chunk entsteht. Create-Result-Antwort (`kMsgAssetCreateResult`) bleibt Einzelnachricht an den Absender.

---

### NET-104 – Join und Transfers laufen ungedrosselt im Frame: Snapshot-Capture + alle Chunks in einem Aufruf, `pump()` ohne Budget
- **ID**: NET-104
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/CollabSession.cpp:479-551`; `src/HE_Net/src/NetSession.cpp:262-322`
- **Problem**: `handleJoinRequest` fängt den Snapshot synchron (`captureSnapshot`) und sendet danach alle Chunks in einer Schleife in denselben Frame; die Kosten stecken in `captureSnapshot` plus NET-101/102. Der gesamte Snapshot landet dabei am Stück im Ausgangspuffer der Verbindung (bis zu 64 MiB zusätzlich zum `snapshot`-Vektor). Auf der Empfängerseite zieht `NetSession::pump` alle anstehenden Events in einer `while (poll)`-Schleife ohne Zeit- oder Mengenbudget; jedes Chunk-Event läuft komplett durch den Handler. Für den Host bedeutet ein Join mitten in der Arbeit einen sichtbaren Hänger, für den Client ebenfalls beim Empfang. Das Problem ist ohne Messung nicht quantifizierbar, die Struktur (alles im Frame) ist aber eindeutig.
- **Code-Auszug**:
```cpp
if (!m_state || !m_state->captureSnapshot(snapshot)) { ... }
...
for (std::uint32_t i = 0; i < chunks; ++i) {
    ...
    chunk.writeBytes(snapshot.data() + offset, len);
    m_net->send(conn, kMsgSnapshotChunk, chunk);
}
```
```cpp
NetEvent ev;
while (m_transport->poll(ev)) { switch (ev.type) { ... } }
```
- **Lösung**: (1) Nach NET-101/102 zuerst messen (`HE_LOG=Net=Trace`, Join mit 5/20/60 MiB-Szene). (2) Falls weiter nötig: pro `update()` nur ein Chunk-Budget je Joiner senden (Zustandsautomat „SnapshotSending{conn, offset}") und bei TCP auf Backpressure achten (`connectionCount`/Queue-Tiefe ist nicht abfragbar, daher festes Chunk-Budget). (3) `pump(maxEvents)` bzw. Zeitbudget, damit ein Burst nicht den ganzen Frame blockiert; Reihenfolge je Verbindung bleibt erhalten, weil nur Rest-Events im Transport verbleiben. `captureSnapshot` selbst gehört zu HE_Scene/Editor.
- **Erwarteter Effekt**: Schätzung: Frame-Spitze beim Join von „Dauer der gesamten Serialisierung" auf „Dauer eines Chunks"; Gesamtdauer des Joins bleibt gleich oder wird wegen kleiner Budgets minimal länger.
- **Risiko / Verifikation**: Mittel: Zeitbudgets verändern das Timing von Handshake-Tests (Late-Join-Reihenfolge: Snapshot vor Lock-Tabelle/Presence/Asset-Catch-up, siehe Kommentar „the link is ordered"). Tests mit zwei `NetSession`s über Loopback und `LossyTransport` mit Latenz; Lock-Tabelle, Presence und Asset-Catch-up dürfen erst nach `SnapshotEnd` gesendet werden.

---

### NET-105 – Snapshot-Chunk-Prüfung kann überlaufen (uint32), danach `resize` auf Peer-Wert
- **ID**: NET-105
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (Korrektheit/Robustheit)
- **Ort**: `src/HE_Net/src/CollabSession.cpp:973-992`
- **Problem**: `m_snapshotReceived + len > m_snapshotExpected` addiert zwei `uint32_t` ohne Überlaufschutz. Mit `len = 0xFFFFFFF0` und `m_snapshotReceived >= 0x10` läuft die Summe über, die Prüfung passiert, und `m_snapshotBuf.resize(offset + len)` fordert ~4 GiB an (OOM/`bad_alloc` im Client-Prozess). Der Kommentar „Refuse a chunk that would push the payload past the announced size" will genau das verhindern. Der Asset-Handler (`:2347`, `size_t`-Arithmetik) ist unauffällig.
- **Code-Auszug**:
```cpp
if (len == 0 || m_snapshotReceived + len > m_snapshotExpected) { ... abort ... }
const std::size_t offset = m_snapshotBuf.size();
m_snapshotBuf.resize(offset + len);
```
- **Lösung**: Subtraktion statt Addition: `len > m_snapshotExpected - m_snapshotReceived` (letzteres ist nie negativ, da `received <= expected` Invariante). Zusätzlich NET-106: erst prüfen, ob der Rahmen `len` Bytes enthält.
- **Erwarteter Effekt**: Keine Performance-Änderung; schließt einen Absturz-Pfad durch einen fehlerhaften/feindlichen Host.
- **Risiko / Verifikation**: Minimal. Neuer Test: Begin(total=100), Chunk(len=50), Chunk(len=0xFFFFFFF0) → `onJoinRejected(SnapshotFailed)`, kein großer Allokationsversuch.

---

### NET-106 – Mehrere Reader allozieren/nullen den vom Peer behaupteten Puffer, bevor geprüft wird, dass der Rahmen ihn enthält
- **ID**: NET-106
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Beides
- **Ort**: `src/HE_Net/src/CollabSession.cpp:1219-1226` (`readComponentBody`), `:1296-1308` (`readTerrainBody`), `:1566-1587` (`readStructuralBody`), `:990-992` (Snapshot-Chunk)
- **Problem**: Diese Funktionen begrenzen `len` auf `maxBlob`/`maxSnapshotBytes` (64 MiB), rufen aber `blob.resize(len)` auf, bevor `readBytes` prüft, ob der Rahmen überhaupt so viele Bytes hat. Eine einzelne kleine Nachricht eines beliebigen beigetretenen Clients lässt den Host bis zu 64 MiB anfordern und nullen (und wieder freigeben), pro Nachricht. `readAvatar` (`:845-850`) macht es richtig („Nothing is allocated until the frame actually holds that many bytes") – das Muster ist also bekannt, aber nicht überall angewendet.
- **Code-Auszug**:
```cpp
if (len == 0 || len > maxBlob) return false;   // never allocate on trust
blob.resize(len);
return r.readBytes(blob.data(), len);
```
- **Lösung**: Ein gemeinsamer Helfer `BitReader::readByteVector(std::vector<uint8_t>& out, size_t len)`, der zuerst `bitsRemaining() >= len*8` prüft, dann `resize` + (mit NET-101) `memcpy`. Alle vier Stellen und `readAvatar` nutzen ihn. Das ist zugleich eine Vereinfachung (vier Kopien derselben Zeilen).
- **Erwarteter Effekt**: Schätzung: eliminiert den Verstärkungsfaktor Nachricht → 64 MiB Allokation; kein Effekt im Normalbetrieb.
- **Risiko / Verifikation**: Gering. Test mit absichtlich verkürzten Frames (len = Maximum, 0 Bytes Rest) für jedes Update-Kommando: Handler kehrt zurück, keine Callback-Aufrufe, Peak-Speicher bleibt klein.

---

### NET-107 – Transfer-Zustand wird bei Disconnect nicht aufgeräumt (`m_assetAssembly`, Snapshot-Puffer)
- **ID**: NET-107
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung (Leak)
- **Ort**: `src/HE_Net/src/CollabSession.cpp:596-628` (`onPeerDisconnected`), `:633-658` (`retireParticipant`), `:1796-1812`, `:2184-2198` (Assembly), `:961-967`
- **Problem**: `m_assetAssembly` wird nur beim nächsten Begin desselben Peers, bei End oder nach erfolgreichem Create-Streit bereinigt. Bricht ein Peer mitten im Transfer ab oder wird gekickt, bleibt sein `AssetAssembly` samt `bytes.reserve(total)` (bis 64 MiB, siehe `:1804`) für die Lebensdauer der Session im Vektor, da `ConnectionId`s nicht wiederverwendet werden. Auf dem Client löscht der Disconnect-Zweig weder `m_assetAssembly` noch `m_snapshotBuf`/`m_snapshotActive`/`m_localId`; ein Abbruch mitten im Snapshot hält bis zu 64 MiB, bis ein neuer Join `handleSnapshotBegin` erreicht.
- **Code-Auszug**:
```cpp
m_joined   = false;
m_joinSent = false;
m_participants.clear();
m_presence.clear();
m_locks.clear();
// kein m_assetAssembly.clear(), kein m_snapshotBuf-Reset
```
- **Lösung**: In `retireParticipant` (Host) `m_assetAssembly` per `remove_if` auf `from == conn` bereinigen; im Client-Zweig von `onPeerDisconnected` `m_assetAssembly.clear()`, `m_snapshotActive=false`, `m_snapshotBuf = {}`. Eine kleine private Methode `dropTransferState(conn)` hält beide Stellen konsistent.
- **Erwarteter Effekt**: Verhindert wachsende, ungenutzte Reservierungen bei instabilen Peers; im Normalfall kein Unterschied.
- **Risiko / Verifikation**: Gering. Test: Peer sendet AssetBegin (total = 10 MiB), Verbindung fällt, `m_assetAssembly` leer (über ein Test-Hook/Speichermessung oder indirekt: neues Begin eines neuen Peers erzeugt keinen Rest).

---

### NET-108 – Asset-Bytes werden unnötig vollständig kopiert (Senden und Empfangen)
- **ID**: NET-108
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/CollabSession.cpp:1665-1673`, `:2371-2376`
- **Problem**: Die Drei-Argument-Variante `sendAsset(subject, path, bytes)` kopiert die komplette `bytes`-Referenz in ein neues `AssetUpdate` (bis 64 MiB), nur um sie sofort an `sendAssetChunks` weiterzureichen. Im End-Handler wird `up.bytes = a.bytes` kopiert, obwohl `a` direkt danach aus `m_assetAssembly` gelöscht wird und nach dem Kopieren nur `a.sender` gelesen wird.
- **Code-Auszug**:
```cpp
a.bytes   = bytes;   // sendAsset(subject, path, bytes)
...
up.bytes   = a.bytes;   // End-Handler; a wird danach erase()d
```
- **Lösung**: End-Handler: `up.bytes = std::move(a.bytes);` (`a.sender` vorher in eine lokale Variable, falls nötig; `a.bytes.size()` wird danach nicht mehr benötigt). `sendAsset(subject,path,bytes)`: interne Variante `sendAssetImpl(subject, path, const std::vector<uint8_t>&, intent, revision)` verwenden, die `sendAssetChunks` mit einer View (`const uint8_t*, size_t`) füttert, statt ein `AssetUpdate` zu bauen; die öffentliche `AssetUpdate`-API bleibt unverändert.
- **Erwarteter Effekt**: Schätzung: je ein Memcpy der Asset-Größe weniger pro Senden und pro Empfangen (bei 64 MiB ~10-20 ms und Spitzenspeicher halbiert).
- **Risiko / Verifikation**: Gering. Die Move-Variante ändert nichts am Callback-Vertrag (`const AssetUpdate&`). Asset-Roundtrip-Tests (Host↔Client, Create/Update) genügen.

---

### NET-109 – `setLocalPresence` (jeden Frame) kopiert den gesamten Presence-Zustand zweimal und macht linearen Lookup
- **ID**: NET-109
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/CollabSession.cpp:1040-1068`, `:1086`
- **Problem**: Der Header sagt „Cheap to call every frame", aber jeder Aufruf kopiert die Auswahl (`m_localPresence.selection = selection`, bis 1024 × 8 Byte), kopiert dann den kompletten `PresenceState` per Wert in `applyPresence(m_localId, m_localPresence)` und move-assigned ihn in den Vektor (lineare Suche über die Teilnehmer). Danach vergleicht `presenceDiffersFromSent()` in `update()` jeden Frame erneut die Selektion. Bei großer Auswahl ist das pro Frame Heap-/Kopierarbeit, ohne dass sich etwas geändert hat.
- **Code-Auszug**:
```cpp
m_localPresence.selection = selection;
...
applyPresence(m_localId, m_localPresence);   // Kopie des ganzen PresenceState
```
- **Lösung**: In `setLocalPresence` zuerst auf Änderung prüfen (Kamera mit denselben Epsilon-Regeln wie `presenceDiffersFromSent`, `selection == m_localPresence.selection`) und nur bei Änderung speichern/spiegeln. Für das Spiegeln in `m_presence` die Zuweisung vorhandener Elemente nutzen (kein Wegwerf-Temporary) oder `m_presence` den lokalen Eintrag per Referenz auf `m_localPresence` führen lassen. Zusätzlich ein `m_localDirty`-Flag, das `presenceDiffersFromSent` im unveränderten Fall spart.
- **Erwarteter Effekt**: Schätzung: bei 500 selektierten Entities ~8 KB Kopie × 2 pro Frame → 0; sonst im Rauschen. Wirkt nur, wenn der Editor tatsächlich jeden Frame aufruft (Aufrufer in `HE_Editor/CollabController.cpp` nicht geprüft).
- **Risiko / Verifikation**: Gering. `onPresenceChanged` für den lokalen Teilnehmer wird heute nicht ausgelöst (kein Callback in `setLocalPresence`), das bleibt so. Test: wiederholter Aufruf mit identischen Werten erzeugt keine zusätzliche Sendung; `presenceOf(localId)` bleibt aktuell.

---

### NET-110 – UPnP: `LOCATION`/`controlURL` aus unauthentifizierten LAN-Antworten werden ungeprüft angesprochen
- **ID**: NET-110
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung (Härtung)
- **Ort**: `src/HE_Net/src/PortMapper.cpp:313-332`, `:113-125`, `:156`
- **Problem**: `discover()` liest die `LOCATION`-URL aus der SSDP-Antwort und führt ein `httpGet` darauf aus, ohne zu prüfen, dass der Host der URL der Absender (`fromHost`) ist. `resolveUrl` akzeptiert außerdem absolute `controlURL`s aus der Beschreibung unverändert. Ein Gerät im selben Netz (Café, Büro) kann den Editor so dazu bringen, GET/SOAP-POST an beliebige interne Hosts zu senden (blindes SSRF; später werden Ports mit einer Konfiguration des Angreifers „gemappt").
- **Code-Auszug**:
```cpp
const std::string location = parseSsdpLocation(response);
...
const HttpResponse desc = httpGet(location, 4000);
...
if (toLower(relative).compare(0, 7, "http://") == 0) return relative;
```
- **Lösung**: URL parsen (`httpParseUrl`) und verlangen, dass `host == fromHost` (für IPv6 numerisch normalisiert) und nur der Port frei bleibt; in `resolveUrl` absolute URLs nur akzeptieren, wenn der Host mit `baseUrl` übereinstimmt. Verhalten für ehrliche Router bleibt gleich.
- **Erwarteter Effekt**: Keine Performance-Wirkung; reduziert Angriffsfläche.
- **Risiko / Verifikation**: Gering-mittel: Manche Router liefern `LOCATION` mit anderer Adresse (mehrere Interfaces/Bridge). Fallback „gleiches /24 wie die Absenderadresse" oder nur loggen und nicht ablehnen, falls Tests mit Fake-Routern (`tests/test_port_mapper*` o. ä.) ausfallen.

---

### NET-111 – HttpClient: unbegrenzte Antwort, abgeschnittene Antwort gilt als Erfolg, veralteter Kopfkommentar
- **ID**: NET-111
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Net/src/HttpClient.cpp:272-301`, `:158-188`; `src/HE_Net/include/Net/HttpClient.h:7-12`
- **Problem**: (1) `raw` wächst bis zum Deadline-Ende ohne Obergrenze; ein feindlicher „Router" kann 5 s lang Daten liefern. (2) Bei Timeout oder Abbruch wird das bis dahin Empfangene trotzdem geparst; ist `Content-Length` größer als der empfangene Body (`body.size() < want`), liefert `httpParseResponse` `ok = true` mit abgeschnittenem Body, bei `chunked` ebenso (`decodeChunked` bricht bei „truncated" lautlos ab). (3) Der Verbindungsaufbau pollt `socketConnectPoll` mit 2-ms-Sleeps statt auf Schreibbereitschaft zu warten (Worker-Thread, daher nur ein Schönheitsfehler). (4) Der Kopfkommentar behauptet „the engine currently links only the crypto primitives … not a TLS stack"; `HttpsClient.h` existiert und delegiert an das OS – der Hinweis ist veraltet. (5) `findHeader` erzeugt pro Aufruf eine Kleinschreib-Kopie des Header-Blocks, ebenso `httpParseResponse` zwei Substring-Kopien des gesamten Bodys.
- **Code-Auszug**:
```cpp
if (rr == SocketResult::Ok) { raw.append(reinterpret_cast<char*>(buf), got); continue; }
...
if (body.size() > want) body.resize(want);   // body.size() < want wird nicht erkannt
```
- **Lösung**: Obergrenze (z. B. 1 MiB, für Gerätebeschreibung/SOAP ausreichend) in `httpRequest`; in `httpParseResponse` bei `Content-Length` und `body.size() < want` bzw. unvollständigem Chunk-Ende `ok=false` mit `error="truncated"`; Kommentar aktualisieren; optional `socketWaitWritable` statt Sleep-Schleife.
- **Erwarteter Effekt**: Robuster gegen kaputte/feindliche Geräte; Performance irrelevant (selten, Worker-Thread).
- **Risiko / Verifikation**: Gering. Es gibt reine Parser-Tests (`httpParseResponse`); ergänzen: abgeschnittene Antworten, sehr große Antwort.

---

### NET-112 – HTTPS-Backends: pro Request neue Session/TLS-Handshake, Win32 verschluckt Lesefehler, uneinheitliche Timeouts
- **ID**: NET-112
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Beides
- **Ort**: `src/HE_Net/src/HttpsClient_Win.cpp:306-318`, `:373-386`; `src/HE_Net/src/HttpsClient_Curl.cpp:170-197`; `src/HE_Net/src/HttpsClient_Apple.mm:66-69`
- **Problem**: (1) Alle drei Backends bauen pro Aufruf Session, Verbindung und TLS-Handshake neu auf (`WinHttpOpen`+`WinHttpConnect`, `curl_easy_init`, `NSURLSession` ephemeral). Wie oft `SessionDirectory::heartbeat` läuft, liegt außerhalb dieses Slice (`SessionDirectory.cpp`); bei periodischen Heartbeats wäre Wiederverwendung (ein `CURL*` pro Thread, ein `HINTERNET`-Session-Handle) billig. (2) Windows: Schlägt `WinHttpReadData`/`WinHttpQueryDataAvailable` mitten im Body fehl, bleibt `out.ok = true` mit abgeschnittenem Body; die Daten werden zudem pro Chunk in einen temporären `std::string` gelesen und angehängt statt direkt in `out.body`. (3) Keine Antwortgrößenbegrenzung (curl: `CURLOPT_MAXFILESIZE`/Write-Callback-Limit fehlt; `FOLLOWLOCATION` ohne `CURLOPT_MAXREDIRS`/`CURLOPT_REDIR_PROTOCOLS`, d. h. ein 307/308 schickt das POST-Body samt Management-Token an das Redirect-Ziel). (4) `timeoutMs` bedeutet je Backend etwas anderes: curl Gesamtdauer, WinHTTP je Phase (bis ~4x), NSURLSession Leerlaufzeit.
- **Code-Auszug**:
```cpp
::WinHttpSetTimeouts(session.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
...
if (!::WinHttpReadData(request.h, chunk.data(), available, &read)) break;
...
out.ok = true;
```
- **Lösung**: Windows: Lesefehler → `ok=false`, direkt in `out.body` lesen (`resize` + Read an Offset). curl: `CURLOPT_MAXREDIRS 3`, `CURLOPT_REDIR_PROTOCOLS_STR "https"` (bei http-URLs ggf. beides), Größenlimit im `writeCallback` (z. B. 4 MiB, bei Überschreitung 0 zurückgeben). Optional: Handle-Wiederverwendung hinter derselben API (thread-lokaler Cache). Timeout-Semantik in `HttpsClient.h` dokumentieren oder angleichen.
- **Erwarteter Effekt**: Schätzung: Handshake-Latenz pro Heartbeat (50-200 ms) entfällt bei Wiederverwendung; ansonsten Robustheit.
- **Risiko / Verifikation**: Gering-mittel (Redirect-Beschränkung kann den produktiven Webserver beeinflussen, falls er http→https umleitet; dann `https` erlauben und http-Downgrade verbieten). Nur per manueller Prüfung gegen den Verzeichnisdienst plus Unit-Test des `writeCallback`-Limits. macOS/Windows-Pfade im Linux-Container nicht baubar: nicht verifiziert.

---

### NET-113 – LanBeacon: Log nach `std::remove_if` liest unbestimmte Elemente
- **ID**: NET-113
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung (Korrektheit)
- **Ort**: `src/HE_Net/src/LanBeacon.cpp:547-556`
- **Problem**: Nach `std::remove_if` sind die Elemente im Bereich `[gone, end)` in gültigem aber unbestimmtem Zustand (verschobene Quellen). Die Schleife loggt `it->address` aus genau diesem Bereich; das Log nennt dadurch meist eine leere oder die falsche Adresse („LAN: lost the session at "). Die Logik ist auch unnötig verschachtelt (zwei Durchläufe).
- **Code-Auszug**:
```cpp
const auto gone = std::remove_if(m_sessions.begin(), m_sessions.end(), [&](const Session& s) { ... });
for (auto it = gone; it != m_sessions.end(); ++it)
    HE_LOG_INFO(Net, "LAN: lost the session at %s (stopped announcing)", it->address.c_str());
m_sessions.erase(gone, m_sessions.end());
```
- **Lösung**: Im Prädikat loggen (`return expired;` nach dem `HE_LOG_INFO`) oder in einer normalen Schleife mit `erase` iterieren; bei ≤ 32 Sessions (`kMaxSessions`) ist der O(n²)-Aufwand irrelevant.
- **Erwarteter Effekt**: Korrekte Logzeilen; keine Performance-Änderung.
- **Risiko / Verifikation**: Minimal. Ein Test über `Browser::ingest` + `update(nowMs+8000)` kann die Adresse im Log nicht prüfen; über eine kleine Log-Capture-Hilfe (falls vorhanden) oder rein durch Review absichern.

---

### NET-114 – LanBeacon::Browser: pro Frame ein `poll`-Syscall, unbegrenzte Drain-Schleife, `instance` überschreibt fremde Einträge
- **ID**: NET-114
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/LanBeacon.cpp:529-545`, `:486-526`
- **Problem**: Das Draining ruft pro Datagramm `socketWaitReadable(…, 0)` (ein Syscall) und danach `socketRecvFrom` (zweiter Syscall). Der Socket ist ohnehin nicht-blockierend, `recvfrom` meldet `WouldBlock` selbst. Die Schleife hat keine Obergrenze pro Frame; ein Flooder auf Port 47823 kann den Frame mit `decode` + `ingest` füllen. Außerdem überschreibt `ingest` für eine bekannte `instance` immer `address`/`port` aus dem neuen Paket: wer die `instance` einer echten Ankündigung kennt (öffentlich im Datagramm), kann den Listeneintrag umbiegen.
- **Code-Auszug**:
```cpp
while (socketWaitReadable(m_sock, 0)) {
    ...
    if (socketRecvFrom(m_sock, buf, sizeof(buf), got, from, fromPort) != SocketResult::Ok) break;
```
- **Lösung**: Direkt `socketRecvFrom` in der Schleife (Ende bei `WouldBlock`), maximal z. B. 64 Datagramme pro Update; bei bekannter `instance` die Adresse nur ändern, wenn sie übereinstimmt (andernfalls Eintrag ignorieren, eine echte Adresswechsel-Situation heilt sich über den Expiry).
- **Erwarteter Effekt**: Ein Syscall weniger pro Frame (nur während die Join-Liste offen ist), begrenzter Worst-Case; Schätzung: im Mikrosekundenbereich.
- **Risiko / Verifikation**: Gering. Bestehende `ingest`-Tests (Dedup, Expiry, Cap, Goodbye) unverändert; ein neuer Test für „andere Quelladresse mit gleicher instance wird nicht übernommen" – bewusst zu entscheiden, da dies ein Verhalten ändert (Adresswechsel bei DHCP-Renew während der Session).

---

### NET-115 – PortMapper: strikt sequentielle Leiter mit ~15 s Worst Case, doppelte Router-Abfragen
- **ID**: NET-115
- **Kategorie**: E
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/PortMapper.cpp:1138-1280`, `:221-359`, `:476-478`, `:785-786`, `:1094`
- **Problem**: `mapPort` probiert UPnP (`discover` bis 3 s + `addMapping` bis 5 s HTTP-Timeout), dann NAT-PMP (1,5 s) und PCP (1,5 s) nacheinander; bei einem Router ohne beides summiert sich das auf der Worker-Thread-Seite auf ~11-15 s bis zum Ergebnis. Zusätzlich holt `addMapping`/`natPmpAddMapping` nach dem Erfolg die externe IP mit einem weiteren Roundtrip (bis 1,5 s bzw. 5 s bei UPnP), und `openPinhole` ruft ein zweites `discover(dev, 3000)`, wenn der Aufrufer keine `IgdDevice` übergibt. In `discover` wird jeder Probe-Socket reihum mit 50 ms gepollt, statt mit einem `poll` über alle Sockets.
- **Code-Auszug**:
```cpp
if (!socketWaitReadable(probe.sock, 50)) continue;   // pro Interface 50 ms, reihum
...
if (discover(igd, 3000) == PortMapResult::Ok) { ... addMapping(...) }
// NAT-PMP, dann PCP, jeweils 1500 ms
```
- **Lösung**: NAT-PMP und PCP sind je ein Datagramm an dasselbe Gateway und Port 5351 und können parallel laufen (zwei Sockets, ein gemeinsames Deadline). Die UPnP-Discovery kann parallel zu NAT-PMP/PCP starten, da das Ergebnis dieser Zweige nur bei UPnP-Fehlschlag relevant ist. Mehrere Interfaces mit einer `poll`-Liste bedienen (neue Socket-Funktion oder Wartezeit pro Runde durch die Zahl der Probes teilen). Externe IP optional/asynchron nachladen.
- **Erwarteter Effekt**: Schätzung: Worst Case von ~15 s auf ~5-6 s, typischer Erfolgsfall (UPnP antwortet in < 1 s) unverändert. Läuft nicht im Frame-Thread, daher nur Wartezeit-Komfort im Host-Dialog.
- **Risiko / Verifikation**: Mittel: Parallelität verändert die Reihenfolge der Logzeilen und das Verhältnis „UPnP bevorzugt": die Auswahlregel „erst UPnP, dann NAT-PMP, dann PCP" muss bei parallelem Ergebnis erhalten bleiben. Nur mit echten Routern wirklich testbar; die Pure-Helper-Tests (PCP/NAT-PMP-Bytes) decken die Verzweigung nicht ab. Im Container nicht messbar → „messen".

---

### NET-116 – RelayFanout: mehrere vermeidbare Payload-Kopien pro Nachricht
- **ID**: NET-116
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/RelayFanout.cpp:187-194`, `:210-220`, `:277-302`, `:363`, `:411`
- **Problem**: Host-Eingang: `data.erase(data.begin())` verschiebt den gesamten Nutzlastpuffer um ein Byte (memmove der Chunk-Größe), um das Kind-Byte zu entfernen. Host-Ausgang: `sendPair` kopiert in `buf`, `sendToMany` baut `plain` (Kopie), `cipher` (Verschlüsselung) und `frame` (Kopie) nacheinander; Client-Eingang kopiert den Payload bei `HeldPair` (`assign`) und beim Broadcast ein weiteres Mal aus `plain` in `b.payload`. Zusammen mit `NetSession::fanOut`/`SecureTransport` sind es pro 256-KiB-Chunk 5-6 Kopien. Gegen AES-GCM und TCP ist das nur dann relevant, wenn NET-101/102 behoben sind.
- **Code-Auszug**:
```cpp
case Kind::Data:
    data.erase(data.begin());
    m_events.push_back(NetEvent{ NetEventType::Data, conn, std::move(data) });
```
- **Lösung**: Kind-Byte nicht per `erase` entfernen, sondern `NetEvent.data` mit einem Offset ausliefern (oder `ITransport::poll` mit Header-Skip) bzw. beim Erzeugen auf Sender-Seite so rahmen, dass Empfänger `std::vector::erase(begin)` nur einmal pro Chunk braucht – schlicht akzeptieren, wenn Chunks klein bleiben. Auf Sender-Seite `plain` direkt als Eingabe zur Verschlüsselung mit vorab reserviertem Platz für Index + Tag nutzen. Erst nach Messung angehen.
- **Erwarteter Effekt**: Schätzung: wenige Prozent der Relay-Pfad-CPU; ohne Messdaten nicht priorisieren.
- **Risiko / Verifikation**: Gering-mittel (Wire-Format und Ordnungslogik `merge()` sind subtil). Die vorhandenen RelayFanout-Tests (Ordnung Pair/Broadcast, Skip/Copy) müssen unverändert grün bleiben.

---

### NET-117 – `installHandlers`: 40 handgeschriebene Lambdas, die nur weiterleiten; Dispatch über `unordered_map<…, std::function>`
- **ID**: NET-117
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Net/src/CollabSession.cpp:158-287`, `:2330-2417`; `src/HE_Net/src/NetSession.cpp:170-172`, `:297-302`
- **Problem**: Jede Registrierung ist dieselbe Zeile `m_net->on(id, [this](ConnectionId c, BitReader& r){ handleX(c, r); })`, teils mit abweichender Signatur (`handleX(r)` ohne `conn`). Die Rolle (`Host`/`Client`) wird an zwei Stellen ausgewertet (in `installHandlers` und in `installAssetChunkHandlers` über `host` zur Laufzeit). Die Lambdas für Chunk/End sind je ~25-60 Zeilen lang inline in einer Installationsfunktion. Der Dispatch selbst (`unordered_map` + `std::function` + zweifacher Sprung) ist für die Nachrichtenraten nicht kritisch, aber unnötig allgemein: Nachrichten-IDs `1024 + 0..41` sind dicht.
- **Code-Auszug**:
```cpp
m_net->on(kMsgLockRequest, [this](ConnectionId conn, BitReader& r) {
    handleLockRequest(conn, r);
});
m_net->on(kMsgLockRelease, [this](ConnectionId conn, BitReader& r) {
    handleLockRelease(conn, r);
});
```
- **Lösung**: Eine kleine Hilfe `bind(id, &CollabSession::handleX)` (Template über Member-Zeiger, zwei Überladungen für `(conn, r)` und `(r)`), die ein Tabellenliteral je Rolle erlaubt: `for (auto [id, fn] : kHostHandlers) bind(id, fn);`. Die Chunk-/End-Lambdas werden zu Memberfunktionen `handleAssetChunk(conn, r, host)` / `handleAssetEnd(conn, r, host)`. Optional `NetSession` auf einen `std::vector<MessageHandler>` (Index `id - kFirstUserMessage`) umstellen; ohne Messung nicht nötig.
- **Erwarteter Effekt**: Rund 100 Zeilen weniger Boilerplate, besser lesbare Rolle→Handler-Zuordnung. Performance: nicht messbar.
- **Risiko / Verifikation**: Gering; reine Umstrukturierung, die vollständigen Collab-Tests (Host/Client/Loopback) müssen unverändert grün sein.

---

### NET-118 – `handleJoinRequest` ist eine 210-Zeilen-Funktion mit sieben Ablehnungsgründen und vier Nebenaufgaben
- **ID**: NET-118
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Net/src/CollabSession.cpp:383-594`
- **Problem**: Eine Funktion liest und validiert das Paket, trifft acht Ablehnungsentscheidungen, fängt den Snapshot, baut die Annahme, sendet Snapshot-Chunks, registriert den Teilnehmer, sendet Lock-Tabelle und Presence-Nachzügler und kündigt den Beitritt an. Reihenfolgeabhängigkeiten stehen in Kommentaren, nicht in Struktur.
- **Code-Auszug**:
```cpp
void CollabSession::handleJoinRequest(ConnectionId conn, BitReader& r) {
    ...  // parse + 8x reject(...)
    ...  // captureSnapshot
    ...  // JoinAccepted
    ...  // chunk loop
    ...  // register + lock table + presence + announce
```
- **Lösung**: In `readJoinRequest` (Parsing), `checkJoinPolicy` (liefert `std::optional<JoinRejectReason>` + Detail), `sendJoinAccepted`/`sendSnapshot` und `admitParticipant` (Registrierung + Nachzügler + Ankündigung) aufteilen. Reihenfolge bleibt exakt.
- **Erwarteter Effekt**: Lesbarkeit/Testbarkeit; Voraussetzung für das Zeit-Budget aus NET-104 (der Snapshot-Teil wird zu einem fortsetzbaren Zustand).
- **Risiko / Verifikation**: Gering; Bestehende Join-/Reject-Tests (alle `JoinRejectReason`-Varianten) absichern.

---

### NET-119 – Schwere Header: `ScClient.h`/`TeamClient.h` ziehen `RelayTransport.h` + `TcpTransport.h` wegen einer 4-Zeilen-Struktur
- **ID**: NET-119
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Net/include/Net/ScClient.h:21`, `:33`; `src/HE_Net/include/Net/TeamClient.h:50-54`; `src/HE_Net/include/Net/RelayTransport.h:44, 54-58`
- **Problem**: Beide Header brauchen nur `RelayAddress` (`host`, `port`), inkludieren dafür aber `RelayTransport.h` und damit `TcpTransport.h`, `Socket.h`, `ITransport.h`, `<deque>`, `<memory>`. Die Vorwärtsdeklaration `class TcpTransport;` in `ScClient.h` ist dadurch wirkungslos. Editor-Dateien (`CollabController.h`, `ProjectSettingsPanel.cpp`, `TeamView.cpp`, …) ziehen den Transportstapel ebenfalls nur für `parseRelayAddress`. Der MSVC-Kommentar (vollständiger Typ wegen dllexport und `unique_ptr<TcpTransport>`) gilt für `ScClient`/`TeamClient`, nicht für `RelayAddress`.
- **Code-Auszug**:
```cpp
#include "Net/RelayTransport.h"   // RelayAddress; the team server IS the relay
#include "Net/TcpTransport.h"
```
- **Lösung**: `RelayAddress`, `parseRelayAddress` und `effectiveRelayAddress` in `Net/RelayAddress.h` auslagern (nur `<string>`, `<cstdint>`); `RelayTransport.h` inkludiert es. `ScClient.h`/`TeamClient.h` inkludieren `RelayAddress.h`; die Header, die tatsächlich `TcpTransport` als Member brauchen (`std::unique_ptr<TcpTransport>`), behalten `TcpTransport.h`. Editor-Dateien, die nur die Adresse parsen, wechseln auf `RelayAddress.h`.
- **Erwarteter Effekt**: Kleinere Inkludier-Ketten in `HE_Editor` (mehrere TUs); Schätzung: wenige Prozent Build-Zeit für diese TUs.
- **Risiko / Verifikation**: Gering; reiner Include-Umbau, Build aller Ziele (`HorizonEditor`, `he_tests`, `HorizonGame`) genügt.

---

### NET-120 – Tote oder redundante Parameter und Deklarationen
- **ID**: NET-120
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Net/src/CollabSession.cpp:2521-2523`, `:633-658` (Aufrufe `:623`, `:708`), `:377-381`; `src/HE_Net/include/Net/CollabSession.h:974`, `:1027`, `:984`; `src/HE_Net/src/PortMapper.cpp:1014-1016`, `:25`
- **Problem**: (1) `broadcastLock(…, ConnectionId /*except*/)`: Der Parameter wird nie benutzt und von jedem der sieben Aufrufer mit `kInvalidConnection` belegt. (2) `retireParticipant(id, conn, except)`: beide Aufrufer übergeben `conn, conn`. (3) `sendSnapshotTo(ConnectionId)` ist in `CollabSession.h:974` deklariert, aber nirgends definiert oder aufgerufen. (4) `onPeerConnected` ist ein leerer Handler, nur ein Kommentar. (5) `PortMapper.cpp:1014` trägt eine Abschnittsüberschrift „The fallback ladder", direkt gefolgt von einer anderen Überschrift (die Leiter steht erst ab Zeile 1138). (6) `<cstdlib>` in `PortMapper.cpp` wird nicht benutzt (kein `strto*`/`abs`).
- **Code-Auszug**:
```cpp
void CollabSession::broadcastLock(std::uint64_t subject, bool acquired,
                                  ConnectionId /*except*/) {
...
retireParticipant(id, conn, conn);
```
- **Lösung**: Parameter und Deklarationen entfernen; `onPeerConnected` samt Registrierung löschen (der Kommentar wandert an die `onConnect`-Zeile); Überschrift verschieben/löschen; unbenutzten Include entfernen.
- **Erwarteter Effekt**: Weniger Rauschen; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Minimal; Compiler findet jeden übersehenen Aufruf. Prüfen, dass kein Test `sendSnapshotTo` referenziert (Suche in `tests/` ergab keinen Treffer).

---

### NET-121 – Doppelte Hilfsfunktionen und -typen quer durch HE_Net
- **ID**: NET-121
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Net/src/RelayFanout.cpp:28-49` und `src/HE_Net/src/PortMapper.cpp:527-540` (`putU16/putU32/getU16/getU32`); `src/HE_Net/src/HttpClient.cpp:73-77` und `src/HE_Net/src/PortMapper.cpp:41-45` (`toLower`); `src/HE_Net/include/Net/TeamClient.h:68-73` und `src/HE_Net/include/Net/CollabSession.h:179-186` (`TeamColor` vs. `ParticipantColor`)
- **Problem**: Big-Endian-Lese/Schreibhelfer, `toLower` und `trim` existieren mehrfach in anonymen Namespaces; `TeamColor` und `ParticipantColor` sind bis auf den Namen identisch (der Kommentar im `TeamClient.h` sagt selbst „one converts into the other field by field"). Außerdem existieren in `RelayTransport.cpp`/`TeamClient.cpp`/`ScClient.cpp` laut Header-Beschreibung ebenfalls eigene Frame-Kodierer (`putStr8`, `readU64`, …); diese Dateien liegen nicht in diesem Slice und sind nicht geprüft.
- **Code-Auszug**:
```cpp
void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {          // RelayFanout.cpp
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}
void putU32(std::vector<std::uint8_t>& v, std::uint32_t x) {            // PortMapper.cpp
    for (int i = 3; i >= 0; --i) v.push_back(static_cast<std::uint8_t>((x >> (i * 8)) & 0xFF));
}
```
- **Lösung**: Interner Header `src/NetBytes.h` (neben `NetLog.h`) mit `putU16/32/64`, `getU16/32/64`, `toLower`, `trim`; `TeamColor` auf `using TeamColor = ParticipantColor;` oder beide auf eine gemeinsame `Rgb8` in `NetCommon.h` umstellen (`ParticipantColor::unset()` und `TeamColor::unset()` sind identisch).
- **Erwarteter Effekt**: Weniger Code, ein Ort für Endian-Fehler; keine Laufzeitwirkung.
- **Risiko / Verifikation**: Gering; `operator==` von `TeamColor` bleibt nötig (`ParticipantColor` hat keinen), also als Teil der gemeinsamen Struktur ergänzen. Alle Net-Tests ausführen.

---

### NET-122 – Kommentare, die Geschichte statt Absicht beschreiben, und veraltete Aussagen
- **ID**: NET-122
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Net/include/Net/CollabSession.h:44-128`; `src/HE_Net/src/LoopbackTransport.cpp:110-112` vs. `src/HE_Net/include/Net/LoopbackTransport.h:11-14`; `src/HE_Net/include/Net/HttpClient.h:7-12`; `src/HE_Net/include/Net/CollabSession.h:1170-1208`
- **Problem**: (1) 85 Zeilen Protokoll-Changelog (v2-v16) im Kopf von `CollabSession.h`, jeder Eintrag ein Absatz. Das ist Verlauf und gehört in `docs/` (Handoff-Doc zum Protokoll); der Header sollte nur den aktuellen Vertrag und die Bump-Regel nennen. (2) Widerspruch: `LoopbackTransport.h` sagt „Delivery is queued (not synchronous)", `LoopbackTransport.cpp:111` sagt „Loopback delivers eagerly on send()". Beides stimmt auf je eigene Weise (in die Peer-Queue eingereiht, aber sofort), die Wortwahl verwirrt. (3) Der veraltete TLS-Hinweis in `HttpClient.h` (siehe NET-111). (4) Die ~35 `std::function`-Member sind in drei Gruppen verteilt (Zeilen 1170-1208) und nicht in einem `Callbacks`-Aggregat zusammengefasst; das macht den Header in jeder Editor-TU groß (`<functional>`-Instanziierungen).
- **Code-Auszug**:
```cpp
// v13: a refusal travels BACK to whoever sent the file. The two ceilings are set
// on two machines by two people, and until now the lower one won in silence: ...
```
- **Lösung**: Changelog nach `docs/collab-protocol-history.md` verschieben, Header auf „Version = Gleichheitsprüfung; jede Änderung der Rahmen bumpt" kürzen. Callback-Member in `struct Callbacks { … } m_cb;` bündeln. Loopback-Kommentare angleichen.
- **Erwarteter Effekt**: Lesbarkeit; leicht kleinere Header-Parsingzeit in Editor-TUs.
- **Risiko / Verifikation**: Keines (nur Kommentare), bei der `Callbacks`-Bündelung reine Umbenennung.

---

### NET-123 – Test-Doubles in der Produktionsbibliothek: Mutex im Single-Thread-Loopback, O(n²)-Erase im Lossy
- **ID**: NET-123
- **Kategorie**: B
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/LoopbackTransport.cpp:97-169`; `src/HE_Net/include/Net/LoopbackTransport.h:64`; `src/HE_Net/src/LossyTransport.cpp:392-413`
- **Problem**: `ITransport.h` legt Single-Thread-Polling als Vertrag fest („single-threaded, deterministic drain"), `LoopbackTransport` sperrt dennoch pro `send/poll/connectionCount` einen `std::mutex` (Play-in-Editor nutzt Loopback laut Header produktiv). Der Mutex schützt `m_inbound` gegen einen Thread, den es laut Vertrag nicht gibt; ob doch einer existiert (z. B. Editor-Host im Hintergrund-Thread), ist aus dem Slice nicht ersichtlich. `LossyTransport::update` löscht fällige Einträge einzeln per `m_queue.erase(it)` in einer Schleife (O(n²) bei vielen Wartenden) und kopiert sie dann in einen Zwischenvektor; im Testbetrieb irrelevant. Zusätzlich: `LoopbackTransport::send` kopiert Daten pro Nachricht in ein neues `NetEvent`.
- **Code-Auszug**:
```cpp
for (auto it = m_queue.begin(); it != m_queue.end();) {
    if (it->dueMs <= m_nowMs) { due.push_back(std::move(*it)); it = m_queue.erase(it); }
    else ++it;
}
```
- **Lösung**: Erst klären, ob Loopback aus mehreren Threads genutzt wird (Aufrufer suchen: `grep LoopbackTransport src/`); falls nein, Mutex entfernen oder per Kommentar festschreiben, dass er bewusst für Tests mit Worker-Threads bleibt. `LossyTransport::update` mit `std::stable_partition` oder `remove_if`+Move statt Einzel-`erase`.
- **Erwarteter Effekt**: Vernachlässigbar im Test; relevant nur, falls Loopback im Editor tatsächlich pro Frame hohe Nachrichtenraten sieht.
- **Risiko / Verifikation**: Gering; falls der Mutex doch eine Datenrasse verhindert, würde er TSAN-Läufe der Net-Tests brechen. Unsicher / vor Änderung klären.

---

## Gelesen

Ja, alle 33 Dateien des Slice vollständig gelesen (`CollabSession.cpp` in drei Blöcken, `PortMapper.cpp` in zwei Blöcken, alle übrigen am Stück); nicht gelesen wurden Dateien außerhalb des Slice (`TcpTransport.cpp`, `SecureTransport.cpp`, `UdpTransport.cpp` wurden nur an den genannten Stellen per `grep`/`sed` gegengeprüft).

## Net Slice 2 – RelayTransport, RouterProbe, ScClient, SecureTransport, SessionDirectory, Socket, TcpTransport, TeamClient, UdpTransport

## Zusammenfassung

Der Code ist insgesamt sauber, gut kommentiert und fehlerrobust. Die Probleme liegen in der Datenpfad-Mechanik und in Duplikation, nicht in der Protokolllogik. Die gravierendsten Funde betreffen den Transport-Hot-Path. UDP löst pro Datagramm Adressen über Strings und `getaddrinfo` auf. TCP verwaltet den Ausgangspuffer quadratisch (exaktes `reserve` plus `erase` am Vektoranfang). Außerdem gibt es mehrere vermeidbare Kopien/Allokationen pro Frame und ein O(n²)-`poll()` in `SecureTransport`. Auf der Vereinfachungsseite sind Wire-Helfer, der Request/Reply-Client-Zustandsautomat (ScClient/TeamClient) und die Plattform-`#ifdef`-Blöcke in `Socket.cpp` mehrfach kopiert. Funde: 2 Hoch, 7 Mittel, 9 Niedrig. Alle Performance-Aussagen sind Schätzungen ohne Messdaten (siehe "Unsicher / Messung nötig").

---

### NET-201 – UDP: Adress-Strings, `getaddrinfo` und Key-Allokationen pro Datagramm
- **ID**: NET-201
- **Kategorie**: D (auch C)
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/UdpTransport.cpp:71-80, 289-312, 456-480, 1007`; `src/HE_Net/src/Socket.cpp:751-764, 1116-1172, 1174-1210`
- **Problem**: Peers werden nur über `host`-Strings identifiziert. Beim Senden ruft `rawSend` → `socketSendTo` → `parseNumericAddress` pro Datagramm `getaddrinfo(AI_NUMERICHOST)` samt `std::to_string(port)` auf (Heap, libc-Parser, `freeaddrinfo`). Beim Empfangen formatiert `socketRecvFrom` jede Absenderadresse per `inet_ntop` in einen frischen `std::string` (IPv6 > 15 Zeichen: Heap). Danach baut `findPeer` über `stripScope` (Kopie) und `addressKey` (String-Konkatenation plus `to_string`) einen weiteren temporären Key für die `unordered_map<std::string,…>`. `dropPeer` wiederholt das. Im Loopback-Testmodus kommt pro Sendung außerdem ein `getsockname`-Syscall (`socketBoundPort`) dazu.
- **Code-Auszug**:
```cpp
// UdpTransport.cpp:300 (pro Datagramm)
const SocketResult rc = socketSendTo(m_socket, datagram, len, host, port, sent);
// Socket.cpp:758 (pro Aufruf)
if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
// UdpTransport.cpp:71 / 478 (pro empfangenem Datagramm)
return host + "|" + std::to_string(port);
const auto it = m_byAddress.find(addressKey(stripScope(host), port));
```
- **Lösung**: (1) Im `Peer` das aufgelöste `sockaddr_storage` plus Länge einmalig beim Handshake speichern. Neue Socket-API `socketSendToAddr(h, data, len, const sockaddr_storage&, socklen_t, sent)`; der String-`socketSendTo` bleibt für LanBeacon/PortMapper und delegiert dorthin. (2) `socketRecvFromRaw(..., sockaddr_storage& from)` liefern; als Peer-Key eine kleine POD-Struktur `{family, scope-freie 16 Byte, port}` mit eigenem Hash verwenden. Strings nur noch für Logs/`PeerStats` erzeugen. Der Scope (`%en0`) wird heute für den Key ohnehin entfernt, der Struct-Key lässt ihn einfach weg. (3) `dropPeer` nutzt den im Peer gespeicherten Key.
- **Erwarteter Effekt** (Schätzung): pro Datagramm entfallen ca. 3-6 Heap-Allokationen und ein libc-Parserlauf, grob 1-3 µs je Richtung. Bei 16-32 Peers und Snapshot-Raten von 20-60 Hz sind das einige tausend Datagramme/s, also im Bereich von einigen Prozent eines Kerns. Nur durch Messung bestätigbar.
- **Risiko / Verifikation**: Gleiche Peer-Zuordnung wie heute muss erhalten bleiben (v4-mapped-IPv6 `::ffff:a.b.c.d` vs. IPv4, Link-Local-Scope). Dafür bestehende UDP-Tests (`tests/test_net_*`) ausführen und je einen Test für den Key-Vergleich (v4, v6, v4-mapped, Scope) ergänzen. Windows/macOS-Pfad: nur `sockaddr_storage`-Nutzung, keine neuen Plattformabhängigkeiten.

---

### NET-202 – TcpTransport: Ausgangspuffer wächst per exaktem `reserve` und wird per `erase(begin)` verkleinert
- **ID**: NET-202
- **Kategorie**: C / D
- **Schweregrad**: Hoch
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/TcpTransport.cpp:282-310, 346-352` (Wirkung auf `ScClient.cpp:301-349`, `RelayTransport.cpp:541-550`)
- **Problem**: (a) `send()` ruft `c.outBuf.reserve(c.outBuf.size() + header + len)` auf. In libstdc++/libc++ ist `reserve(n)` exakt und umgeht das geometrische Wachstum. Jede weitere Sendung, deren Kapazität nicht reicht, reallokiert und kopiert den gesamten wartenden Puffer. Mehrere große Frames hintereinander (ScClient sendet bis zu 1-MiB-Frames in einer Schleife, Szenen-Snapshots laut `NetLog.h` bis ~200 MB) sind damit quadratisch. (b) `flushOutput` löscht gesendete Bytes mit `outBuf.erase(begin, begin+sentTotal)`. Bei Backpressure wird bei jedem Tick der komplette Rest (bis zu hunderte MB) nach vorne verschoben, obwohl pro Tick nur Kernel-Puffergröße (einige 100 KiB) abfließt. Das sind pro Tick Speicherbewegungen in der Größenordnung des restlichen Puffers.
- **Code-Auszug**:
```cpp
c.outBuf.reserve(c.outBuf.size() + kFrameHeaderSize + len);   // 346
...
c.outBuf.erase(c.outBuf.begin(),
               c.outBuf.begin() + static_cast<std::ptrdiff_t>(sentTotal));   // 307
```
- **Lösung**: `reserve` entfernen (der anschließende `insert` wächst geometrisch von selbst). Statt `erase` am Anfang einen Lesezeiger `outHead` im `Conn` führen: `socketSend` ab `outBuf.data()+outHead`; erst wenn `outHead == outBuf.size()` den Puffer `clear()`en, oder wenn `outHead` > Hälfte der Größe kompaktieren. Alternativ `std::deque<std::vector<uint8_t>>` mit einem Eintrag pro Frame (zusätzlich eliminiert das den Kopierschritt beim Anhängen, wenn `send` den Vektor übernimmt, siehe NET-204). Gleiches Muster für `inBuf` (`extractFrames`, Z. 276-279) mit Lesekopf statt `erase`.
- **Erwarteter Effekt** (Schätzung): Bei Bulk-Transfers (Asset-Upload, Snapshot) von O(N²) auf O(N) Speicherbewegungen. Bei einem 100-MB-Upload sind das mehrere GB eingesparter memmove-Verkehr und damit Frame-Stalls im Editor-Thread. Für kleine Nachrichten (< 1 KB) praktisch kein Unterschied.
- **Risiko / Verifikation**: Reihenfolge und Frame-Grenzen müssen identisch bleiben. Vorhandene TCP-/Relay-/ScClient-Tests laufen lassen. Neuer Test: 64 Frames à 1 MiB gegen einen absichtlich nicht lesenden Gegner senden, dann entleeren; Inhalt prüfen. Messen mit 50-200 MB `put` über Loopback.

---

### NET-203 – SecureTransport: Ereigniswarteschlange als `std::vector` mit `erase(begin())`
- **ID**: NET-203
- **Kategorie**: D
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/SecureTransport.cpp:625-634`; `src/HE_Net/include/Net/SecureTransport.h:196`
- **Problem**: `m_events` ist ein `std::vector<NetEvent>`. `poll()` entnimmt das vorderste Element mit `erase(begin())`, was alle restlichen Events verschiebt. Das Leeren einer Queue mit n Events kostet O(n²) Bewegungen. Alle anderen Transporte (`Tcp`, `Udp`, `Relay`) nutzen `std::deque`.
- **Code-Auszug**:
```cpp
out = std::move(m_events.front());
m_events.erase(m_events.begin());
return true;
```
- **Lösung**: `m_events` auf `std::deque<NetEvent>` und `pop_front()` umstellen (eine Zeile im Header plus eine im .cpp).
- **Erwarteter Effekt** (Schätzung): Bei normalen Tick-Mengen (< 10 Events) kaum messbar. Bei Rückstau (Editor-Hitch, Reconnect-Burst, Bulk-Download in Hunderte von Frames) vermeidet es quadratisches Verhalten.
- **Risiko / Verifikation**: Keines (gleiche Semantik). Bestehende SecureTransport-Tests laufen lassen.

---

### NET-204 – Mehrfachkopien jedes gesendeten Frames (Encrypt → Frame → outBuf) und vorab gebaute Upload-Frames
- **ID**: NET-204
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/SecureTransport.cpp:604-622`; `src/HE_Net/src/RelayTransport.cpp:544-548`; `src/HE_Net/src/ScClient.cpp:309-348`; `src/HE_Net/src/TcpTransport.cpp:346-348`
- **Problem**: Ein Frame durchläuft auf dem Weg ins Netz mehrere Heap-Kopien. `SecureTransport::send` verschlüsselt in einen neuen Vektor `cipher`, kopiert diesen mit dem Zähler-Präfix in einen weiteren Vektor `out` und `TcpTransport::send` kopiert ihn erneut in `outBuf`. `RelayTransport::sendBroadcast` baut ebenfalls einen Zwischen-`frame` (Header + Payload) vor der TCP-Kopie. `ScClient::request` erzeugt für einen `put` alle Daten-Frames vorab in `frames` (kompletter Blob nochmals im Speicher), bevor sie Frame für Frame in `outBuf` kopiert werden.
- **Code-Auszug**:
```cpp
std::vector<std::uint8_t> cipher;
if (!Hpak::aesGcmEncrypt(p.sessionKey, nonce, data, len, cipher)) { ... }
std::vector<std::uint8_t> out;
out.reserve(kCounterLen + cipher.size());
out.resize(kCounterLen);
writeU64BE(out.data(), counter);
out.insert(out.end(), cipher.begin(), cipher.end());
```
- **Lösung**: (1) `Hpak::aesGcmEncrypt` um eine Variante erweitern, die an einen bestehenden Vektor anhängt (z. B. `aesGcmEncryptAppend(..., std::vector<uint8_t>& out)`, der Zähler-Präfix liegt dann schon in `out`). Das spart `cipher` komplett. (2) `ITransport`/`TcpTransport` um `sendOwned(conn, std::vector<uint8_t>&&)` oder eine Zwei-Teile-Sendung (`header`, `payload`) erweitern, damit Frames mit Header nicht erst zusammenkopiert werden. (3) `ScClient::request` Daten-Frames lazy beim Senden bauen (pro Update je ein Chunk, solange `outBuf` unter einer Schwelle liegt), statt alles vorab; das braucht eine `queuedBytes(conn)`-Abfrage am Transport. Reihenfolge (3), dann (1), (2).
- **Erwarteter Effekt** (Schätzung): Je Frame 1-2 Allokationen und Kopien weniger. Für kleine Nachrichten gering, für Blob-Uploads halbiert es den Spitzen-Speicherbedarf (Blob + Frames + outBuf).
- **Risiko / Verifikation**: Wire-Format unverändert. Bei (1) muss der AEAD-Ausgabeaufbau (Ciphertext || Tag) identisch bleiben; Known-Answer-Test in `tests/` für Hpak-AES-GCM und SecureTransport-Roundtrip vorhanden bzw. ergänzen. (3) ändert Timeout-Verhalten (`allowanceMs`, `m_lastActivityMs`) nicht, aber testen.

---

### NET-205 – UdpTransport: Heap-Allokationen pro Datagramm und pro `update()`
- **ID**: NET-205
- **Kategorie**: C
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/UdpTransport.cpp:330-347, 358-368, 381-383, 262-267`
- **Problem**: `emitDatagram` und `sendControl` bauen jedes Datagramm in einen neuen `std::vector` (max. `mtuPayload`+Header ≈ 1,2 KB, also eine kleine, bekannte Obergrenze). `update()` erzeugt bei jedem Aufruf einen `std::vector<ConnectionId> ids` für alle Peers. `makeCookie` allokiert pro Connect/Cookie-Check einen Vektor für die HMAC-Nachricht (zweimal in `cookieValid`). Außerdem führt `emitDatagram` pro Datagramm 32 `recvSeq.seen()`-Aufrufe aus, um `ackBits` zu bilden.
- **Code-Auszug**:
```cpp
std::vector<std::uint8_t> out;
out.reserve(kHeaderSize + kMsgSeqSize + kFragmentHeaderSize + len);
...
std::vector<ConnectionId> ids;
ids.reserve(m_peers.size());
for (const auto& [id, p] : m_peers) ids.push_back(id);
```
- **Lösung**: Datagramm in einen Stack-Puffer `std::array<uint8_t, kMaxDatagram>` (Größe aus `mtuPayload`-Maximum plus Header; `Config::mtuPayload` hart auf diese Grenze klemmen oder bei Überschreitung auf einen `thread_local`-Scratch zurückfallen) schreiben. `ids` als wiederverwendete Member-Scratch (`m_scratchIds.clear()`). `makeCookie` mit Stack-Array (Host ≤ 45 Zeichen + 2 + 8). `ackBits` bei jeder neuen Sequenz im `SeqWindow` inkrementell halten (Bitfeld statt 32 `seen`-Aufrufe), wenn Messung das nötig zeigt.
- **Erwarteter Effekt** (Schätzung): 1-2 Allokationen pro Datagramm weniger; zusammen mit NET-201 spürbar bei vielen Peers. Allein gering.
- **Risiko / Verifikation**: Wire-Bytes müssen identisch bleiben, die bestehenden UDP-Reliability-Tests (`setTestDropFn`/`setTestDuplicateFn`) decken das ab. Beim Stack-Puffer auf Überlauf bei `len > mtuPayload` achten (`send` prüft bereits, `sendReliable` fragmentiert auf `mtuPayload`).

---

### NET-206 – Blockierendes DNS (`getaddrinfo`) im Aufrufer-Thread bei `TcpTransport::connect` und Relay-Reconnect
- **ID**: NET-206
- **Kategorie**: E
- **Schweregrad**: Mittel
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/Socket.cpp:358-381` (und `322-340`); `src/HE_Net/src/TcpTransport.cpp:75-86`; `src/HE_Net/src/RelayTransport.cpp:279-293, 297-311`
- **Problem**: Der Kommentar in `socketCreateTcp` sagt "nothing in the engine may stall on a socket". Der Verbindungsaufbau macht aber ein synchrones `getaddrinfo` mit Hostnamen. Bei einem nicht erreichbaren Resolver kann das Sekunden bis zu 30 s blockieren. `RelayTransport::update()` ruft bei getrenntem Listener `openLink` → `TcpTransport::connect` im Update-Takt auf (mit Backoff 1 s … 30 s), jeder Versuch kann den Aufrufer-Thread blockieren. Die Aufrufer in `CollabController.cpp:211, 943, 950` laufen synchron (Thread nicht verifiziert; Directory-Aufrufe laufen dort dagegen korrekt per `std::async`).
- **Code-Auszug**:
```cpp
addrinfo* res = nullptr;
if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
    return SocketResult::Error;
}
```
- **Lösung**: Namensauflösung in einen Worker auslagern (`std::async`/eigener Resolver-Thread; `TcpTransport::connect` liefert die Verbindung im Zustand `connecting`, das Socket wird erst nach Auflösung erzeugt). Minimalvariante: Relay-/Host-Auflösung nur beim ersten Verbindungsaufbau und bei Retry in einem Hintergrund-Task, Ergebnis per `std::future` im `update()` abholen. Numerische Adressen (`AI_NUMERICHOST`-Vortest) direkt synchron weiter behandeln.
- **Erwarteter Effekt** (Schätzung): Beseitigt mögliche UI-Hänger (Sekunden) bei Netzproblemen. Im Normalfall (Cache-Treffer) kein Unterschied.
- **Risiko / Verifikation**: Zustandsmaschine in `TcpTransport` (`connecting`) und Fehlerpfade (`Disconnected`-Event bei Fehlschlag) müssen gleich bleiben. Tests mit unauflösbarem Hostnamen (`.invalid`) und Zeitmessung des `update()`-Aufrufs ergänzen. Auf macOS Local-Network-Permission-Verhalten achten (siehe `RouterProbe`).

---

### NET-207 – `select()` mit `FD_SET` statt `poll`: Überlauf bei Deskriptoren ≥ `FD_SETSIZE`
- **ID**: NET-207
- **Kategorie**: F (Robustheit)
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Net/src/Socket.cpp:581-628, 662-681`
- **Problem**: `socketConnectPoll` (pro Tick und ausstehender Verbindung) und `socketWaitReadable` verwenden `fd_set`. Auf Linux/macOS ist `FD_SET` für `fd >= FD_SETSIZE` (1024) undefiniertes Verhalten (Stack-Überschreibung, mit `_FORTIFY_SOURCE` Abbruch). Ein Editor mit vielen offenen Dateien/Texturen/Mmaps kann diese Schwelle erreichen, und die Sockets werden dann zufällig hohe Nummern bekommen.
- **Code-Auszug**:
```cpp
fd_set writeSet, errSet;
FD_ZERO(&writeSet);
...
FD_SET(s, &writeSet);
FD_SET(s, &errSet);
const int nfds = s + 1;
```
- **Lösung**: POSIX: `poll()` mit `pollfd{fd, POLLOUT}` (Fehler über `SO_ERROR`/`POLLERR`). Windows: `WSAPoll`. Die Aufrufstelle bleibt gleich, die Funktion wird kürzer (kein `FD_ZERO`/`errSet`).
- **Erwarteter Effekt**: Keine Performance-Änderung; beseitigt latenten Absturz/UB und verkürzt den Code.
- **Risiko / Verifikation**: Semantik "zero timeout = pure poll" beibehalten. `WSAPoll` meldet fehlgeschlagene Non-Blocking-Connects auf älteren Windows-Versionen abweichend (Bug in Pre-Windows-10-Builds); auf Windows ggf. beim bestehenden `select` bleiben und nur den POSIX-Zweig umstellen. `tests/test_net_discovery.cpp` (Connect-Tests) laufen lassen.

---

### NET-208 – `Socket.cpp`: 58 Zeilen mit `_WIN32`-Verzweigungen und kopierter Dual-Stack-Logik
- **ID**: NET-208
- **Kategorie**: A
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Net/src/Socket.cpp` (u. a. 175-195, 252-268, 432-547, 768-819, 873-947, 1025-1155, 1227-1251, 1376-1394)
- **Problem**: Fast jeder Systemaufruf steht doppelt (`#ifdef _WIN32 … static_cast<SOCKET>(h) … #else … static_cast<int>(h) …`), insgesamt 58 Zeilen mit `_WIN32`. Das Muster "IPV6_V6ONLY löschen und zurücklesen" ist zweimal fast wörtlich vorhanden (`socketCreateListenerDualStack` 463-480 und `socketCreateUdpDualStack` 884-897). In `socketCreateListenerDualStack` hängt der gesamte Bind-Zweig in einem eingerückt falschen `else { … }` (Z. 493-527 mit Leerzeile nach dem `else {`), was die Kontrollstruktur schwer lesbar macht.
- **Code-Auszug**:
```cpp
#ifdef _WIN32
    return ::bind(static_cast<SOCKET>(h),
                  reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
#else
    return ::bind(static_cast<int>(h),
                  reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
#endif
```
- **Lösung**: Oben in der Datei einmal `#ifdef _WIN32 using Native = SOCKET; using SockLen = int; using OptPtr = const char*; #else … #endif` plus `inline Native nat(SocketHandle h)` und ein `setOpt(h, level, name, const void*, len)`-Wrapper definieren. Danach entfallen alle Doppel-Blöcke von `bind`, `getsockname`, `connect`, `setsockopt`, `sendto`. Dual-Stack-V6ONLY-Prüfung als `static bool clearV6Only(SocketHandle)`; Bind-Zweig per frühem `if (!clearV6Only(h)) { warn; close; goto/lambda }` flach machen. Die plattformspezifischen Teile (Gateway/Interfaces) bleiben getrennt, werden aber ebenfalls zu je einer Funktion pro Plattform in einem `#if`-Block gebündelt.
- **Erwarteter Effekt** (Schätzung): ca. 250-350 Zeilen weniger; keine Laufzeitänderung.
- **Risiko / Verifikation**: Reines Refactoring. Alle drei Plattformen kompilieren (Windows-CI, macOS), `test_net_discovery`, `test_net_*` unverändert grün. Die Flags/Optionen (Windows ohne `SO_REUSEADDR`) dürfen dabei nicht angeglichen werden.

---

### NET-209 – Mehrfach kopierte Wire-Helfer und ein doppelter Request/Reply-Client (ScClient / TeamClient)
- **ID**: NET-209
- **Kategorie**: A / F
- **Schweregrad**: Mittel
- **Typ**: Vereinfachung
- **Ort**: `RelayTransport.cpp:20-53, 145-158`; `ScClient.cpp:14-77, 118-210`; `TeamClient.cpp:18-103, 139-260, 799-818`; `SecureTransport.cpp:45-52, 76-79`; `SessionDirectory.cpp:18-24`; `UdpTransport.cpp:30-69`; `TcpTransport.cpp:15-27`
- **Problem**: Dieselben Bausteine existieren mehrfach: `kMagic = "HERL"` und `putHeader` in drei Dateien (`Relay`, `ScClient`, `TeamClient`); `putStr8` in `Relay`, `ScClient`, `TeamClient` (als statische Member); `steadyMs()/steadyNowMs()/nowMs()` in sechs Dateien (plus ein weiterer Inline-Block in `SecureTransport.cpp:536` und `TcpTransport.cpp:157`); `fillRandom` in `SecureTransport`, `SessionDirectory`, `UdpTransport`; Big-Endian-put/get in `ScClient`, `TeamClient::Reader`, `UdpTransport`, `SecureTransport`, `TcpTransport`; `withReason` in `ScClient` und `TeamClient` mit leicht unterschiedlicher Semantik (ScClient entfernt den Schlusspunkt, TeamClient nicht); der HMAC→Hex(16 Byte)-Code in `RelayTransport::sessionIdForJoinCode` und `TeamClient::joinCodeForSession`. `ScClient` und `TeamClient` implementieren außerdem denselben Zustandsautomaten (Connecting→Hello/LoggingIn→Ready, Held-Frames, Antwort-Timeout, `fail()` mit `ConnectionLost` an alle Pending, Callbacks-zuletzt, Wipe des Login-Frames).
- **Code-Auszug**:
```cpp
// ScClient.cpp:14 und TeamClient.cpp:18 (identisch):
constexpr char kMagic[4] = { 'H', 'E', 'R', 'L' };
// RelayTransport.cpp:36, ScClient.cpp:52, TeamClient.cpp:814:
const std::size_t n = std::min<std::size_t>(s.size(), 255);
out.push_back(static_cast<std::uint8_t>(n));
```
- **Lösung**: Interne Header `src/HE_Net/src/NetWire.h` (nicht installiert) mit `ByteWriter`/`ByteReader` (put/get u8/u16/u32/u64, str8/str16, mit Fehlerzustand wie `TeamClient::Reader`), `putRelayHeader(type)`, `steadyMs()`, `fillRandom()`, `secureWipe()` (vgl. NET-215) und `hmacHex16(label, input)`. Für die beiden Clients eine kleine Basis `RelayRequestClient` (State, `m_pending`, `m_ready`, `fail`, Timeout) oder zumindest gemeinsame freie Hilfsfunktionen. Die Reihenfolge: zuerst die reinen Helfer (risikoarm), dann die Basisklasse.
- **Erwarteter Effekt** (Schätzung): ca. 250-400 Zeilen weniger, ein Ort für Wire-Fehler. Keine Laufzeitwirkung.
- **Risiko / Verifikation**: Wire-Bytes bleiben identisch; Tests vorhanden für Relay-/Team-/Sc-Protokoll (`tests/test_net_*`, `test_team*`). Beim Zusammenlegen von `withReason` Textausgabe prüfen (Tests/UI vergleichen Strings).

---

### NET-210 – TeamClient: Avatar-Pixel doppelt gehalten, `mergeProfiles` vergleicht und kopiert ganze Bilder
- **ID**: NET-210
- **Kategorie**: C / D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/TeamClient.cpp:603-617, 366-373, 400-415`
- **Problem**: Jedes Profil hält `avatarRgba` (bis `kMaxAvatarSize`², RGBA) und zusätzlich hält jedes `TeamMember` eine zweite Kopie. `mergeProfiles()` läuft bei jedem Profil-Event/jeder Listenantwort über alle Mitglieder, allokiert pro Mitglied einen gefoldeten Namen (`foldName`) und vergleicht mit `m.avatarRgba != p.avatarRgba` byteweise, bevor kopiert wird. Der Vergleich könnte über `avatarId`/`pixelsId` erfolgen.
- **Code-Auszug**:
```cpp
auto it = m_profiles.find(foldName(m.name));
...
if (m.avatarSize != p.avatarSize || m.avatarRgba != p.avatarRgba) {
    m.avatarSize = p.avatarSize;
    m.avatarRgba = p.avatarRgba;
}
```
- **Lösung**: Mit `Profile::pixelsId` vs. neu gehaltener `TeamMember::pixelsId` vergleichen (O(1)). Alternativ `std::shared_ptr<const std::vector<uint8_t>>` für Pixel, damit Mitglied und Profil teilen. Gefoldeten Namen einmal im `TeamMember` cachen.
- **Erwarteter Effekt** (Schätzung): Gering (Teams haben < 50 Mitglieder, Events sind selten), spart nur Speicher/Allokationen im UI-Pfad.
- **Risiko / Verifikation**: Verhalten "altes Bild bleibt bis das neue ankommt" muss erhalten bleiben. Team-Tests/UI-Test des Team-Panels prüfen.

---

### NET-211 – TcpTransport-Leseweg: 16-KiB-Chunk, Zwischenkopie in `inBuf`, zweite Kopie in `ev.data`
- **ID**: NET-211
- **Kategorie**: C
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/TcpTransport.cpp:203-213, 242-280`
- **Problem**: Daten werden in einen 16-KiB-Stackpuffer gelesen, an `inBuf` angehängt und beim Framing erneut in `ev.data` kopiert. Bei großen Frames (Asset-Blobs bis 1 MiB, Snapshots) bedeutet das 64 Syscalls pro MiB statt weniger, plus eine zusätzliche Kopie. Die Rückgabe von `extractFrames` löscht danach den Anfang von `inBuf` (siehe NET-202).
- **Code-Auszug**:
```cpp
std::uint8_t chunk[kRecvChunk];   // 16 KiB
...
c.inBuf.insert(c.inBuf.end(), chunk, chunk + got);
...
ev.data.assign(payload, payload + len);
```
- **Lösung**: Direkt in den Schwanz von `inBuf` lesen (`resize(old + 64K)`, `socketRecv` in `inBuf.data()+old`, danach auf `got` zurückschneiden) und Chunk auf 64 KiB anheben. Bei bekanntem Frame-Header (Länge bekannt) `inBuf` auf Framegröße vorreservieren, um Realloc-Kopien zu vermeiden.
- **Erwarteter Effekt** (Schätzung): Ein Memcpy pro Byte weniger (memcpy-Tempo, im Vergleich zum Netz selten limitierend); weniger Syscalls bei Bulk. Gering, aber billig.
- **Risiko / Verifikation**: Partielle Frames und Verbindungsabbruch mit Rest in `inBuf` ("discarding … of a partially received frame") müssen gleich bleiben. TCP-Framing-Tests wiederholen.

---

### NET-212 – Handgebaute Log-Drosselung statt `HE_LOG_THROTTLE`
- **ID**: NET-212
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Net/src/TcpTransport.cpp:156-165`; `src/HE_Net/src/SecureTransport.cpp:533-556`
- **Problem**: `acceptPending` hält `static std::uint64_t s_lastAcceptErrLog` (global über alle Instanzen, nicht atomar) und rechnet `steady_clock` selbst aus. `Diagnostics/Log.h:290` bietet `HE_LOG_THROTTLE(cat, level, seconds, ...)` genau dafür (atomar). In `SecureTransport::replayWindowAdmits` ist die Drosselung ("zehnter Drop pro Minute") bewusst anders und bleibt sinnvoll, aber sie ruft pro verworfenem Frame `steady_clock::now()` zusätzlich zu `m_clock`-losem Code auf.
- **Code-Auszug**:
```cpp
static std::uint64_t s_lastAcceptErrLog = 0;
const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(...
if (nowMs - s_lastAcceptErrLog > 5'000) {
    s_lastAcceptErrLog = nowMs;
    HE_LOG_WARN(Net, "TCP accept failed on the listening socket — %s", socketErrorText().c_str());
}
```
- **Lösung**: Durch `HE_LOG_THROTTLE(Net, Warning, 5.0, "TCP accept failed on the listening socket — %s", socketErrorText().c_str());` ersetzen. `<chrono>`-Include in `TcpTransport.cpp` entfällt dann.
- **Erwarteter Effekt**: -8 Zeilen, Thread-Sicherheit. Keine Laufzeitwirkung.
- **Risiko / Verifikation**: Gering. Drosselung ist jetzt pro Aufrufstelle (bisher auch global); Log-Inhalt gleich.

---

### NET-213 – UdpTransport: doppelte Reject-Behandlung, mehrfache Peer-Lookups, Testhooks im Sendepfad
- **ID**: NET-213
- **Kategorie**: A
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Net/src/UdpTransport.cpp:503-560, 289-299, 232-245`
- **Problem**: (a) Die Reject-Behandlung steht in `Connecting` (528-533) und `Cookie` (547-552) wortgleich; ein früher Check vor dem `switch` genügt. (b) `handleDatagram` sucht den Peer: `findPeer` (hash), `m_peers.at(id)` (hash), dann nach `handleEstablished` nochmals `m_peers.find(id)` und `m_peers.at(id)`: vier Hash-Lookups pro empfangenem Datagramm. (c) `rawSend` prüft pro Datagramm zwei `std::function`-Testhooks (`m_testDrop`, `m_testDuplicate`), die nur in Tests gesetzt werden. (d) `m_clock` ist ein `std::function` und wird pro Datagramm 4-6-mal aufgerufen (`emitDatagram`, `transmitPacket`, `recordSent`, `handleEstablished`, `update`), obwohl ein `now` pro `update()`/`send()` genügt.
- **Code-Auszug**:
```cpp
if (m_peers.find(id) != m_peers.end()) processAcks(m_peers.at(id), ack, ackBits);
```
- **Lösung**: `Peer*` aus `findPeer` direkt liefern und weiterreichen; `handleEstablished` meldet per Rückgabewert, ob der Peer verworfen wurde. Reject-Fall vor dem `switch`. `now` einmal pro `receiveAll()`/`update()`/`send()` lesen und als Member `m_now` führen (Tests setzen `setClock`, das bleibt der einzige Lesepunkt). Testhooks hinter einem einzigen `if (m_hasTestHooks)`.
- **Erwarteter Effekt** (Schätzung): Sehr gering pro Datagramm (zig ns), dafür kürzerer Code. Nur zusammen mit NET-201/205 sinnvoll.
- **Risiko / Verifikation**: Die Zeitsemantik für Tests (`setClock`) muss gleich bleiben; UDP-Reliability-Tests mit deterministischem Clock laufen lassen.

---

### NET-214 – RelayTransport: lineare Suche nach Verbindungen in jedem `send`
- **ID**: NET-214
- **Kategorie**: D
- **Schweregrad**: Niedrig
- **Typ**: Performance
- **Ort**: `src/HE_Net/src/RelayTransport.cpp:588-604, 531-537, 331`
- **Problem**: `send()` durchsucht `m_links` linear nach `outer == conn`. Der Host sendet über die Standard-Schleife `ITransport::send` an jeden Peer einzeln, das ergibt O(N²) je Broadcast (N = Peers). Für kleine N irrelevant, aber ein `unordered_map<ConnectionId, Link*>` ist trivial. `serviceLink` kopiert außerdem pro Connected-Event über den Ternary `cond ? std::string() : l.peer` den Peer-String (Mischung aus Prvalue und Lvalue ergibt Kopie).
- **Code-Auszug**:
```cpp
for (auto& l : m_links) {
    if (l->outer != conn || l->dead) continue;
```
- **Lösung**: Index `std::unordered_map<ConnectionId, Link*> m_byOuter` pflegen (beim Paaren eintragen, beim Entfernen löschen) oder N klein halten und nur dokumentieren. Den Ternary durch `const std::string& target = l.role == Role::Listen ? kEmpty : l.peer;` mit statischem leerem String ersetzen.
- **Erwarteter Effekt** (Schätzung): Vernachlässigbar bei < 10 Peers.
- **Risiko / Verifikation**: Gering; Zeiger müssen beim `m_links.erase` ungültig werden (Link liegt in `unique_ptr`, Adresse stabil). Relay-Tests.

---

### NET-215 – Geheimnis-Hygiene: `std::fill`-Wipe kann wegoptimiert werden, Schlüssel bleiben in Kopien
- **ID**: NET-215
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `ScClient.cpp:118-121, 145, 195`; `TeamClient.cpp:139-143, 167, 229, 783`; `SecureTransport.cpp:235, 242, 76-79`
- **Problem**: `SecureTransport.cpp` besitzt ein korrektes `secureWipe` (über `volatile`). `ScClient`/`TeamClient` löschen Ticket bzw. Passwort dagegen mit `std::fill`, was der Compiler (Dead-Store vor Freigabe/`clear()`) entfernen darf. Zusätzlich kopiert `SecureTransport::onInnerConnected` das lokale `Peer p` (mit `ephemeralPriv`) per `m_peers[conn] = p`, die Stack-Kopie wird nicht gelöscht; `Peer` löscht seinen `sessionKey` beim `erase` nicht.
- **Code-Auszug**:
```cpp
std::fill(m_helloFrame.begin(), m_helloFrame.end(), std::uint8_t(0));
...
m_peers[conn] = p;      // p.ephemeralPriv bleibt im Stack-Frame
```
- **Lösung**: `secureWipe` in `NetWire.h` (NET-209) auslagern und überall verwenden. `m_peers.emplace(conn, std::move(p))` bzw. direkt `Peer& p = m_peers[conn];` verwenden und `Peer` einen Destruktor geben, der `sessionKey`/`ephemeralPriv` löscht.
- **Erwarteter Effekt**: Kein Performance-Effekt; reduziert die Verweildauer von Schlüsselmaterial im Speicher.
- **Risiko / Verifikation**: Gering. Ein Test, der `SecureTransport` mehrfach auf-/abbaut, bleibt grün.

---

### NET-216 – Magic Numbers und Enum-Reihenfolge-Kopplung bei Status-/Result-Namen; `const_cast` in TeamClient
- **ID**: NET-216
- **Kategorie**: A / F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Net/src/TeamClient.cpp:844-869, 507, 521, 546`; `src/HE_Net/src/ScClient.cpp:462-471`
- **Problem**: `TeamClient::statusName` benennt Codes mit nackten Zahlen (`case 4`, `16`, `17`, `20` …), obwohl `describeStatus` direkt daneben die `StatusCode`-Enumeratoren verwendet. `resultName` indiziert ein Namensarray mit dem Enumwert; eine neue Enum-Konstante an der falschen Stelle verschiebt still alle Namen. `onEvent` ruft dreimal `const_cast<…>(findMember/findSession)` auf, weil nur Const-Varianten existieren.
- **Code-Auszug**:
```cpp
case 4:  return "logged-in";
case 16: return "bad-hello";
...
if (TeamMember* m = const_cast<TeamMember*>(findMember(ev.member))) {
```
- **Lösung**: `case std::uint8_t(StatusCode::LoggedIn):` usw. verwenden. Nach dem Array `static_assert(std::size(kNames) == std::size_t(Result::LastValue) + 1)` (ggf. Sentinel-Enumerator ergänzen). Nicht-const-Überladungen `findMember`/`findSession` (oder private Templates) anlegen, `const_cast` entfällt.
- **Erwarteter Effekt**: Lesbarkeit und Sicherheit gegen Enum-Drift.
- **Risiko / Verifikation**: Keines; Statusnamen-Tests (falls vorhanden) bleiben identisch.

---

### NET-217 – SessionDirectory: Session-ID unescaped in der URL, doppelte `get<string>()`
- **ID**: NET-217
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Net/src/SessionDirectory.cpp:213, 111-116, 26-35`
- **Problem**: `lookup` hängt die (vom Benutzer eingetippte) Session-ID ungeprüft an: `buildUrl(...) + "&sessionId=" + sessionId`. Enthält sie `&`, `#`, Leerzeichen, entsteht eine kaputte Anfrage bzw. ein eingeschleuster Parameter. Die Funktionsparameter sind sonst nirgends validiert (`RelayTransport::isValidId` existiert). `parseLookup` ruft `h.get<std::string>()` zweimal pro Element auf; `intField` verwendet Exceptions für nicht numerische Strings.
- **Code-Auszug**:
```cpp
const std::string url = buildUrl(m_endpoint, "lookup") + "&sessionId=" + sessionId;
...
if (h.is_string() && !h.get<std::string>().empty())
    out.hosts.push_back(h.get<std::string>());
```
- **Lösung**: Vor dem Request `RelayTransport::isValidId`-artig prüfen (oder Prozent-kodieren) und `DirectoryStatus::Rejected` zurückgeben. In `parseLookup` `const auto& s = h.get_ref<const std::string&>();`. `intField` kann `std::from_chars` statt `stoi`+`try/catch` nutzen.
- **Erwarteter Effekt**: Robustheit; Performance nicht relevant (Netzwerkaufruf).
- **Risiko / Verifikation**: Die Server-ID-Alphabet-Regeln (Join-IDs `g-…`, generierte 20 Zeichen) vorher prüfen, damit gültige IDs nicht abgelehnt werden. `tests/test_net_directory.cpp` ergänzen.

---

### NET-218 – Toter Code und doppelte Arbeit in `Socket.cpp`/`RouterProbe`
- **ID**: NET-218
- **Kategorie**: F
- **Schweregrad**: Niedrig
- **Typ**: Vereinfachung
- **Ort**: `src/HE_Net/src/Socket.cpp:322-356`; `src/HE_Net/src/Socket.cpp:1643-1664, 1790-1800` (Aufruf `RouterProbe.cpp:35-36`)
- **Problem**: `socketConnect` hat nach Suche in `src/` und `tests/` keinen Aufrufer (nur Deklaration in `Socket.h:142` und Kommentar). Sie ist zudem die einzige Stelle, die `AF_INET` hart verdrahtet und `getaddrinfo` blockierend aufruft. `probeRouter` ruft `socketLocalNetworkBlocked()` und kurz darauf `socketDefaultGateway()` auf; beide lesen die Routingtabelle (`/proc/net/route`, `sysctl`, `GetBestRoute`) neu über `defaultGatewayImpl()`.
- **Code-Auszug**:
```cpp
p.localNetworkBlocked = socketLocalNetworkBlocked();   // defaultGatewayImpl()
p.gatewayV4           = socketDefaultGateway();        // defaultGatewayImpl() erneut
```
- **Lösung**: `socketConnect` (und die Header-Deklaration) entfernen, sofern kein Außen-Modul sie nutzt (vorher `grep` über das ganze Repo, einschließlich `src/HE_ContentSync`, `src/HE_Python`). `socketLocalNetworkBlocked(const std::string& gateway)` als Überladung, `probeRouter` übergibt das bereits bestimmte Gateway.
- **Erwarteter Effekt**: ca. 35 Zeilen weniger; ein Routingtabellen-Lesevorgang weniger pro Probe (läuft einmal pro Editorstart im Thread).
- **Risiko / Verifikation**: Gering. Öffentliche API (`HE_NET_API`): Vor dem Löschen Abhängigkeiten außerhalb von `HE_Net` ausschließen.

---

## Gelesen

Alle neun Dateien des Slice (`RelayTransport.cpp`, `RouterProbe.cpp`, `ScClient.cpp`, `SecureTransport.cpp`, `SessionDirectory.cpp`, `Socket.cpp`, `TcpTransport.cpp`, `TeamClient.cpp`, `UdpTransport.cpp`) vollständig gelesen: ja. Zusätzlich gezielt gelesen: relevante Header-Ausschnitte (`UdpTransport.h`, `TcpTransport.h`, `SecureTransport.h`), `NetLog.h`, `Diagnostics/Log.h` (Makros) und `Hpak/Aes256Gcm.h`.
