"""Writes the VFXLab scene (python tools/make_scene.py, from the sample folder).

Four effects built only from ParticleEmitter and Trail settings, left to right:
  Campfire   flames (color/size curves, turbulence), smoke (random sheet frame, spin, opacity curve),
             embers (velocity-aligned streaks)
  Portal     a ring of motes born on a circle's rim, pulled inward as they rise
  Fireworks  a burst every two seconds (loop + duration) with a delayed smoke puff; Space fires one now
  Orb        an entity on a figure-eight that leaves a Trail ribbon and world-space sparkles
"""
import json
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
GLOW = "assets/textures/glow.png"
SMOKE = "assets/textures/smoke.png"

entities = []


def add(name, components, parent=None):
    entity = {"id": len(entities) + 1, "name": name, "components": components}
    if parent is not None:
        entity["parent"] = parent
    entities.append(entity)
    return entity["id"]


def emitter(name, position, parent=None, **settings):
    values = {"texture": GLOW, "pixelArt": False, "space": "world"}
    values.update(settings)
    return add(name, {"Transform": {"position": position}, "ParticleEmitter": values}, parent)


# ----- Stage ----------------------------------------------------------------------------------
add("Moon", {"Transform": {"rotation": [-50, 30, 0]},
             "DirectionalLight": {"color": [0.55, 0.65, 0.9], "intensity": 0.5, "ambient": [0.08, 0.09, 0.14]}})
add("Camera", {"Transform": {"position": [0, 3.2, 10.5], "rotation": [-11, 0, 0]},
               "Camera": {"fov": 45, "clearColor": [0.02, 0.025, 0.05]},
               "PostProcess": {"toneMapping": "reinhard", "exposure": 1.3, "bloom": 0.9, "bloomThreshold": 0.7, "bloomRadius": 10},
               "Script": {"path": "scripts/lab.lua"}})
add("Ground", {"Transform": {"scale": [30, 1, 30]}, "MeshRenderer": {"mesh": "plane", "color": [0.1, 0.11, 0.14]}})

# ----- Campfire -------------------------------------------------------------------------------
fire = add("Campfire", {"Transform": {"position": [-4.5, 0, 0]}})
for i, (x, z, turn) in enumerate([(0.35, 0, 0), (-0.2, 0.3, 60), (-0.2, -0.3, 120)]):
    add("Log%d" % (i + 1), {"Transform": {"position": [x * 0.5, 0.1, z * 0.5], "rotation": [0, turn, 0], "scale": [1.0, 0.18, 0.18]},
                            "MeshRenderer": {"mesh": "cube", "color": [0.2, 0.12, 0.07]}}, fire)
add("FireLight", {"Transform": {"position": [0, 0.8, 0]}, "PointLight": {"color": [1.0, 0.55, 0.2], "intensity": 3, "range": 6}}, fire)
emitter("Flames", [0, 0.2, 0], fire, rate=45, lifetime=0.9, lifetimeVariation=0.3, speed=1.3, speedVariation=0.4, spread=12,
        gravity=[0, 1.2, 0], shape="circle", shapeSize=[0.28, 0, 0], blend="add", maxParticles=96, turbulence=3, turbulenceScale=1.6,
        sizeOverLife=[[0, 0.35], [0.25, 0.75], [1, 0.15]],
        colorOverLife=[[0, [1.0, 0.9, 0.5]], [0.35, [1.0, 0.45, 0.1]], [1, [0.5, 0.05, 0.0]]],
        opacityOverLife=[[0, 0], [0.12, 0.85], [0.7, 0.5], [1, 0]])
emitter("Smoke", [0, 1.1, 0], fire, texture=SMOKE, columns=2, rows=2, frames=4, randomFrame=True, rate=7, lifetime=3.2, speed=0.7,
        spread=14, gravity=[0, 0.25, 0], shape="circle", shapeSize=[0.2, 0, 0], maxParticles=48, turbulence=1.2, turbulenceScale=0.5,
        rotationVariation=180, spinVariation=40, sizeVariation=0.3, sizeOverLife=[[0, 0.5], [1, 2.2]],
        colorOverLife=[[0, [0.35, 0.3, 0.28]], [1, [0.16, 0.16, 0.18]]], opacityOverLife=[[0, 0], [0.2, 0.42], [1, 0]])
emitter("Embers", [0, 0.4, 0], fire, rate=14, lifetime=1.8, lifetimeVariation=0.5, speed=2.2, speedVariation=0.6, spread=25,
        gravity=[0, 0.4, 0], drag=0.6, blend="add", maxParticles=64, turbulence=7, turbulenceScale=1.1, align="velocity", stretch=0.06,
        startSize=0.07, endSize=0.02, startColor=[1.0, 0.7, 0.25], endColor=[1.0, 0.2, 0.0], startOpacity=1, endOpacity=0)

