#include "physics/Physics2D.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>

#include "core/Log.h"
#include "physics/PhysicsWorld.h"
#include "scene/Components.h"
#include "scene/Scene.h"

#include <box2d/box2d.h>

namespace oe {

namespace {

// ----- Collision filtering ------------------------------------------------------
// 64 category bits = four groups of 16 layers. A shape's category is its layer
// bit inside its group; its mask is the allowed layers repeated in every group.
// Queries pick groups: the character mover sweeps only normal shapes, handles
// one-way shapes itself, and ray/overlap queries skip triggers.
constexpr int kNormalShift = 0, kOneWayShift = 16, kTriggerShift = 32, kCharacterShift = 48;
constexpr uint64_t kNormal = 0xFFFFull << kNormalShift;
constexpr uint64_t kOneWay = 0xFFFFull << kOneWayShift;
constexpr uint64_t kTrigger = 0xFFFFull << kTriggerShift;
constexpr uint64_t kCharacter = 0xFFFFull << kCharacterShift;

uint64_t AllowedLayers(const Json& ignore) {
    uint64_t allowed = 0xFFFF;
    if (ignore.isArray()) {
        for (const Json& l : ignore.items()) {
            int layer = l.asInt(-1);
            if (layer >= 0 && layer < 16) allowed &= ~(1ull << layer);
        }
    }
    return allowed;
}
uint64_t Spread(uint64_t layers) { return layers | layers << 16 | layers << 32 | layers << 48; }
int LayerOf(int layer) { return std::max(0, std::min(15, layer)); }

b2Filter MakeFilter(int shift, int layer, const Json& ignore) {
    b2Filter f = b2DefaultFilter();
    f.categoryBits = 1ull << (shift + LayerOf(layer));
    f.maskBits = Spread(AllowedLayers(ignore));
    return f;
}

// ----- Poses ---------------------------------------------------------------------

b2Vec2 V2(const Vec3& v) { return b2Vec2{v.x, v.y}; }

struct Pose2 {
    b2Vec2 p{0, 0};
    b2Rot q = b2Rot_identity;
    float z = 0;
    float sx = 1, sy = 1;  // signed scale in the plane
};

// World pose of an entity in the plane. Unparented entities with no X/Y
// rotation (the usual 2D case) take position, angle and scale straight from the
// Transform through Box2D's own cos/sin, so bodies start identically on every
// platform (the C library's trigonometry, used for world matrices, may differ in the last bit).
Pose2 PoseOf(const Scene& scene, EntityId id) {
    const Transform* t = scene.Get<Transform>(id);
    const EntityRecord* rec = scene.Record(id);
    Pose2 pose;
    if (t && (!rec || rec->parent == kNullEntity) && t->rotation.x == 0.0f && t->rotation.y == 0.0f) {
        pose.p = b2Vec2{t->position.x, t->position.y};
        pose.z = t->position.z;
        pose.q = t->rotation.z == 0.0f ? b2Rot_identity : b2MakeRot(Radians(t->rotation.z));
        pose.sx = t->scale.x;
        pose.sy = t->scale.y;
        return pose;
    }
    Mat4 m = scene.WorldMatrix(id);
    pose.p = b2Vec2{m.at(0, 3), m.at(1, 3)};
    pose.z = m.at(2, 3);
    Vec3 x(m.at(0, 0), m.at(1, 0), m.at(2, 0)), y(m.at(0, 1), m.at(1, 1), m.at(2, 1));
    pose.sx = Length(x);
    pose.sy = Length(y);
    float len = std::sqrt(x.x * x.x + x.y * x.y);
    if (len > 1e-6f) pose.q = b2Rot{x.x / len, x.y / len};
    if (x.x * y.y - x.y * y.x < 0) pose.sy = -pose.sy;  // mirrored
    return pose;
}

// The Transform values last seen for a body, to tell a script/editor teleport
// from the pose the simulation wrote itself (exact comparison, no round trip).
struct Seen {
    bool valid = false;  // unparented only
    Vec3 p, r, s;
};

Seen SeenOf(const Scene& scene, EntityId id) {
    Seen seen;
    const Transform* t = scene.Get<Transform>(id);
    const EntityRecord* rec = scene.Record(id);
    if (t && (!rec || rec->parent == kNullEntity)) {
        seen.valid = true;
        seen.p = t->position;
        seen.r = t->rotation;
        seen.s = t->scale;
    }
    return seen;
}

bool SameSeen(const Seen& a, const Seen& b) { return a.p == b.p && a.r == b.r && a.s == b.s; }

bool SameP(b2Vec2 a, b2Vec2 b) { return b2LengthSquared(b2Sub(a, b)) < 1e-10f; }
bool SameQ(b2Rot a, b2Rot b) { return std::fabs(a.c - b.c) < 1e-6f && std::fabs(a.s - b.s) < 1e-6f; }

// Writes a world pose (XY + angle) into the entity's local Transform, keeping z and the X/Y rotation.
void WritePose2(Scene& scene, EntityId id, b2Vec2 p, b2Rot q, bool writeRotation) {
    Transform* t = scene.Get<Transform>(id);
    if (!t) return;
    const EntityRecord* rec = scene.Record(id);
    float angle = Degrees(b2Rot_GetAngle(q));
    if (rec && rec->parent != kNullEntity) {
        Mat4 parent = scene.WorldMatrix(rec->parent);
        Vec3 world(p.x, p.y, scene.WorldMatrix(id).at(2, 3));
        Vec3 local = parent.Inverse().TransformPoint(world);
        t->position.x = local.x;
        t->position.y = local.y;
        float parentAngle = Degrees(b2Atan2(parent.at(1, 0), parent.at(0, 0)));
        angle -= parentAngle;
    } else {
        t->position.x = p.x;
        t->position.y = p.y;
    }
    if (writeRotation) t->rotation.z = angle;
}

EntityId EntityOfBody(b2BodyId body) { return static_cast<EntityId>(reinterpret_cast<uintptr_t>(b2Body_GetUserData(body))); }
EntityId EntityOfShape(b2ShapeId shape) { return EntityOfBody(b2Shape_GetBody(shape)); }

// One-way contacts only hold when the other shape is on top: the manifold
// normal (A -> B) must point up away from the platform.
bool PreSolve(b2ShapeId a, b2ShapeId b, b2Manifold* manifold, void*) {
    bool oneWayA = (b2Shape_GetFilter(a).categoryBits & kOneWay) != 0;
    bool oneWayB = (b2Shape_GetFilter(b).categoryBits & kOneWay) != 0;
    if (!oneWayA && !oneWayB) return true;
    float up = oneWayA ? manifold->normal.y : -manifold->normal.y;
    return up > 0.7f;
}

// ----- Geometry --------------------------------------------------------------------

float SignedArea(const std::vector<TilePoint>& p) {
    float a = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        const TilePoint& u = p[i];
        const TilePoint& v = p[(i + 1) % p.size()];
        a += u.x * v.y - v.x * u.y;
    }
    return 0.5f * a;
}

float Cross3(const TilePoint& a, const TilePoint& b, const TilePoint& c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

bool IsConvex(const std::vector<TilePoint>& p) {
    for (size_t i = 0; i < p.size(); ++i) {
        if (Cross3(p[i], p[(i + 1) % p.size()], p[(i + 2) % p.size()]) < -1e-6f) return false;
    }
    return true;
}

bool InTriangle(const TilePoint& p, const TilePoint& a, const TilePoint& b, const TilePoint& c) {
    return Cross3(a, b, p) >= 0 && Cross3(b, c, p) >= 0 && Cross3(c, a, p) >= 0;
}

b2Vec2 B(const TilePoint& p) { return b2Vec2{p.x, p.y}; }

}  // namespace

std::vector<std::vector<TilePoint>> ConvexPieces(std::vector<TilePoint> poly) {
    std::vector<std::vector<TilePoint>> out;
    // Drop repeated points.
    std::vector<TilePoint> clean;
    for (const TilePoint& p : poly) {
        if (clean.empty() || std::fabs(clean.back().x - p.x) > 1e-6f || std::fabs(clean.back().y - p.y) > 1e-6f) clean.push_back(p);
    }
    while (clean.size() > 1 && std::fabs(clean.front().x - clean.back().x) < 1e-6f && std::fabs(clean.front().y - clean.back().y) < 1e-6f) clean.pop_back();
    poly = std::move(clean);
    if (poly.size() < 3 || std::fabs(SignedArea(poly)) < 1e-7f) return out;
    if (SignedArea(poly) < 0) std::reverse(poly.begin(), poly.end());
    if (IsConvex(poly)) {
        // Convex: fan into pieces of at most 8 points (Box2D's polygon limit).
        if (poly.size() <= 8) {
            out.push_back(poly);
            return out;
        }
        for (size_t start = 1; start + 1 < poly.size(); start += 6) {
            std::vector<TilePoint> piece = {poly[0]};
            for (size_t i = start; i < poly.size() && i <= start + 6; ++i) piece.push_back(poly[i]);
            if (piece.size() >= 3) out.push_back(piece);
        }
        return out;
    }
    // Ear clipping into triangles.
    std::vector<std::vector<TilePoint>> tris;
    std::vector<TilePoint> rest = poly;
    int guard = 0;
    while (rest.size() > 3 && guard++ < 10000) {
        bool clipped = false;
        size_t n = rest.size();
        for (size_t i = 0; i < n; ++i) {
            const TilePoint& a = rest[(i + n - 1) % n];
            const TilePoint& b = rest[i];
            const TilePoint& c = rest[(i + 1) % n];
            if (Cross3(a, b, c) <= 1e-7f) continue;  // reflex or collinear
            bool inside = false;
            for (size_t j = 0; j < n && !inside; ++j) {
                if (j == i || j == (i + 1) % n || j == (i + n - 1) % n) continue;
                inside = InTriangle(rest[j], a, b, c);
            }
            if (inside) continue;
            tris.push_back({a, b, c});
            rest.erase(rest.begin() + static_cast<long>(i));
            clipped = true;
            break;
        }
        if (!clipped) break;  // self-intersecting input: keep what we have
    }
    if (rest.size() == 3 && Cross3(rest[0], rest[1], rest[2]) > 1e-7f) tris.push_back(rest);
    // Merge neighbouring pieces while the result stays convex (Hertel-Mehlhorn).
    bool merged = true;
    while (merged) {
        merged = false;
        for (size_t i = 0; i < tris.size() && !merged; ++i) {
            for (size_t j = i + 1; j < tris.size() && !merged; ++j) {
                const auto& A = tris[i];
                const auto& Bp = tris[j];
                for (size_t ai = 0; ai < A.size() && !merged; ++ai) {
                    const TilePoint& a0 = A[ai];
                    const TilePoint& a1 = A[(ai + 1) % A.size()];
                    for (size_t bi = 0; bi < Bp.size() && !merged; ++bi) {
                        const TilePoint& b0 = Bp[bi];
                        const TilePoint& b1 = Bp[(bi + 1) % Bp.size()];
                        if (std::fabs(a0.x - b1.x) > 1e-6f || std::fabs(a0.y - b1.y) > 1e-6f || std::fabs(a1.x - b0.x) > 1e-6f || std::fabs(a1.y - b0.y) > 1e-6f) continue;
                        // Shared edge a0-a1: A without that edge, then B's other points.
                        std::vector<TilePoint> m;
                        for (size_t k = 0; k < A.size(); ++k) m.push_back(A[(ai + 1 + k) % A.size()]);
                        for (size_t k = 2; k < Bp.size(); ++k) m.push_back(Bp[(bi + k) % Bp.size()]);
                        if (m.size() <= 8 && IsConvex(m)) {
                            tris[i] = m;
                            tris.erase(tris.begin() + static_cast<long>(j));
                            merged = true;
                        }
                    }
                }
            }
        }
    }
    return tris;
}

// ----- Impl ------------------------------------------------------------------------------

struct Physics2D::Impl {
    enum class Kind { Static, Dynamic, Kinematic };
    struct BodyRec {
        b2BodyId id = b2_nullBodyId;
        std::string key;
        Kind kind = Kind::Static;
        b2Vec2 lastPos{0, 0};
        b2Rot lastRot = b2Rot_identity;
        Vec3 lastVelocity;
        float lastAngular = 0;
        std::vector<b2ShapeId> triggers;
        bool tilemap = false;
        Seen seen;
    };
    struct CharacterRec {
        b2BodyId body = b2_nullBodyId;
        std::string key;
        b2Vec2 pos{0, 0};
        b2Vec2 c1{0, 0}, c2{0, 0};  // capsule centers relative to pos
        float radius = 0.4f;
        uint64_t category = 0, allowed = 0;
        b2BodyId ground = b2_nullBodyId;
        b2Vec2 groundPoint{0, 0};
        b2Vec2 groundNormal{0, 1};
        std::set<EntityId> touching;
    };

