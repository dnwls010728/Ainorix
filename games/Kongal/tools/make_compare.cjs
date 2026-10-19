// Generates games/Kongal/scenes/compare.scene.json: Kongal alone on a cream backdrop, framed like the default
// view of the three.js prototype viewer (kongal/viewer: camera (1.1, 0.85, 2.6) looking at (0, 0.42, 0), fov 30,
// light toward (-0.55, 0.7, 0.45)), with the Idle clip held at `pose` seconds, for side-by-side captures.
// Usage: node tools/make_compare.cjs scenes/compare.scene.json [poseSeconds]
const fs = require('fs');
const pose = Number(process.argv[3] ?? 0.5);
const deg = (r) => (r * 180) / Math.PI;
// Euler [pitch, yaw, 0] (degrees) whose local -Z points along `dir` (yaw about Y applied after pitch about X).
const aim = (dir) => {
  const len = Math.hypot(...dir);
  const [x, y, z] = dir.map((v) => v / len);
  return [deg(Math.asin(y)), deg(Math.atan2(-x, -z)), 0];
};
const eye = [1.1, 0.85, 2.6], target = [0, 0.42, 0];
const W = [1, 1, 1];
const entities = [
  { id: 1, name: 'Main Camera', components: {
    Transform: { position: eye, rotation: aim(target.map((t, i) => t - eye[i])) },
    Camera: { fov: 30, nearPlane: 0.05, farPlane: 50, clearColor: [0.925, 0.93, 0.905] },
    PostProcess: { bloom: 0.18, bloomThreshold: 0.85, vignette: 0.35, vignetteRadius: 0.3, vignetteSoftness: 1.1, vignetteColor: [0.8, 0.6, 0.37], saturation: 0.97,
      shadowTint: [1.0, 0.96, 0.92], highlightTint: [1.04, 1.0, 0.92], fxaa: true,
      shader: 'shaders/watercolor.shader.json' } } },
  { id: 2, name: 'Sun', components: {
    // the viewer's lightDir points toward the light; a DirectionalLight's -Z is the direction light travels
    Transform: { position: [0, 3, 0], rotation: aim([0.55, -0.7, -0.45]) },
    // the viewer's paintLight (linear project): sunColor #ffe6b8 * 0.45, hemisphere sky #f6ead4 / ground #d0a070 * 0.82
    DirectionalLight: { color: [1, 0.902, 0.7216], intensity: 0.45, ambient: [0.8838, 0.8405, 0.761], hemisphere: true,
      groundAmbient: [0.7466, 0.5733, 0.4], shadows: true, shadowStrength: 0.3 } } },
  // no floor: the viewer draws a backdrop and a soft contact blob (radius 0.6) instead
  { id: 3, name: 'Blob', components: {
    Transform: { position: [0, 0.002, 0], rotation: [-90, 0, 0], scale: [1.2, 1.2, 1] },
    MeshRenderer: { mesh: 'quad', material: 'materials/blob.mat.json', color: W, castShadows: false } } },
  { id: 4, name: 'Kongal', components: {
    MeshRenderer: { mesh: 'assets/models/kongal_v7_anim.glb', material: 'materials/kongal_toon.mat.json', color: W },
    Animator: { clip: 'Idle', playing: false, time: pose } } },
];
fs.writeFileSync(process.argv[2], JSON.stringify({ format: 'ownengine.scene', version: 1, name: 'Compare', entities }, null, 1) + '\n');
console.log('camera', entities[0].components.Transform.rotation.map((v) => v.toFixed(2)).join(','),
  'sun', entities[1].components.Transform.rotation.map((v) => v.toFixed(2)).join(','));
