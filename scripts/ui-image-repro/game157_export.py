"""Thema 157 Schritt 4: Widget anlegen und das Zeugenprojekt exportieren.

Laeuft gegen einen privaten Editor (APPDATA umgebogen, HE_MCP=1, LastProjectPath =
ImgWit.heproj aus game157_setup.py). Legt UI/ImgWitness.hasset an:
  Bild A  400x400, mitte-mitte verankert, Texture UI/Pic.hasset, Tint weiss
  Bild B  200x200 bei (200,200) (Pivot ist die Mitte, nicht oben links), gleiche Textur, Corner Radius 60
und exportiert dann per project_package (Host, unkomprimiert).

Aufruf: python game157_export.py <mcp-endpoint.json> <outDir> [deploy/Editor]
"""
import json, os, sys, time

ep, out = sys.argv[1], sys.argv[2]
sys.path.insert(0, sys.argv[3] if len(sys.argv) > 3 else r"C:\hw157\deploy\Editor")
import he_mcp

b = he_mcp.Bridge(ep)
W = "UI/ImgWitness.hasset"


def call(name, args=None, show=True):
    r = b.request("tools/call", {"name": name, "arguments": args or {}})
    if show:
        print(name, "->", json.dumps(r)[:600])
    return r


call("asset_create", {"path": W, "type": "Widget"})
call("widget_add", {"path": W, "type": "Image", "name": "ImgCentre", "size": [400, 400],
                    "properties": {"Texture": "UI/Pic.hasset", "Tint": [1, 1, 1, 1]}})
call("widget_add", {"path": W, "type": "Image", "name": "ImgRounded", "position": [200, 200],
                    "size": [200, 200],
                    "properties": {"Texture": "UI/Pic.hasset", "Tint": [1, 1, 1, 1],
                                   "Corner Radius": 60}})
call("widget_save", {"path": W})
call("widget_tree", {"path": W})

call("project_package", {"outputDir": out, "targetPlatform": "Host", "incremental": False,
                         "startupScene": "Content/StartupScene.hescene",
                         "compress": False, "encrypt": False})
t0 = time.time()
txt = ""
while time.time() - t0 < 600:
    time.sleep(3)
    txt = json.dumps(call("project_build_status", show=False))
    if '"running": false' in txt or '"running":false' in txt:
        break
print("status:", txt[:3000])