    Impl() {
        b2WorldDef def = b2DefaultWorldDef();
        def.gravity = b2Vec2{0.0f, PhysicsWorld::kGravity};
        def.workerCount = 1;
        world = b2CreateWorld(&def);
        b2World_SetPreSolveCallback(world, PreSolve, nullptr);
    }
    ~Impl() { b2DestroyWorld(world); }

    static void Warn(std::vector<std::string>& warnings, const std::string& msg) {
        if (std::find(warnings.begin(), warnings.end(), msg) != warnings.end()) return;
        warnings.push_back(msg);
        OE_LOG_WARN("physics", "%s", msg.c_str());
    }

    void DestroyBody(BodyRec& rec) {
        if (b2Body_IsValid(rec.id)) b2DestroyBody(rec.id);
        rec.id = b2_nullBodyId;
        rec.triggers.clear();
    }

    // ----- Shapes -----

    static b2ShapeDef ShapeDef(const Collider2D& c) {
        b2ShapeDef def = b2DefaultShapeDef();
        def.material.friction = std::max(0.0f, c.friction);
        def.material.restitution = Clamp(c.bounciness, 0.0f, 1.0f);
        def.density = std::max(0.0f, c.density);
        def.isSensor = c.isTrigger;
        def.enableSensorEvents = false;
        def.enableContactEvents = false;
        def.filter = MakeFilter(c.isTrigger ? kTriggerShift : c.oneWay ? kOneWayShift : kNormalShift, c.layer, c.ignoreLayers);
        def.enablePreSolveEvents = c.oneWay && !c.isTrigger;
        def.updateBodyMass = false;
        return def;
    }

