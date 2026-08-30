# Ablaufplan, Stand 30.08.2026

**Grundlage:** `docs/game-readiness-audit-2026-08-27.md`, dessen Umsetzungsstand, und was seither
auf `main` gelandet ist. **Stand:** fünf der sechs Blocker sind zu.

Dieser Plan ordnet, was übrig ist. Er ist nicht nach Schwere sortiert, sondern danach, was am
meisten freischaltet, was wovon abhängt und was billig viel wegräumt.

---

## Wo wir stehen

| | |
|---|---|
| Erledigt | B1 Physik zur Laufzeit, B2 Kollisionsformen und Terrain, B3 Spawnen mit Inhalt, B4 Navigation, B5 Typindex in der Pak |
| Offen | B6 Partikel, plus die schmerzhafte Liste |
| Neu dazu | Sprung mit Coyote-Zeit, fünfzehn Anleitungen im Handbuch |

Damit ist die Engine von „damit kann man kein Spiel bauen" auf „damit kann man ein
Erkundungsspiel bauen und ausliefern" gekommen. Was zwischen Erkundung und Kampf steht, ist
im Wesentlichen Welle 2.

---

## Welle 0: nachsehen, ob es stimmt

**Zuerst, und es dauert Minuten.** Fünf Dinge sind gebaut, getestet und nie in der laufenden
Anwendung gesehen worden. Wenn eines davon nicht hält, steht alles darüber auf Sand, und je
später das auffällt, desto teurer wird es.

| Prüfen | Woran man sieht, dass es hält |
|---|---|
| Handbuch, F1 drücken | Anleitungen lesbar, Schrittketten und Tabellen brechen nicht |
| Panel aus dem Dock ziehen, loslassen | Es nimmt danach wieder Tastaturfokus an |
| Close Project | Der Hub bleibt stehen, Neues Projekt führt weiter direkt in den Editor |
| Play in einer Terrain-Szene | Der Spieler steht auf der Landschaft, springt, fällt nicht hindurch |
| Ein NPC mit Nav Agent | Er läuft wirklich los und bleibt am Ziel stehen |
| Export, dann starten | Ein Spieler ist da (das war B5) |

Zwei Dinge, die dabei auffallen werden und kein Fehler sind: bestehende Szenen haben jetzt
Terrain-Kollision, was vorher hindurchfiel landet darauf. Und bestehende Exporte müssen einmal
neu gebaut werden.

**Ebenfalls hier:** `.github/workflows/ci.yml` pushen. Vier Zeilen, `ctest -j4`, 442 Sekunden
seriell gegen rund zehn parallel. Mein Token hat keinen `workflow`-Scope, das geht nur von Hand.

---

## Welle 1: der letzte Blocker

### B6, Partikel auslösbar machen — Aufwand L

Kein Mündungsfeuer, keine Explosion, kein Einschlagsstaub. Es gibt keine `particle`-Gruppe in
der Registry, `looping` wird in der Emissionsschleife nie abgefragt, und `playing` ist eine
Inspector-Checkbox.

Drei Teile, die zusammengehören:

1. `looping` wirklich auswerten. Heute ist `looping=false` in beide Richtungen falsch: bei
   einem Intervall größer als die Frame-Zeit schaltet sich der Emitter im ersten Frame ab,
   bevor ein Partikel entstand, bei einem kleineren nie.
2. Eine `particle`-Gruppe: `play`, `stop`, `burst`, `isPlaying`, in `isScriptGroup` aufnehmen.
3. Der schönere Weg ist ein gespawnter Effekt, und der geht seit B3. Also muss eine
   Partikel-Klasse spawnbar sein und sich selbst nach ihrer Lebenszeit aufräumen.

**Fertig, wenn:** ein Treffer im Spiel eine Staubwolke erzeugt, die von selbst wieder
verschwindet, und der bestehende Test in `tests/test_particles.cpp` nicht mehr grün ist, *weil*
der Emitter kaputt ist.

Danach ist kein Blocker mehr offen.

---

## Welle 2: von Erkundung zu Spiel

Diese vier zusammen sind der Unterschied zwischen „ich kann herumlaufen" und „ich kann ein
Spiel schreiben". Sie hängen kaum voneinander ab und können in beliebiger Reihenfolge.

### Tags und Teams — Aufwand M, größter Hebel

Ein NPC kann heute nicht fragen, welche Entities Gegner sind. Die einzige Weltabfrage ist
`entity.findByName`. Das blockiert Zielauswahl, Geschosse ohne Selbsttreffer, Aggro,
Freund-Feind in jeder Form, und es steht inzwischen als Lücke in drei Anleitungen.

