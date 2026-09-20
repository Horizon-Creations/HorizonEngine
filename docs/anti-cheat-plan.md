# Anti-Cheat: Architektur, Integration, Event/Response

Plan für einen Anti-Cheat-Dienst der Engine: server-autoritative Validierung,
leichte Usermode-Integritätsprüfungen, Telemetrie. **Ausdrücklich kein
Kernel-Mode-Treiber** (Begründung in §2.3). Dieses Dokument kartiert den
Ist-Zustand, legt das Bedrohungsmodell fest, entwirft die Bausteine und
beschreibt vor allem, wie ein Spiel das Ganze mit möglichst wenig Handgriffen
anschaltet. Es ändert keinen Code.

Stand 2026-09-20, Thema 58.

---

## Stand

Das Fundament ist server-autoritativ, und zwar schon heute: der Server
simuliert, der Client schickt nur Eingaben und bekommt Snapshots zurück. Ein
Client, der seine Position lokal verbiegt, verbiegt sie **nur für sich** und
wird vom nächsten Snapshot zurückgesetzt. Damit ist die wichtigste Klasse von
Cheats, das direkte Setzen von Zustand, durch Konstruktion schon ausgeschlossen.

Was fehlt, ist dreierlei: (1) die Eingaben selbst werden nur zum Teil auf
Plausibilität geprüft, (2) alles, was der Server heute verweigert, verweigert
er **stumm**, es gibt kein Signal nach oben, das ein Spiel auswerten könnte,
und (3) außerhalb von Transforms gibt es noch keinen Client→Server-Pfad, an dem
ein Spiel eigene Ansprüche (Schaden, Inventar) validieren lassen könnte, weil
Skripte gar keine Netzwerk-API haben.

---

## 1. Ausgangslage, belegt

### 1.1 Was der Server heute schon verweigert

`GameReplication::handleInput` (`src/HE_Scene/src/GameReplication.cpp:112-147`)
ist der **einzige** Ort, an dem Client-Ansprüche in die Simulation gelangen.
Dort stehen bereits drei Prüfungen, die man als Anti-Cheat lesen kann:

| Prüfung | Zeile | Wirkung |
|---|---|---|
| Sequenz-Dedupe: `cmd.sequence <= last` → verwerfen | `:125` | ein wiederholtes Datagramm bewegt nicht zweimal |
| Owner-Check: Connection darf nur die ihr per `assignControl` zugewiesene Entity fahren | `:129-132` | fremde Figuren sind nicht steuerbar |
| `deltaTime`-Clamp auf `kMaxInputDeltaTime = 0.1 s` | `:25`, `:141` | ein einzelner Befehl mit `dt = 10` überquert nicht das Level |

Ebenso `adoptEntity`: Clients prägen nie eigene `netId`s
(`GameReplication.h:115-118`). Und der Kanal darunter, `SecureTransport`, hat
einen streng steigenden GCM-Counter als Nonce, also **Replay-Schutz auf
Frame-Ebene** (`docs/networking-layer-design.md`, §Security model).

### 1.2 Was daran fehlt

- **Der `move`-Vektor ist ungeprüft.** Der Server ruft `m_move(*tc, cmd)` mit
  dem, was der Client schickt. Ob die Spiel-Mover-Funktion die Länge klammert,
  ist Sache des Spiels; die Engine kennt kein `maxSpeed` an der Entity.
- **Der dt-Clamp ist nur pro Befehl.** Wer statt eines Befehls mit `dt = 10`
  hundert Befehle mit `dt = 0.1` innerhalb einer echten Sekunde schickt, hat
  denselben Effekt: zehnfache Geschwindigkeit. Genau das ist ein Speedhack, wie
  ihn Cheat Engine mit einem verstellten `QueryPerformanceCounter` erzeugt.
  Eine Prüfung „Summe der akzeptierten `deltaTime` gegen echt verstrichene
  Zeit" gibt es nicht.
- **Alle Ablehnungen sind stumm.** `handleInput` `return`t. Kein Zähler, kein
  Log, kein Event. Das ist das bewusste Leitmotiv von HorizonNet (abgewiesener
  Peer = korrektes Verhalten, das Log ist der einzige Ort), aber für ein Spiel,
  das reagieren soll, reicht es nicht.
- **`NetworkComponent` ist weder serialisiert noch im Inspector.**
  `grep NetworkComponent src/HE_Scene/src/SceneSerializer.cpp src/HE_Editor/`
  findet nichts. Die Replikation ist als Schicht fertig, aber im Editor nicht
  authorbar; ein Anti-Cheat-Feld an dieser Komponente setzt voraus, dass die
  Komponente erst einmal authorbar wird.
- **Skripte haben keine Netzwerk-API.** `HE::api` hat keine `net`-Gruppe
  (`src/HE_Scene/include/HorizonScene/EngineApi.h`, Namespaces `entity` bis
  `input`, kein `net`, kein RPC, keine replizierten Variablen). Ein Spiel kann
  heute aus HorizonCode/Lua/Python weder hosten noch senden. Die Wünsche
  „Schadenswerte und Inventar serverseitig prüfen" haben deshalb noch keinen
  Pfad, an den man eine Prüfung hängen könnte.
- **`GameApplication` hat keine Netzwerkanbindung.** `grep Net:: GameApplication.cpp`
  ist leer. `GameReplication` wird außer von seinem Test von nichts angetrieben
  (bekannt aus Memory `networking-layer`).

### 1.3 Transport-Realität

`src/HE_Net` kennt `LoopbackTransport`, `TcpTransport` und den Decorator
`SecureTransport`. **Es gibt keinen UDP-Transport**, `GnsTransport` ist in
`ITransport.h:8` nur als Option genannt. Das Design-Doc sagt „unreliable by
intent", faktisch fährt `GameReplication` über `NetSession` auf TCP. Für die
Anti-Cheat-Prüfungen heißt das: **Bursts sind normal**. Nach einem
Congestion-Stall kommen zwanzig Eingaben in einem Frame an. Jede Ratenprüfung
muss über ein Fenster von Sekunden messen, nicht pro Frame (§3.3).

### 1.4 Topologie: Host-autoritativ, nicht Server-autoritativ

Es gibt keinen Dedicated-Server-Modus (`grep -i dedicated|headless
GameApplication.cpp` leer). Der „Server" ist der Host, und der Host ist ein
Spieler. Alles in diesem Plan schützt die Gäste vor einander und vor sich
selbst; **den Host schützt es vor niemandem, und niemanden vor dem Host.** Das
ist keine Lücke der Umsetzung, sondern der Topologie (§6).

### 1.5 Keine Spieleridentität

Join = Session-ID + Join-Code (`SessionDirectory`, `CollabSession`). Es gibt
keine Accounts, keine persistente Spielerkennung. Ein „harter Ban" kann im
Engine-Umfang deshalb nur **sitzungslokal** sein (diese `ConnectionId`, diese
Session). Persistente Bans brauchen eine Identität, die das Spiel oder ein
Backend liefern muss (§6).

### 1.6 Bausteine, die wiederverwendet werden

