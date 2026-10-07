#include "app/Vfx.h"

namespace oe {
Json VfxPresets(const std::string& tag) {
    // Settings derived from VFXLab, with solid quads so every project can use them.
    static const Json presets = Json::parse(R"J([
      {"name":"fire","intent":"A warm flame rises and fades with turbulent motion.","tags":["fire","continuous"],
       "emitters":[{"name":"flames","emitter":{"rate":45,"lifetime":0.9,"lifetimeVariation":0.3,"speed":1.3,"speedVariation":0.4,"spread":12,"gravity":[0,1.2,0],"shape":"circle","shapeSize":[0.28,0,0],"blend":"add","maxParticles":96,"turbulence":3,"turbulenceScale":1.6,"sizeOverLife":[[0,0.35],[0.25,0.75],[1,0.15]],"colorOverLife":[[0,[1,0.9,0.5]],[0.35,[1,0.45,0.1]],[1,[0.5,0.05,0]]],"opacityOverLife":[[0,0],[0.12,0.85],[0.7,0.5],[1,0]],"pixelArt":false}}],"trails":[]},
      {"name":"smoke","intent":"Soft gray smoke grows and drifts upward.","tags":["smoke","continuous"],
       "emitters":[{"name":"smoke","emitter":{"rate":7,"lifetime":3.2,"speed":0.7,"spread":14,"gravity":[0,0.25,0],"shape":"circle","shapeSize":[0.2,0,0],"maxParticles":48,"turbulence":1.2,"turbulenceScale":0.5,"rotationVariation":180,"spinVariation":40,"sizeVariation":0.3,"sizeOverLife":[[0,0.5],[1,2.2]],"colorOverLife":[[0,[0.35,0.3,0.28]],[1,[0.16,0.16,0.18]]],"opacityOverLife":[[0,0],[0.2,0.42],[1,0]],"pixelArt":false}}],"trails":[]},
      {"name":"sparks","intent":"Hot orange streaks scatter continuously and fade.","tags":["sparks","continuous"],
       "emitters":[{"name":"sparks","emitter":{"rate":14,"lifetime":1.8,"lifetimeVariation":0.5,"speed":2.2,"speedVariation":0.6,"spread":25,"gravity":[0,0.4,0],"drag":0.6,"blend":"add","maxParticles":64,"turbulence":7,"align":"velocity","stretch":0.06,"startSize":0.07,"endSize":0.02,"startColor":"#ffb340","endColor":"#ff3300"}}],"trails":[]},
      {"name":"hit_burst","intent":"A short red impact burst scatters once and finishes.","tags":["hit","burst"],
       "emitters":[{"name":"hit","emitter":{"rate":0,"burst":16,"loop":false,"lifetime":0.5,"speed":3,"speedVariation":0.5,"spread":180,"gravity":[0,-2,0],"blend":"add","maxParticles":32,"startSize":0.15,"startColor":"#ff6633","endColor":"#990000","space":"world"}}],"trails":[]},
      {"name":"portal_ring","intent":"Blue and violet motes rise from a ring and pull inward.","tags":["portal","continuous"],
       "emitters":[{"name":"ring","emitter":{"rate":70,"lifetime":1.6,"speed":0.9,"spread":0,"gravity":[0,0,0],"shape":"circle","shapeSize":[1.1,0,0],"shapeEdge":true,"radialSpeed":-0.45,"blend":"add","maxParticles":160,"sizeOverLife":[[0,0],[0.15,0.2],[1,0.03]],"colorOverLife":[[0,[0.3,0.9,1]],[1,[0.6,0.25,1]]],"opacityOverLife":[[0,1],[0.7,0.8],[1,0]]}}],"trails":[]},
      {"name":"fireworks","intent":"A bright radial firework explodes every two seconds.","tags":["fireworks","burst","continuous"],
       "emitters":[{"name":"burst","emitter":{"rate":0,"burst":70,"duration":2,"lifetime":1.3,"lifetimeVariation":0.4,"speed":5.5,"speedVariation":0.5,"spread":180,"gravity":[0,-3.5,0],"drag":1.6,"blend":"add","maxParticles":160,"align":"velocity","stretch":0.07,"startSize":0.11,"endSize":0.03,"colorOverLife":[[0,[1,1,0.8]],[0.3,[1,0.5,0.9]],[1,[0.3,0.3,1]]],"opacityOverLife":[[0,1],[0.6,0.9],[1,0]]}}],"trails":[]},
      {"name":"trail_swing","intent":"A short bright ribbon follows a moving sword or swing.","tags":["trail","swing"],"emitters":[],
       "trails":[{"name":"swing","trail":{"lifetime":0.25,"minDistance":0.02,"width":0.5,"endWidth":0,"color":"#ccffff","endColor":"#3388ff","blend":"add","maxPoints":64}}]},
      {"name":"trail_projectile","intent":"A glowing ribbon follows a moving projectile.","tags":["trail","projectile"],"emitters":[],
       "trails":[{"name":"projectile","trail":{"lifetime":0.7,"minDistance":0.04,"width":0.34,"endWidth":0.02,"color":[0.6,1,0.75],"endColor":[0.1,0.5,1],"opacity":0.95,"blend":"add","maxPoints":96}}]}
      ,{"name":"sword_slash","intent":"A swept blade ribbon, a crescent mesh and short sparks make a sword aura.","tags":["slash","sword","burst"],
       "emitters":[{"name":"crescent","emitter":{"mesh":"crescent","batching":true,"burst":1,"rate":0,"loop":false,"lifetime":0.35,"speed":0,"gravity":[0,0,0],"startSize":3,"endSize":4,"startColor":[0.3,0.8,1],"endColor":[0.05,0.2,1],"blend":"add","maxParticles":4}},
       {"name":"sparks","emitter":{"burst":18,"rate":0,"loop":false,"lifetime":0.45,"speed":2,"speedVariation":0.5,"spread":180,"gravity":[0,0,0],"align":"velocity","stretch":0.08,"blend":"add","batching":true,"startSize":0.08,"endSize":0.01,"startColor":[0.4,0.9,1],"maxParticles":32}}],
       "trails":[{"name":"blade","trail":{"mode":"ribbon","baseOffset":[0,0,0],"tipOffset":[0,1.5,0],"duration":0.2,"lifetime":0.25,"blend":"add","color":[0.4,0.9,1],"endColor":[0.05,0.2,1],"minDistance":0.01}}]}
    ])J");
    Json out = Json::MakeArray();
    for (const Json& preset : presets.items()) {
        bool match = tag.empty();
        for (const Json& value : preset["tags"].items()) match = match || value.asString() == tag;
        if (match) out.push(preset);
    }
    return out;
}
}  // namespace oe