Klein halten: eine Menge von Zeichenketten pro Entity, serialisiert, plus `entity.hasTag`,
`addTag`, `removeTag` und `entity.findByTag`. Kollisions-Layer sind die zweite Hälfte davon und
können danach kommen.

**Fertig, wenn:** ein Geschoss seinen eigenen Schützen überspringen kann, ohne dass jemand eine
Referenz von Hand verdrahtet.

### Interpolation zwischen den Physikschritten — Aufwand M

Der Akkumulator läuft mit 1/60, der Writeback schreibt Jolt direkt in die Transform, der
Extractor nimmt was dasteht. Auf jedem Bildschirm, der kein Vielfaches von 60 Hz ist, springt
jedes physikgetriebene Objekt alle 2,4 Bilder, während die Blickrichtung mit voller Rate läuft.
Gemischtes Ruckeln, die unangenehmste Variante.

Das ist die Sorte Problem, die Autoren als „die Engine fühlt sich schlecht an" melden, ohne die
Ursache benennen zu können. Es trifft die Spielfigur und damit in beiden Kameramodi die ganze
Ansicht.

**Fertig, wenn:** eine fallende Kiste bei 144 Hz gleichmäßig fällt.

### Ein Ort für Spielregeln, mit Tick — Aufwand M

Es gibt vier Engine-Klassen und keine Regel-Klasse. Rundenuhr, Punktestand, Wellen, Spielende
haben kein Zuhause und landen im Level-Skript.

Zusammen mit **Timern mit Handle** (Aufwand M) machen: heute gibt es nur `Delay`, nur einmal
gleichzeitig, ohne Handle und ohne Abbruch, und in Lua und Python gar nicht. Cooldowns,
Wellen und Rundenuhren sind dieselbe Baustelle.

**Fertig, wenn:** eine Runde mit Countdown und Punktestand ohne einen einzigen Umweg über das
Level-Skript geschrieben werden kann.

### Eine spielbare Projektvorlage — Aufwand S bis M, und der eigentliche Punkt

Die fünf Presets sind Ordnergerüste. Keines bringt einen Spieler mit.

Eine Third-Person-Vorlage mit fertigem PlayerController, Charakter, InputMappingContext,
CameraRig und Startpunkt hätte B1, B3, B4 und B5 **am ersten Tag** aufgedeckt statt nach zwölf
Blockern in einem Audit. Sie ist die billigste Gegenmaßnahme gegen das Muster, das der Audit
der ganzen Engine attestiert: gebaut bis zur Testbarkeit, angeschlossen beim ersten echten
Anwendungsfall, und der kam nie.

Seit dem 30.08. ist sie außerdem billig: die Anleitung „Your first playable scene" beschreibt
Schritt für Schritt, was hineingehört. Die Vorlage ist fast nur noch das Abtippen dessen, was
dort steht.

**Fertig, wenn:** ein neues Projekt aus der Vorlage sich starten und herumlaufen lässt, ohne
dass jemand eine Zeile schreibt. Und wenn sie im CI gebaut wird, damit sie nicht verrottet.

---

## Welle 3: bevor jemand anders es spielt

### Laufzeit-Diagnose im Build — Aufwand M

Keine Konsole, kein FPS-Anzeiger, kein Debug-Schalter, keine Kommandozeile. Der ganze Profiler
liegt im Editor. Meldet ein Tester „ruckelt bei mir" oder „hängt nach zehn Minuten", gibt es
außer der Logdatei nichts.

Für eine Engine, deren Editor-Vorschau an mindestens vier Stellen etwas anderes ist als der
Build, ist das die falsche Stelle zum Sparen. Der Absturz-Handler ist scharf, aber alles
unterhalb eines Absturzes bleibt unsichtbar.

### Assets zur Laufzeit freigeben — Aufwand M

`unloadAsset` hat außerhalb von Editor-Panels und Tests keinen Aufrufer. Kein Speicherbudget,
keine LRU-Räumung. Eine Zone laden, verlassen, entladen und zurückkehren hebt RAM und VRAM jedes
Mal und senkt sie nie. Damit ist Streaming in langen Sitzungen unbrauchbar, und zwar genau auf
der schwachen Hardware, um die es dabei geht.

Der Audio-Voice-Leak ist ein Sonderfall derselben Regel und geht mit weg.

### Einstellungen im Spiel — Aufwand M