    // Local points of a Collider2D polygon/edge (center applied, scaled).
    static std::vector<TilePoint> ColliderPoints(const Collider2D& c, const Pose2& pose) {
        std::vector<TilePoint> pts;
        if (!c.points.isArray()) return pts;
        for (const Json& p : c.points.items()) {
            if (!p.isArray() || p.size() < 2) continue;
            pts.push_back({(p[0].asFloat() + c.center.x) * pose.sx, (p[1].asFloat() + c.center.y) * pose.sy});
        }
        return pts;
    }

    void AddSegments(b2BodyId body, const b2ShapeDef& def, const std::vector<TilePoint>& pts, bool loop, std::vector<b2ShapeId>* made) {
        size_t n = pts.size();
        for (size_t i = 0; i + 1 < n || (loop && i < n && n > 2); ++i) {
            b2Segment seg{B(pts[i]), B(pts[(i + 1) % n])};
            if (b2LengthSquared(b2Sub(seg.point2, seg.point1)) < 1e-10f) continue;
            b2ShapeId s = b2CreateSegmentShape(body, &def, &seg);
            if (made) made->push_back(s);
        }
    }

    // Returns false (with a warning) when the collider has no usable shape.
    bool AddColliderShapes(b2BodyId body, EntityId id, const Collider2D& c, const Pose2& pose, std::vector<b2ShapeId>& made, std::vector<std::string>& warnings) {
        b2ShapeDef def = ShapeDef(c);
        const float sx = std::fabs(pose.sx), sy = std::fabs(pose.sy);
        const bool mirrored = (pose.sx < 0) != (pose.sy < 0);
        b2Vec2 center{c.center.x * pose.sx, c.center.y * pose.sy};
        b2Rot rot = b2MakeRot(Radians(mirrored ? -c.angle : c.angle));
        const std::string who = "entity " + std::to_string(id) + ": ";
        if (c.shape == "circle") {
            b2Circle circle{center, std::max(0.005f, c.radius * std::max(sx, sy))};
            made.push_back(b2CreateCircleShape(body, &def, &circle));
        } else if (c.shape == "capsule") {
            float r = std::max(0.005f, c.radius * sx);
            float half = 0.5f * c.height * sy - r;
            if (half <= 0.001f) {
                b2Circle circle{center, r};
                made.push_back(b2CreateCircleShape(body, &def, &circle));
            } else {
                b2Vec2 axis = b2RotateVector(rot, b2Vec2{0.0f, half});
                b2Capsule cap{b2Sub(center, axis), b2Add(center, axis), r};
                made.push_back(b2CreateCapsuleShape(body, &def, &cap));
            }
        } else if (c.shape == "polygon") {
            std::vector<TilePoint> pts = ColliderPoints(c, pose);
            std::vector<std::vector<TilePoint>> pieces = ConvexPieces(pts);
            if (pieces.empty()) {
                Warn(warnings, who + "Collider2D polygon needs at least 3 points [[x,y],...] enclosing an area");
                return false;
            }
            for (const auto& piece : pieces) {
                std::vector<b2Vec2> v;
                for (const TilePoint& p : piece) v.push_back(B(p));
                b2Hull hull = b2ComputeHull(v.data(), static_cast<int>(v.size()));
                if (hull.count < 3) continue;
                b2Polygon poly = b2MakePolygon(&hull, 0.0f);
                made.push_back(b2CreatePolygonShape(body, &def, &poly));
            }
        } else if (c.shape == "edge") {
            std::vector<TilePoint> pts = ColliderPoints(c, pose);
            if (pts.size() < 2) {
                Warn(warnings, who + "Collider2D edge needs at least 2 points [[x,y],...]");
                return false;
            }
            if (c.isTrigger) {
                Warn(warnings, who + "an edge cannot be a trigger; use a polygon or box");
                return false;
            }
            if (c.loop && pts.size() >= 4 && !c.oneWay) {
                // Smooth one-sided loop (no snagging on seams); counter-clockwise = solid inside.
                if (SignedArea(pts) < 0) std::reverse(pts.begin(), pts.end());
                std::vector<b2Vec2> v;
                for (const TilePoint& p : pts) v.push_back(B(p));
                b2ChainDef chain = b2DefaultChainDef();
                chain.points = v.data();
                chain.count = static_cast<int>(v.size());
                chain.isLoop = true;
                chain.filter = def.filter;
                b2SurfaceMaterial mat = def.material;
                chain.materials = &mat;
                chain.materialCount = 1;
                b2ChainId chainId = b2CreateChain(body, &chain);
                (void)chainId;
            } else {
                AddSegments(body, def, pts, c.loop, &made);
            }
        } else {
            float hx = std::max(0.005f, 0.5f * std::fabs(c.size.x) * sx), hy = std::max(0.005f, 0.5f * std::fabs(c.size.y) * sy);
            float r = std::min(c.rounding * std::min(sx, sy), 0.99f * std::min(hx, hy));
            b2Polygon box = r > 0.001f ? b2MakeOffsetRoundedBox(hx - r, hy - r, center, rot, r) : b2MakeOffsetBox(hx, hy, center, rot);
            made.push_back(b2CreatePolygonShape(body, &def, &box));
        }
        return true;
    }

