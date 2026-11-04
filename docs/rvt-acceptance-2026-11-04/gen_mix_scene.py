import json, base64, sys
import numpy as np
# Writes a Mix-material test scene from a Big.hescene (a 1024 m HE_DUMP_AUTOLANDSAVE landscape, run in the project dir).
# usage: gen_mix_scene.py <out.hescene> <view: oblique|ground|down> [mix=1]
out, view = sys.argv[1], sys.argv[2]
mix = (sys.argv[3] if len(sys.argv) > 3 else '1') == '1'
big = json.load(open('Content/Scenes/Big.hescene', encoding='utf-8-sig'))
res = 1025
xs = np.arange(res, dtype=np.float32) - 512.0
X, Z = np.meshgrid(xs, xs)            # [zi][xi]
H = (28*np.sin(X/83.0)*np.cos(Z/71.0) + 14*np.sin(X/37.0+1.3)*np.sin(Z/29.0) + 6*np.sin(X/11.0)*np.cos(Z/13.0) + 34).astype(np.float32)
WR = 256
wx = (np.arange(WR, dtype=np.float32) + 0.5) / WR * 1024 - 512
WX, WZ = np.meshgrid(wx, wx)
# four smooth group fields; the shares are their positive parts squared, so two groups dominate most texels
f = [np.sin(WX/140+0.2)*np.cos(WZ/170+1.0), np.sin(WX/110+2.0)*np.sin(WZ/130), np.cos(WX/190+0.5)*np.sin(WZ/100+2.2), np.sin((WX+WZ)/150+3.0)]
sh = np.stack([np.maximum(0.0, v+0.15)**2 + 0.02 for v in f], axis=-1)
sh = sh / sh.sum(-1, keepdims=True) * 255.0
q = np.floor(sh).astype(np.int32)
rem = 255 - q.sum(-1)
order = np.argsort(-(sh - q), axis=-1)
for k in range(4):
    q[np.arange(WR)[:,None], np.arange(WR)[None,:], order[...,k]] += (rem > k)
w3 = q.astype(np.uint8).reshape(-1)
w0 = np.zeros((WR, WR, 4), np.uint8); w0[...,0] = 255
for e in big['entities']:
    c = e.get('components', {})
    if isinstance(c, dict) and 'terrain' in c:
        t = c['terrain']
        t['sculptHeightsB64'] = base64.b64encode(H.tobytes()).decode()
        if mix:
            c['material']['asset'] = [0x4A4, 1]
            t['weightRes'] = WR
            t['layerWeightsB64'] = base64.b64encode(w0.tobytes()).decode()
            t['layerWeights2B64'] = base64.b64encode(np.zeros(WR*WR*4, np.uint8).tobytes()).decode()
            t['layerWeights3B64'] = base64.b64encode(w3.tobytes()).decode()
            t['groupMix'] = True; t['baseGroup'] = 0
    if isinstance(c, dict) and 'camera' in c:
        if view == 'oblique':
            c['transform']['position'] = [0.0, 470.0, 470.0]; c['transform']['rotation'] = [-32.0, 0.0, 0.0]
        elif view == 'ground':
            c['transform']['position'] = [0.0, 395.0, 200.0]; c['transform']['rotation'] = [-12.0, 0.0, 0.0]
        else:
            c['transform']['position'] = [0.0, 800.0, 0.0]; c['transform']['rotation'] = [-89.0, 0.0, 0.0]
# a Sky entity (sun, sky, ambient): without one the scene has no light and the landscape draws black
world = [e for e in big['entities'] if e['name'] == 'World'][0]
sky = {'name': 'Sky', 'uuid': [111111111, 222222222], 'parent': world['uuid'], 'children': [],
       'components': {'transform': {'position': [0.0, 0.0, 0.0], 'rotation': [0.0, 0.0, 0.0], 'scale': [1.0, 1.0, 1.0]}, 'environment': {}}}
big['entities'].append(sky)
world['children'].append(sky['uuid'])
json.dump(big, open(out, 'w', encoding='utf-8'), separators=(',', ':'))
print('wrote', out, 'mix' if mix else 'plain', view)


