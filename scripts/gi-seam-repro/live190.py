"""Thema 190 step 6: chunk seams of a REAL project's landscape, live editor.

The live editor ticks the world (TerrainTess -> LOD -> TerrainSeams), so the
chunks pick their levels from the EDITOR camera. scene_screenshot with a
position renders from a separate client camera and never moves the editor
camera (McpToolsScreenshot.cpp), which is exactly the "LOD camera != image
camera" case of editor Play: park the editor camera far away and the seams in
the shot are mixed-LOD ones; park it at the shot and they are LOD0|LOD0.

  python live190.py <deploy Editor dir> <project.heproj COPY> <out dir> <name>
                    <rhi 0=GL 1=Vulkan 2=D3D11 3=D3D12> <editorcam near|far>
                    [renderpath=1] [gi=1]

Fresh APPDATA per run (config.json: the project, RHI, RenderPath, GI, the
editor camera), HE_MCP=1, HE_COLLAB_OFFLINE=1, window minimised without
activation. Shots go to <out>/<name>_<shot>.png. Pass a COPY of the project.

The shots are for the NN-WS03 HEProject (2000 m, res 1025: 16x16 chunks of
125 m, seams every 125 m from -1000; flat at y=0 except the mountain block
x -500..125, z 125..750).
"""
import json
import os
import shutil
import subprocess
import sys
import time

SHOTS = {
    # Flat ground, seam crossing (250, -250): close, 60 m, 200 m away.
    'flat_close': ([262.0, 6.0, -212.0], [250.0, 0.0, -250.0]),
    'flat_mid':   ([290.0, 18.0, -190.0], [250.0, 0.0, -250.0]),
    'flat_far':   ([360.0, 45.0, -60.0], [250.0, 0.0, -250.0]),
    # Mountain flank, seam x=-125 around z=500 (relief: steps if unstitched).
    'mtn_close':  ([-60.0, 75.0, 470.0], [-125.0, 45.0, 500.0]),
    'mtn_far':    ([60.0, 120.0, 380.0], [-125.0, 45.0, 520.0]),
}

EDITOR_CAM = {
    # near: on top of the shots -> every chunk in frame LOD0.
    'near': (200.0, 60.0, -100.0),
    # far: ~900 m from the shots -> the chunks in frame sit on the LOD0/LOD1
    # border (LOD0 ends 750 m from a chunk centre at lodDistanceScale 1).
    'far': (-750.0, 150.0, -900.0),
}


def main(argv):
    deploy, project, out, name, rhi, cam = argv[1:7]
    renderpath = int(argv[7]) if len(argv) > 7 else 1
    gi = (argv[8] if len(argv) > 8 else '1') == '1'
    os.makedirs(out, exist_ok=True)
    app = os.path.join(out, 'appdata-' + name)
    shutil.rmtree(app, ignore_errors=True)
    cfg_dir = os.path.join(app, 'HorizonEngine')
    os.makedirs(cfg_dir)
    ex, ey, ez = EDITOR_CAM[cam]
    cfg = {
        'LastProjectPath': project.replace('\\', '/'),
        'RHI': int(rhi),
        'CustomConfig': [
            {'Key': 'RenderPath', 'Value': renderpath},
            {'Key': 'GlobalIlluminationEnabled', 'Value': gi},
            {'Key': 'EditorCamValid', 'Value': True},
            {'Key': 'EditorCamPosX', 'Value': ex},
            {'Key': 'EditorCamPosY', 'Value': ey},
            {'Key': 'EditorCamPosZ', 'Value': ez},
        ],
    }
    with open(os.path.join(cfg_dir, 'config.json'), 'w') as f:
        json.dump(cfg, f, indent=2)
    cache = os.environ.get('LIVE190_CACHE')
    if cache:
        shutil.copytree(cache, os.path.join(cfg_dir, 'EngineContentCache'))

    env = {k: v for k, v in os.environ.items() if not k.startswith('HE_DUMP')}
    env.update(APPDATA=app, HE_MCP='1', HE_COLLAB_OFFLINE='1', HE_NET_LOOPBACK_ONLY='1')
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
        deadline = time.time() + 180
        while not os.path.exists(ep) and time.time() < deadline:
            time.sleep(0.5)
        if not os.path.exists(ep):
            print('no MCP endpoint after 180 s')
            return 1
        bridge = he_mcp.Bridge(ep)

        def call(tool, args):
            r = bridge.request('tools/call', {'name': tool, 'arguments': args})
            return r.get('result', r)

        deadline = time.time() + 180
        while time.time() < deadline:
            r = call('scene_screenshot', {'render': False})
            if 'no_world' not in json.dumps(r):
                break
            time.sleep(1.0)
        time.sleep(float(os.environ.get('LIVE190_SETTLE', '20')))
        for shot, (pos, look) in SHOTS.items():
            fname = '%s_%s' % (name, shot)
            r = call('scene_screenshot', {'position': pos, 'look_at': look, 'output': 'file',
                                          'name': fname})
            path = None
            for c in r.get('content', []):
                if c.get('type') == 'text':
                    try:
                        path = json.loads(c['text']).get('path')
                    except (ValueError, AttributeError):
                        pass
            if path and os.path.exists(path):
                shutil.copy(path, os.path.join(out, fname + '.png'))
                print('%-28s ok' % fname)
            else:
                print('%-28s FAILED %s' % (fname, json.dumps(r)[:300]))
    finally:
        proc.kill()
        proc.wait()
        if os.path.exists(log):
            shutil.copy(log, os.path.join(out, name + '.log'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
