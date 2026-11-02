# Mesh/Textur-Backen beim Export (Thema 228, Punkt 6)

Auftrag (Nutzer, 2026-11-02): Bei statischen Meshes, deren Materialien nur Texturen zeigen, soll der Export die Texturen auf das Mesh backen, damit
die Laufzeit weniger zu tun hat. Alle drei Formen, pro Mesh abschaltbar im Mesh Viewer: (1) Vertex-Farben, (2) Texturen zusammenlegen,
(3) eine UV-Textur pro Mesh. (2) und (3) "wenn möglich", auch wenn das Pak dadurch größer wird, solange es zur Laufzeit schneller ist.

## 1. Bestand (geprüft am 2026-11-02)

- `StaticMeshAsset` (`Assets.h:135`): `vertices`, `normals`, `uvs`, `indices`, `sections` (MSEC: Indexbereich + Material). Gebacken im Pak als
  `CHUNK_MVBO` = `interleaved`, 8 Floats pro Vertex (pos3 + norm3 + uv2). **Keine Vertex-Farben**, der glTF-Import liest kein `COLOR_0`.
- `vColor` im Material-Shader ist nur der Tint des Draws (`u.color.rgb`, `MaterialShaderLibrary.cpp:145`), keine Mesh-Eigenschaft.
- Das Wort `interleaved` kommt in 16 Renderer-/Szenen-Dateien vor (GL, Metal, Vulkan, D3D11/12, `ScenePick`, `GiBvh`, `OcclusionCuller`, `LODSystem`,
  `PhysicsWorld`, `NavigationSystem`). Eine Änderung des 8-Float-Layouts ist deshalb keine Option.
- Kein UV-Entfalter im Code (kein xatlas). Der Mesh Viewer zeigt nur vorhandene UVs.
- Export: `HpakWriter` schreibt `.hasset`-Bytes je UUID, Material- und Mesh-Pfade werden beim Packen in UUIDs gebacken (MREF/MRFU, MSEC),
  Texturen gehen durch `cookTexture`. Der Erreichbarkeitsfilter (Abschnitt 8 des Audits) lässt eine Textur weg, auf die nichts mehr zeigt.

## 2. Entscheidungen

1. **Schalter pro Mesh:** drei Flags am Mesh-Asset (neuer Tail-Chunk, kein Versions-Bump, Test "altes Format lädt"). Gesetzt in der Werkzeugleiste des
   Mesh Viewers (`StaticMeshEditorPanel`), gelesen vom Exporter. Vorgabe: (2) und (3) an, (1) aus.
2. **Ein Ergebnis pro Mesh, feste Reihenfolge:** (1) Vertex-Farben, wenn eingeschaltet und zulässig; sonst (2) verlustfrei zusammenlegen; sonst (3)
   resampeln; sonst unverändert. Nie zwei Formen auf einem Mesh.
3. **(2) und (3) sind ein Packer:** je Sektion die Texturfläche seines UV-Rechtecks in eine gemeinsame Textur legen, UVs des Meshes umrechnen, Sektionen
   zu einer verschmelzen. (2) ist der verlustfreie Fall (alle UV in [0,1], Quelltexel 1:1 kopiert, kein Tiling). (3) löst Tiling auf (Rechteck wird
   mehrfach gebacken, deshalb größer) und skaliert unter eine Grenze (Vorgabe 4096 pro Seite), wenn es sonst zu groß wird.
4. **"Einfaches Material" = ein glatter PBR-Texturensatz** (korrigiert nach Rückfrage des Nutzers: Albedo allein gibt es bei importierten Modellen
   kaum). Opaque Surface, deren Pins nur in diesen Formen belegt sind: Base Color und Emissive je Konstante oder Texture Sample (mal Faktor),
   Metallic, Roughness und Ambient Occlusion je Konstante oder ein Kanal einer Textur (Split RGBA, mal Faktor; AO als Lerp(1, Kanal, Stärke)),
   Specular nur Konstante, Normal nur eine Normal-Map, jedes Sample mit eigenem UV-Knoten (Tiling/Offset). Opacity, WPO und Displacement
   unverbunden, kein Switch, keine Parameter, keine Berechnung in der UV. Der Standard-Shader zählt als Albedo-Textur mal `baseColor` plus
   die Skalare. `HE::classifySimpleTextureMaterial` (`SimpleTextureMaterial.h`), getestet gegen ein echtes, vom glTF-Import erzeugtes Material.
   Ein Texture Sample mit offenem UV-Pin liest den Pin-Standard `vec2(0)` (einen Texel), nicht die Mesh-UV; nur die Normal-Map fällt auf die
   Mesh-UV zurück. Ersteres ist deshalb kein Kandidat.
   **Folge für die Bakes:** Jede Rolle (Albedo, Normal, Metallic/Roughness/AO, Emissive) wird mit denselben Rechtecken in eine eigene Atlas-Textur
   gelegt, Metallic/Roughness/AO aus ihren Kanälen in eine ORM-Textur (R = AO, G = Roughness, B = Metallic). Ein Abschnitt ohne Normal-Map bekommt
   eine flache Normale (0,5 / 0,5 / 1) im Rechteck, eine Konstante füllt das Rechteck. Vertex-Farben (Form 1) gibt es nur, wenn nichts außer
   Base Color eine Textur hat und keine Normal-Map da ist (sonst ginge das Detail verloren).
