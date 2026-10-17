"""Thema 190 step 6: export a project COPY as a packed game through a private
editor's MCP bridge (project_package), for the packed-build seam check.

  python export190.py <deploy Editor dir> <project.heproj COPY> <out dir> <scratch dir>

Fresh APPDATA under <scratch>/appdata-export, HE_MCP=1, window minimised
without activation. Then run <out>/HorizonGame.exe with GameBackend set in
its config.json (see the verify-exported-game recipe: windowed, ~7 s of black
frames on D3D before the first picture).
"""
import json
import os
import shutil
import subprocess
import sys
import time


def main(argv):
    deploy, project, out, scratch = argv[1:5]
    app = os.path.join(scratch, 'appdata-export')
    shutil.rmtree(app, ignore_errors=True)
    cfg_dir = os.path.join(app, 'HorizonEngine')
    os.makedirs(cfg_dir)
    with open(os.path.join(cfg_dir, 'config.json'), 'w') as f:
        json.dump({'LastProjectPath': project.replace('\\', '/'), 'RHI': 1}, f)
    # EXPORT190_CACHE=<EngineContentCache dir>: without it the export has no
    # Engine/Textures/Landscape arrays (the packer takes them from the cache).
    cache = os.environ.get('EXPORT190_CACHE')
    if cache:
        shutil.copytree(cache, os.path.join(cfg_dir, 'EngineContentCache'))
    env = {k: v for k, v in os.environ.items() if not k.startswith('HE_DUMP')}
    env.update(APPDATA=app, HE_MCP='1', HE_COLLAB_OFFLINE='1', HE_NET_LOOPBACK_ONLY='1')
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 7  # SW_SHOWMINNOACTIVE
    proc = subprocess.Popen([os.path.join(deploy, 'HorizonEditor.exe')], cwd=deploy,
                            env=env, startupinfo=si)
    try:
        sys.path.insert(0, deploy)
        import he_mcp
        ep = os.path.join(cfg_dir, he_mcp.ENDPOINT_FILENAME)
        deadline = time.time() + 180
        while not os.path.exists(ep) and time.time() < deadline:
            time.sleep(0.5)
        b = he_mcp.Bridge(ep)

        def call(name, args=None):
            return b.request('tools/call', {'name': name, 'arguments': args or {}})

        time.sleep(10)
        print('package:', json.dumps(call('project_package', {
            'outputDir': out, 'targetPlatform': 'Host', 'incremental': False,
            'startupScene': 'Content/StartupScene.hescene',
            'compress': False, 'encrypt': False}))[:600])
        t0 = time.time()
        txt = ''
        while time.time() - t0 < 900:
            time.sleep(3)
            txt = json.dumps(call('project_build_status'))
            # The tool answer nests its JSON as text: match escaped and plain.
            flat = txt.replace('\\"', '"').replace(' ', '')
            if '"running":false' in flat:
                break
        print('status:', txt[:1500])
    finally:
        proc.kill()
        proc.wait()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
