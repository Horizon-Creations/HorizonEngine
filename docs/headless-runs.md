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
