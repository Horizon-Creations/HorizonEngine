# Horizon Engine — Versions-Codenamen

Jede Engine-Version bekommt einen **Himmels-Codenamen** (Tag-Nacht-Zyklus,
Atmosphäre, Himmelsereignisse) — passend zur Marke *Horizon*. Der Name lebt an
zwei Stellen sichtbar:

- **App-Titel / About**: z. B. `Horizon Engine 0.3.0 „Aurora"`
- **DMG-Installer-Look**: jeder Codename hat ein passendes Hintergrund-Theme
  (`scripts/dmg_assets/gen_assets.py`). Vorhanden: `twilight`, `midnight`,
  `sunrise`, `aurora`. Geplant: `sunset`, `solar-eclipse`.

---

## Vergeben (Release-Historie)

| Version   | Codename    | Headline                                                                 | DMG-Theme |
|-----------|-------------|--------------------------------------------------------------------------|-----------|
| 0.2.0     | **Sunrise** | erste gebrandete Builds, DMG-Installer, Codename-System                   | `sunrise` |
| **0.3.0** | **Aurora**  | Licht-Release: Deferred Renderer, Ray-Traced DDGI, Point-/Spot-Shadow-Maps, HBAO/GTAO | `aurora` |
| **0.3.2** | **Aurora**  | Projekt-Typen (Struct/Enum), Save v2, HorizonCode→C++-Codegen             | `aurora` |
| **0.3.3** | **Aurora**  | Collab-Protokoll v7, Aufräumen der Portfreigabe, Lösch-Rückfrage         | `aurora` |

Ein Patch behält den Codenamen seines Minor-Release — der Name gehört zu 0.3,
nicht zu 0.3.x. Was ein Patch trotzdem braucht, ist der Versions-Bump selbst:
0.3.1 wurde auf der Website ausgerufen, ohne dass `project()` mitzog, und der
ausgelieferte Editor meldete danach eine Version, die es laut Devlog nicht mehr
gab. Wenn die Website eine Nummer nennt, muss `CMakeLists.txt` sie auch tragen.

**So bumpst du ein Release** — eine einzige Stelle, alles andere zieht nach:

1. `CMakeLists.txt` → `project(HorizonEngine VERSION x.y.z)` und
   `set(HE_VERSION_CODENAME "...")`.
2. Optional: passendes Theme in `THEMES` (und ggf. `CURTAINS`) in
   `scripts/dmg_assets/gen_assets.py` anlegen.

