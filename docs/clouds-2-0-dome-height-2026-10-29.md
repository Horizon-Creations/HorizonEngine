# Wolken 2.0, Schritt 2: Höhe der Dome-Wolken (Thema 227, Hive-Schritt 1138)

Zweig `claude/wolken-2-0-wind-sprung-dome-hoehe`, baut auf `3b3d0afc` (Wind-Drift, `docs/clouds-2-0-wind-drift-2026-10-29.md`).

## Das Problem
`cloudHeight` wirkt nur im 3D-Modus (`cloudMode 1`). Die Dome-Wolken (`cloudMode 0`) liegen auf einer festen Einheits-Halbkugel
(Slab bei Höhe `kCloudBase..kCloudTop`), sie wirkten deshalb immer gleich nah und gleich groß.

## Was geändert ist
* Neues Feld `cloudDomeHeight` (Standard **1,0** = das heutige Bild): ein Faktor auf die Noise-Projektion des Domes. Größer: die Wolken
  wirken höher, kleiner und gegenüber ihrer eigenen Größe langsamer (der Drift wird nicht mitskaliert). Kleiner: nah und groß.
* Wo es steht (jeweils neben `cloudHeight`): `EnvironmentSettings.h` (HE_Core), `EnvironmentComponent.h`, `EnvironmentPush.cpp`,
  `EngineApi.h` (`HE_ENV_FIELDS_FLOAT`, damit gibt es `env.getCloudDomeHeight`/`setCloudDomeHeight` für Lua, Python, HorizonCode),
  `HcNodeDocs.cpp`, Inspector (Slider "Dome Cloud Height", nur im Dome-Modus, 0,25..3,0), `EditorHelp.cpp`, Dump-Schlüssel
  `HE_DUMP_CLOUDDOMEHEIGHT`. Die Szene speichert es über dieselbe Feldliste wie alle Umgebungsfelder (alte Szenen laden mit 1,0).
* Transport: KEIN neuer Slot. `SkyFrameParams::drift.w` (war frei), in der CPU auf 0,1..16 geklemmt. Vulkan/D3D-Prelude
  `#define uCloudDomeHeight heSky.drift.w`, OpenGL ein eigenes Uniform (`m_uSkyCloudDomeHeight`, beide Push-Stellen), Metal `float4 drift`
  (`.w` = Faktor, `.xyz` = Drift) durch alle Cloud-Funktionen.
* Shader: an den fünf Dome-Stellen `pos * kCloudScale` -> `pos * (kCloudScale * H)` (`cloudDensity`, `cloudShadowDensity`, der Marsch in
  `applyClouds`, `godrayClear`) und `domeDeckXZ`. Der vertikale Dichteverlauf liest weiter die unskalierte Höhe. Die Wolkenschatten
  (Dome-Modus) und die Godrays laufen über dieselben Funktionen und folgen damit automatisch.

## Geprüft (Mac, Release, Metal + OpenGL)
* Standardwert 1,0: dieselben 16 gepinnten Bilder wie beim Wind-Drift-Schritt, **16/16 bitgleich** zur Baseline vom Release-Editor `31d6253c`.
* Der Faktor wirkt und ist monoton: 0,5 / 1,0 / 2,0 / 3,5 liefern deutlich verschiedene Bilder (mittlere Abweichung zu 1,0: 16 / 0 / 18 / 24 von 255):
  kleinerer Faktor = größere, weichere, nähere Wolken, größerer = kleinere, feinere. Metal gegen OpenGL bei gleichem Faktor: 0,29 / 0,76 / 1,74 / 3,05 von 255.
* `ctest` voll (siehe Handoff-Post im Hive), neu: `test_weather` (Übersetzung), `test_scene_serializer` (Rundreise), `test_culling`
  (`drift.w` = Faktor, ein Wert 0 wird nicht zu einem kollabierten Dome), `test_engine_api` (eine Float-Zeile mehr), HLSL/SPIR-V über `test_sky_shader`.

## Nicht geprüft / bekannt
* Ab etwa 2,0 wird der Marsch körnig: die Schrittweite folgt der Slab-Geometrie, nicht dem skalierten Noise. Deshalb endet der Slider bei 3,0.
  Wer größere Werte per Script setzt, bekommt Körnung (Klemme erst bei 16). Cure wäre die Schrittzahl mit dem Faktor zu erhöhen.
* Metal und OpenGL weichen bei großem Faktor stärker voneinander ab (3,05/255 bei 3,5, gleiche Ursache, nicht untersucht).
* D3D11/D3D12/Vulkan zur Laufzeit (Windows). Die Fallback-Shader ohne shaderc (`HlslSources.h`, `shaders/sky.frag`) ignorieren den Faktor.
* Im laufenden Editor den Slider nicht bedient; kein Bild von Wolkenschatten am Boden mit Faktor ungleich 1.

## Handoff
* Nächste Schritte (Hive 227): 1139 Bestandsaufnahme und Plan der 3D-Volumetrik (Befund aus meinen Renderern: zu glatte Blobs, flache
  Beleuchtung, Streifenmuster auf den Oberflächen, siehe Post im Hive), 1141 Abnahme.
* Entscheidung: bewusst ein eigener Faktor statt `cloudHeight` auch für den Dome zu verwenden: das hätte jede Szene mit gesetztem
  `cloudHeight` im Dome-Modus verändert.
