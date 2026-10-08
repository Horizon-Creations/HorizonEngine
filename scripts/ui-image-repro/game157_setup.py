"""Thema 157 Schritt 4: Zeugenprojekt fuer UI-Bilder im EXPORTIERTEN Spiel.

Der Editor-Zeuge (cap157.ps1, HE_DUMP_UITEST=image) zeichnet ins Viewport-RT;
das Spiel zeichnet die UI ueber den Swapchain-Pfad (Vulkan: m_uiPipeline). Dieses
Projekt bringt ein Image-Widget ohne Nutzer-Code auf den Schirm:

  GameInstance.hcode   OnInit -> Create Widget("UI/ImgWitness.hasset") -> Show Widget
  Content/UI/Pic.hasset  256x256 Vier-Quadranten-Bild, von asset_compiler importiert
                         (Name 'Pic' -> sRGB-Flag, wie jede normale Farbtextur)
  Content/UI/ImgWitness.hasset  legt game157_export.py ueber die MCP-Bruecke an

Entity-UI (uiimage in der .hescene) taugt NICHT als Zeuge: sie zeichnet ueber eine
Material-UUID, nicht ueber textureAssetId, also nicht den Pfad, den Thema 157 repariert.

Aufruf: python game157_setup.py [root=C:/hw157/game] [asset_compiler.exe]
"""
import json, os, shutil, subprocess, sys
from PIL import Image

ROOT = sys.argv[1] if len(sys.argv) > 1 else "C:/hw157/game"
AC = sys.argv[2] if len(sys.argv) > 2 else "C:/hw157/src/HE_Tools/asset_compiler.exe"
PROJ = os.path.join(ROOT, "proj", "ImgWit")
SRC = os.path.join(ROOT, "src")

# Quadrantenfarben wie der Editor-Zeuge (ana157): oben links rot, oben rechts gruen,
# unten links blau, unten rechts gelb -- ein Tint-Quad kann keine vier Farben.
TL, TR, BL, BR = (230, 30, 30), (30, 200, 60), (30, 80, 230), (240, 220, 40)

if os.path.isdir(PROJ):
    shutil.rmtree(PROJ)
if os.path.isdir(SRC):
    shutil.rmtree(SRC)
os.makedirs(os.path.join(PROJ, "Content"))
os.makedirs(os.path.join(SRC, "UI"))

img = Image.new("RGB", (256, 256))
px = img.load()
for y in range(256):
    for x in range(256):
        px[x, y] = (TL if x < 128 else TR) if y < 128 else (BL if x < 128 else BR)
img.save(os.path.join(SRC, "UI", "Pic.png"))

r = subprocess.run([AC, SRC, os.path.join(PROJ, "Content"), "--force"],
                   capture_output=True, text=True)
print(r.stdout.strip()[-800:], r.stderr.strip()[-400:], "rc=%d" % r.returncode)
if r.returncode != 0 or not os.path.isfile(os.path.join(PROJ, "Content", "UI", "Pic.hasset")):
    sys.exit("asset_compiler hat UI/Pic.hasset nicht geschrieben")

proj = {
    "activeExportProfile": "Development",
    "exportProfiles": [{
        "appBundle": False, "compileHorizonCode": False, "compress": False,
        "enableModSupport": True, "encrypt": False, "excludePatterns": [],
        "hcStopOnFailure": False, "incremental": False, "name": "Development",
        "outputDir": "", "shaderBackends": 17, "startupScene": "", "targetPlatform": "Host"}],
    "id": "5d3e112f0000400080000000000001a7",
    "name": "ImgWit", "preset": 1, "scriptLanguage": "HorizonCode",
    "startupScene": "Content/StartupScene.hescene", "version": "1.0",
}
with open(os.path.join(PROJ, "ImgWit.heproj"), "w") as f:
    json.dump(proj, f, indent=4)

root = [7700157, 1]
floor, cam, sky = [7700157, 1001], [7700157, 1002], [7700157, 1003]
ents = [
    {"children": [], "name": "Floor", "parent": root, "uuid": floor, "components": {
        "mesh": {"asset": [1, 1], "castsShadow": True, "receivesShadow": True, "visible": True, "lodBias": 0},
        "transform": {"position": [0, -0.1, 0], "rotation": [0, 0, 0], "scale": [40, 0.2, 40]}}},
    {"children": [], "name": "MainCamera", "parent": root, "uuid": cam, "components": {
        "camera": {"fovDegrees": 60.0, "nearPlane": 0.1, "farPlane": 200.0, "isMain": True, "orthographic": False},
        "transform": {"position": [0, 3.2, 10], "rotation": [-12, 0, 0], "scale": [1, 1, 1]}}},
    {"children": [], "name": "Sky", "parent": root, "uuid": sky, "components": {
        "environment": {"timeOfDay": 0.38, "sunIntensity": 2.2, "dayNightCycle": False,
                        "autoAdvance": False, "cloudCoverage": 0.3}}},
    {"children": [floor, cam, sky], "name": "World", "parent": None, "uuid": root},
]
with open(os.path.join(PROJ, "Content", "StartupScene.hescene"), "w") as f:
    json.dump({"entities": ents, "version": "1.1"}, f, indent=2)

# Pins: Event exec-out 0; Create Widget exec-in 0 / exec-out 1 / Widget-out 2;
# Show Widget exec-in 0 / Widget-in 2 (HorizonCode.cpp, wie ProjectManager::writeAppGameInstance).
graph = {"nextId": 4, "nodes": [
    {"id": 1, "type": "Event", "s": "OnInit", "pos": [0, 0]},
    {"id": 2, "type": "Create Widget", "s": "UI/ImgWitness.hasset", "pos": [260, 0]},
    {"id": 3, "type": "Show Widget", "pos": [520, 0]}],
    "links": [[1, 0, 2, 0], [2, 1, 3, 0], [2, 2, 3, 2]], "variables": []}
with open(os.path.join(PROJ, "GameInstance.hcode"), "w") as f:
    json.dump(graph, f, indent=1)
print("Projekt:", PROJ)
