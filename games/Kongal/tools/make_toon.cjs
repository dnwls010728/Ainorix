// Builds games/Kongal/shaders/toon.shader.json: the painted lighting of the three.js prototype
// (kongal/viewer/src/gouacheMaterial.js + lighting.glsl.js paintLight + noise.glsl.js brushDabs) as a lit surface
// graph with a light stage, so the engine runs it once per light (sun, point, spot) with shadows. The project
// lights in linear space (project.json "rendering": {"colorSpace": "linear"}) like the viewer, so its constants
// are the viewer's own colors decoded (three.js THREE.Color hex values are sRGB) instead of values fitted by eye.
// Like the viewer the brush noise is laid over the rest pose (objectPosition), so it stays on the surface while
// the rig animates, and the rim reads the engine's viewDirection.
//   wobble = (fbm(P * 9) - 0.5) * 0.18                         P = objectPosition, N = normal, V = viewDirection
//   dab    = brushDabs(P, N) * 0.3                             two layers of value noise along the form
//   lit    = smoothstep(-0.32, 0.32, N.L + wobble) * lightAttenuation
//   T      = 1 + termK * lit * (1 - lit)                       hot terminator, mix(1, termColor * 1.15, term * 0.15)
//   D      = 1 + mix(dabShadeK, dabLightK, lit) * w(lit)       dabs, w = |dab| * (1.2 [dab >= 0] + 0.8 [dab < 0] (1 - lit))
//   G      = 1 + (fbm(P * 140) - 0.5) * 0.04                   pigment grain
//   rim    = rimColor * 0.35 * (1 - clamp(N.V))^2.5 * smoothstep(-0.25, 0.55, N.rimDir)
//   color  = (albedo * ambient * D0 + rim) * G                 D0 = D at lit = 0 (once)
//   light  = albedo * ((ambient + lightColor * lit) * T * D - ambient * D0) * G     (per light)
// With the one sun, color + light is the viewer's paintLight exactly (the hemisphere ambient is the engine
// `ambient` input: the Sun's ambient/groundAmbient = sky #f6ead4 / ground #d0a070 x 0.82; lightColor = #ffe6b8 x
// 0.45). The viewer's brushDabs falls back to the x axis where the normal points straight up (t = 0); the graph
// then gives no dab (normalize of zero), a few pixels on top of the head.
// Usage: node tools/make_toon.cjs shaders/toon.shader.json
const fs = require('fs');
const nodes = [], ids = {};
const n = (name, op, args, extra = {}) => {
  if (name in ids) throw new Error('duplicate ' + name);
  ids[name] = nodes.length;
  const node = { op, ...extra };
  if (args) node.args = args.map((a) => { if (!(a in ids)) throw new Error('unknown ' + a); return ids[a]; });
  nodes.push(node);
};
const k = (name, value) => n(name, 'constant', null, { value });
const u = (name) => n(name, 'uniform', null, { name });
const swz = (name, arg, channels) => n(name, 'swizzle', [arg], { value: channels });
// cross(a, b) = a.yzx * b.zxy - a.zxy * b.yzx
const cross = (name, a, b) => {
  swz(name + 'Ayzx', a, [1, 2, 0, 3]); swz(name + 'Bzxy', b, [2, 0, 1, 3]);
  swz(name + 'Azxy', a, [2, 0, 1, 3]); swz(name + 'Byzx', b, [1, 2, 0, 3]);
  n(name + 'L', 'multiply', [name + 'Ayzx', name + 'Bzxy']); n(name + 'R', 'multiply', [name + 'Azxy', name + 'Byzx']);
  n(name, 'subtract', [name + 'L', name + 'R']);
};
// vnoise(vec3(dot(p, a) * sa, dot(p, b) * sb, z))
const stroke = (name, a, b, sa, sb, z) => {
  n(name + 'A', 'dot', ['pos', a]); k(name + 'Sa', [sa, 0, 0, 0]); n(name + 'X', 'multiply', [name + 'A', name + 'Sa']);
  n(name + 'B', 'dot', ['pos', b]); k(name + 'Sb', [0, sb, 0, 0]); n(name + 'Y', 'multiply', [name + 'B', name + 'Sb']);
  k(name + 'Z', [0, 0, z, 0]); n(name + 'XY', 'add', [name + 'X', name + 'Y']); n(name + 'In', 'add', [name + 'XY', name + 'Z']);
  n(name, 'noise', [name + 'In']);
};
// three.js hex colors are sRGB; the shader works on their linear values.
const decode = (c) => (c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4));
const hex = (h, scale = 1) => [1, 3, 5].map((i) => decode(parseInt(h.slice(i, i + 2), 16) / 255) * scale);
const round = (v) => v.map((x) => +x.toFixed(4));
const norm = (v) => { const l = Math.hypot(...v); return v.map((x) => x / l); };

