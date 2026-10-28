import json, sys, time
sys.path.insert(0, r"C:\hw189\editor")
import he_mcp

ep, out = sys.argv[1], sys.argv[2]
# Step 3: "incr" re-exports over an existing pak the way the dialog's default does.
incremental = len(sys.argv) > 3 and sys.argv[3] == "incr"
b = he_mcp.Bridge(ep)
call = lambda n, a=None: b.request("tools/call", {"name": n, "arguments": a or {}})
print("package:", json.dumps(call("project_package", {
    "outputDir": out, "targetPlatform": "Host", "incremental": incremental,
    "compress": False, "encrypt": False}))[:800])
t0 = time.time(); txt = ""
while time.time() - t0 < 900:
    time.sleep(3)
    txt = json.dumps(call("project_build_status"))
    if '"running": false' in txt or '"running":false' in txt:
        st = json.loads(call("project_build_status")["result"]["content"][0]["text"])
        log = st.get("log", [])
        print("finished:", st.get("finished"), "log lines:", len(log),
              "packed:", sum(1 for l in log if l["text"].startswith("packed")),
              "reused:", sum(1 for l in log if l["text"].startswith("reused")))
        for l in log:   # the drop warnings of step 2 come with severity 1
            if l.get("severity", 0) > 0 or l["text"].startswith("packed"):
                print("  [sev %d] %s" % (l.get("severity", 0), l["text"]))
        break
else:
    print("timeout; last:", txt[:2000])
