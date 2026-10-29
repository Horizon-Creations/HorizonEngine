# Wolken 2.0, Schritt 4a: Streifen und Licht der Realistic-Wolken (Thema 227, Hive-Schritt 1140, erster Teil)

Zweig `claude/wolken-2-0-wind-sprung-dome-hoehe`, nach `da9ba2dd`. Gilt nur für `applyClouds3DReal` (3D, Stil Realistic = Standard) in
`SkyShaderSource.h` und im Metal-Spiegel; Classic, Dome, Cirrus und Godrays sind unverändert (bitgleich, siehe unten).
Bild vorher links, nachher rechts: `docs/clouds-2-0-survey/realistic-before-after-2026-10-29.jpg` (Metal, Mittag, Morgen, Medium).

## Was geändert ist
* **Sprung-Offset:** `skyBlueDither` (16x16 Void-and-Cluster-Kachel, jeder 16x16-Block per Hash verschoben) statt `skyIgn` in der Realistic-Marschschleife.
  Die diagonale IGN-Struktur ist weg; was bleibt, ist feine unstrukturierte Körnung. Die Tabelle steht in GL und MSL gleich (Test).
* **Licht:** `lit = pow(sun, 1.15) * mix(0.50, 1.0, powder)` (Schattenseite fällt schneller ab), der Bauch bekommt Himmelslicht aus `baseSky`
  (blau oben, hell zum Horizont) statt einer festen Blau-Grau-Konstante, Sonnenfarbe der Oberseite `*1.45`, Höhenrampe `0.40..1.22`,
  Silberrand `*1.15`. Bei Overcast wird das Umgebungslicht heller und grauer (`ov`), damit geschlossene Decken nicht dunkler werden.

## Geprüft (Mac, Release, Metal + OpenGL, `HE_SKY_TIME` fest, AA aus)
* 16 Bilder Dome/Cirrus/Godrays/3D wie in Schritt 1: die 10 ohne Realistic-Wolken **bitgleich** zur Baseline, die 6 mit Realistic geändert wie gewollt.
* Realistic-Satz (8 Bilder je Backend: Low/Medium/High, Low-Res, Mittag, Morgen, Dämmerung, Overcast): Baseline reproduzierbar (zwei Läufe 16/16 identisch).
  Mittlere Nachbar-Differenz im Wolkenausschnitt (Maß für Körnung/Schraffur, Medium): 3,07 -> 2,93; Low 8,67 -> 8,20; High 2,14 -> 2,15; Low-Res 1,24 -> 1,51.
  Mittlere Helligkeit Overcast 139 -> 143 (vorher mit dem ersten Wurf dunkler und blauer, deshalb die Overcast-Korrektur), Medium 187 -> 182.
* Metal gegen OpenGL, gleicher Satz: mittlere Abweichung 0,5-2,2 von 255 (vorher 0,3-2,0).
* `ctest` voll 345/345 (privates HOME), neu: `test_culling` "blue-noise dither tile is one permutation, identical in GL and MSL"; `editor_help_audit.py --check` grün.

## Nicht geprüft / offen
* Die Körnung ist nicht weg: Blue Noise ersetzt die Linien durch unstrukturiertes Rauschen. Wirklich glatt wird es nur mit zeitlicher Akkumulation
  (Low-Res-Reprojektion für Realistic als Standard) oder mehr Schritten (Medium `qMinN` 26). Beides ändert Kosten, nicht in diesem Schritt.
* Keine GPU-Zeit gemessen (Kosten sollten gleich sein: gleiche Schrittzahl, ein Array-Fetch statt einer Hash-Rechnung).
* Dämmerung (TOD 0,72) bleibt blass-rosa: das Dämmerungsfenster ist unverändert.
* D3D11/D3D12/Vulkan zur Laufzeit; die Konstantenfeld-Übersetzung nach HLSL und SPIR-V läuft über `test_sky_shader`. Die Alt-Fallback-Shader haben das Wolkenmodell nicht.
* Der Metal-Spiegel `applyClouds3DReal` und die GL-Quelle sind getrennte Textkopien: Licht-Konstanten bei einer Änderung an beiden Stellen anpassen.

## Handoff
* Nächste Schritte laut Plan (`docs/clouds-2-0-survey-2026-10-29.md`, Abschnitt 4): Feindetail (Maßnahme 3), Wolkentypen (4), Horizont (6), Dome-Modus (5).
* Offene Entscheidung beim Menschen: Körnung per Low-Res-Standard oder mehr Schritte lösen (Kosten gegen Qualität).