n('albedo', 'baseColor'); n('normal', 'normal'); n('ambient', 'ambient'); n('pos', 'objectPosition'); n('view', 'viewDirection');
k('zero', 0); k('one', 1); k('half', 0.5);
// boundary wobble and pigment grain: fbm over the rest pose
k('wobbleScale', 9); n('wobbleP', 'multiply', ['pos', 'wobbleScale']); n('wobbleFbm', 'fbm', ['wobbleP']);
n('wobbleC', 'subtract', ['wobbleFbm', 'half']); k('wobbleK', 0.18); n('wobble', 'multiply', ['wobbleC', 'wobbleK']);
k('grainScale', 140); n('grainP', 'multiply', ['pos', 'grainScale']); n('grainFbm', 'fbm', ['grainP']);
n('grainC', 'subtract', ['grainFbm', 'half']); k('grainK', 0.04); n('grainD', 'multiply', ['grainC', 'grainK']);
n('grain', 'add', ['one', 'grainD']);
// brushDabs: t = normalize(cross(up, N)) = normalize(N.z, 0, -N.x), b = cross(N, t)
swz('nzyx', 'normal', [2, 1, 0, 3]); k('tMask', [1, 0, -1, 0]); n('tRaw', 'multiply', ['nzyx', 'tMask']); n('t', 'normalize', ['tRaw']);
cross('b', 'normal', 't');
stroke('s1', 't', 'b', 16, 75, 1.7);
k('dMix', 0.8); n('b08', 'multiply', ['b', 'dMix']); n('dRaw', 'add', ['t', 'b08']); n('d', 'normalize', ['dRaw']);
cross('e', 'normal', 'd');
stroke('s2', 'd', 'e', 20, 85, 5.3);
k('s1K', 0.65 * 0.3); k('s2K', 0.35 * 0.3); k('dabBias', 0.5 * 0.3);
n('s1W', 'multiply', ['s1', 's1K']); n('s2W', 'multiply', ['s2', 's2K']); n('sSum', 'add', ['s1W', 's2W']);
n('dab', 'subtract', ['sSum', 'dabBias']);  // brushDabs * dabs (0.3)
// dab weight: |dab| * (1.2 [dab >= 0] + 0.8 [dab < 0] * (1 - lit))
n('dabAbs', 'abs', ['dab']); n('dabPos', 'step', ['zero', 'dab']); n('dabNeg', 'subtract', ['one', 'dabPos']);
k('k12', 1.2); k('k08', 0.8); n('pos12', 'multiply', ['dabPos', 'k12']); n('neg08', 'multiply', ['dabNeg', 'k08']);
// unlit (the ambient part, lit = 0): D0 = 1 + dabShadeK * w0
n('w0Sum', 'add', ['pos12', 'neg08']); n('w0', 'multiply', ['dabAbs', 'w0Sum']);
u('dabShadeK'); n('d0Tint', 'multiply', ['dabShadeK', 'w0']); n('d0', 'add', ['one', 'd0Tint']);
// backlight rim
n('ndv', 'dot', ['normal', 'view']); n('ndvC', 'clamp', ['ndv', 'zero', 'one']); n('fres0', 'subtract', ['one', 'ndvC']);
k('rimPow', 2.5); n('fres', 'pow', ['fres0', 'rimPow']);
u('rimDir'); n('rimDot', 'dot', ['normal', 'rimDir']); k('rimLow', -0.25); k('rimHigh', 0.55);
n('rimSide', 'smoothstep', ['rimLow', 'rimHigh', 'rimDot']); n('rimMask', 'multiply', ['fres', 'rimSide']);
u('rimColor'); n('rim', 'multiply', ['rimMask', 'rimColor']);
n('ambientD0', 'multiply', ['ambient', 'd0']); n('albedoAmb', 'multiply', ['albedo', 'ambientD0']);
n('colorRaw', 'add', ['albedoAmb', 'rim']); n('color', 'multiply', ['colorRaw', 'grain']);
// light stage
n('lightVector', 'lightVector'); n('ndl0', 'dot', ['normal', 'lightVector']); n('ndl', 'add', ['ndl0', 'wobble']);
k('bandLow', -0.32); k('bandHigh', 0.32); n('band', 'smoothstep', ['bandLow', 'bandHigh', 'ndl']);
n('attenuation', 'lightAttenuation'); n('lit', 'multiply', ['band', 'attenuation']);
n('unlit', 'subtract', ['one', 'lit']); n('edge', 'multiply', ['lit', 'unlit']);
u('termK'); n('termTint', 'multiply', ['edge', 'termK']); n('term', 'add', ['one', 'termTint']);
n('negLit', 'multiply', ['neg08', 'unlit']); n('wSum', 'add', ['pos12', 'negLit']); n('w', 'multiply', ['dabAbs', 'wSum']);
u('dabLightK'); n('dabTint', 'mix', ['dabShadeK', 'dabLightK', 'lit']); n('dTint', 'multiply', ['dabTint', 'w']);
n('dabs', 'add', ['one', 'dTint']);
n('lightColor', 'lightColor'); n('sun', 'multiply', ['lightColor', 'lit']); n('received', 'add', ['ambient', 'sun']);
n('termed', 'multiply', ['received', 'term']); n('painted', 'multiply', ['termed', 'dabs']);
n('added', 'subtract', ['painted', 'ambientD0']); n('lightAlbedo', 'multiply', ['albedo', 'added']);
n('light', 'multiply', ['lightAlbedo', 'grain']);

const minusOne = (c, weight) => c.map((x) => (x - 1) * weight);
const graph = {
  format: 'ownengine.shader',
  uniforms: {
    rimDir: [...round(norm([0.65, 0.45, -0.6])), 0],
    rimColor: [...round(hex('#ffe08a', 0.35)), 0],
    // mix(1, termColor * 1.15, lit (1 - lit) * 4 * 0.15)
    termK: [...round(minusOne(hex('#ffb060', 1.15), 0.6)), 0],
    // mix(col, col * tint, w) = col * (1 + (tint - 1) w)
    dabShadeK: [...round(minusOne(hex('#c0601e', 1.5), 1)), 0],
    dabLightK: [...round(minusOne(hex('#fff0b0', 1.2), 1)), 0],
  },
  nodes, color: ids.color, light: ids.light,
};
fs.writeFileSync(process.argv[2], JSON.stringify(graph, null, 1) + '\n');
console.log(nodes.length, 'nodes');