`scripts/package_macos.sh` liest Version *und* Codename aus `CMakeLists.txt` und
leitet das DMG-Theme aus dem Codename ab („Solar Eclipse" → `solar-eclipse`);
fehlt das Theme, fällt es auf `twilight` zurück statt zu scheitern. Editor-Titel,
Project Hub, About-Panel und der Export-Stempel hängen am generierten
`HorizonVersion.h` — ebenfalls automatisch.

> Achtung: `HE_VERSION_STRING` ist zugleich der Handshake für kompilierte
> HorizonCode-Klassen (`GameApplication.cpp`). Ein Bump lässt eine mit der
> Vorversion exportierte `HorizonCodeGen`-Bibliothek bewusst durchfallen — das
> Spiel läuft dann interpretiert weiter und loggt eine Warnung. Gewollt: neu
> exportieren.

**Wann 1.0?** Nicht am Namen festmachen, sondern an Plattform-Vollständigkeit:
Linux-Packaging (6.6), D3D12/Vulkan-Basecolor-Texturen + MaterialComponent-
Override, BCn-Kompression. Bis dahin bleibt die 0.x-Reihe im Morgen-Pool.

Legende: ★ = es gibt bereits ein passendes Engine-Feature (Nachthimmel-Overhaul:
Nebula, Aurora, Sterne, Mond mit Phasen, Wolken/Cirrus, Twilight-Basishimmel),
der Name wäre also doppelt sinnvoll. ✅ = DMG-Theme existiert schon.

---

## Empfohlener Bogen — Major-Versionen erzählen den Tagesverlauf

Die Story: Die Engine „erwacht" (Dämmerung), steigt zum Höhepunkt (Tag), geht in
Abend/Nacht über; **dramatische Himmelsereignisse** sind den großen Meilensteinen
(2.0, 3.0 …) vorbehalten.

| Phase            | Version (Vorschlag) | Codename            | Stimmung / Anlass                          | DMG-Theme        |
|------------------|---------------------|---------------------|--------------------------------------------|------------------|
| Erste Builds     | 0.2.0 *(vergeben)*  | **Sunrise** ✅      | der „Aufgang" — erstes gebrandetes Release | `sunrise` ✅     |
| Licht-Release    | 0.3.0 *(vergeben)*  | **Aurora** ✅ ★     | Deferred + DDGI = die Engine lernt Licht   | `aurora` ✅      |
| Rest der 0.x     | 0.4+                | **Alpenglow**, **Daybreak**, **Morning Star** | Morgen-Pool weiterzählen | (neu anlegen)    |
| Erstes Stable    | **1.0**             | **Golden Hour**     | warmes, reifes Licht — alle Plattformen da | sunset (geplant) |
| Reifer Höhepunkt | 2.0                 | **Zenith**          | Sonne am höchsten — Leistungs-/Feature-Peak| (neu: „day")     |
| Übergang         | 2.x                 | **Sunset**          | Abendlicht                                  | `sunset` (geplant)|
| Abenddämmerung   | 2.x/3.0             | **Twilight** ✅     | das Zwischenlicht                           | `twilight` ✅    |
| Nacht            | 3.x                 | **Midnight** ✅     | tiefe Nacht, Sternenhimmel                  | `midnight` ✅    |
| Großes Spektakel | Major-Meilenstein   | **Solar Eclipse**   | seltenes Ereignis = großer Sprung           | `solar-eclipse` (geplant) |

> Hinweis: Reihenfolge ist nur ein Vorschlag — die Pools unten sind frei kombinierbar.

---

## Namens-Pools (zum Schöpfen)

### 🌄 Morgen / Sonnenaufgang  (frühe / „frische" Releases)
- **First Light** — Astronomie-Begriff (erstes Licht eines Teleskops). Stark für eine Premiere.
- **Daybreak**
- **Dawn**
- **Sunrise** ✅
- **Alpenglow** — das rosa Bergleuchten bei Auf-/Untergang; passt poetisch zu *Horizon*
- **Morning Star** — Venus am Morgenhimmel
- **Aurora** ★ — Morgenröte *und* Polarlicht (Doppelbedeutung)
- **Daylight**

### ☀️ Tag / Sonne  (reife, leistungsstarke Releases)
- **Zenith** — Sonnenhöchststand; ideal für einen Peak/Major
- **Solstice** — Sonnenwende (längster Tag)
- **Equinox** — Tagundnachtgleiche → „Balance", gut für ein besonders stabiles Release
- **Meridian**
- **High Noon**
- **Corona** ★ — Sonnenkorona (auch Eclipse-Bezug)
- **Helios**

### 🌆 Abend / Sonnenuntergang
- **Sunset**
- **Golden Hour** — warmes Fotograf:innen-Licht
- **Afterglow** — Nachglühen nach Sonnenuntergang
- **Dusk**
- **Gloaming** — poetisch für Dämmerung
- **Vesper** — Abendstern / Abend
- **Nightfall**

### 🌙 Nacht / Mond / Sterne
- **Midnight** ✅
- **Twilight** ✅ ★ (Twilight-Basishimmel ist umgesetzt)
- **Moonrise** ★ (Mond mit Phasen vorhanden)
- **Selene** / **Luna** ★ — Mondgöttin / Mond
- **Starlight** ★ (prozedurale Sterne)
- **Stardust**
- **Nocturne** — nächtliches Musikstück; elegant
- **Eventide** — Abend/Anbruch der Nacht
- **Polaris** — Polarstern, „Wegweiser" → gut für ein Fundament-Release
- **Nebula** ★ (3-Farb-Nebula im Himmel)
- **Constellation** / **Zodiac**
- **Milky Way** / **Galaxy**

### 🌫️ Atmosphäre & Wetter  (oft Feature-bezogen)
- **Aurora** ★ (Polarlicht/`applyAurora3D`)
- **Cirrus** ★ / **Cumulus** / **Nimbus** ★ (Wolken-System)
- **Halo** — Lichtring um Sonne/Mond
- **Mirage** — Luftspiegelung am Horizont
- **Tempest** / **Monsoon** — wenn das Wetter-System ein Headline-Feature wird
- **Zephyr** — sanfter Westwind

### 🌑 Himmelsereignisse  (für GROSSE Releases / Majors)
- **Solar Eclipse** — Sonnenfinsternis (Korona-Ring) → spektakulär
- **Lunar Eclipse** / **Blood Moon** — Mondfinsternis
- **Supermoon** / **Blue Moon** — seltene Vollmonde
- **Meteor Shower** / **Comet** — schnelle, auffällige Releases
- **Syzygy** — Ausrichtung dreier Himmelskörper (obskur, einprägsam)
- **Conjunction** — Planetenkonjunktion
- **Transit** — z. B. Venustransit

### 🧭 Horizont-/Marken-nah
- **Skyline**
- **Vista**
- **Overlook**
- **Vanishing Point**
- **Horizon** (evtl. für eine sehr besondere Version reservieren)

---

## Nächste Kandidaten (subjektiv)

1. **Alpenglow** — praktisch unbesetzt als Produktname, poetisch nah an *Horizon*.
2. **Zenith** (Peak-Major) — kraftvoll, „Höhepunkt".
3. **Solar Eclipse** — für den ganz großen Sprung; sehr dramatisches DMG-Theme möglich.
4. **Nebula** / **Twilight** / **Midnight** — ★ alle durch existierende Sky-Features gedeckt.

> Namens-Kollisionen mitdenken: **Aurora** (AWS, Chromium-Channel), **Corona**,
> **Zephyr** und **Polaris** sind anderweitig stark belegt — für 0.3.0 in Kauf
> genommen, weil der Feature-Match zählt; für ein 1.0 lieber etwas Eigenes.

---

## Anbindung an die Technik

- **Codename in CMake** → `set(HE_VERSION_CODENAME "Aurora")`, per `configure_file`
  in `HorizonVersion.h` → Fenstertitel, Project Hub, About-Panel, Tutorial.
- **DMG-Theme = Codename**: `package_macos.sh` leitet `DMG_THEME` automatisch ab
  („Solar Eclipse" → `solar-eclipse`), mit Fallback auf `twilight`. Neue Codenamen
  brauchen einen Eintrag im `THEMES`-Dict von `gen_assets.py` (Verlauf-Farben,
  Glow, Sterne) — optional zusätzlich `CURTAINS` für Polarlicht-Bänder.
- **Feature-Match als Bonus**: ★-Namen (Aurora, Nebula, Moonrise, Cirrus,
  Starlight, Twilight) lassen sich beim Release mit genau dem gezeigten
  Sky-Feature bewerben.

_Stand: 2026-08-04 — verdrahtet; aktuell 0.3.0 „Aurora"._