Der Spieler kann heute weder Auflösung noch Lautstärke noch Tastenbelegung ändern. Die Werte
kommen aus der mitgelieferten `config.json` und werden jeden Frame neu gelesen, eine
Einstellungs-API ist deshalb billiger als sie aussieht. Danach ist ein Optionsmenü reine
Skriptarbeit.

Barrierefreiheit ist hier eine Weiche, keine Forderung: solange der Font-Atlas ohnehin auf UTF-8
umgebaut wird und eine Einstellungs-API entsteht, kosten ein Untertitel-Kanal und eine UI-Skala
fast nichts. Danach kosten sie einen Umbau. Für Konsolen-Zertifizierung sind Untertitel Pflicht.

---

## Welle 4: nach Bedarf gezogen

Nichts davon blockiert, jedes ist teuer, und welches zuerst drankommt entscheidet das Spiel,
das gebaut wird.

| | Aufwand | Wann es dringend wird |
|---|---|---|
| Submeshes und Material-Slots | XL | Sobald gekaufte Assets verwendet werden. Ein Haus trägt heute ein einziges Material |
| Animation: Notifies, Root Motion, dann Layer und Blend Spaces | M, M, XL | Sobald der Charakter gut aussehen soll. Die ersten beiden liefern sofort Spielgefühl, die letzten bauen das Datenmodell um |
| Sequencer und ladbare PropertyAnimClips | L | Cutscenes, Kamerafahrten |
| IK, Ragdoll, Constraints | L, XL | Füße auf schrägem Boden, Sterbeanimationen |
| Occlusion Culling | XL | Sobald ein Innenlevel groß wird |
| Foliage-Pinsel | L | Sobald Außenlevel bepflanzt werden |
| Lokalisierung | L | Vor der ersten Veröffentlichung außerhalb der eigenen Sprache |
| Gameplay-Replikation anschließen | L | Sobald Mehrspieler ein Ziel ist. Die Schicht ist gebaut, nur nicht verdrahtet |
| Editor: Mehrfachauswahl, Lichter im Viewport, Autosave, Prefab-Verknüpfung | M je | Sobald die Autorenarbeit länger dauert als die Programmierarbeit |
| Weitere Mesh-Formate als glTF | M | Sobald ein Werkzeug in der Kette nichts anderes ausgibt |
| Backend-Parität, D3D und Vulkan auf echter Hardware | L | Vor der ersten Windows- oder Linux-Auslieferung |
| NAT-PMP, Zwei-Instanzen-Durchlauf der Collaboration | M | Sobald zu zweit an einer Szene gearbeitet wird |

---

## Was sich als Methode bewährt hat

Vier Dinge, die in diesem Monat mehrfach den Unterschied gemacht haben und die in jeder
weiteren Welle gelten sollten:

**Am Code prüfen, nie am Plan.** Das Audit vom 25.08. führte acht Dinge als offen, die längst
fertig waren. Umgekehrt führte `networking-layer-design.md` die Gameplay-Replikation als fertig,
die nirgends angeschlossen ist. Ein Plan ist eine Absicht, kein Befund.

**Jeden Fund zu widerlegen versuchen.** Von 76 Rohfunden im Bereitschafts-Audit hielten 66. Die
zehn, die nicht hielten, hätten Tage an Arbeit an Dingen gekostet, die es schon gab.

**Ein Test ist erst dann einer, wenn er ohne die Änderung rot wird.** Jede Zusicherung dieses
Monats wurde durch Umkehren im Quellcode falsifiziert. Das Gegenbeispiel steht im Repo:
`tests/test_particles.cpp` besteht, *weil* der Emitter kaputt ist.

**Die Hälfte, die man vergisst, ist der Rückweg.** Beim Physik-Umbau war es der Writeback nach
lokal, bei der Navigation die Freigabe der Geschwindigkeit, beim Sprung der Spiegel in die
Komponente. Dreimal dasselbe Muster: der Hinweg war gebaut, der Rückweg fehlte, und ohne ihn war
das Ergebnis schlechter als vorher.

---

## Die kürzeste sinnvolle Reihenfolge

Wenn nur wenig Zeit ist, in dieser Reihenfolge:

1. **Welle 0** durchklicken und `ci.yml` pushen. Minuten.
2. **B6**, damit kein Blocker mehr offen ist.
3. **Projektvorlage**, weil sie billig ist und die nächste Blockerklasse verhindert.
4. **Tags und Teams**, weil drei Anleitungen sie schon als Lücke nennen.
5. **Physik-Interpolation**, weil sie das Spielgefühl am sichtbarsten hebt.

Danach entscheidet das Spiel, was als Nächstes kommt.