| Baustein | Ort | Verwendung hier |
|---|---|---|
| Pak-TOC-Hash + Content-Hash pro Eintrag | `Hpak/HpakReader.h:17-47` | Integritäts-Manifest (§3.5) |
| HMAC-SHA256 | `Hpak/KeyDerivation.h:27` | Manifest-Signatur, Telemetrie-Auth |
| OS-TLS-HTTPS-Client | `Net/HttpsClient.h` (NSURLSession/WinHTTP/libcurl) | Telemetrie-Upload (§3.7) |
| `session-api.php`-Muster auf der Website | `Website/HorizonEngine/session-api.php` | Vorlage für einen Telemetrie-Endpunkt |
| Log-Kategorien | `Diagnostics/Log.h:43-87` | neue `Cat::AntiCheat` |
| `ProjectSettings` mit Versionsvertrag | `Project/ProjectSettings.h` | Opt-in + Policy (§4.4) |
| Event-Vierteiler HC/Lua/Python/Compiled | `HorizonCode.cpp:2519`, `IScriptBackend.h:51-101`, `HorizonCodeCompiled.h:100-205` | `OnCheatDetected` (§5.4) |
| Services-Tabelle für C++-GameLogic | `HorizonGameServices.h`, Muster in `docs/cpp-scripting-gamelogic-services-plan.md` §1 | `HeAntiCheatServices` |
| `EventBus` (typisiert, exportiert, ohne Konsumenten) | `Events/EventBus.h` | natives C++ im Host-Prozess |
| `OnHttpResponse` = Int-Ticket + Reader | `EngineApi.h:1564`, `HorizonCode.cpp:2481` | Payload-Muster für Reports |
| `NetSession::disconnect(conn)` | `Net/NetSession.h:66` | Kick ist ein Aufruf |
| `MovementComponent::maxSpeed` | `Components/MovementComponent.h:62` | Default für die Bewegungsprüfung |

---

## 2. Bedrohungsmodell

### 2.1 Wer ist der Angreifer

`SecureTransport` authentifiziert den **Kanal**: wer den Join-Code hat, ist
drin, und niemand Drittes kann mitlesen oder einspeisen. Der Angreifer dieses
Plans ist deshalb nicht ein Fremder auf der Leitung, sondern **der
legitime Client selbst**: derselbe Mensch, derselbe Join-Code, aber eine
modifizierte Exe, ein editiertes Lua-Skript, ein Speicher-Editor, ein
verstellter Systemtimer. Alles, was er tut, kommt authentifiziert und
verschlüsselt an. Die Verschlüsselung hilft gegen ihn genau null.

### 2.2 Cheat-Klassen und was dieser Ansatz dagegen kann

| Klasse | Wie es geht | Erkennbar durch | Grenze |
|---|---|---|---|
| **Speedhack** | Systemzeit gestreckt → mehr Eingaben pro echter Sekunde | dt-Budget pro Connection (§3.3) | Toleranz gegen Jitter und TCP-Bursts nötig |
| **Teleport / Fly / Noclip** | Position lokal gesetzt | **schon heute wirkungslos**: Snapshot korrigiert, andere sehen es nie | nur lokal sichtbar (für Videos/Screenshots „echt") |
| **Übergroßer move-Vektor** | Länge > 1 bei Mover ohne Clamp | Wegstrecken-Prüfung gegen `maxSpeed·dt` (§3.3) | Mover mit eigenen Dashes müssen das deklarieren |
| **Falsche Ansprüche** (Schaden, Loot, Währung) | Client meldet „hab getroffen, 999 Schaden" | Regel-Tabelle + `check` (§3.4) | braucht erst einen Script-Netz-Pfad (§1.2) |
| **Wallhack / ESP** | Client liest Snapshot-Daten fremder Entities | **Vermeiden, nicht erkennen**: Interest-Management schickt nur, was relevant ist | `relevanceRadius` ist eine Kugel, keine Sichtbarkeit; occlusion-basiertes Culling ist Zukunft |
| **Aimbot / Trigger-Bot** | Eingaben sehen legitim aus | nur statistisch (Reaktionszeiten, Winkelsprünge), später | keine harte Erkennung in diesem Plan |
| **Pak- / Skript-Edit** | Lua/Python im Pak geändert, Werte in Assets | Integritäts-Manifest beim Join (§3.5) | umgehbar von jedem, der die Exe patcht; Wert liegt gegen Gelegenheits-Edits |
| **Paket-Replay** | Mitgeschnittenes Frame erneut senden | **schon abgedeckt**: GCM-Counter + Sequenz-Dedupe | keine |
| **Host cheatet** | Der Host ist die Autorität | nicht erkennbar | Topologie (§1.4, §6) |
| **DMA / externe Hardware** | zweiter Rechner liest RAM über PCIe | nicht erkennbar, auch nicht mit Kernel-Treiber zuverlässig | bewusste Grenze (§6) |

Die Tabelle sagt, worum es bei diesem Plan wirklich geht: **Zwei Klassen sind
durch Konstruktion schon erledigt** (Teleport, Replay), **eine ist mit einem
kleinen, klaren Check erledigbar** (Speedhack), **eine braucht Deklarationen
vom Spiel** (falsche Ansprüche), **eine ist Vermeidung statt Erkennung**
(Wallhack), und **drei bleiben draußen** (Aimbot hart, Host, DMA).

### 2.3 Warum kein Kernel-Mode-Treiber, und was daraus folgt

Die Entscheidung aus der Design-Diskussion vom 19.09. bleibt stehen. Vier
Gründe, jeder für sich hinreichend:

1. **Signierungsaufwand.** Ein Kernel-Treiber braucht ein EV-Zertifikat und
   Microsofts Attestation-Signing (seit Windows 10 1607 Pflicht, für
   Anti-Cheat faktisch WHQL). Das ist ein laufender Prozess mit Firmenidentität,
   Hardware-Token und Wochen Vorlauf pro Release, nicht ein Build-Schritt.
2. **Plattformbeschränkung auf Windows.** macOS erlaubt seit Big Sur keine
   Kexts mehr für diesen Zweck, Linux hat keine stabile Treiber-ABI. Die Engine
   liefert auf drei Plattformen; ein Treiber schützt eine davon und lässt die
   anderen beiden als „Cheater-Plattform" stehen, auf die der Rest ausweicht.
3. **Antivirus-Reputationsrisiko.** Ein Treiber, der fremde Prozesse
   inspiziert, ist aus Sicht jedes AV-Herstellers ein Rootkit, bis das
   Gegenteil per Reputation bewiesen ist. Diese Reputation hat ein Studio dieser
   Größe nicht, und ein Fehlalarm beim Spieler kostet den Verkauf.
4. **Unverhältnismäßig für die Team-Größe.** Ein Kernel-Anti-Cheat ist ein
   Wettrüsten mit Vollzeit-Gegnern. Ohne ein Team, das nichts anderes tut, ist
   er nach dem ersten Bypass wertlos und dann nur noch ein Sicherheitsrisiko im
   Ring 0 des Spielers.

**Folgeentscheidung, die aus denselben Gründen gilt:** auch **kein
Prozess-Scanning, kein Debugger-Detection, kein Fenstertitel-Abgleich** im
Usermode. Das ist derselbe AV-Reputationspfad wie der Treiber, nur eine Stufe
tiefer, und es ist dasselbe Wettrüsten. Die Integritätsprüfung dieses Plans
prüft **das eigene Programm** (Exe, Dylibs, Paks) und nichts, was dem Spieler
gehört.

---

## 3. Architektur

### 3.1 Überblick

```
                     CLIENT (Gast)                          HOST (Server)
  ┌──────────────────────────────────┐        ┌─────────────────────────────────────────┐
  │ IntegrityProbe (HE_Core)         │        │ GameReplication::handleInput            │
  │  hasht Exe/Dylibs/Paks beim Start│        │   ├─ Pre-Apply:  Rate, dt-Budget,       │
  │  → Hash-Liste in den Join        │───────▶│   │              Owner, Format          │
  │                                  │  Join  │   ├─ m_move(tc, cmd)   (Spiel-Mover)    │
  │ GameReplication::pushInput       │───────▶│   └─ Post-Apply: Weg vs maxSpeed·dt,    │
  │  (Prediction wie heute)          │ Input  │                  worldExtent            │
  │                                  │        │                       │ Observation     │
  │ Snapshot → Reconcile (wie heute) │◀───────│                       ▼                 │
  │                                  │Snapshot│ AntiCheatService (HE_Scene)             │
  │ OnCheatDetected (Client-Kopie,   │◀───────│   Score pro Connection, Stufen,          │
  │   nur bei Kick: Grund für die UI)│ Notice │   Policy → Response                     │
  └──────────────────────────────────┘        │        │              │                 │
                                              │        ▼              ▼                 │
                                              │  Log Cat::AntiCheat   Events:           │
                                              │  Telemetrie (HTTPS,   HC/Lua/Py/C++     │
                                              │   async, redigiert)   EventBus          │
                                              │                       NetSession::      │
                                              │                        disconnect       │
                                              └─────────────────────────────────────────┘
```

Fünf Komponenten, drei davon neu:

| Komponente | Modul | Neu? | Aufgabe |
|---|---|---|---|
| `AntiCheatService` | HE_Scene (`HorizonScene/AntiCheat/`) | neu | Server-seitig: nimmt Observations entgegen, führt Score + Stufen pro Connection, wendet Policy an, feuert Events, füttert Telemetrie |
| Validatoren | HE_Scene, in `GameReplication` eingehängt | neu | Pre-/Post-Apply-Checks in `handleInput` (§3.3); Regel-Check für Spielwerte (§3.4) |
| `IntegrityProbe` | HE_Core (`Integrity/`) | neu | Client-seitig: Hashes des eigenen Programms; Server-seitig: Vergleich gegen Manifest (§3.5) |
| Telemetrie-Sink | HE_Scene, über `HE::Net::HttpsClient` | neu | gepufferte, redigierte Reports als JSON an einen konfigurierten Endpunkt (§3.7) |
| Konfiguration | HE_Core `ProjectSettings` + Editor-Page + `NetworkComponent`-Felder | erweitert | Opt-in, Policy, Limits (§4) |

**Schichtregel bleibt:** HorizonNet weiß weiter nichts von Szenen. Alles, was
Transform oder Physik braucht, liegt in HE_Scene neben `GameReplication`; die
Integritäts-Probe braucht nur Dateien und liegt in HE_Core; HorizonNet liefert
nur `disconnect` und den HTTPS-Client. Kein neues Modul, keine neue DLL, also
keine neuen Deploy-Listen (Windows-Falle Nr. 2 aus `networking-layer`).

### 3.2 Der Engpass: `handleInput`

Weil `handleInput` der einzige Client→Server-Pfad ist, ist er auch die einzige
Stelle, an der Validierung heute überhaupt greifen kann. Der Umbau ist deshalb
klein: die Funktion bekommt einen Vorher- und einen Nachher-Hook und meldet
jede Verweigerung als Observation statt stumm zurückzukehren.

```cpp
// Skizze, GameReplication.cpp
void GameReplication::handleInput(ConnectionId conn, BitReader& r)
{
    InputCommand cmd;
    if (!parse(r, cmd)) { observe(conn, Obs::Malformed); return; }
    if (cmd.sequence <= last) return;                         // Dedupe bleibt stumm: normal auf jedem Kanal
    if (!ownsControlled(conn)) { observe(conn, Obs::ForeignEntity); return; }

    if (m_antiCheat && !m_antiCheat->preApply(conn, cmd))     // Rate, dt-Budget, |move|
        return;                                               // Befehl fällt weg, Snapshot korrigiert

    const glm::vec3 before = tc->position;
    cmd.deltaTime = std::clamp(cmd.deltaTime, 0.0f, kMaxInputDeltaTime);
    m_move(*tc, cmd);

    if (m_antiCheat && !m_antiCheat->postApply(conn, *tc, before, cmd))
        tc->position = before;                                // Weg unplausibel: zurück, nicht anwenden
    ...
}
```

Der Service ist ein optionaler Zeiger. `nullptr` = heutiges Verhalten, Byte
für Byte. Das ist der Versionsvertrag von `ProjectSettings`, hier auf Code
übertragen: **Anti-Cheat aus ist die Abwesenheit des Objekts, nicht ein
Zweig in jeder Prüfung.**

### 3.3 Was die Engine automatisch prüft

Alle Prüfungen laufen nur auf dem Host, nur wenn `anticheat.enabled`, und alle
liefern **Observations** mit einem Gewicht, nie direkt eine Reaktion (§3.6).

**a) Format und Rate (Pre-Apply).**
Malformed Frame = Gewicht hoch (ein echter Client schickt nie eines). Eingaben
pro Sekunde über einem Fenster von 2 s gegen `maxInputsPerSecond` (Default 240
= 4× die 60-Hz-Rate, weil TCP-Bursts nach einem Stall alles auf einmal
liefern).

