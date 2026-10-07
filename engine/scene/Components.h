#pragma once
#include <memory>
#include <string>
#include <vector>

#include "scene/Reflect.h"

namespace oe {
struct VfxAsset;

// Built-in components. To add one: declare a struct with kTypeName, kDoc and
// Reflect(), then register it in RegisterBuiltinComponents(). Serialization,
// the API, schemas and the editor inspector pick it up automatically.

struct Transform {
    static constexpr const char* kTypeName = "Transform";
    static constexpr const char* kDoc = "Position, rotation (Euler degrees, applied X then Y then Z) and scale relative to the parent entity.";
    Vec3 position{0, 0, 0};
    Vec3 rotation{0, 0, 0};
    Vec3 scale{1, 1, 1};
    static void Reflect(FieldList& f) {
        f.Add("position", &Transform::position, "Local position in meters. +Y is up.");
        f.Add("rotation", &Transform::rotation, "Local rotation as Euler angles in degrees.");
        f.Add("scale", &Transform::scale, "Local scale factors.");
    }
};

struct MeshRenderer {
    static constexpr const char* kTypeName = "MeshRenderer";
    static constexpr const char* kDoc = "Draws a mesh: a built-in shape (cube, sphere, plane, plane64, pyramid, quad) or a glTF model file. Unknown meshes render as a magenta cube.";
    std::string mesh = "cube";
    std::string material;
    Json shaderUniforms = Json::MakeObject();
    Color color{0.8f, 0.8f, 0.8f};
    float opacity = 1.0f;
    std::string texture;
    std::string shading = "smooth";
    bool unlit = false;
    bool castShadows = true;
    bool visible = true;
    static void Reflect(FieldList& f) {
        f.Add("mesh", &MeshRenderer::mesh, "Built-in name (cube, sphere, plane, plane64 = plane split into 64x64 cells for shader graph vertex offsets, pyramid, quad) or model path, e.g. \"assets/models/fox.glb\" (.glb/.gltf).");
        f.Add("material", &MeshRenderer::material,
              "Material file (*.mat.json: PBR color/metallic/roughness/normal/emissive maps, transparency) used for every part of the mesh. "
              "Empty = the model's own materials (glTF) or the default (white, roughness 0.7).");
        f.Add("shaderUniforms", &MeshRenderer::shaderUniforms,
              "Per-entity values for the material's shader graph uniforms: {name: number or [x, y, z, w]}. Unnamed uniforms keep the "
              "material's values; scripts can change them every frame. An unknown name or a material without a graph renders magenta.");
        f.Add("color", &MeshRenderer::color, "Tint multiplied with the material/texture color, linear RGB 0..1 (or \"#rrggbb\").");
        FieldInfo& op = f.Add("opacity", &MeshRenderer::opacity, "Below 1 the mesh is drawn transparent (alpha blended, sorted back to front, no shadow).");
        op.hasRange = true;
        op.min = 0.0f;
        op.max = 1.0f;
        f.Add("texture", &MeshRenderer::texture, "Image (.png/.jpg) overriding the material's base color texture. Empty = use the material's.");
        f.Add("shading", &MeshRenderer::shading, "smooth = interpolated vertex normals, flat = faceted.").options = {"smooth", "flat"};
        f.Add("unlit", &MeshRenderer::unlit, "Ignore lighting and shadows (UI-like, emissive look). Overrides the material.");
        f.Add("castShadows", &MeshRenderer::castShadows, "Casts shadows from the directional light.");
        f.Add("visible", &MeshRenderer::visible, "Whether the mesh is drawn.");
    }
};

struct Animator {
    static constexpr const char* kTypeName = "Animator";
    static constexpr const char* kDoc = "Fixed-step glTF TRS animation on the entity's MeshRenderer; clips are listed by asset.info.";
    std::string clip;
    float speed = 1.0f;
    bool loop = true;
    bool playing = true;
    float time = 0.0f;
    static void Reflect(FieldList& f) {
        f.Add("clip", &Animator::clip, "Exact model clip name; empty = default pose. animation.play changes clips and restarts by default.");
        f.Add("speed", &Animator::speed, "Playback multiplier; negative plays backwards, zero holds the pose.");
        f.Add("loop", &Animator::loop, "Wrap time at the clip duration; otherwise clamp and stop at either endpoint.");
        f.Add("playing", &Animator::playing, "Advance time while true; false holds the current pose.");
        f.Add("time", &Animator::time, "Seconds into the clip; set to seek. Direct clip edits preserve time.");
    }
};

// Runtime particle values are snapshots of the emitter settings at birth.
struct Particle {
    Vec3 position, velocity, gravity;
    float age = 0, lifetime = 1;
    float drag = 0;
    float startSize = 0.1f, endSize = 0;
    Color startColor, endColor;
    float startOpacity = 1, endOpacity = 0;
    float rotation = 0, spin = 0;  // degrees around the view axis, degrees per second
    float sizeScale = 1;           // sizeVariation factor, applied to sizeOverLife keys
    int frameOffset = 0;           // randomFrame: first flipbook frame of this particle
    Mat4 meshBasis;    // world-space mesh orientation captured at birth
    float random = 0;  // stable birth hash, no extra random draws
    int depth = 0;     // bounded death-chain generation
    bool worldSpace = false;
};

struct ParticleEmitter {
    static constexpr const char* kTypeName = "ParticleEmitter";
    static constexpr const char* kDoc = "Deterministic 2D/3D billboard particles; initial burst and continuous rate, bounded per emitter.";
    float rate = 10;
    int burst = 0;
    float lifetime = 1;
    float speed = 1;
    float spread = 30;
    float speedVariation = 0, lifetimeVariation = 0, sizeVariation = 0;
    float drag = 0;
    Vec3 direction{0, 1, 0};
    Vec3 gravity{0, -9.8f, 0};
    float startSize = 0.1f, endSize = 0;
    Color startColor{1, 1, 1}, endColor{1, 1, 1};
    float startOpacity = 1, endOpacity = 0;
    std::string texture;
    std::string material;  // optional .mat.json surface graph
    std::string mesh;      // empty = billboard; otherwise static project/built-in mesh
    Vec3 meshScale{1, 1, 1};
    bool batching = false;  // opt-in atomic emitter draw; legacy per-particle sorting otherwise
    std::string onDeath;   // named target emitter in the same VFX asset
    int deathCount = 1;
    int frame = 0, columns = 1, rows = 1;
    std::string space = "local";
    int dimensions = 3;
    int seed = 1;
    int maxParticles = 256;
    std::string blend = "alpha";
    int layer = 0;
    bool loop = true;
    bool playing = true;
    float delay = 0, duration = 0;
    std::string shape = "point";
    Vec3 shapeSize{1, 1, 1};
    bool shapeEdge = false;
    float radialSpeed = 0;
    float rotation = 0, rotationVariation = 0;
    float spin = 0, spinVariation = 0;
    std::string align = "camera";
    float stretch = 0;
    int frames = 1;
    float frameRate = 0;
    bool randomFrame = false;
    Json sizeOverLife = Json::MakeArray();
    Json colorOverLife = Json::MakeArray();
    Json opacityOverLife = Json::MakeArray();
    float turbulence = 0, turbulenceScale = 1;
    bool pixelArt = true;
    std::vector<Particle> particles;  // runtime, not serialized
    uint32_t randomState = 0;        // runtime xorshift stream; zero = uninitialized
    double carry = 0;               // fractional rate births, bounded to [0,1)
    double elapsed = 0;             // seconds simulated while playing; drives delay/duration
    int64_t cycle = 0;              // duration cycles started (loop with a duration re-fires the burst)
    bool initialBurstEmitted = false;
    uint64_t emitted = 0;            // accepted births since creation/clear
    bool invalidSettingsReported = false;  // suppress repeated runtime error logs
    static void Reflect(FieldList& f) {
        auto range = [](FieldInfo& field, float lo, float hi) { field.hasRange = true; field.min = lo; field.max = hi; };
        range(f.Add("rate", &ParticleEmitter::rate, "Automatic particles per second while loop and playing are true."), 0, 10000);
        range(f.Add("burst", &ParticleEmitter::burst, "Automatic initial burst on the first playing step."), 0, 10000);
        range(f.Add("lifetime", &ParticleEmitter::lifetime, "Lifetime in seconds, captured at birth."), 0.001f, 600);
        range(f.Add("speed", &ParticleEmitter::speed, "Initial speed in emitter coordinates, captured at birth."), 0, 10000);
        range(f.Add("spread", &ParticleEmitter::spread, "Cone half-angle in degrees around direction; planar fan in 2D."), 0, 180);
        range(f.Add("speedVariation", &ParticleEmitter::speedVariation, "0..1: each particle starts between speed * (1 - variation) and speed."), 0, 1);
        range(f.Add("lifetimeVariation", &ParticleEmitter::lifetimeVariation, "0..1: each particle lives between lifetime * (1 - variation) and lifetime."), 0, 1);
        range(f.Add("sizeVariation", &ParticleEmitter::sizeVariation, "0..1: each particle's start and end size are scaled between (1 - variation) and 1."), 0, 1);
        range(f.Add("drag", &ParticleEmitter::drag, "Velocity lost per second (0 = none, 4 = slows to a stop quickly), captured at birth."), 0, 60);
        f.Add("direction", &ParticleEmitter::direction, "Emission direction; zero falls back to +Y. 2D uses XY only.");
        f.Add("gravity", &ParticleEmitter::gravity, "Acceleration in particle coordinates (world or local), captured at birth.");
        range(f.Add("startSize", &ParticleEmitter::startSize, "Billboard size in meters at birth."), 0, 10000);
        range(f.Add("endSize", &ParticleEmitter::endSize, "Billboard size at expiry."), 0, 10000);
        f.Add("startColor", &ParticleEmitter::startColor, "RGB tint at birth.");
        f.Add("endColor", &ParticleEmitter::endColor, "RGB tint at expiry.");
        range(f.Add("startOpacity", &ParticleEmitter::startOpacity, "Opacity at birth."), 0, 1);
        range(f.Add("endOpacity", &ParticleEmitter::endOpacity, "Opacity at expiry."), 0, 1);
        f.Add("material", &ParticleEmitter::material, "Optional project-relative .mat.json, including surface graphs.");
        f.Add("mesh", &ParticleEmitter::mesh, "Static project mesh or built-in name; empty uses camera-facing quads.");
        f.Add("meshScale", &ParticleEmitter::meshScale, "Mesh XYZ scale multiplied by lifetime size; orientation follows emitter and rotation.");
        f.Add("batching", &ParticleEmitter::batching, "Combine particles into one emitter draw, sorted internally; transparent emitters sort as whole groups.");
        f.Add("onDeath", &ParticleEmitter::onDeath, "VFX asset only: named emitter receiving a burst at each expired particle position; maximum depth 4.");
        range(f.Add("deathCount", &ParticleEmitter::deathCount, "Particles per death event, subject to target and system budgets."), 0, 256);
        f.Add("texture", &ParticleEmitter::texture, "Optional texture/sprite sheet; empty uses solid quads.");
        range(f.Add("frame", &ParticleEmitter::frame, "Sprite sheet frame, row-major."), 0, 1000000);
        range(f.Add("columns", &ParticleEmitter::columns, "Sprite sheet columns."), 1, 4096);
        range(f.Add("rows", &ParticleEmitter::rows, "Sprite sheet rows."), 1, 4096);
        f.Add("space", &ParticleEmitter::space, "Local particles follow the emitter; world particles remain where spawned.").options = {"local", "world"};
        range(f.Add("dimensions", &ParticleEmitter::dimensions, "2 = XY fan/planar motion; 3 = cone in 3D."), 2, 3);
        f.Add("seed", &ParticleEmitter::seed, "Fixed seed mixed with entity id; changes take effect after particles.clear.");
        range(f.Add("maxParticles", &ParticleEmitter::maxParticles, "Maximum live particles per emitter; excess births are dropped."), 0, 10000);
        f.Add("blend", &ParticleEmitter::blend, "alpha = normal; add = particles brighten what is behind them (sparks, fire).").options = {"alpha", "add"};
        f.Add("layer", &ParticleEmitter::layer, "2D sorting layer, like Sprite.layer: particles draw in front of lower layers.");
        f.Add("loop", &ParticleEmitter::loop, "Enable continuous rate births; false emits only the initial burst.");
        f.Add("playing", &ParticleEmitter::playing, "Automatic emission switch; existing particles continue to age while false.");
        range(f.Add("delay", &ParticleEmitter::delay, "Seconds of playing before the initial burst and the rate start (layered effects: flash, then smoke)."), 0, 600);
        range(f.Add("duration", &ParticleEmitter::duration,
                    "0 = no limit. Otherwise the rate emits for this many seconds after the delay; with loop the cycle repeats and re-fires the burst."), 0, 600);
        f.Add("shape", &ParticleEmitter::shape,
              "Where particles are born, in emitter coordinates: point, sphere (radius shapeSize.x), box (half extents shapeSize) or "
              "circle (radius shapeSize.x; in XY for 2D, on the ground plane XZ for 3D).").options = {"point", "sphere", "box", "circle"};
        f.Add("shapeSize", &ParticleEmitter::shapeSize, "Sphere/circle radius in x, or box half extents, in meters.");
        f.Add("shapeEdge", &ParticleEmitter::shapeEdge, "Spawn only on the shape's surface or rim (rings, shells).");
        range(f.Add("radialSpeed", &ParticleEmitter::radialSpeed,
                    "Extra speed away from the emitter origin (negative = toward it), for particles born off the origin; captured at birth."), -10000, 10000);
        range(f.Add("rotation", &ParticleEmitter::rotation, "Billboard rotation in degrees at birth, counter-clockwise on screen."), -36000, 36000);
        range(f.Add("rotationVariation", &ParticleEmitter::rotationVariation, "Each particle starts within +/- this many degrees of rotation."), 0, 360);
        range(f.Add("spin", &ParticleEmitter::spin, "Rotation speed in degrees per second, captured at birth."), -36000, 36000);
        range(f.Add("spinVariation", &ParticleEmitter::spinVariation, "Each particle spins within +/- this many degrees per second of spin."), 0, 36000);
        f.Add("align", &ParticleEmitter::align,
              "camera = upright billboards; velocity = the billboard's up axis follows the direction of travel on screen (sparks, rain).").options = {"camera", "velocity"};
        range(f.Add("stretch", &ParticleEmitter::stretch, "align velocity only: seconds of travel added to the billboard's length (0.05 = streaks)."), 0, 100);
        range(f.Add("frames", &ParticleEmitter::frames, "Flipbook length: frames played from `frame` on, row-major. 1 = a single frame."), 1, 1000000);
        range(f.Add("frameRate", &ParticleEmitter::frameRate, "Flipbook frames per second, looping; 0 plays the frames once over the particle's lifetime."), 0, 240);
        f.Add("randomFrame", &ParticleEmitter::randomFrame, "Each particle starts at a random flipbook frame and keeps it when frameRate is 0 (a sheet of variants).");
        f.Add("sizeOverLife", &ParticleEmitter::sizeOverLife,
              "Size keys [[t, meters], ...] with t ascending in 0..1; replaces startSize/endSize when not empty (sizeVariation still scales it). At most 16 keys.");
        f.Add("colorOverLife", &ParticleEmitter::colorOverLife,
              "Color keys [[t, [r, g, b]] or [t, \"#rrggbb\"], ...]; replaces startColor/endColor when not empty. At most 16 keys.");
        f.Add("opacityOverLife", &ParticleEmitter::opacityOverLife,
              "Opacity keys [[t, 0..1], ...]; replaces startOpacity/endOpacity when not empty (fade in, hold, fade out). At most 16 keys.");
        range(f.Add("turbulence", &ParticleEmitter::turbulence, "Strength in m/s^2 of a fixed noise field that pushes particles around (smoke, embers)."), 0, 10000);
        range(f.Add("turbulenceScale", &ParticleEmitter::turbulenceScale, "Noise cells per meter: small = wide swirls, large = jitter."), 0.001f, 1000);
        f.Add("pixelArt", &ParticleEmitter::pixelArt, "Sharp nearest-neighbour texels; false filters the texture smoothly (soft glows, smoke).");
    }
};

// One sample of the path a Trail follows, in world coordinates.
struct TrailPoint {
    Vec3 position;
    float age = 0;
    Vec3 tip;  // second world-space anchor in ribbon mode
};

struct Trail {
    static constexpr const char* kTypeName = "Trail";
    static constexpr const char* kDoc = "Camera-facing ribbon left behind the entity as it moves (sword swings, projectiles, dashes); it narrows and fades along its length.";
    float lifetime = 0.5f;
    float minDistance = 0.05f;
    float width = 0.2f, endWidth = 0;
    Color color{1, 1, 1}, endColor{1, 1, 1};
    float opacity = 1, endOpacity = 0;
    std::string texture;
    std::string material;
    std::string mode = "camera";  // ribbon sweeps base to tip, independent of camera
    std::string baseAnchor, tipAnchor;  // optional unique entity names; empty uses owner
    Vec3 baseOffset, tipOffset{0, 1, 0};
    float duration = 0;  // finite emission window; zero keeps emitting
    double elapsed = 0;  // runtime emission clock
    std::string blend = "alpha";
    int layer = 0;
    int maxPoints = 64;
    bool emitting = true;
    std::vector<TrailPoint> points;  // runtime, oldest first; not serialized
    static void Reflect(FieldList& f) {
        auto range = [](FieldInfo& field, float lo, float hi) { field.hasRange = true; field.min = lo; field.max = hi; };
        range(f.Add("lifetime", &Trail::lifetime, "Seconds a point of the path stays; with the entity's speed it sets the ribbon's length."), 0.01f, 60);
        range(f.Add("minDistance", &Trail::minDistance, "Meters the entity moves before the path gets a new point (smaller = smoother curves)."), 0.001f, 100);
        range(f.Add("width", &Trail::width, "Ribbon width in meters at the entity."), 0, 1000);
        range(f.Add("endWidth", &Trail::endWidth, "Width at the oldest point."), 0, 1000);
        f.Add("color", &Trail::color, "RGB tint at the entity.");
        f.Add("endColor", &Trail::endColor, "RGB tint at the oldest point.");
        range(f.Add("opacity", &Trail::opacity, "Opacity at the entity."), 0, 1);
        range(f.Add("endOpacity", &Trail::endOpacity, "Opacity at the oldest point."), 0, 1);
        f.Add("material", &Trail::material, "Optional .mat.json surface material/graph.");
        f.Add("mode", &Trail::mode, "camera faces the view; ribbon sweeps between two world-space anchors.").options = {"camera", "ribbon"};
        f.Add("baseAnchor", &Trail::baseAnchor, "Unique scene entity name for blade base; empty uses owner/local block transform.");
        f.Add("tipAnchor", &Trail::tipAnchor, "Unique scene entity name for blade tip; empty uses owner/local block transform.");
        f.Add("baseOffset", &Trail::baseOffset, "Base offset in anchor coordinates.");
        f.Add("tipOffset", &Trail::tipOffset, "Tip offset in anchor coordinates.");
        range(f.Add("duration", &Trail::duration, "Seconds of emission, followed by drain; zero emits indefinitely."), 0, 600);
        f.Add("texture", &Trail::texture, "Optional image stretched over the ribbon: left edge at the entity, right edge at the oldest point. Empty = solid.");
        f.Add("blend", &Trail::blend, "alpha = normal; add = the ribbon brightens what is behind it (energy, fire).").options = {"alpha", "add"};
        f.Add("layer", &Trail::layer, "2D sorting layer, like Sprite.layer.");
        range(f.Add("maxPoints", &Trail::maxPoints, "Most path points kept; the oldest are dropped first."), 2, 1024);
        f.Add("emitting", &Trail::emitting, "Add new points while true; false lets the existing ribbon fade out.");
    }
};

// A linked effect's authored fields; all instance arrays/counters are transient and copied by snapshots.
struct VFXSystem {
    static constexpr const char* kTypeName = "VFXSystem";
    static constexpr const char* kDoc = "Live-linked .vfx.json effect with named parameters, independent emitters and a shared particle budget.";
    std::string asset;  // project-relative .vfx.json
    Json params = Json::MakeObject();  // named instance overrides
    bool playing = true;  // enable automatic emission; existing particles/points still age
    bool autoDestroy = false;  // destroy the owner once finished
    std::shared_ptr<const VfxAsset> definition;  // immutable cached definition retained in native snapshots
    std::string loadedPath, error;  // last successful source and current reload diagnostic
    Json appliedParams;  // last validated overrides; avoids per-frame configuration work
    Json runtimeParams = Json::MakeObject();  // Lua/command overrides excluded from authored scene JSON
    bool playbackOverride = false, runtimePlaying = true;  // runtime controls leave authored playing unchanged
    std::vector<ParticleEmitter> emitters;
    std::vector<Trail> trails;
    bool started = false;
    bool suppressDestroy = false;  // a clear operation can leave an empty reusable instance
    static void Reflect(FieldList& f) {
        f.Add("asset", &VFXSystem::asset, "Project-relative effect asset (*.vfx.json).");
        f.Add("params", &VFXSystem::params, "Named parameter overrides defined by the effect asset; unknown names/ranges are errors.");
        f.Add("playing", &VFXSystem::playing, "Automatic emission switch; existing particles and trail points keep aging while false.");
        f.Add("autoDestroy", &VFXSystem::autoDestroy, "Destroy the owner and its children after the effect finishes (vfx.spawn defaults to true).");
    }
};

// ----- 2D --------------------------------------------------------------------------

struct Sprite {
    static constexpr const char* kTypeName = "Sprite";
    static constexpr const char* kDoc = "Draws an image (or one frame of a sprite sheet) on a quad facing +Z, placed at the entity. Transparent pixels are cut out. For 2D games use an orthographic Camera looking down -Z.";
    std::string texture;
    Color color{1, 1, 1};
    int frame = 0;
    int columns = 1;
    int rows = 1;
    float pixelsPerUnit = 16.0f;
    float width = 0.0f;
    float height = 0.0f;
    float pivotX = 0.5f;
    float pivotY = 0.5f;
    bool flipX = false;
    bool flipY = false;
    bool pixelArt = true;
    float alphaCutoff = 0.5f;
    float opacity = 1.0f;
    bool lit = false;
    std::string blend = "alpha";
    int layer = 0;
    int order = 0;
    bool castShadows = false;
    bool visible = true;
    static void Reflect(FieldList& f) {
        f.Add("texture", &Sprite::texture, "Image file (.png with transparency). A sprite sheet is a grid of equally sized frames.");
        f.Add("color", &Sprite::color, "Tint multiplied with the image.");
        f.Add("frame", &Sprite::frame, "Frame index in the sheet, row by row from the top-left (SpriteAnimation sets it).");
        f.Add("columns", &Sprite::columns, "Frames per row in the sheet.");
        f.Add("rows", &Sprite::rows, "Rows of frames in the sheet.");
        f.Add("pixelsPerUnit", &Sprite::pixelsPerUnit, "Image pixels per world unit; sets the size when width/height are 0 (16 = a 16 px frame is 1 unit).");
        f.Add("width", &Sprite::width, "Width in world units (0 = frame pixels / pixelsPerUnit). Transform.scale multiplies it.");
        f.Add("height", &Sprite::height, "Height in world units (0 = from pixels).");
        f.Add("pivotX", &Sprite::pivotX, "Point of the sprite placed at the entity position: 0 = left edge, 0.5 = center, 1 = right edge.");
        f.Add("pivotY", &Sprite::pivotY, "0 = bottom edge (feet), 0.5 = center, 1 = top edge.");
        f.Add("flipX", &Sprite::flipX, "Mirror horizontally (face left).");
        f.Add("flipY", &Sprite::flipY, "Mirror vertically.");
        f.Add("pixelArt", &Sprite::pixelArt, "Sharp nearest-neighbour pixels instead of smooth filtering.");
        f.Add("alphaCutoff", &Sprite::alphaCutoff, "Pixels with alpha below this are not drawn. 0 = soft edges: the image's alpha is blended (smoke, glows, UI-like art).");
        FieldInfo& op = f.Add("opacity", &Sprite::opacity, "Below 1 the sprite is drawn see-through (alpha blended).");
        op.hasRange = true;
        op.min = 0.0f;
        op.max = 1.0f;
        f.Add("lit", &Sprite::lit, "Apply scene lighting (default: full brightness, like classic 2D).");
        f.Add("blend", &Sprite::blend, "alpha = normal; add = the image brightens what is behind it (glows, fire, light beams) and is always blended.").options = {"alpha", "add"};
        f.Add("layer", &Sprite::layer,
              "2D sorting layer: sprites of a higher layer are drawn in front of lower ones at the same depth, whatever their "
              "position on screen (background 0, characters 1, effects 2, ...).");
        f.Add("order", &Sprite::order, "Sorting inside the layer: higher is drawn in front.");
        f.Add("castShadows", &Sprite::castShadows,
              "Cast a shadow shaped by the image (texels below alphaCutoff cast none). Needs lit: true and a cut-out sprite "
              "(alphaCutoff > 0); the sprite is then drawn from both sides.");
        f.Add("visible", &Sprite::visible, "Whether the sprite is drawn.");
    }
};

struct SpriteAnimation {
    static constexpr const char* kTypeName = "SpriteAnimation";
    static constexpr const char* kDoc = "Plays frame sequences on the entity's Sprite. clips = {\"run\": {\"frames\": [2,3,4,5], \"fps\": 10, \"loop\": true}}; set clip to switch (restarts it).";
    Json clips = Json::MakeObject();
    std::string clip;
    float speed = 1.0f;
    bool playing = true;
    float time = 0.0f;
    bool finished = false;
    std::string current;  // runtime: clip that `time` refers to (not reflected)
    static void Reflect(FieldList& f) {
        f.Add("clips", &SpriteAnimation::clips, "Clip name -> {frames: [indices], fps: number, loop: bool (default true)}.");
        f.Add("clip", &SpriteAnimation::clip, "Clip being played. Changing it starts the new clip from its first frame.");
        f.Add("speed", &SpriteAnimation::speed, "Playback speed multiplier.");
        f.Add("playing", &SpriteAnimation::playing, "Pause/resume.");
        f.Add("time", &SpriteAnimation::time, "Runtime: seconds into the current clip.");
        f.Add("finished", &SpriteAnimation::finished, "Runtime: true once a non-looping clip reached its last frame.");
    }
};

struct Tilemap {
    static constexpr const char* kTypeName = "Tilemap";
    static constexpr const char* kDoc = "Grid of tiles written as text rows (map): each character is a tile defined by a tileset file or the legend (fixed frame, random variants or autotile that picks frames from the neighbours). The entity position is the top-left corner; rows go down (-Y), columns right (+X). Tiles can be solid, one-way or shaped (slopes) for 2D and 3D physics.";
    std::string tileset;
    int columns = 1;
    int rows = 1;
    float tileSize = 1.0f;
    Json map = Json::MakeArray();
    Json legend = Json::MakeObject();
    std::string solid;
    Color color{1, 1, 1};
    bool pixelArt = true;
    bool lit = false;
    bool visible = true;
    static void Reflect(FieldList& f) {
        f.Add("tileset", &Tilemap::tileset, "Tileset: an image (grid of equally sized tiles, see columns/rows) or a *.tileset.json file (image, grid and tile rules shared by maps).");
        f.Add("columns", &Tilemap::columns, "Tiles per row in the tileset image (ignored with a .tileset.json).");
        f.Add("rows", &Tilemap::rows, "Rows of tiles in the tileset image (ignored with a .tileset.json).");
        f.Add("tileSize", &Tilemap::tileSize, "Size of one tile in world units.");
        f.Add("map", &Tilemap::map, "Array of strings, one per row from the top; one character per tile. Characters without a rule (space, '.') are empty.");
        f.Add("legend", &Tilemap::legend, "Character -> rule; overrides the tileset file. A rule is a frame number or {frame, variants, autotile: \"sides\"|\"blob\", frames, connects, edges, collision}, e.g. {\"#\": {\"autotile\": \"blob\", \"frame\": 0, \"collision\": \"solid\"}, \"=\": {\"frame\": 47, \"collision\": \"oneway\"}}.");
        f.Add("solid", &Tilemap::solid, "Characters that collide as full blocks, e.g. \"#=\" (shortcut for collision: \"solid\").");
        f.Add("color", &Tilemap::color, "Tint.");
        f.Add("pixelArt", &Tilemap::pixelArt, "Sharp nearest-neighbour pixels.");
        f.Add("lit", &Tilemap::lit, "Apply scene lighting.");
        f.Add("visible", &Tilemap::visible, "Whether the tiles are drawn (they still collide).");
    }
};

struct Camera {
    static constexpr const char* kTypeName = "Camera";
    static constexpr const char* kDoc = "Camera looking down its local -Z axis. The first active camera renders the game view.";
    std::string projection = "perspective";
    float fov = 60.0f;
    float orthoSize = 5.0f;
    float nearPlane = 0.1f;
    float farPlane = 500.0f;
    Color clearColor{0.12f, 0.14f, 0.18f};
    bool active = true;
    static void Reflect(FieldList& f) {
        f.Add("projection", &Camera::projection, "perspective or orthographic (no perspective shrink; good for 2D/isometric).").options = {"perspective", "orthographic"};
        f.Add("orthoSize", &Camera::orthoSize, "Orthographic: half of the visible height in meters.");
        FieldInfo& fov = f.Add("fov", &Camera::fov, "Perspective: vertical field of view in degrees.");
        fov.hasRange = true; fov.min = 1.0f; fov.max = 179.0f;
        f.Add("nearPlane", &Camera::nearPlane, "Near clip distance in meters.");
        f.Add("farPlane", &Camera::farPlane, "Far clip distance in meters.");
        f.Add("clearColor", &Camera::clearColor, "Background color.");
        f.Add("active", &Camera::active, "Only the first active camera (lowest entity id) is used.");
    }
};

struct PostProcess {
    static constexpr const char* kTypeName = "PostProcess";
    static constexpr const char* kDoc = "Optional screen effects on the active Camera. Defaults preserve existing frames; UI and selection outlines are unaffected.";
    float exposure = 1.0f;         // multiplier applied before tone mapping; one is neutral
    std::string toneMapping = "none";  // none or reinhard (per-channel c / (1 + c))
    float bloom = 0.0f;            // blurred highlight strength; zero disables bloom
    float bloomThreshold = 1.0f;   // threshold in unexposed scene RGB, using the largest channel
    int bloomRadius = 8;           // separable tent filter radius in scene pixels
    bool fxaa = false;             // directional edge smoothing after color effects, before overlays
    float vignette = 0.0f;          // edge darkening strength; zero disables the effect
    float vignetteRadius = 0.75f;   // normalized distance from screen center
    float vignetteSoftness = 0.5f;  // smooth transition width
    int dofRadius = 0;             // largest depth-of-field blur radius in scene pixels; zero disables it
    float dofFocus = 10.0f;        // view depth in meters that is sharp
    float dofRange = 2.0f;         // depth on either side of dofFocus that stays sharp
    float dofFalloff = 8.0f;       // further depth over which the blur grows to dofRadius
    static void Reflect(FieldList& f) {
        FieldInfo& exposureField = f.Add("exposure", &PostProcess::exposure, "Scene brightness multiplier before tone mapping. 1 preserves brightness.");
        exposureField.hasRange = true; exposureField.min = 0; exposureField.max = 32;
        f.Add("toneMapping", &PostProcess::toneMapping, "none (disabled) or reinhard (compress HDR channels as c / (1 + c)).").options = {"none", "reinhard"};
        FieldInfo& bloomStrength = f.Add("bloom", &PostProcess::bloom, "Blurred highlight strength before exposure/tone mapping. 0 disables bloom.");
        bloomStrength.hasRange = true; bloomStrength.min = 0; bloomStrength.max = 4;
        FieldInfo& threshold = f.Add("bloomThreshold", &PostProcess::bloomThreshold, "Highlight threshold in unexposed scene RGB (largest channel); extraction preserves hue.");
        threshold.hasRange = true; threshold.min = 0; threshold.max = 32;
        FieldInfo& bloomRadiusField = f.Add("bloomRadius", &PostProcess::bloomRadius, "Separable tent filter radius in scene pixels, 1..32. Full-resolution bloom is optional.");
        bloomRadiusField.hasRange = true; bloomRadiusField.min = 1; bloomRadiusField.max = 32;
        f.Add("fxaa", &PostProcess::fxaa, "Optional directional FXAA on display scene color before UI and selection outlines; disabled by default.");
        FieldInfo& strength = f.Add("vignette", &PostProcess::vignette, "Edge darkening strength: 0 disables, 1 is fully dark outside the transition.");
        strength.hasRange = true; strength.min = 0; strength.max = 1;
        FieldInfo& radius = f.Add("vignetteRadius", &PostProcess::vignetteRadius, "Normalized radius: center 0, edge midpoint 1, corner sqrt(2).");
        radius.hasRange = true; radius.min = 0; radius.max = 1.5f;
        FieldInfo& softness = f.Add("vignetteSoftness", &PostProcess::vignetteSoftness, "Smooth transition width in normalized screen coordinates (minimum 0.01).");
        softness.hasRange = true; softness.min = 0.01f; softness.max = 2;
        FieldInfo& dof = f.Add("dofRadius", &PostProcess::dofRadius,
                               "Depth of field: largest blur radius in scene pixels (0..16) for surfaces far from dofFocus. 0 disables it.");
        dof.hasRange = true; dof.min = 0; dof.max = 16;
        FieldInfo& focus = f.Add("dofFocus", &PostProcess::dofFocus, "Depth of field: distance in front of the camera, in meters, that is sharp.");
        focus.hasRange = true; focus.min = 0; focus.max = 100000;
        FieldInfo& range = f.Add("dofRange", &PostProcess::dofRange, "Depth of field: depth on either side of dofFocus that stays fully sharp.");
        range.hasRange = true; range.min = 0; range.max = 100000;
        FieldInfo& falloff = f.Add("dofFalloff", &PostProcess::dofFalloff,
                                   "Depth of field: depth beyond dofRange over which the blur grows to dofRadius (minimum 0.01).");
        falloff.hasRange = true; falloff.min = 0.01f; falloff.max = 100000;
    }
};

struct Light2D {
    static constexpr const char* kTypeName = "Light2D";
    static constexpr const char* kDoc = "2D light: a soft round hole in the active camera's Darkness2D, centred on the entity. Without a Darkness2D it does nothing.";
    float radius = 5.0f;
    float inner = 0.25f;
    float strength = 1.0f;
    bool enabled = true;
    static void Reflect(FieldList& f) {
        f.Add("radius", &Light2D::radius, "World units where the light ends (Transform.scale does not change it).");
        FieldInfo& in = f.Add("inner", &Light2D::inner, "Fraction of the radius that is fully lit before the falloff starts.");
        in.hasRange = true; in.min = 0.0f; in.max = 0.99f;
        FieldInfo& st = f.Add("strength", &Light2D::strength, "How much darkness the centre removes: 1 = all, 0.5 = a dim glimpse.");
        st.hasRange = true; st.min = 0.0f; st.max = 1.0f;
        f.Add("enabled", &Light2D::enabled, "Off = no light, without removing the component.");
    }
};

struct Darkness2D {
    static constexpr const char* kTypeName = "Darkness2D";
    static constexpr const char* kDoc =
        "On an orthographic Camera: covers the 2D view with a colour that Light2D entities cut soft holes into (night, fog of war, caves). "
        "Drawn at `layer`: blended sprites and particles in higher layers stay visible on top of it.";
    Color color{0.02f, 0.02f, 0.06f};
    float opacity = 0.9f;
    int layer = 100;
    int resolution = 144;
    bool enabled = true;
    static void Reflect(FieldList& f) {
        f.Add("color", &Darkness2D::color, "Colour of the unlit area.");
        FieldInfo& op = f.Add("opacity", &Darkness2D::opacity, "How opaque the unlit area is (1 hides it completely).");
        op.hasRange = true; op.min = 0.0f; op.max = 1.0f;
        f.Add("layer", &Darkness2D::layer,
              "2D sorting layer of the overlay (see Sprite.layer). Blended sprites (alphaCutoff 0) and particles above it are not darkened; "
              "cut-out sprites are always under it.");
        FieldInfo& res = f.Add("resolution", &Darkness2D::resolution, "Light map rows across the view height; edges are smoothed when it is stretched.");
        res.hasRange = true; res.min = 16; res.max = 512;
        f.Add("enabled", &Darkness2D::enabled, "Off = no darkness.");
    }
};

struct DirectionalLight {
    static constexpr const char* kTypeName = "DirectionalLight";
    static constexpr const char* kDoc = "Sun-like light. Direction comes from the entity's rotation (local -Z).";
    Color color{1.0f, 0.97f, 0.9f};
    float intensity = 1.0f;
    Color ambient{0.18f, 0.2f, 0.25f};
    bool shadows = true;
    float shadowStrength = 0.75f;
    static void Reflect(FieldList& f) {
        f.Add("color", &DirectionalLight::color, "Light color.");
        f.Add("intensity", &DirectionalLight::intensity, "Brightness multiplier.");
        f.Add("ambient", &DirectionalLight::ambient, "Ambient light added to every surface.");
        f.Add("shadows", &DirectionalLight::shadows, "Cast shadows (first directional light only).");
        FieldInfo& s = f.Add("shadowStrength", &DirectionalLight::shadowStrength, "0 = no darkening, 1 = black shadows.");
        s.hasRange = true; s.min = 0.0f; s.max = 1.0f;
    }
};

struct PointLight {
    static constexpr const char* kTypeName = "PointLight";
    static constexpr const char* kDoc = "Light bulb at the entity position, fading to zero at `range`.";
    Color color{1.0f, 0.85f, 0.6f};
    float intensity = 1.5f;
    float range = 6.0f;
    static void Reflect(FieldList& f) {
        f.Add("color", &PointLight::color, "Light color.");
        f.Add("intensity", &PointLight::intensity, "Brightness at the center.");
        f.Add("range", &PointLight::range, "Distance in meters where the light reaches zero.");
    }
};

struct CameraFollow {
    static constexpr const char* kTypeName = "CameraFollow";
    static constexpr const char* kDoc = "Moves this entity (usually the camera) to target + offset after physics each frame and aims it at the target.";
    EntityId target = kNullEntity;
    Vec3 offset{0, 4, 8};
    Vec3 lookOffset{0, 0.5f, 0};
    float smoothing = 8.0f;
    bool useBounds = false;
    Vec3 boundsMin{-1000, -1000, -1000};
    Vec3 boundsMax{1000, 1000, 1000};
    static void Reflect(FieldList& f) {
        f.Add("target", &CameraFollow::target, "Entity id to follow.");
        f.Add("offset", &CameraFollow::offset, "Position relative to the target (world axes).");
        f.Add("lookOffset", &CameraFollow::lookOffset, "Point to look at, relative to the target.");
        f.Add("smoothing", &CameraFollow::smoothing, "Catch-up speed (per second); 0 = snap instantly.");
        f.Add("useBounds", &CameraFollow::useBounds, "Clamp the camera position to boundsMin..boundsMax and keep its rotation (2D side-scrollers: the view stops at the level edges).");
        f.Add("boundsMin", &CameraFollow::boundsMin, "Lowest camera position when useBounds is on.");
        f.Add("boundsMax", &CameraFollow::boundsMax, "Highest camera position when useBounds is on.");
    }
};

struct Tag {
    static constexpr const char* kTypeName = "Tag";
    static constexpr const char* kDoc = "Free-form labels for gameplay queries (e.g. \"enemy\", \"pickup\").";
    std::string tags;
    static void Reflect(FieldList& f) { f.Add("tags", &Tag::tags, "Comma separated tags."); }
};

struct Script {
    static constexpr const char* kTypeName = "Script";
    static constexpr const char* kDoc = "Runs a Lua script while simulating. The file returns a table with optional onStart(self), onUpdate(self, dt) and onDestroy(self). See docs/SCRIPTING.md.";
    std::string path;
    Json params = Json::MakeObject();
    bool enabled = true;
    static void Reflect(FieldList& f) {
        f.Add("path", &Script::path, "Lua file relative to the project, e.g. \"scripts/rotator.lua\".");
        f.Add("params", &Script::params, "JSON object passed to the script as self.params.");
        f.Add("enabled", &Script::enabled, "Disabled scripts are not run.");
    }
};

// ----- Physics (simulated by Jolt, see engine/physics) ----------------------

struct Collider {
    static constexpr const char* kTypeName = "Collider";
    static constexpr const char* kDoc = "Collision shape. Alone it is static (walls, floors); add RigidBody to make it move. isTrigger makes it a non-solid volume that reports onTriggerEnter/onTriggerExit.";
    std::string shape = "box";
    Vec3 size{1, 1, 1};
    float radius = 0.5f;
    float height = 2.0f;
    Vec3 center{0, 0, 0};
    bool isTrigger = false;
    float friction = 0.5f;
    float bounciness = 0.0f;
    static void Reflect(FieldList& f) {
        f.Add("shape", &Collider::shape, "Shape type.").options = {"box", "sphere", "capsule"};
        f.Add("size", &Collider::size, "Box: full size in local units (multiplied by Transform.scale). Matches the 1x1x1 cube mesh by default.");
        f.Add("radius", &Collider::radius, "Sphere/capsule radius in local units.");
        f.Add("height", &Collider::height, "Capsule: total height along local Y, including the rounded ends.");
        f.Add("center", &Collider::center, "Offset of the shape from the entity origin (local units).");
        f.Add("isTrigger", &Collider::isTrigger, "Non-solid volume: reports overlaps instead of colliding.");
        FieldInfo& fr = f.Add("friction", &Collider::friction, "Surface friction (0 = ice, 1 = rubber).");
        fr.hasRange = true; fr.min = 0.0f; fr.max = 2.0f;
        FieldInfo& b = f.Add("bounciness", &Collider::bounciness, "Restitution (0 = no bounce, 1 = perfectly elastic).");
        b.hasRange = true; b.min = 0.0f; b.max = 1.0f;
    }
};

struct RigidBody {
    static constexpr const char* kTypeName = "RigidBody";
    static constexpr const char* kDoc = "Makes a Collider move. dynamic = driven by gravity and collisions; kinematic = follows its Transform (moving platforms, doors) and pushes dynamic bodies.";
    std::string type = "dynamic";
    float mass = 1.0f;
    Vec3 velocity{0, 0, 0};
    Vec3 angularVelocity{0, 0, 0};
    float gravityScale = 1.0f;
    float linearDamping = 0.05f;
    bool lockRotation = false;
    bool continuous = false;
    bool plane2D = false;
    static void Reflect(FieldList& f) {
        f.Add("type", &RigidBody::type, "Body type.").options = {"dynamic", "kinematic"};
        FieldInfo& m = f.Add("mass", &RigidBody::mass, "Mass in kg (dynamic bodies).");
        m.hasRange = true; m.min = 0.001f; m.max = 1.0e6f;
        f.Add("velocity", &RigidBody::velocity, "Linear velocity in m/s. Written by the simulation every step; set it to launch the body.");
        f.Add("angularVelocity", &RigidBody::angularVelocity, "Angular velocity in degrees/s (world axes). Written by the simulation.");
        f.Add("gravityScale", &RigidBody::gravityScale, "Multiplier for gravity (0 = floats).");
        f.Add("linearDamping", &RigidBody::linearDamping, "Air resistance.");
        f.Add("lockRotation", &RigidBody::lockRotation, "Prevent the body from rotating (keeps it upright).");
        f.Add("continuous", &RigidBody::continuous, "Continuous collision detection for fast objects (prevents tunneling through thin walls).");
        f.Add("plane2D", &RigidBody::plane2D, "2D physics: move only in the XY plane and rotate only around Z.");
    }
};

struct CharacterBody {
    static constexpr const char* kTypeName = "CharacterBody";
    static constexpr const char* kDoc = "Physics character for players/NPCs: set velocity (x/z to walk, y to jump) and the engine moves it, sliding along walls, climbing steps and slopes, applying gravity. PlayerController uses it automatically when present.";
    std::string shape = "capsule";
    float radius = 0.4f;
    float height = 1.8f;
    float maxSlope = 50.0f;
    float stepHeight = 0.3f;
    float gravityScale = 1.0f;
    Vec3 velocity{0, 0, 0};
    bool grounded = false;
    bool plane2D = false;
    static void Reflect(FieldList& f) {
        f.Add("shape", &CharacterBody::shape, "Shape centered on the entity origin.").options = {"capsule", "sphere"};
        f.Add("radius", &CharacterBody::radius, "Radius in meters.");
        f.Add("height", &CharacterBody::height, "Capsule total height in meters.");
        f.Add("maxSlope", &CharacterBody::maxSlope, "Steepest walkable slope in degrees.");
        f.Add("stepHeight", &CharacterBody::stepHeight, "Highest step the character walks up automatically.");
        f.Add("gravityScale", &CharacterBody::gravityScale, "Multiplier for gravity.");
        f.Add("velocity", &CharacterBody::velocity, "Desired velocity in m/s; after each step it holds the actual velocity.");
        f.Add("grounded", &CharacterBody::grounded, "Runtime state: true while standing on walkable ground.");
        f.Add("plane2D", &CharacterBody::plane2D, "2D games: keep the character on its starting Z (velocity.z is ignored).");
    }
};

// ----- 2D physics (simulated by Box2D, see engine/physics/Physics2D.cpp) ---------
// Bodies live in the XY plane of their entity: position x/y, rotation around Z,
// Transform.scale x/y scales the shape. 2D and 3D bodies do not collide with
// each other; Tilemaps collide with both.

struct Collider2D {
    static constexpr const char* kTypeName = "Collider2D";
    static constexpr const char* kDoc = "2D collision shape in the XY plane (Box2D). Alone it is static (walls, ground); add RigidBody2D to make it move. Shapes: box (optionally rounded), circle, capsule, polygon (any simple outline, concave is split), edge (line strip; loop closes it). isTrigger makes a sensor volume; oneWay makes a platform you can pass from below.";
    std::string shape = "box";
    Vec3 size{1, 1, 0};
    float radius = 0.5f;
    float height = 1.0f;
    float rounding = 0.0f;
    Vec3 center{0, 0, 0};
    float angle = 0.0f;
    Json points = Json::MakeArray();
    bool loop = false;
    bool isTrigger = false;
    bool oneWay = false;
    float friction = 0.6f;
    float bounciness = 0.0f;
    float density = 1.0f;
    int layer = 0;
    Json ignoreLayers = Json::MakeArray();
    static void Reflect(FieldList& f) {
        f.Add("shape", &Collider2D::shape, "Shape type.").options = {"box", "circle", "capsule", "polygon", "edge"};
        f.Add("size", &Collider2D::size, "Box: width and height in local units (x, y; z is ignored), multiplied by Transform.scale.");
        f.Add("radius", &Collider2D::radius, "Circle/capsule radius in local units.");
        f.Add("height", &Collider2D::height, "Capsule: total height along local Y, including the rounded ends.");
        f.Add("rounding", &Collider2D::rounding, "Box: corner radius (0 = sharp corners). Rounded boxes slide over bumps more smoothly.");
        f.Add("center", &Collider2D::center, "Offset of the shape from the entity origin (x, y).");
        f.Add("angle", &Collider2D::angle, "Box/capsule rotation around the shape center in degrees (added to the entity rotation).");
        f.Add("points", &Collider2D::points, "Polygon/edge points [[x, y], ...] in local units. Polygon: outline in any winding (concave is triangulated). Edge: a line strip, two-sided.");
        f.Add("loop", &Collider2D::loop, "Edge: close the strip into a loop (smooth one-sided outline, solid inside: good for terrain).");
        f.Add("isTrigger", &Collider2D::isTrigger, "Sensor: reports onTriggerEnter/onTriggerExit for moving bodies and characters instead of colliding.");
        f.Add("oneWay", &Collider2D::oneWay, "One-way platform: solid only from above (bodies and characters pass through from below and the sides).");
        FieldInfo& fr = f.Add("friction", &Collider2D::friction, "Surface friction (0 = ice, 1 = rubber).");
        fr.hasRange = true; fr.min = 0.0f; fr.max = 2.0f;
        FieldInfo& b = f.Add("bounciness", &Collider2D::bounciness, "Restitution (0 = no bounce, 1 = perfectly elastic).");
        b.hasRange = true; b.min = 0.0f; b.max = 1.0f;
        FieldInfo& d = f.Add("density", &Collider2D::density, "Relative density when a RigidBody2D has several shapes (the body's mass is RigidBody2D.mass).");
        d.hasRange = true; d.min = 0.0f; d.max = 1000.0f;
        FieldInfo& l = f.Add("layer", &Collider2D::layer, "Collision layer 0-15.");
        l.hasRange = true; l.min = 0; l.max = 15;
        f.Add("ignoreLayers", &Collider2D::ignoreLayers, "Layers this shape does not collide with, e.g. [2, 3]. Two shapes collide unless either ignores the other's layer.");
    }
};

struct RigidBody2D {
    static constexpr const char* kTypeName = "RigidBody2D";
    static constexpr const char* kDoc = "Makes a Collider2D move in the XY plane (Box2D). dynamic = gravity and collisions; kinematic = follows its Transform (moving platforms, doors) and pushes dynamic bodies. For top-down games set gravityScale 0 and some linearDamping.";
    std::string type = "dynamic";
    float mass = 1.0f;
    Vec3 velocity{0, 0, 0};
    float angularVelocity = 0.0f;
    float gravityScale = 1.0f;
    float linearDamping = 0.0f;
    float angularDamping = 0.05f;
    bool fixedRotation = false;
    bool bullet = false;
    static void Reflect(FieldList& f) {
        f.Add("type", &RigidBody2D::type, "Body type.").options = {"dynamic", "kinematic"};
        FieldInfo& m = f.Add("mass", &RigidBody2D::mass, "Mass in kg (dynamic bodies).");
        m.hasRange = true; m.min = 0.001f; m.max = 1.0e6f;
        f.Add("velocity", &RigidBody2D::velocity, "Linear velocity in m/s (x, y). Written by the simulation every step; set it to launch the body.");
        f.Add("angularVelocity", &RigidBody2D::angularVelocity, "Spin in degrees/s (counter-clockwise). Written by the simulation.");
        f.Add("gravityScale", &RigidBody2D::gravityScale, "Multiplier for gravity (0 = floats; top-down games).");
        f.Add("linearDamping", &RigidBody2D::linearDamping, "Slows the body down over time (top-down friction).");
        f.Add("angularDamping", &RigidBody2D::angularDamping, "Slows spinning down over time.");
        f.Add("fixedRotation", &RigidBody2D::fixedRotation, "Never rotate (keeps it upright).");
        f.Add("bullet", &RigidBody2D::bullet, "Continuous collision against other moving bodies too (fast projectiles). Static geometry is always continuous.");
    }
};

struct CharacterBody2D {
    static constexpr const char* kTypeName = "CharacterBody2D";
    static constexpr const char* kDoc = "2D character for players/NPCs (Box2D mover): set velocity and the engine moves it, sliding along walls and slopes, standing on moving platforms, passing one-way platforms from below and pushing dynamic bodies. platformer mode applies gravity and sets grounded; topdown mode has no gravity.";
    std::string mode = "platformer";
    std::string shape = "capsule";
    float radius = 0.4f;
    float height = 1.0f;
    Vec3 velocity{0, 0, 0};
    float gravityScale = 1.0f;
    float maxSlope = 50.0f;
    float pushStrength = 1.0f;
    bool grounded = false;
    bool onWall = false;
    bool onCeiling = false;
    bool dropThrough = false;
    int layer = 0;
    Json ignoreLayers = Json::MakeArray();
    static void Reflect(FieldList& f) {
        f.Add("mode", &CharacterBody2D::mode, "platformer: gravity pulls down -Y, grounded is tracked; topdown: no gravity, velocity x/y moves freely.").options = {"platformer", "topdown"};
        f.Add("shape", &CharacterBody2D::shape, "Shape centered on the entity origin.").options = {"capsule", "circle"};
        f.Add("radius", &CharacterBody2D::radius, "Radius in meters.");
        f.Add("height", &CharacterBody2D::height, "Capsule total height in meters (vertical).");
        f.Add("velocity", &CharacterBody2D::velocity, "Desired velocity in m/s (x, y); after each step it holds the actual velocity (stopped by walls, ceilings, ground).");
        f.Add("gravityScale", &CharacterBody2D::gravityScale, "Multiplier for gravity (platformer mode). 2-3 gives a snappy jump arc.");
        f.Add("maxSlope", &CharacterBody2D::maxSlope, "Steepest walkable slope in degrees (steeper surfaces are walls).");
        f.Add("pushStrength", &CharacterBody2D::pushStrength, "How hard the character pushes dynamic bodies it walks into (0 = not at all).");
        f.Add("grounded", &CharacterBody2D::grounded, "Runtime state: standing on walkable ground (platformer mode).");
        f.Add("onWall", &CharacterBody2D::onWall, "Runtime state: touching a wall (steeper than maxSlope) this step.");
        f.Add("onCeiling", &CharacterBody2D::onCeiling, "Runtime state: hit a ceiling this step.");
        f.Add("dropThrough", &CharacterBody2D::dropThrough, "Set true to fall through one-way platforms (e.g. Down + Jump); reset it when the character is below.");
        FieldInfo& l = f.Add("layer", &CharacterBody2D::layer, "Collision layer 0-15.");
        l.hasRange = true; l.min = 0; l.max = 15;
        f.Add("ignoreLayers", &CharacterBody2D::ignoreLayers, "Layers the character does not collide with.");
    }
};

// ----- Prefabs ------------------------------------------------------------------

struct Prefab {
    static constexpr const char* kTypeName = "Prefab";
    static constexpr const char* kDoc = "Marks the root of a prefab instance and remembers which prefab file it came from.";
    std::string path;
    static void Reflect(FieldList& f) { f.Add("path", &Prefab::path, "Prefab file, e.g. \"prefabs/coin.prefab.json\"."); }
};

// ----- In-game UI ----------------------------------------------------------------
// Screen-space UI laid out on a reference canvas (1280x720 unless a UICanvas
// says otherwise) scaled to the screen. Every element has the same placement
// fields: `anchor` picks the point of the parent rectangle the element hangs
// from and its pivot (or a stretch mode), x/y offset it in reference pixels
// (+x right, +y down), width/height size it. The parent rectangle is the
// nearest ancestor entity with a UI element, else the screen, so panels can
// hold buttons and text. Siblings draw in `order`, then id; children draw on
// top of their parent. A UILayout on the parent arranges children instead
// (in hierarchy order). A UIPanel with opacity 0 is an invisible container.

inline const std::vector<std::string>& UIAnchors() {
    static const std::vector<std::string> a = {"top-left", "top", "top-right", "left", "center", "right", "bottom-left", "bottom", "bottom-right",
                                               "stretch-top", "stretch-middle", "stretch-bottom", "stretch-left", "stretch-center", "stretch-right", "stretch"};
    return a;
}

constexpr const char* kUIAnchorDoc =
    "Where the element hangs in its parent (the screen or the parent UI element) and its pivot. stretch-* modes stretch along an axis: "
    "there width/height is added to the parent's size (-40 leaves 20 px on each side).";

// Placement fields shared by all UI components.
template <class T>
void ReflectUIPlacement(FieldList& f, const char* sizeDoc) {
    f.Add("anchor", &T::anchor, kUIAnchorDoc).options = UIAnchors();
    f.Add("x", &T::x, "Horizontal offset in reference pixels (canvas is 1280x720 by default).");
    f.Add("y", &T::y, "Vertical offset in reference pixels (+y is down).");
    f.Add("width", &T::width, sizeDoc);
    f.Add("height", &T::height, sizeDoc);
}

template <class T>
void ReflectUICommon(FieldList& f) {
    FieldInfo& o = f.Add("opacity", &T::opacity, "0 = invisible, 1 = opaque (this element only; children keep theirs).");
    o.hasRange = true;
    o.min = 0.0f;
    o.max = 1.0f;
    f.Add("visible", &T::visible, "Hidden elements (and their children) are not drawn and cannot be clicked.");
    f.Add("order", &T::order, "Draw order among siblings (higher on top).");
}

// Image look shared by UI elements (Unity's sliced Image, Unreal's box brush, Godot's StyleBoxTexture): a texture
// replaces the solid background and every other image of the element uses the same slicing and sampling.
template <class T>
void ReflectUISkin(FieldList& f) {
    f.Add("texture", &T::texture, "Background image (png/jpg/bmp/tga); the color then tints it (white = unchanged). Empty = solid color.");
    f.Add("slice", &T::slice, "9-slice border of the element's images in image pixels: corners keep their size, edges and center stretch (0 = plain stretch).");
    f.Add("sliceScale", &T::sliceScale, "Size of the 9-slice border on screen, in reference pixels per image pixel.");
    f.Add("pixelArt", &T::pixelArt, "Nearest-neighbour sampling of the element's images (sharp pixels).");
}

constexpr const char* kUIFontDoc =
    "\"default\" (built-in Roboto), \"pixel\" (built-in 5x7 pixel font, ASCII) or a font file in the project (assets/fonts/x.ttf, .otf, .ttc). "
    "Characters a font lacks fall back to the default font.";

struct UIText {
    static constexpr const char* kTypeName = "UIText";
    static constexpr const char* kDoc = "Screen-space text: TrueType fonts (any language the font covers), alignment, wrapping, outline, shadow, rich text.";
    std::string text = "Text";
    std::string font = "default";
    float size = 32.0f;
    Color color{1, 1, 1};
    std::string anchor = "top-left";
    float x = 24.0f;
    float y = 24.0f;
    float width = 0.0f;
    float height = 0.0f;
    std::string align = "auto";
    std::string verticalAlign = "top";
    bool wrap = true;
    float lineSpacing = 1.0f;
    float letterSpacing = 0.0f;
    bool bold = false;
    bool richText = true;
    float outlineWidth = 0.0f;
    Color outlineColor{0, 0, 0};
    float shadowDistance = 0.0f;
    Color shadowColor{0, 0, 0};
    int visibleCharacters = -1;
    float minSize = 0.0f;
    float opacity = 1.0f;
    bool visible = true;
    int order = 0;
    static void Reflect(FieldList& f) {
        f.Add("text", &UIText::text, "Text to show (UTF-8). \\n starts a new line. With richText: <color=#ff8800>..</color>, <b>..</b>.");
        f.Add("font", &UIText::font, kUIFontDoc);
        f.Add("size", &UIText::size, "Font size in reference pixels (em height; for \"pixel\" the line height).");
        f.Add("color", &UIText::color, "Text color.");
        ReflectUIPlacement<UIText>(f, "Box size in reference pixels; 0 = fit the text. A width makes text wrap and align inside it.");
        f.Add("align", &UIText::align, "Horizontal alignment of lines; auto follows the anchor (left/center/right).").options = {"auto", "left", "center", "right"};
        f.Add("verticalAlign", &UIText::verticalAlign, "Vertical alignment inside a box with a height.").options = {"top", "middle", "bottom"};
        f.Add("wrap", &UIText::wrap, "Break lines at word boundaries to fit width (when width > 0).");
        f.Add("lineSpacing", &UIText::lineSpacing, "Line height multiplier.");
        f.Add("letterSpacing", &UIText::letterSpacing, "Extra space between characters in reference pixels.");
        f.Add("bold", &UIText::bold, "Synthetic bold.");
        f.Add("richText", &UIText::richText, "Interpret <color=..> and <b> tags.");
        f.Add("outlineWidth", &UIText::outlineWidth, "Outline thickness in reference pixels (0 = none).");
        f.Add("outlineColor", &UIText::outlineColor, "Outline color.");
        f.Add("shadowDistance", &UIText::shadowDistance, "Drop shadow offset (right and down) in reference pixels (0 = none).");
        f.Add("shadowColor", &UIText::shadowColor, "Drop shadow color (drawn at half the text opacity).");
        f.Add("minSize", &UIText::minSize, "Shrink the font down to this size until the text fits its width/height box (0 = never shrink).");
        f.Add("visibleCharacters", &UIText::visibleCharacters, "Show only the first N characters (typewriter effects); -1 = all.");
        ReflectUICommon<UIText>(f);
    }
};

struct UIPanel {
    static constexpr const char* kTypeName = "UIPanel";
    static constexpr const char* kDoc = "Screen-space rectangle (backgrounds, windows, bars): a solid color or a 9-slice image, rounded corners, a border. Parent of other UI elements.";
    std::string anchor = "top-left";
    float x = 16.0f;
    float y = 16.0f;
    float width = 240.0f;
    float height = 64.0f;
    Color color{0, 0, 0};
    std::string texture;
    float slice = 0.0f;
    float sliceScale = 1.0f;
    bool pixelArt = false;
    float opacity = 0.5f;
    float radius = 0.0f;
    float borderWidth = 0.0f;
    Color borderColor{1, 1, 1};
    bool clip = false;
    bool blockInput = false;
    bool visible = true;
    int order = -1;
    static void Reflect(FieldList& f) {
        ReflectUIPlacement<UIPanel>(f, "Size in reference pixels.");
        f.Add("color", &UIPanel::color, "Fill color.");
        ReflectUISkin<UIPanel>(f);
        f.Add("radius", &UIPanel::radius, "Corner radius in reference pixels.");
        f.Add("borderWidth", &UIPanel::borderWidth, "Border thickness in reference pixels (0 = none).");
        f.Add("borderColor", &UIPanel::borderColor, "Border color.");
        f.Add("blockInput", &UIPanel::blockInput,
              "Modal backdrop: buttons and sliders drawn under this panel cannot be clicked or hovered; elements drawn over it (its children) can.");
        f.Add("clip", &UIPanel::clip, "Children are cut off at the panel's edges (and cannot be clicked outside it).");
        ReflectUICommon<UIPanel>(f);
    }
};

struct UIButton {
    static constexpr const char* kTypeName = "UIButton";
    static constexpr const char* kDoc =
        "Clickable screen-space button: a solid or textured (9-slice) background with a look per state (normal, hover, pressed, disabled, on), "
        "a label and an optional icon. A click calls onClick(self) on the entity's Script; the pointer entering/leaving calls "
        "onPointerEnter/onPointerExit; a toggle button calls onToggle(self, on). Click it from tools with input.click (ui.layout lists where it is).";
    std::string text = "Button";
    std::string font = "default";
    float size = 28.0f;
    Color textColor{1, 1, 1};
    std::string align = "center";
    float padding = 8.0f;
    std::string icon;
    float iconSize = 0.0f;
    std::string iconSide = "left";
    std::string anchor = "center";
    float x = 0.0f;
    float y = 0.0f;
    float width = 240.0f;
    float height = 64.0f;
    Color color{0.25f, 0.45f, 0.9f};
    std::string texture;
    float slice = 0.0f;
    float sliceScale = 1.0f;
    bool pixelArt = false;
    float radius = 8.0f;
    float borderWidth = 0.0f;
    Color borderColor{1, 1, 1};
    float hoverBrightness = 1.15f;
    float pressedBrightness = 0.85f;
    std::string hoverTexture;
    std::string pressedTexture;
    std::string disabledTexture;
    float hoverScale = 1.0f;
    float pressedScale = 1.0f;
    bool interactable = true;
    std::string clickOn = "press";
    bool toggle = false;
    bool on = false;
    std::string group;
    Color onColor{0.2f, 0.7f, 0.4f};
    std::string onTexture;
    std::string key;
    float opacity = 1.0f;
    bool visible = true;
    int order = 10;
    // Runtime state set by the simulation (not saved).
    bool hovered = false;
    bool pressed = false;
    bool focused = false;   // has the keyboard/gamepad focus (UICanvas.navigation)
    float scaleNow = 1.0f;  // eases toward hoverScale / pressedScale
    static void Reflect(FieldList& f) {
        f.Add("text", &UIButton::text, "Label (same rich text as UIText). Empty = no label (icon buttons).");
        f.Add("font", &UIButton::font, kUIFontDoc);
        f.Add("size", &UIButton::size, "Label font size in reference pixels.");
        f.Add("textColor", &UIButton::textColor, "Label color.");
        f.Add("align", &UIButton::align, "Where the label (and icon) sits across the button.").options = {"left", "center", "right"};
        f.Add("padding", &UIButton::padding, "Space kept free at the left and right edge in reference pixels.");
        f.Add("icon", &UIButton::icon, "Image drawn next to the label (png/jpg/bmp/tga). Empty = none.");
        f.Add("iconSize", &UIButton::iconSize, "Icon height in reference pixels, the width follows the image; 0 = the image's pixel size.");
        f.Add("iconSide", &UIButton::iconSide, "Side of the label the icon is on.").options = {"left", "right", "top", "bottom"};
        ReflectUIPlacement<UIButton>(f, "Size in reference pixels.");
        f.Add("color", &UIButton::color, "Background color.");
        ReflectUISkin<UIButton>(f);
        f.Add("radius", &UIButton::radius, "Corner radius in reference pixels.");
        f.Add("borderWidth", &UIButton::borderWidth, "Border thickness in reference pixels (0 = none).");
        f.Add("borderColor", &UIButton::borderColor, "Border color.");
        f.Add("hoverBrightness", &UIButton::hoverBrightness, "Background brightness while the pointer is over the button.");
        f.Add("pressedBrightness", &UIButton::pressedBrightness, "Background brightness while pressed.");
        f.Add("hoverTexture", &UIButton::hoverTexture, "Background image while the pointer is over the button. Empty = texture.");
        f.Add("pressedTexture", &UIButton::pressedTexture, "Background image while pressed. Empty = texture.");
        f.Add("disabledTexture", &UIButton::disabledTexture, "Background image while not interactable. Empty = texture.");
        f.Add("hoverScale", &UIButton::hoverScale, "Size multiplier while the pointer is over the button (1.05 = grows a little); eased, also while the game is paused.");
        f.Add("pressedScale", &UIButton::pressedScale, "Size multiplier while pressed (0.95 = squeezes).");
        f.Add("interactable", &UIButton::interactable, "Disabled buttons are drawn faded and ignore clicks.");
        f.Add("clickOn", &UIButton::clickOn,
              "When the click happens: press = as the pointer goes down; release = when it comes up while still over the button "
              "(moving off before releasing cancels).").options = {"press", "release"};
        f.Add("toggle", &UIButton::toggle, "A click flips `on` and calls onToggle(self, on) (check boxes, tabs, switches).");
        f.Add("on", &UIButton::on, "Toggle state. Scripts may set it; the group rule is applied on clicks only.");
        f.Add("group", &UIButton::group,
              "Radio group: turning a toggle on turns off every other toggle in the scene with the same group name, and a click cannot "
              "turn the one that is on off. Empty = independent.");
        f.Add("onColor", &UIButton::onColor, "Background color while on (replaces color).");
        f.Add("onTexture", &UIButton::onTexture, "Background image while on. Empty = texture.");
        f.Add("key", &UIButton::key,
              "On-screen control: while the mouse or any finger holds the button, this key is down (input.down / input.pressed, "
              "CharacterBody controls), e.g. \"Left\", \"Space\". Several fingers hold several buttons at once. Empty = none.");
        ReflectUICommon<UIButton>(f);
    }
};

struct UIImage {
    static constexpr const char* kTypeName = "UIImage";
    static constexpr const char* kDoc = "Screen-space image: icons, portraits, 9-slice frames, fill bars (fill + fillOrigin), sprite sheet frames.";
    std::string texture;
    Color color{1, 1, 1};
    std::string anchor = "center";
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    int frame = 0;
    int columns = 1;
    int rows = 1;
    float slice = 0.0f;
    float sliceScale = 1.0f;
    bool preserveAspect = false;
    bool pixelArt = false;
    float fill = 1.0f;
    std::string fillOrigin = "left";
    float radius = 0.0f;
    float opacity = 1.0f;
    bool visible = true;
    int order = 0;
    static void Reflect(FieldList& f) {
        f.Add("texture", &UIImage::texture, "Image file (png/jpg/bmp/tga). Empty = solid color.");
        f.Add("color", &UIImage::color, "Tint (multiplies the image).");
        ReflectUIPlacement<UIImage>(f, "Size in reference pixels; 0 = the image's (frame's) pixel size.");
        f.Add("frame", &UIImage::frame, "Frame of a sprite sheet (row by row from the top-left).");
        f.Add("columns", &UIImage::columns, "Sheet columns.");
        f.Add("rows", &UIImage::rows, "Sheet rows.");
        f.Add("slice", &UIImage::slice, "9-slice border in image pixels: corners keep their size, edges and center stretch (0 = plain stretch).");
        f.Add("sliceScale", &UIImage::sliceScale, "Size of the 9-slice border on screen, in reference pixels per image pixel.");
        f.Add("preserveAspect", &UIImage::preserveAspect, "Fit the image inside the box without distorting it.");
        f.Add("pixelArt", &UIImage::pixelArt, "Nearest-neighbour sampling (sharp pixels).");
        FieldInfo& fl = f.Add("fill", &UIImage::fill, "Visible fraction (health bars, cooldowns).");
        fl.hasRange = true;
        fl.min = 0.0f;
        fl.max = 1.0f;
        f.Add("fillOrigin", &UIImage::fillOrigin, "Side the fill grows from.").options = {"left", "right", "top", "bottom"};
        f.Add("radius", &UIImage::radius, "Corner radius in reference pixels (round avatars, pills).");
        ReflectUICommon<UIImage>(f);
    }
};

struct UISlider {
    static constexpr const char* kTypeName = "UISlider";
    static constexpr const char* kDoc =
        "Bar showing value in [min, max]: a draggable slider (calls onValueChanged(self, value) on the entity's Script) or, with interactable "
        "false, a progress/health bar.";
    float value = 0.5f;
    float min = 0.0f;
    float max = 1.0f;
    float step = 0.0f;
    std::string direction = "left-to-right";
    std::string anchor = "center";
    float x = 0.0f;
    float y = 0.0f;
    float width = 320.0f;
    float height = 24.0f;
    Color color{0.12f, 0.13f, 0.16f};
    Color fillColor{0.25f, 0.45f, 0.9f};
    bool handle = true;
    Color handleColor{1, 1, 1};
    std::string texture;
    float slice = 0.0f;
    float sliceScale = 1.0f;
    bool pixelArt = false;
    std::string fillTexture;
    std::string handleTexture;
    float radius = 12.0f;
    bool interactable = true;
    float opacity = 1.0f;
    bool visible = true;
    int order = 10;
    // Runtime state set by the simulation (not saved).
    bool hovered = false;
    bool pressed = false;
    bool focused = false;  // has the keyboard/gamepad focus (UICanvas.navigation)
    static void Reflect(FieldList& f) {
        f.Add("value", &UISlider::value, "Current value.");
        f.Add("min", &UISlider::min, "Value at the start.");
        f.Add("max", &UISlider::max, "Value at the end.");
        f.Add("step", &UISlider::step, "Snap dragged values to multiples of this (0 = continuous).");
        f.Add("direction", &UISlider::direction, "Fill direction.").options = {"left-to-right", "right-to-left", "bottom-to-top", "top-to-bottom"};
        ReflectUIPlacement<UISlider>(f, "Size in reference pixels.");
        f.Add("color", &UISlider::color, "Track (background) color.");
        f.Add("fillColor", &UISlider::fillColor, "Filled part color.");
        f.Add("handle", &UISlider::handle, "Draw a round handle at the value.");
        f.Add("handleColor", &UISlider::handleColor, "Handle color.");
        ReflectUISkin<UISlider>(f);
        f.Add("fillTexture", &UISlider::fillTexture, "Image of the filled part (tinted by fillColor), laid over the whole bar and cut at the value. Empty = solid color.");
        f.Add("handleTexture", &UISlider::handleTexture, "Image of the handle (tinted by handleColor), drawn at its own proportions. Empty = a round handle.");
        f.Add("radius", &UISlider::radius, "Corner radius in reference pixels.");
        f.Add("interactable", &UISlider::interactable, "Drag to change the value; false = display only (progress bar).");
        ReflectUICommon<UISlider>(f);
    }
};

struct UIInput {
    static constexpr const char* kTypeName = "UIInput";
    static constexpr const char* kDoc =
        "Text field (player names, chat, codes, notes). A click gives it the focus and places the caret; arrows, Home/End, Shift+arrows or a drag "
        "select, Ctrl+A/C/X/V use the clipboard, Enter calls onSubmit(self, text), Escape or a click elsewhere leaves it. Every change calls "
        "onTextChanged(self, text). While it has the focus scripts do not see the keyboard. Tools type with input.text.";
    std::string text;
    std::string placeholder = "Enter text";
    std::string font = "default";
    float size = 24.0f;
    Color textColor{1, 1, 1};
    std::string anchor = "center";
    float x = 0.0f;
    float y = 0.0f;
    float width = 320.0f;
    float height = 48.0f;
    Color color{0.12f, 0.13f, 0.16f};
    std::string texture;
    float slice = 0.0f;
    float sliceScale = 1.0f;
    bool pixelArt = false;
    float radius = 8.0f;
    float borderWidth = 2.0f;
    Color borderColor{0.4f, 0.45f, 0.55f};
    int maxLength = 0;
    bool password = false;
    bool multiline = false;
    bool interactable = true;
    float opacity = 1.0f;
    bool visible = true;
    int order = 10;
    // Runtime state set by the simulation (not saved).
    bool hovered = false;
    bool focused = false;
    float caret = 0.0f;  // seconds since the caret last moved (blink)
    int caretAt = 0;     // byte offset of the caret in `text`
    int selectAt = 0;    // other end of the selection (== caretAt: nothing selected)
    static void Reflect(FieldList& f) {
        f.Add("text", &UIInput::text, "What the player typed (UTF-8). Scripts may set it.");
        f.Add("placeholder", &UIInput::placeholder, "Hint shown faded while the field is empty and not focused.");
        f.Add("font", &UIInput::font, kUIFontDoc);
        f.Add("size", &UIInput::size, "Font size in reference pixels.");
        f.Add("textColor", &UIInput::textColor, "Text and caret color.");
        ReflectUIPlacement<UIInput>(f, "Size in reference pixels.");
        f.Add("color", &UIInput::color, "Background color.");
        ReflectUISkin<UIInput>(f);
        f.Add("radius", &UIInput::radius, "Corner radius in reference pixels.");
        f.Add("borderWidth", &UIInput::borderWidth, "Border thickness in reference pixels (0 = none).");
        f.Add("borderColor", &UIInput::borderColor, "Border color.");
        f.Add("maxLength", &UIInput::maxLength, "Most characters the player can type (0 = no limit).");
        f.Add("password", &UIInput::password, "Show every character as * (and never copy it).");
        f.Add("multiline", &UIInput::multiline, "Several lines: the text wraps at the width, Enter starts a new line and Ctrl+Enter submits.");
        f.Add("interactable", &UIInput::interactable, "Disabled fields are drawn faded and cannot be focused.");
        ReflectUICommon<UIInput>(f);
    }
};

struct UIDropdown {
    static constexpr const char* kTypeName = "UIDropdown";
    static constexpr const char* kDoc =
        "Drop-down list (language, difficulty, resolution): shows the selected option; a click opens the list on top of everything else, a click "
        "on a row selects it and calls onValueChanged(self, index) on the entity's Script, a click elsewhere closes it.";
    Json options = Json::MakeArray();
    int selected = 0;
    std::string font = "default";
    float size = 24.0f;
    Color textColor{1, 1, 1};
    std::string anchor = "center";
    float x = 0.0f;
    float y = 0.0f;
    float width = 240.0f;
    float height = 48.0f;
    Color color{0.18f, 0.2f, 0.26f};
    std::string texture;
    float slice = 0.0f;
    float sliceScale = 1.0f;
    bool pixelArt = false;
    float radius = 8.0f;
    float borderWidth = 0.0f;
    Color borderColor{1, 1, 1};
    bool interactable = true;
    float opacity = 1.0f;
    bool visible = true;
    int order = 10;
    // Runtime state set by the simulation (not saved).
    bool hovered = false;
    bool focused = false;
    bool open = false;
    int hoverItem = -1;  // row under the pointer while open
    int first = 0;       // first row shown when the list is longer than the screen
    static void Reflect(FieldList& f) {
        f.Add("options", &UIDropdown::options, "The choices, e.g. [\"Easy\", \"Normal\", \"Hard\"]. The open list has one row per option, each as tall as the element; a list longer than the screen scrolls with the wheel or a drag.");
        f.Add("selected", &UIDropdown::selected, "Index of the chosen option (0 = the first).");
        f.Add("font", &UIDropdown::font, kUIFontDoc);
        f.Add("size", &UIDropdown::size, "Font size in reference pixels.");
        f.Add("textColor", &UIDropdown::textColor, "Text color.");
        ReflectUIPlacement<UIDropdown>(f, "Size in reference pixels.");
        f.Add("color", &UIDropdown::color, "Background color (the list is a little darker, the row under the pointer lighter).");
        ReflectUISkin<UIDropdown>(f);
        f.Add("radius", &UIDropdown::radius, "Corner radius in reference pixels.");
        f.Add("borderWidth", &UIDropdown::borderWidth, "Border thickness in reference pixels (0 = none).");
        f.Add("borderColor", &UIDropdown::borderColor, "Border color.");
        f.Add("interactable", &UIDropdown::interactable, "Disabled drop-downs are drawn faded and cannot be opened.");
        ReflectUICommon<UIDropdown>(f);
    }
};

struct UIJoystick {
    static constexpr const char* kTypeName = "UIJoystick";
    static constexpr const char* kDoc =
        "On-screen stick for touch screens: while a finger (or the mouse) that went down on it is held, two gamepad axes follow it, so "
        "input.axis(\"LeftX\") works the same with a gamepad and on a phone. Releasing returns the axes to 0.";
    static constexpr int kNoPointer = -1;
    static constexpr int kMousePointer = -2;
    std::string anchor = "bottom-left";
    float x = 40.0f;
    float y = -40.0f;
    float width = 200.0f;
    float height = 200.0f;
    std::string axisX = "LeftX";
    std::string axisY = "LeftY";
    Color color{1, 1, 1};
    Color handleColor{1, 1, 1};
    std::string texture;
    std::string handleTexture;
    bool pixelArt = false;
    float opacity = 0.3f;
    bool visible = true;
    int order = 10;
    // Runtime state set by the simulation (not saved).
    float valueX = 0.0f;      // -1 left .. 1 right
    float valueY = 0.0f;      // -1 down .. 1 up
    int held = kNoPointer;    // touch id holding the stick, or kMousePointer
    static void Reflect(FieldList& f) {
        ReflectUIPlacement<UIJoystick>(f, "Size in reference pixels (the stick is the circle inside this box).");
        f.Add("axisX", &UIJoystick::axisX, "Axis the horizontal position drives (+ = right).").options = {"LeftX", "RightX"};
        f.Add("axisY", &UIJoystick::axisY, "Axis the vertical position drives (+ = up).").options = {"LeftY", "RightY"};
        f.Add("color", &UIJoystick::color, "Base color.");
        f.Add("handleColor", &UIJoystick::handleColor, "Handle color (drawn at twice the base opacity).");
        f.Add("texture", &UIJoystick::texture, "Image of the base (tinted by color). Empty = a disc.");
        f.Add("handleTexture", &UIJoystick::handleTexture, "Image of the handle (tinted by handleColor). Empty = a disc.");
        f.Add("pixelArt", &UIJoystick::pixelArt, "Nearest-neighbour sampling of the images (sharp pixels).");
        ReflectUICommon<UIJoystick>(f);
    }
};

struct UILayout {
    static constexpr const char* kTypeName = "UILayout";
    static constexpr const char* kDoc =
        "Arranges the UI children of this entity's UI element in a column, row or grid (their anchor/x/y are ignored). "
        "fit resizes the element to its content (menus, lists, inventories).";
    std::string direction = "vertical";
    float spacing = 8.0f;
    float padding = 0.0f;
    int columns = 3;
    std::string align = "start";
    std::string crossAlign = "start";
    bool fit = false;
    static void Reflect(FieldList& f) {
        f.Add("direction", &UILayout::direction, "vertical (column), horizontal (row) or grid (rows of `columns` equal cells).").options = {"vertical", "horizontal", "grid"};
        f.Add("spacing", &UILayout::spacing, "Gap between children in reference pixels.");
        f.Add("padding", &UILayout::padding, "Inner margin on every side in reference pixels.");
        f.Add("columns", &UILayout::columns, "Cells per row for grid.");
        f.Add("align", &UILayout::align, "Where the children sit along the layout direction.").options = {"start", "center", "end"};
        f.Add("crossAlign", &UILayout::crossAlign, "Placement across the direction; stretch makes children as wide (tall) as the element.").options = {"start", "center", "end", "stretch"};
        f.Add("fit", &UILayout::fit, "Resize the element to wrap its children.");
    }
};

struct UIScroll {
    static constexpr const char* kTypeName = "UIScroll";
    static constexpr const char* kDoc =
        "Makes a UIPanel a scroll view: its children are clipped to the panel and move with `scroll`. The mouse wheel over the panel, "
        "dragging its empty area (or a finger) and the scroll bar change `scroll`. Combine with UILayout for lists.";
    std::string direction = "vertical";
    float scroll = 0.0f;
    float wheelStep = 60.0f;
    bool bar = true;
    Color barColor{1, 1, 1};
    static void Reflect(FieldList& f) {
        f.Add("direction", &UIScroll::direction, "Axis the content scrolls along.").options = {"vertical", "horizontal"};
        f.Add("scroll", &UIScroll::scroll, "Offset in reference pixels from the start of the content; kept inside the content by the engine while simulating.");
        f.Add("wheelStep", &UIScroll::wheelStep, "Reference pixels per mouse wheel notch.");
        f.Add("bar", &UIScroll::bar, "Draw a scroll bar thumb when the content is larger than the panel.");
        f.Add("barColor", &UIScroll::barColor, "Scroll bar colour.");
    }
};

struct UIMotion {
    static constexpr const char* kTypeName = "UIMotion";
    static constexpr const char* kDoc =
        "Entrance animation of a UI element and its children: plays each time the element becomes visible (screens, dialogs, toasts). "
        "Runs on real frames, so it also plays while the game is paused. Outside a play session the element shows its final state.";
    std::string enter = "fade";
    float duration = 0.25f;
    float delay = 0.0f;
    float distance = 40.0f;
    float time = 1e9f;        // runtime: seconds since the element became visible
    bool wasVisible = false;  // runtime
    static void Reflect(FieldList& f) {
        f.Add("enter", &UIMotion::enter, "fade = opacity; pop = grows from 85% with a small overshoot; slide-* = moves in from `distance` away, all with a fade.")
            .options = {"none", "fade", "pop", "slide-up", "slide-down", "slide-left", "slide-right"};
        f.Add("duration", &UIMotion::duration, "Seconds the animation takes.");
        f.Add("delay", &UIMotion::delay, "Seconds to wait first (stagger a row of cards with increasing delays).");
        f.Add("distance", &UIMotion::distance, "Slide distance in reference pixels.");
        f.Add("time", &UIMotion::time, "Runtime: seconds since the element became visible; set 0 to replay.");
    }
};

struct UICanvas {
    static constexpr const char* kTypeName = "UICanvas";
    static constexpr const char* kDoc = "Optional, one per scene: the reference resolution the UI is authored for and how it scales to other screen sizes.";
    float referenceWidth = 1280.0f;
    float referenceHeight = 720.0f;
    float match = 1.0f;
    bool navigation = false;
    Color focusColor{1, 1, 1};
    static void Reflect(FieldList& f) {
        f.Add("referenceWidth", &UICanvas::referenceWidth, "Reference canvas width in pixels.");
        f.Add("referenceHeight", &UICanvas::referenceHeight, "Reference canvas height in pixels.");
        FieldInfo& m = f.Add("match", &UICanvas::match, "0 = scale with the screen width, 1 = with the height (default), in between = blend.");
        m.hasRange = true;
        m.min = 0.0f;
        m.max = 1.0f;
        f.Add("navigation", &UICanvas::navigation,
              "Keyboard/gamepad control of the UI: arrows or the D-pad move the focus between buttons and sliders (left/right changes a focused "
              "horizontal slider), Enter or GamepadA clicks the focused button. Off = pointer only.");
        f.Add("focusColor", &UICanvas::focusColor, "Color of the frame around the focused element.");
    }
};

// ----- Audio -----------------------------------------------------------------------

struct AudioSource {
    static constexpr const char* kTypeName = "AudioSource";
    static constexpr const char* kDoc = "Plays a WAV clip while simulating (music, ambience). One-shot effects are easier from Lua: audio.play(path).";
    std::string clip;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool playOnStart = true;
    static void Reflect(FieldList& f) {
        f.Add("clip", &AudioSource::clip, "WAV file relative to the project, e.g. \"sounds/music.wav\".");
        FieldInfo& v = f.Add("volume", &AudioSource::volume, "0..1 (up to 2 for boost).");
        v.hasRange = true; v.min = 0.0f; v.max = 2.0f;
        FieldInfo& p = f.Add("pitch", &AudioSource::pitch, "Playback speed (1 = normal).");
        p.hasRange = true; p.min = 0.1f; p.max = 4.0f;
        f.Add("loop", &AudioSource::loop, "Repeat forever.");
        f.Add("playOnStart", &AudioSource::playOnStart, "Start when the entity first appears in a play session.");
    }
};

// ----- Authoritative replication ---------------------------------------------------
struct NetSync {
    static constexpr const char* kTypeName = "NetSync";
    static constexpr const char* kDoc = "Server-owned replication policy. Fields use Component.field keys; clients cannot write authority.";
    int owner = 0;
    Json fields = Json::parse(R"({"Transform.position":{"onChange":true},"Transform.rotation":{"onChange":true}})");
    std::string prefab;
    float distance = 0;
    int team = 0;
    bool teamOnly = false;
    bool predict = true;
    static void Reflect(FieldList& f) {
        f.Add("owner", &NetSync::owner, "Controlling player id; 0 means server.");
        f.Add("fields", &NetSync::fields, "Component.field -> {always,onChange,ownerOnly,quantize}; at most 64 fields.");
        f.Add("prefab", &NetSync::prefab, "Optional project-relative prefab path for reliable spawn.");
        f.Add("distance", &NetSync::distance, "Relevance distance from receiver's NetPlayer (0 = unlimited).");
        f.Add("team", &NetSync::team, "Relevance team.");
        f.Add("teamOnly", &NetSync::teamOnly, "Show only to matching team, or owner.");
        f.Add("predict", &NetSync::predict, "Client predicts its owned entity; others are interpolated.");
    }
};
struct NetPlayer {
    static constexpr const char* kTypeName = "NetPlayer";
    static constexpr const char* kDoc = "Player entity assigned by the authoritative server; network.playerPrefab spawns one per ready participant.";
    int player = 0;
    int team = 0;
    static void Reflect(FieldList& f) {
        f.Add("player", &NetPlayer::player, "Authenticated player id assigned by the server.");
        f.Add("team", &NetPlayer::team, "Player's relevance team.");
    }
};

void RegisterBuiltinComponents();

}  // namespace oe