5. **Abgeleitete Assets:** Der Export legt ein Material (eigene UUID) und eine Textur (eigene UUID) an und biegt `MSEC.materialId` um. Die
   Original-Textur fällt über den Erreichbarkeitsfilter heraus, wenn sonst nichts auf sie zeigt.
6. **Vertex-Farben ohne Layoutänderung:** ein zweiter, optionaler Puffer (RGBA8 je Vertex, neuer Chunk `MVCL`) neben `interleaved`, den nur GL und Metal als
   zusätzliches Attribut/Vertex-Puffer binden. CPU-Verbraucher von `interleaved` sehen nichts davon. Der Export backt Vertex-Farben nur, wenn die
   Shader-Backend-Maske des Exports ausschließlich GL und/oder Metal enthält (wie die PSHD-Zwillinge), damit Vulkan/D3D nie ein Mesh bekommen, das sie
   nicht zeichnen können. Auf dem Mac nicht prüfbar für Vulkan/D3D, deshalb dort nicht angefasst.
7. **Qualität der Vertex-Farben:** Die Farbe kommt aus dem Mip, dessen Texelgröße zur Dreiecksgröße passt, nicht aus der obersten Stufe an einem Punkt.
   Meshes, deren Dreiecke deutlich größer als ein Texel sind, bleiben unverändert (Zulässigkeit statt Verschmieren).

## 3. Prüfmaßstab (vorab, damit niemand einen Hash-Unterschied für einen Fehler hält)

Die Frames sind **nicht** bitgleich, das Backen verändert das Bild absichtlich (Vertex-Farben, Resampling). Geprüft wird: 0 Fehler im Log, die
Original-Textur fehlt im Pak-TOC, weniger Draw-Calls/Texturbindungen, und ein Bild, das sichtbar zum Original passt (Nebeneinander). Der verlustfreie
Fall (2) ist dagegen auf Texel genau zu prüfen (gebackene Textur enthält die Quelltexel unverändert).

## 4. Reihenfolge der Umsetzung

A. Flags am Mesh-Asset (Chunk, Laden/Speichern, Test), Schalter im Mesh Viewer, Hilfetexte.
B. Prädikat "einfaches Material" mit Test.
C. Packer (2)/(3) im Export, Test auf Texel-Genauigkeit, Lauf auf Metal mit dem Foliage-Projekt.
D. Vertex-Farben: `MVCL`-Chunk, Bake, GL- und Metal-Bindung, Gate über die Backend-Maske.
Jeder Schritt wird einzeln committet und gepusht.

## Handoff

- **Topic / step:** 228, Punkt 6 (neu).
- **Done:** Bestandsaufnahme und Entwurf (dieses Dokument). Kein Code.
- **Verified:** Layout, `vColor`, fehlender Import von `COLOR_0`, fehlender Entfalter per grep; Verbraucher von `interleaved` gezählt.
- **Not verified:** Wie ein Material seine Albedo-Textur benennt (Slot-Konvention des Uber-Shaders und der Graph-Knoten), ob Texturen mit Wrap-Modus
  ungleich Repeat vorkommen, wie der Reach-Filter abgeleitete UUIDs behandelt.
- **Decisions taken:** Abschnitt 2 (mit Nutzer abgesprochen: alle drei Formen, abschaltbar pro Mesh, größer ist erlaubt).
- **Decisions suggested, not taken:** Vorgaben der Flags, Grenze 4096, Reihenfolge der Formen.
- **Next steps:** Schritt A bis D wie oben.
