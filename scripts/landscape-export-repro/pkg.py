import json, sys, time
sys.path.insert(0, r"C:\hw189\editor")
import he_mcp

ep, out = sys.argv[1], sys.argv[2]
b = he_mcp.Bridge(ep)
call = lambda n, a=None: b.request("tools/call", {"name": n, "arguments": a or {}})
print("package:", json.dumps(call("project_package", {
    "outputDir": out, "targetPlatform": "Host", "incremental": False,
    "compress": False, "encrypt": False}))[:800])
t0 = time.time(); txt = ""
while time.time() - t0 < 900:
    time.sleep(3)
    txt = json.dumps(call("project_build_status"))
    if '"running": false' in txt or '"running":false' in txt:
        print("status:", txt[:3000]); break
else:
    print("timeout; last:", txt[:2000])
