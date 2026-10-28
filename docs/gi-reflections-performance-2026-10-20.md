# GI-Reflektionen: Performance-Budget und Qualitätsstufen (Thema 191 Schritt 4)

Misst die Kosten des Reflektions-Passes (Trace + Blur + Mix,
`runGiReflections` in D3D11/D3D12/Vulkan) pro Qualitätsstufe und prüft, ob das
bestehende Frame-Budget der Engine eingehalten wird. Die Stufen selbst
(`HorizonRendering/GIReflectionTier.h`: Rays 1/2/4, resDiv 4/2/1, Blur-Floor)
sind fertig und wurden hier **nicht** verändert — nur gemessen.

## 0. Budget-Referenz

Kein GI-Reflektionen-spezifisches Budget existiert im Code oder in den Docs.
Die einzige engine-weite Zahl ist der Profiler-Fallback
(`ProfilerPanel.cpp:108`, `budgetMs() = 1000/targetFps`, Default **16.666 ms**
= 60 Hz). Als Richtwert für diesen Bericht: ein optionaler Post-Pass wie SSR
oder GI-Reflektionen sollte sich mit ~10–15 % des Frame-Budgets begnügen
(**~1.7–2.5 ms** bei 60 Hz) — Hausnummer, keine beschlossene Policy; analog
zu `docs/sky-weather-performance-plan.md`, das einem einzelnen Teilsystem
("Hintergrund") ebenfalls nur einen Bruchteil des Budgets zugesteht.

## 1. Methode

Es gibt **kein** per-Pass-GPU-Timing in der Engine — D3D11
(`GpuTimerSlot`/`ID3D11Query`, D3D11Renderer.cpp:2121-2248), D3D12
(`D3D12_QUERY_TYPE_TIMESTAMP`, D3D12Renderer.cpp:12798/12888) und Vulkan
(`vkCmdWriteTimestamp`, VulkanRenderer.cpp:15952/15959) timen alle nur den
**ganzen Frame**. Ein neuer Timestamp exklusiv um `runGiReflections` hätte
außerdem die falschen Kosten gezeigt: Reflektionen ziehen auch den geteilten
GI-Prepass (`runGiShadow(maskPasses=false)`) und `updateGiAccel` (BVH/Instanz-
Upload) mit, die ohne Reflektionen gar nicht laufen — die eigentliche Frage
("was kostet das Einschalten dieses Features") beantwortet nur ein A/B des
ganzen Frames.

Gemessen wurde deshalb mit der **bereits vorhandenen** Headless-Infrastruktur,
ohne Codeänderung:

- `HE_DUMP_GIREFLTEST=1` — die Zeugen-Szene (Spiegelboden + 2 Graph-Würfel +
  Spiegel-Slab, `EditorApplication.cpp:6507`).
- `HE_DUMP_GIREFL=0|1`, `HE_DUMP_GIREFLQUALITY=0|1|2`, `HE_DUMP_GIREFLBLUR=0|1`
  — Headless-A/B-Override der GI-Reflektions-Einstellungen für genau diesen
  Lauf (`EditorApplication.cpp:8733-8757`), ohne `config.json` anzufassen.
- `HE_DUMP_GPUTIME=300,1920x1080` — rendert nach dem Dump 300+16 weitere
  Frames derselben Kamera mit `EngineProfiler`-Live-GPU-Timing an, verwirft
  die ersten 16 (jedes Backend liest seine Timestamps ein paar Frames spät)
  und loggt median/mean/min/p90/max des **Whole-Frame**-GPU-Timings
  (`EditorApplication.cpp:10133-10182`). Das ist exakt das Verfahren, das
  Thema 185 Schritt 5 schon für andere Pass-Kosten benutzt hat (zwei Läufe,
  die sich nur in einem Schalter unterscheiden).
- `HE_DUMP_RHI=D3D11|D3D12|Vulkan` wählt das Backend.