    // Static collision of a tilemap: outlines of solid regions as smooth
    // chains, one-way runs as platforms along their top edge, shaped cells as polygons.
    bool AddTilemapShapes(b2BodyId body, const Tilemap& tm, const TileRules& rules, const Pose2& pose) {
        const float ts = std::max(0.001f, tm.tileSize);
        const float kx = ts * pose.sx, ky = ts * pose.sy;
        const bool mirrored = (pose.sx < 0) != (pose.sy < 0);
        bool any = false;
        b2SurfaceMaterial mat = b2DefaultSurfaceMaterial();
        mat.friction = 0.6f;
        b2Filter filter = b2DefaultFilter();
        filter.categoryBits = 1ull << kNormalShift;  // layer 0
        filter.maskBits = Spread(0xFFFF);
        for (auto loop : SolidOutlines(tm, rules)) {
            std::vector<b2Vec2> v;
            for (const TilePoint& p : loop) v.push_back(b2Vec2{p.x * kx, p.y * ky});
            if (mirrored) std::reverse(v.begin(), v.end());
            b2ChainDef chain = b2DefaultChainDef();
            chain.points = v.data();
            chain.count = static_cast<int>(v.size());
            chain.isLoop = true;
            chain.filter = filter;
            chain.materials = &mat;
            chain.materialCount = 1;
            b2CreateChain(body, &chain);
            any = true;
        }
        b2ShapeDef def = b2DefaultShapeDef();
        def.material = mat;
        def.filter = filter;
        def.enableSensorEvents = false;
        def.enableContactEvents = false;
        for (const TileShape& cell : ShapedCells(tm, rules)) {
            std::vector<b2Vec2> v;
            for (const TilePoint& p : cell.points) v.push_back(b2Vec2{p.x * kx, p.y * ky});
            b2Hull hull = b2ComputeHull(v.data(), static_cast<int>(v.size()));
            if (hull.count < 3) continue;
            b2Polygon poly = b2MakePolygon(&hull, 0.0f);
            b2CreatePolygonShape(body, &def, &poly);
            any = true;
        }
        b2ShapeDef oneWay = def;
        oneWay.filter.categoryBits = 1ull << kOneWayShift;
        oneWay.enablePreSolveEvents = true;
        for (const TileRect& run : OneWayRuns(tm, rules)) {
            float y = -static_cast<float>(run.row) * ky;
            b2Segment seg{b2Vec2{static_cast<float>(run.col) * kx, y}, b2Vec2{static_cast<float>(run.col + run.width) * kx, y}};
            b2CreateSegmentShape(body, &oneWay, &seg);
            any = true;
        }
        return any;
    }

    // ----- Sync -----

    static std::string ColliderKey(const Scene& s, EntityId id, const Pose2& pose) {
        std::string key = ComponentToJson(*TypeRegistry::Find("Collider2D"), s.Get<Collider2D>(id)).dump();
        if (const RigidBody2D* rb = s.Get<RigidBody2D>(id)) {
            Json j = ComponentToJson(*TypeRegistry::Find("RigidBody2D"), rb);
            j.erase("velocity");
            j.erase("angularVelocity");
            key += j.dump();
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "|%.4f,%.4f", pose.sx, pose.sy);
        return key + buf;
    }

    static std::string CharacterKey(const CharacterBody2D& c) {
        Json j = ComponentToJson(*TypeRegistry::Find("CharacterBody2D"), &c);
        for (const char* k : {"velocity", "grounded", "onWall", "onCeiling", "dropThrough"}) j.erase(k);
        return j.dump();
    }

