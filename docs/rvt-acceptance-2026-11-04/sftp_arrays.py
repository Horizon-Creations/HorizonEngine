#!/usr/bin/env python3
"""Compare, and refresh, the landscape texture arrays of the EngineContent cache against the SFTP server.

Run this BEFORE judging any landscape picture on a machine whose editor has not synced lately: the arrays are what the
material draws with, and an old file (the pre-variant 112 / 358 MB ones) draws another picture with the same shader.

    python3 -I sftp_arrays.py list                      # server manifest vs the cache: the size of every array
    python3 -I sftp_arrays.py fetch [name ...]          # download what differs (default: every *Array*), 8 connections

Credentials: cmake/EngineContentCredentials.cmake (gitignored, the same file the build reads); never printed.
Cache: macOS ~/Library/Application Support/HorizonEngine/EngineContentCache, Windows %APPDATA%/HorizonEngine/..., override
with HE_ENGINE_CONTENT_CACHE. Needs `pip install paramiko`.

Notes (measured 2026-11-05): one SFTP connection gives ~0.66 MB/s, eight parallel ranged connections ~3.5 MB/s. A
whole-file prefetch (paramiko's default `get`) on a 1 GB file makes the server drop the connection. A file is written as
<name>.part and renamed once its size matches the server, so an aborted run leaves the old file standing. The editor's
freshness check (ContentSync/CacheFreshness) keeps a file whose size AND FNV-1a equal the manifest's, so a fetched array
survives the next editor start. Checked once by hand (2026-11-05, a C loop over the six files): all six FNV-1a values
equal the server manifest's `hash`, i.e. the parallel download is byte-exact."""
import os, re, sys, threading, time
import paramiko

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
CRED = os.path.join(REPO, "cmake", "EngineContentCredentials.cmake")
REL = "Textures/Landscape"


def cache_dir():
    if os.environ.get("HE_ENGINE_CONTENT_CACHE"): return os.environ["HE_ENGINE_CONTENT_CACHE"]
    if sys.platform == "darwin": return os.path.expanduser("~/Library/Application Support/HorizonEngine/EngineContentCache")
    if sys.platform.startswith("win"): return os.path.join(os.environ["APPDATA"], "HorizonEngine", "EngineContentCache")
    return os.path.expanduser("~/.local/share/HorizonEngine/EngineContentCache")


def credentials():
    vals = {}
    for line in open(CRED):
        m = re.match(r'\s*set\((\w+)\s+"(.*)"\)', line)
        if m: vals[m.group(1)] = m.group(2)
    return vals


def connect(v):
    t = paramiko.Transport((v["HE_ENGINE_CONTENT_HOST"], int(v.get("HE_ENGINE_CONTENT_PORT") or 22)))
    t.connect(username=v["HE_ENGINE_CONTENT_USER"], password=v["HE_ENGINE_CONTENT_PASSWORD"])
    return t, paramiko.SFTPClient.from_transport(t)


def fetch(v, name, size, out, counter, parts=8):
    with open(out + ".part", "wb") as o: o.truncate(size)
    step = (size + parts - 1) // parts

    def worker(lo, hi):
        t, sf = connect(v)
        try:
            f = sf.open(f"{v.get('HE_ENGINE_CONTENT_BASEPATH', '').rstrip('/')}/{REL}/{name}", "rb")
            f.set_pipelined(True)
            f.seek(lo)
            pos = lo
            with open(out + ".part", "r+b") as o:
                while pos < hi:
                    d = f.read(min(1 << 15, hi - pos))
                    if not d: break
                    o.seek(pos); o.write(d); pos += len(d); counter[0] += len(d)
        finally:
            t.close()

    ths = [threading.Thread(target=worker, args=(i * step, min(size, (i + 1) * step))) for i in range(parts)]
    t0 = time.time()
    for th in ths: th.start()
    while any(th.is_alive() for th in ths):
        time.sleep(10)
        print(f"  {name}: {counter[0] / 1e6:.0f} MB, {counter[0] / 1024 / (time.time() - t0):.0f} KB/s", flush=True)
    for th in ths: th.join()
    if os.path.getsize(out + ".part") == size and counter[0] == size:
        os.replace(out + ".part", out)
        return True
    os.remove(out + ".part")
    return False


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "list"
    v = credentials()
    t, sf = connect(v)
    base = v.get("HE_ENGINE_CONTENT_BASEPATH", "").rstrip("/")
    import json, tempfile
    man = os.path.join(tempfile.mkdtemp(), "manifest.json")
    sf.get(f"{base}/manifest.json", man)
    entries = {e["path"].split("/")[-1]: e for e in json.load(open(man))["entries"] if e["path"].startswith(REL + "/") and "Array" in e["path"]}
    ldir = os.path.join(cache_dir(), REL)
    stale = []
    for name in sorted(entries):
        e = entries[name]
        p = os.path.join(ldir, name)
        have = os.path.getsize(p) if os.path.exists(p) else None
        state = "MISSING" if have is None else ("size differs" if have != e["size"] else "size ok")
        print(f"{name:44s} server {e['size']:>11d} B  cache {('-' if have is None else have):>11}  {state}")
        if state != "size ok": stale.append(name)
    if mode == "fetch":
        want = sys.argv[2:] or sorted(entries)
        for name in want:
            if name not in entries: print("not on the server:", name); continue
            if name not in stale: print("up to date (size):", name); continue
            print("downloading", name, entries[name]["size"], "B")
            ok = fetch(v, name, entries[name]["size"], os.path.join(ldir, name), [0])
            print("placed" if ok else "INCOMPLETE, old file kept", name)
    sf.close(); t.close()


if __name__ == "__main__":
    main()
