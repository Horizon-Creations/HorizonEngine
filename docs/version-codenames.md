# Horizon Engine — Versions-Codenamen (Brainstorm)

Idee: Jede Engine-Version bekommt einen **Himmels-Codenamen** (Tag-Nacht-Zyklus,
Atmosphäre, Himmelsereignisse) — passend zur Marke *Horizon*. Der Name lebt an
zwei Stellen sichtbar:

- **App-Titel / About**: z. B. `Horizon Editor 1.0 „Sunrise"`
- **DMG-Installer-Look**: jeder Codename hat ein passendes Hintergrund-Theme
  (`scripts/dmg_assets/gen_assets.py`). Vorhanden: `twilight`, `midnight`,
  `sunrise`. Geplant: `sunset`, `solar-eclipse`.

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
| Pre-Release      | 0.x                 | **First Light**     | erstes Licht — Alpha/erste Builds          | sunrise (hell)   |
| Erstes Stable    | **1.0**             | **Sunrise** ✅      | der „Aufgang" der stabilen Engine          | `sunrise` ✅     |
| Feature-Welle    | 1.x                 | **Golden Hour**     | warmes, reifes Licht                       | sunset (geplant) |
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

## Meine Top-Picks (subjektiv)

1. **First Light** (0.x / Pre-Release) — perfekter Name für die allerersten Builds.
2. **Sunrise** (1.0) — dein Vorschlag, sitzt; Theme ist schon da.
3. **Zenith** (Peak-Major) — kraftvoll, „Höhepunkt".
4. **Aurora** — ★ matcht ein vorzeigbares Feature; klingt premium.
5. **Solar Eclipse** — für den ganz großen Sprung; sehr dramatisches DMG-Theme möglich.
6. **Nebula** / **Twilight** / **Midnight** — ★ alle durch existierende Sky-Features gedeckt.

---

## Anbindung an die Technik (wenn wir es verdrahten)

- **Codename in CMake** → `set(HE_VERSION_CODENAME "Sunrise")`, per `configure_file`
  in ein `version.h` → Anzeige in Fenstertitel + About.
- **DMG-Theme = Codename**: Packager wählt automatisch `DMG_THEME` passend zum
  Codename (z. B. „Sunrise" → `sunrise`). Neue Codenamen brauchen ggf. ein neues
  Theme im `THEMES`-Dict von `gen_assets.py` (Verlauf-Farben, Glow, Sterne).
- **Feature-Match als Bonus**: ★-Namen (Aurora, Nebula, Moonrise, Cirrus,
  Starlight, Twilight) lassen sich beim Release mit genau dem gezeigten
  Sky-Feature bewerben.

_Stand: 2026-06-28 — reines Brainstorm, noch nichts verdrahtet._