    void Sync(Scene& scene, float dt, std::vector<std::string>& warnings, const TilesetLookup* tilesets) {
        // Characters: a kinematic capsule body so other bodies, triggers and
        // rays see them; the mover (UpdateCharacters) drives it.
        std::set<EntityId> seenChars;
        for (auto& kv : scene.Pool<CharacterBody2D>()) {
            EntityId id = kv.first;
            const CharacterBody2D& cb = kv.second;
            seenChars.insert(id);
            Pose2 pose = PoseOf(scene, id);
            std::string key = CharacterKey(cb);
            CharacterRec& rec = characters[id];
            if (!b2Body_IsValid(rec.body) || rec.key != key) {
                if (b2Body_IsValid(rec.body)) b2DestroyBody(rec.body);
                rec = CharacterRec();
                rec.key = key;
                rec.pos = pose.p;
                rec.radius = std::max(0.02f, cb.radius);
                float half = cb.shape == "circle" ? 0.001f : std::max(0.001f, 0.5f * cb.height - rec.radius);
                rec.c1 = b2Vec2{0, -half};
                rec.c2 = b2Vec2{0, half};
                rec.category = 1ull << (kCharacterShift + LayerOf(cb.layer));
                rec.allowed = AllowedLayers(cb.ignoreLayers);
                b2BodyDef bd = b2DefaultBodyDef();
                bd.type = b2_kinematicBody;
                bd.position = pose.p;
                bd.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
                rec.body = b2CreateBody(world, &bd);
                b2ShapeDef sd = b2DefaultShapeDef();
                sd.filter.categoryBits = rec.category;
                sd.filter.maskBits = Spread(rec.allowed);
                sd.enableSensorEvents = false;
                sd.enableContactEvents = false;
                b2Capsule cap{rec.c1, rec.c2, rec.radius};
                b2CreateCapsuleShape(rec.body, &sd, &cap);
                if (scene.Get<Collider2D>(id)) Warn(warnings, "entity " + std::to_string(id) + " has both CharacterBody2D and Collider2D; the Collider2D is ignored (CharacterBody2D defines the shape)");
                if (scene.Get<CharacterBody>(id) || scene.Get<RigidBody2D>(id)) Warn(warnings, "entity " + std::to_string(id) + " has CharacterBody2D with CharacterBody/RigidBody2D; use one body component");
            } else if (!SameP(pose.p, rec.pos)) {
                rec.pos = pose.p;  // teleported by a script or the editor
                b2Body_SetTransform(rec.body, rec.pos, b2Rot_identity);
                rec.ground = b2_nullBodyId;
            }
        }
        for (auto it = characters.begin(); it != characters.end();) {
            if (!seenChars.count(it->first)) {
                if (b2Body_IsValid(it->second.body)) b2DestroyBody(it->second.body);
                it = characters.erase(it);
            } else {
                ++it;
            }
        }

        std::set<EntityId> seenBodies;
        for (auto& kv : scene.Pool<Collider2D>()) {
            EntityId id = kv.first;
            if (seenChars.count(id)) continue;
            seenBodies.insert(id);
            const Collider2D& c = kv.second;
            const RigidBody2D* rb = scene.Get<RigidBody2D>(id);
            Pose2 pose = PoseOf(scene, id);
            std::string key = ColliderKey(scene, id, pose);
            auto it = bodies.find(id);
            if (it != bodies.end() && it->second.key != key) {
                DestroyBody(it->second);
                bodies.erase(it);
                it = bodies.end();
            }
            if (it == bodies.end()) {
                BodyRec rec;
                rec.kind = !rb ? Kind::Static : (rb->type == "kinematic" ? Kind::Kinematic : Kind::Dynamic);
                b2BodyDef bd = b2DefaultBodyDef();
                bd.type = rec.kind == Kind::Static ? b2_staticBody : rec.kind == Kind::Kinematic ? b2_kinematicBody : b2_dynamicBody;
                bd.position = pose.p;
                bd.rotation = pose.q;
                bd.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
                if (rb) {
                    bd.linearVelocity = V2(rb->velocity);
                    bd.angularVelocity = Radians(rb->angularVelocity);
                    bd.gravityScale = rb->gravityScale;
                    bd.linearDamping = std::max(0.0f, rb->linearDamping);
                    bd.angularDamping = std::max(0.0f, rb->angularDamping);
                    bd.fixedRotation = rb->fixedRotation;
                    bd.isBullet = rb->bullet;
                }
                rec.id = b2CreateBody(world, &bd);
                std::vector<b2ShapeId> made;
                if (!AddColliderShapes(rec.id, id, c, pose, made, warnings)) {
                    b2DestroyBody(rec.id);
                    continue;
                }
                if (c.isTrigger) rec.triggers = made;
                if (rec.kind == Kind::Dynamic) {
                    b2Body_ApplyMassFromShapes(rec.id);
                    b2MassData md = b2Body_GetMassData(rec.id);
                    float mass = std::max(0.001f, rb->mass);
                    if (md.mass > 1e-9f) {
                        float k = mass / md.mass;
                        md.mass = mass;
                        md.rotationalInertia *= k;
                    } else {
                        md.mass = mass;
                        md.rotationalInertia = 0.1f * mass;
                    }
                    b2Body_SetMassData(rec.id, md);
                }
                rec.key = key;
                rec.lastPos = pose.p;
                rec.lastRot = pose.q;
                rec.seen = SeenOf(scene, id);
                if (rb) {
                    rec.lastVelocity = rb->velocity;
                    rec.lastAngular = rb->angularVelocity;
                }
                if (scene.Get<RigidBody>(id) || scene.Get<Collider>(id)) Warn(warnings, "entity " + std::to_string(id) + " mixes 2D (Collider2D) and 3D (Collider/RigidBody) physics; they simulate separately");
                if (rb == nullptr && scene.Get<RigidBody>(id)) Warn(warnings, "entity " + std::to_string(id) + ": Collider2D moves with RigidBody2D, not RigidBody");
                bodies[id] = std::move(rec);
                continue;
            }
            BodyRec& rec = it->second;
            Seen seen = SeenOf(scene, id);
            bool moved = (seen.valid && rec.seen.valid) ? !SameSeen(seen, rec.seen) : (!SameP(pose.p, rec.lastPos) || !SameQ(pose.q, rec.lastRot));
            rec.seen = seen;
            if (rec.kind == Kind::Kinematic && dt > 0) {
                // Reach the Transform pose at the end of this step (pushes dynamic bodies, carries characters).
                b2Transform cur = b2Body_GetTransform(rec.id);
                b2Body_SetLinearVelocity(rec.id, b2MulSV(1.0f / dt, b2Sub(pose.p, cur.p)));
                b2Body_SetAngularVelocity(rec.id, b2RelativeAngle(pose.q, cur.q) / dt);
            } else if (moved) {
                b2Body_SetTransform(rec.id, pose.p, pose.q);
                if (rec.kind != Kind::Static) b2Body_SetAwake(rec.id, true);
            }
            rec.lastPos = pose.p;
            rec.lastRot = pose.q;
            if (rec.kind == Kind::Dynamic && rb) {
                if (!(rb->velocity == rec.lastVelocity)) {
                    b2Body_SetLinearVelocity(rec.id, V2(rb->velocity));
                    b2Body_SetAwake(rec.id, true);
                    rec.lastVelocity = rb->velocity;
                }
                if (rb->angularVelocity != rec.lastAngular) {
                    b2Body_SetAngularVelocity(rec.id, Radians(rb->angularVelocity));
                    b2Body_SetAwake(rec.id, true);
                    rec.lastAngular = rb->angularVelocity;
                }
            }
        }

        // Tilemaps: one static body per map.
        for (auto& kv : scene.Pool<Tilemap>()) {
            EntityId id = kv.first;
            if (seenBodies.count(id) || seenChars.count(id)) {
                Warn(warnings, "entity " + std::to_string(id) + " has a Tilemap and a Collider2D/CharacterBody2D; the tilemap does not collide in 2D");
                continue;
            }
            TileRules rules = BuildTileRules(kv.second, tilesets);
            Pose2 pose = PoseOf(scene, id);
            char buf[96];
            std::snprintf(buf, sizeof(buf), "|%.4f|%.4f,%.4f", kv.second.tileSize, pose.sx, pose.sy);
            std::string key = "tilemap|" + kv.second.map.dump() + "|" + rules.key + buf;
            auto it = bodies.find(id);
            if (it != bodies.end() && it->second.key != key) {
                DestroyBody(it->second);
                bodies.erase(it);
                it = bodies.end();
            }
            if (it == bodies.end()) {
                BodyRec rec;
                rec.kind = Kind::Static;
                rec.tilemap = true;
                rec.key = key;
                b2BodyDef bd = b2DefaultBodyDef();
                bd.position = pose.p;
                bd.rotation = pose.q;
                bd.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
                rec.id = b2CreateBody(world, &bd);
                rec.lastPos = pose.p;
                rec.lastRot = pose.q;
                AddTilemapShapes(rec.id, kv.second, rules, pose);  // an empty body is kept so the key is remembered
                bodies[id] = std::move(rec);
            } else if (!SameP(pose.p, it->second.lastPos) || !SameQ(pose.q, it->second.lastRot)) {
                b2Body_SetTransform(it->second.id, pose.p, pose.q);
                it->second.lastPos = pose.p;
                it->second.lastRot = pose.q;
            }
            seenBodies.insert(id);
        }

        for (auto it = bodies.begin(); it != bodies.end();) {
            if (!seenBodies.count(it->first)) {
                DestroyBody(it->second);
                it = bodies.erase(it);
            } else {
                ++it;
            }
        }

        for (const auto& imp : pendingImpulses) {
            auto it = bodies.find(imp.first);
            if (it != bodies.end() && it->second.kind == Kind::Dynamic) b2Body_ApplyLinearImpulseToCenter(it->second.id, V2(imp.second), true);
        }
        pendingImpulses.clear();
    }

    // ----- Characters -----

    struct PlaneInfo {
        b2ShapeId shape;
        b2BodyId body;
        EntityId entity;
        b2Vec2 normal;
        b2Vec2 point;
    };
    struct MoverContext {
        EntityId self = kNullEntity;
        b2Vec2 velocity{0, 0};
        float feetY = 0;
        bool dropThrough = false;
        bool oneWay = true;
        std::vector<b2CollisionPlane> planes;
        std::vector<PlaneInfo> info;
    };

