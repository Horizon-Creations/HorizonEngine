# Gameplay-Replikation: UDP-Transport, Produktions-Verdrahtung, Property-Sync, RPC, Editor-Schalter

Plan, wie aus der fertigen, aber nirgends angeschlossenen Replikationsbibliothek
`GameReplication` ein benutzbares Multiplayer wird. Ziel, in den Worten der
Vorgabe: eine Entity zu teilen soll so einfach sein wie im Inspector unter
einer Kategorie **Replication** den Schalter **Replicates** anzustellen; dazu
einzelne Variablen synchronisieren mit Benachrichtigung (OnRep), und eine API
`CallServer` / `CallClient`, um Funktionen auf anderen Geräten auszulösen. Das
Ganze über UDP, für ein echtes Spiel, nicht nur für Editor-Kollaboration.

Wie `docs/anti-cheat-plan.md` ist das ein Entwurf: er kartiert den Ist-Zustand
mit Belegen, trifft die Entscheidungen, die man vor der ersten Zeile treffen
muss, und endet mit einer Roadmap, aus der die weiteren Schritte des Themas
abgeleitet werden. **Es ändert keinen Code.**

Stand 2026-09-22, Thema 77.

---

## Stand

Es gibt mehr, als die Themenbeschreibung annimmt, und weniger, als man von
außen glaubt.