**b) dt-Budget (Pre-Apply), die eigentliche Speedhack-Erkennung.**
Pro Connection: `acceptedDt += cmd.deltaTime` für jeden angenommenen Befehl,
`wall += dt` aus `GameReplication::update(dt)` (laut Header echtes
Frame-Delta). Über ein gleitendes Fenster von `windowSec = 3.0`:

```
ratio = Σ acceptedDt / Σ wall          (erst ab 1 s Fensterfüllung bewertet)
Observation, wenn ratio > 1 + tolerance   (Default tolerance = 0.15)
Gewicht = (ratio − 1 − tolerance) · 10   (10 % drüber = 1 Punkt, 100 % = 10)
```

Warum das gegen Bursts robust ist: nach einem 500-ms-Stall kommen 30 Befehle
mit je 16 ms auf einmal, ihre Summe ist 0.5 s, und genau 0.5 s sind auf dem
Host auch verstrichen. Das Fenster gleicht das aus, ein Pro-Frame-Check würde
sofort auslösen. Warum die Toleranz nicht kleiner darf: Client-Uhr und
Host-Uhr driften, und ein Client unter Last liefert ein paar lange Frames, die
der Clamp auf 0.1 s kürzt, was das Verhältnis *nach unten* verfälscht; nach
oben verfälscht es nur ein gestreckter Timer. Die 15 % sind ein Startwert, den
der Testplan (§7.4) mit einem simulierten 1.5×-Speedhack und einem simulierten
Stall gegenprüft.

**c) Bewegungsplausibilität (Post-Apply), Sicherheitsnetz für Spiel-Mover.**
Die Engine kennt den Mover des Spiels nicht und muss es nicht: sie misst das
Ergebnis.

```
horizontal = |Δxz|, vertical = |Δy|
allowedH   = maxSpeed        · cmd.deltaTime · (1 + tol) + quantStep
allowedV   = maxVerticalSpeed· cmd.deltaTime · (1 + tol) + quantStep     (0 = ungeprüft)
quantStep  = 2 · worldExtent / 2^positionBits                             (dieselbe Lehre wie die Reconcile-Totzone)
```

`maxSpeed` kommt aus `NetworkComponent::maxSpeed`; **0 (Default) leitet aus
`MovementComponent::maxSpeed` ab**, falls die Entity eine trägt, sonst ist die
Prüfung aus. Vertikal ist separat, weil Sprünge und Fallen von der
Spiel-Gravitation abhängen und ein einziges `maxSpeed` sonst entweder Fallen
verbietet oder Fliegen erlaubt. Ein Verstoß setzt die Position zurück; die
Observation wiegt `dist / allowed`.

