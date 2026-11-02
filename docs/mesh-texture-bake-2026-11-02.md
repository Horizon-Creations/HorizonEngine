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
4. **"Einfaches Material":** Uber-Shader (`customShaderFragGlsl` leer) mit genau einer Albedo-Textur, oder ein Graph, dessen BaseColor nur aus einem
   Texture-Sample (mal Tint) besteht; Opaque oder Masked mit gleichem Schwellwert, gleiche `metallic`/`roughness`/`doubleSided`, keine WPO, keine
   Displacement, kein Switch. Das Prädikat steht neben dem MTRL-Skim im `HpakWriter` und hat einen eigenen Test.
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