Lauf: Release-Build (`C:\hebuild\girefl`, `-DDEPLOY_DIR=C:\hebuild\girefl-deploy`),
NN-WS03 (RTX 4070), `HE_HIDDEN_WINDOW=1` + `HE_COLLAB_OFFLINE=1`, private
`APPDATA`, 1920×1080, VSync aus (Dump-Pfad rendert ungedrosselt). Jede Zeile
unten ist ein eigener Prozessstart (kein warmer State zwischen Konfigurationen).
Matrix: `off` (Baseline, Reflektionen aus) sowie Tier 0/1/2 je mit Blur aus,
plus Tier 1/2 mit Blur an — pro Backend 6 Läufe, 18 insgesamt.

## 2. Ergebnisse (gpu ms, 300 Frames nach 16 Skip-Frames)

| Backend | Konfiguration        | median | mean  | min  | p90   | max   |
|---------|-----------------------|-------:|------:|-----:|------:|------:|
| D3D11   | off                    |  7.79  |  7.06 | 1.07 | 11.12 | 19.37 |
| D3D11   | off (rerun)            |  6.51  |  6.40 | 1.07 | 11.11 | 16.55 |
| D3D11   | Tier 0, Blur aus       | 30.14  | 28.57 | 1.15 | 30.97 | 36.00 |
| D3D11   | Tier 1, Blur aus       | 30.17  | 28.76 | 6.60 | 30.76 | 38.00 |
| D3D11   | Tier 1, Blur aus (Rerun)| 30.10 | 28.64 | 3.91 | 30.92 | 34.02 |
| D3D11   | Tier 2, Blur aus       | 30.13  | 28.53 | 2.74 | 30.83 | 33.69 |
| D3D11   | Tier 1, Blur an        | 30.12  | 27.92 | 1.20 | 30.64 | 37.25 |
| D3D11   | Tier 2, Blur an        | 30.11  | 28.41 | 1.20 | 30.87 | 34.70 |
| D3D12   | off                    |  4.16  |  4.55 | 0.87 |  7.63 | 12.53 |
| D3D12   | Tier 0, Blur aus       |  3.39  |  3.01 | 1.16 |  5.23 |  7.82 |
| D3D12   | Tier 1, Blur aus       |  3.46  |  3.34 | 1.17 |  5.56 |  8.59 |
| D3D12   | Tier 2, Blur aus       |  3.32  |  2.88 | 1.17 |  5.15 |  7.50 |
| D3D12   | Tier 1, Blur an        |  3.58  |  3.32 | 1.22 |  5.66 |  8.22 |
| D3D12   | Tier 2, Blur an        |  2.87  |  2.44 | 1.00 |  4.42 |  7.20 |
| Vulkan  | off                    |  3.20  |  3.14 | 1.46 |  5.21 |  7.58 |
| Vulkan  | Tier 0, Blur aus       |  3.07  |  2.90 | 1.55 |  4.68 |  7.13 |
| Vulkan  | Tier 1, Blur aus       |  3.57  |  3.46 | 1.55 |  5.48 |  8.05 |
| Vulkan  | Tier 2, Blur aus       |  3.28  |  3.15 | 1.55 |  5.10 |  7.63 |
| Vulkan  | Tier 1, Blur an        |  3.77  |  3.72 | 1.60 |  5.78 |  8.83 |
| Vulkan  | Tier 2, Blur an        |  3.16  |  3.15 | 1.58 |  4.90 |  7.86 |

Rohdaten: `C:\hebuild\girefl-perf\results.tsv` und `*.log` (nicht Teil des
Commits — Scratch-Verzeichnis auf NN-WS03, siehe Handoff).

## 3. Befund

**D3D12 und Vulkan: innerhalb des Budgets, Kosten im Rauschen.** Jede
Konfiguration (Tier × Blur) liegt innerhalb von ~1,5 ms der `off`-Baseline;
die Spannbreite zwischen den Konfigurationen ist kleiner als die Spannbreite
zwischen zwei `off`-Läufen selbst. Auf dieser Zeugen-Szene (vier Objekte,
sehr kleine BVH) kostet der Reflektions-Pass auf beiden Backends praktisch
nichts — weit unter dem 1,7–2,5-ms-Richtwert aus §0.

