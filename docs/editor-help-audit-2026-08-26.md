# Audit: Was das Handbuch im Editor abdecken kann — und was heute fehlt

> Stand: 2026-08-26 · Gemessen an `main`, nicht geschätzt: die Zahlen unten kommen aus
> einem Lauf über `src/HE_Editor/*.cpp`, der jedes beschriftete Bedienelement einsammelt
> und gegen die Schlüssel in `EditorHelp.cpp` hält.
>
> Ziel, das der Audit prüft: **jedes Bedienelement, bei dem man nicht weiß, was es tut,
> erklärt sich beim Hovern in einem Satz — und wer mehr will, drückt F1 und landet bei
> einem Eintrag, der genau dieses Ding behandelt.**

> **Die Zahlen kommen aus `scripts/editor_help_audit.py`** — dasselbe Skript hängt als
> ctest `editor_help_audit` im Build und schlägt an, sobald ein Bereich ein
> Bedienelement dazubekommt, für das es keinen Eintrag gibt. Ein fehlender Tooltip ist
> sonst unsichtbar: nichts bricht, niemand merkt es außer dem, der hovert.

## 1. Was gemessen wurde

Eingesammelt wird jedes Widget mit sichtbarem Label: die `Row::*`-Zeilen, Checkboxen,
Slider, Drags, Combos, Farbfelder, Textfelder — und Aktionen (Buttons, Menüeinträge).
Reine Bestätigungs-Chrome (`OK`, `Cancel`, `Close`, `Save`, `Browse`, `+`) zählt **nicht**
mit: Bedienelemente, deren Wirkung im Wort steht, brauchen keinen Eintrag, und sie
mitzuzählen würde die Lücke größer aussehen lassen als sie ist.

Als *gedeckt* gilt ein Label, das die Lookup-Regel von `Help::find` auflöst — also
`"<Abschnitt>/<Label>"`, `"<Label>"`, jeweils mit und ohne `##suffix`.

## 2. Die Zahlen

| Bereich | Bedienelemente | gedeckt | offen |
|---|---:|---:|---:|
| Interface (Menüs, Toolbars, Outliner, Content Browser) | 104 | 7 | **97** |
| Details-Panel (Komponenten) | 219 | 208 | 11 |
| Einstellungen | 60 | 42 | **18** |
| Zusammenarbeit + Source Control | 39 | 0 | **39** |
| UI-Designer | 30 | 0 | **30** |
| HorizonCode-Panels (ohne Nodes) | 30 | 0 | **30** |
| Export + Profiler | 27 | 0 | **27** |
| Material-Editor | 26 | 0 | **26** |
| Landschaft + Environment | 17 | 0 | **17** |
| Animation + Audio + Mesh | 15 | 0 | **15** |
| Input | 7 | 0 | **7** |
| **Gesamt** | **574** | **257** | **317** |

Dazu kommen die **311 Nodes** der HorizonCode-Referenz, die vollständig gedeckt sind
(generiert aus den Registern, Test erzwingt Vollständigkeit in beide Richtungen).

**Zu den 11 offenen im Details-Panel**, weil die Zahl der Meldung „252 von 252" von
gestern zu widersprechen scheint: die 252 waren beschriftete **Wertezeilen und
Checkboxen**, und die sind weiterhin vollständig. Was hier dazukommt, sind **Aktionen**,
die der frühere Lauf gar nicht gezählt hat — und die genau so erklärungsbedürftig sind:
`Bake` (Nav Mesh), `Go`/`Stop` (Nav Agent), `Regenerate` (Foliage), `+ Texture Slot`,
`Reset to material default`, `Set for 4 m tiles` (Terrain), `+ Level` (LOD),
`Remove Component`.

## 3. Der eigentliche Befund

Die 317 fehlenden Tooltips sind die kleinere Hälfte des Problems. Die größere:

> **320 Hilfe-Einträge zeigen auf 47 verschiedene Themen.**

F1 auf „Cloud Fluffiness" landet auf `rendering#sky` — einem Abschnitt über den Himmel im
Allgemeinen, in dem das Wort „Fluffiness" nicht vorkommen muss. 46 Einträge teilen sich
dieses eine Thema, 25 teilen sich `systems#animation`, 22 `rendering#postfx`.

