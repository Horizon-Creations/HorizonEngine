# Thema 112 step 2: export the Depthy witness project through the editor's own
# MCP bridge (project_package), then wait for the verdict.
import json, sys, time
sys.path.insert(0, r"C:\hw112\deploy\Editor")
import he_mcp

ep = sys.argv[1]
out = sys.argv[2]
b = he_mcp.Bridge(ep)

def call(name, args=None):
    r = b.request("tools/call", {"name": name, "arguments": args or {}})
    return r

print("package:", json.dumps(call("project_package", {
    "outputDir": out, "targetPlatform": "Host", "incremental": False,
    "startupScene": "Content/StartupScene.hescene",
    "compress": False, "encrypt": False}))[:800])
t0 = time.time()
while time.time() - t0 < 600:
    time.sleep(3)
    st = call("project_build_status")
    txt = json.dumps(st)
    if '"running": false' in txt or '"running":false' in txt:
        print("status:", txt[:4000])
        break
else:
    print("timeout; last:", txt[:2000])