**Ehrliche Einordnung:** weil der Host den Mover selbst ausführt, kann der
Client die Position ohnehin nicht setzen, nur über `move` und `dt` lügen. Diese
Prüfung fängt deshalb genau zwei Dinge: einen Mover, der `move` nicht auf Länge
1 klammert (Spielfehler, den der Cheater ausnutzt), und Kombinationen, die das
dt-Budget einzeln noch nicht auslösen. Sie ist Defense in Depth, nicht die
Hauptverteidigung. Die Hauptverteidigung ist die Topologie.

**d) Ownership.** Eingabe für eine nicht zugewiesene Entity wird schon heute
verworfen; sie wird zur Observation mit hohem Gewicht, weil kein legitimer
Client sie erzeugt.

**e) Was durch Konstruktion gilt und keine Prüfung braucht.**
Rollback ist gratis: ein verworfener Befehl wird nicht angewendet, der nächste
Snapshot setzt den Client zurück, die Reconcile-Logik behandelt das wie jede
Misprediction. Interest-Management ist Informationsverbergung: was der Server
nicht schickt, kann kein Wallhack lesen. Das ist heute eine Kugel
(`relevanceRadius`), also grob; ein Spiel mit Wänden bekommt davon weniger als
ein Spiel mit offener Landschaft (§6).

### 3.4 Regeln für Spielwerte: deklarieren, dann prüfen

Schaden, Loot, Währung, Cooldowns: Werte, die die Engine nicht kennt und
nicht kennen soll. Das Muster, das ohne neue Konzepte auskommt:

1. **Einmal deklarieren**, in den Project Settings (oder als Asset, §4.4):
   Name, Typ, `min`, `max`, `maxPerSecond`, `maxPerEvent`, Stufe bei Verstoß.
2. **Einmal rufen**, an der Stelle, wo der Server den Client-Anspruch ohnehin
   entgegennimmt: `anticheat.check("Damage", value, sourcePlayer)` → `bool`.
   `false` heißt „nicht anwenden"; die Engine hat Score, Event und Telemetrie
   schon erledigt.

Was die Engine dabei prüft: Bereich, Rate pro Quelle über ein Fenster, Summe
pro Sekunde. Was sie **nicht** prüft: ob der Treffer in der Welt möglich war.
Das kann nur das Spiel (Sichtlinie, Reichweite, Cooldown-Zustand), und dafür
bleibt der Rückgabewert die Stelle, an der das Spiel sein eigenes Urteil
hinzufügt.

**Zielbild, sobald Property-Replikation existiert:** dieselbe Deklaration
wandert an die replizierte Property (`Replicated(range=0..100,
maxDeltaPerSec=300)`), und der `check`-Aufruf entfällt, weil der Server jede
eingehende Änderung selbst durch die Regel schickt. Die Regel-Tabelle von
heute ist genau die, die diese Property dann referenziert; nichts wird
weggeworfen. Bis dahin ist `check` der eine explizite Aufruf, den ein Spiel
schreibt, und selbst der setzt voraus, dass die `net`-Gruppe (Script-RPC)
zuerst gebaut wird, sonst kommt kein Anspruch je beim Host an (§7.1).

### 3.5 Client-Integrität, leicht und ehrlich

**Beim Export** schreibt der Exporter neben `project.hcfg` ein
`integrity.json`: SHA-256 der Exe, jeder Engine-Dylib/DLL, und für jedes Pak
dessen `tocHash` (existiert schon, `HpakReader::tocHash()`). Signiert mit
HMAC-SHA256 über einen **Projekt-Schlüssel, der nur im Export-Profil liegt**,
nicht im Client. Der Client kann sein Manifest also nicht neu signieren.

**Beim Join** hasht der Client sein eigenes Programm (einmal beim Start, im
Job-System, nicht auf dem Frame-Thread; ein 2-GB-Pak braucht nur den
`tocHash`, nicht den Inhalt) und schickt die Liste im Join-Handshake. **Der
Host vergleicht** gegen sein eigenes Manifest. Mismatch = Observation
`IntegrityMismatch` mit dem Dateinamen als Detail. Ob daraus ein Kick wird,
entscheidet die Policy (Default: Suspect, nur Event).

**Was das wert ist, und was nicht.** Ein Angreifer, der die Exe patcht, patcht
auch die Stelle, die die Hash-Liste schickt, und schickt die richtige. Diese
Prüfung fängt deshalb **Gelegenheits-Edits**: ein geändertes Lua-Skript im Pak,
ein per Hex-Editor verstellter Asset-Wert, eine Debug-Dylib. Gegen einen
entschlossenen Angreifer ist der Server-Pfad (§3.3, §3.4) die einzige
Verteidigung, und genau deshalb ist er der Hauptteil dieses Plans. Das
Dokument verspricht nichts anderes.

**Dev-Builds** (lose Projektdateien, Editor-PIE) haben kein Manifest; die
Prüfung ist dort aus und loggt einmal, dass sie aus ist.

### 3.6 Score und Stufen

Eine einzelne Observation ist fast nie Beweis: ein Server-Hitch, ein Client
unter Last, ein Paket-Burst erzeugen dieselben Muster wie ein schlechter Cheat.
Deshalb ein **abklingender Score** pro Connection:

```
score += weight                  bei jeder Observation
score *= exp(−dt / halfLife)     pro Server-Frame, halfLife = 30 s (Default)

Stufe = Info      wenn score <  suspectThreshold   (Default  5)
        Suspect   wenn score >= suspectThreshold
        Confirmed wenn score >= confirmedThreshold  (Default 20)
        oder: eine einzelne Observation mit weight >= confirmedThreshold
              (Malformed, ForeignEntity: Dinge, die kein legitimer Client tut)
```

Jeder Stufenwechsel nach oben ist ein **Report**: Connection, Stufe, die
Observations der letzten 10 s als Liste, der Score. Reports sind das, was
Events, Log und Telemetrie tragen. Ein Stufenwechsel nach unten (Abklingen)
erzeugt keinen Report, er schließt nur die Eskalation ab.

Falsch-Positiv-Schutz, konkret:

- **Hitch auf dem Host:** `wall` springt, `acceptedDt` nicht → Verhältnis sinkt,
  keine Observation.
- **Hitch auf dem Client:** wenige lange Frames, Clamp auf 0.1 s → Verhältnis
  sinkt.
- **Burst nach Stall:** Fenster gleicht aus (§3.3 b).
- **Spiel-Teleports** (Respawn, Portal, Dash): das Spiel meldet sie
  **vorher**: `anticheat.expectDisplacement(entity, maxDistance)` gibt der
  nächsten Post-Apply-Prüfung dieser Entity eine einmalige Freigabe. Ohne
  diese Meldung wäre jeder Respawn eine Observation mit Gewicht 50.
- **Quantisierung:** `quantStep` in `allowed` eingerechnet, sonst zählt eine
  perfekte Bewegung als Verstoß (dieselbe Lehre wie Prediction-Erkenntnis c).

### 3.7 Telemetrie

- **Was:** Reports (§3.6) als JSON, gepuffert, alle 30 s oder bei `Confirmed`
  sofort, per `HE::Net::HttpsClient` POST an `anticheat.telemetryUrl`.
  **Asynchron über `std::future`** wie die Directory-Aufrufe in
  `CollabController`: HTTPS auf dem Frame-Thread friert den Host sekundenlang
  ein.
- **Wohin:** Default **leer** = keine Telemetrie. Ein Spiel trägt eine URL ein.
  Als Referenz-Endpunkt kann `anticheat-api.php` neben `session-api.php` auf
  der Website entstehen (append-only JSON-Lines pro Projekt-ID, Management-Token
  wie beim Directory), das ist aber ein eigener Schritt und nicht Bedingung.