    static bool PlaneCallback(b2ShapeId shape, const b2PlaneResult* plane, void* context) {
        MoverContext& ctx = *static_cast<MoverContext*>(context);
        if (ctx.planes.size() >= 64) return false;
        b2BodyId body = b2Shape_GetBody(shape);
        EntityId entity = EntityOfBody(body);
        if (entity == ctx.self) return true;
        uint64_t cat = b2Shape_GetFilter(shape).categoryBits;
        if (cat & kTrigger) return true;
        if (cat & kOneWay) {
            // Only the top surface holds, and only for a character above it that is not rising.
            if (!ctx.oneWay || ctx.dropThrough || plane->plane.normal.y < 0.7f || ctx.velocity.y > 0.01f) return true;
            if (plane->point.y > ctx.feetY + 0.05f) return true;
        }
        b2CollisionPlane cp;
        cp.plane = plane->plane;
        cp.pushLimit = FLT_MAX;
        cp.push = 0.0f;
        cp.clipVelocity = true;
        if (cat & kCharacter) {
            // Characters push each other softly instead of blocking.
            cp.pushLimit = 0.02f;
            cp.clipVelocity = false;
        }
        ctx.planes.push_back(cp);
        ctx.info.push_back({shape, body, entity, plane->plane.normal, plane->point});
        return true;
    }

    struct OneWayCast {
        float fraction = 1.0f;
        float feetY = 0;
    };
    static float OneWayCastCallback(b2ShapeId, b2Vec2 point, b2Vec2 normal, float fraction, void* context) {
        OneWayCast& c = *static_cast<OneWayCast*>(context);
        if (normal.y < 0.7f || fraction <= 0.0f || point.y > c.feetY + 0.05f) return -1.0f;
        c.fraction = std::min(c.fraction, fraction);
        return fraction;
    }

    b2Capsule MoverAt(const CharacterRec& rec, b2Vec2 p) const { return b2Capsule{b2Add(p, rec.c1), b2Add(p, rec.c2), rec.radius}; }
    float FeetY(const CharacterRec& rec, b2Vec2 p) const { return p.y + std::min(rec.c1.y, rec.c2.y) - rec.radius; }

    // Collects planes around the mover at p (one-way surfaces only while moving down onto them).
    void Collide(const CharacterRec& rec, EntityId id, b2Vec2 p, b2Vec2 velocity, bool dropThrough, MoverContext& ctx) const {
        ctx.self = id;
        ctx.velocity = velocity;
        ctx.feetY = FeetY(rec, p);
        ctx.dropThrough = dropThrough;
        ctx.planes.clear();
        ctx.info.clear();
        b2Capsule mover = MoverAt(rec, p);
        b2QueryFilter filter{rec.category, Spread(rec.allowed) & (kNormal | kOneWay | kCharacter)};
        b2World_CollideMover(world, &mover, filter, PlaneCallback, &ctx);
    }

    // Sweeps the mover; returns the fraction of `delta` that is free.
    float Cast(const CharacterRec& rec, b2Vec2 p, b2Vec2 delta, bool dropThrough) const {
        b2Capsule mover = MoverAt(rec, p);
        float fraction = b2World_CastMover(world, &mover, delta, b2QueryFilter{rec.category, Spread(rec.allowed) & kNormal});
        if (delta.y < 0.0f && !dropThrough) {
            b2Vec2 pts[2] = {mover.center1, mover.center2};
            b2ShapeProxy proxy = b2MakeProxy(pts, 2, mover.radius);
            OneWayCast c;
            c.feetY = FeetY(rec, p);
            b2World_CastShape(world, &proxy, delta, b2QueryFilter{rec.category, Spread(rec.allowed) & kOneWay}, OneWayCastCallback, &c);
            // Stop just short of the platform so the next step starts on top of it.
            if (c.fraction < 1.0f) fraction = std::min(fraction, std::max(0.0f, c.fraction - 0.005f / std::max(1e-6f, b2Length(delta))));
        }
        return fraction;
    }

