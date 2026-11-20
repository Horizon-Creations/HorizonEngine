# Auto-LOD: GPU-Zeit-Breakdown und eine Optimierung (Thema 331, Schritt 8)

Stand 08.11.2026, Windows/D3D12 auf NN-WS03 (RTX 4070). Baut auf `docs/auto-lod-abnahme-2026-11-08.md`
(Schritt 7, Metal/M5). **Die Poly-Haven-Testszene existiert nur auf dem Mac, der Schritt 1-7 gefahren hat**
(`~/HorizonEngineTestAssets/foliage-test-project`, ein macOS-Home-Pfad) — auf diesem Windows-Geraet nicht
vorhanden (volle `C:`-Suche, `hive_read_topic` zeigt alle Schritte 1-7 als Metal/M5-Laeufe). Vorgehen mit
der Queen abgesprochen (`hive_request` #168): Breakdown per Subtraktion aus den Mac-Zahlen, Fix im Code,
Vorher/Nachher-Messung auf einem **Windows/D3D12-Proxy** (klar gekennzeichnet, kein Ersatz fuer die echte
Szene), RS2-60fps ist out of scope (siehe Abschnitt 1).

## 1. Breakdown per Subtraktion (keine neue Messung, nur die Zahlen aus Schritt 1 und 7 neu gelesen)

* Schritt 7 Endkonfiguration (Kette + Impostor), RS2: GPU gesamt 27,1-29,8 ms, davon **Scene (Hauptpass)
  23,5-25,7 ms = 85-90 %**, Hauptpass-Dreiecke nur 0,14 Mio (`docs/auto-lod-abnahme-2026-11-08.md` Abschnitt 3).
  Das bestaetigt die Vermutung des Schritts: der Rest ist **Pixel-/Shading-Kosten, nicht Geometrie**.
* Schritt 1 Baseline (`docs/auto-lod-baseline-2026-10-29.md` Tabelle 4.1): **v6 (kein Foliage, nur Boden)
  liegt bei 20,9 ms GPU / 17,5 ms Scene bei RS2 — schon ueber dem 16,7-ms-Budget fuer 60 fps.** Keine
  Foliage-Massnahme kann RS2 unter 16,7 ms bringen, der Boden allein ist dort schon zu teuer. **RS2-60fps ist
  damit strukturell ausserhalb des erreichbaren Bereichs** (mit der Queen abgestimmt) — das 60fps-Ziel gilt
  nur fuer die **Standard-RenderScale**, wo v6 60,1 fps / 16,7 ms p50-Frametime erreicht (Tabelle 4.3).
* Folgerung: der Hebel fuer "echte" 60 fps bei Standard-RenderScale ist die **Pixelkoste der Foliage-Materialien
  im Hauptpass** — genau das, was dieser Schritt angeht.

## 2. Code-Untersuchung: wo die Pixelkosten herkommen

Masked-/Cutout-Materialien (alle 5 Backends: EIN GLSL-String aus `src/HE_Core/src/MaterialGraph/
MaterialGraph.cpp`, per SPIRV-Cross nach MSL/HLSL fuer Metal/D3D cross-compiliert — Kontrollfluss-Reihenfolge
bleibt beim Cross-Compile erhalten, der Befund gilt also fuer alle Backends gleich):

* `generateFragment()` emittierte bisher **BaseColor, Metallic, Specular, Roughness, Emissive, AO und erst
  danach den Opacity/Mask-Pin** (Zeilen ~2502-2528 vor diesem Fix), und den `if (mask < cutoff) discard;`
  erst ganz am Ende des Funktionskoerpers (vor `heLitP`, aber nach JEDER Pin-Textur). Fuer eine **einfache**
  Cutout-Karte (Schritt 4: ein Texturkanal, Alpha=Basisfarbe) kostet das wenig. Fuer **echte Pflanzengeometrie**
  (volle PBR-Graphen aus `PbrMaterialImport.cpp::buildGraph`, alphaMode MASK, Basis+Normal+ORM+Emissive) kostet
  es: Normalkarten-Abtastung (Screen-Space-Tangentenrahmen, `MaterialGraph.cpp:526-543`) und den ORM-Split
  (Metallic/Roughness/AO) **fuer JEDES Fragment, auch fuer die, die der Cutoff gleich wegwirft.**
* Es gibt **kein** Early-Depth-Prepass, kein Alpha-to-Coverage und keinen Per-Draw-GPU-Timer feiner als die
  Pass-p50-Zahlen in den bisherigen Berichten (durchsucht: `src/HE_Rendering`, `src/HE_Core`) — die Pass-p50-
  Zahlen sind das feinste vorhandene Werkzeug ohne neue Instrumentierung.

## 3. Fix: Masked-Discard vor BaseColor/Metallic/.../Normal

`src/HE_Core/src/MaterialGraph/MaterialGraph.cpp`, `generateFragment()`: der Opacity/Mask-Pin wird jetzt
**zuerst** ausgewertet (vor BaseColor/Metallic/Specular/Roughness/Emissive/AO), und der `discard` direkt
danach in `c.body` geschrieben — statt erst am Ende der Funktion. Der Mask-Wert liest in den meisten
Importgraphen denselben Basisfarb-Textur-Alphakanal, den BaseColor sowieso braucht (`PbrMaterialImport.cpp`
Zeile ~602-614); die Codegen-Node-Cache (`EmitCtx::outVar`) dedupliziert den Sample, es entsteht **kein**
zusaetzlicher Texturzugriff fuer ueberlebende Fragmente. Fuer verworfene Fragmente entfaellt die
Normalkarten- und ORM-Abtastung komplett. Ein bestehendes Muster (`refsPending()` + `flushGuards()`, schon an
drei Stellen im selben File fuer genau dieses Problem benutzt) stellt sicher, dass ein ueber ein If-Node
verzoegerter Guard-Block vor dem fruehen Discard-Statement geflusht wird, falls der Mask-Wert von einem
solchen Block kommt — sonst wuerde die neue `discard`-Zeile eine noch nicht zugewiesene Variable lesen.
Translucent- und Opaque-Materialien sind **byteweise unveraendert** (nur der Masked-Zweig wurde verschoben).

Keine Aenderung an `FoliageExtract.cpp` oder der Render-Pipeline: der Fix sitzt in der einen Stelle, die jedes
Masked-Material jedes Backends durchlaeuft.

## 4. Messung: Windows/D3D12-Proxy (NICHT die Poly-Haven-Szene)

**Aufbau** (reproduzierbar, keine externen Assets): `asset_compiler` importiert ein handgeschriebenes
glTF-Quad (`C:/hw331/src_assets/plant.gltf`, per Python/PIL erzeugt) mit einem **echten** Masked-PBR-Material
(1024x1024 Basisfarbe+Alpha-Schachbrett, 1024x1024 Normalkarte, 1024x1024 gepackte ORM-Textur — exakt die Pin-
Form, die `PbrMaterialImport::buildGraph` fuer ein alphaMode-MASK-Pflanzenmaterial mit Normalkarte baut).
3000 ueberlappende, mit dem Abstand mitskalierte Karten fuellen die Kamera (starkes Alpha-Test-Overdraw, kein
Shortcut im Szenenaufbau). `he_perf_capture.py --detailed --set RenderScale=2`, D3D12, RTX 4070, HE_HIDDEN_WINDOW=1,
120 Warmup / 300 Frames, EIN Editor-Binary vor/nach dem Fix (Source reverted → gebaut → gemessen → Fix wieder
angewendet → gebaut → gemessen; Asset-UUIDs bleiben über den Re-Import stabil, keine Szenen-Aenderung zwischen
den Laeufen).

| | Scene p50 | GPU gesamt p50 | Scene p90 (Tail) | GPU gesamt max |
|---|---|---|---|---|
| **Vorher** (unveraendertes `generateFragment`) | 56,56 ms | 64,84 ms | 333,7 ms | 4602 ms |
| **Nachher** (dieser Fix) | **8,91 ms** | **11,38 ms** | **12,14 ms** | 59,1 ms |
| Veraenderung | **-84,3 %** | **-82,5 %** | **-96,4 %** | **-98,7 %** |

* Nachher-Scene-p50 (8,91 ms) trifft fast genau den Nachher-Minimumwert (8,90 ms) — stabil, kein Ausreisser.
  Vorher streut dramatisch (Min 8,88 ms, aber p90 333,7 ms, Max 4602 ms): die zusaetzliche Register-/ALU-Last
  von Normalkarte+ORM auf JEDEM der 3000 ueberlappenden Fragmente treibt die Occupancy/den Treiber
  gelegentlich in sehr teure Einzelframes — der Fix macht die Szene nicht nur schneller, sondern auch
  **berechenbar**.
* **Nicht verglichen werden darf** mit den ms-Zahlen aus Schritt 1/7 (Mac, Metal, andere Szene, andere
  Hardware). Die **Code-Pfad-Identitaet** (ein GLSL-String, cross-compiliert fuer alle 5 Backends) ist der
  Grund, warum dieses Ergebnis trotzdem etwas ueber die echte Szene aussagt: derselbe `discard`-vor-statt-
  nach-den-Texturen-Effekt trifft jedes Masked-Material mit Normalkarte, also die echten Pflanzen-LOD-Stufen
  0/1 der Poly-Haven-Szene (volle PBR-Textur-Saetze) — und trifft die **Impostor-Stufe aus Schritt 4 kaum**
  (nur eine Textur, Alpha=Basisfarbe, schon vorher fast nichts vor dem Discard).

## 5. 60fps-Ziel: Verdict

* **RS2:** ausserhalb des Scopes (Abschnitt 1, mit der Queen abgestimmt) — der Boden allein ist dort schon
  ueber dem Budget.
* **Standard-RenderScale, echte Poly-Haven-Szene:** **nicht nachgemessen** — dieses Geraet hat keinen Zugriff
  auf die Szene. Der Windows-Proxy zeigt eine sehr grosse (82-84 %), stabile Verbesserung fuer exakt den
  Materialtyp (Masked + Normalkarte + ORM), der in der echten Szene die LOD-Stufen 0/1 jeder Pflanze ist;
  die Impostor-Fernstufe (Schritt 4, schon nur eine Textur) profitiert kaum. Ob das in der echten Szene die
  Luecke zu 60 fps (Schritt 7: 41-64 fps schwankend bei Standard-RS) schliesst, **ist nicht bewiesen** — dafuer
  braucht es einen Nachlauf von Schritt 7s Methodik (gleiche Kamera, gleiche Projekt-Klone) mit diesem Fix auf
  dem Mac. Empfehlung in den Next Steps.

## 6. Tests

* `C:/hw331/tests/he_tests.exe` (Release, scratch `%APPDATA%`, dieser Fix im Baum): **6010 von 6010 Faellen,
  0 Fehler, 19 skipped** (keine Regression; die 19 sind laufzeitbedingte Skips ohne Zusammenhang zu diesem
  Fix — kein `HE_TEST_RELAY`/`HE_TEST_TEAM_SERVER`/`HE_TEST_SFTP`/`HE_GITHUB_LIVE_*` gesetzt, plus ein
  Plattform-Skip in `test_app_todo.cpp`, laut `C:/hw331/full_tests_after.log`). `*MaterialGraph*`-Filter: 13/13 Faelle,
  184/184 Assertions — insbesondere "Blend modes: opaque forces alpha 1, masked discards, translucent feeds
  alpha" (prueft nur, DASS `discard` im GLSL vorkommt, nicht die Position — durch den vollen Lauf trotzdem
  indirekt pruefbar: kein `test_foliage_lod`/`test_impostor`/`test_auto_lod`/`test_graph_instancing` rot).
  `editor_help_audit.py` nicht erneut gelaufen (dieser Schritt beruehrt keinen Editor-Knopf).
