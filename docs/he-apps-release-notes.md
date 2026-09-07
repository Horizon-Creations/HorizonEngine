# Versionshinweise: HE Apps (Entwurf)

**Stand:** 06.09.2026, Branch `claude/he-apps-ui-framework-83b5f7`.
**Zweck:** Der Textentwurf für die Ankündigung des Merges. Noch nicht veröffentlicht,
noch nicht auf der Website, noch nicht in Discord. Grundlage sind die Entscheidungen
des Menschen zu `docs/he-apps-merge-analysis.md` §6.1.

**Lesehinweis:** Abschnitt 1 ist der Text, der wörtlich in eine Ankündigung kann.
Abschnitt 2 ist die Begründung dahinter, für den, der nachfragt. Abschnitt 3 sammelt
die kleineren Punkte, die eine Zeile verdienen, aber keinen Absatz.

---

## 1. Was ein bestehendes Projekt merken wird

### Create Widget zeigt sein Widget nicht mehr von selbst

Bisher hat der Knoten **Create Widget** das erzeugte Widget gleichzeitig sichtbar
gemacht. Das tut er nicht mehr: er erzeugt es versteckt, und **Show Widget** ist der
Knoten, der es auf den Bildschirm bringt. Der Grund ist, dass ein Widget zwischen
Erzeugen und Anzeigen befüllt werden will, und ein Widget, das schon steht, während
man es noch aufbaut, flackert einmal durch alle Zwischenstände.

**Was zu tun ist:** In jedem Graphen, der ein Widget erzeugt, den Ausgang *Widget* von
Create Widget in ein Show Widget führen. Die Projektvorlagen sind bereits so gebaut.

**Es gibt keine automatische Umstellung.** Ein Graph, der beim Laden umgeschrieben
wird, ist eine Änderung an einem Projekt, die niemand mitbekommen hat, und danach
stimmt die Datei nicht mehr mit dem überein, was der Autor zuletzt gesehen hat. Statt
dessen sagt die Engine es: sobald ein Graph registriert wird, in dem ein Create Widget
sein Widget an nichts weitergibt, was es zeigen könnte, steht im Log

```
HorizonCode: 'Content/Menu.hasset': Create Widget (node 3, UI/Root.hasset) creates a
HIDDEN widget and nothing shows it - wire its Widget output into a Show Widget
```

einmal pro Klasse, mit dem Namen des Graphen und dem des Widgets. Die Meldung bleibt
aus, sobald das Widget irgendwo hingeht, dem die Engine nicht folgen kann (eine
Funktion, eine Engine-Zeile wie Show Modal Widget): ein Hinweis, der auf
funktionierenden Graphen anspringt, ist ein Hinweis, den man sich abgewöhnt zu lesen.

### Feste Simulationsschritte werden am Limit verworfen, nicht nachgeholt

Der `GameLoop` holt Rückstand bei den festen Simulationsschritten (`onUpdate`) nicht
mehr nach, wenn er sein Schrittlimit erreicht: die überschüssige Zeit wird verworfen
und der Akkumulator auf null gesetzt. **Das Verhalten bleibt so, es steht hier nur,
damit es gewusst ist.**

Der Grund heißt Spirale des Todes: wer den Rückstand mitträgt, hat im nächsten Bild
mehr zu rechnen als im letzten, und die Simulation rutscht immer weiter in Zeitlupe,
ohne Rückweg. Für eine ereignisgetriebene Anwendung ist es außerdem der Normalfall und
kein Zeichen von Langsamkeit: wer nur zeichnet, wenn sich etwas ändert, hat Bilder von
einer Zehntelsekunde, und das sind sechs feste Schritte bei einem Limit von fünf, in
jedem Bild, im Leerlauf, für immer.

**Was zu tun ist:** nichts, außer es zu wissen. Ein Spiel, das an dieses Limit stößt,
läuft in Zeitlupe langsamer als vorher, aber es kommt wieder heraus. Die Warnung im
Log bleibt stehen: dass die Simulation zurückfällt, ist wissenswert, auch wenn die
Folge jetzt begrenzt ist.

