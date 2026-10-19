// Builds games/Kongal/shaders/watercolor.shader.json: the three.js WatercolorPass as an engine post graph.
const fs = require('fs');
const nodes = [], ids = {};
const n = (name, op, args, extra = {}) => {
  ids[name] = nodes.length;
  const node = { op, ...extra };
  if (args) node.args = args.map((a) => { if (!(a in ids)) throw new Error('unknown ' + a); return ids[a]; });
  nodes.push(node);
};
const k = (name, value) => n(name, 'constant', null, { value });
const u = (name, uniform) => n(name, 'uniform', null, { name: uniform });
// pixel coordinates with z = 1, so one multiply also adds a noise seed in z
n('uv', 'uv'); n('texel', 'texel');
n('size', 'swizzle', ['texel'], { value: [2, 3, 2, 3] });
n('fc0', 'multiply', ['uv', 'size']);
k('z1', [0, 0, 1, 0]); n('fc', 'add', ['fc0', 'z1']);
k('half', 0.5); k('one', 1); k('zero', 0);
// edges wander like pigment carried by water: fbm(fc * 0.012) per axis
k('wfx', [0.012, 0.012, 1.3, 0]); n('wpx', 'multiply', ['fc', 'wfx']); n('wx', 'fbm', ['wpx']);
k('wfy', [0.012, 0.012, 7.1, 0]); n('wpy', 'multiply', ['fc', 'wfy']); n('wy', 'fbm', ['wpy']);
k('pickY', [0, 1, 0, 0]); n('warp0', 'mix', ['wx', 'wy', 'pickY']); n('warp', 'subtract', ['warp0', 'half']);
u('wobble', 'wobble'); n('wpix', 'multiply', ['warp', 'wobble']); n('woff', 'multiply', ['wpix', 'texel']);
n('wuv', 'add', ['uv', 'woff']);
// soft bleed toward a small gaussian blur, pigment pooling where colour changes
n('color', 'sceneColor', ['wuv']); k('spacing', 1.6); n('blur', 'sceneBlur', ['wuv', 'spacing']);
u('bleed', 'bleed'); n('bled', 'mix', ['color', 'blur', 'bleed']);
n('diff', 'subtract', ['bled', 'blur']); n('dlen', 'length', ['diff']); k('six', 6); n('e0', 'multiply', ['dlen', 'six']);
n('edge', 'clamp', ['e0', 'zero', 'one']); u('edgeDarken', 'edgeDarken'); n('pool', 'multiply', ['edge', 'edgeDarken']);
n('poolK', 'subtract', ['one', 'pool']); n('pooled', 'multiply', ['bled', 'poolK']);
// blotchy wash density: pow(c, 1 + (fbm - 0.5) * wash)
k('wsf', [0.007, 0.007, 3.7, 0]); n('wsp', 'multiply', ['fc', 'wsf']); n('wsn', 'fbm', ['wsp']); n('wsc', 'subtract', ['wsn', 'half']);
u('wash', 'wash'); n('wsv', 'multiply', ['wsc', 'wash']); n('wexp', 'add', ['wsv', 'one']); n('washed', 'pow', ['pooled', 'wexp']);
// granulation and paper tooth, applied together: c *= 1 + gran + tooth
k('gf', [0.35, 0.35, 2.0, 0]); n('gp', 'multiply', ['fc', 'gf']); n('gn', 'fbm', ['gp']); n('gc', 'subtract', ['gn', 'half']);
u('granulation', 'granulation'); n('gran', 'multiply', ['gc', 'granulation']);
k('tf', [0.08, 0.08, 5.0, 0]); n('tp', 'multiply', ['fc', 'tf']); n('tfbm', 'fbm', ['tp']);
k('vf', [0.6, 0.6, 9.0, 0]); n('vp', 'multiply', ['fc', 'vf']); n('vn', 'noise', ['vp']);
k('vw', 0.3); n('tmix', 'mix', ['tfbm', 'vn', 'vw']); n('tc', 'subtract', ['tmix', 'half']);
u('paper', 'paper'); n('tooth', 'multiply', ['tc', 'paper']);
n('grain', 'add', ['gran', 'tooth']); n('grainK', 'add', ['grain', 'one']); n('out', 'multiply', ['washed', 'grainK']);
const graph = {
  format: 'ownengine.shader', stage: 'post',
  // three.js values with their constant factors folded in: wobble 1.4 * 2, edgeDarken 0.45 * 0.5,
  // washVariation 0.16 * 2, granulation 0.05 * 2, paper 0.045 * 2
  uniforms: { wobble: 2.8, bleed: 0.2, edgeDarken: 0.1, wash: 0.32, granulation: 0.045, paper: 0.041 },
  nodes, color: ids.out,
};
fs.writeFileSync(process.argv[2], JSON.stringify(graph, null, 1) + '\n');
console.log(nodes.length, 'nodes');
