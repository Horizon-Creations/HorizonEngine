# Net Slice 2 – RelayTransport, RouterProbe, ScClient, SecureTransport, SessionDirectory, Socket, TcpTransport, TeamClient, UdpTransport

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

## Cross-Modul-Hinweise

- `src/HE_Core/include/Hpak/Aes256Gcm.h:34-45`: `aesGcmEncrypt/Decrypt` liefern immer in einen frisch geleerten Ausgabevektor. Für NET-204 wäre eine "append"-Variante (oder Ausgabe in vorbelegten Puffer) im Core nötig.
- `src/HE_Editor/CollabController.cpp:211, 943, 950`: `RelayTransport::host/connect` und `TcpTransport::connect` laufen synchron (Thread nicht verifiziert), relevant für NET-206. Die Directory-Aufrufe (`:327, :830, :2450`) laufen korrekt off-thread.
- `src/HE_Net/src/LanBeacon.cpp:175-177, 322` und `src/HE_Net/src/PortMapper.cpp:264, 313, 585, 605` nutzen `socketSendTo`/`socketRecvFrom` mit Strings. Sie sind nicht heiß, müssen aber bei NET-201 mit der alten String-API kompatibel bleiben (nicht entfernen).
- `src/HE_ContentSync/src/SftpClient.cpp:64` ist der einzige Aufrufer von `skipCandidates`/`outCandidateCount`; deshalb bleiben diese Parameter von `socketCreateTcpConnecting` erhalten.
- `src/HE_Net/src/HttpClient.cpp:157` ruft ebenfalls `socketCreateTcpConnecting` (blocking DNS), betrifft NET-206 analog.

## Unsicher / Messung nötig

- NET-201: tatsächlicher Anteil von `getaddrinfo`/`inet_ntop`/String-Keys an der Frame-Zeit bei 16-32 Peers (Profiler: `rawSend`, `handleDatagram`).
- NET-202: Stall-Dauer pro Tick bei 50-200 MB `put`/Snapshot über Loopback und über echtes Netz; Kernel-Sendepuffer-Größe pro Plattform bestimmt, wie viel pro Tick abfließt.
- NET-205: Gewinn durch Stack-Puffer und inkrementelle `ackBits` nur nach NET-201 sinnvoll messbar.
- NET-206: ob die `CollabController`-Aufrufstellen im UI-Thread laufen; Dauer von `getaddrinfo` bei Offline-Betrieb je Plattform.
- NET-207: ob in der Praxis Sockets mit `fd >= 1024` entstehen (Editor mit vielen offenen Dateien); Windows-`WSAPoll`-Verhalten bei fehlgeschlagenem Non-Blocking-Connect.
- `UdpTransport::serviceReliability/ackOne/windowAdmits` (Z. 801-920) laufen O(`inFlight` ≤ 256) pro Paket/Ack; bei Bulk-Transfers zu vielen Peers evtl. messbar, nicht als Fund aufgenommen, weil die Zahlen klein sind.
- `ScClient`/`TeamClient`: Dateigrößenobergrenze eines `put` im Produktivbetrieb (bestimmt, wie relevant NET-204(3) ist).

## Gelesen

Alle neun Dateien des Slice (`RelayTransport.cpp`, `RouterProbe.cpp`, `ScClient.cpp`, `SecureTransport.cpp`, `SessionDirectory.cpp`, `Socket.cpp`, `TcpTransport.cpp`, `TeamClient.cpp`, `UdpTransport.cpp`) vollständig gelesen: ja. Zusätzlich gezielt gelesen: relevante Header-Ausschnitte (`UdpTransport.h`, `TcpTransport.h`, `SecureTransport.h`), `NetLog.h`, `Diagnostics/Log.h` (Makros) und `Hpak/Aes256Gcm.h`.
