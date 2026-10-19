// Generates games/Kongal/scenes/main.scene.json: meadow, props and the player (outlines come from the materials).
const fs = require('fs');
const E = [];
let next = 100;
const W = [1, 1, 1];
const add = (e) => { E.push(e); return e.id; };
const prop = (name, parent, pos, scale, mesh, mat, extra = {}) => {
  const id = add({ id: next++, name, ...(parent ? { parent } : {}), components: {
    Transform: { position: pos, scale }, MeshRenderer: { mesh, material: mat, color: W }, ...extra } });
  return id;
};
add({ id: 1, name: 'Main Camera', components: {
  Transform: { position: [0, 2.36, 3.4], rotation: [-28, 0, 0] },
  Camera: { fov: 36, clearColor: [0.97, 0.92, 0.82] },
  CameraFollow: { target: 10, offset: [0, 1.7, 3.0], lookOffset: [0, 0.1, 0], smoothing: 6 },
  PostProcess: { bloom: 0.18, bloomThreshold: 0.85, vignette: 0.12, vignetteRadius: 0.3, vignetteSoftness: 1.1, vignetteColor: [0.62, 0.42, 0.28], saturation: 0.97,
    shadowTint: [1.0, 0.96, 0.92], highlightTint: [1.04, 1.0, 0.92], fxaa: true,
    fog: 0.35, fogColor: [0.97, 0.92, 0.82], fogStart: 8, fogEnd: 28,
    shader: 'shaders/watercolor.shader.json' } } });
add({ id: 2, name: 'Sun', components: {
  Transform: { position: [0, 6, 0], rotation: [-50, 35, 0] },
  // the viewer's paintLight (linear project): sunColor #ffe6b8 * 0.45, hemisphere sky #f6ead4 / ground #d0a070 * 0.82
  DirectionalLight: { color: [1, 0.902, 0.7216], intensity: 0.45, ambient: [0.8838, 0.8405, 0.761], hemisphere: true,
    groundAmbient: [0.7466, 0.5733, 0.4], shadows: true, shadowStrength: 0.6 } } });
add({ id: 3, name: 'Sky', components: {
  Skybox: { sky: [0.93, 0.88, 0.76], horizon: [0.98, 0.93, 0.82], ground: [0.84, 0.74, 0.56], intensity: 0.9 } } });
add({ id: 4, name: 'Meadow', components: {
  Transform: { scale: [60, 1, 60] },
  MeshRenderer: { mesh: 'plane', material: 'materials/meadow.mat.json', color: W },
  Collider: { center: [0, -0.5, 0] } } });
// player: CharacterBody sphere centred at body height; the model child stands on the ground
add({ id: 10, name: 'Player', components: {
  Transform: { position: [0, 0.34, 0] },
  CharacterBody: { shape: 'sphere', radius: 0.34, stepHeight: 0.12 },
  Script: { path: 'scripts/kongal_player.lua', params: { walk: 1.4, run: 2.6 } },
  Tag: { tags: 'player' } } });
add({ id: 11, name: 'Kongal', parent: 10, components: {
  Transform: { position: [0, -0.34, 0] },
  MeshRenderer: { mesh: 'assets/models/kongal_v7_anim.glb', material: 'materials/kongal_toon.mat.json', color: W },
  Animator: { clip: 'Idle', blend: 0.2 } } });
// deterministic scatter
let seed = 7;
const rnd = () => ((seed = (seed * 16807) % 2147483647) / 2147483647);
const tree = (x, z, s) => {
  const root = add({ id: next++, name: 'Tree', components: {
    Transform: { position: [x, 0, z] }, Collider: { shape: 'capsule', radius: 0.16 * s, height: 1.6 * s, center: [0, 0.8 * s, 0] } } });
  prop('Trunk', root, [0, 0.45 * s, 0], [0.18 * s, 0.9 * s, 0.18 * s], 'cube', 'materials/toon_trunk.mat.json');
  prop('Canopy', root, [0, 1.15 * s, 0], [0.95 * s, 0.8 * s, 0.95 * s], 'sphere', 'materials/toon_leaf.mat.json');
  prop('Canopy', root, [0.28 * s, 1.45 * s, 0.1 * s], [0.6 * s, 0.55 * s, 0.6 * s], 'sphere', 'materials/toon_leaf.mat.json');
};
const rock = (x, z, s) => prop('Rock', null, [x, 0.12 * s, z], [0.55 * s, 0.32 * s, 0.45 * s], 'sphere',
  'materials/toon_rock.mat.json', { Collider: { shape: 'sphere', radius: 0.5 } });
const bush = (x, z, s) => prop('Bush', null, [x, 0.16 * s, z], [0.5 * s, 0.36 * s, 0.5 * s], 'sphere', 'materials/toon_leaf.mat.json');
const flower = (x, z) => prop('Flower', null, [x, 0.05, z], [0.07, 0.07, 0.07], 'sphere', 'materials/toon_flower.mat.json');
for (const [x, z, s] of [[-2.6, -2.2, 1.1], [3.1, -3.4, 1.3], [-4.4, 1.8, 1.0], [4.6, 1.2, 0.9], [0.6, -5.6, 1.4], [-1.2, 4.8, 1.2], [6.5, -6.5, 1.5], [-7, -5, 1.3]]) tree(x, z, s);
for (const [x, z, s] of [[1.8, -1.4, 1.0], [-1.6, 1.4, 0.8], [2.4, 2.8, 1.2], [-3.5, -4.5, 1.4], [5.2, -1.8, 0.9]]) rock(x, z, s);
for (let i = 0; i < 10; i++) bush((rnd() - 0.5) * 16, (rnd() - 0.5) * 16, 0.7 + rnd() * 0.6);
for (let i = 0; i < 40; i++) flower((rnd() - 0.5) * 14, (rnd() - 0.5) * 14);
add({ id: 13, name: 'Hint', components: {
  UIText: { text: 'WASD / 방향키: 이동   Shift: 달리기   Q/E: 카메라 회전', anchor: 'bottom', y: -24, size: 20, font: "assets/fonts/NanumMyeongjo-Bold.ttf", color: [0.3, 0.22, 0.13] } } });
fs.writeFileSync(process.argv[2], JSON.stringify({ format: 'ownengine.scene', version: 1, name: 'Meadow', entities: E }, null, 1));
console.log(E.length, 'entities');