    void UpdateCharacters(Scene& scene, float dt) {
        const float g = PhysicsWorld::kGravity;
        for (auto& kv : characters) {
            EntityId id = kv.first;
            CharacterRec& rec = kv.second;
            CharacterBody2D* cb = scene.Get<CharacterBody2D>(id);
            if (!cb || !b2Body_IsValid(rec.body)) continue;
            const bool platformer = cb->mode != "topdown";
            const b2CosSin slope = b2ComputeCosSin(Radians(Clamp(cb->maxSlope, 0.0f, 89.0f)));  // deterministic everywhere
            const float cosSlope = slope.cosine;
            const bool wasGrounded = cb->grounded && platformer;

            b2Vec2 v = V2(cb->velocity);
            if (platformer) {
                if (wasGrounded && v.y <= 0.0f) v.y = 0.0f;
                else v.y += g * cb->gravityScale * dt;
            }
            b2Vec2 carry{0, 0};  // moving platform under the character
            if (wasGrounded && b2Body_IsValid(rec.ground) && b2Body_GetType(rec.ground) != b2_staticBody) {
                carry = b2MulSV(dt, b2Body_GetWorldPointVelocity(rec.ground, rec.groundPoint));
            }

            b2Vec2 start = rec.pos;
            b2Vec2 step = b2MulSV(dt, v);
            if (wasGrounded && v.y <= 0.0f && rec.groundNormal.y > 0.05f) {
                // Walk along the ground: the same horizontal speed up and down slopes.
                step.y = -step.x * rec.groundNormal.x / rec.groundNormal.y;
            }
            b2Vec2 target = b2Add(b2Add(rec.pos, step), carry);
            MoverContext ctx;
            rec.touching.clear();
            bool onWall = false, onCeiling = false;
            std::vector<PlaneInfo> pushes;
            for (int iteration = 0; iteration < 5; ++iteration) {
                Collide(rec, id, rec.pos, v, cb->dropThrough, ctx);
                for (const PlaneInfo& pi : ctx.info) {
                    rec.touching.insert(pi.entity);
                    if (pi.normal.y < -0.7f) onCeiling = true;
                    else if (std::fabs(pi.normal.y) < cosSlope) onWall = true;
                    if (b2Body_GetType(pi.body) == b2_dynamicBody &&
                        std::none_of(pushes.begin(), pushes.end(), [&](const PlaneInfo& q) { return B2_ID_EQUALS(q.body, pi.body); })) {
                        pushes.push_back(pi);
                    }
                }
                b2PlaneSolverResult solved = b2SolvePlanes(b2Sub(target, rec.pos), ctx.planes.data(), static_cast<int>(ctx.planes.size()));
                float fraction = Cast(rec, rec.pos, solved.translation, cb->dropThrough);
                b2Vec2 delta = b2MulSV(fraction, solved.translation);
                rec.pos = b2Add(rec.pos, delta);
                if (b2LengthSquared(delta) < 1e-6f) break;
            }
            v = b2ClipVector(v, ctx.planes.data(), static_cast<int>(ctx.planes.size()));

            // Ground probe just below the feet (platformer), snapping down slopes.
            bool grounded = false;
            rec.ground = b2_nullBodyId;
            rec.groundNormal = b2Vec2{0, 1};
            auto probe = [&]() {
                MoverContext g2;
                Collide(rec, id, b2Vec2{rec.pos.x, rec.pos.y - 0.02f}, v, cb->dropThrough, g2);
                g2.feetY = FeetY(rec, rec.pos);
                for (const PlaneInfo& pi : g2.info) {
                    if (pi.normal.y >= cosSlope) {
                        uint64_t cat = b2Shape_GetFilter(pi.shape).categoryBits;
                        if ((cat & kOneWay) && pi.point.y > FeetY(rec, rec.pos) + 0.05f) continue;
                        if (!grounded || pi.normal.y > rec.groundNormal.y) rec.groundNormal = pi.normal;  // the flattest support
                        grounded = true;
                        rec.ground = pi.body;
                        rec.groundPoint = pi.point;
                        rec.touching.insert(pi.entity);
                    }
                }
            };
            if (platformer) {
                probe();
                if (!grounded && wasGrounded && V2(cb->velocity).y <= 0.0f) {
                    float snap = 0.05f + std::fabs(v.x) * dt * std::min(5.7f, slope.sine / std::max(0.01f, slope.cosine));
                    float f = Cast(rec, rec.pos, b2Vec2{0, -snap}, cb->dropThrough);
                    if (f < 1.0f) {
                        rec.pos.y -= f * snap;
                        probe();
                    }
                }
                if (V2(cb->velocity).y <= 0.0f) {
                    // Walking or falling, not jumping: surfaces never add upward speed
                    // (no launching off the top of a ramp). On the ground the velocity
                    // keeps the requested horizontal speed unless a wall stops it.
                    if (v.y > 0.0f) v.y = 0.0f;
                    if (grounded) {
                        b2Vec2 walk{cb->velocity.x, 0.0f};
                        for (size_t i = 0; i < ctx.planes.size(); ++i) {
                            const b2Vec2 n = ctx.planes[i].plane.normal;
                            bool wall = n.y < cosSlope && n.y > -0.7f && b2Body_GetType(ctx.info[i].body) != b2_dynamicBody;
                            if (wall && walk.x * n.x < 0.0f) walk.x = 0.0f;
                        }
                        v = walk;
                    }
                }
            }
            if (onCeiling && v.y > 0.0f) v.y = 0.0f;

            // Push dynamic bodies the character walks into (force limited by pushStrength).
            if (cb->pushStrength > 0.0f) {
                for (const PlaneInfo& pi : pushes) {
                    b2Vec2 dir = b2Neg(pi.normal);
                    float into = b2Dot(V2(cb->velocity), dir);
                    if (into <= 0.0f || pi.normal.y > cosSlope) continue;  // not walking into it / standing on it
                    float mass = b2Body_GetMass(pi.body);
                    float have = b2Dot(b2Body_GetLinearVelocity(pi.body), dir);
                    float want = std::max(0.0f, into - have);
                    float impulse = std::min(mass * want, cb->pushStrength * 50.0f * dt);
                    if (impulse > 0.0f) b2Body_ApplyLinearImpulseToCenter(pi.body, b2MulSV(impulse, dir), true);
                }
            }

            // Let the world step carry the kinematic body to the new position.
            b2Body_SetTransform(rec.body, start, b2Rot_identity);
            b2Body_SetLinearVelocity(rec.body, b2MulSV(1.0f / dt, b2Sub(rec.pos, start)));

            cb->velocity.x = v.x;
            cb->velocity.y = v.y;
            cb->velocity.z = 0.0f;
            cb->grounded = grounded;
            cb->onWall = onWall;
            cb->onCeiling = onCeiling;
            WritePose2(scene, id, rec.pos, b2Rot_identity, false);
        }
    }

    void SyncOut(Scene& scene) {
        for (auto& kv : characters) {
            if (!b2Body_IsValid(kv.second.body)) continue;
            b2Body_SetTransform(kv.second.body, kv.second.pos, b2Rot_identity);
            b2Body_SetLinearVelocity(kv.second.body, b2Vec2{0, 0});
        }
        for (auto& kv : bodies) {
            BodyRec& rec = kv.second;
            if (rec.kind != Kind::Dynamic) continue;
            b2Transform xf = b2Body_GetTransform(rec.id);
            bool rotChanged = !SameQ(xf.q, rec.lastRot);
            if (!SameP(xf.p, rec.lastPos) || rotChanged) {
                WritePose2(scene, kv.first, xf.p, xf.q, rotChanged);
                rec.seen = SeenOf(scene, kv.first);
            }
            rec.lastPos = xf.p;
            rec.lastRot = xf.q;
            if (RigidBody2D* rb = scene.Get<RigidBody2D>(kv.first)) {
                b2Vec2 lv = b2Body_GetLinearVelocity(rec.id);
                rb->velocity = Vec3(lv.x, lv.y, 0.0f);
                rb->angularVelocity = Degrees(b2Body_GetAngularVelocity(rec.id));
                rec.lastVelocity = rb->velocity;
                rec.lastAngular = rb->angularVelocity;
            }
        }
    }

    // ----- Contacts & queries -----

    std::set<EntityPair> Collisions() const {
        std::set<EntityPair> out;
        std::vector<b2ContactData> contacts;
        auto collect = [&](b2BodyId body) {
            int cap = b2Body_GetContactCapacity(body);
            if (cap <= 0) return;
            contacts.resize(static_cast<size_t>(cap));
            int n = b2Body_GetContactData(body, contacts.data(), cap);
            for (int i = 0; i < n; ++i) {
                if (contacts[static_cast<size_t>(i)].manifold.pointCount == 0) continue;
                EntityId a = EntityOfShape(contacts[static_cast<size_t>(i)].shapeIdA), b = EntityOfShape(contacts[static_cast<size_t>(i)].shapeIdB);
                if (a != b && a != kNullEntity && b != kNullEntity) out.insert(a < b ? EntityPair(a, b) : EntityPair(b, a));
            }
        };
        for (const auto& kv : bodies) {
            if (kv.second.kind != Kind::Static) collect(kv.second.id);
        }
        for (const auto& kv : characters) {
            if (b2Body_IsValid(kv.second.body)) collect(kv.second.body);
            for (EntityId other : kv.second.touching) {
                if (other != kv.first && other != kNullEntity) out.insert(kv.first < other ? EntityPair(kv.first, other) : EntityPair(other, kv.first));
            }
        }
        return out;
    }

