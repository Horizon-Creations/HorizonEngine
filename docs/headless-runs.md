# Editor/Game unbeaufsichtigt starten

Rezept für jeden Lauf, den ein Skript, ein Test oder eine Claude-Instanz startet
und niemand am Bildschirm verfolgt: ctest, `he_shot.py`, `he_mcp_multiclient.py`,
ein Smoke-Test „startet der Editor noch", eine Verifikation wie Thema 73
Schritt 4. Hintergrund und Ursachenanalyse: `docs/headless-test-runs-audit-2026-09-23.md`
(Thema 87).

**Kurz:** `HE_HIDDEN_WINDOW=1` vor jeden solchen Lauf. Dann erscheint kein
Fenster, kein Splash, kein Dock-Icon, und der Fokus bleibt, wo er war. Das
Programm läuft sonst ganz normal: Fenster (versteckt), Renderer, Present, Loop,
OnInit, UI. (Belegt ist das bisher über SDLs eigenen Fensterzustand im Log;
die Kontrolle am entsperrten Bildschirm steht noch aus, siehe Audit
„Nicht verifiziert" und die Negativkontrolle unten.)

## Die Schalter

| Variable | Gilt für | Was sie tut | Macht hidden? |
|---|---|---|---|
| `HE_HIDDEN_WINDOW` | Game + Editor | `1` (alles außer `0`): Hidden-Modus. `0`: sichtbar erzwingen, auch wenn einer der Auslöser unten gesetzt ist. Explizit gewinnt immer. | ist der Schalter |
| `HE_EXIT_AFTER_FRAMES=N` | Game + Editor | Nach N Frames beenden, Log „HE_EXIT_AFTER_FRAMES=N reached — leaving cleanly". `0` = kein Budget. | ja, automatisch (N≠0) |
| `HE_CAPTURE_FRAME=N`, `HE_CAPTURE_PATH=datei.ppm` | Game + Editor | Frame N (genauer: den davor) als PPM schreiben. Liest das Offscreen-Target, braucht kein sichtbares Fenster. | nein |
| `HE_DUMP_PATH=bild.bmp` (+ `HE_DUMP_QUIT=1`, `HE_DUMP_FRAMES`, `HE_DUMP_RHI`, `HE_DUMP_<KEY>`) | nur Editor | Offscreen-Dump in `OnInit`, **vor** dem ersten UI-Frame. `HE_DUMP_QUIT=1` beendet danach. Das ist, was `he_shot.py` setzt. | ja, automatisch |
| `HE_DUMP_LIVE=bild.bmp`, `HE_DUMP_LIVE_TRIGGER=datei` | nur Editor | Viewport des **laufenden** Editors schreiben, sobald die Trigger-Datei auftaucht. | **nein**, selbst setzen |
| `HE_NO_SPLASH=1` | Game + Editor | Nur den Splash weglassen, Hauptfenster bleibt sichtbar. Im Hidden-Modus unnötig. | nein |
| `HE_BACKGROUND_FPS=N` | Game + Editor | Rate für ein verstecktes, verdecktes oder minimiertes Fenster (Standard 15). `0` = nicht drosseln. Bild-Läufe (`HE_EXIT_AFTER_FRAMES`≠0, `HE_DUMP_PATH`, `HE_CAPTURE_FRAME`≠0) und eine laufende Profiler-Aufnahme sind ohnehin ausgenommen. | nein |

`HE_HEADLESS_DUMP` und `HE_HEADLESS_DUMP_FRAMES` **gibt es nicht**. Der Editor
ignoriert sie stillschweigend und startet normal mit Fenster (Audit, F4).

## Was der Hidden-Modus genau macht

- Jedes Fenster (auch Sekundärfenster) wird mit `SDL_WINDOW_HIDDEN` angelegt,
  `Window::Show()` tut nichts.
- macOS: `SDL_HINT_MAC_BACKGROUND_APP=1` vor dem ersten `SDL_Init(VIDEO)`,
  also kein Dock-Icon und kein `activateIgnoringOtherApps`.
- Kein Splash (Editor und Game, auch bei `game.splashEnabled`).
- Keine ImGui-Multi-Viewports (losgelöste Panels aus einer `imgui.ini` bleiben
  im Hauptfenster).
- Fehler-Messageboxen der `Application` und die Skript-Dialoge
  `dialog.message`/`dialog.confirm` werden nur geloggt; `confirm` antwortet
  „nein". Ein Fehlerlauf wird also rot, statt bis zum Timeout zu hängen.

- Die Hauptschleife läuft gedrosselt (Standard 15 FPS, `HE_BACKGROUND_FPS`),
  genau wie bei einem verdeckten oder minimierten Fenster. Ein verstecktes
  Fenster hängt nicht am Displaytakt und rendert sonst 70+ Bilder pro Sekunde,
  die niemand sieht (Perf-Audit B1: ein verwaister Editor nahm 35–54 % GPU).
  `HE_HIDDEN_WINDOW` allein nimmt davon **nicht** aus, das ist genau dieser
  Fall. Ausgenommen sind Läufe, die Bilder brauchen: `HE_EXIT_AFTER_FRAMES`≠0,
  `HE_DUMP_PATH`, `HE_CAPTURE_FRAME`≠0. Collab-/MCP-Läufe
  (`he_mcp_multiclient.py`) laufen gedrosselt weiter; wer dort volle Frames
  braucht, setzt `HE_BACKGROUND_FPS=0`. Beleg im Log:
  „Window in background — throttled to 15.0 FPS" bzw.
  „Background throttle off (…)".

**Windows, Kindprozesse:** Der Editor startet beim Hochfahren `cmd.exe /c …`
(cmake-Probe, vswhere) und pro GameLogic-Build weitere. Bis Thema 110 lief das
über `std::system`/`_popen`. Weil der Editor keine eigene Konsole hat, bekam
jeder dieser Aufrufe eine eigene, und mit Windows Terminal als
Standard-Terminal ist das ein echtes Fenster, das den Vordergrund nimmt.
Auf NN-WS03 beobachtet: drei solche Fenster je (sichtbarem) Editorstart. Für
`HE_HIDDEN_WINDOW`-Läufe ist das ein Schluss, keine eigene Beobachtung. Die
Probe läuft auch dort (fünf `cmd.exe` im versteckten Lauf), und ein einziges
`_popen` aus einem Prozess ohne Konsole brachte in der Kontrolle ein
WT-Fenster in den Vordergrund. Seit Thema 110 laufen die Aufrufe über
`HE_Scene/src/HiddenShell` mit `CREATE_NO_WINDOW`, unabhängig vom
Hidden-Modus. Versteckter Editor mit Fix: fünf Shells, null Fenster. Git lief
schon immer über `HE::Proc::run` und war nie betroffen. ctest/`he_tests`
erben die Konsole der Shell, aus der sie gestartet werden. In allen Läufen
mit Prozesszuordnung kam kein Konsolenfenster aus ihrem Prozessbaum. Ein
WT-Fenster im allerersten Lauf, noch ohne Prozessprotokoll, ist nicht
zuordenbar und trat danach nicht wieder auf.

**Windows, Firewall-Dialog (für `he_tests` behoben, für neue Editor-Deploys
nicht):** Der erste Start einer Exe unter
einem **neuen Pfad**, die auf allen Schnittstellen lauscht, öffnet den Dialog
der Windows-Defender-Firewall („Windows-Sicherheit“, `PickerHost.exe`, dunkelt
den Bildschirm ab). Er nimmt den Vordergrund. Wer „Zulassen“ klickt, bestätigt
danach noch eine Rechteanhebung (`consent.exe`). Weil jede Biene in ein eigenes
Buildverzeichnis baut, trifft das jedes neue `he_tests.exe` und jeden neuen
Editor-Deploy einmal. Auf NN-WS03 am 30.09. gemessen: zehn Dialoge zwischen
16:34 und 18:04 (sechs `he_tests.exe`, vier `HorizonEditor.exe`), jeder als
„Query User“-Regel im Firewall-Log (Ereignis 2097, rund 2 s später 2099). Alle
vier Dialoge, die focuslog dabei als `PickerHost` sah, fallen auf eines dieser
Ereignisse. Auslöser:

- **Editor:** Die LAN-Suche (`CollabController::updateLanDiscovery`,
  `CollabLanDiscovery` ist standardmäßig an) bindet beim Start
  UDP `0.0.0.0:47823` (`LanBeacon::kPort`). Mit `HE_COLLAB_OFFLINE=1` bindet
  der Editor nichts. Gemessen mit `Get-NetUDPEndpoint` an einem bereits
  freigegebenen Pfad, also ohne Dialog. Die MCP-Brücke (`HE_MCP=1`) lauscht
  nur auf `127.0.0.1` (gemessen mit `Get-NetTCPConnection`) und löst ihn
  nicht aus.
- **he_tests (behoben):** Gemessen mit einem Poller auf die TCP-/UDP-Tabellen
  des Prozesses (`GetExtendedTcpTable`/`GetExtendedUdpTable`), von einem schon
  freigegebenen Pfad, also ohne Dialog. Ein voller Lauf hielt 178 Endpunkte
  außerhalb von Loopback: TCP-Listener auf `0.0.0.0`/`::` und UDP auf
  `0.0.0.0`/`::`. Sie kamen aus `test_collab_controller` (90),
  `test_net_udp` (58), `test_net_tcp` (18), `test_net_discovery` (6),
  `test_net_secure` (4), `test_net_game_session` (3) und `test_engine_api`
  (2, darunter die LAN-Suche auf `0.0.0.0:47823`). Seitdem setzt
  `tests/main.cpp` `HE_NET_LOOPBACK_ONLY=1` (`HE::Net::socketLoopbackOnly`),
  wenn es nicht schon gesetzt ist. Jeder Bind „auf alle Schnittstellen“ in
  `HE_Net` geht dann auf Loopback, die Dual-Stack-Konstruktoren nehmen den
  IPv4-Weg, und ein ungebundener UDP-Socket wird vor dem ersten `sendto` an
  Loopback gebunden. Danach bleibt außerhalb von Loopback nur die
  Routenprobe von `socketLocalAddress` übrig (UDP-`connect` auf 8.8.8.8, es
  geht kein Paket raus). Auf NN-WS03 am 30.09. von einem **neuen** Pfad
  gemessen (`C:\hw110\fwfresh`, alle Netz-Testdateien, 661 Fälle): kein
  Ereignis 2097, keine neue Regel, focuslog ohne Vordergrundwechsel. Mit dem
  alten Code fragte heute jeder der sechs neuen `he_tests`-Pfade. Die drei
  Tests zu Dual-Stack und globaler IPv6-Adresse melden im Loopback-Modus per
  `MESSAGE`, dass sie übersprungen werden. Wer sie echt laufen lassen will,
  setzt `HE_NET_LOOPBACK_ONLY=0` (CI tut das, ein Runner fragt niemanden)
  und nimmt dafür an einem neuen Pfad den Dialog in Kauf.

Ein Pfad, der schon einmal zugelassen wurde, fragt nicht wieder. Wer ein
Buildverzeichnis wiederverwendet, statt ein neues anzulegen, erspart dem
Menschen also den Dialog. Das gilt jetzt vor allem noch für Editor-Deploys:
Bienen setzen dort `HE_COLLAB_OFFLINE=1`, das die LAN-Suche abschaltet.
(`HE_NET_LOOPBACK_ONLY` lässt Binds an eine ausdrücklich genannte Adresse
unberührt, etwa die LAN-Ankündigung beim Hosten. Für den Editor ist deshalb
`HE_COLLAB_OFFLINE` der Schalter.)

Vorschlag, nicht eingerichtet: Braucht ein Lauf an einem neuen Pfad echte
Netz-Binds, kann ein Admin einmalig eine Regel für genau diese Exe anlegen,
bevor sie zum ersten Mal startet, zum Beispiel
`netsh advfirewall firewall add rule name="HorizonEngine hwNN he_tests" dir=in action=allow program="C:\hwNN\tests\he_tests.exe" profile=private`.
Das braucht Adminrechte und gilt nur für diesen einen Programmpfad. Eine
Regel nur über den Port würde jedes Programm freigeben. Deshalb läuft das
nicht bei jedem Testlauf automatisch, und für die Tests reicht der
Loopback-Schalter.

**Windows, sichtbare Fenster:** Splash und Hauptfenster eines Editors ohne
Hidden-Modus nehmen beim Erscheinen den Vordergrund, das ist so gewollt. Läufe
von Bienen setzen deshalb `HE_HIDDEN_WINDOW=1`. Braucht ein Lauf ein sichtbares
Fenster, gibt es dafür noch keinen Schalter „zeigen, ohne zu aktivieren“ (SDL3
hätte `SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN`).

**Nicht abgedeckt:** Datei-Picker (`dialog.open*`/`save*`) und eine gebündelte
`.app` (die holt sich die Activation-Policy aus ihrem Info.plist, das
Dock-Icon bliebe). Alle heutigen Test- und Skriptläufe starten unbundled
Binaries.

## Woran man sieht, dass es gewirkt hat

Drei Zeilen im Log (stdout bzw. `HorizonEngine.log` neben der Binary):

```
Hidden mode on (HE_HIDDEN_WINDOW) — no window, splash, Dock icon or message box will be shown
Splash: off in hidden mode                        (nur wenn ein Splash angefragt war)
Primary window hidden as the main loop starts
```

Der Grund in Klammern nennt den Auslöser (`HE_HIDDEN_WINDOW`,
`HE_EXIT_AFTER_FRAMES` oder `HE_DUMP_PATH`). Das ist der einzige Beleg, der auch
auf einer gesperrten Sitzung oder einem CI-Runner trennt: dort meldet auch
`CGWindowListCopyWindowInfo` für ein sichtbares Fenster nichts. `test_app_todo`
prüft die letzte Zeile als Assertion.

## Rezepte

**Editor-Smoke-Test / „läuft die UI noch" (statt Thema 73 Schritt 4 mit sichtbarem Fenster und SIGTERM):**

```sh
HOME=$(mktemp -d) HE_HIDDEN_WINDOW=1 HE_EXIT_AFTER_FRAMES=600 \
  out/deploy/Editor/HorizonEditor > /tmp/editor.log 2>&1; echo rc=$?
grep -E "Hidden mode|Primary window|leaving cleanly" /tmp/editor.log
```

Das privat umgebogene `HOME` hält Config, Projektliste und Endpunktdatei des
Menschen heraus. Mit `HE_EXIT_AFTER_FRAMES` endet der Lauf sauber nach einer
Frame-Zahl, „leaving cleanly" ist der Exit-Beleg; kein `kill` nötig. Soll ein
Projekt geladen werden, eine `config.json` mit `LastProjectPath` und `"RHI": 4`
(Metal) ins private `HOME` legen, so wie `he_mcp_multiclient.py` es macht.

Windows (PowerShell; die Variablen gelten nur im selben Aufruf, ein privates
`APPDATA` schützt die `config.json` des Menschen, die ein sauberes Ende sonst
neu schreibt; `HE_COLLAB_OFFLINE` hält die LAN-Suche und damit den
Firewall-Dialog eines neuen Deploy-Pfads fern):

```powershell
$env:APPDATA="C:\tmp\he_appdata"; $env:HE_HIDDEN_WINDOW="1"; $env:HE_COLLAB_OFFLINE="1"; $env:HE_EXIT_AFTER_FRAMES="600"
$p = Start-Process <deploy>\Editor\HorizonEditor.exe -PassThru -WorkingDirectory <deploy>\Editor
$p.WaitForExit(); "rc=$($p.ExitCode)"
```

**Negativkontrolle** (entsperrter Bildschirm): dasselbe mit `HE_HIDDEN_WINDOW=0`.
Dann erscheinen Splash und Fenster, im Log steht „Hidden mode off" und
„Primary window shown".

**Game-Runtime starten** (so macht es `test_app_todo`):

```sh
cd <export> && HE_HIDDEN_WINDOW=1 HE_EXIT_AFTER_FRAMES=30 \
  HE_CAPTURE_FRAME=26 HE_CAPTURE_PATH=boot.ppm ./HorizonGame > boot.log 2>&1
```

**Offscreen-Bild aus dem Editor:** `scripts/he_shot.py OUT.png KEY=VAL…`.
Braucht nichts extra, `HE_DUMP_PATH` schaltet den Hidden-Modus selbst ein.

**Live-Editor mit MCP-Clients:** `scripts/he_mcp_multiclient.py` setzt
`HE_HIDDEN_WINDOW=1` selbst (per `setdefault`, ein `HE_HIDDEN_WINDOW=0` aus der
Shell gewinnt). Wer den Editor für eigene MCP-Tests von Hand mit `HE_MCP=1`
startet, muss es selbst setzen, denn ohne Frame-Budget und ohne `HE_DUMP_PATH`
wird nichts automatisch versteckt.

## Fallen

- **Config-RHI gegen `HE_DUMP_RHI`:** `HE_DUMP_RHI=Metal` tauscht nur den
  Renderer; das ImGui-Backend kommt aus der Config. Config `RHI 0` plus
  `HE_DUMP_RHI=Metal` stürzt im ersten UI-Frame in `ImGui_ImplOpenGL3_NewFrame`
  ab. Im privaten `HOME` `"RHI": 4` setzen.
- **Debug-Deploys** brauchen für die Metal-Pipelines beim ersten Start mehrere
  Minuten (~160 s bis zum ersten Frame). Timeouts danach wählen
  (`HE_SHOT_TIMEOUT=400` für `he_shot.py`).
- **Log über eine Pipe** geht bei einem Absturz im Teardown verloren; in eine
  Datei umleiten, nicht pipen.
- **Tempo:** ein verstecktes Fenster wird nicht gedrosselt (gemessen 23.09.2026:
  Editor hidden 21,1 ms/Frame gegen 24,0 ms sichtbar). `test_app_todo` läuft
  hidden in ~32 s für beide Flavours.