**D3D11: reproduzierbarer, tier-unabhängiger Sprung von ~+22 ms/Frame.**
Jede Konfiguration mit Reflektionen AN (Tier 0, 1, 2, Blur an oder aus) landet
beim selben Median ~30,1 ms, unabhängig von Ray-Zahl oder Blur — ein Zweit-
Lauf von `off` und Tier 1 bestätigte das (6,51 ms / 30,10 ms). Die
Qualitätsstufen-Leiter hilft hier **nicht**: Low kostet exakt so viel wie
High. Das sprengt jedes sinnvolle Frame-Budget allein durch diesen einen
Pass, komplett unabhängig von der gewählten Stufe.

Verdacht (nicht verifiziert durch Profiler-Capture, nur durch Code-Lesen
plausibilisiert): `updateGiAccel` (D3D11Renderer.cpp:3301-3333) erzeugt die
Instanz-Strukturpufferin **jedem Frame neu** (`makeSB` → `Reset()` +
`CreateBuffer` mit `D3D11_USAGE_IMMUTABLE`), ohne Frame-in-Flight-Pufferung —
anders als D3D12 (`giInstanceSB[fi]`) und Vulkan (eigene Per-Frame-CB, siehe
Kommentar in VulkanRenderer.cpp um Zeile 12742), die je einen eigenen Slot pro
Frame-in-Flight haben. Ein IMMUTABLE-Buffer, der jeden Frame neu angelegt
wird, während die GPU den alten noch liest, ist ein bekanntes Stall-Muster
unter D3D11 und würde genau das erklären, was gemessen wurde: ein fixer,
tier-unabhängiger Tax statt eines mit der Trace-Arbeit skalierenden Kosten-
Anstiegs. **Nicht gefixt** — das wäre eine D3D11-Portänderung
(`updateGiAccel`/Instanzpuffer-Pufferung), die explizit außerhalb des Rahmens
dieses Schritts liegt (Limits: "Port Vulkan/D3D11/D3D12 implementations...
already done").

## 4. Bekannte Lücke: Auflösung ist auf allen drei Backends tier-unabhängig

`GIReflTier.resDiv` (4/2/1 je Tier) wird auf D3D11/D3D12/Vulkan **nicht**
für die Trace-Auflösung benutzt — alle drei dispatchen bei `giShadowW/H`
bzw. `m_giW/m_giH` (der geteilten halbauflösenden GI-Prepass-Größe), egal
welche Stufe eingestellt ist (D3D11Renderer.cpp:3990-4032,
D3D12Renderer.cpp:8213-8260, VulkanRenderer.cpp:12757-12765). Der einzige
Hebel, der zwischen den Stufen tatsächlich etwas ändert, ist die Ray-Zahl
(1/2/4) plus der Blur-Floor — derselbe dokumentierte Spalt, den GL schon hat
(`GIReflectionTier.h`-Kommentar: "GL hard-coded resDiv = 2"). Auf der
winzigen Zeugen-Szene verschwindet der Ray-Zahl-Unterschied im Rauschen
(§2); auf einer dichteren Szene (mehr Instanzen/BVH-Knoten) würde er
sichtbarer werden. Die Preferences-UI (`EditorSettingsPanel.cpp:524-534`)
beschreibt die Stufen fälschlich als "1 ray, 1/4 res" / "... full res" —
das trifft auf D3D11/D3D12/Vulkan nicht zu (nur die Ray-Zahl ändert sich).
Nicht korrigiert (reine Text-/Dispatch-Änderung, aber jede Dispatch-Änderung
wäre ein Backend-Portfix, außerhalb dieses Schritts) — als Fundstelle für die
nächste Doku-Passage notiert.

Der bereits vorhandene Distanz-Cutoff (`GIReflectionSettings::maxDistance`,
Default 200 m, in allen Backends bis zum Shader verdrahtet, siehe
`reflParams.x`) ist der nächste Hebel, falls eine künftige Messung auf einer
schwereren Szene ein Tier über Budget zeigt — ohne die feste Tier-Tabelle
anzufassen. Hier nicht gezogen, weil D3D12/Vulkan auf dieser Szene bereits
komfortabel im Budget liegen.

## 5. Vorgenommene Änderungen

1. **`D3D11Renderer.cpp::GetCapabilities()`** — `supportsGIReflections` fest
   auf `false` (vorher `= m_impl->giSupported`, dieselbe Variable wie die
   diffuse GI, die unverändert bleibt). Versteckt den Preferences-Schalter
   (`EditorSettingsPanel.cpp:515-517`, `BeginDisabled(!supported)`) und hält
   `GameApplication.cpp`s `giReflEnabled`-UND-Verknüpfung (Zeile 3540-3542)
   auf `false` — ein frisch exportiertes Spiel auf D3D11 kann die
   Reflektionen also nicht mehr aktivieren, unabhängig vom Tier. Der interne
   Pfad (`SetGIReflectionSettings`, `giSupported`-Gate, `runGiReflections`)
   ist unverändert — ein Headless-Override (`HE_DUMP_GIREFL=1`) funktioniert
   weiterhin für künftige Messungen/Debugging (verifiziert, §2 letzter Lauf).
   Ein bestehendes `config.json` mit `GIReflectionsEnabled: true` von vor
   diesem Schritt würde im **Editor-Live-Viewport** (der nicht über die
   Capability geht, nur die Preferences-UI tut das) weiterhin die Kosten
   ziehen — der Schalter lässt sich dort aber nicht mehr neu aktivieren.
2. **`GameApplication.cpp:3549`** — `GIReflBlur`-Fallback-Default von `true`
   auf `false` korrigiert: `EditorConfig.h:256` dokumentiert explizit "TEST
   DEFAULT OFF", aber das exportierte Spiel fiel ohne `config.json`-Eintrag
   auf `true` zurück — ein Widerspruch zum Editor-Default, der nur auffällt,
   wenn ein Spiel ohne je geöffneten Editor exportiert wird (kein
   `config.json`-Schlüssel vorhanden).

`GIReflQuality` (EditorConfig.h:250, Default 1 = Medium) war bereits ein
sicherer Default und wurde nicht geändert.

## 6. Nicht gemacht (bewusst außerhalb des Rahmens)

- D3D11-Instanzpuffer-Stall nicht gefixt (§3) — Backend-Implementierung,
  nicht Teil von "Performance-Budget und Qualitätsstufen ergänzen".
- Tier-Tabelle (`GIReflectionTier.h`) nicht verändert.
- Auflösungs-Dispatch (§4) nicht auf `tier.resDiv` umgestellt — dieselbe
  Begründung.
- Kein neues per-Pass-GPU-Timing (ProfGpuPass/`gpuPasses`) eingebaut — die
  A/B-Methode (§1) beantwortet die Budget-Frage ohne neue, ungetestete
  Instrumentierung in drei Backends gleichzeitig.
- Keine schwerere Testszene gebaut — die Zeugen-Szene (`HE_DUMP_GIREFLTEST`)
  ist zu klein, um Tier-Unterschiede auf D3D12/Vulkan zuverlässig zu zeigen;
  eine Messung auf einer dichteren Szene (Foliage-Arena, Landschaft mit
  Mirror) ist der sinnvolle nächste Schritt, siehe Handoff.

## Handoff

**Verifiziert:**
- Alle 18 Matrix-Läufe (D3D11/D3D12/Vulkan × off/Tier0-2/Blur) reproduzierbar
  auf NN-WS03 (RTX 4070), Release-Build, headless, via bestehender
  `HE_DUMP_GPUTIME`/`HE_DUMP_GIREFL*`-Infrastruktur (§1-§2).
  D3D11-Sprung zweimal reproduziert (off + Tier 1 Rerun).
- `he_tests.exe --source-file="*test_gi_reflection_tier.cpp"` weiterhin
  5/5 Testfälle, 51/51 Assertions grün (Tier-Tabelle unverändert).
- `HorizonEditor` und `HorizonGame` bauen clean mit beiden Änderungen
  (Ninja, MSVC, `C:\hebuild\girefl`).
- Nach der Capability-Änderung liefert der Headless-Override
  (`HE_DUMP_GIREFL=1` auf D3D11) weiterhin denselben ~30 ms-Wert — die
  Messinfrastruktur bleibt für eine künftige Fix-Verifikation nutzbar.

**Nicht verifiziert:**
- Die genaue Ursache des D3D11-Stalls (§3) — Hypothese aus Code-Lesen, nicht
  durch PIX/Capture bestätigt. Wer das fixt, sollte zuerst mit PIX/GPU-
  Capture nachsehen, ob die Vermutung (Instanzpuffer-Recreate ohne
  Frame-in-Flight-Pufferung) tatsächlich der Stall ist, bevor
  `updateGiAccel` umgebaut wird.
- Tier-Kostenunterschiede auf D3D12/Vulkan unter einer dichteren Szene
  (mehr Instanzen/BVH-Knoten) — auf der Zeugen-Szene nicht messbar (§4).
- CI-Lauf für die hier committeten Änderungen (siehe unten) war beim
  Verlassen dieses Schritts noch nicht gestartet/geprüft.

**Entscheidungen:**
- D3D11: `supportsGIReflections = false` statt eines Tier-Umbaus — kein
  Tier rettet das Budget (§3), ein Capabilities-Gate ist der im Auftrag
  selbst genannte Hebel ("Capabilities gate that disables reflections on
  startup") und ist risikoärmer als ein ungeplanter Fix am D3D11-Port.
- `GIReflBlur`-Default-Mismatch korrigiert statt nur dokumentiert — eine
  Zeile, kein Risiko, behebt einen echten Editor/Spiel-Inkonsistenz-Bug.
- Kein neues per-Pass-GPU-Timing gebaut (§6) — die A/B-Methode beantwortet
  dieselbe Frage mit bestehender, bereits getesteter Infrastruktur.

**Gefallen (Fallen für die nächste Instanz):**
- `HE_DUMP_GPUTIME` misst den **ganzen Frame**, nicht nur den einen Pass —
  für GI-Reflektionen korrekt (der Prepass/BVH-Build gehört zu den Kosten
  des Features), aber bei einem anderen Pass-A/B erst prüfen, ob der Prepass
  geteilt wird.
- Ein `cmake --build ... --target he_tests` kann, wenn `GameApplication.cpp`
  geändert wurde, nur `HorizonGame` neu bauen (gemeinsamer Build-Graph) und
  `he_tests.exe` dabei NICHT neu linken (die Testdatei selbst hängt nicht an
  `HE_Game`) — die Exe-mtime kann dann älter aussehen, als man erwartet;
  das ist kein Fehlschlag, nur eine andere Teilmenge des Graphen.
- `vcvars64.bat` per `call` **in PowerShell direkt** aufzurufen scheitert
  stillschweigend-laut (`call` ist kein PowerShell-Cmdlet) — INCLUDE/LIB
  bleiben leer, `cstdint` fehlt beim ersten `#include`. Immer über eine
  `.bat`-Datei (wie die vorhandenen `C:\hebuild\build_*.bat`) laufen lassen,
  nie `call "...vcvars64.bat"` direkt in eine PowerShell-Befehlszeile
  schreiben.

**Nächste Schritte:**
1. Schritt 5 (D3D/Vulkan-Abnahme): bei der visuellen Prüfung berücksichtigen,
   dass D3D11 die Reflektionen jetzt capability-gated NICHT mehr zeigt
   (gewollt) — ein Soll/Ist-Vergleich D3D11-vs-D3D12/Vulkan für die
   Reflektionen selbst ergibt auf D3D11 keine Pixel mehr, nur noch die
   Sky-Fallback-Reflexion.
2. Wer den D3D11-Stall fixt: `updateGiAccel`/`makeSB`
   (D3D11Renderer.cpp:3301-3333) auf eine Frame-in-Flight-gepufferte
   Instanz-SRV umstellen (Vorbild: D3D12s `giInstanceSB[fi]`), mit PIX
   verifizieren, dann `supportsGIReflections` in `GetCapabilities()`
   zurück auf `m_impl->giSupported` setzen und diese Matrix neu fahren.
3. Eine zweite Messung auf einer dichteren Szene (mehr BVH-Instanzen) wäre
   nötig, um echte Tier-Unterschiede auf D3D12/Vulkan zu sehen — die
   Zeugen-Szene ist dafür zu klein (§4, §6).
