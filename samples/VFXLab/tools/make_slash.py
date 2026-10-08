"""Generate the phase-4 sword aura assets. Requires build/bin/oe.exe."""
import json
import math
import struct
import zlib
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
engine = root.parents[1] / 'build/bin/oe.exe'
result = subprocess.run([str(engine), 'exec', str(root), 'vfx.presets', '{"tag":"slash"}'], capture_output=True, text=True, check=True)
preset = json.loads(result.stdout)['result'][0]

def save(path, value):
    target = root / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')

asset = {'format': 'oe.vfx', 'version': 1, 'name': 'Sword Slash', 'description': 'Two-anchor blade ribbon, dissolving mesh crescent and sparks.',
         'emitters': preset['emitters'], 'trails': preset['trails'], 'budget': {'maxParticles': 64}}
asset['emitters'][0]['emitter']['material'] = 'materials/slash.mat.json'
asset['trails'][0]['trail']['material'] = 'materials/slash.mat.json'
save('effects/sword_slash.vfx.json', asset)
# ParticleAge drives UV scrolling and a moving dissolve threshold; ParticleRandom
# gives every mesh birth a stable phase without using one of the eight uniform slots.
nodes = [
    {'op': 'baseColor'}, {'op': 'uv'}, {'op': 'particleAge'}, {'op': 'particleRandom'},
    {'op': 'constant', 'value': [0.25, 0, 0, 0]}, {'op': 'multiply', 'args': [2, 4]},
    {'op': 'constant', 'value': [0.12, 0, 0, 0]}, {'op': 'multiply', 'args': [3, 6]},
    {'op': 'add', 'args': [1, 5]}, {'op': 'add', 'args': [8, 7]}, {'op': 'fract', 'args': [9]},
    {'op': 'swizzle', 'args': [10], 'value': [0, 0, 0, 0]}, {'op': 'step', 'args': [2, 11]},
    {'op': 'multiply', 'args': [0, 12]}, {'op': 'constant', 'value': [1, 1, 1, 1]},
    {'op': 'subtract', 'args': [14, 2]}, {'op': 'multiply', 'args': [13, 15]},
    {'op': 'texture', 'args': [10]}, {'op': 'multiply', 'args': [16, 17]}
]
save('materials/slash.shader.json', {'format': 'ownengine.shader', 'nodes': nodes, 'color': 18})
save('materials/slash.mat.json', {'format': 'ownengine.material', 'shader': 'materials/slash.shader.json',
                                'unlit': True, 'doubleSided': True, 'alphaMode': 'blend', 'baseTexture': 'assets/slash.png'})
scene = {'format': 'ownengine.scene', 'version': 1, 'name': 'Sword Aura', 'entities': [
    {'id': 1, 'name': 'Camera', 'components': {'Transform': {'position': [0, 0.7, 6]},
        'Camera': {'clearColor': [0.02, 0.025, 0.04]},
        'PostProcess': {'bloom': 0.5, 'bloomThreshold': 0.7}}},
    {'id': 2, 'name': 'Slash', 'components': {'Transform': {},
        'VFXSystem': {'asset': 'effects/sword_slash.vfx.json'}, 'Script': {'path': 'scripts/slash.lua'}}}
]}
save('scenes/slash.scene.json', scene)

# Small seamless streak texture, generated deterministically without external libraries.
def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
size = 64
rows = bytearray()
for y in range(size):
    rows.append(0)
    for x in range(size):
        streak = 0.55 + 0.45 * math.sin(x / size * math.pi * 8)
        edge = math.sin((y + 0.5) / size * math.pi) ** 1.5
        rows.extend((255, 255, 255, round(255 * edge * streak)))
image = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
image += chunk(b'IDAT', zlib.compress(bytes(rows))) + chunk(b'IEND', b'')
(root / 'assets/slash.png').write_bytes(image)
