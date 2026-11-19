# Wolken 2.0, Schritt 3: Bestandsaufnahme der Wolken und Plan (Thema 227, Hive-Schritt 1139)

Zweig `claude/wolken-2-0-wind-sprung-dome-hoehe`, nach `7cade91b`. Nur Doku und ein Dump-Schlüssel (`HE_DUMP_CLOUDQUALITY`), kein Shader geändert.
Bilder in `docs/clouds-2-0-survey/`. Die Referenzbilder des Menschen (`~/Downloads/clouds-4215608_1920.jpg`, `images*.jpeg`, Thema-Dateien 69-76)
sind nicht im Repo; ich habe sie angesehen und fasse sie unten zusammen.

## 1. Was die Engine heute zeichnet
Drei Wolkenpfade in **einer** Quelle (`kSkyFS` in `SkyShaderSource.h`, GL/Vulkan/D3D), Metal hat den Spiegel in `MetalRenderer.mm`:

| Pfad | Funktion | Wesen |
|---|---|---|
| Dome (`cloudMode 0`) | `applyClouds` | Slab auf einer Einheits-Halbkugel (`kCloudBase..kCloudTop`), 16/12/8 Basisschritte, Worley-Billow über Perlin-Coverage, 3-Schritt-Sonnenmarsch, Deck für Overcast. Seit Schritt 2 mit Höhenfaktor `cloudDomeHeight`. |
| 3D Classic (`cloudMode 1`, Stil 0) | `applyClouds3D` | Welt-verankerter Slab (`cloudHeight`..+300), gerundete fBm-Blobs, weicher Boden. |
| 3D Realistic (Stil 1) | `applyClouds3DReal` + `cloudFieldDensity` | HZD-artig: Weather-Map (Spalte, Domain-Warp, Makro-Oktave, Perlin-Worley-Clustering), Turmhöhe je Formation, flacher Boden, Zwei-Skalen-Carve, Nebel-Saum, Feinoktave, Wrenninge-Mehrfachstreuung (3 Oktaven), HG-Phase mit Silberrand, Powder, Wolkenschatten über dasselbe Feld. |

Qualität (`uCloudQuality`) steuert Schrittzahl: Realistic Medium `N = clamp(Strecke / (0,28 * 300), 26, 80)`, Low 14..44, High 32..128;
Sonnenmarsch 3/4/6 Schritte (exponentiell, erster Schritt eng). Sprung-Offset ist `skyIgn(gl_FragCoord)` (Interleaved-Gradient-Noise).
Es gibt keine zeitliche Akkumulation im normalen Himmelspass; der Low-Res-Pfad (`lowResClouds`, Viertelauflösung + Reprojektion) ist optional.
Die Form ist **nicht** das Hauptproblem: das Realistic-Feld hat schon Weather-Map, Domain-Warp, Türme und Carve.

## 2. Was auf den Bildern fehlt (Engine heute: `engine-today-2026-10-29.jpg`, Referenzen des Menschen)
Referenzen: (1) hohe Blumenkohl-Türme mit grellweißen Oberseiten, (2) dichte Bank aus vielen überlappenden Lappen mit tiefen Spalten,
(3) einzelne warm beleuchtete Wolke mit dunkler Unterseite, (4) verstreute Cumuli mit flachem Boden und scharfen Rändern.

| Kriterium | Referenzen | Engine heute |
|---|---|---|
| Feindetail | Lappen in vielen Größen bis in den Meterbereich, Ränder aus Wölbungen | nur zwei Carve-Oktaven und eine Feinoktave (`np*2.6`), vom Boden aus glatte Blobs |
| Hell/Dunkel | gleißende Oberseite, deutlich dunklere Spalten und Unterseite, Silberrand an der Sonnenseite | mittelgrau, flach; Bauch ist die Konstante `(0.30,0.35,0.46)`, Sonnenmarsch-Kontrast gering |
| Randschärfe | scharf mit weichem Saum (4), Lappen-Kanten (1, 2) | überall weich, fast nebelartig (Nebel-Saum und Distanz-Fade) |
| Wolkenmaßstab | kleine bis mittlere Zellen, Himmel dazwischen (4) | große zusammenhängende Haufen, wenig Himmel zwischen den Formationen |
| Dämmerung | warmer Rand, Rückenlicht (3) | bei TOD 0,70 kaum von Tag zu unterscheiden (nicht untersucht, ob das Zeitfenster stimmt) |
| Dome-Modus | nicht in den Referenzen, aber: flach, verschwommen, ohne Tiefe | Höhenfaktor jetzt vorhanden; die Form bleibt die alte Billow-Formel |
| Artefakte | keine | diagonales Schraffurmuster auf Oberflächen (siehe 3) |

