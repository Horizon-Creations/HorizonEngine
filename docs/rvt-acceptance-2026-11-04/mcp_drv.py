import json, sys, os
sys.path.insert(0, os.environ.get('HE_EDITOR_DIR', '.'))   # the deployed Editor dir, where he_mcp.py is
import he_mcp

EP = os.environ['LIVE_EP']   # the editor's mcp-endpoint.json
b = he_mcp.Bridge(EP)

def call(name, args=None):
    r = b.request("tools/call", {"name": name, "arguments": args or {}})
    return r

if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "tools":
        r = b.request("tools/list", {})["result"]
        for t in r["tools"]:
            if any(k in t["name"] for k in sys.argv[2:]):
                print(t["name"], "-", t["description"][:100].replace("\n", " "))
                print("   ", json.dumps(t["inputSchema"].get("properties", {}))[:400])
    elif cmd == "call":
        r = call(sys.argv[2], json.loads(sys.argv[3]) if len(sys.argv) > 3 else {})
        out = json.dumps(r)
        print(out[:1500])