- **Redaktion**, dieselben Regeln wie `NetLog.h`: nie Join-Secret, nie
  Schlüsselmaterial, IP nur als das, was der Endpunkt ohnehin sieht
  (`REMOTE_ADDR`), Spielername nur, wenn das Spiel ihn dem Report explizit
  anhängt (`anticheat.setPlayerLabel(conn, label)`), Session-ID gekürzt
  (`logSessionId`).
- **Lokal immer:** jede Observation ab `Suspect` und jeder Report gehen in
  `Log::Cat::AntiCheat` (neu, vor `Count`, plus `kCategoryNames`). Damit ist
  `HE_LOG=AntiCheat=Debug` der erste Diagnosegriff, ohne Endpunkt.
- **Das reservierte „Network access"-Häkchen** der Permissions-Seite gilt für
  **Skripte** (`perm`-Gruppe), nicht für Engine-Telemetrie. Die beiden werden
  nicht vermischt: die Telemetrie-URL ist ihre eigene, sichtbare Einstellung,
  und leer heißt aus.

### 3.8 Engine automatisch vs. Entwickler deklariert

| Die Engine prüft von selbst (bei `enabled`) | Der Entwickler deklariert |
|---|---|
| Format, Rate, dt-Budget aller Eingaben | nichts (Defaults) |
| Ownership | nichts (`assignControl` gibt es schon) |
| Bewegungsplausibilität horizontal | `maxSpeed` an `NetworkComponent` **oder** ein `MovementComponent` an der Entity (dann 0 lassen) |
| Bewegungsplausibilität vertikal | `maxVerticalSpeed` (0 = aus) |
| Weltgrenzen | nichts (`worldExtent` aus `GameReplication::Config`) |
| Integrität Exe/Dylibs/Paks | Häkchen `integrityCheck` (Default an, greift nur in Packaged Builds) |
| Score, Stufen, Abklingen | optional Schwellen und Halbwertszeit |
| Regeln für Spielwerte | Tabelle + `check`-Aufruf pro Regel (§3.4) |
| Spiel-Teleports | `expectDisplacement` vor Respawn/Portal/Dash |
| Reaktion | Policy pro Stufe; optional Event-Handler für eigene UI/Logik (§5) |
| Telemetrie | URL (leer = aus) |

---

## 4. Integration in ein Spiel

Das ist der Teil, an dem der Plan gemessen wird. Ziel: **ein Häkchen und eine
Zahl** für den Standardfall, alles Weitere optional und an Stellen, die es
schon gibt.

### 4.1 Minimalpfad

1. **Project Settings ▸ Game ▸ Anti-Cheat ▸ „Enable"** anhaken.
2. An der Spielerfigur im Inspector **`Network ▸ Max speed`** eintragen, oder
   auf 0 lassen, wenn die Figur ein `MovementComponent` hat.

Fertig. Ab jetzt: dt-Budget, Rate, Format, Ownership, Bewegungsplausibilität,
Weltgrenzen, Integrität (Packaged Build), alles mit Default-Policy „Suspect =
Log + Event, Confirmed = Log + Event + Kick" (§5.3). Kein Handler nötig, die
Engine loggt in `Cat::AntiCheat` und kickt bei `Confirmed`.

**Voraussetzung, die vorher gebaut werden muss:** `NetworkComponent` im
Inspector und in der Serialisierung (§1.2). Ohne sie gibt es keine
Netzwerk-Sektion, in die eine Zahl eingetragen werden könnte. Das ist Teil von
Schritt 2 (§7.1), nicht dieses Plans.

### 4.2 Schritt für Schritt, mit allem Optionalen

| Schritt | Wo | Was | Pflicht? |
|---|---|---|---|
| 0 | Project Settings ▸ Game ▸ Anti-Cheat | Enable | ja |
| 1 | Inspector der replizierten Figur | Max speed (+ Max vertical speed) | nein, wenn `MovementComponent` da ist |
| 2 | Project Settings ▸ Game ▸ Anti-Cheat ▸ Rules | Regel pro Spielwert: Name, min/max, max/s | nur wenn das Spiel Client-Ansprüche entgegennimmt |
| 3 | Server-seitiger Spielcode | `anticheat.check(rule, value, player)` an der Annahme-Stelle | zusammen mit 2 |
| 4 | Spielcode vor Respawn/Portal/Dash | `anticheat.expectDisplacement(entity, dist)` | nur bei Teleports |
| 5 | Game Instance (HC) oder `onCheatDetected` (Lua/Py/C++) | eigene UI, eigene Logik, Policy überstimmen | nein |
| 6 | Project Settings ▸ Game ▸ Anti-Cheat ▸ Policy | Stufe → Reaktion anpassen | nein |
| 7 | Project Settings ▸ Game ▸ Anti-Cheat ▸ Telemetry | URL | nein |

### 4.3 API-Skizze in allen vier Frontends

Die Gruppe heißt `anticheat` und geht wie jede andere durch die
`HE::api`-Registry (Display-Name-Map, `HcNodeDocs`, `isScriptGroup`: die drei
Stellen aus Memory `engine-api-row-three-places`). Lua/Python bekommen sie über
den generischen Dispatcher (`kGroups`-Liste um `anticheat` erweitern), C++ über
eine `HeAntiCheatServices`-Tabelle nach dem Fünf-Teile-Muster.

**Registry-Rows (Server-Seite, no-op auf dem Client und ohne Session):**

| Id | Exec? | Params → Results | Bedeutung |
|---|---|---|---|
| `anticheat.check` | exec | rule: String, value: Float, player: Int → ok: Bool | Regel anwenden (§3.4) |
| `anticheat.expectDisplacement` | exec | entity: Int, maxDistance: Float | einmalige Freigabe für die nächste Bewegung |
| `anticheat.report` | exec | player: Int, rule: String, weight: Float, detail: String | eigene Observation melden (Sichtlinie verletzt etc.) |
| `anticheat.setPlayerLabel` | exec | player: Int, label: String | Name für Log/Telemetrie/UI, sonst nur ConnectionId |
| `anticheat.respond` | exec | reportId: Int, response: Int | Policy für diesen Report überstimmen (§5.3) |
| `anticheat.kick` | exec | player: Int, reasonCode: Int | explizit, unabhängig vom Score |
| `anticheat.reportLevel` | pure | reportId: Int → Int | Reader wie `http.*` |
| `anticheat.reportRule` | pure | reportId: Int → String | |
| `anticheat.reportPlayer` | pure | reportId: Int → Int | |
| `anticheat.reportEntity` | pure | reportId: Int → Int | |
| `anticheat.reportScore` | pure | reportId: Int → Float | |
| `anticheat.reportDetail` | pure | reportId: Int → String | die Observation-Liste als lesbare Zeile |
| `anticheat.playerScore` | pure | player: Int → Float | Diagnose-Overlay |
| `anticheat.isEnabled` | pure | → Bool | |

**HorizonCode.** Im Game-Instance-Graph (dort, wo `OnWindowFocusChanged`
lebt) ein Event-Node `OnCheatDetected` mit Int-Pin `reportId`, danach die
Reader-Nodes „Engine · AntiCheat · Report Level/Rule/Player…". Ein Beispiel-
Graph: `OnCheatDetected → Report Level == 2 (Confirmed)? → Show Widget
"KickNotice" mit Report Detail`. Kein neuer Node-Typ, alles `EngineCall`.

**Lua** (Level- oder Entity-Skript auf dem Host):

```lua
-- Deklariert ist "Damage" in den Project Settings: 0..100, max 300/s.
function onHitClaimed(self, attackerId, targetId, amount)
    if not horizon.anticheat.check("Damage", amount, attackerId) then return end
    applyDamage(targetId, amount)
end

function onCheatDetected(self, reportId)
    local level = horizon.anticheat.reportLevel(reportId)
    if level >= 2 then
        horizon.log.warn("kicking " .. horizon.anticheat.reportPlayer(reportId)
                         .. ": " .. horizon.anticheat.reportDetail(reportId))
    end
end
```