# ----- Portal ---------------------------------------------------------------------------------
portal = add("Portal", {"Transform": {"position": [-1.3, 0.05, 0]}})
add("PortalLight", {"Transform": {"position": [0, 0.6, 0]}, "PointLight": {"color": [0.3, 0.7, 1.0], "intensity": 2, "range": 4}}, portal)
emitter("Rim", [0, 0, 0], portal, rate=70, lifetime=1.6, speed=0.9, spread=0, gravity=[0, 0, 0], shape="circle", shapeSize=[1.1, 0, 0],
        shapeEdge=True, radialSpeed=-0.45, blend="add", maxParticles=160, sizeOverLife=[[0, 0.0], [0.15, 0.2], [1, 0.03]],
        colorOverLife=[[0, [0.3, 0.9, 1.0]], [1, [0.6, 0.25, 1.0]]], opacityOverLife=[[0, 1], [0.7, 0.8], [1, 0]])
emitter("Core", [0, 0.15, 0], portal, rate=10, lifetime=1.2, speed=0.2, spread=180, gravity=[0, 0.6, 0], shape="circle",
        shapeSize=[0.6, 0, 0], blend="add", maxParticles=32, spinVariation=90, sizeOverLife=[[0, 0.2], [0.5, 0.9], [1, 0.4]],
        colorOverLife=[[0, [0.2, 0.5, 1.0]], [1, [0.5, 0.2, 0.9]]], opacityOverLife=[[0, 0], [0.4, 0.3], [1, 0]])

# ----- Fireworks ------------------------------------------------------------------------------
works = add("Fireworks", {"Transform": {"position": [1.7, 2.6, 0]}})
emitter("Burst", [0, 0, 0], works, rate=0, burst=70, loop=True, duration=2, lifetime=1.3, lifetimeVariation=0.4, speed=5.5,
        speedVariation=0.5, spread=180, gravity=[0, -3.5, 0], drag=1.6, blend="add", maxParticles=160, align="velocity", stretch=0.07,
        startSize=0.11, endSize=0.03, colorOverLife=[[0, [1.0, 1.0, 0.8]], [0.3, [1.0, 0.5, 0.9]], [1, [0.3, 0.3, 1.0]]],
        opacityOverLife=[[0, 1], [0.6, 0.9], [1, 0]])
emitter("Flash", [0, 0, 0], works, rate=0, burst=1, loop=True, duration=2, lifetime=0.22, speed=0, gravity=[0, 0, 0], blend="add",
        maxParticles=4, sizeOverLife=[[0, 0.6], [0.3, 3.2], [1, 2.0]], startColor=[1.0, 0.9, 0.8], endColor=[1.0, 0.6, 0.9],
        opacityOverLife=[[0, 1], [1, 0]])
emitter("Puff", [0, 0, 0], works, texture=SMOKE, columns=2, rows=2, frames=4, randomFrame=True, rate=0, burst=7, loop=True, duration=2,
        delay=0.12, lifetime=1.7, speed=0.6, spread=180, gravity=[0, 0.15, 0], shape="sphere", shapeSize=[0.35, 0, 0], maxParticles=24,
        rotationVariation=180, spinVariation=25, sizeOverLife=[[0, 0.6], [1, 1.7]], startColor=[0.4, 0.38, 0.45],
        endColor=[0.2, 0.2, 0.25], opacityOverLife=[[0, 0], [0.15, 0.3], [1, 0]])

# ----- Orb ------------------------------------------------------------------------------------
orb = add("Orb", {"Transform": {"position": [4.6, 1.4, 0]},
                  "Script": {"path": "scripts/orbit.lua", "params": {"width": 1.5, "height": 0.9, "speed": 2.4}},
                  "Trail": {"lifetime": 0.7, "minDistance": 0.04, "width": 0.34, "endWidth": 0.02, "color": [0.6, 1.0, 0.75],
                            "endColor": [0.1, 0.5, 1.0], "opacity": 0.95, "endOpacity": 0, "texture": "assets/textures/streak.png",
                            "blend": "add", "maxPoints": 96},
                  "PointLight": {"color": [0.4, 1.0, 0.7], "intensity": 1.5, "range": 3}})
emitter("OrbGlow", [0, 0, 0], orb, space="local", rate=0, burst=1, loop=True, duration=0.5, lifetime=0.5, speed=0, gravity=[0, 0, 0],
        blend="add", maxParticles=4, startSize=0.55, endSize=0.55, startColor=[0.7, 1.0, 0.8], endColor=[0.7, 1.0, 0.8],
        opacityOverLife=[[0, 0.9], [1, 0.9]])
emitter("Sparkles", [0, 0, 0], orb, rate=30, lifetime=0.8, lifetimeVariation=0.4, speed=0.5, speedVariation=0.6, spread=180,
        gravity=[0, -0.8, 0], blend="add", maxParticles=48, startSize=0.1, endSize=0, startColor=[0.8, 1.0, 0.9],
        endColor=[0.2, 0.6, 1.0], startOpacity=1, endOpacity=0)

# ----- Labels ---------------------------------------------------------------------------------
add("Help", {"UIText": {"text": "VFXLab   campfire / portal / fireworks / orb trail      Space: fire a burst",
                        "anchor": "bottom-left", "x": 16, "y": -16, "size": 18, "color": [0.8, 0.85, 0.95]}})

full = os.path.join(ROOT, "scenes/main.scene.json")
os.makedirs(os.path.dirname(full), exist_ok=True)
with open(full, "w", encoding="utf-8", newline="\n") as f:
    json.dump({"format": "ownengine.scene", "version": 1, "name": "VFXLab", "entities": entities}, f, indent=2)
    f.write("\n")
print("wrote %d entities" % len(entities))