* **Wichtige Falle vermieden:** `he_tests.exe` ueberschreibt das reale `%APPDATA%\HorizonEngine\config.json`
  mit Defaults (bekannt, `vulkan-d3d-baseline-noise`-Lektion). Ein erster Lauf ohne Scratch-`APPDATA` wurde
  rechtzeitig per Zeitstempel-Check als unbeschaedigt erkannt und der Hintergrundlauf gestoppt, bevor ein
  zweiter (voller) Lauf mit `$env:APPDATA="C:/hw331/appdata"` wiederholt wurde — keine Beschaedigung der
  echten Konfiguration dieses Geraets.

## 7. Nicht verifiziert

* Die echte Poly-Haven-Testszene (kein Zugriff von diesem Geraet) — **der groesste offene Punkt**.
* Metal/OpenGL/Vulkan/D3D11: nur D3D12 gemessen. Der Code-Pfad ist backend-uebergreifend identisch (ein
  GLSL-String, SPIRV-Cross), aber nicht unabhaengig auf den anderen vier Backends gemessen.
* CI nicht gestartet.
* Bildqualitaet: keine Sichtpruefung noetig (die Aenderung reordnet nur WANN eine Textur gelesen wird, nicht
  WAS sie liefert — `common`-Snapshot in `generateFragment` bleibt byteweise aequivalent fuer Translucent/
  Opaque, fuer Masked nur anders sortiert; der volle Testlauf inkl. aller Material-Graph- und Foliage-Tests
  ist gruen).