## 3. Das Streifenmuster, gemessen
Ursache: `skyIgn` als Sprung-Offset bei zu großen Schritten (Realistic Medium: ~84 Welteinheiten pro Schritt bei `thick` 300).
Der Zeuge `stripes-quality-2026-10-29.jpg` (Metal, gleicher Ausschnitt, `HE_SKY_TIME=30`, 4x vergrößert): oben links Low, oben rechts Medium, unten links High,
unten rechts Medium mit Low-Res. Mittlere Nachbar-Differenz im Ausschnitt (Maß für Schraffur/Körnung): Low 8,7, Medium 3,1, High 2,1, Medium + Low-Res 1,2.
Low zeigt deutliche Diagonalen, Medium schwach, High sauber; Low-Res glättet am stärksten. Heilmittel in Preis-Reihenfolge: nicht-strukturierter Sprung
(Blue-Noise-Textur oder R2-Folge statt IGN), zeitliche Akkumulation (Low-Res-Reprojektion als Standard für Realistic), dann erst mehr Schritte.
Dieser Dump-Schlüssel (`HE_DUMP_CLOUDQUALITY`, `HE_DUMP_LOWRESCLOUDS`) ist jetzt in `EditorApplication.cpp`.

## 4. Plan, nach Wirkung pro Aufwand
| # | Maßnahme | Aufwand | Kosten (erwartet) | Prüfung |
|---|---|---|---|---|
| 1 | Sprung-Offset ohne Struktur (Blue Noise / R2) und Schraffur messen; Low-Res-Reprojektion für Realistic als Standard prüfen | S | ~0 (Textur-Fetch) | Nachbar-Differenz-Maß wie oben, Metal + GL, Bild |
| 2 | Beleuchtung: Himmelslicht als Umgebungsterm statt Konstante, tieferer Bauch, stärkerer Sonnenmarsch-Kontrast, Silberrand und Powder neu abstimmen | M | gering | Referenzen (1)(2)(3) nebeneinander, Dämmerung (TOD 0,65-0,75) |
| 3 | Feindetail: zwei bis drei zusätzliche Oktaven (curl-verzerrtes Worley), mit Distanz und Dichte ausgeblendet, nur im Rindenbereich | M | +1-2 Fetches je Schritt mit Dichte, messen | Referenz (1), Nahaufnahme vom Boden |
| 4 | Wolkentyp-Parameter (Cumulus flach, Cumulus congestus, Stratocumulus, Cumulonimbus) mit Höhenprofil und Zellgröße; kleinere Zellen mit Himmel dazwischen | M-L | ~0 | Referenz (4), neue Preset-Stufen im Inspector |
| 5 | Dome-Modus: das Realistic-Feld (billig, ohne Parallaxe) auch für `cloudMode 0` | L | wie Realistic | A/B Dome gegen 3D von unten |
| 6 | Horizont und Luftperspektive: Wolken in den Dunst statt in ein flaches Band (`hazeFar`, Extinktion entlang des Strahls) | S-M | ~0 | Flachwinkel-Bilder |
| 7 | Backends: Metal-Spiegel, Vulkan/D3D über Cross-Compile, Fallback-Shader entscheiden | je Schritt | | `test_sky_shader`, Windows offen |

Reihenfolge: 1, 2, 3 zuerst (sichtbarster Gewinn), dann 4, dann 6, zuletzt 5. Jede Stufe bitgleich bei ausgeschaltetem Schalter oder mit neuer, vom Menschen erlaubter Baseline
(Zeugensatz: 16 gepinnte Sky-Bilder je Backend, Rauschboden 16/16; das Skript ist nicht im Repo, die Bilder und Parameter stehen in den Handoff-Dokumenten von Schritt 1 und 2), und vor Stufe 3 eine GPU-Messung der Himmelskosten (`scripts/perf/he_perf_capture.py`, nur Pass-p50 an/aus; Memory perf-measurement-on-m5).

## 5. Nicht geprüft
* Keine GPU-Zeit gemessen (Kostenspalte ist Schätzung aus dem Code und aus `docs/perf-audit/clouds-a1-a3-2026-09-30.md`: der Sonnenmarsch ist der teure Posten).
* Referenzbilder sind Fotos, kein Zielwert: Ziel ist "liest sich wie diese", nicht pixelnah.
* D3D11/D3D12/Vulkan zur Laufzeit; die Alt-Fallback-Shader (`HlslSources.h`, `shaders/sky.frag`) haben die Wolkenmodelle nicht.
* Dämmerungsfenster (TOD 0,70) nicht untersucht: das Bild sah dem Tag ähnlich, ob das Fenster oder die Wahl des TOD schuld ist, ist offen.

## Handoff
* Schritt 1139 fertig. Nächster Hive-Schritt 1140 (3D-Wolken verbessern): mit Maßnahme 1 beginnen, dann 2 und 3 (siehe Tabelle). Entscheidungen beim Menschen: Priorität Licht gegen Form, Performance-Budget für den Himmel, gewünschte Wolkentypen.
* Die Streifen sind der billigste sichtbare Gewinn; sie sind kein Formproblem.