    static b2ShapeProxy WorldProxy(b2ShapeId shape, b2Transform xf) {
        switch (b2Shape_GetType(shape)) {
            case b2_circleShape: {
                b2Circle c = b2Shape_GetCircle(shape);
                b2Vec2 p = b2TransformPoint(xf, c.center);
                return b2MakeProxy(&p, 1, c.radius);
            }
            case b2_capsuleShape: {
                b2Capsule c = b2Shape_GetCapsule(shape);
                b2Vec2 p[2] = {b2TransformPoint(xf, c.center1), b2TransformPoint(xf, c.center2)};
                return b2MakeProxy(p, 2, c.radius);
            }
            case b2_segmentShape: {
                b2Segment s = b2Shape_GetSegment(shape);
                b2Vec2 p[2] = {b2TransformPoint(xf, s.point1), b2TransformPoint(xf, s.point2)};
                return b2MakeProxy(p, 2, 0.0f);
            }
            default: {
                b2Polygon poly = b2Shape_GetPolygon(shape);
                b2Vec2 p[B2_MAX_POLYGON_VERTICES];
                for (int i = 0; i < poly.count; ++i) p[i] = b2TransformPoint(xf, poly.vertices[i]);
                return b2MakeProxy(p, poly.count, poly.radius);
            }
        }
    }

    struct TriggerQuery {
        EntityId self;
        std::set<EntityId>* hits;
    };
    static bool TriggerCallback(b2ShapeId shape, void* context) {
        TriggerQuery& q = *static_cast<TriggerQuery*>(context);
        b2BodyId body = b2Shape_GetBody(shape);
        if (b2Body_GetType(body) == b2_staticBody) return true;  // only moving things enter triggers
        EntityId e = EntityOfBody(body);
        if (e != q.self && e != kNullEntity) q.hits->insert(e);
        return true;
    }

    std::set<EntityPair> Triggers() const {
        std::set<EntityPair> out;
        for (const auto& kv : bodies) {
            if (kv.second.triggers.empty()) continue;
            b2Transform xf = b2Body_GetTransform(kv.second.id);
            std::set<EntityId> hits;
            TriggerQuery q{kv.first, &hits};
            for (b2ShapeId s : kv.second.triggers) {
                b2Filter f = b2Shape_GetFilter(s);
                b2ShapeProxy proxy = WorldProxy(s, xf);
                b2World_OverlapShape(world, &proxy, b2QueryFilter{f.categoryBits, f.maskBits & ~kTrigger}, TriggerCallback, &q);
            }
            for (EntityId e : hits) out.insert(EntityPair(kv.first, e));
        }
        return out;
    }

    b2WorldId world;
    std::map<EntityId, BodyRec> bodies;
    std::map<EntityId, CharacterRec> characters;
    std::vector<std::pair<EntityId, Vec3>> pendingImpulses;
    uint64_t steps = 0;
};

// ----- Physics2D -----------------------------------------------------------------------------

Physics2D::Physics2D() : impl_(std::make_unique<Impl>()) {}
Physics2D::~Physics2D() = default;

bool Physics2D::Wanted(const Scene& scene) { return !scene.Pool<Collider2D>().empty() || !scene.Pool<CharacterBody2D>().empty(); }

void Physics2D::Sync(Scene& scene, std::vector<std::string>& warnings) { impl_->Sync(scene, 0.0f, warnings, tilesets_); }

void Physics2D::Step(Scene& scene, float dt, std::vector<std::string>& warnings) {
    impl_->Sync(scene, dt, warnings, tilesets_);
    impl_->UpdateCharacters(scene, dt);
    b2World_Step(impl_->world, dt, 4);
    impl_->SyncOut(scene);
    ++impl_->steps;
}

std::set<EntityPair> Physics2D::Collisions() const { return impl_->Collisions(); }
std::set<EntityPair> Physics2D::Triggers(Scene&) const { return impl_->Triggers(); }

bool Physics2D::Raycast(const Vec3& origin, const Vec3& direction, float maxDistance, RaycastHit& out) const {
    Vec3 dir = Normalize(direction);
    b2Vec2 translation{dir.x * maxDistance, dir.y * maxDistance};
    if (b2LengthSquared(translation) < 1e-12f) return false;  // straight along Z: misses the plane's shapes
    b2RayResult r = b2World_CastRayClosest(impl_->world, b2Vec2{origin.x, origin.y}, translation, b2QueryFilter{~0ull, kNormal | kOneWay | kCharacter});
    if (!r.hit) return false;
    out.hit = true;
    out.entity = EntityOfShape(r.shapeId);
    out.distance = r.fraction * maxDistance;
    out.point = origin + dir * out.distance;
    out.normal = Vec3(r.normal.x, r.normal.y, 0.0f);
    return true;
}

namespace {
bool CollectOverlap(b2ShapeId shape, void* context) {
    static_cast<std::set<EntityId>*>(context)->insert(EntityOfShape(shape));
    return true;
}
}  // namespace

std::vector<EntityId> Physics2D::OverlapCircle(const Vec3& center, float radius) const {
    std::set<EntityId> ids;
    b2Vec2 c{center.x, center.y};
    b2ShapeProxy proxy = b2MakeProxy(&c, 1, std::max(0.001f, radius));
    b2World_OverlapShape(impl_->world, &proxy, b2QueryFilter{~0ull, kNormal | kOneWay | kCharacter}, CollectOverlap, &ids);
    ids.erase(kNullEntity);
    return std::vector<EntityId>(ids.begin(), ids.end());
}

void Physics2D::AddImpulse(EntityId id, const Vec3& impulse) { impl_->pendingImpulses.emplace_back(id, impulse); }

bool Physics2D::Has(EntityId id) const { return impl_->bodies.count(id) > 0 || impl_->characters.count(id) > 0; }

void Physics2D::AppendStats(Json& s) const {
    int statics = 0, dynamics = 0, kinematics = 0, triggers = 0;
    for (const auto& kv : impl_->bodies) {
        if (!kv.second.triggers.empty()) ++triggers;
        else if (kv.second.kind == Impl::Kind::Static) ++statics;
        else if (kv.second.kind == Impl::Kind::Dynamic) ++dynamics;
        else ++kinematics;
    }
    Json j = Json::MakeObject();
    j["backend"] = "box2d 3.1.1 (cross-platform deterministic)";
    j["staticBodies"] = statics;
    j["dynamicBodies"] = dynamics;
    j["kinematicBodies"] = kinematics;
    j["characters"] = static_cast<uint64_t>(impl_->characters.size());
    j["triggers"] = triggers;
    j["steps"] = static_cast<uint64_t>(impl_->steps);
    b2Counters c = b2World_GetCounters(impl_->world);
    j["shapes"] = c.shapeCount;
    j["contacts"] = c.contactCount;
    s["world2D"] = j;
}

}  // namespace oe