**Python:**

```python
def on_cheat_detected(self, report_id):
    if horizon.anticheat.reportLevel(report_id) == 1:      # Suspect: nur merken
        self.flagged.add(horizon.anticheat.reportPlayer(report_id))
        horizon.anticheat.respond(report_id, 0)              # Response.Log statt Default
```

**C++ (GameLogic-Modul)** über die Services-Tabelle:

```cpp
// HorizonGameServices.h, append-only, eigene ABI-Version
#define HE_ANTICHEAT_ABI_VERSION 1u
struct HeAntiCheatServices {
    uint32_t abiVersion; void* host;
    int  (*check)(void* host, const char* rule, float value, uint32_t player);
    void (*expectDisplacement)(void* host, uint32_t entity, float maxDistance);
    void (*report)(void* host, uint32_t player, const char* rule, float weight, const char* detail);
    void (*respond)(void* host, int reportId, int response);
    int  (*reportLevel)(void* host, int reportId);
    int  (*reportDetail)(void* host, int reportId, char* buf, int cap);   // copyOut-Konvention
    // …
};
// IGameLogic bekommt einen optionalen Hook, Default leer:
virtual void onCheatDetected(int reportId) {}
```

Spielseite: `he::anticheat::check("Damage", amount, player)` als
inline-Wrapper mit dem üblichen Vertrag „ohne Injektion sicherer No-Op":
`check` liefert dann `true`, denn ein nicht injizierter Dienst darf kein Spiel
blockieren.

**Natives C++ im Host-Prozess** (kein GameLogic-Modul): `EventBus::publish(
AntiCheatReport{…})` auf dem App-Bus; dieselben Felder wie die Reader.

### 4.4 Konfiguration: `ProjectSettings.json`

Neue Struct `ProjectAntiCheatSettings` in `ProjectSettings`, mit dem
bestehenden Versionsvertrag: Default-konstruiert = heutiges Verhalten (aus),
unbekannter Key = ignoriert, `clamp()` hält Schwellen in Bereichen.

```json
"anticheat": {
  "enabled": true,
  "integrityCheck": true,
  "tolerance": 0.15,
  "windowSec": 3.0,
  "maxInputsPerSecond": 240,
  "score": { "halfLifeSec": 30.0, "suspect": 5.0, "confirmed": 20.0 },
  "policy": { "suspect": ["log", "event", "telemetry"],
              "confirmed": ["log", "event", "telemetry", "kick"] },
  "telemetryUrl": "",
  "rules": [
    { "name": "Damage",  "min": 0, "max": 100, "maxPerSecond": 300, "level": "suspect" },
    { "name": "Pickup",  "min": 1, "max": 1,   "maxPerSecond": 5,   "level": "suspect" }
  ]
}
```

Die Regeln liegen hier und nicht in einem eigenen Asset, weil sie
projektweit gelten und im Merge-Request lesbar sein sollen (derselbe Grund,
aus dem `ProjectSettings.json` überhaupt existiert). Ein Spiel mit hundert
Regeln kann später auf ein `.hasset` umziehen; die Struct bleibt dieselbe.

**Editor-Page:** `Page::AntiCheat` unter der Gruppe **Game** in
`ProjectSettingsPanel.cpp` (Rail wie Preferences, Sofort-Speichern wie alle
Seiten). Vier `SeparatorText`-Blöcke: Enable + Integrity, Limits, Policy
(zwei Zeilen Checkboxen), Rules (Tabelle wie die Collision-Layer-Namen).
Tooltips und Handbuch-Einträge nach der 715/715-Regel (`in-engine-docs`).

**`NetworkComponent`** bekommt zwei Felder, im Inspector unter „Network":

```cpp
float maxSpeed         = 0.0f;   // m/s; 0 = MovementComponent::maxSpeed, sonst ungeprüft
float maxVerticalSpeed = 0.0f;   // m/s; 0 = ungeprüft
```

### 4.5 Was ein Entwickler ausdrücklich NICHT tun muss

- keine Anti-Cheat-Bibliothek einbinden, keine Init-Reihenfolge lernen
- keinen eigenen Score, keine eigenen Schwellen, keine eigene Ban-Liste
- keine Netzwerk-Nachrichten für Reports entwerfen
- keinen Handler schreiben, wenn Log + Kick reichen
- nichts auf dem Client tun; alles läuft auf dem Host, der Client bekommt
  nur bei einem Kick einen Grund-Code (§5.5)

### 4.6 Einordnung: Easy Anti-Cheat, BattlEye

Beide sind **Kernel-Mode-Systeme mit Client-Fokus**: ein Treiber überwacht
Prozesse, Speicher und Module des Spielers, ein Backend vergleicht Signaturen
bekannter Cheats. Beide sind für Studios lizenziert (EAC über Epic Online
Services kostenlos, BattlEye kommerziell), beide sind Windows-first, beide
haben die AV-Reputation, die ein kleines Studio nicht hat, und beide sind
**Erkennung von Cheat-Software**, nicht Validierung von Spielzuständen.

Dieser Plan ist das Gegenstück: **Validierung von Spielzuständen, keine
Erkennung von Software.** Er kann nicht sehen, ob ein Aimbot läuft; er kann
sehen, ob das, was am Server ankommt, physikalisch und regelseitig möglich
ist. Für Spiele dieser Engine-Größe ist das der Teil mit dem besten
Verhältnis von Wirkung zu Aufwand, und er ist auch das, was EAC/BattlEye
**nicht** übernehmen: die Server-Validierung muss jedes Spiel ohnehin selbst
bauen. Sollte ein Projekt später einen Client-Scanner wollen, ist er ein
Zusatz neben diesem Plan, kein Ersatz.

---

## 5. Event/Response-System

### 5.1 Stufen

| Stufe | Bedeutung | Erzeugt Report? |
|---|---|---|
| **Info** | einzelne Observation unter `suspect`-Schwelle | nein (nur Log ab Debug) |
| **Suspect** | Score über `suspect` | ja |
| **Confirmed** | Score über `confirmed`, oder eine Observation, die kein legitimer Client erzeugt (Malformed, ForeignEntity, Integrity mit `level: confirmed`) | ja |

Eine Regel (§3.4) trägt ihre eigene Stufe bei Verstoß; `"level": "confirmed"`
ist für Dinge, bei denen ein einziger Verstoß reicht (Währung von 0 auf 10^9).

### 5.2 Reaktionen

| Response | Wirkung | Umfang |
|---|---|---|
| `Log` | Zeile in `Cat::AntiCheat` | immer, nicht abschaltbar ab Suspect |
| `Event` | `OnCheatDetected` in allen Frontends + EventBus | Host |
| `Telemetry` | Report in die Upload-Queue | nur mit URL |
| `Flag` | Connection als „zur Prüfung" markiert; `playerScore` + Flag lesbar; taucht im Report-Detail auf; **keine Spielwirkung** | Session |
| `Kick` | `NetSession::disconnect(conn)` nach einer Notice mit Grund-Code | Session |
| `Ban` | Kick + diese Connection-Identität (Session-Fingerprint + Label) wird für den Rest **dieser Session** abgewiesen | Session; persistent nur über das Spiel (§6) |
| `Rollback` | implizit: verworfener Befehl, zurückgesetzte Position | immer, keine Option |

### 5.3 Policy und die Regel „nie autonom eskalieren"