Das ist die Folge davon, dass das Handbuch **die Website 1:1 spiegelt**. Die Website ist
nach Themen geschrieben — „Rendering & Environment", „Gameplay Systems" —, und das ist
für Prosa richtig. Als Nachschlagewerk für ein Bedienelement ist es die falsche Form: der
Leser hat eine konkrete Frage („was macht dieser Regler?") und bekommt ein Kapitel.

Für die Nodes ist das schon gelöst: die Referenz wird aus den Registern **gebaut**, ein
Abschnitt pro Node, und F1 landet genau dort. Für den Rest des Editors fehlt das noch.

## 4. Vorschlag: die Hilfe-Tabelle IST das Nachschlagewerk

Dieselbe Bewegung wie bei den Nodes, eine Ebene höher:

    Hilfe-Tabelle (EditorHelp.cpp)
        ├── Tooltip beim Hovern            (gibt es)
        └── generierte Referenzseiten      (fehlt)  ← ein Abschnitt pro Bedienelement

Aus den Einträgen wird pro Bereich eine Seite gebaut (`HcNodeReference` als Vorlage):
**Editor-Oberfläche · Komponenten · Einstellungen · Materialien · UI · HorizonCode ·
Input · Landschaft · Export · Zusammenarbeit**. Jeder Eintrag wird ein Abschnitt mit

* seinem Namen als Überschrift und der Kategorie als Eyebrow (wo es sitzt),
* dem Satz, den auch der Tooltip zeigt,
* dem Shortcut, wenn es einen gibt,
* einem Verweis auf das Konzeptkapitel der Website („mehr dazu: Rendering ▸ Sky").

**F1 muss dabei umziehen, sonst ist nichts gewonnen.** Heute zeigt das `topic`-Feld eines
Eintrags dorthin, wo F1 landet — also auf das Kapitel. Nach dem Umbau ist es der
„mehr dazu"-Verweis *innerhalb* des Eintrags, und F1 geht auf den **eigenen Abschnitt**
des Bedienelements. Bleibt das Routing wie es ist, entstehen die Referenzseiten und F1
springt weiter nach `rendering#sky` — genau die Beschwerde, unverändert.

Damit gilt automatisch:

1. **Jeder Tooltip hat einen Eintrag** — F1 landet auf dem Bedienelement selbst, nicht auf
   einem Kapitel. Der Test dafür schreibt sich von allein: die Seite wird aus derselben
   Tabelle gebaut, aus der der Tooltip kommt.
2. **Kein Eintrag ohne Bedienelement**: was in der Tabelle steht, aber im Editor nicht
   vorkommt, fällt im Abgleich auf — das Skript oben läuft als ctest `editor_help_audit`
   mit und schlägt an, sobald ein Bereich ein ungedecktes Bedienelement dazubekommt.
3. **Die Website behält, was sie gut kann**: die Konzeptkapitel. Sie bleiben im Bündel und
   sind das Ziel der „mehr dazu"-Verweise, statt das Nachschlagewerk zu sein.

Das Handbuch im Editor besteht danach aus drei Teilen, und nur der mittlere kommt von der
Website:

| Teil | Herkunft | Antwortet auf |
|---|---|---|
| Editor-Referenz | generiert aus `EditorHelp.cpp` | „Was macht dieses Bedienelement?" |
| Handbuch | Website-Bündel | „Wie funktioniert das Ganze?" |
| Node-Referenz | generiert aus den Registern | „Was macht dieser Node?" |

## 5. Reihenfolge

Nach Nutzen pro Aufwand, und die erste Zeile ist die wichtigste — sie baut die Maschine,
die die anderen erst zu Einträgen macht.

| # | Schritt | Umfang |
|---|---|---|
| 0 | Generierte Referenzseiten aus der Hilfe-Tabelle + Abgleich-Test | **erledigt** |
| 1 | **Interface**: Menüs, Viewport-Leiste, Outliner, Content Browser, Konsole | 97 |
| 2 | **Einstellungen** fertig machen (Rest + Werkzeug-Status) | 18 |
| 3 | **Materialien** (Graph-Editor, Vorschau, Parameter) | 26 |
| 4 | **UI-Designer** (Palette, Anker, Ereignisse) | 30 |
| 5 | **Input** (Aktionen, Kontexte, Bindungen) | 7 |
| 6 | **Landschaft** (Pinsel, Ebenen, Environment-Fenster) | 17 |
| 7 | **Export + Profiler** | 27 |
| 8 | **HorizonCode-Panels** (Variablen, Funktionen, Klassen — Nodes sind fertig) | 30 |
| 9 | **Zusammenarbeit + Source Control** | 39 |
| 10 | Animation, Audio, Mesh-Editoren | 15 |

## 5b. Umsetzungsstand

Die laufende Zahl steht in `scripts/editor_help_audit.py` als `BASELINE`, und
`ctest -R editor_help_audit` schlägt fehl, sobald ein Bereich schlechter wird als dort
notiert. Die Baseline wird gesenkt, wenn eine Stufe fertig ist, und nie angehoben, um
den Test zu beruhigen.

| Stufe | Stand | Offen |
|---|---|---:|
| 0 · Generierte Referenzseiten | erledigt | — |
| 1 · Interface | erledigt (~90 neue Einträge, 19 Area-Regeln, `menuItem`/`button`/`selectable`) | 0 |
| 2 · Einstellungen | erledigt (18 Einträge, `smallButton`, drei neue Scopes) | 0 |
| 3–10 | offen | 191 |

Zwei Dinge, die die Stufe 2 nebenbei gefunden hat und die für die restlichen Stufen
gelten:

* **Ein Eintrag ohne offenen Scope ist tot.** `Help::find` hängt den obersten Scope
  davor; ist keiner offen, wird `"Preferences/Private"` nie nachgeschlagen. Genau das
  war bei den drei Repository-Einträgen der Fall, seit es sie gibt. Das Audit-Skript hat
  es nicht gesehen, weil es den Scope für die ganze Datei angenommen hat. Deshalb prüft
  `tests/test_editor_help.cpp` jetzt jeden neuen Scope zur Laufzeit.
* **Die gestylten Knöpfe haben gar nicht nachgeschlagen.** `primaryButton`,
  `dangerButton`, `cancelButton`, `dangerSmallButton` und `dangerMenuItem` haben ihre
  Beschriftung nie an die Hilfe-Tabelle gegeben, obwohl das Audit sie als
  Bedienelemente gezählt hat. Ein Dialog-Bestätiger ist der Knopf, der einen Satz am
  nötigsten hat. Jetzt schlagen sie alle nach.

## 6. Was bewusst NICHT abgedeckt wird

* **Bestätigungs-Chrome** (`OK`, `Abbrechen`, `Schließen`): das Wort ist die Erklärung.
* **Dynamische Beschriftungen** (Asset-Namen, Peer-Namen, Zahlen): sie beschreiben Daten,
  nicht eine Funktion.
* **Die Prosa der Website**: sie wird nicht ersetzt, sondern verlinkt. Zwei Beschreibungen
  desselben Konzepts, die auseinanderlaufen, sind schlechter als eine.