## Handoff

- **Topic / step:** 331, Schritt 8 von urspruenglich 7 (GPU-Breakdown + Optimierung). Branch
  `claude/auto-lod-und-sichtbarkeitsgetriebenes-rendern-f-r-foliage-un`.
- **Done:** Breakdown per Subtraktion (Scene-Pass dominiert, Boden allein sprengt RS2-Budget schon),
  Code-Ursache gefunden (spaeter Discard nach allen Textur-Pins in `MaterialGraph.cpp`), Fix umgesetzt
  (Mask-Pin + Discard vor BaseColor/Metallic/.../Normal, mit `refsPending`/`flushGuards`-Absicherung),
  Windows/D3D12-Proxy-Messung zeigt -84 % Scene-p50 / -82 % GPU gesamt / -96 % Tail, voller `he_tests`
  gruen (6010/6010).
- **Result in one line:** RS2-60fps ist strukturell out of scope (Boden allein 20,9 ms); die Ursache fuer
  die Pixelkosten ist ein "spaeter Discard" in der geteilten Material-Codegen, behoben in
  `MaterialGraph.cpp`, auf einem Windows/D3D12-Proxy mit -82 bis -96 % GPU-Zeit belegt; Wirkung auf der
  echten Poly-Haven-Szene (Standard-RenderScale, Schritt-7-Methodik) nicht nachgemessen.