Default-Policy (§4.4): Suspect = Log + Event + Telemetry, Confirmed = Log +
Event + Telemetry + **Kick**. Kein Ban per Default: ein sitzungslokaler Ban
ohne Identität ist kaum mehr als ein Kick, und ein persistenter braucht das
Spiel. Die Engine schlägt vor, das Spiel entscheidet:

- Ohne Handler gilt die Policy.
- Ein Handler kann im selben Frame `anticheat.respond(reportId, response)`
  rufen und die Policy für diesen Report **ersetzen** (nicht ergänzen).
  Der Kick wird deshalb am **Frame-Ende** ausgeführt, nicht beim Feuern des
  Events, sonst gäbe es kein Fenster zum Überstimmen (dieselbe Regel wie
  „Continue/Step nie im UI-Pass" bei den HC-Breakpoints).
- Eine Policy `[]` für eine Stufe heißt: nur Log. Damit lässt sich Anti-Cheat
  im Beobachtungsmodus fahren, um Schwellen an echten Spielern zu kalibrieren,
  bevor der erste Kick passiert. **Das ist der empfohlene erste Betriebsmodus.**

### 5.4 Das Event `OnCheatDetected`

Nach dem Vierteiler, mit genau den Stellen, die jedes Engine-Event braucht:

| Frontend | Stelle | Form |
|---|---|---|
| HorizonCode | Event-Tabelle `HorizonCode.cpp` (Zeile bei `OnWindowFocusChanged`): `{ "OnCheatDetected", "onCheatDetected", P::Int, false }`; `kEvents` in `LevelScriptPanel.cpp:1481` (Game Instance); `Runtime::fireOnCheatDetected(id, reportId)` | Event-Node, Int-Pin `reportId` |
| Compiled HC | `HorizonCodeCompiled.h`: `virtual void onCheatDetected(int reportId) { fireEvent("OnCheatDetected", 0, Value::ofInt(reportId)); }` | Override oder Fallback auf `fireEvent` |
| Lua / Python | `IScriptBackend::callOnCheatDetected(id, reportId)` mit Default-No-Op (wie `callOnTimer`), Broadcast über `eachScript` wie Input-Events | `onCheatDetected(self, reportId)` |
| C++ GameLogic | `IGameLogic::onCheatDetected(int)` (virtuell, leer) + `HeAntiCheatServices` für die Reader | Override |
| Natives C++ | `EventBus::publish(AntiCheatReport)` | typisierter Subscriber |

**Feuerort:** Game Instance zuerst (dort lebt alles Session-weite), dann Level
Script, dann Entity-Skripte der betroffenen Entity. Der Report ist ein
**Int-Ticket**, die Felder kommen über Reader (`anticheat.report*`), exakt wie
`OnHttpResponse` (`HorizonCode.cpp:2481`, `EngineApi.h:1564`). Grund: ein
HC-Event trägt genau einen `Value`, und ein Struct-Typ für Reports würde einen
Engine-Struct in die Projekt-Typen ziehen. Tickets leben bis Frame-Ende + 60 s
(damit ein Widget, das erst später aufgeht, noch lesen kann), danach liefern
die Reader neutrale Defaults.

### 5.5 Die Client-Seite

Der Client rechnet nichts, aber er soll wissen, **warum** er rausflog. Vor
`disconnect` schickt der Host eine `kMsgAntiCheatNotice {level, reasonCode,
ruleName}` (neue Message-Id in `GameReplication`, reliable). Beim Client feuert
daraufhin dasselbe `OnCheatDetected` mit einem lokal erzeugten Ticket, dessen
`reportPlayer` die eigene Id ist und dessen `reportDetail` nur den Regelnamen
trägt: **keine Observations-Liste, keine Schwellen**, ein Prober lernt nichts
über die Heuristiken (dieselbe Regel wie der nackte Reject-Code in
`failPeer`). Ein Spiel zeigt damit „Aus der Sitzung entfernt: Damage" statt
eines stummen Verbindungsabbruchs. Auf dem Client ist das die einzige
Wirkung; alle anderen Reader liefern dort Defaults.

### 5.6 Reihenfolge innerhalb eines Host-Frames

```
handleInput (Observations)  →  update(dt): Score abklingen, Schwellen prüfen, Reports erzeugen
  →  Events feuern (HC/Lua/Py/C++), Handler dürfen respond() rufen
  →  Frame-Ende: Policy oder überstimmte Response ausführen (Notice, Kick, Flag, Telemetrie-Queue)
  →  Telemetrie-Future einsammeln (wie pumpDirectory: VOR jedem Early-Return)
```

---

## 6. Nicht-Ziele und offene Fragen

### 6.1 Nicht-Ziele, bewusst

- **DMA- und Hardware-Cheats.** Ein zweiter Rechner liest den RAM über PCIe;
  das sieht kein Usermode-Code und auch kein Kernel-Treiber zuverlässig. Das
  Spiel schützt sich davor nur über Informationsverbergung (Interest-Management),
  und das ist die Grenze dieses Ansatzes.
- **Aimbot-Erkennung.** Eingaben eines Aimbots sind gültige Eingaben.
  Statistische Heuristiken (Reaktionszeit unter menschlichem Minimum,
  Winkelsprünge mit Null-Overshoot) sind ein möglicher späterer Validator über
  `anticheat.report`, aber kein Teil dieses Plans, und sie liefern nie mehr als
  Suspect.
- **Schutz vor dem Host.** Host-autoritativ heißt: der Host ist die Wahrheit.
  Ein Dedicated-Server-Modus (`GameApplication` ohne Fenster und Renderer)
  wäre die Antwort und ist ein eigenes Thema.
- **Client-Scanning** jeder Art (Prozesse, Module, Debugger, Fenstertitel),
  siehe §2.3.
- **Persistente Bans, Ban-Datenbank, Appeals.** Ohne Identität kein Ban über
  die Session hinaus; das Spiel oder ein Backend liefert beides.
- **Zeitskala im Mehrspieler.** `docs/time-control-plan.md` §7 hat das schon
  ausgeklammert; das dt-Budget nimmt die **unskalierte** Zeit auf beiden Seiten.
- **Verschlüsselung als Anti-Cheat.** Der Kanal ist schon verschlüsselt; gegen
  den authentifizierten Client selbst hilft das nicht (§2.1) und mehr davon
  auch nicht.

### 6.2 Offene Fragen

1. **Reihenfolge der Voraussetzungen.** `NetworkComponent` authorbar machen
   und `GameApplication` an `GameReplication` anschließen sind beides
   Voraussetzungen, die nicht Anti-Cheat sind, aber ohne die kein Spieler je
   in `handleInput` landet. Gehören sie in dieses Thema oder in ein eigenes
   „Gameplay-Networking nutzbar machen"? Empfehlung: **ein eigenes Thema,
   vor Schritt 3**, damit Schritt 2 (Validatoren, testbar über
   `LoopbackTransport`) nicht darauf warten muss.
2. **Script-Netzwerk-API (`net`-Gruppe).** Ohne sie bleibt §3.4 eine leere
   Zusage. Minimal nötig: `net.isHost()`, `net.sendToHost(name, payload)`,
   `net.sendToClient(player, name, payload)`, `OnNetMessage(name, payload,
   player)`. Sobald das existiert, gilt: **jeder eingehende `OnNetMessage`
   auf dem Host ist ein Client-Anspruch**, und `check` ist die Prüfung dafür.
3. **Identität.** Session-Fingerprint + Label reicht für die Session. Für
   Telemetrie, die über Sessions hinweg etwas aussagen soll, braucht es eine
   stabile, vom Spieler nicht triviale Kennung. Kandidaten: vom Spiel
   geliefertes Account-Token, oder eine beim ersten Start gewürfelte, in
   `prefs` gespeicherte UUID (löschbar, aber für Gelegenheitsfälle
   ausreichend). Entscheidung liegt beim Spiel; die Engine sollte nur ein
   `setPlayerLabel` anbieten.
4. **Occlusion-basiertes Interest-Management.** Die eigentliche
   Wallhack-Abwehr. Aufwand: Sichtbarkeitsabfrage pro Client pro Entity pro
   Tick gegen die Physikwelt, mit Hysterese, damit niemand hinter einer Ecke
   „aufpoppt". Eigenes Thema, Renderer-nahe.
5. **Toleranz-Kalibrierung.** 15 % und 3 s sind Startwerte aus Überlegung,
   nicht aus Messung. Der Beobachtungsmodus (§5.3) ist dafür da; die Zahlen
   in §4.4 müssen nach den ersten echten Sessions nachgezogen werden.
6. **Was ist mit dem Editor-PIE?** Anti-Cheat im Play-in-Editor mit
   `LoopbackTransport` ist der beste Ort, um Regeln zu testen. Vorschlag: im
   PIE läuft alles außer Kick und Telemetrie, Reports gehen in die Konsole.
7. **Telemetrie-Endpunkt auf der Website.** Wollen wir das Muster
   `session-api.php` wirklich um einen Sammler erweitern (Shared Hosting,
   append-only, Größenlimit), oder bleibt Telemetrie reine Projektsache?
   Empfehlung: erst wenn ein Projekt es braucht.

---

## 7. Umsetzungs-Roadmap

Schritt 1 ist dieses Dokument. Die weiteren Schritte sind Vorschläge für das
Thema, klein genug, dass jeder für sich mergebar ist.

### 7.1 Schritte

| # | Inhalt | Testbar über | Voraussetzung |
|---|---|---|---|
| 2 | `AntiCheatService` + Pre/Post-Apply in `handleInput`: Format, Rate, **dt-Budget**, Ownership-Observation, Bewegungsplausibilität; Score + Stufen; `Cat::AntiCheat`; Reports als Struct + Ticket-Reader (noch ohne Frontends) | `test_game_replication.cpp`: Loopback, handgebaute Frames (wie der Presence-Spoof-Test) | keine |
| 3 | `ProjectAntiCheatSettings` (toJson/fromJson/clamp/isDefault) + Editor-Page unter Game; `NetworkComponent::maxSpeed/maxVerticalSpeed` | `test_project_settings`, Panel-Audit (Tooltips 715/715) | `NetworkComponent` authorbar (eigenes Thema, §6.2.1) |
| 4 | `OnCheatDetected` durch alle vier Frontends + `anticheat.*`-Registry-Rows (drei Stellen) + `HeAntiCheatServices` + `EventBus`-Typ; `respond` am Frame-Ende; Kick + Notice + Client-Ticket | `test_engine_api`, `test_scripting_binding`, `test_python_scripting`, `test_hc_node_docs`, Parity-Fixture in `HCGEN_CLASSES` | 2 |
| 5 | Regel-Tabelle + `check` + `expectDisplacement` + `report` | `test_engine_api` | 3, 4; sinnvoll erst mit `net`-Gruppe (§6.2.2) |
| 6 | Integritäts-Manifest im Export + Probe beim Start + Vergleich beim Join | Export-Test mit manipuliertem Pak | 2 |
| 7 | Telemetrie-Sink (async, redigiert) + optional `anticheat-api.php` | Log-Sink-Test wie `test_net_secure` (Secret fehlt, aber es wurde geloggt) | 4 |

### 7.2 Checkliste berührter Dateien (für Schritt 2 bis 4)

- `src/HE_Core/include/Diagnostics/Log.h` (`Cat::AntiCheat` vor `Count`) + `Log.cpp` (`kCategoryNames`)
- `src/HE_Scene/include/HorizonScene/AntiCheat/AntiCheatService.h` + `.cpp` (neu; **kein `HE_API`**, HE_Scene exportiert alles)
- `src/HE_Scene/include/HorizonScene/GameReplication.h` + `.cpp` (Hooks, Observations, Notice-Message)
- `src/HE_Scene/include/HorizonScene/Components/NetworkComponent.h` (zwei Felder) + Serializer + Inspector
- `src/HE_Core/include/Project/ProjectSettings.h` + `.cpp` (`ProjectAntiCheatSettings`)
- `src/HE_Editor/ProjectSettingsPanel.cpp` (`Page::AntiCheat`, Gruppe Game)
- `src/HE_Scene/include/HorizonScene/EngineApi.h` + `EngineApi.cpp` (`namespace anticheat`, Registry-Rows, `fillAntiCheatServices`)
- `src/HE_Core/src/HorizonCode/HorizonCode.cpp` (Event-Tabelle), `HorizonCodeRuntime.h/.cpp` (`fireOnCheatDetected`), `HorizonCodeCompiled.h` (virtual)
- `src/HE_Editor/LevelScriptPanel.cpp` (`kEvents` der Game Instance)
- `src/HE_Core/include/Scripting/IScriptBackend.h` (Default-No-Op), `ScriptContext.cpp`, `PyScriptBackend.cpp`
- `src/HE_Core/include/HorizonGameServices.h` (`HeAntiCheatServices`, append-only), `IGameLogic.h` (virtueller Hook)
- Display-Name-Map, `HcNodeDocs`, `isScriptGroup` (Registry-Dreiklang), `tests/CMakeLists.txt` `HCGEN_CLASSES`
- `src/HE_Game/src/GameApplication.cpp` (Ctx-Binding, Frame-Ende-Ausführung), `EditorApplication.cpp` (PIE-Variante ohne Kick)

### 7.3 Was in Schritt 2 NICHT passiert

Keine Frontends, keine Settings-Page, kein Export, keine Telemetrie. Nur der
Server-Pfad mit Tests, damit die Heuristiken (dt-Budget, Fenster, Toleranz)
an simulierten Fällen stehen, bevor irgendjemand einen Knopf dafür sieht.

### 7.4 Testplan für Schritt 2

Alle über `LoopbackTransport`, ohne Sockets, wie `test_game_replication.cpp`
heute:

1. **Speedhack 1.5×:** Client schickt 90 Eingaben à 16.7 ms pro simulierter
   Host-Sekunde → nach 3 s Fenster `Suspect`, nach ~8 s `Confirmed`.
2. **Negativkontrolle Burst:** 500 ms Stall, dann 30 Eingaben auf einmal, dann
   normal → Score bleibt unter `suspect`. Ohne diese Kontrolle wäre Test 1 grün
   ohne Aussage.
3. **Negativkontrolle Host-Hitch:** `update(0.5)` einmal → keine Observation.
4. **move-Länge 5 bei Mover ohne Clamp:** Post-Apply setzt zurück, Observation
   mit Gewicht ≈ 5, Position unverändert.
5. **`expectDisplacement` dann Teleport:** keine Observation; **derselbe
   Teleport ohne Freigabe:** Observation Gewicht ≥ 50 → sofort `Confirmed`.
6. **Handgebautes Frame für fremde Entity:** `Confirmed` in einem Schritt
   (hohes Gewicht), Entity unbewegt.
7. **Abklingen:** Score 4.9 nach 30 s ≈ 2.45, kein Report.
8. **`nullptr`-Service:** alle sechs Fälle verhalten sich wie heute (kein
   Rollback, keine Observation), damit „aus = Abwesenheit" gepinnt ist.
9. **Log-Sink:** bei `AntiCheat=Debug` enthält kein Record das Join-Secret,
   **und** es wurde mindestens ein Record geschrieben.
