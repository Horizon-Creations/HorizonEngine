"""Thema 204: render a REAL project's terrain with GI, through the live editor.

The headless dump (HE_DUMP_PATH) renders before the first tickWorld, so a
project loaded from LastProjectPath has no terrain chunks in it. The live
editor builds them; this drives it over its MCP bridge (scene_screenshot)
instead of clicking.

  python live204.py <deploy> <project.heproj COPY> <out dir> <name> <indirect>
                    [rhi=1] [renderpath=1]

One fresh APPDATA under <out dir>/appdata-<name> (config.json: the project,
the RHI, RenderPath, GI on, GIIndirectIntensity=<indirect>), HE_MCP=1,
HE_COLLAB_OFFLINE=1, window started minimised WITHOUT activation (the
editor is on the human's desktop). Shots of the mountain block of the
NN-WS03 HEProject (x -500..125, z 125..750, peak ~68 m) go to
<out dir>/<name>_<shot>.png. Pass a COPY of the project: the editor may
write into it. Never point this at the human's own deploy or APPDATA.
"""
import json
import os
import shutil
import subprocess
import sys
import time

SHOTS = {
    # name: (position, look_at)
    'south':  ([-190.0, 60.0, 150.0], [-190.0, 30.0, 520.0]),
    'oblique': ([-430.0, 150.0, 300.0], [-200.0, 30.0, 560.0]),
    'close':  ([-90.0, 80.0, 430.0], [-200.0, 40.0, 570.0]),
}


def main(argv):
    deploy, project, out, name, indirect = argv[1:6]
    rhi = int(argv[6]) if len(argv) > 6 else 1
    renderpath = int(argv[7]) if len(argv) > 7 else 1
    os.makedirs(out, exist_ok=True)
    app = os.path.join(out, 'appdata-' + name)
    shutil.rmtree(app, ignore_errors=True)
    cfg_dir = os.path.join(app, 'HorizonEngine')
    os.makedirs(cfg_dir)
    cfg = {
        'LastProjectPath': project.replace('\\', '/'),
        'RHI': rhi,
        'CustomConfig': [
            {'Key': 'RenderPath', 'Value': renderpath},
            # <indirect> 'nogi' = GI off altogether (CSM + sky ambient), the control.
            {'Key': 'GlobalIlluminationEnabled', 'Value': indirect != 'nogi'},
            {'Key': 'GIIndirectIntensity', 'Value': 1.0 if indirect == 'nogi' else float(indirect)},
        ],
    }
    with open(os.path.join(cfg_dir, 'config.json'), 'w') as f:
        json.dump(cfg, f, indent=2)
    # LIVE204_CACHE=<an EngineContentCache dir>: copied in, so the auto
    # landscape material gets the real texture sets instead of placeholders.
    cache = os.environ.get('LIVE204_CACHE')
    if cache:
        shutil.copytree(cache, os.path.join(cfg_dir, 'EngineContentCache'))

    env = {k: v for k, v in os.environ.items() if not k.startswith('HE_DUMP')}
    env.update(APPDATA=app, HE_MCP='1', HE_COLLAB_OFFLINE='1')
    # LIVE204_VIEWMODE=basecolor|unlit|normals|…: the viewport honours
    # HE_DUMP_VIEWMODE every frame, live too (ViewportPanel.cpp) — albedo vs light.
    if os.environ.get('LIVE204_VIEWMODE'):
        env['HE_DUMP_VIEWMODE'] = os.environ['LIVE204_VIEWMODE']
    log = os.path.join(deploy, 'HorizonEngine.log')
    if os.path.exists(log):
        os.remove(log)
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 7  # SW_SHOWMINNOACTIVE
    proc = subprocess.Popen([os.path.join(deploy, 'HorizonEditor.exe')], cwd=deploy,
                            env=env, startupinfo=si)
    try:
        sys.path.insert(0, deploy)
        import he_mcp
        ep = os.path.join(cfg_dir, he_mcp.ENDPOINT_FILENAME)
        deadline = time.time() + 120
        while not os.path.exists(ep) and time.time() < deadline:
            time.sleep(0.5)
        if not os.path.exists(ep):
            print('no MCP endpoint after 120 s')
            return 1
        bridge = he_mcp.Bridge(ep)

        def call(tool, args):
            r = bridge.request('tools/call', {'name': tool, 'arguments': args})
            return r.get('result', r)

        # Wait for the scene, then give the probes and the shadow history
        # time to converge (DDGI re-converges over frames after the fit).
        deadline = time.time() + 120
        while time.time() < deadline:
            r = call('scene_screenshot', {'render': False})
            if 'no_world' not in json.dumps(r):
                break
            time.sleep(1.0)
        time.sleep(float(os.environ.get('LIVE204_SETTLE', '15')))
        for shot, (pos, look) in SHOTS.items():
            fname = '%s_%s' % (name, shot)
            r = call('scene_screenshot', {'position': pos, 'look_at': look, 'output': 'file',
                                          'name': fname})
            text = json.dumps(r)
            path = None
            for c in r.get('content', []):
                if c.get('type') == 'text':
                    try:
                        path = json.loads(c['text']).get('path')
                    except (ValueError, AttributeError):
                        pass
            if path and os.path.exists(path):
                shutil.copy(path, os.path.join(out, fname + '.png'))
                print('%-24s ok' % fname)
            else:
                print('%-24s FAILED %s' % (fname, text[:300]))
    finally:
        proc.kill()
        proc.wait()
        if os.path.exists(log):
            shutil.copy(log, os.path.join(out, name + '.log'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