- **Files:** `src/HE_Core/src/MaterialGraph/MaterialGraph.cpp` (Fix), `docs/auto-lod-gpu-breakdown-2026-11-08.md`
  (dieser Bericht). Keine Aenderung an `FoliageExtract.cpp`, keine Pipeline-Architektur-Aenderung.
- **Verified:** voller `he_tests` (Release, C:/hw331, scratch APPDATA) 6010/6010; `*MaterialGraph*`-Filter
  13/13; Vorher/Nachher auf dem Windows-Proxy (Quelle reverted→gebaut→gemessen→Fix wieder angewendet→
  gebaut→gemessen, identische Szene/Kamera/Editor-Konfiguration).
- **Not verified:** die echte Poly-Haven-Szene (kein Zugriff), Metal/OpenGL/Vulkan/D3D11, CI,
  Standard-RenderScale-60fps-Ziel auf der echten Szene.
- **Decisions taken:** Fix in der geteilten `MaterialGraph.cpp`-Codegen statt in `FoliageExtract.cpp`
  (der Discard-Ort ist backend- und material-uebergreifend, nicht foliage-spezifisch). Windows/D3D12-Proxy-
  Szene statt Download/Sync der Mac-Testassets (Queen-Entscheidung, `hive_request` #168). RS2 aus dem
  60fps-Scope genommen (Queen-Entscheidung, selbe Anfrage).
- **Decisions suggested, not taken:** Mac-Bee soll Schritt 7s Messmethodik mit diesem Fix wiederholen
  (gleiche Kamera/Projekt-Klone), um die reale Standard-RS-60fps-Luecke zu schliessen oder zu belegen, dass
  sie bleibt; D3D11/Vulkan/Metal/GL unabhaengig auf dem Proxy nachmessen, falls ein Zweifel am
  Cross-Compile-Argument aufkommt.
- **Traps:** `he_tests.exe` ueberschreibt `%APPDATA%\HorizonEngine\config.json` — immer Scratch-`APPDATA`
  setzen (vor diesem Schritt fast passiert, per Zeitstempel rechtzeitig erkannt). `gen_reference_world.py
  --foliage` (von der Queen vorgeschlagen) nutzt NO_ASSET/Opaque-Material — kein Alpha-Test, reproduziert
  das Overdraw-Problem NICHT; der Proxy hier importiert stattdessen ein echtes Masked-PBR-Material per
  `asset_compiler`. Quad-Karten muessen mit dem Abstand mitskaliert werden, sonst schrumpft die
  Bildschirmabdeckung der hinteren Lagen und das Overdraw-Signal verschwindet im Rauschen (erste Version
  mit 300 Lagen/konstanter Groesse: Effekt nicht von Rauschen unterscheidbar). `asset_compiler <src_dir>
  <out_dir>` braucht kein Projekt; UUIDs sind über Re-Import stabil (aus dem Dateinamen/Pfad abgeleitet).
- **Next steps:** 1. Queen: Mac-Nachlauf von Schritt 7 mit diesem Fix (Standard-RenderScale, echte Szene).
  2. Falls die Luecke bleibt: naechster Kandidat waere Schritt 1s eigener Vorschlag (Ueberzeichnungs-/
  Early-Z-Test) oder ein echter Early-Depth-Prepass fuer Masked-Materialien (Architekturaenderung, aus dem
  Scope dieses Schritts). 3. CI starten.