**Mehr:** `NetworkComponent` ist inzwischen serialisiert und im Inspector
authorbar (Sektion „Network" mit Relevance Radius, Replicate Transform, Max
Speed), und der Anti-Cheat-Plan ist bis zu seinem Schritt 7 umgesetzt: Service,
Score, Integritäts-Manifest, Notice vor dem Kick, Telemetrie, `OnCheatDetected`
durch alle vier Frontends, `anticheat.*`-Registry. Die Themenbeschreibung
(„NetworkComponent hat kein replicates-Flag, ist nicht authorbar") ist in dem
Punkt überholt; das Flag fehlt weiterhin, die Authorbarkeit nicht.

**Weniger:** `GameReplication` wird von keiner Anwendung instanziiert.
`AntiCheatHost::attach(service, replication)` (`AntiCheatHost.h:115`) ruft
weder `GameApplication.cpp` noch `EditorApplication.cpp`; beide binden nur den
Host in den `Ctx`. Damit ist **auch das komplette Anti-Cheat in Produktion
inert**: es wartet auf genau die Verdrahtung, die dieser Plan beschreibt. Es
gibt keinen UDP-Transport, kein Spawn/Despawn über das Netz, keine
Property-Replikation, kein RPC, keine `net`-Gruppe in `HE::api`, keinen
Start-Pfad für Host oder Client im Spiel oder im Play-Modus.

Und ein Fund, der die Reihenfolge diktiert: `SecureTransport` beendet bei
jedem umsortierten Frame die **ganze Verbindung** (§1.4). Über TCP ist das nie
passiert, über UDP passiert es jede Sekunde. Der UDP-Transport ist deshalb
nicht ein Austausch der untersten Schicht, sondern eine Änderung an zwei.

---

## 1. Ausgangslage, belegt

### 1.1 Was `GameReplication` heute kann

`src/HE_Scene/include/HorizonScene/GameReplication.h` (338 Zeilen),
`src/HE_Scene/src/GameReplication.cpp` (781 Zeilen), getestet in
`tests/test_game_replication.cpp` über `LoopbackTransport::createPair()`.

| Fähigkeit | Wo | Anmerkung |
|---|---|---|
| Snapshot-Replikation der Entities mit `NetworkComponent` | `sendSnapshots`, `kMsgSnapshot = kFirstUserMessage + 200`, `SendMode::Unreliable` | nur `TransformComponent` Position + Rotation, quantisiert (24 Bit über ±`worldExtent`, 16 Bit Euler) |
| Tick-Rate | `Config::tickHz = 30` | Bandbreite linear |
| Interest-Management | `setViewpoint`, `relevanceRadius` | Kugel um den Viewpoint |
| Interpolation fremder Entities | `advanceInterpolation`, zwei Samples, `interpolationDelaySec` | |
| Prediction + Reconciliation der eigenen Entity | `pushInput`, `reconcile`, `applySmoothing`, `kMaxInputDeltaTime = 0.1` | Mover ist eine vom Spiel gestellte `MoveFn` |
| Client-Eingabe | `kMsgInput = +201`, `SendMode::Unreliable`, `InputCommand { sequence, deltaTime, move, yaw }` | **nur Bewegung**; Aktionen (Springen, Schießen) reisen nicht |
| Ownership | `assignControl(conn, netId)`, `m_controlledByConn` | Eingabe für fremde Entity ist `Hard`-Observation |
| Identität | `registerEntity` (Server prägt `netId`), `adoptEntity` (Client übernimmt) | **kein Spawn-Message**; der Test spiegelt Entities von Hand (`mirrorEntity`, Kommentar: „what the real spawn path would do via a spawn message") |
| Integrität | `kMsgIntegrity = +202`, `ReliableOrdered`, bis `kMaxManifestEntries = 256` Einträge à Kind + Name + Hash | ein einziges Frame; über UDP zu groß für ein Datagramm (§4.5) |
| Anti-Cheat-Notice | `kMsgAntiCheatNotice = +203`, `ReliableOrdered` | |
| Rollen | `NetRole::{None, Client, Server, Host}` in `NetCommon.h` | `Server` ist nur ein Enum-Wert; nichts unterscheidet ihn von `Host` |

### 1.2 Was fehlt

- **Kein Start-Pfad.** `grep -n "GameReplication\|NetSession" src/HE_Game/src/GameApplication.cpp` ist leer; `EditorApplication.cpp` kennt HorizonNet nur über `CollabController` und `RouterProbe`. Ein gebautes Spiel kann weder hosten noch beitreten.
- **Kein Spawn/Despawn.** Eine Entity, die ein Client sehen soll, existiert dort nur, wenn beide dieselbe Szene geladen haben und jemand `adoptEntity` mit der richtigen Id ruft. Für `Create Object` zur Laufzeit gibt es keinen Pfad.
- **Keine Property-Replikation.** Ein Snapshot trägt Position und Rotation, sonst nichts. `HorizonCode::Variable` (`HorizonCode.h:399`) hat `access`, `scope`, `typeName`, aber kein `replicated`.
- **Kein RPC.** `grep -rn "CallServer\|callServer\|CallClient" src/` ist leer. Der Anti-Cheat-Plan §6.2.2 hat eine `net`-Gruppe skizziert (`isHost`, `sendToHost`, `sendToClient`, `OnNetMessage`) und ausdrücklich als Voraussetzung für `anticheat.check` benannt; sie existiert nicht.
- **Keine `net`-Gruppe in `HE::api`.** `EngineApi.h` hat über 40 Namespaces von `entity` bis `input`, keinen `net`; `isScriptGroup` (`EngineApi.cpp:6933`) kennt keine solche Gruppe.
- **Kein Spieler-Begriff im Netz.** `PlayerHost` instanziiert genau einen Controller pro `PlayerController`-Klasse, lokal; das Spiel spawnt den Charakter aus dessen BeginPlay und ruft `player.possess`. Wer für einen **entfernten** Spieler auf dem Host Controller und Charakter erzeugt, ist nirgends definiert.
- **Kein Lebenszyklus-Event.** Es gibt kein `OnPlayerJoined`, `OnPlayerLeft`, `OnConnected`, `OnDisconnected` in der Event-Tabelle (`HorizonCode.cpp:2526` zeigt das Muster mit `OnCheatDetected`).
- **`LoopbackTransport` ist verlustfrei und geordnet** (`LoopbackTransport.h:11`). Nichts im Testbestand kann Verlust, Reorder oder Duplikate erzeugen; Prediction und Reconciliation sind nur auf einem perfekten Kanal getestet.

### 1.3 Transport-Realität

`src/HE_Net` kennt `LoopbackTransport`, `TcpTransport` (längenpräfixierte
Frames, `kMaxFrameSize = 64 MiB`) und den Decorator `SecureTransport`
(X25519 + HMAC-Handshake, AES-256-GCM pro Frame). `ITransport.h:8` nennt
`GnsTransport` (Valve GameNetworkingSockets) als geplantes Backing, und
`docs/networking-layer-design.md` reserviert GNS an drei Stellen für N4a
(Zeilen 41, 95, 159). Gebaut wurde es nie.

Was es an UDP gibt, ist Layer 0: `socketCreateUdp/Udp6`, `socketBindUdp*`,
`socketSendTo`, `socketRecvFrom`, `socketSetNonBlocking` (`Socket.h:139-215`),
benutzt von `PortMapper` (SSDP, NAT-PMP, PCP), `LanBeacon` und `RouterProbe`.
Datagramme senden und empfangen kann die Engine also; eine Verbindung,
Zuverlässigkeit, Fragmentierung, Keepalive hat sie darüber nicht.

### 1.4 Der Fund: `SecureTransport` verträgt keine Umsortierung

`SecureTransport.cpp:452-459`:

```cpp
const std::uint64_t counter = readU64BE(frame.data());
if (counter <= p.lastRecvCounter) {
    HE_LOG_WARN(Net, "Replay or reorder on conn %llu: counter %llu after %llu", …);
    failPeer(conn, p, "frame counter did not increase (replay or reorder)");
```

Die Regel „Counter streng steigend" ist über TCP korrekt und kostenlos, und
sie ist der Replay-Schutz. Über UDP kommen Frames vertauscht an, und dann ist
jedes vertauschte Paar **ein Verbindungsabbruch**. Der GCM-Invariant, den die
Regel schützt, lautet aber nicht „streng steigend", sondern **„kein Counter
wird zweimal akzeptiert"**. Das leistet auch ein Anti-Replay-Fenster (DTLS,
IPsec): höchster gesehener Counter plus 64-Bit-Bitmaske der zuletzt
akzeptierten. §4.6 legt fest, wie es in den Transport kommt.

Zweiter Punkt derselben Datei: der Handshake (Challenge, Response, Accept,
Reject) geht mit `SendMode::ReliableOrdered` durch den inneren Transport
(`SecureTransport.cpp:237, 355, 381, 387`). Ein UDP-Transport muss
zuverlässig-geordnete Zustellung also **selbst** leisten, **bevor** die
Verschlüsselung überhaupt beginnt. Reliable ist keine Option, sondern
Voraussetzung der Krypto.

### 1.5 Discovery und NAT sind auf TCP zugeschnitten

| Baustein | Was er heute tut | Was für UDP fehlt |
|---|---|---|
| `SessionDirectory` (`session-api.php`) | Host registriert `{sessionId, port}`, Server prüft `reachable` per **TCP-Connect-Back** (`SessionRegistration::reachable`) | ein UDP-Port ist so nicht prüfbar; `reachable` sagt für ein Spiel nichts |
| `PortMapper::mapPort(port, description, …)` | UPnP `NewProtocol = "TCP"` (`PortMapper.cpp:448, 494`), PCP `kPcpProtoTcp = 6` (`:779`) | **kein Protokoll-Parameter**; für UDP muss die Freigabe UDP heißen |
| `LanBeacon::Announcement` | `protocol = kCollabProtocolVersion` (= 13), `sessionId`, `port`, `hostName`, `projectLabel` | keine **Art**: ein Spiel-Lobby-Browser sähe Editor-Sitzungen und umgekehrt |
| `CollabController::startHosting / joinBySessionId / joinSession` | die ganze Kette `TcpTransport → SecureTransport → NetSession → CollabSession` im Editor | lebt in HE_Editor; das gebaute Spiel kann sie nicht benutzen |

### 1.6 Topologie und Sicherheit, wie sie bleiben

Host-autoritativ: der Host ist ein Spieler und die Wahrheit (Anti-Cheat-Plan
§1.4). Der Kanal ist authentifiziert (Join-Code) und verschlüsselt. Der Angreifer
bleibt der legitime Client mit modifizierter Software (Anti-Cheat-Plan §2.1).
Jede Nachricht Client → Host, die dieser Plan neu einführt (RPC, Client-seitige
Variablenänderung), ist ein **Anspruch**, den der Host prüft, nicht eine
Tatsache, die er übernimmt. Das ist die eine Regel, die alle Abschnitte teilen.

### 1.7 Bausteine, die wiederverwendet werden

| Baustein | Ort | Verwendung hier |
|---|---|---|
| UDP-Sockets, non-blocking, `sendTo/recvFrom` | `Net/Socket.h:139-215` | `UdpTransport` (§4) |
| `BitWriter/BitReader` | `Net/BitStream.h` | Paket-Header, Property-Werte |
| `SecureTransport::wrap` als Decorator über *jedem* `ITransport` | `Net/SecureTransport.h` | bleibt die Krypto; bekommt das Replay-Fenster (§4.6) |
| `NetSession` (Message-Ids, Handler, Peer-Liste) | `Net/NetSession.h` | unverändert |
| `SessionDirectory`, `LanBeacon`, `PortMapper` | `Net/*.h` | Session finden und ankündigen (§5.3), mit den Ergänzungen aus §1.5 |
| `CollabController` als Vorlage für „ein Objekt besitzt den ganzen Stack" | `HE_Editor/CollabController.h` | `NetGameSession` in HE_Scene (§5.1) |
| `AntiCheatHost::attach`, `AntiCheatEvents::dispatch` (fünf Stationen) | `HorizonScene/AntiCheat/` | Anti-Cheat wird mit angeschlossen; Lebenszyklus-Events nach demselben Dispatcher (§7.4) |
| Registry-Dreiklang Display-Name-Map, `HcNodeDocs`, `isScriptGroup`; Row-Form wie `anticheat.check` (`EngineApi.cpp:6283`) | `EngineApi.cpp`, `HcNodeDocs.cpp` | `net`-Gruppe (§7) |
| Event-Tabelle + `fireOn…` + `IScriptBackend::callOn…` + `IGameLogic`-Hook | `HorizonCode.cpp:2526`, `HorizonCodeRuntime.cpp:944`, `IScriptBackend.h:101-110` | `OnPlayerJoined` & Co. (§7.4) |
| `Runtime::callFunction(id, fn, requirePublic, args, results)` | `HorizonCodeRuntime.h:191` | RPC-Zustellung und `OnRep_<Var>` (§6.4, §7.2) |
| `HorizonWorld::entityId / findByEntityId` (UUID) | `HorizonWorld.h:30-33` | Szenen-Entities über das Netz binden (§5.5) |
| `Ctx::createObject` (Klasse + Position + Rotation) | `EngineApi.h` | Spawn auf dem Client (§5.5) |
| `valueToJson` (Save-Pfad) | `EngineApi.cpp:3959` | JSON-Form für die C++-Grenze (§6.3, §7.3) |
| `Application::launchArguments()` | `Application.h:222` | `--host`, `--join` für Tests am gebauten Spiel (§5.7) |
| `HeAntiCheatServices` als Muster einer append-only Services-Tabelle | `HorizonGameServices.h:242` | `HeNetServices` (§7.3) |
| `Log::Cat::Net`, `Cat::AntiCheat` | `Diagnostics/Log.h:80, 86` | `Net` bleibt; Replikations-Zeilen bekommen `Cat::Replication` |

---

## 2. Leitentscheidungen

Fünf Entscheidungen, jede mit dem Grund, und mit dem, was sie ausschließt.

**E1. Eigener UDP-Transport, kein GameNetworkingSockets.** GNS bringt
OpenSSL oder libsodium, protobuf, ein eigenes Build-System und ~100 000
Zeilen mit; auf Windows-CI wäre das die dritte große Fremd-Abhängigkeit mit
eigenen Link-Fallen. Was davon gebraucht wird (Zuverlässigkeit, Fragmentierung,
Keepalive, Verschlüsselung), hat die Engine zur Hälfte schon
(`SecureTransport`) und ist zur anderen Hälfte ein paar hundert Zeilen mit
klar bekannten Algorithmen (Ack-Bitfeld, RTO nach Jacobson, Fragment-Reassembly).
Was GNS darüber hinaus kann (Relay über Valves Netz, ICE), setzt Steam voraus
und ist kein Ziel (§9). **Folge:** `docs/networking-layer-design.md` wird in
Schritt 2 an den drei Stellen korrigiert, die GNS reservieren.

**E2. `SecureTransport` bleibt die einzige Krypto, und bekommt ein
Replay-Fenster.** Kein zweiter Verschlüsselungspfad für UDP. Der Decorator
wrappt `UdpTransport` genau wie `TcpTransport`; der einzige Unterschied ist die
Replay-Regel (§4.6). Damit gilt das gesamte Sicherheitsmodell aus
`networking-layer-design.md` §Security auch für das Spiel, und ein Test über
`SecureTransport` + `LossyTransport` deckt beide Konsumenten.

**E3. Host-autoritativ bleibt; `NetRole::Server` wird nicht gebaut.** Ein
dedizierter Server (GameApplication ohne Fenster und Renderer) ist ein eigenes
Thema (§9.2, Frage 1). Alles hier funktioniert mit `Host` + `Client`, und
nichts hier verbaut `Server`: der Code fragt `isAuthority()` (Host oder
Server), nie `role == Host`.

**E4. Zustand fließt nur Host → Client; Ansprüche fließen Client → Host.**
Replizierte Variablen schreibt nur die Autorität. Ein Client, der eine
replizierte Variable setzt, ändert sie lokal (Prediction erlaubt), und der
nächste Host-Wert überschreibt sie; einmal pro Variable wird das geloggt. Was
ein Client dem Host **sagen** will, sagt er per `CallServer`. So gibt es genau
einen Pfad für Ansprüche, und der ist der, an dem `anticheat.check` hängt.

**E5. Argumente reisen als typisierte `Value`-Liste, nicht als JSON.** Die
Registry-Rows sind fixarity mit typisierten Pins (siehe die Row-Form von
`anticheat.check`). „`CallServer` mit beliebigen Argumenten" kann deshalb
keine einzelne Row sein. Die Lösung ist zweiteilig (§7.2): in HorizonCode
bekommt der `FunctionEntry`-Knoten einen **Run-On**-Modus, und die Parameter
der Funktion selbst sind die Argumente; Lua/Python rufen `net.callServer(self,
"Name", …)` variadisch über den generischen Dispatcher; C++ bekommt eine
JSON-Form, weil die C-ABI kein `Value` kennt. Alle vier landen auf derselben
Nachricht.

---

## 3. Architektur

### 3.1 Überblick

```
                CLIENT                                        HOST (Autorität)
 ┌───────────────────────────────────┐          ┌───────────────────────────────────────────┐
 │ NetGameSession (HE_Scene)         │          │ NetGameSession (HE_Scene)                 │
 │  ├ UdpTransport ──► SecureTransport ═════════► SecureTransport ◄── UdpTransport          │
 │  ├ NetSession (Message-Ids)       │   UDP    │  ├ NetSession                             │
 │  ├ GameReplication  (wie heute)   │          │  ├ GameReplication  (wie heute)           │
 │  │   Snapshot ◄── unreliable ─────┼──────────┼──┤   sendSnapshots                        │
 │  │   pushInput ── unreliable ─────┼──────────┼─►│   handleInput → AntiCheat pre/post     │
 │  ├ PropertyReplicator  (neu)      │          │  ├ PropertyReplicator (neu)               │
 │  │   apply + OnRep_<Var> ◄ reliable ─────────┼──┤   dirty → Delta                        │
 │  ├ SpawnReplicator     (neu)      │          │  ├ SpawnReplicator (neu)                  │
 │  │   Bind/Spawn/Despawn ◄ reliable ──────────┼──┤   Szenen-Bind + Create Object          │
 │  ├ RpcRouter           (neu)      │          │  ├ RpcRouter (neu)                        │
 │  │   CallServer ── reliable ──────┼──────────┼─►│   Owner? Rate? → callFunction          │
 │  │   OnRpc ◄─ CallClient/All ─────┼──────────┼──┤                                        │
 │  └ PlayerRoster        (neu)      │          │  └ PlayerRoster (neu)                     │
 │      OnPlayerJoined/Left ◄────────┼──────────┼──┤   conn ↔ playerId ↔ Controller ↔ netId │
 └───────────────────────────────────┘          └───────────────────────────────────────────┘
                 ▲                                                   ▲
    GameApplication / EditorApplication (PIE): pump, Ctx-Binding, Frame-Ende-Ausführung
```

### 3.2 Komponenten

| Komponente | Modul | Neu? | Aufgabe |
|---|---|---|---|
| `UdpTransport` | HE_Net | neu | `ITransport` über UDP-Sockets: Verbindung, drei `SendMode`s, Fragmentierung, Keepalive (§4) |
| `LossyTransport` | HE_Net | neu | Decorator für Tests: Verlust, Reorder, Duplikate, Latenz, deterministisch geseedet (§4.8) |
| `SecureTransport` | HE_Net | erweitert | Anti-Replay-Fenster statt streng steigendem Counter (§4.6) |
| `PortMapper`, `SessionDirectory`, `LanBeacon` | HE_Net | erweitert | Protokoll-Parameter, `kind`-Feld, UDP-Reachability (§5.3) |
| `NetGameSession` | HE_Scene (`HorizonScene/Net/`) | neu | besitzt den Stack, Host/Join/Leave, Roster, verdrahtet `GameReplication` + `AntiCheatHost` (§5) |
| `SpawnReplicator` | HE_Scene | neu | Bind (Szenen-UUID ↔ netId), Spawn (Klasse + Transform), Despawn, Late-Join-Baseline (§5.5) |
| `PropertyReplicator` | HE_Scene | neu | Registrierung replizierter Variablen, Dirty-Tracking, Delta, `OnRep` (§6) |
| `RpcRouter` | HE_Scene | neu | `CallServer` / `CallClient` / `CallAllClients`: Serialisierung, Owner- und Ratenprüfung, Zustellung an den Frontend-Vierteiler (§7) |
| `HE::api::net` | HE_Scene | neu | Registry-Gruppe, Display-Namen, `HcNodeDocs`, `isScriptGroup`; `HeNetServices` (§7.3) |
| `NetworkComponent::replicates` | HE_Scene | erweitert | das eine Flag hinter dem Schalter (§8.1) |
| `HorizonCode::Variable::replicated / repNotify`, `Node::runOn` | HE_Core | erweitert | Authoring von Property-Sync und RPC im Graph (§6.2, §7.2) |
| `ProjectMultiplayerSettings` | HE_Core | neu | Port, Tick, Spielerzahl, Discovery, Prediction-Grenzen (§8.4) |
| Inspector-Kategorie „Replication", Play-Toolbar, Net-Overlay | HE_Editor | neu/erweitert | §8 |

**Schichtregel bleibt:** HorizonNet weiß nichts von Szenen, Werten oder
Klassen. Alles, was `Value`, `HorizonWorld` oder den Runtime braucht, liegt in
HE_Scene unter `HorizonScene/Net/` neben `GameReplication`. Kein neues Modul,
keine neue DLL, keine neue Deploy-Liste.

---

## 4. (a) `UdpTransport`

### 4.1 Vertrag

`UdpTransport final : public ITransport`, derselbe Vertrag wie `TcpTransport`:
`listen(port)`, `connect(host, port)`, `update()`, `send(conn, data, len,
mode)`, `poll(out)`, `disconnect(conn)`, `connectionCount()`, `boundPort()`.
Ein Socket pro Transport; auf dem Host teilen sich alle Peers einen Socket,
unterschieden nach Absenderadresse. `update()` ist non-blocking, liest bis
`WouldBlock`, treibt Resends, Keepalives und Timeouts, und wird wie heute aus
dem Frame gepumpt.

Alle drei `SendMode`s werden umgesetzt, nicht zwei: `Unreliable` (Snapshots,
Input), `Reliable` (ungeordnet: Property-Deltas verschiedener Entities dürfen
einander überholen), `ReliableOrdered` (Handshake, Spawn/Despawn, RPC,
Integrity, Notice). Die Unterscheidung Reliable/ReliableOrdered kostet nichts
extra, weil die Reihenfolge ohnehin pro Kanal geführt wird.

### 4.2 Paketformat

```
Datagramm  = [Header 13 B][Payload ≤ kMtuPayload]
Header     = magic:u16 "HU"      Protokoll-Kennung, filtert fremde Datagramme auf dem Port
             version:u8          kUdpProtocolVersion, Mismatch → Reject
             flags:u8            bit0 Connect, bit1 Challenge, bit2 Accept, bit3 Reject,
                                 bit4 Disconnect, bit5 KeepAlive, bit6 Fragment, bit7 AckOnly
             channel:u8          0 Unreliable, 1 Reliable, 2 ReliableOrdered
             seq:u16             Paket-Nummer, EIN Zahlenraum pro Richtung für alle Kanäle
             ack:u16             höchste vom Peer empfangene Paket-seq
             ackBits:u32         Bitfeld der 32 davor (1 = empfangen)
Fragment   = [fragId:u16][index:u8][count:u8] zusätzlich, wenn bit6
Ordered    = [msgSeq:u16] zusätzlich auf Kanal 2: Nachrichten-Nummer für die Reihenfolge
```

**Ein Paket-Zahlenraum, nicht einer pro Kanal.** `seq` zählt jedes gesendete
Datagramm dieser Richtung, egal auf welchem Kanal; `ack`/`ackBits` bestätigen
Paket-Nummern, und der Sender weiß pro Paket-Nummer, ob dahinter eine
Reliable-Nachricht stand (dann Resend) oder nicht (dann nichts). Das ist die
Form, in der ein Ack-Feld für alle Kanäle zugleich gelten kann, und in der der
Unreliable-Strom die Reliable-Acks wirklich huckepack trägt. Die Reihenfolge
auf `ReliableOrdered` kommt aus dem eigenen `msgSeq`, das ein Resend
unverändert mitnimmt; Duplikaterkennung für alle Kanäle läuft über die
Paket-`seq`.

`kMtuPayload = 1200` Byte nach dem Header, die konservative Größe unter der
1280-Byte-Grenze von IPv6 minus IP-, UDP- und eigenem Header. Alles darüber wird fragmentiert (§4.5). Kein
Path-MTU-Discovery; 1200 hält auf jedem Weg, auf dem die Engine je getestet
wird.

`seq`, `ack` und `ackBits` sitzen in **jedem** Datagramm, auch in Unreliable:
so trägt der 30-Hz-Snapshot-Strom die Acks des Reliable-Kanals huckepack, und
ein Peer, der nur Snapshots bekommt, muss trotzdem kein eigenes Ack-Paket
schicken. `AckOnly` (bit7) gibt es für die Stille: wenn 50 ms lang nichts
gesendet wurde, aber Reliable-Daten unbestätigt angekommen sind.

### 4.3 Verbindungsaufbau: Cookie gegen Spoofing

```
client → host   Connect   [clientNonce:u64]
host   → client Challenge [cookie:u64 = HMAC(hostSecret, clientAddr || clientNonce)[0..8]]
client → host   Connect   [clientNonce][cookie]
host   → client Accept    [connId:u32]   (oder Reject [reason:u8])
```

Der Host **allokiert nichts**, bevor der Client den Cookie zurückgeschickt hat,
den nur der echte Absender der Adresse empfangen kann. Ohne diesen Schritt
könnte ein gefälschtes Absender-Datagramm den Host Peer-Zustand anlegen lassen
(Speicher) und die Challenge an ein Opfer schicken (Amplifikation, Faktor
klein, aber nicht null). `hostSecret` wird pro Prozess gewürfelt und alle 60 s
rotiert (zwei gelten gleichzeitig), wie es QUIC und DTLS 1.3 tun.

Erst nach `Accept` meldet `UdpTransport` `NetEventType::Connected` nach oben,
und **erst dann** beginnt `SecureTransport` seine Challenge. Zwei Handshakes
hintereinander, ~2 RTT insgesamt. Das ist gewollt: der UDP-Handshake beweist
Adressbesitz, der Secure-Handshake beweist den Join-Code; nichts von beidem
kann das andere übernehmen, ohne `SecureTransport` UDP-spezifisch zu machen (E2).

### 4.4 Zuverlässigkeit

Pro Peer ein Sendfenster unbestätigter Reliable-Pakete (`kSendWindow = 256`),
ein Sendpuffer mit Sendezeit, und die Ack-Auswertung aus jedem eingehenden
Header: `ack` bestätigt eine Paket-seq, `ackBits` 32 weitere. Ein Paket gilt
als verloren, wenn (a) es älter als `rto` ist, oder (b) drei jüngere Pakete
bestätigt wurden und es nicht (Fast Retransmit). Dann wird sein Inhalt neu
gesendet, als **neues Paket mit neuer Paket-seq**, aber mit demselben
`msgSeq` (Kanal 2) bzw. derselben Nachrichten-Kennung (Kanal 1); so bleibt
die Ack-Buchführung eindeutig (jede Paket-seq wird genau einmal gesendet) und
der Empfänger erkennt die Nachricht trotzdem als Duplikat.

```
rtt-Schätzer (Jacobson/Karels):  srtt = 7/8 srtt + 1/8 sample
                                 rttvar = 3/4 rttvar + 1/4 |srtt − sample|
                                 rto = clamp(srtt + 4 rttvar, 100 ms, 2 s)
Backoff bei wiederholtem Verlust: rto *= 2 pro Resend desselben Pakets, bis 2 s
```

`ReliableOrdered`: der Empfänger hält `nextExpected` über `msgSeq`; was davor
liegt, ist Duplikat (verworfen); was danach liegt, wartet im Reorder-Puffer
(`kReorderWindow = 256`); `poll()` liefert lückenlos. `Reliable`: jede
Nachricht trägt ebenfalls eine laufende Nummer (`msgSeq` desselben Formats,
eigener Zähler), die Duplikaterkennung läuft über ein 256-Bit-Fenster darüber,
Zustellung sofort.

Volles Sendfenster = `send()` puffert weiter, bis `kMaxQueuedReliable = 64
KiB` pro Peer; darüber wird der Peer getrennt, weil ein Empfänger, der 64 KiB
Reliable nicht abnimmt, ohnehin verloren ist (dieselbe Lehre wie
`TcpTransport::kMaxFrameSize`: nie unbegrenzt für einen Peer allokieren).

`Unreliable` kennt kein Resend und keinen Reorder-Puffer, aber die
Duplikaterkennung über die Paket-`seq` (256-Bit-Fenster): ein dupliziertes
Snapshot-Datagramm wird verworfen, nicht zweimal angewendet. Reorder in
Unreliable bleibt sichtbar, und `GameReplication` verträgt das heute nicht:
der Snapshot-Kopf ist `ack:u32 | count:u16` (`sendSnapshots`, `applySnapshot`
`GameReplication.cpp:610-640`), `applySnapshot` schiebt bei **jedem**
Snapshot `current → previous`, und `ack` taugt nicht als Ordnung, weil es für
einen Zuschauer ohne Eingaben konstant 0 bleibt. **Schritt 3 gibt dem
Snapshot eine Tick-Nummer** (`u32` vor `ack`) und verwirft ältere; das ist ein
Vergleich in `applySnapshot` und vier Byte pro Snapshot.

*Stand nach Schritt 3 (22.09.2026):* umgesetzt als `tick:u32 | ack:u32 |
count:u16`. Der Vergleich ist die vorzeichenbehaftete 32-Bit-Differenz zum
neuesten angewendeten Tick, damit der Zähler umlaufen darf; der Server beginnt
bei 1 und überspringt 0, weil 0 auf dem Client „noch nichts gesehen" heißt.
Älter = ganzes Datagramm weg (`Stats::snapshotsStale`), gleich = weiterer Teil
desselben Ticks. Ein Duplikat wird **pro Entity** über den Tick des letzten
Samples erkannt (`Stats::samplesDuplicate`), weil ein nochmaliges Schieben
`previous = current` setzen und die Entity bis zum nächsten Tick einfrieren
würde; die Reconciliation der eigenen Entity läuft aus demselben Grund einmal
pro Tick. Dazu kam ein Punkt, den §4.5 nur als Regel nennt: `UdpTransport`
**verweigert** Unreliable über `mtuPayload`, und ein Snapshot mit 19 Byte pro
Entity sprengt 1200 Byte ab ~60 relevanten Entities. `sendSnapshots` teilt
deshalb pro Client und Tick nach `Config::snapshotBudgetBytes` (Default 1024 =
1200 minus NetSession-Id 2 minus SecureTransport-Counter+Tag 24 minus Reserve)
in mehrere `kMsgSnapshot`-Datagramme mit derselben Tick-Nummer; jede Entity
liegt in genau einem Teil, es gibt nichts zusammenzusetzen, und ein verlorener
Teil kostet genau die Entities darin einen Sample. Gemessen in
`test_game_replication`: 200 Entities → 4 Datagramme ≤ 1026 Byte auf dem
Loopback; das größte Manifest, das der Writer erzeugen kann (256 Einträge,
255-Zeichen-Namen, SHA-256), ist 82 949 Byte = 70 von 255 Fragmenten.

### 4.5 Fragmentierung

Nur auf den Reliable-Kanälen (Unreliable-Nachrichten über 1200 Byte sind ein
Designfehler des Senders und werden mit einem Log verworfen). Eine Nachricht
> `kMtuPayload` wird in `count ≤ 255` Fragmente à 1196 Byte (1200 minus die 4
Byte Fragment-Kopf) zerlegt, jedes ein eigenes Reliable-Paket mit eigener seq;
der Empfänger sammelt pro `fragId` und liefert erst die vollständige Nachricht.
Maximale Nachricht damit ~300 KiB.

Warum das schon in Schritt 2 sein muss: `kMsgIntegrity` trägt bis 256 Einträge
à Kind + Name + Hex-SHA256 (`GameReplication.cpp:342-352`), ein Eintrag
~100 Byte, das Manifest eines Spiels mit 40 Dateien also ~4 KiB, mit Paks und
Dylibs auf Windows leicht 8 KiB. Ohne Fragmentierung kommt der Integritäts-
Check als erstes Feature über UDP nicht an. Die Late-Join-Baseline (§5.5) ist
ebenfalls größer als ein Datagramm, sobald eine Szene mehr als ~60
replizierte Entities hat.

### 4.6 `SecureTransport`: Anti-Replay-Fenster

Der Empfänger führt pro Peer `highest:u64` und `window:u64` (Bit i = Counter
`highest − i` gesehen). Ein Frame mit Counter `c`:

```
c >  highest              → akzeptieren, Fenster um (c − highest) schieben, Bit 0 setzen
highest − 63 ≤ c ≤ highest → Bit gesetzt? verwerfen (Replay), sonst akzeptieren + Bit setzen
c <  highest − 63         → verwerfen (zu alt)
```

Verwerfen heißt **Frame weg, Verbindung bleibt**; heute heißt es `failPeer`.
Ein echter Replay-Versuch fällt so geräuschlos durch, und ein umsortiertes
Paket kommt an. Der Invariant „kein Counter zweimal" bleibt exakt, und die
GCM-Nonce-Eindeutigkeit ist ohnehin Sache des Senders.

Die Schwelle, ab der ein verworfener Frame noch geloggt wird, ist eine
Zählerschwelle (ab dem zehnten verworfenen Frame pro Minute eine Warnung),
nicht jeder einzelne: über UDP ist Reorder jenseits von 64 keine Anomalie,
sondern Wetter.

**Wie das Fenster in den Transport kommt:** `SecureTransport::Config` bekommt
`replayWindow: u8` (0 = heutiges Verhalten, streng steigend; 64 = Fenster).
Der Aufrufer weiß, was er wrappt; `CollabController` lässt es bei 0,
`NetGameSession` setzt 64. Nicht aus dem inneren Transport erraten, weil ein
`ITransport` nicht sagt, ob er umsortiert, und ein Decorator das auch nicht
wissen sollte.

Die tests in `test_net_secure.cpp` bleiben alle grün, weil Default 0 ist; ein
neuer Test wrappt `LossyTransport` mit `replayWindow = 64` und prüft: 1000
Frames mit Reorder bis 30 kommen an, ein bewusst dupliziertes Frame wird
verworfen, die Verbindung lebt.

### 4.7 Keepalive, Timeout, Trennung

TCP meldet ein totes Gegenüber irgendwann selbst; UDP nie. Deshalb:

- `KeepAlive` alle 250 ms, wenn 250 ms nichts gesendet wurde (auch der Host zum Client).
- Peer-Timeout `kTimeoutMs = 5000` ohne irgendein empfangenes Datagramm → `Disconnected` nach oben, Zustand weg.
- `disconnect(conn)` sendet dreimal `Disconnect` (unbestätigt, im Abstand von 50 ms), dann vergisst der Initiator den Peer; wie bei TCP entsteht für den Initiator kein lokales Event (`ITransport.h`-Vertrag).
- Ein Datagramm von einer Adresse ohne Peer-Zustand und ohne `Connect`-Flag wird still verworfen; ein verwaister Client bekommt so nie eine Antwort und läuft in seinen eigenen Timeout, statt dem Host einen Reject pro Snapshot abzuringen.

### 4.8 `LossyTransport` für Tests

Decorator über `ITransport` mit `Config { lossPercent, reorderPercent,
duplicatePercent, latencyMs, jitterMs, seed }`, deterministisch (`std::mt19937`
mit `seed`). Verzögerte Datagramme liegen in einer nach Zustellzeit
sortierten Queue, die `update()` mit einer **simulierten Uhr** abarbeitet
(`advance(ms)`), damit ein Test nicht wirklich wartet. Über
`LoopbackTransport::createPair()` gelegt, deckt das jeden Konsumenten:
`UdpTransport`s Reliability lässt sich damit **nicht** testen (die läuft unter
dem Decorator), wohl aber alles darüber: `SecureTransport` mit Fenster,
`GameReplication` mit Reorder und Verlust, Property-Deltas, RPC-Reihenfolge.
Die Reliability-Schicht selbst wird über echte Localhost-Sockets getestet
(`test_net_udp.cpp` wie `test_net_tcp.cpp`) mit einem **Loss-Hook im
`UdpTransport` selbst** (`setTestDropFn(std::function<bool(const uint8_t*,
size_t)>)`, nur in Tests gesetzt): der einzige Weg, Verlust *unter* der
Reliability zu erzeugen, ohne den Kernel zu bitten.

### 4.9 Konfiguration und Statistik

```cpp
struct UdpTransport::Config {
    std::uint32_t mtuPayload        = 1200;
    std::uint32_t timeoutMs         = 5000;
    std::uint32_t keepAliveMs       = 250;
    std::uint32_t maxQueuedReliable = 64 * 1024;
    std::uint32_t maxPeers          = 64;     // Host: darüber Reject
};
struct UdpTransport::Stats {  // pro Transport, plus peerStats(conn)
    std::uint64_t datagramsSent, datagramsReceived, bytesSent, bytesReceived;
    std::uint32_t resends, duplicatesDropped, fragmentsReassembled, cookiesRejected;
    float         srttMs, lossPercentWindow;  // gleitendes 5-s-Fenster
};
```

`srttMs` und `lossPercentWindow` sind das, was das Overlay (§8.5) und
`net.ping(player)` anzeigen.

### 4.10 IPv6, NAT, Dual-Stack

Wie `TcpTransport`: `listen` bindet einen v6-Socket mit `IPV6_V6ONLY = 0`, wo
das geht (macOS, Linux), sonst zwei Sockets (Windows kann Dual-Stack, aber
das Beacon-Verhalten aus Memory `port-forward-refusal-vs-absence` legt nahe,
jede Familie explizit zu behandeln). `connect` nimmt, was `SessionLookup`
liefert, best-first. NAT-Traversal ist **kein** Teil: kein STUN, kein
Hole-Punching, kein Relay (§9.1). Ein Host hinter NAT braucht die
Portfreigabe (§5.3), sonst LAN.

---

## 5. (b) Verdrahtung: `NetGameSession`

### 5.1 Ein Objekt besitzt den Stack

Nach dem Vorbild `CollabController`, aber in **HE_Scene**, damit das gebaute
Spiel und der Editor dasselbe benutzen:

```cpp
// HorizonScene/Net/NetGameSession.h  (kein HE_API, wie GameReplication)
class NetGameSession {
public:
    enum class Status { Idle, Hosting, Connecting, Joined, Failed };
    struct HostOptions { std::uint16_t port = 0; std::string displayName;
                         bool announceLan = true; bool publishDirectory = true;
                         bool mapPort = true; std::uint32_t maxPlayers = 8; };
    bool host(const HostOptions&);
    bool joinBySessionId(const std::string& sessionId, const std::string& joinCode, const std::string& displayName);
    bool joinDirect(const std::string& host, std::uint16_t port, const std::string& joinCode, const std::string& displayName);
    void leave();
    void update(float dt);          // pumpt Transport → NetSession → Replication/Property/Spawn/Rpc
    // Rollen und Roster
    bool isAuthority() const;       // Host oder (künftig) Server
    bool isClient() const;
    PlayerId localPlayer() const;   // 1 auf dem Host, vom Host vergeben auf Clients
    const PlayerRoster& roster() const;
    // Teile
    GameReplication& replication(); PropertyReplicator& properties();
    SpawnReplicator& spawns();      RpcRouter& rpc();
    HE::AntiCheat::AntiCheatHost* antiCheat();
    // Discovery (LAN + Directory), wie CollabController::lanSessions / directoryBusy
    ...
};
```

`host()` baut `UdpTransport::listen → SecureTransport::wrap(replayWindow = 64)
→ NetSession(Host) → GameReplication(Host) + PropertyReplicator +
SpawnReplicator + RpcRouter`, ruft `AntiCheatHost::attach(&service,
&replication)` **wenn** `ProjectAntiCheatSettings::enabled`, generiert
Session-Id und Join-Code (`SecureTransport::generateJoinSecret`), startet
Beacon/Directory/PortMapper nach Optionen. `join…()` baut dieselbe Kette
mit `connect`. Beide asynchron über `Status`, wie bei Collab.

### 5.2 Frame-Reihenfolge (Host)

```
UdpTransport::update()  →  NetSession::pump()  (Connected/Disconnected/Data, Handler laufen)
  → RpcRouter: eingegangene CallServer ausführen  (VOR der Simulation: sie sind Eingaben)
  → Simulation (tickWorld, Mover über handleInput ist schon in pump gelaufen)
  → PropertyReplicator::collectDirty()  (nach der Simulation: was sich geändert hat)
  → GameReplication::update(dt)  (Snapshot, wenn Tick fällig)
  → PropertyReplicator::flush()  (Deltas, reliable)  +  SpawnReplicator::flush()
  → AntiCheatHost::pump()/flush()  (wie heute vorgesehen: Frame-Ende)
  → Lebenszyklus-Events zustellen (OnPlayerJoined/Left), die pump() gesammelt hat
```

Client: dasselbe ohne Simulation der fremden Entities; `pushInput` läuft aus
dem Input-System **vor** `update()`; `OnRep_*` und `OnRpc` werden nach
`pump()` und **vor** der Skript-Tick-Phase zugestellt, damit ein Skript im
selben Frame den neuen Wert sieht.

Eine Regel aus Memory `hc-breakpoints-latent-resume` gilt hier wörtlich:
Zustellung an Skripte **nie aus einem NetSession-Handler heraus** (das wäre
mitten in `pump()`), sondern gesammelt und am definierten Punkt. Handler
schreiben in Queues; die Frame-Reihenfolge leert sie.

### 5.3 Session finden und ankündigen

Dieselben drei Wege wie Collab, mit den Ergänzungen aus §1.5:

| Weg | Reuse | Ergänzung |
|---|---|---|
| **LAN** | `LanBeacon::Announcer/Browser` | `Announcement` bekommt `kind: u8` (0 Collab, 1 Game) und `protocol` trägt für Spiele `kGameProtocolVersion`; `Browser` filtert nach `kind`. Editor-Join-Liste zeigt nur 0, Spiel-Lobby nur 1. Wire-kompatibel: alte Announcer senden kein `kind`, Leser nehmen 0 an. |
| **Directory** | `SessionDirectory::register/lookup/heartbeat` | `session-api.php` bekommt `kind` und `transport: "udp"`; **`reachable` wird für UDP nicht behauptet** (der Server kann keinen UDP-Connect-Back machen). Stattdessen macht der Host nach dem Registrieren einen **Self-Probe**: er schickt ein UDP-Datagramm an seine eigene öffentliche Adresse:Port und wartet auf das Echo aus dem eigenen Socket (Hairpin). Kommt es, ist die Freigabe wirksam; kommt es nicht, sagt die UI, was sie heute bei `reachable = false` sagt. Ehrlich: Hairpin-NAT unterstützen nicht alle Router; das Ergebnis ist dann „unbekannt", nicht „nein", und die UI sagt das auch. |
| **Direkt** | `joinDirect(host, port, code)` | wie `CollabController::joinSession` |
| **Portfreigabe** | `PortMapper::mapPort` | bekommt `Protocol { Tcp, Udp }` als Parameter; UPnP `NewProtocol = "UDP"`, PCP proto 17. `unmapPort` trägt das Protokoll im Handle. |

Die `session-api.php`-Änderung ist ein Website-Schritt (`Website/HorizonEngine/`),
in der Roadmap eigens aufgeführt, weil sie ein Deploy braucht.

### 5.4 Spieler: Roster, Controller, Charakter

Das fehlende Konzept. Ein **Spieler** ist eine Verbindung mit Namen und Id:

```cpp
using PlayerId = std::uint32_t;   // 1 = der Host selbst, 2… vom Host vergeben; 0 = niemand
struct PlayerInfo { PlayerId id; HE::Net::ConnectionId conn; std::string name;
                    std::uint32_t controllerInstance; std::uint32_t characterNetId; };
```

**Join-Ablauf auf dem Host** (nach `Connected` des SecureTransport, also
nach dem Join-Code):

1. `kMsgHello` (Client → Host, reliable): Anzeigename, `kGameProtocolVersion`, Projekt-Id (Vergleich wie `CollabController::setProjectIdentity`; Mismatch → Reject mit Label).
2. Host vergibt `PlayerId`, trägt ihn ins Roster, schickt `kMsgWelcome` (PlayerId, Szene-Pfad, Tick-Konfiguration).
3. Host instanziiert für den Spieler **einen Controller** derselben `PlayerController`-Klasse, die `PlayerHost` für den lokalen Spieler nimmt, markiert die Instanz als „remote, gehört PlayerId". Dessen BeginPlay läuft **auf dem Host** und spawnt, wie heute, den Charakter und ruft `player.possess`.
4. `player.possess(controller, character)` auf einem Controller mit Remote-Besitzer ist der Moment, in dem die Engine alles Nötige tut: `registerEntity(character, owner = playerId)`, `assignControl(conn, netId)` (**vor** der Spawn-Nachricht, genau in der Reihenfolge, die der Kommentar in `GameReplication.h` zu `setAntiCheat` verlangt), Spawn-Nachricht an alle, `kMsgControl` an den Besitzer (`setLocallyControlled` dort).
5. Late-Join-Baseline (§5.5) an den neuen Client, dann `OnPlayerJoined(playerId)` an alle Frontends auf dem Host und `OnConnected` auf dem Client.

**Eingaben eines entfernten Spielers:** Bewegung geht wie heute über
`InputCommand`. Alle **anderen** Input-Aktionen des entfernten Controllers
laufen auf dem **Client**, wo sein `PlayerHost` ganz normal Events zustellt;
was davon den Host erreichen soll, ruft der Graph explizit als `CallServer`
(§7). Das ist die Entscheidung E4 auf Input angewandt: `InputCommand` wird
**nicht** um Aktionen erweitert, weil eine Aktion („Feuer") ein Anspruch ist,
den der Host prüfen will, und `CallServer` genau dieser Pfad ist. Was die
Engine dafür liefert: `net.isAuthority()`, `net.isLocallyControlled(entity)`
und die Konvention in §7.5, damit dieselbe Klasse auf beiden Seiten korrekt
läuft.

**Was `PlayerHost` dafür lernen muss.** Heute ist er auf einen lokalen
Spieler gebaut: ein Controller pro `PlayerController`-Klasse, **jedes**
Input-Event an **jeden** Controller, und Charaktere über `addCharacter`
registriert (PlayerHost.h, Kopfkommentar „Where input goes"). Vier Folgen,
die Schritt 4 auflösen muss, weil sonst der Host mit zwei Spielern doppelt
läuft:

1. **Input-Routing nach Besitzer.** Jede Controller-Instanz bekommt einen
   `PlayerId`-Besitzer (1 = lokal). Lokale Eingaben gehen nur an Controller
   mit Besitzer 1 und an deren Charaktere; Remote-Controller hören auf dem
   Host **keine** lokalen Eingaben. Was sie hören, sind `CallServer`-Aufrufe
   ihres Clients (§7) und die `InputCommand`s über `handleInput`.
2. **Kein zweiter lokaler Charakter auf dem Client.** Der Client hat seinen
   eigenen `PlayerHost` mit eigenem Controller, dessen BeginPlay heute den
   Charakter spawnt. Im Netz darf er das nicht: der Host spawnt, der Client
   bekommt `kMsgSpawn`. Die Engine unterdrückt das nicht per Magie, sondern
   `Create Object` einer Klasse mit `replicates` auf einem **Client** ist ein
   No-op mit einer Log-Zeile (`Cat::Replication`, einmal pro Klasse): nur
   die Autorität erzeugt replizierte Objekte. Ein Graph, der offline und
   online derselbe sein soll, braucht dafür keinen Zweig; die Regel in §7.5
   sagt trotzdem, wie man es sauber schreibt.
3. **Possess auf dem Client.** `kMsgControl` (Host → Besitzer, nach dem
   Spawn) trägt `netId` des Charakters; der Client löst ihn auf, ruft
   `setLocallyControlled` **und** `player.possess(localController, character)`,
   damit Kamera-Rig, Input-Weiterleitung und `player.character()` auf dem
   Client dasselbe sagen wie auf dem Host.
4. **`Ctx::createObject` auf dem Client registriert PlayerCharacter beim
   PlayerHost** (Ctx-Kommentar in `EngineApi.h`). Für fremde Spieler ist das
   falsch: ihre Charaktere dürfen keine lokalen Eingaben bekommen. Der
   `SpawnReplicator` ruft deshalb einen Spawn-Pfad mit `owner`-Parameter, der
   die Registrierung nur für `owner == localPlayer` macht.

**Verlassen:** `Disconnected` oder `kMsgBye` → `OnPlayerLeft(playerId)`,
Despawn des Charakters (konfigurierbar: `keepCharacterOnLeave` für
Rejoin-Spiele, außerhalb v1), `dropConnection` in `GameReplication` (existiert
schon), Controller-Instanz weg.

### 5.5 Spawn, Bind, Despawn, Late Join

Drei Nachrichten, alle `ReliableOrdered`, alle Host → Client:

| Nachricht | Inhalt | Wann |
|---|---|---|
| `kMsgBind` | `netId`, Szenen-Entity-UUID (`HorizonWorld::entityId`) | für jede Szenen-Entity mit `replicates`, beim Join |
| `kMsgSpawn` | `netId`, `owner: PlayerId`, Klassenpfad, Position, Rotation, initiale Property-Werte | für jede Laufzeit-Entity (`Create Object`) mit `replicates` |
| `kMsgDespawn` | `netId` | `Destroy Object` oder Spieler weg |

Der Client bindet per `findByEntityId(uuid)` → `adoptEntity`. Für Spawn ruft
er `Ctx::createObject(classPath, pos, rot)` (genau die Funktion, die die
Text-Frontends und der Graph auch benutzen, samt PlayerHost-Registrierung)
und adoptiert das Ergebnis. **Auf dem Client läuft die Klasse also auch**, mit
BeginPlay; §7.5 sagt, wie ein Graph erkennt, dass er nicht die Autorität ist.

**Late-Join-Baseline:** nach `Welcome` schickt der Host alle Binds, alle
Spawns und für jede replizierte Entity alle replizierten Properties (§6) als
eine Folge von Reliable-Nachrichten, plus **einen** Transform-Snapshot auch
für Entities mit `replicateTransform = false` (die sonst nie einen bekämen).
Erst dann gilt der Client als `Joined`. Der Host merkt sich nichts Besonderes
dafür: die Baseline ist „alles dirty für diesen einen Peer".

Woher weiß der Host, welche Szenen-Entities repliziert werden? Beim
Session-Start (und bei jedem Szenenwechsel) läuft ein Walk über
`registry.view<NetworkComponent>()` mit `replicates`, jede wird registriert.
Das ersetzt das heutige manuelle `registerEntity` im Spielcode: **niemand ruft
das mehr selbst**; der Schalter im Inspector ist die Registrierung.

### 5.8 Stand nach Schritt 4

Umgesetzt (Commits 1c5d9a7b und der Nachtrag dazu): `NetGameSession`,
`SpawnReplicator`, `PlayerRoster`, `NetworkComponent::replicates` samt
Registry-Walk und Serializer-Feld, `Cat::Replication`, `LanBeacon`-`kind`,
`Ctx::net`. Test `tests/test_net_game_session.cpp`, 12 Fälle.

**Die Nachrichten-Ids liegen jetzt an einer Stelle**
(`HorizonScene/Net/NetMessages.h`), weil drei Konsumenten auf derselben
`NetSession` senden und drei private Tabellen beim ersten gemeinsamen freien
Wert kollidiert wären:

| Id | Nachricht | Richtung | Modus |
|---|---|---|---|
| +200…203 | Snapshot, Input, Integrity, AntiCheatNotice | wie bisher | wie bisher |
| +204 | `kMsgHello` | Client → Host | ReliableOrdered |
| +205 | `kMsgWelcome` | Host → Client | ReliableOrdered |
| +206 | `kMsgReject` | Host → Client | ReliableOrdered |
| +207 | `kMsgBind` | Host → Client | ReliableOrdered |
| +208 | `kMsgSpawn` | Host → Client | ReliableOrdered |
| +209 | `kMsgDespawn` | Host → Client | ReliableOrdered |
| +210 | `kMsgBaseline` | Host → Client | ReliableOrdered |
| +211 | `kMsgJoinComplete` | Host → Client | ReliableOrdered |
| +212 | `kMsgBye` | Client → Host | ReliableOrdered |

**Vier Abweichungen von der Beschreibung oben**, jede mit Grund:

1. **`kMsgJoinComplete` steht nicht im Entwurf.** §5.5 sagt „erst dann gilt
   der Client als `Joined`", nennt aber keinen Marker, an dem der Client das
   merkt. Weil die ganze Join-Folge ReliableOrdered ist, reicht eine leere
   Nachricht am Ende; ohne sie müsste der Client raten, ob noch Binds kommen.
2. **Die Baseline ist eine eigene Nachricht, kein Snapshot.** Als Snapshot
   ginge sie zweifach verloren: `sendSnapshots` geht Unreliable (die einmalige
   Baseline wäre verlustgefährdet) und filtert `replicateTransform = false`
   heraus — genau die Entities, für die sie existiert —, und `applySnapshot`
   verwirft jeden Tick, der älter als der neueste angewendete ist, sodass der
   erste reguläre Snapshot nach der Baseline diese überholen und killen kann.
   `GameReplication::sendBaseline(conn)` steht deshalb außerhalb der
   Tick-Ordnung.
3. **Der Client-Spawn läuft über eine Callback-Naht, nicht über
   `Ctx::createObject`.** Objekte erzeugen ist Anwendungssache (Engine-Basis
   auflösen, nur Entity-Klassen durch `EntityHost`, PlayerCharacter beim
   `PlayerHost` anmelden); HE_Scene kann davon nichts rufen. Schritt 5 hängt
   `Ctx::createObject` an `SpawnReplicator::setSpawnFunction`. Damit ist auch
   „BeginPlay lief dort (Zähler in der Klasse)" aus §11.5 Fall 3 im Test
   vorerst durch den Callback-Zähler vertreten.
4. **Directory-Registrierung und `PortMapper::mapPort` sind NICHT verdrahtet**,
   anders als die Klammer in der Roadmap-Zeile 4 sagt, und `joinBySessionId`
   (§5.1) fehlt entsprechend. Beides hängt an denselben Feldern, die 5c ohnehin
   anfasst (`kind`, `transport`, Hairpin-Self-Probe); es zweimal zu verdrahten
   wäre Arbeit, die 5c wieder aufmacht. LAN-Announce ist da, mit `kind = Game`.

**Zwei Punkte für Schritt 5**, im Test sichtbar geworden:

- `notifySpawned` liest die Pose von der Entity, statt sie als Parameter zu
  nehmen. Als Parameter konnten Nachricht und Host-Transform auseinanderlaufen,
  und genau das tat der erste Testlauf: der Spawn landete beim Client an der
  richtigen Stelle und wurde vom nächsten Snapshot zum Ursprung gezogen.
- `bindSceneEntities` und `notifySpawned` senden an alle Verbindungen, auch an
  solche, die noch kein Hello geschickt haben. Der Dedupe im Client fängt die
  Doppelung ab, aber Schritt 5 wertet `owner == localPlayer` aus, und vor dem
  Welcome ist `localPlayer` noch 0.

Szenenwechsel (`scene.load`) im Multiplayer: der Host schickt `kMsgScene`
(Pfad), Clients laden, melden `kMsgSceneReady`, der Host bindet neu. Bis alle
bereit sind, pausiert der Host die Snapshots für die, die noch laden. Additive
Zonen (`loadAdditive`) sind v1-Nicht-Ziel (§9.1), weil ihre Root-Entities
Laufzeit-Identitäten haben, die erst ein Zonen-Bind bräuchte.

### 5.9 Stand nach Schritt 5

Umgesetzt (Commits `cec54ae3`, `fdc0c86e` und der Nachtrag dazu): beide
Anwendungen halten eine `NetGameSession`, `Ctx::net` zeigt darauf, die
`net`-Gruppe steht mit 22 Rows, und die sechs Lebenszyklus-Events laufen durch
alle vier Frontends.

**Die zwei Punkte aus §5.8 sind erledigt.** `notifySpawned` las die Pose schon
in Schritt 4 von der Entity; der zweite, dass Binds und Spawns auch an Peers
vor dem Welcome gingen, ist jetzt ein `SpawnReplicator::setJoinedFilter`, den
`NetGameSession` gegen das Roster bindet. Das war kein Schönheitsfehler: `owner
== localPlayer` ist der Test, mit dem der Client entscheidet, ob ein Spawn in
seinen `PlayerHost` gehört, und vor dem Welcome ist `localPlayer` noch 0 --
genau der Wert, den eine host-eigene Entity als `owner` trägt. Ein Client hätte
also die Kiste des Hosts als seine eigene Spielfigur angemeldet. Die
Negativkontrolle steht im Test: ohne den Filter zählt `spawnsSent` 1 statt 0.

**Der PlayerHost-Umbau ist NUR ZUR HÄLFTE gemacht, und das ist die wichtigste
Zeile dieses Abschnitts.** §5.4 verlangt drei Dinge; Schritt 5 hat eineinhalb:

| §5.4 | Stand |
|---|---|
| 3. Possess auf dem Client | **fertig.** `kMsgControl` → `setLocallyControlled` + `player.possess` über die `ControlFn`, in beiden Anwendungen gebunden. |
| 2. `Create Object` auf einem Client ist ein No-op | **fertig.** `NetGameSession::refuseClientSpawn` in beiden `Ctx::createObject`, eine Log-Zeile pro Klasse. |
| 1. Besitzer pro Controller, Eingaben nur an Besitzer 1 | **offen.** |
| Host instanziiert pro Beitretendem einen Controller, dessen BeginPlay spawnt und possesst | **offen.** |

Die letzten beiden gehören zusammen und sind deshalb zusammen offen: solange
der Host für einen Beitretenden gar keinen Controller anlegt, gibt es auch
keinen, an den lokale Eingaben fälschlich gingen. Die Folge heute, klar
gesagt: **ein Beitretender ist ein Zuschauer.** Er bekommt Binds, Spawns,
Baseline und Snapshots, aber niemand ruft in der Produktion `assignControl` --
`kMsgControl` hat bisher nur den Test als Aufrufer -- und sein eigener
`Create Object` ist seit diesem Schritt korrekt ein No-op. Er sieht die Welt
des Hosts und fährt nichts darin. Das ist der erste Punkt für den nächsten
Schritt.

**Zwei weitere offene Punkte, an denen dieser Schritt vorbeigegangen ist:**

- **Ein Client kennt die anderen Spieler nicht.** Sein Roster hält sich selbst,
  unter der Id, die der Host im Welcome vergeben hat (das war ein Fehler in
  Schritt 4: der Eintrag trug 1, `localPlayer()` sagte 2, also gab
  `net.playerName(net.localPlayer())` auf jedem Client leer zurück). Die
  anderen fehlen, weil nichts sie schickt; der natürliche Ort ist die
  Late-Join-Baseline, eine Nachrichten-Id ist dafür noch nicht reserviert. Die
  Docs von `net.playerCount/playerAt/playerName` sagen das jetzt.
- **Szenenwechsel in laufender Session.** `kMsgScene`/`kMsgSceneReady` (214/215)
  sind weiterhin nur reserviert. Schlimmer als nur fehlend: die Replikation
  hält Entity-Handles in die Welt, die `performSceneSwitch` ersetzt, und ein
  Handle aus der alten Registry kann in der neuen eine andere Entity
  bezeichnen. Deshalb beendet der Schritt die Session beim Szenenwechsel, mit
  einer Warnung -- ein sichtbarer Ausgang statt zweier Peers, die still
  verschiedener Meinung darüber sind, was Netz-Id 12 ist.

**Nicht gebaut, bewusst:** die Parity-Fixture `net_events` in `HCGEN_CLASSES`.
Damit ist nicht bewiesen, dass der Codegen `onPlayerJoined(int)` genau so
emittiert, wie die Virtuals in `HorizonCodeCompiled.h` sie deklarieren -- das
Muster ist `cheat_event`, das Risiko klein, die Lücke aber echt.

**Vier Abweichungen von der Beschreibung oben:**

1. **`kMsgControl` (+213) ist aus der Reservierung geholt und `assignControl`
   ist EINE Funktion**, nicht drei Aufrufe an einer Aufrufstelle. §5.4 Punkt 4
   nennt die Reihenfolge (Besitz, `assignControl` in der Replikation, dann die
   Nachricht), und sie falsch zu haben ist unsichtbar: die erste Eingabe des
   Besitzers käme bei einem Host an, der die Entity noch nicht für seine hält,
   und das ist eine Hard-Observation gegen einen ehrlichen Spieler.
2. **Der LAN-Browser wohnt jetzt doch in `NetGameSession`.** Schritt 4 hatte ihn
   bewusst draußen gelassen ("Browsen ist Sache der UI"), aber `net.joinLan` und
   `net.lanSessionCount` sind Rows, und eine Row hat keine UI, in der sie
   nachsehen könnte. Er läuft unabhängig von `status()`, weil ein Hauptmenü
   sucht, bevor es irgendwo beigetreten ist -- deshalb wird er in `update()`
   VOR der Leerlauf-Bremse gepumpt.
3. **`Application::launchFlags()` ist neu.** `--host` wäre nie angekommen:
   `launchArguments()` wirft alles mit Bindestrich weg, weil ein Dokument keine
   Option ist. Der Join-Code steht beim `--host` auf INFO im Log, und das ist
   Absicht -- ohne diese Zeile kann kein zweiter Prozess ihn erfahren.
4. **Kein `net.declareVar*/setVar*/getVar*` und kein `net.call*`.** Die stehen
   in §7.1 in derselben Tabelle, gehören aber zu Schritt 6 und 7; sie hier
   anzulegen hieße, Rows ohne den `PropertyReplicator` bzw. den `RpcRouter`
   dahinter zu haben.
5. **Der Editor pumpt die Session auf `m_isPlaying`, nicht auf `simulating`.**
   Zuerst stand sie beim Anti-Cheat-Pump, und der hängt hinter der Pause. Ein
   Host, der aufhört, seinen Socket zu leeren, weil sein Autor Pause gedrückt
   hat, ist ein Host, dessen Spieler alle in den Timeout laufen. Pause friert
   die Simulation ein, nicht die Verbindung.

**Der Zwei-Prozess-Durchlauf, 23.09.2026, macOS 27, zwei echte Prozesse über
echte UDP-Sockets auf 127.0.0.1** (gebautes Spiel gegen gebautes Spiel, nicht
Editor gegen Spiel -- siehe unten):

```
Host   : --host=7777 --name=HostBox
         "Hosting session 'X7M7WJCC' as 'HostBox': 0 replicated entities"
         "--host: listening on port 7777, join code HBAQ68M3YVWZSB8YV0VS05WDDM"
         "Player 2 ('ClientBox') joined on connection 1 (2 in session)"
         "Player 2 ('ClientBox') left (0)"
Client : --join=127.0.0.1:7777 --code=<der Code> --name=ClientBox
         "UDP connect established (conn 1, host's id 1)"
         "Secure channel armed as client, payload encryption on (AES-256-GCM)"
         "Welcome: player 2, scene '', 30.0 Hz"  →  "Joined as player 2"
```

Damit sind Handshake, Cookie, Krypto, Hello/Welcome/JoinComplete, Roster und
der Abgang über Prozessgrenzen hinweg einmal wirklich gelaufen.

**Was dieser Durchlauf NICHT zeigt, ehrlich gesagt.** Das benutzte Projekt war
ein alter Export ohne eine einzige Entity mit `Replicates` -- "0 replicated
scene entities" steht so im Log. Es ist also der Verbindungs- und
Spielerpfad, der belegt ist, nicht Snapshots, Spawns oder Possession über
den Draht; die haben ihren Beleg im headless-Test über `LossyTransport`. Und
es war Spiel gegen Spiel: der Editor-Host geht nur über einen Menüklick, den
niemand von der Kommandozeile auslösen kann. Beides gehört zu 5b, wo mit der
Inspector-Kategorie erst eine Szene entsteht, die etwas zu replizieren hat.

Die LAN-Ankündigung scheiterte an der fehlenden Local-Network-Berechtigung des
Terminals (`LanBeacon.cpp:192` sagt genau das) -- für einen Direkt-Join
belanglos, für `net.joinLan` auf diesem Rechner nicht, und deshalb hier notiert.

### 5.6 Play-Modus im Editor

Zwei Wege, der zweite als Ausbau:

1. **Zwei Prozesse, wie Collab.** Play-Toolbar bekommt neben ▶ ein Menü
   „Play as Host" / „Join…" (Session-Liste aus dem LAN-Browser, Filter
   `kind = Game`). Ein zweiter Editor (oder ein gebautes Spiel mit `--join`,
   §5.7) tritt bei. Das ist die Variante, die in Schritt 4 kommt, weil sie
   nichts anderes voraussetzt als `NetGameSession`. Kick und Telemetrie
   bleiben im PIE aus (Anti-Cheat-Plan §6.2.6).
2. **Zwei Welten in einem Prozess** über `LoopbackTransport::createPair()`
   (die zweite Welt ohne Renderer, ein Viewport-Umschalter). Ergonomisch die
   beste Variante, technisch die teurere: `g_host`-Statics in beiden
   Anwendungen (Memory `dialog-static-leaks-across-projects`), ein Runtime pro
   Welt, doppelte PlayerHosts. Offene Frage §9.2 Nr. 4; nicht in v1.

Die Tests aus §11.5 brauchen keinen von beiden: sie fahren zwei
`NetGameSession`s über `LossyTransport` headless.

### 5.7 Gebautes Spiel

`GameApplication` erzeugt eine `NetGameSession` (inert bis `host/join`),
bindet sie in `g_host` (`Ctx::net`, §7.1) und pumpt sie in der
Frame-Reihenfolge §5.2. **Starten tut das Spiel selbst**, aus seinem Menü:
`net.host()` / `net.join(sessionId, code)` / `net.joinLan(index)` sind
Registry-Rows. Zwei Startargumente für Tests und Server-Skripte:
`--host[=port]` und `--join=<host:port|sessionId> --code=<joinCode>`, gelesen
aus `launchArguments()` nach dem Laden der Projekt-Settings; sie rufen
dieselben Rows.

---

## 6. (c) Property-Replikation und OnRep

### 6.1 Was eine „Property" hier ist

Ein benannter, typisierter Wert an einer replizierten Entity, den die
Autorität schreibt und alle Clients lesen. Drei Quellen, ein Modell:

| Quelle | Deklaration | Speicher | Schreiben (Autorität) | Lesen (Client) |
|---|---|---|---|---|
| HorizonCode-Klassenvariable | Checkbox **Replicated** (+ **Notify**) in der Variablen-Liste; `Variable::replicated`, `Variable::repNotify` | die Instanzvariable selbst | `SetVariable`-Knoten wie immer; der Runtime meldet `dirty` | `GetVariable` wie immer |
| Lua / Python | `horizon.net.declareVar(self, "health", 100)` in `onInit` | `ReplicatedVarsComponent` an der Entity: `name → Value` | `net.setVar(self, "health", v)` | `net.getVar(self, "health")` |
| C++ GameLogic | `he::net::declareVar(entity, "health", json)` | dieselbe Komponente | `setVar(entity, name, json)` | `getVar` |

Text-Skripte brauchen die explizite Form, weil ihre Variablen für die Engine
unsichtbar sind (Memory `savegames-v2`: „Script-Var-Capture" ist dort dieselbe
Lücke). HorizonCode braucht sie nicht, weil `Runtime::setVariable` der eine
Ort ist, durch den jede Änderung geht. `ReplicatedVarsComponent` ist auch das,
was `kMsgSpawn` als initiale Werte trägt, und was die Baseline sendet.

**Unterstützte Typen in v1:** `Bool, Int, Float, String, Vec2, Vec3, Vec4,
Color, Transform, Enum, Struct` und Arrays/Sets/Maps davon. **Nicht `Ref`**:
ein `Ref` ist ein lokaler `InstanceId` (`Value::ref`), der auf dem anderen
Gerät nichts benennt. Eine Netz-Referenz (`netId` statt `InstanceId`) ist ein
späterer Schritt (§9.2 Nr. 5); bis dahin lehnt die Checkbox `Ref`-Variablen
mit einem Tooltip ab. `Enum`/`Struct` setzen dieselbe `TypeRegistry` auf beiden
Seiten voraus, was in einem gepackten Spiel gilt (beide laufen denselben
Build) und im Editor-Host-gegen-gepackten-Client-Fall eine Versionsfrage ist,
die der Hello-Handshake über den Projekt-Id-Vergleich abfängt.

### 6.2 Wire-Format für `Value`

`BitWriter`-Kodierung, rekursiv, mit dem `PinType` als erstem Byte, damit ein
Client einen Wert prüfen kann, bevor er ihn anwendet:

```
Value = type:u8 | flags:u8 (bit0 isArray, bit1..2 container) | payload
  Bool: u8   Int: i32   Float: f32   String: len:u16 + utf8
  Vec2/3/4, Color: N × f32    Transform: 9 × f32
  Enum: typeName:string + i32   Struct: typeName:string + count:u16 + Value…   (Definitionsreihenfolge)
  Array/Set: count:u16 + Value…   Map: count:u16 + (Value key, Value value)…
```

Ein Property-Delta: `kMsgProperties` (Host → Client, `Reliable`, ungeordnet
über Entities, geordnet innerhalb einer Entity durch eine per-Entity
Sequenznummer) = `netId | count:u8 | (propIndex:u8 | Value)…`. `propIndex`
statt Name: die Namen wurden beim Bind/Spawn einmal als Tabelle geschickt
(`kMsgPropertyTable`: `netId | count | name…`), danach kostet jede Property ein
Byte. Ein Client, der einen `propIndex` nicht kennt oder dessen Typ nicht
passt, verwirft das Delta mit einer Log-Zeile (`Cat::Replication`), nie mit
einem Crash.

**Warum reliable, nicht in den Snapshot:** Position ändert sich jeden Tick,
und jeder Snapshot ersetzt den vorigen vollständig, deshalb unreliable. Eine
Property (`health`, `ammo`, `doorOpen`) ändert sich selten und muss
**ankommen**, sonst bleibt eine Tür für einen Spieler zu. Reliable ohne
Ordnung über Entities hinweg, weil Tür A und Tür B einander überholen dürfen;
per Entity geordnet, weil `health = 50` nach `health = 0` falsch ist. Ein
Wert, der sich 30-mal pro Sekunde ändert, gehört nicht in eine replizierte
Variable, sondern in den Snapshot; das Overlay (§8.5) zeigt Bytes pro
Property, damit man das sieht.

**Dirty-Tracking:** der Host vergleicht am Frame-Ende (§5.2) den aktuellen
Wert mit dem zuletzt gesendeten (Vergleich wie der `Equals`-Knoten, für
Container und Structs rekursiv über `items`) und sendet nur Änderungen. Kein Bitfeld pro Frame in `setVariable`,
weil ein Graph eine Variable zehnmal pro Frame setzen darf und nur der
Endwert zählt. Interest-Management gilt auch hier: kein Delta an einen Client,
der die Entity nicht sieht; beim Eintritt in den Radius kommt die Entity als
Baseline.

### 6.3 Autorität, Client-Schreibzugriff, Prediction

E4: nur der Host schreibt. `setVariable` auf einem Client an einer
replizierten Variable **wirkt lokal** (das ist gewollt: ein Client darf
`ammo -= 1` vorhersagen), und das nächste Delta des Hosts überschreibt es.
Damit die Vorhersage nicht flackert, wird ein eingehendes Delta, das dem
lokal vorhergesagten Wert gleicht, nicht als Änderung gezählt (kein `OnRep`).
Einmal pro Variable und Session loggt der Client auf `Debug`, dass er eine
replizierte Variable lokal geschrieben hat, damit ein „warum springt mein
Wert zurück" eine Log-Zeile hat.

Was der Client dem Host mitteilen will, geht als `CallServer`. Die
Anti-Cheat-Regeltabelle (Anti-Cheat-Plan §3.4 „Zielbild: Deklaration wandert
an die Property") wird dafür in v1 **nicht** angebunden, sondern bleibt bei
`anticheat.check` im `CallServer`-Handler (§7.6). Grund: eine Regel an einer
Property nur für Host-Schreibzugriff prüft nichts (der Host lügt nicht gegen
sich selbst), und Client-Schreibzugriff gibt es per E4 nicht. Das Zielbild
bleibt für den Tag stehen, an dem Client-Ownership von Properties kommt (§9.2
Nr. 6).

### 6.4 OnRep: die Benachrichtigung

Konvention statt neuer Event-Typ, in allen vier Frontends gleich benannt:

| Frontend | Wird gerufen, wenn vorhanden | Argument |
|---|---|---|
| HorizonCode | Funktion `OnRep_<Variable>` in derselben Klasse (`Runtime::callFunction(inst, "OnRep_Health", requirePublic = false, {old})`) | ein Parameter vom Typ der Variablen: der **alte** Wert; der neue steht schon in der Variablen |
| Lua | `onRep_health(self, old)` | dito |
| Python | `on_rep_health(self, old)` | dito |
| C++ GameLogic | `IGameLogic::onRep(uint32_t entity, const char* name)` + `getVar` | kein alter Wert (JSON-Grenze; wer ihn braucht, merkt ihn sich) |

Die Checkbox **Notify** in der Variablen-Liste legt in HorizonCode die
Funktion `OnRep_<Name>` mit dem passenden Parameter an (wie ein
Rechtsklick-„Add Function"), damit man sie nicht von Hand tippen muss; ohne
Häkchen wird nicht gerufen, auch wenn eine gleichnamige Funktion existiert
(explizit ist billiger als Magie beim Umbenennen). Für Text-Skripte gilt
„vorhanden = wird gerufen", wie bei jedem anderen Hook dort.

Zustellung: gesammelt in `pump()`, ausgeführt nach `pump()` und vor der
Skript-Tick-Phase (§5.2), pro Entity in Property-Reihenfolge. **Auf dem Host
wird `OnRep` nicht gerufen**, wie in Unreal: der Host hat den Wert selbst
gesetzt und weiß es. Ein Graph, der auf beiden Seiten reagieren will, ruft
seine `OnRep_X` nach dem Set selbst; die Docs sagen das an der Checkbox.

Beim Spawn und bei der Baseline feuert `OnRep` **einmal pro Property**, nach
BeginPlay der Klasse, mit dem Default als `old`. Sonst sähe eine Tür, die
schon offen war, als der Spieler kam, nie den Grund, ihre Animation zu setzen.

---

## 7. (d) RPC: `CallServer`, `CallClient`, `CallAllClients`

### 7.1 Die `net`-Gruppe in `HE::api`

Nach dem Dreiklang aus Memory `engine-api-row-three-places`: Rows in
`registry()`, Display-Name-Map, `HcNodeDocs`, `isScriptGroup` (+
Parity-Fixture in `HCGEN_CLASSES`). `Ctx` bekommt `NetGameSession* net`
(am **Ende** der Struct, positional-init-Regel aus dem `Ctx`-Kommentar); null
= kein Netz, jede Row liefert ihren neutralen Wert: `isAuthority` **true**
(ein Einzelspielerspiel ist seine eigene Autorität, damit ein Graph, der
`isAuthority` prüft, offline genau so läuft), `isClient` false, `localPlayer`
1, `players` leer, jede exec-Row no-op.

Diese Gruppe **ersetzt** die Skizze aus dem Anti-Cheat-Plan §6.2.2
(`sendToHost`, `sendToClient`, `OnNetMessage`): dieselbe Absicht, mit Namen,
die der Vorgabe folgen, und typisiert statt Payload-Blob.

| Id | Exec? | Params → Results | Bedeutung |
|---|---|---|---|
| `net.host` | exec | port: Int, displayName: String → ok: Bool | Session eröffnen (Optionen aus Project Settings) |
| `net.join` | exec | sessionId: String, joinCode: String, displayName: String → ok: Bool | über das Directory |
| `net.joinLan` | exec | index: Int, joinCode: String, displayName: String → ok: Bool | aus `lanSessions` |
| `net.joinDirect` | exec | host: String, port: Int, joinCode: String, displayName: String → ok: Bool | |
| `net.leave` | exec | | |
| `net.status` | pure | → Int | 0 Idle, 1 Hosting, 2 Connecting, 3 Joined, 4 Failed |
| `net.sessionId`, `net.joinCode` | pure | → String | zum Anzeigen; **`joinCode` nur auf dem Host** |
| `net.lanSessionCount`, `net.lanSessionName(i)`, `net.lanSessionPlayers(i)` | pure | | Lobby-Liste |
| `net.refreshLan` | exec | | Browser neu starten |
| `net.isAuthority`, `net.isClient` | pure | → Bool | E3: nie `isHost` prüfen, immer Autorität |
| `net.localPlayer` | pure | → Int | PlayerId |
| `net.playerCount`, `net.playerAt(i)`, `net.playerName(p)` | pure | | Roster |
| `net.ownerOf` | pure | entity: Int → Int | PlayerId, 0 = Host/keiner |
| `net.isLocallyControlled` | pure | entity: Int → Bool | |
| `net.ping` | pure | player: Int → Float | ms, aus `srttMs` |
| `net.kick` | exec | player: Int | Host; ruft `anticheat.kick` mit reasonCode 0 (ein Pfad) |
| `net.declareVar*` | exec | entity: Int, name: String, initial: (je Typ) | Text-Frontends (§6.1); eine Row pro Typ, siehe unten |
| `net.setVar*` / `net.getVar*` | exec / pure | entity: Int, name: String, value / → value | dito |
| `net.callServer` | exec | entity: Int, function: String | **HC-Form**, siehe §7.2: Argumente kommen nicht über diese Row |
| `net.callClient` | exec | player: Int, entity: Int, function: String | dito |
| `net.callAllClients` | exec | entity: Int, function: String | dito |

`declareVar/setVar/getVar` mit einem typisierten Wert sind in der Registry ein
Problem derselben Art wie die RPC-Argumente: ein Pin hat einen Typ. Lösung wie
bei `save::setNumber/setString/setBool/setStructV`: **je Typ eine Row**
(`net.setVarNumber`, `setVarString`, `setVarBool`, `setVarVec3`,
`setVarStruct`), dazu `getVar*`. Für HorizonCode sind diese Rows ohnehin
zweitrangig (dort ist die Checkbox der Weg); für Lua/Python macht der
generische Dispatcher aus dem Lua-Typ die richtige Row, so dass ein Skript
`horizon.net.setVar(self, "health", 90)` schreibt.

### 7.2 Wie Argumente reisen

**HorizonCode: Run-On am `FunctionEntry`.** `Node` bekommt `runOn: u8` (0
Local, 1 Server, 2 OwningClient, 3 AllClients), editierbar im
Funktions-Header wie `access`/`overridable`. Ein `FunctionCall` einer Funktion
mit `runOn ≠ Local` wird vom Runtime **nicht** lokal ausgeführt (außer die
Seite ist das Ziel), sondern an den `RpcRouter` gegeben: Klasse-Instanz →
Entity → `netId`, Funktionsname, die Parameter als `Value`-Liste (§6.2-Format).
Ergebnisse gibt es nicht (`FunctionReturn` einer Run-On-Funktion ist ein
Validierungsfehler im Editor): ein RPC ist fire-and-forget, wie in Unreal.
Auf der Zielseite: `Runtime::callFunction(inst, name, requirePublic = false,
args)`.

Die `net.call*`-Rows aus 7.1 sind damit für HorizonCode die **explizite**
Form ohne Argumente („ruf `Respawn` auf dem Server"); die implizite Form ist
der Modus am Funktionskopf. Beide erzeugen dieselbe Nachricht.

**Lua / Python: variadisch.** `horizon.net.callServer(self, "takeDamage",
40, attackerId)` und `horizon.net.callClient(player, self, "showHit", 40)`.
Der generische Dispatcher wandelt Lua-/Python-Werte in `Value`s (das kann er
schon für jede Row); auf der Zielseite ruft `ScriptContext` die Methode
`takeDamage(self, 40, attackerId)`. Kein Registrieren nötig: „vorhanden = wird
gerufen", wie bei jedem Hook.

**C++ GameLogic: JSON.** `HeNetServices::callServer(host, entity, name,
argsJson)` und `IGameLogic::onRpc(entity, name, argsJson)`, JSON-Array von
`valueToJson`-Werten. Dieselbe Grenze wie `setStructJson`.

**Cross-Frontend:** ein Lua-Skript kann auf dem Client `callServer(self,
"Open")` rufen und auf dem Host ist auf derselben Entity eine
HorizonCode-Klasse mit Funktion `Open`. Der Router kennt nur Entity + Name
und fragt der Reihe nach: HC-Instanz auf der Entity (`EntityHost::instanceOf`),
Lua/Python-Instanz, `IGameLogic`. Der erste, der die Funktion hat, bekommt
den Aufruf; keiner → Log-Zeile mit Entity und Name, kein Fehler.

### 7.3 Nachricht und Zustellung

`kMsgRpc` (`ReliableOrdered`, in beide Richtungen): `netId:u32 | target:u8
(1 Server, 2 OwningClient, 3 AllClients) | fromPlayer:u32 | name:string |
argc:u8 | Value…`. Auf dem Host wird `fromPlayer` **aus der Verbindung**
gesetzt, nie aus der Nachricht gelesen (dieselbe Regel wie Beacon-Adresse
und `REMOTE_ADDR`). `CallClient` und `CallAllClients` vom Client sind
ungültig: der Host verwirft sie mit `Hard`-Observation, wie Eingaben für
fremde Entities.

Reihenfolge: RPCs derselben Richtung sind geordnet (ein `Open` nach `Close`
kommt als `Open` nach `Close`). Gegen Property-Deltas sind sie **nicht**
geordnet (anderer Kanal); ein RPC, der von einer Property abhängt, die im
selben Frame gesetzt wurde, sollte den Wert als Argument tragen. Das steht in
den Docs an `runOn`.

Zeitpunkt (§5.2): `CallServer` **vor** der Simulation des Frames (es ist eine
Eingabe), `CallClient`/`AllClients` **nach** `pump()` vor dem Skript-Tick.

### 7.4 Lebenszyklus-Events

Nach dem Vierteiler mit den fünf Stationen aus `AntiCheatEvents::dispatch`
(Game Instance, Level Script, Klasse auf der betroffenen Entity, alle
Lua/Python-Instanzen, `IGameLogic`), über einen `NetEvents::dispatch` daneben:

| Event | Pin | Feuert wo |
|---|---|---|
| `OnPlayerJoined` | player: Int | Host, nach Baseline |
| `OnPlayerLeft` | player: Int | Host |
| `OnConnected` | | Client, nach Baseline (`Joined`) |
| `OnDisconnected` | reason: Int (0 Leave, 1 Timeout, 2 Kicked, 3 Rejected, 4 VersionMismatch, 5 WrongProject) | Client |
| `OnSessionStarted` | | Host, nach `host()` |
| `OnSessionEnded` | | beide |

Event-Tabelle `HorizonCode.cpp:2526`, `kEvents` der Game Instance in
`LevelScriptPanel.cpp:1446`, `Runtime::fireOn…`, `HorizonCodeCompiled.h`
virtuelle Methoden, `IScriptBackend::callOn…` mit Default-No-Op,
`IGameLogic`-Hooks. Der Kick-Grund läuft weiter über `OnCheatDetected` mit
dem Client-Ticket (Anti-Cheat-Plan §5.5); `OnDisconnected(2)` kommt danach.

### 7.5 Die eine Konvention für Graphen, die auf beiden Seiten laufen

Weil eine Klasse auf Host **und** Client instanziiert wird (Spawn, §5.5),
läuft ihr BeginPlay zweimal, auf zwei Geräten. Die Regel, die die Docs an
`Replicates` und an `runOn` wiederholen:

> **Simulation nur mit `Is Authority`. Darstellung überall. Absichten per
> `CallServer`.**

Ein Beispiel-Graph in der Doku (Tür): `OnInteract → CallServer "Open"`;
`Open [Run On: Server] → Is Authority? → Set doorOpen = true`;
`OnRep_doorOpen → Play Animation`. Auf dem Host setzt der Graph direkt und
spielt die Animation aus `OnRep` nicht (§6.4), also ruft er sie nach dem Set;
die Doku zeigt beide Zweige. Das ist der Preis der Unreal-Semantik, und er
ist bekannt.

### 7.6 Sicherheit: ein RPC ist ein Anspruch

Auf dem Host, in dieser Reihenfolge, **vor** der Zustellung:

1. `netId` bekannt und Funktion existiert (sonst Log, verwerfen).
2. **Owner-Check:** `CallServer` ist nur erlaubt an Entities, die der Spieler besitzt (`owner == fromPlayer`), **oder** an Entities, deren Klasse die Funktion als `runOn = Server` mit `anyClient = true` markiert (zweites Häkchen am Funktionskopf, Default aus). Eine Tür gehört niemandem, also braucht `Open` das Häkchen; das ist gewollt, damit „jeder Client darf das aufrufen" eine sichtbare Entscheidung ist.
3. **Rate:** `kMaxRpcPerSecond = 60` pro Verbindung über ein 2-s-Fenster (dieselbe Fenster-Lehre wie das dt-Budget), darüber `Observation` mit Gewicht wie die Input-Rate.
4. **Format:** `argc` und Typen gegen die Funktionssignatur; Mismatch = `Hard` (kein legitimer Client erzeugt das).
5. Zustellung. Was der Handler dann mit `anticheat.check("Damage", amount, player)` tut, ist Spielsache; die Engine hat ihm `player` als `fromPlayer` schon in `net.rpcSender()` (pure Row, gültig während der Zustellung) hingelegt.

---

## 8. (e) Editor-Bedienung

### 8.1 Die Kategorie „Replication" mit dem einen Schalter

Heute: `componentHeader("Network", …)` erscheint nur, wenn die Entity ein
`NetworkComponent` hat (`InspectorPanel.cpp:2113`), hinzugefügt über das
Komponenten-Menü (`addRow<NetworkComponent>("Network")`, `:3425`). Ziel: **jede
Entity** hat die Kategorie, und ein Häkchen reicht.

```
▾ Replication
   [x] Replicates                      ← der Schalter
       Relevance Radius   150 m        ← die heutigen Zeilen, nur sichtbar wenn an
       Replicate Transform [x]
       Max Speed           0.0 m/s  (Max Speed 0: checked against Movement's 6.0 m/s.)
       Max Vertical Speed  0.0 m/s
       Replicated Variables: 3 (Health, Ammo, DoorOpen)   ← Lesehinweis aus der Klasse, §8.2
```

Mechanik: `NetworkComponent` bekommt `bool replicates = true`. Der Schalter
liest `nc && nc->replicates`; Anschalten ohne Komponente emplaced eine mit
Defaults (Undo-Snapshot wie `componentHeader`s Remove); Ausschalten setzt
`replicates = false` und **behält** die Komponente, damit Radius und Limits
überleben (ein zweiter Klick stellt alles wieder her). Der Menü-Eintrag
„Network" im Komponenten-Menü entfällt; der Serializer-Key `"network"`
bleibt, mit dem neuen Feld (Default `true` ⇒ alte Szenen replizieren
weiter, unbekannter Key wird ignoriert: Versionsvertrag wie überall).

Was der Schalter intern anschaltet, in einem Satz für den Tooltip: „Diese
Entity wird beim Session-Start registriert, ihr Transform per Snapshot
gesendet, ihre als Replicated markierten Variablen synchronisiert, und ihre
Run-On-Funktionen über das Netz gerufen." Das ist die Vorgabe „ein Schalter,
der intern alles Nötige anschaltet", wörtlich.

Tooltips und Handbuch-Einträge nach der 715/715-Regel (Memory
`in-engine-docs-and-tooltips`), inklusive der neuen Variablen- und
Funktionskopf-Häkchen.

### 8.2 Variablen-Liste und Funktionskopf im HorizonCode-Editor

- Variablen-Zeile: Checkbox **Replicated**, daneben **Notify** (nur wenn
  Replicated). `Ref`-Typ: Checkbox deaktiviert mit Tooltip (§6.1). Notify
  anhaken legt `OnRep_<Name>` an (§6.4).
- Funktionskopf: Combo **Run On** (Local / Server / Owning Client / All
  Clients) neben Public/Private und Overridable; bei Server zusätzlich
  Checkbox **Any Client May Call** (§7.6). Eine Run-On-Funktion mit Rückgabe
  bekommt die rote Validierungszeile, die der Editor für andere Graphfehler hat.
- Node-Palette: Gruppe „Engine · Net" mit den Rows aus 7.1, wie jede Gruppe.

### 8.3 Play-Toolbar

Neben ▶: Dropdown **Play as Host** / **Join…** (§5.6). Beim Host-Start
erscheint der Join-Code wie bei Collab in der Statuszeile, kopierbar. Der
Editor setzt `NetGameSession::HostOptions::publishDirectory = false` per
Default (PIE ist lokal), LAN-Announce an.

### 8.4 Project Settings ▸ Game ▸ Multiplayer

Neue Struct `ProjectMultiplayerSettings` mit dem Versionsvertrag von
`ProjectSettings` (Default = heutiges Verhalten = kein Netz):

```json
"multiplayer": {
  "defaultPort": 47824,
  "maxPlayers": 8,
  "tickHz": 30,
  "worldExtent": 4096.0,
  "discovery": { "lan": true, "directory": true, "portMapping": true },
  "rpcPerSecond": 60,
  "timeoutSec": 5.0
}
```

Page `Page::Multiplayer` unter Game in `ProjectSettingsPanel.cpp`, drei
`SeparatorText`-Blöcke (Session, Replication, Discovery). `worldExtent` und
`tickHz` landen in `GameReplication::Config`; `47824` ist `LanBeacon::kPort + 1`,
damit die beiden nebeneinander liegen und eine Firewall-Regel beide nennt.

### 8.5 Diagnose-Overlay

`Debug ▸ Network Stats` (Editor) und `debug.netStats()` (Row): Rolle,
Spieler, Ping, Verlust im Fenster, Bytes/s gesendet und empfangen aufgeteilt
nach Snapshot / Properties / RPC / Reliability-Overhead, und die drei
teuersten Properties nach Bytes. Das ist der Ort, an dem „meine Variable
gehört in den Snapshot" (§6.2) sichtbar wird, und der einzige Ort, an dem
`UdpTransport::Stats` je jemand sieht.

### 8.6 Stand nach Schritt 5b

Umgesetzt: die Kategorie, die Seite, das Overlay und, was in der
Schrittbeschreibung nicht stand, die Verdrahtung der Seite in die Session.

**Inspector.** `componentHeader("Replication", …)` wird jetzt für **jede**
Entity gezeichnet, nicht nur für die mit `NetworkComponent`; der Schalter
`Replicates` legt die Komponente beim Anschalten mit Defaults an und lässt sie
beim Ausschalten stehen (`replicates = false`), mit einer Zeile darunter, die
das sagt. Der Menü-Eintrag „Network" ist weg. Die Label→Scene-Key-Tabelle in
`InspectorPrefabKeys.cpp` bildet „Replication" weiter auf `"network"` ab: der
Scene-Key umzubenennen hätte jede `.hescene` auf der Platte verwaist.

In den stillen Modi (`listComponents`, `removeComponent`) verhält sich die
Sektion wie jede andere und meldet sich nur, wenn die Komponente da ist —
sonst hätte der Komponentenbaum des Klassen-Tabs bei jeder Entity
„Replication" gelistet, und die Kategorie, die jeder angeboten bekommt, wäre
zu einer Komponente geworden, die jeder besitzt.

**Project Settings.** `ProjectMultiplayerSettings` in HE_Core, Page
`Page::Multiplayer` mit den drei Blöcken aus §8.4. Die JSON-Form weicht in
einem Punkt von §8.4 ab: die vier Prediction-Grenzen der Schrittbeschreibung
(`interpolationDelaySec`, `reconcileSnapDistance`, `reconcileSmoothing`,
`maxPendingInputs`) stehen unter einem eigenen `"prediction"`-Objekt, weil
sie zusammen gelesen werden und einzeln nichts heißen.

**Und wer das liest.** Eine Seite, deren Werte niemand liest, ist ein
Versprechen ohne Deckung, deshalb ist die Verdrahtung Teil dieses Schritts:
`NetGameSession::setProjectDefaults` + `defaultHostOptions()` /
`defaultJoinOptions()` sind der eine Ort, an dem die Seite auf die Session
trifft, und **alle** Einstiege gehen darüber — `net.host`/`joinDirect`/
`joinLan` in `EngineApi.cpp`, Play as Host im Editor, `--host`/`--join` im
gepackten Spiel. Gelesen werden damit: Port, Plätze, Timeout, Tick,
Weltausdehnung und die vier Prediction-Grenzen.

**Nicht** gelesen werden `discovery.directory`, `discovery.portMapping` (beide
Schritt 5c) und `rpcPerSecond` (Schritt 7). Sie werden gespeichert, und die
Hilfe-Einträge sagen wörtlich „stored, but nothing reads it yet" — ein
Schalter, der still nichts tut, ist schlimmer als kein Schalter.

Zwei Werte gehen bewusst **nicht** in die Seite: `positionBits`/`rotationBits`
und `snapshotBudgetBytes`. Das sind Wire-Format-Entscheidungen, auf die sich
beide Enden byteweise einigen müssen; ein Projekt, das daran drehen kann,
erzeugt zwei Builds desselben Spiels, die inkompatibel sind, ohne dass es
irgendwo auffällt. Ein Test hält das fest.

**Overlay.** Die vier Zeilen (Rolle, Spieler, Ping, Verlust) hängen an der
bestehenden Stats-Überlagerung des Viewports (Show ▸ Stats) statt an einem
neuen Fenster: das ist die Ecke, in die beim Spielen ohnehin geschaut wird.
Ping und Verlust kommen über `NetGameSession::linkStats()` aus dem
`UdpTransport` und **fehlen** (statt 0 zu sein), wenn darunter kein Socket
liegt — bei einem injizierten Transport gibt es keine Laufzeit, und eine 0
läse sich dort als perfekte Verbindung. Das Voll-Overlay aus §8.5 (Bytes/s
nach Nachrichtenart, die teuersten Properties) kommt mit Schritt 6, wo es
etwas zu zeigen gibt.

**Belegt durch:** `editor_help_audit --check` 974/974 in allen elf Bereichen 0
offen (13 neue Einträge für die Seite, die vier `Network/`-Keys auf
`Replication/` umbenannt samt `kComponentScopes` — ohne das wäre der Bereich
`components` von 321/321 auf 316 gefallen); drei Inspector-Tests headless
(Kategorie ohne Komponente da, ein Klick legt sie an, Ausschalten behält den
Radius, `listComponents` nennt sie nur wo eine ist); fünf
`ProjectMultiplayerSettings`-Tests (Defaults = heutiges Verhalten, Rundreise,
fehlender Schlüssel, clamp mit Port 0 als echter Antwort, echter
Save/Load); ein `net session`-Test für die beiden Fabriken inklusive der
Negativaussage über das Wire-Format.

---

## 9. Nicht-Ziele und offene Fragen

### 9.1 Nicht-Ziele, bewusst

- **Dedizierter Server.** `NetRole::Server` bleibt ein Enum-Wert. Alles
  hier ist Listen-Server; `isAuthority()` hält die Tür offen (E3). Ein
  fensterloser `GameApplication`-Modus ist ein eigenes Thema mit eigenen
  Fragen (Headless-Renderer-Stub, Input-Quelle, Prozessverwaltung).
- **NAT-Traversal, Relay, Matchmaking, Steam/EOS.** Ein Host hinter NAT
  braucht die Portfreigabe oder LAN. STUN/ICE/TURN sind ein Thema mit
  Infrastruktur, das dieses nicht hat.
- **GameNetworkingSockets** (E1).
- **Lag-Compensation / Server-Rewind** („Treffer zum Zeitpunkt des Schusses
  aus Sicht des Clients"). Setzt Positions-History pro Entity auf dem Host
  voraus; nachrüstbar über `GameReplication`, nicht v1.
- **Prediction für Nicht-Transform-Zustand** über das lokale Schreiben
  hinaus (§6.3). Kein Rollback von Variablen.
- **Delta-Kompression der Snapshots gegen eine Baseline** (Quake-3-Stil).
  Bandbreite bei 30 Hz und ≤ 64 Entities ist ohne das im Rahmen; das Overlay
  sagt, wann nicht.
- **Voice, Text-Chat.** Ein Chat ist ein `CallAllClients` mit einem String
  und gehört ins Spiel, nicht in die Engine.
- **Additive Zonen im Multiplayer** (§5.5).
- **Rejoin mit Zustandserhalt** (`keepCharacterOnLeave`).
- **Client-Ownership von Properties** (Client schreibt, Host validiert per
  Regel). Das Zielbild aus dem Anti-Cheat-Plan §3.4 bleibt stehen, kommt
  aber nach v1.
- **Verschlüsselung optional machen.** `requireEncryption` bleibt `true`;
  ein Spiel über das Internet ohne GCM gibt es nicht.

### 9.2 Offene Fragen

1. **Dedizierter Server: wann?** Empfehlung: erst wenn ein Projekt mehr als
   ~8 Spieler oder Host-Neutralität braucht. Der Plan verbaut nichts.
2. **UDP-Port-Prüfung im Directory.** Self-Probe per Hairpin (§5.3) ist die
   billige Variante. Die robuste wäre ein kleiner UDP-Echo-Dienst neben
   `session-api.php`, und Shared Hosting kann keinen UDP-Socket halten.
   Empfehlung: Hairpin + ehrliche UI, Echo-Dienst nur, wenn ein Projekt es
   braucht.
3. **Wire-Kompatibilität Editor-Host ↔ gepackter Client.** `TypeRegistry`
   und Klassen müssen gleich sein. Der Projekt-Id-Vergleich fängt das falsche
   Projekt, nicht den falschen Stand desselben Projekts. Ein Content-Hash
   (das Integritäts-Manifest gibt es schon) im Hello würde beides fangen;
   Empfehlung: in Schritt 4 den Manifest-Vergleich als „Version" wiederverwenden,
   Mismatch = Warnung, nicht Reject, weil Editor-Host gegen gepackten Client
   der normale Testfall ist.
4. **Zwei Welten in einem Prozess** (§5.6 Nr. 2). Bester Workflow, teuerster
   Umbau. Empfehlung: nach v1, wenn der Zwei-Prozess-Weg sich als zu träge
   erweist.
5. **Netz-Referenzen** (`Ref` über das Netz als `netId`). Braucht eine
   `Value`-Variante oder ein Mapping im Runtime. Empfehlung: eigener Schritt
   nach v1; bis dahin reisen Entity-Ids als Int (`netId`), und `net.entityOf(netId)`
   löst sie auf.
6. **Client-Ownership von Properties** (siehe Nicht-Ziele). Wann: wenn ein
   Spiel eine Property hat, die zu häufig für `CallServer` ist und zu
   wichtig für den Snapshot. Bis dahin: nicht.
7. **Tick-Nummer im Snapshot und Reorder** (§4.4): reicht „älter verwerfen",
   oder soll der Interpolations-Puffer umsortieren? Empfehlung: verwerfen;
   bei 30 Hz und Reorder-Abstand 1 kostet das einen Sample von 33 ms, den die
   Interpolation überbrückt.
8. **`kMaxFrameSize` für Reliable-Nachrichten über UDP.** ~300 KiB durch
   `count:u8`-Fragmente. Die Baseline einer Szene mit 2000 replizierten
   Entities sprengt das. Empfehlung: Baseline in Stücke von 64 Entities
   senden (mehrere Nachrichten, ohnehin geordnet), dann braucht nichts
   Größeres als das Manifest je ein Fragment-Bündel.

---

## 10. Integration: was ein Entwickler tut

Der Maßstab, an dem der Plan gemessen wird.

### 10.1 Minimalpfad (Türen, Kisten, alles Statische mit Zustand)

1. Entity auswählen, **Replication ▸ Replicates** anhaken.
2. In der Klasse die Variable `doorOpen` als **Replicated** + **Notify**
   markieren; im erzeugten `OnRep_doorOpen` die Animation setzen.
3. Funktion `Open` mit **Run On: Server** und **Any Client May Call**; darin
   `Set doorOpen = true` (+ Animation für den Host).
4. Im Interaktions-Event `CallServer "Open"` (oder einfach `Open` rufen; der
   Modus routet).

Kein `registerEntity`, kein `adoptEntity`, keine Message-Id, kein `isHost`.

### 10.2 Spielerfigur

Im Graph nichts Neues gegenüber heute: der Controller spawnt den Charakter
aus BeginPlay und ruft `player.possess`. Auf dem Host passiert das für jeden
Spieler, die Engine registriert und weist zu; auf dem Client ist derselbe
`Create Object` ein No-op und der Charakter kommt per Spawn-Nachricht (§5.4,
Punkte 2 und 3). Was die Engine dafür intern umbaut (Input-Routing nach
Besitzer im `PlayerHost`), sieht der Graph nicht. Die Klasse der Figur
hat **Replicates** an (oder wird es per Spawn, wenn die Klasse es in ihren
Components-Tab hat: `EntityHost::spawn` liest die Komponentenliste der Klasse,
`NetworkComponent` reist darin wie jede andere Komponente).

### 10.3 Session starten, aus dem Menü-Widget

```lua
-- Lua, Game-Instance-Skript
function onHostClicked(self)  horizon.net.host(0, self.playerName) end
function onJoinClicked(self)  horizon.net.join(self.sessionField, self.codeField, self.playerName) end
function onPlayerJoined(self, player)
    horizon.log.info(horizon.net.playerName(player) .. " joined")
end
```

### 10.4 Was ein Entwickler NICHT tut

- keinen Transport wählen, keine Ports außer dem einen in den Settings
- keine Nachrichten definieren, keine Serialisierung schreiben
- keine Ids vergeben oder spiegeln
- kein Anti-Cheat anschließen (es hängt am Session-Start mit dran)
- nichts auf dem Client anders bauen als auf dem Host, außer der einen
  Konvention aus §7.5

---

## 11. Umsetzungs-Roadmap

Schritt 1 ist dieses Dokument. Jeder weitere Schritt ist für sich mergebar,
hat einen Test, der ohne Netzwerk-Hardware läuft, und nennt, was er **nicht**
tut. Die Reihenfolge ist durch Abhängigkeiten festgelegt, nicht durch
Sichtbarkeit: der erste sichtbare Knopf kommt in Schritt 5b.

### 11.1 Schritte

| # | Inhalt | Testbar über | Voraussetzung | Tut NICHT |
|---|---|---|---|---|
| 2 | **`UdpTransport`** (Cookie-Handshake, drei Kanäle, Ack-Bitfeld, RTO, Fragmentierung, Keepalive/Timeout, Stats, Test-Drop-Hook) + **`LossyTransport`** + **`SecureTransport::replayWindow`** + `PortMapper::Protocol` + `networking-layer-design.md`-Korrektur (GNS-Reservierung) | `test_net_udp.cpp` (Localhost-Sockets wie `test_net_tcp`), `test_net_lossy.cpp`, neue Fälle in `test_net_secure.cpp` | keine | nichts in HE_Scene; kein Konsument wechselt den Transport |
| 3 | **`GameReplication` über UDP fit:** Tick-Nummer im Snapshot (ältere verwerfen), `test_game_replication` zusätzlich über `LossyTransport` (Verlust 10 %, Reorder 20 %, Dup 5 %) mit Prediction/Reconcile-Negativkontrollen; Manifest-Größe gegen Fragmentierung geprüft | `test_game_replication.cpp` | 2 | keine neuen Nachrichten |
| 4 | **`NetGameSession`** (Host/Join/Leave, Roster, Hello/Welcome, Projekt-Id, Version), **`SpawnReplicator`** (Bind/Spawn/Despawn/Baseline), `NetworkComponent::replicates` + Registry-Walk, `AntiCheatHost::attach` am Session-Start, `Cat::Replication`, Beacon-`kind`, `Ctx::net` (Directory bleibt bis 5c auf dem heutigen Stand: Registrierung funktioniert, `reachable` wird ignoriert) | `test_net_game_session.cpp`: zwei Sessions über `LossyTransport`, headless; Late-Join mit 3 Spawns; Despawn; Kick-Pfad des Anti-Cheat nun **erstmals in einer Session** | 2, 3 | keine Frontends, kein Inspector, keine Properties, kein RPC |
| 5 | **Verdrahtung + `net`-Rows + Lebenszyklus-Events:** `GameApplication` pumpt und bindet (`Ctx::net`), `--host/--join`; `EditorApplication` Play as Host / Join… (Toolbar-Menü), PIE ohne Kick/Telemetrie; `PlayerHost`-Umbau nach §5.4 (Besitzer pro Controller, Spawn-No-op auf Clients, Possess auf dem Client); `net.host/join/joinLan/joinDirect/leave/status/…/isAuthority/localPlayer/players/ping/kick`-Rows (Dreiklang + `HCGEN_CLASSES`); `OnPlayerJoined/Left/OnConnected/OnDisconnected/OnSession*` durch alle vier Frontends (`NetEvents::dispatch`) | `test_engine_api`, `test_hc_node_docs`, `test_scripting_binding`, `test_python_scripting`; **erster Zwei-Prozess-Durchlauf von Hand** (Editor-Host, `--join`-Client), Ergebnis als Devlog | 4 | kein Inspector, keine Settings-Page, keine Properties, kein RPC |
| 5b | **Editor-Bedienung:** **Inspector-Kategorie Replication** mit Schalter (Serializer-Feld, Menü-Eintrag weg), Project Settings Page Multiplayer (`ProjectMultiplayerSettings`), Tooltips + Handbuch für beides, erste Overlay-Zeilen (Rolle, Spieler, Ping, Verlust) | Panel-Audit 715/715, Inspector headless (`imgui-allow-overlap-row-buttons`-Rezept), `test_project_settings` | 4 (Session), unabhängig von 5 | keine Variablen-Checkboxen, kein Funktionskopf |
| 5c | **Website/Directory:** `session-api.php` `kind` + `transport`, Directory-Client-Felder, Hairpin-Self-Probe mit ehrlicher UI, Deploy (mit Bestätigung) | `test_net_directory` (Mock), Hand-Test gegen die echte Website | 4 | |
| 6 | **`PropertyReplicator`:** `Variable::replicated/repNotify`, Dirty-Tracking, `kMsgPropertyTable/Properties`, `Value`-Wire-Format, `ReplicatedVarsComponent` + `net.declareVar/setVar*/getVar*`, `OnRep_<Var>` in allen vier Frontends, Baseline-`OnRep`, Variablen-Liste im HC-Editor (Checkboxen, Ref-Sperre, Notify-Generator), Overlay-Zeilen | `test_net_property_replication.cpp` über `LossyTransport`: Wert kommt an, `OnRep` einmal, alter Wert korrekt, Client-Schreibzugriff wird überschrieben, Typ-Mismatch verworfen ohne Crash, Struct/Enum/Map rund; `test_scripting_binding`, `test_python_scripting` für `onRep_*` | 4, 5, 5b | kein RPC |
| 7 | **`RpcRouter`:** `Node::runOn` + `anyClient`, Router-Umleitung im Runtime, `kMsgRpc`, Owner/Rate/Format-Checks als Observations, variadische Lua/Python-Rows, `HeNetServices` + `IGameLogic::onRpc/onRep`, `net.call*`-Rows, `net.rpcSender`, Funktionskopf-UI + Validierung, Beispiel-Graph Tür in der In-Engine-Doku | `test_net_rpc.cpp`: CallServer vom Owner ok, vom Fremden `Hard`, `anyClient` erlaubt, Rate-Fenster, Format-Mismatch, Reihenfolge unter Reorder, Cross-Frontend Lua→HC | 5, 6 | keine Rückgabewerte |
| 8 | **Abschluss:** Overlay komplett, Memory `networking-layer.md` und Website-Roadmap (`roadmap.json` + `deploy.py`, mit Bestätigung), Devlog, Release-Build mit Zwei-Geräte-Test über LAN **und** über Directory + Portfreigabe (echte Router, siehe `port-forward-refusal-vs-absence`) | Hand-Test mit Protokoll, Log-Sink-Test „kein Join-Secret im Log" | 7 | |

### 11.2 Checkliste berührter Dateien

**Schritt 2 (HE_Net):**
- `src/HE_Net/include/Net/UdpTransport.h` + `src/UdpTransport.cpp` (neu, `HE_NET_API`)
- `src/HE_Net/include/Net/LossyTransport.h` + `.cpp` (neu)
- `src/HE_Net/include/Net/SecureTransport.h` (`Config::replayWindow`) + `.cpp` (Fenster statt `failPeer`)
- `src/HE_Net/include/Net/PortMapper.h` + `.cpp` (`Protocol`-Parameter, UPnP/PCP)
- `src/HE_Net/include/Net/ITransport.h` (Kopfkommentar: GNS-Zeile raus)
- `docs/networking-layer-design.md` (Zeilen 41, 95, 159)
- `tests/test_net_udp.cpp`, `tests/test_net_lossy.cpp`, `tests/test_net_secure.cpp`, `tests/CMakeLists.txt`

**Schritt 3, 4 (HE_Scene):**
- `src/HE_Scene/include/HorizonScene/GameReplication.h` + `.cpp` (Tick-Nummer, `kMsg*` für Bind/Spawn/Despawn ggf. hier oder in `SpawnReplicator`)
- `src/HE_Scene/include/HorizonScene/Net/NetGameSession.h` + `.cpp`, `SpawnReplicator.h` + `.cpp`, `PlayerRoster.h` (neu, **kein `HE_API`**)
- `src/HE_Scene/include/HorizonScene/Components/NetworkComponent.h` (`replicates`), `SceneSerializer.cpp` (`:288`, `:1069`, `:2549`)
- `src/HE_Net/include/Net/LanBeacon.h` + `.cpp` (`kind`), `SessionDirectory.h` + `.cpp`
- `src/HE_Core/include/Diagnostics/Log.h` + `.cpp` (`Cat::Replication` vor `Count`, `kCategoryNames`)
- `src/HE_Scene/include/HorizonScene/EngineApi.h` (`Ctx::net` am Ende)

**Schritt 5 (Verdrahtung, `net`-Rows, Events):**
- `src/HE_Game/src/GameApplication.cpp` (Session, Frame-Reihenfolge, `launchArguments`), `EditorApplication.cpp` (PIE-Variante, Toolbar-Menü), `EditorApplication.h`
- `src/HE_Scene/include/HorizonScene/PlayerHost.h` + `.cpp` (Besitzer pro Controller, Input-Routing), `EntityHost.h` + `.cpp` (Spawn mit `owner`)
- `src/HE_Scene/include/HorizonScene/EngineApi.h` + `EngineApi.cpp` (`namespace net`, Rows, Display-Name-Map, `isScriptGroup`), `src/HE_Editor/HcNodeDocs.cpp`, `tests/CMakeLists.txt` `HCGEN_CLASSES`
- `src/HE_Core/src/HorizonCode/HorizonCode.cpp` (Event-Tabelle `:2526`), `HorizonCodeRuntime.h/.cpp` (`fireOnPlayerJoined` …), `HorizonCodeCompiled.h`, `src/HE_Editor/LevelScriptPanel.cpp` (`kEvents` `:1446`)
- `src/HE_Core/include/Scripting/IScriptBackend.h`, `ScriptContext.cpp`, `PyScriptBackend.cpp`, `IGameLogic.h`
- `src/HE_Scene/include/HorizonScene/Net/NetEvents.h` (neu, Muster `AntiCheatEvents.h`)

**Schritt 5b (Editor-Bedienung):**
- `src/HE_Editor/InspectorPanel.cpp` (`:2113` Kategorie, `:3425` Menü-Eintrag weg), `ProjectSettingsPanel.cpp` (`Page::Multiplayer`)
- `src/HE_Core/include/Project/ProjectSettings.h` + `.cpp` (`ProjectMultiplayerSettings`)
- Handbuch-/Tooltip-Tabellen (715/715), Overlay-Panel

**Schritt 5c (Website/Directory):**
- `Website/HorizonEngine/session-api.php`, `src/HE_Net/include/Net/SessionDirectory.h` + `.cpp` (`kind`, `transport`, Self-Probe)

**Schritt 6, 7:**
- `src/HE_Core/include/HorizonCode/HorizonCode.h` (`Variable::replicated/repNotify`, `Node::runOn/anyClient`), `HorizonCode.cpp` (JSON), `HorizonCodeRuntime.cpp` (`setVariable`-Dirty-Hook, `FunctionCall`-Umleitung), `HorizonCodeCompiled.h`/Codegen (Run-On im kompilierten Pfad)
- `src/HE_Scene/include/HorizonScene/Net/PropertyReplicator.h` + `.cpp`, `RpcRouter.h` + `.cpp`, `Components/ReplicatedVarsComponent.h`, `Net/ValueWire.h` (neu)
- `src/HE_Core/include/HorizonGameServices.h` (`HeNetServices`, append-only, eigene ABI-Version), `EngineApi.cpp` (`fillNetServices`)
- HC-Editor: Variablen-Liste, Funktionskopf, Validierung (`HcEditorUtil.cpp`, Klassen-Panel)
- `tests/test_net_property_replication.cpp`, `tests/test_net_rpc.cpp`, `test_scripting_binding.cpp`, `test_python_scripting.cpp`

### 11.3 Was in Schritt 2 NICHT passiert

Kein Konsument wechselt. `CollabController` bleibt auf TCP, `McpBridge`
bleibt auf TCP-Loopback, `GameReplication` bleibt im Test auf Loopback. Der
Transport wird für sich fertig, mit seinen Tests, bevor irgendetwas darauf
läuft; und `SecureTransport` mit `replayWindow = 0` verhält sich Byte für
Byte wie heute, was `test_net_secure` unverändert beweist.

### 11.4 Testplan für Schritt 2

Alle über Localhost-UDP-Sockets (wie `test_net_tcp.cpp`), Verlust über den
Test-Drop-Hook, Zeit über eine injizierte Uhr (`setClock`), damit RTO-Fälle
nicht wirklich warten:

1. **Verbindung:** `listen` + `connect` → beidseitig `Connected`; `Connect`
   ohne gültigen Cookie erzeugt **keinen** Peer-Zustand auf dem Host
   (`connectionCount() == 0`, `cookiesRejected == 1`).
2. **Unreliable:** 1000 Datagramme bei 20 % Drop → ~800 kommen an, keine
   Duplikate (Dup-Hook 10 %: `duplicatesDropped > 0`, `poll` liefert jedes
   `seq` höchstens einmal).
3. **ReliableOrdered unter Verlust:** 1000 Nachrichten bei 30 % Drop + Reorder
   → alle 1000 in Reihenfolge, `resends > 0`.
   **Negativkontrolle:** dieselben 1000 mit 0 % Drop → `resends == 0`
   (sonst bewiese Test 3 nur, dass Resends passieren, nicht dass sie nötig
   waren).
4. **Reliable (ungeordnet):** wie 3, alle 1000 kommen an, Reihenfolge darf
   abweichen, und tut es bei Reorder auch (mindestens ein Paar vertauscht,
   sonst hat der Test den Modus nicht unterschieden).
5. **Fragmentierung:** eine 40-KiB-Nachricht reliable bei 10 % Drop kommt
   ganz und bytegleich an; eine 40-KiB-Nachricht **unreliable** wird mit
   Log verworfen und `poll` liefert nichts.
6. **Timeout:** Client verstummt (Drop 100 %) → Host meldet `Disconnected`
   nach `timeoutMs` der injizierten Uhr, nicht früher (bei `timeoutMs − 1`
   noch verbunden).
7. **Keepalive hält:** 30 s Stille beider Seiten mit 0 % Drop → keine Trennung.
8. **Fenster voll:** 64 KiB Reliable bei 100 % Drop → der Sender trennt den
   Peer, `send` danach ist No-op, kein Wachstum.
9. **RTO:** Sample-Folge 100/100/100/400 ms → `srttMs` im erwarteten
   Bereich, `rto` steigt nach dem Ausreißer und fällt wieder.
10. **`SecureTransport` mit Fenster über `LossyTransport`:** 1000 Frames,
    Reorder-Abstand bis 30 → alle angenommen, Verbindung lebt; ein bewusst
    dupliziertes Frame verworfen (`replaysDropped == 1`); Reorder-Abstand 100
    → verworfen, Verbindung lebt. **Negativkontrolle:** `replayWindow = 0` und
    ein einziges vertauschtes Paar → `failPeer` wie heute.
11. **Log-Sink:** in keinem Record steht ein Cookie-Secret oder Session-Key,
    und mindestens ein Record wurde geschrieben.

### 11.5 Testplan für Schritt 4 (die erste echte Session, headless)

Zwei `NetGameSession`s über `LossyTransport` (10/10/5, Latenz 50 ± 20 ms):

1. Host `host()`, Client `joinDirect()` → beide `Joined` innerhalb
   simulierter 2 s; Roster hat 2 Spieler; `OnPlayerJoined(2)` auf dem Host,
   `OnConnected` auf dem Client je genau einmal.
2. Szene mit 5 Entities, 3 davon `replicates` → Client hat 3 Binds, die
   anderen 2 keine `NetworkComponent`-Registrierung.
3. Host `Create Object` einer Klasse mit `replicates` → Client bekommt
   `Spawn`, Instanz existiert, BeginPlay lief dort (Zähler in der Klasse).
4. Late Join eines dritten Clients nach 3 → bekommt 3 Binds + 1 Spawn +
   Baseline-Snapshot für die `replicateTransform = false`-Entity.
5. Client trennt → `OnPlayerLeft`, Charakter despawnt bei den anderen,
   `dropConnection` hat den Zustand entfernt (ein neuer Client mit
   wiederverwendeter `ConnectionId` erbt nichts: Anti-Cheat-Score 0).
6. Hello mit falscher Projekt-Id → Reject, `OnDisconnected(5)`, kein Roster-Eintrag.
7. **Anti-Cheat erstmals in Session:** handgebautes fremdes Input-Frame vom
   Client → `Hard`, Notice, Kick am Frame-Ende, `OnDisconnected(2)` auf dem
   Client, `OnCheatDetected` mit Client-Ticket davor.
8. **Negativkontrolle:** dieselbe Session mit `anticheat.enabled = false`
   → kein Kick, keine Observation, alles andere identisch.