### Game-Logic-Module müssen neu gebaut werden

`HorizonCodeCompiled.h` hat rund 17 neue virtuelle Methoden bekommen, und `Context`
zwei zusätzliche Felder. Ein per `HcCompiledLoader` geladenes Modul, das gegen den
alten Header gebaut wurde, hat ein anderes vtable-Layout. Das war bei jeder
HorizonCode-Erweiterung so und ist es hier wieder: **einmal neu bauen.**

---

## 2. Was sich für neue Projekte ändert, für bestehende aber nicht

- **Neue Anwendungen** zeichnen Fließtext regulär statt fett; `<b>` hat dann etwas,
  wovon es sich abheben kann. **Neue Spiele bleiben fett**, weil ein Spielautor sein
  neues Projekt gegen die vergleicht, die schon auf der Platte liegen.
- **Ein Projekt, das älter ist als das Icon-Feld, exportiert weiter ohne Icon.** Der
  fehlende Icon-Name wird nicht mehr mit `widgets` gefüllt; wer ein Icon will, trägt
  in den Projekteinstellungen einen Namen ein und bekommt `.icns`, `.ico` und `.png`
  daraus erzeugt.
- **`project.hcfg` bleibt für Spiele in der alten Fassung.** Die Bundle-Id landet nur
  in der Datei, wenn sie etwas sagt, das der Projektname nicht schon sagt, oder wenn
  es eine Anwendung ist. Damit liest ein vorgebautes Runtime-Bundle unter
  `GameRuntimes/<Plattform>/` die Datei weiter — sonst hätte es sie abgelehnt und wäre
  ohne sein Pak gestartet. Wer eine eigene Bundle-Id gesetzt hat, **muss** sein
  Runtime-Bundle neu bauen.

---

## 3. Kleinere Punkte, je eine Zeile

- `Equals` ist und bleibt ein Zahlenvergleich; für Text gibt es jetzt `String Equals`.
  Ein Graph, der `Equals` auf Zeichenketten benutzt hat, war vorher schon kaputt (alles
  war gleich) und ist es jetzt sichtbar.
- Fokus und Tippen sind zwei Zustände: `setFocus(feld)` beginnt kein Tippen mehr, das
  tun Enter, Leertaste oder ein Klick. Ein Spiel, das ein Namensfeld programmatisch
  fokussiert, braucht zusätzlich `activateFocused`.
- Ein Label lässt genau einen abschließenden Zeilenumbruch fallen. Ein Label „Text\n"
  war vorher doppelt so hoch, wie es aussah.
- Ein Shader-Varianten-Codec v2 steckt in neuen Paks. Ein altes Runtime liest daraus
  null Varianten und übersetzt zur Laufzeit — langsamer, nicht kaputt.
- Die acht Registry-Gruppen `widget`, `theme`, `dialog`, `clipboard`, `process`,
  `json`, `prefs` und `datetime` sind jetzt auch aus Lua und Python erreichbar, als
  `horizon.<gruppe>.<funktion>`, nicht mehr nur aus HorizonCode.
- Auf macOS fügt eine gesetzte Menüleiste sich in SDLs Leiste ein, statt sie zu
  ersetzen: ein Spiel behält seine.
- Neu: die Gruppe `window` (`open`, `close`, `setTitle`, `setSize`, `show`) und das
  Ereignis `OnWindowClosed` — ein zweites Fenster mit eigenem Widget-Baum. `app.*`
  meint weiterhin das Hauptfenster, kein vorhandener Aufruf ändert sich. `window.open`
  antwortet **0**, wenn es nicht geht: im Editor, und auf OpenGL, Vulkan und D3D, die
  keinen Zeichenweg in ein Zweitfenster haben (Software und Metal haben einen). Ein
  Dialog in einem Zweitfenster sperrt nur dieses Fenster.
