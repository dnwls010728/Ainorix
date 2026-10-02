#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

#include "core/Log.h"
#include "physics/Physics2D.h"
#include "scene/Components.h"
#include "scene/TileGrid.h"
#include "scene/Scene.h"

// Jolt must be included first, then the rest of its headers.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/RegisterTypes.h>

namespace oe {

const char* ToString(PhysicsEvent::Kind kind) {
    switch (kind) {
        case PhysicsEvent::Kind::CollisionEnter: return "onCollisionEnter";
        case PhysicsEvent::Kind::CollisionExit: return "onCollisionExit";
        case PhysicsEvent::Kind::TriggerEnter: return "onTriggerEnter";
        case PhysicsEvent::Kind::TriggerExit: return "onTriggerExit";
    }
    return "";
}

namespace {

// ----- Jolt global setup ------------------------------------------------------

void JoltTrace(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    OE_LOG_DEBUG("physics", "jolt: %s", buffer);
}

void EnsureJolt() {
    static bool initialized = [] {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = JoltTrace;
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        return true;
    }();
    (void)initialized;
}

namespace Layers {
constexpr JPH::ObjectLayer kStatic = 0;
constexpr JPH::ObjectLayer kMoving = 1;
}  // namespace Layers

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a == Layers::kMoving || b == Layers::kMoving;
    }
};

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override { return JPH::BroadPhaseLayer(static_cast<JPH::uint8>(layer)); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return static_cast<JPH::BroadPhaseLayer::Type>(layer) == 0 ? "static" : "moving";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override {
        return layer == Layers::kMoving || static_cast<JPH::BroadPhaseLayer::Type>(bp) == Layers::kMoving;
    }
};

// Accepts only moving bodies (dynamic, kinematic, characters). Used for
// trigger overlaps so a coin does not "enter" the floor it rests on.
class MovingOnlyFilter final : public JPH::ObjectLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer) const override { return layer == Layers::kMoving; }
};

JPH::Vec3 J(const Vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
Vec3 O(JPH::Vec3Arg v) { return Vec3(v.GetX(), v.GetY(), v.GetZ()); }

JPH::Mat44 J(const Mat4& m) {
    return JPH::Mat44(JPH::Vec4(m.m[0], m.m[1], m.m[2], m.m[3]), JPH::Vec4(m.m[4], m.m[5], m.m[6], m.m[7]),
                      JPH::Vec4(m.m[8], m.m[9], m.m[10], m.m[11]), JPH::Vec4(m.m[12], m.m[13], m.m[14], m.m[15]));
}

struct WorldXf {
    JPH::Vec3 pos;
    JPH::Quat rot;
    JPH::Vec3 scale;
};

WorldXf WorldOf(const Scene& scene, EntityId id) {
    JPH::Vec3 scale;
    JPH::Mat44 rt = J(scene.WorldMatrix(id)).Decompose(scale);
    return {rt.GetTranslation(), rt.GetQuaternion().Normalized(), scale};
}

bool SamePos(JPH::Vec3Arg a, JPH::Vec3Arg b) { return (a - b).LengthSq() < 1e-10f; }
bool SameRot(JPH::QuatArg a, JPH::QuatArg b) { return std::fabs(a.Dot(b)) > 1.0f - 1e-7f; }

// Writes a world-space pose into the entity's local Transform.
void WritePose(Scene& scene, EntityId id, JPH::Vec3 pos, JPH::Quat rot, bool writeRotation) {
    Transform* t = scene.Get<Transform>(id);
    if (!t) return;
    EntityId parent = scene.Record(id)->parent;
    if (parent != kNullEntity) {
        JPH::Mat44 local = J(scene.WorldMatrix(parent)).Inversed() * JPH::Mat44::sRotationTranslation(rot, pos);
        JPH::Vec3 ignored;
        JPH::Mat44 rt = local.Decompose(ignored);
        pos = rt.GetTranslation();
        rot = rt.GetQuaternion().Normalized();
    }
    t->position = O(pos);
    if (writeRotation) {
        JPH::Vec3 e = rot.GetEulerAngles();
        t->rotation = Vec3(Degrees(e.GetX()), Degrees(e.GetY()), Degrees(e.GetZ()));
    }
}

JPH::RefConst<JPH::Shape> Build(const JPH::ShapeSettings& settings, std::string* error) {
    JPH::ShapeSettings::ShapeResult r = settings.Create();
    if (r.HasError()) {
        if (error) *error = r.GetError().c_str();
        return nullptr;
    }
    return r.Get();
}

JPH::RefConst<JPH::Shape> ColliderShape(const Collider& c, JPH::Vec3 scale, std::string* error) {
    scale = scale.Abs();
    JPH::RefConst<JPH::Shape> inner;
    if (c.shape == "sphere") {
        inner = Build(JPH::SphereShapeSettings(std::max(0.001f, c.radius * scale.ReduceMax())), error);
    } else if (c.shape == "capsule") {
        float r = std::max(0.001f, c.radius * std::max(scale.GetX(), scale.GetZ()));
        float half = std::max(0.001f, 0.5f * c.height * scale.GetY() - r);
        inner = Build(JPH::CapsuleShapeSettings(half, r), error);
    } else {
        JPH::Vec3 half = (J(c.size) * scale * 0.5f).Abs();
        half = JPH::Vec3::sMax(half, JPH::Vec3::sReplicate(0.001f));
        float convex = std::min(JPH::cDefaultConvexRadius, 0.5f * half.ReduceMin());
        inner = Build(JPH::BoxShapeSettings(half, convex), error);
    }
    if (!inner || c.center == Vec3(0, 0, 0)) return inner;
    return Build(JPH::RotatedTranslatedShapeSettings(J(c.center) * scale, JPH::Quat::sIdentity(), inner), error);
}

JPH::RefConst<JPH::Shape> CharacterShape(const CharacterBody& c, float* supportOffset) {
    float r = std::max(0.01f, c.radius);
    float half = c.shape == "sphere" ? 0.0f : std::max(0.01f, 0.5f * c.height - r);
    *supportOffset = half + 0.5f * r;  // contacts below this depth count as ground
    std::string err;
    if (c.shape == "sphere") return Build(JPH::SphereShapeSettings(r), &err);
    return Build(JPH::CapsuleShapeSettings(half, r), &err);
}

std::string ColliderKey(const Scene& s, EntityId id, JPH::Vec3 scale) {
    const Collider* c = s.Get<Collider>(id);
    const RigidBody* rb = s.Get<RigidBody>(id);
    std::string key = ComponentToJson(*TypeRegistry::Find("Collider"), c).dump();
    if (rb) {
        Json j = ComponentToJson(*TypeRegistry::Find("RigidBody"), rb);
        j.erase("velocity");
        j.erase("angularVelocity");
        key += j.dump();
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "|%.4f,%.4f,%.4f", scale.GetX(), scale.GetY(), scale.GetZ());
    return key + buf;
}

// One static compound shape for the collision of a tilemap: solid cells as
// merged boxes, shaped cells (slopes...) as prisms. One-way tiles are 2D only.
JPH::RefConst<JPH::Shape> TilemapShape(const Tilemap& tm, const TileRules& rules, JPH::Vec3 scale) {
    std::vector<TileRect> rects = SolidRects(tm, rules);
    std::vector<TileShape> shaped = ShapedCells(tm, rules);
    if (rects.empty() && shaped.empty()) return nullptr;
    scale = scale.Abs();
    const float ts = std::max(0.001f, tm.tileSize);
    const float depth = std::max(0.5f, ts);  // generous Z thickness so 2D bodies never slip past
    JPH::StaticCompoundShapeSettings compound;
    for (const TileRect& r : rects) {
        JPH::Vec3 half(0.5f * ts * static_cast<float>(r.width), 0.5f * ts * static_cast<float>(r.height), depth);
        half = half * scale;
        JPH::Vec3 center((static_cast<float>(r.col) + 0.5f * static_cast<float>(r.width)) * ts,
                         -(static_cast<float>(r.row) + 0.5f * static_cast<float>(r.height)) * ts, 0.0f);
        float convex = std::min(JPH::cDefaultConvexRadius, 0.5f * half.ReduceMin());
        compound.AddShape(center * scale, JPH::Quat::sIdentity(), new JPH::BoxShapeSettings(half, convex));
    }
    for (const TileShape& cell : shaped) {
        JPH::Array<JPH::Vec3> pts;
        for (const TilePoint& p : cell.points) {
            for (float z : {-depth, depth}) pts.push_back(JPH::Vec3(p.x * ts * scale.GetX(), p.y * ts * scale.GetY(), z * scale.GetZ()));
        }
        compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), new JPH::ConvexHullShapeSettings(pts, 0.0f));
    }
    std::string err;
    return Build(compound, &err);
}

std::string TilemapKey(const Tilemap& tm, const TileRules& rules, JPH::Vec3 scale) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "|%.4f|%.4f,%.4f,%.4f", tm.tileSize, scale.GetX(), scale.GetY(), scale.GetZ());
    return "tilemap|" + tm.map.dump() + "|" + rules.key + buf;
}

std::string CharacterKey(const CharacterBody& c) {
    Json j = ComponentToJson(*TypeRegistry::Find("CharacterBody"), &c);
    j.erase("velocity");
    j.erase("grounded");
    return j.dump();
}

EntityPair Ordered(EntityId a, EntityId b) { return a < b ? EntityPair(a, b) : EntityPair(b, a); }

}  // namespace

// ----- Impl -------------------------------------------------------------------

struct PhysicsWorld::Impl : public JPH::ContactListener {
    enum class Kind { Static, Dynamic, Kinematic };
    struct BodyRec {
        JPH::BodyID id;
        std::string key;
        Kind kind = Kind::Static;
        JPH::Vec3 lastPos;
        JPH::Quat lastRot;
        Vec3 lastVelocity;
        Vec3 lastAngular;
    };
    struct TriggerRec {
        JPH::RefConst<JPH::Shape> shape;
        std::string key;
    };
    struct CharacterRec {
        JPH::Ref<JPH::CharacterVirtual> ch;
        std::string key;
        JPH::Vec3 lastPos;
        JPH::Ref<JPH::CharacterVirtualSettings> settings;
        float planeZ = 0.0f;  // CharacterBody.plane2D keeps the character on this Z
    };

    explicit Impl(bool snapshots = false) : temp(16 * 1024 * 1024), jobs(JPH::cMaxPhysicsJobs), nativeIds(snapshots) {
        system.Init(65536, 0, 65536, 10240, bpLayers, objVsBp, pairFilter);
        system.SetGravity(JPH::Vec3(0, kGravity, 0));
        system.SetContactListener(this);
    }

    ~Impl() override {
        characters.clear();  // removes inner bodies
        JPH::BodyInterface& bi = system.GetBodyInterface();
        for (auto& kv : bodies) {
            bi.RemoveBody(kv.second.id);
            bi.DestroyBody(kv.second.id);
        }
    }

    // ContactListener. Contacts are tracked per sub-shape pair. Jolt also
    // reports a removal when bodies fall asleep; those are resolved after the
    // step (see ApplyRemovals) so resting objects keep touching.
    using ContactKey = std::array<JPH::uint32, 4>;  // body1, sub1, body2, sub2 (body1 < body2)
    static ContactKey Key(JPH::BodyID b1, JPH::SubShapeID s1, JPH::BodyID b2, JPH::SubShapeID s2) {
        JPH::uint32 x = b1.GetIndexAndSequenceNumber(), y = b2.GetIndexAndSequenceNumber();
        if (x < y) return {x, s1.GetValue(), y, s2.GetValue()};
        return {y, s2.GetValue(), x, s1.GetValue()};
    }
    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& m, JPH::ContactSettings&) override {
        std::lock_guard<std::mutex> lock(contactMutex);
        bodyContacts.insert(Key(a.GetID(), m.mSubShapeID1, b.GetID(), m.mSubShapeID2));
    }
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        std::lock_guard<std::mutex> lock(contactMutex);
        pendingRemovals.push_back({Key(pair.GetBody1ID(), pair.GetSubShapeID1(), pair.GetBody2ID(), pair.GetSubShapeID2()), pair.GetBody1ID(), pair.GetBody2ID()});
    }
    struct Removal {
        ContactKey key;
        JPH::BodyID a, b;
    };
    void ApplyRemovals() {
        JPH::BodyInterface& bi = system.GetBodyInterface();
        for (const Removal& r : pendingRemovals) {
            bool bothKnown = bodyToEntity.count(r.a.GetIndexAndSequenceNumber()) && bodyToEntity.count(r.b.GetIndexAndSequenceNumber());
            bool asleep = bothKnown && !bi.IsActive(r.a) && !bi.IsActive(r.b);
            if (!asleep) bodyContacts.erase(r.key);
        }
        pendingRemovals.clear();
    }

    EntityId EntityOf(JPH::BodyID id) const {
        auto it = bodyToEntity.find(id.GetIndexAndSequenceNumber());
        return it == bodyToEntity.end() ? kNullEntity : it->second;
    }

    void ForgetBody(JPH::BodyID id) {
        JPH::uint32 key = id.GetIndexAndSequenceNumber();
        bodyToEntity.erase(key);
        for (auto it = bodyContacts.begin(); it != bodyContacts.end();) {
            it = ((*it)[0] == key || (*it)[2] == key) ? bodyContacts.erase(it) : std::next(it);
        }
    }

    void RemoveBody(EntityId, BodyRec& rec) {
        JPH::BodyInterface& bi = system.GetBodyInterface();
        ForgetBody(rec.id);
        bi.RemoveBody(rec.id);
        bi.DestroyBody(rec.id);
    }

    void RemoveCharacter(CharacterRec& rec) {
        if (rec.ch) ForgetBody(rec.ch->GetInnerBodyID());
        rec.ch = nullptr;
    }

    void Warn(std::vector<std::string>& warnings, const std::string& msg) {
        if (std::find(warnings.begin(), warnings.end(), msg) != warnings.end()) return;
        warnings.push_back(msg);
        OE_LOG_WARN("physics", "%s", msg.c_str());
    }

    // Mirrors components into Jolt. dt == 0 means "query sync" (no motion).
    void SyncIn(Scene& scene, float dt, std::vector<std::string>& warnings, const TilesetLookup* tilesets) {
        JPH::BodyInterface& bi = system.GetBodyInterface();

        // Characters.
        std::set<EntityId> seenChars;
        for (auto& kv : scene.Pool<CharacterBody>()) {
            EntityId id = kv.first;
            seenChars.insert(id);
            WorldXf wx = WorldOf(scene, id);
            std::string key = CharacterKey(kv.second);
            CharacterRec& rec = characters[id];
            if (!rec.ch || rec.key != key) {
                RemoveCharacter(rec);
                float support = 0;
                JPH::Ref<JPH::CharacterVirtualSettings> cs = new JPH::CharacterVirtualSettings();
                if (nativeIds) { cs->mInnerBodyIDOverride = NextBodyId(); cs->mID = JPH::CharacterID(id); }
                rec.settings = cs;
                cs->mShape = CharacterShape(kv.second, &support);
                cs->mInnerBodyShape = cs->mShape;
                cs->mInnerBodyLayer = Layers::kMoving;
                cs->mMaxSlopeAngle = JPH::DegreesToRadians(Clamp(kv.second.maxSlope, 0.0f, 89.0f));
                cs->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), support);
                rec.ch = new JPH::CharacterVirtual(cs, wx.pos, JPH::Quat::sIdentity(), id, &system);
                bodyToEntity[rec.ch->GetInnerBodyID().GetIndexAndSequenceNumber()] = id;
                rec.key = key;
                rec.lastPos = wx.pos;
                rec.planeZ = wx.pos.GetZ();
                if (scene.Get<Collider>(id)) Warn(warnings, "entity " + std::to_string(id) + " has both CharacterBody and Collider; the Collider is ignored (CharacterBody defines the shape)");
            } else if (!SamePos(wx.pos, rec.lastPos)) {
                rec.ch->SetPosition(wx.pos);  // teleported by a script or the editor
                rec.lastPos = wx.pos;
                rec.planeZ = wx.pos.GetZ();
            }
        }
        for (auto it = characters.begin(); it != characters.end();) {
            if (!seenChars.count(it->first)) {
                RemoveCharacter(it->second);
                it = characters.erase(it);
            } else {
                ++it;
            }
        }

        // Colliders: triggers (query volumes) and bodies.
        std::set<EntityId> seenBodies, seenTriggers;
        bool added = false;
        for (auto& kv : scene.Pool<Collider>()) {
            EntityId id = kv.first;
            if (seenChars.count(id)) continue;
            WorldXf wx = WorldOf(scene, id);
            std::string key = ColliderKey(scene, id, wx.scale);
            if (kv.second.isTrigger) {
                seenTriggers.insert(id);
                TriggerRec& tr = triggers[id];
                if (tr.key != key) {
                    std::string err;
                    tr.shape = ColliderShape(kv.second, wx.scale, &err);
                    tr.key = key;
                    if (!tr.shape) Warn(warnings, "entity " + std::to_string(id) + ": invalid trigger shape: " + err);
                }
                continue;
            }
            seenBodies.insert(id);
            const RigidBody* rb = scene.Get<RigidBody>(id);
            auto it = bodies.find(id);
            if (it != bodies.end() && it->second.key != key) {
                RemoveBody(id, it->second);
                bodies.erase(it);
                it = bodies.end();
            }
            if (it == bodies.end()) {
                std::string err;
                JPH::RefConst<JPH::Shape> shape = ColliderShape(kv.second, wx.scale, &err);
                if (!shape) {
                    Warn(warnings, "entity " + std::to_string(id) + ": invalid collider shape: " + err);
                    continue;
                }
                BodyRec rec;
                rec.kind = !rb ? Kind::Static : (rb->type == "kinematic" ? Kind::Kinematic : Kind::Dynamic);
                JPH::EMotionType motion = rec.kind == Kind::Static ? JPH::EMotionType::Static
                                          : rec.kind == Kind::Kinematic ? JPH::EMotionType::Kinematic
                                                                        : JPH::EMotionType::Dynamic;
                JPH::BodyCreationSettings bs(shape, wx.pos, wx.rot, motion, rec.kind == Kind::Static ? Layers::kStatic : Layers::kMoving);
                bs.mUserData = id;
                bs.mFriction = kv.second.friction;
                bs.mRestitution = kv.second.bounciness;
                if (rb) {
                    bs.mGravityFactor = rb->gravityScale;
                    bs.mLinearDamping = std::max(0.0f, rb->linearDamping);
                    bs.mLinearVelocity = J(rb->velocity);
                    bs.mAngularVelocity = J(rb->angularVelocity) * (kPi / 180.0f);
                    bs.mMotionQuality = rb->continuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
                    if (rb->plane2D) {
                        bs.mAllowedDOFs = rb->lockRotation ? JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY : JPH::EAllowedDOFs::Plane2D;
                    } else if (rb->lockRotation) {
                        bs.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
                    }
                    if (rec.kind == Kind::Dynamic) {
                        bs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                        bs.mMassPropertiesOverride.mMass = std::max(0.001f, rb->mass);
                    }
                }
                rec.id = CreateBody(bs, rec.kind == Kind::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
                if (rec.id.IsInvalid()) {
                    Warn(warnings, "physics body limit reached");
                    continue;
                }
                rec.key = key;
                rec.lastPos = wx.pos;
                rec.lastRot = wx.rot;
                if (rb) {
                    rec.lastVelocity = rb->velocity;
                    rec.lastAngular = rb->angularVelocity;
                }
                bodyToEntity[rec.id.GetIndexAndSequenceNumber()] = id;
                bodies[id] = rec;
                added = true;
                continue;
            }
            BodyRec& rec = it->second;
            bool moved = !SamePos(wx.pos, rec.lastPos) || !SameRot(wx.rot, rec.lastRot);
            if (rec.kind == Kind::Kinematic && dt > 0) {
                bi.MoveKinematic(rec.id, wx.pos, wx.rot, dt);
            } else if (moved) {
                bi.SetPositionAndRotation(rec.id, wx.pos, wx.rot, rec.kind == Kind::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
            }
            rec.lastPos = wx.pos;
            rec.lastRot = wx.rot;
            if (rec.kind == Kind::Dynamic && rb) {
                if (!(rb->velocity == rec.lastVelocity)) {
                    bi.SetLinearVelocity(rec.id, J(rb->velocity));
                    bi.ActivateBody(rec.id);
                    rec.lastVelocity = rb->velocity;
                }
                if (!(rb->angularVelocity == rec.lastAngular)) {
                    bi.SetAngularVelocity(rec.id, J(rb->angularVelocity) * (kPi / 180.0f));
                    bi.ActivateBody(rec.id);
                    rec.lastAngular = rb->angularVelocity;
                }
            }
        }
        // Tilemaps: one static body per map with its solid cells.
        for (auto& kv : scene.Pool<Tilemap>()) {
            EntityId id = kv.first;
            if (seenBodies.count(id) || seenChars.count(id) || seenTriggers.count(id)) {
                Warn(warnings, "entity " + std::to_string(id) + " has a Tilemap and a Collider/CharacterBody; the tilemap does not collide");
                continue;
            }
            TileRules rules = BuildTileRules(kv.second, tilesets);
            if (!rules.error.empty()) Warn(warnings, "entity " + std::to_string(id) + " Tilemap: " + rules.error);
            WorldXf wx = WorldOf(scene, id);
            std::string key = TilemapKey(kv.second, rules, wx.scale);
            auto it = bodies.find(id);
            if (it != bodies.end() && it->second.key != key) {
                RemoveBody(id, it->second);
                bodies.erase(it);
                it = bodies.end();
            }
            if (it == bodies.end()) {
                auto empty = emptyTilemaps.find(id);
                if (empty != emptyTilemaps.end() && empty->second == key) continue;  // nothing collides (decoration layer)
                JPH::RefConst<JPH::Shape> shape = TilemapShape(kv.second, rules, wx.scale);
                if (!shape) {
                    emptyTilemaps[id] = key;
                    continue;
                }
                emptyTilemaps.erase(id);
                JPH::BodyCreationSettings bs(shape, wx.pos, wx.rot, JPH::EMotionType::Static, Layers::kStatic);
                bs.mUserData = id;
                bs.mFriction = 0.2f;
                BodyRec rec;
                rec.kind = Kind::Static;
                rec.id = CreateBody(bs, JPH::EActivation::DontActivate);
                if (rec.id.IsInvalid()) {
                    Warn(warnings, "physics body limit reached");
                    continue;
                }
                rec.key = key;
                rec.lastPos = wx.pos;
                rec.lastRot = wx.rot;
                bodyToEntity[rec.id.GetIndexAndSequenceNumber()] = id;
                bodies[id] = rec;
                added = true;
            } else if (!SamePos(wx.pos, it->second.lastPos) || !SameRot(wx.rot, it->second.lastRot)) {
                bi.SetPositionAndRotation(it->second.id, wx.pos, wx.rot, JPH::EActivation::DontActivate);
                it->second.lastPos = wx.pos;
                it->second.lastRot = wx.rot;
            }
            seenBodies.insert(id);
        }

        for (auto it = bodies.begin(); it != bodies.end();) {
            if (!seenBodies.count(it->first)) {
                RemoveBody(it->first, it->second);
                it = bodies.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = triggers.begin(); it != triggers.end();) {
            it = seenTriggers.count(it->first) ? std::next(it) : triggers.erase(it);
        }
        if (added) system.OptimizeBroadPhase();

        // Impulses queued by scripts.
        for (const auto& imp : pendingImpulses) {
            auto it = bodies.find(imp.first);
            if (it != bodies.end() && it->second.kind == Kind::Dynamic) bi.AddImpulse(it->second.id, J(imp.second));
        }
        pendingImpulses.clear();
    }

    void UpdateCharacters(Scene& scene, float dt) {
        JPH::DefaultBroadPhaseLayerFilter bpFilter = system.GetDefaultBroadPhaseLayerFilter(Layers::kMoving);
        JPH::DefaultObjectLayerFilter layerFilter = system.GetDefaultLayerFilter(Layers::kMoving);
        JPH::ShapeFilter shapeFilter;
        JPH::Vec3 gravity = system.GetGravity();
        for (auto& kv : characters) {
            CharacterBody* cb = scene.Get<CharacterBody>(kv.first);
            JPH::CharacterVirtual& ch = *kv.second.ch;
            JPH::Vec3 desired = J(cb->velocity);
            if (cb->plane2D) desired.SetZ(0.0f);
            JPH::Vec3 v = desired;
            bool onGround = ch.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
            if (onGround && desired.GetY() <= 0.0f) v.SetY(ch.GetGroundVelocity().GetY());
            else v.SetY(desired.GetY() + gravity.GetY() * cb->gravityScale * dt);
            ch.SetLinearVelocity(v);

            JPH::CharacterVirtual::ExtendedUpdateSettings settings;
            settings.mWalkStairsStepUp = JPH::Vec3(0, std::max(0.0f, cb->stepHeight), 0);
            JPH::IgnoreSingleBodyFilter bodyFilter(ch.GetInnerBodyID());
            ch.ExtendedUpdate(dt, gravity * cb->gravityScale, settings, bpFilter, layerFilter, bodyFilter, shapeFilter, temp);

            // Hitting a ceiling ends the upward motion (otherwise the character
            // hangs under it until gravity has eaten the jump velocity).
            JPH::Vec3 after = ch.GetLinearVelocity();
            if (after.GetY() > 0.0f) {
                for (const JPH::CharacterContact& c : ch.GetActiveContacts()) {
                    if (c.mHadCollision && c.mContactNormal.GetY() < -0.5f) {
                        after.SetY(0.0f);
                        ch.SetLinearVelocity(after);
                        break;
                    }
                }
            }

            JPH::Vec3 pos = ch.GetPosition();
            if (cb->plane2D && pos.GetZ() != kv.second.planeZ) {
                pos.SetZ(kv.second.planeZ);
                ch.SetPosition(pos);
                JPH::Vec3 lv = ch.GetLinearVelocity();
                lv.SetZ(0.0f);
                ch.SetLinearVelocity(lv);
            }
            Transform* t = scene.Get<Transform>(kv.first);
            if (t) WritePose(scene, kv.first, pos, WorldOf(scene, kv.first).rot, false);
            kv.second.lastPos = pos;
            cb->velocity = O(ch.GetLinearVelocity());
            cb->grounded = ch.GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
        }
    }

    void SyncOut(Scene& scene) {
        JPH::BodyInterface& bi = system.GetBodyInterface();
        for (auto& kv : bodies) {
            BodyRec& rec = kv.second;
            if (rec.kind != Kind::Dynamic) continue;
            JPH::RVec3 pos;
            JPH::Quat rot;
            bi.GetPositionAndRotation(rec.id, pos, rot);
            rot = rot.Normalized();
            bool rotChanged = !SameRot(rot, rec.lastRot);
            if (!SamePos(pos, rec.lastPos) || rotChanged) WritePose(scene, kv.first, pos, rot, rotChanged);
            rec.lastPos = pos;
            rec.lastRot = rot;
            if (RigidBody* rb = scene.Get<RigidBody>(kv.first)) {
                rb->velocity = O(bi.GetLinearVelocity(rec.id));
                rb->angularVelocity = O(bi.GetAngularVelocity(rec.id)) * (180.0f / kPi);
                rec.lastVelocity = rb->velocity;
                rec.lastAngular = rb->angularVelocity;
            }
        }
    }

    std::set<EntityPair> CurrentCollisions() const {
        std::set<EntityPair> out;
        for (const ContactKey& k : bodyContacts) {
            auto a = bodyToEntity.find(k[0]), b = bodyToEntity.find(k[2]);
            if (a != bodyToEntity.end() && b != bodyToEntity.end() && a->second != b->second) out.insert(Ordered(a->second, b->second));
        }
        for (const auto& kv : characters) {
            for (const JPH::CharacterContact& c : kv.second.ch->GetActiveContacts()) {
                if (!c.mHadCollision || c.mBodyB.IsInvalid()) continue;
                EntityId other = EntityOf(c.mBodyB);
                if (other != kNullEntity && other != kv.first) out.insert(Ordered(kv.first, other));
            }
        }
        return out;
    }

    // Trigger pairs are (trigger, other), not ordered, so the trigger side is known.
    std::set<EntityPair> CurrentTriggers(Scene& scene) {
        std::set<EntityPair> out;
        MovingOnlyFilter moving;
        for (auto& kv : triggers) {
            if (!kv.second.shape) continue;
            WorldXf wx = WorldOf(scene, kv.first);
            JPH::RMat44 com = JPH::RMat44::sRotationTranslation(wx.rot, wx.pos + wx.rot * kv.second.shape->GetCenterOfMass());
            JPH::CollideShapeSettings settings;
            JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
            system.GetNarrowPhaseQuery().CollideShape(kv.second.shape, JPH::Vec3::sOne(), com, settings, JPH::RVec3::sZero(), collector, {}, moving);
            for (const JPH::CollideShapeResult& hit : collector.mHits) {
                EntityId other = EntityOf(hit.mBodyID2);
                if (other != kNullEntity && other != kv.first) out.insert(EntityPair(kv.first, other));
            }
        }
        return out;
    }

    JPH::BodyID NextBodyId() {
        for (uint32_t n = 0; n < 65536; ++n) {
            uint32_t serial = bodySerial++;
            JPH::BodyID id(serial % 65536, static_cast<JPH::uint8>(serial / 65536));
            JPH::BodyLockRead lock(system.GetBodyLockInterface(), id);
            if (!lock.Succeeded()) return id;
        }
        return JPH::BodyID();
    }
    JPH::BodyID CreateBody(const JPH::BodyCreationSettings& settings, JPH::EActivation activation) {
        auto& bi = system.GetBodyInterface();
        if (!nativeIds) return bi.CreateAndAddBody(settings, activation);
        auto id = NextBodyId(); if (id.IsInvalid() || !bi.CreateBodyWithID(id, settings)) return JPH::BodyID();
        bi.AddBody(id, activation); return id;
    }

    // Declaration order matters: the filters must outlive the PhysicsSystem.
    BroadPhaseLayers bpLayers;
    ObjectVsBroadPhase objVsBp;
    ObjectPairFilter pairFilter;
    JPH::TempAllocatorImpl temp;
    JPH::JobSystemSingleThreaded jobs;
    JPH::PhysicsSystem system;

    std::map<EntityId, BodyRec> bodies;
    std::map<EntityId, TriggerRec> triggers;
    std::map<EntityId, CharacterRec> characters;
    std::map<EntityId, std::string> emptyTilemaps;  // tilemap -> key of a map without colliding tiles
    std::map<JPH::uint32, EntityId> bodyToEntity;
    std::mutex contactMutex;
    std::set<ContactKey> bodyContacts;
    std::vector<Removal> pendingRemovals;
    std::vector<std::pair<EntityId, Vec3>> pendingImpulses;
    uint64_t steps = 0;
    bool nativeIds = false;
    uint32_t bodySerial = 0;
};

// ----- PhysicsWorld -------------------------------------------------------------

PhysicsWorld::PhysicsWorld() = default;
PhysicsWorld::~PhysicsWorld() = default;

void PhysicsWorld::Reset() {
    impl_.reset();
    world2d_.reset();
    prevCollisions_.clear();
    prevTriggers_.clear();
    warnings_.clear();
}

void PhysicsWorld::SetTilesets(TilesetLookup lookup) { tilesets_ = std::move(lookup); }

Physics2D* PhysicsWorld::World2D(Scene& scene) {
    if (!world2d_ && Physics2D::Wanted(scene)) {
        world2d_ = std::make_unique<Physics2D>(snapshotsEnabled_);
        world2d_->SetTilesets(&tilesets_);
    }
    return world2d_.get();
}

std::vector<PhysicsEvent> PhysicsWorld::Step(Scene& scene, float dt) {
    std::vector<PhysicsEvent> events;
    // Each world exists once the scene has bodies for it.
    if (!impl_ && (!scene.Pool<Collider>().empty() || !scene.Pool<CharacterBody>().empty())) {
        EnsureJolt();
        impl_ = std::make_unique<Impl>(snapshotsEnabled_);
    }
    Physics2D* w2 = World2D(scene);
    if (!impl_ && !w2) return events;
    std::set<EntityPair> collisions, triggers;
    if (impl_) {
        Impl& w = *impl_;
        w.SyncIn(scene, dt, warnings_, &tilesets_);
        w.UpdateCharacters(scene, dt);
        w.system.Update(dt, 1, &w.temp, &w.jobs);
        w.ApplyRemovals();
        w.SyncOut(scene);
        ++w.steps;
        collisions = w.CurrentCollisions();
        triggers = w.CurrentTriggers(scene);
    }
    if (w2) {
        w2->Step(scene, dt, warnings_);
        for (const EntityPair& p : w2->Collisions()) collisions.insert(p);
        for (const EntityPair& p : w2->Triggers(scene)) triggers.insert(p);
    }
    auto diff = [&](const std::set<EntityPair>& now, const std::set<EntityPair>& before, PhysicsEvent::Kind enter, PhysicsEvent::Kind exit) {
        for (const EntityPair& p : before) {
            if (!now.count(p)) events.push_back({exit, p.first, p.second});
        }
        for (const EntityPair& p : now) {
            if (!before.count(p)) events.push_back({enter, p.first, p.second});
        }
    };
    diff(collisions, prevCollisions_, PhysicsEvent::Kind::CollisionEnter, PhysicsEvent::Kind::CollisionExit);
    diff(triggers, prevTriggers_, PhysicsEvent::Kind::TriggerEnter, PhysicsEvent::Kind::TriggerExit);
    prevCollisions_ = std::move(collisions);
    prevTriggers_ = std::move(triggers);
    return events;
}

RaycastHit PhysicsWorld::Raycast(Scene& scene, const Vec3& origin, const Vec3& direction, float maxDistance) {
    RaycastHit out;
    if (!impl_) {
        EnsureJolt();
        impl_ = std::make_unique<Impl>(snapshotsEnabled_);
    }
    impl_->SyncIn(scene, 0.0f, warnings_, &tilesets_);
    Vec3 dir = Normalize(direction);
    if (Length(dir) < 0.5f || maxDistance <= 0) return out;
    JPH::RRayCast ray{J(origin), J(dir * maxDistance)};
    JPH::RayCastResult result;
    if (impl_->system.GetNarrowPhaseQuery().CastRay(ray, result)) {
        out.hit = true;
        out.entity = impl_->EntityOf(result.mBodyID);
        JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
        out.point = O(point);
        out.distance = result.mFraction * maxDistance;
        JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), result.mBodyID);
        if (lock.Succeeded()) out.normal = O(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
    }
    if (Physics2D* w2 = World2D(scene)) {
        w2->Sync(scene, warnings_);
        RaycastHit hit2;
        if (w2->Raycast(origin, dir, maxDistance, hit2) && (!out.hit || hit2.distance < out.distance)) out = hit2;
    }
    return out;
}

std::vector<EntityId> PhysicsWorld::OverlapSphere(Scene& scene, const Vec3& center, float radius) {
    std::vector<EntityId> out;
    if (!impl_) {
        EnsureJolt();
        impl_ = std::make_unique<Impl>(snapshotsEnabled_);
    }
    impl_->SyncIn(scene, 0.0f, warnings_, &tilesets_);
    JPH::RefConst<JPH::Shape> sphere = new JPH::SphereShape(std::max(0.001f, radius));
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    impl_->system.GetNarrowPhaseQuery().CollideShape(sphere, JPH::Vec3::sOne(), JPH::RMat44::sTranslation(J(center)), settings, JPH::RVec3::sZero(), collector);
    std::set<EntityId> ids;
    for (const JPH::CollideShapeResult& hit : collector.mHits) {
        EntityId e = impl_->EntityOf(hit.mBodyID2);
        if (e != kNullEntity) ids.insert(e);
    }
    if (Physics2D* w2 = World2D(scene)) {
        w2->Sync(scene, warnings_);
        for (EntityId e : w2->OverlapCircle(center, radius)) ids.insert(e);
    }
    out.assign(ids.begin(), ids.end());
    return out;
}

std::vector<ContactPair> PhysicsWorld::Contacts() const {
    std::vector<ContactPair> out;
    for (const EntityPair& p : prevCollisions_) out.push_back({p.first, p.second, false});
    for (const EntityPair& p : prevTriggers_) out.push_back({p.first, p.second, true});
    return out;
}

void PhysicsWorld::AddImpulse(EntityId id, const Vec3& impulse) {
    if (impl_) impl_->pendingImpulses.emplace_back(id, impulse);
    if (world2d_) world2d_->AddImpulse(id, impulse);
}

Json PhysicsWorld::Stats() const {
    Json s = Json::MakeObject();
    s["backend"] = "jolt 5.6.0 (3D) + box2d 3.1.1 (2D), cross-platform deterministic";
    s["active"] = impl_ != nullptr || world2d_ != nullptr;
    s["gravity"] = Json(Json::Array{0.0, kGravity, 0.0});
    if (impl_) {
        int statics = 0, dynamics = 0, kinematics = 0;
        for (const auto& kv : impl_->bodies) {
            if (kv.second.kind == Impl::Kind::Static) ++statics;
            else if (kv.second.kind == Impl::Kind::Dynamic) ++dynamics;
            else ++kinematics;
        }
        s["staticBodies"] = statics;
        s["dynamicBodies"] = dynamics;
        s["kinematicBodies"] = kinematics;
        s["characters"] = static_cast<uint64_t>(impl_->characters.size());
        s["triggers"] = static_cast<uint64_t>(impl_->triggers.size());
        s["steps"] = static_cast<uint64_t>(impl_->steps);
    }
    if (world2d_) world2d_->AppendStats(s);
    Json w = Json::MakeArray();
    for (const std::string& msg : warnings_) w.push(msg);
    s["warnings"] = w;
    return s;
}

// ----- Debug wireframes -----------------------------------------------------------

namespace {
void AddCircle(std::vector<DebugLine>& lines, const Mat4& m, Vec3 center, Vec3 u, Vec3 v, float r, Color c) {
    const int n = 24;
    for (int i = 0; i < n; ++i) {
        float a0 = 2 * kPi * static_cast<float>(i) / n, a1 = 2 * kPi * static_cast<float>(i + 1) / n;
        Vec3 p0 = center + u * (std::cos(a0) * r) + v * (std::sin(a0) * r);
        Vec3 p1 = center + u * (std::cos(a1) * r) + v * (std::sin(a1) * r);
        lines.push_back({m.TransformPoint(p0), m.TransformPoint(p1), c});
    }
}

// Unit-scale world matrix (position + rotation) so spheres stay round.
Mat4 RigidWorld(const Scene& scene, EntityId id, Vec3* scaleOut) {
    Mat4 m = scene.WorldMatrix(id);
    Vec3 x(m.at(0, 0), m.at(1, 0), m.at(2, 0)), y(m.at(0, 1), m.at(1, 1), m.at(2, 1)), z(m.at(0, 2), m.at(1, 2), m.at(2, 2));
    Vec3 s(Length(x), Length(y), Length(z));
    *scaleOut = s;
    Mat4 r = m;
    for (int row = 0; row < 3; ++row) {
        r.at(row, 0) = s.x > 1e-6f ? m.at(row, 0) / s.x : 0;
        r.at(row, 1) = s.y > 1e-6f ? m.at(row, 1) / s.y : 0;
        r.at(row, 2) = s.z > 1e-6f ? m.at(row, 2) / s.z : 0;
    }
    return r;
}

void AddCapsule(std::vector<DebugLine>& lines, const Mat4& m, Vec3 c, float r, float half, Color col) {
    Vec3 up(0, half, 0);
    AddCircle(lines, m, c + up, Vec3(1, 0, 0), Vec3(0, 0, 1), r, col);
    AddCircle(lines, m, c - up, Vec3(1, 0, 0), Vec3(0, 0, 1), r, col);
    AddCircle(lines, m, c, Vec3(1, 0, 0), Vec3(0, 1, 0), r, col);
    AddCircle(lines, m, c, Vec3(0, 0, 1), Vec3(0, 1, 0), r, col);
    for (Vec3 d : {Vec3(r, 0, 0), Vec3(-r, 0, 0), Vec3(0, 0, r), Vec3(0, 0, -r)}) {
        lines.push_back({m.TransformPoint(c + up + d), m.TransformPoint(c - up + d), col});
    }
    if (half > 0) {
        AddCircle(lines, m, c + up, Vec3(1, 0, 0), Vec3(0, 1, 0), r, col);
        AddCircle(lines, m, c - up, Vec3(1, 0, 0), Vec3(0, 1, 0), r, col);
    }
}
}  // namespace

void AppendColliderLines(const Scene& scene, std::vector<DebugLine>& lines, const TilesetLookup* tilesets) {
    const Color solid(0.2f, 0.95f, 0.35f), trigger(1.0f, 0.85f, 0.2f), character(0.3f, 0.85f, 1.0f), oneWay(1.0f, 0.55f, 0.15f);
    for (const auto& kv : scene.Pool<CharacterBody>()) {
        Vec3 scale;
        Mat4 m = RigidWorld(scene, kv.first, &scale);
        float r = kv.second.radius;
        float half = kv.second.shape == "sphere" ? 0.0f : std::max(0.01f, 0.5f * kv.second.height - r);
        AddCapsule(lines, m, Vec3(0, 0, 0), r, half, character);
    }
    for (const auto& kv : scene.Pool<Collider>()) {
        if (scene.Get<CharacterBody>(kv.first)) continue;
        const Collider& c = kv.second;
        Color col = c.isTrigger ? trigger : solid;
        Vec3 scale;
        Mat4 m = RigidWorld(scene, kv.first, &scale);
        Vec3 center = Mul(c.center, scale);
        if (c.shape == "sphere") {
            float r = c.radius * std::max(scale.x, std::max(scale.y, scale.z));
            AddCircle(lines, m, center, Vec3(1, 0, 0), Vec3(0, 1, 0), r, col);
            AddCircle(lines, m, center, Vec3(1, 0, 0), Vec3(0, 0, 1), r, col);
            AddCircle(lines, m, center, Vec3(0, 1, 0), Vec3(0, 0, 1), r, col);
        } else if (c.shape == "capsule") {
            float r = c.radius * std::max(scale.x, scale.z);
            AddCapsule(lines, m, center, r, std::max(0.001f, 0.5f * c.height * scale.y - r), col);
        } else {
            Vec3 h = Mul(c.size, scale) * 0.5f;
            Vec3 p[8];
            for (int i = 0; i < 8; ++i) {
                p[i] = m.TransformPoint(center + Vec3(i & 1 ? h.x : -h.x, i & 2 ? h.y : -h.y, i & 4 ? h.z : -h.z));
            }
            const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto& e : edges) lines.push_back({p[e[0]], p[e[1]], col});
        }
    }
    // 2D shapes in the entity's XY plane (scaled like the physics shapes), lifted a
    // little toward +Z so they are not hidden by the sprites/tiles in the same plane.
    const Vec3 lift(0, 0, 0.03f);
    auto poly = [&](const Mat4& m, const std::vector<Vec3>& pts, bool closed, Color col) {
        for (size_t i = 0; i + 1 < pts.size() || (closed && i < pts.size() && pts.size() > 2); ++i) {
            lines.push_back({m.TransformPoint(pts[i]) + lift, m.TransformPoint(pts[(i + 1) % pts.size()]) + lift, col});
        }
    };
    auto circle2 = [&](const Mat4& m, Vec3 c, float r, Color col) {
        size_t first = lines.size();
        AddCircle(lines, m, c, Vec3(1, 0, 0), Vec3(0, 1, 0), r, col);
        for (size_t i = first; i < lines.size(); ++i) lines[i].a = lines[i].a + lift, lines[i].b = lines[i].b + lift;
    };
    auto capsule2 = [&](const Mat4& m, Vec3 c, float r, float half, float angleDeg, Color col) {
        float a = Radians(angleDeg);
        Vec3 axis(-std::sin(a) * half, std::cos(a) * half, 0), side(std::cos(a) * r, std::sin(a) * r, 0);
        circle2(m, c + axis, r, col);
        circle2(m, c - axis, r, col);
        lines.push_back({m.TransformPoint(c + axis + side) + lift, m.TransformPoint(c - axis + side) + lift, col});
        lines.push_back({m.TransformPoint(c + axis - side) + lift, m.TransformPoint(c - axis - side) + lift, col});
    };
    for (const auto& kv : scene.Pool<CharacterBody2D>()) {
        Vec3 scale;
        Mat4 m = RigidWorld(scene, kv.first, &scale);
        float r = std::max(0.02f, kv.second.radius);
        float half = kv.second.shape == "circle" ? 0.0f : std::max(0.0f, 0.5f * kv.second.height - r);
        if (half <= 0.0f) circle2(m, Vec3(), r, character);
        else capsule2(m, Vec3(), r, half, 0.0f, character);
    }
    for (const auto& kv : scene.Pool<Collider2D>()) {
        if (scene.Get<CharacterBody2D>(kv.first)) continue;
        const Collider2D& c = kv.second;
        Color col = c.isTrigger ? trigger : c.oneWay ? oneWay : solid;
        Vec3 scale;
        Mat4 m = RigidWorld(scene, kv.first, &scale);
        Vec3 center(c.center.x * scale.x, c.center.y * scale.y, 0);
        if (c.shape == "circle") {
            circle2(m, center, c.radius * std::max(scale.x, scale.y), col);
        } else if (c.shape == "capsule") {
            float r = c.radius * scale.x;
            float half = 0.5f * c.height * scale.y - r;
            if (half <= 0.001f) circle2(m, center, r, col);
            else capsule2(m, center, r, half, c.angle, col);
        } else if (c.shape == "polygon" || c.shape == "edge") {
            std::vector<Vec3> pts;
            if (c.points.isArray()) {
                for (const Json& p : c.points.items()) {
                    if (p.isArray() && p.size() >= 2) pts.push_back(Vec3((p[0].asFloat() + c.center.x) * scale.x, (p[1].asFloat() + c.center.y) * scale.y, 0));
                }
            }
            poly(m, pts, c.shape == "polygon" || c.loop, col);
        } else {
            float hx = 0.5f * c.size.x * scale.x, hy = 0.5f * c.size.y * scale.y, a = Radians(c.angle);
            Vec3 ux(std::cos(a), std::sin(a), 0), uy(-std::sin(a), std::cos(a), 0);
            poly(m, {center - ux * hx - uy * hy, center + ux * hx - uy * hy, center + ux * hx + uy * hy, center - ux * hx + uy * hy}, true, col);
        }
    }
    // Tilemaps: outlines of the solid regions, one-way platforms and shaped cells in the tile plane.
    for (const auto& kv : scene.Pool<Tilemap>()) {
        const float ts = std::max(0.001f, kv.second.tileSize);
        Mat4 m = scene.WorldMatrix(kv.first);
        TileRules rules = BuildTileRules(kv.second, tilesets);
        for (const auto& loop : SolidOutlines(kv.second, rules)) {
            std::vector<Vec3> pts;
            for (const TilePoint& p : loop) pts.push_back(Vec3(p.x * ts, p.y * ts, 0));
            poly(m, pts, true, solid);
        }
        for (const TileShape& cell : ShapedCells(kv.second, rules)) {
            std::vector<Vec3> pts;
            for (const TilePoint& p : cell.points) pts.push_back(Vec3(p.x * ts, p.y * ts, 0));
            poly(m, pts, true, solid);
        }
        for (const TileRect& r : OneWayRuns(kv.second, rules)) {
            float y = -static_cast<float>(r.row) * ts;
            poly(m, {Vec3(static_cast<float>(r.col) * ts, y, 0), Vec3(static_cast<float>(r.col + r.width) * ts, y, 0)}, false, oneWay);
        }
    }
}

struct PhysicsWorld::Snapshot {
    bool jolt = false;
    std::shared_ptr<const Physics2D::Snapshot> box2d;
    std::string system;
    std::map<EntityId, Impl::BodyRec> bodies;
    std::map<EntityId, JPH::BodyCreationSettings> settings;
    std::map<EntityId, Impl::TriggerRec> triggers;
    std::map<EntityId, Impl::CharacterRec> characters;
    std::map<EntityId, std::string> characterStates, emptyTilemaps;
    std::map<JPH::uint32, EntityId> bodyToEntity;
    std::set<Impl::ContactKey> contacts;
    std::vector<Impl::Removal> removals;
    std::vector<std::pair<EntityId, Vec3>> impulses;
    std::set<std::pair<EntityId, EntityId>> collisions, triggerPairs;
    std::vector<std::string> warnings;
    uint64_t steps = 0; uint32_t serial = 0;
};
std::shared_ptr<const PhysicsWorld::Snapshot> PhysicsWorld::SaveState() const {
    auto s = std::make_shared<Snapshot>();
    s->collisions = prevCollisions_; s->triggerPairs = prevTriggers_; s->warnings = warnings_;
    if (world2d_) s->box2d = world2d_->SaveState();
    if (!impl_) return s;
    const auto& w = *impl_; s->jolt = true;
    JPH::StateRecorderImpl recorder; w.system.SaveState(recorder); s->system = recorder.GetData();
    s->bodies = w.bodies; s->triggers = w.triggers; s->characters = w.characters;
    for (auto& item : s->characters) {
        JPH::StateRecorderImpl character; item.second.ch->SaveState(character);
        s->characterStates[item.first] = character.GetData(); item.second.ch = nullptr;
    }
    for (const auto& item : w.bodies) {
        JPH::BodyLockRead lock(w.system.GetBodyLockInterface(), item.second.id);
        if (lock.Succeeded()) s->settings[item.first] = lock.GetBody().GetBodyCreationSettings();
    }
    s->emptyTilemaps = w.emptyTilemaps; s->bodyToEntity = w.bodyToEntity; s->contacts = w.bodyContacts;
    s->removals = w.pendingRemovals; s->impulses = w.pendingImpulses; s->steps = w.steps; s->serial = w.bodySerial;
    return s;
}
void PhysicsWorld::LoadState(const Snapshot& s) {
    impl_.reset();
    if (s.jolt) {
        EnsureJolt(); impl_ = std::make_unique<Impl>(true); auto& w = *impl_; auto& bi = w.system.GetBodyInterface();
        for (const auto& item : s.settings) {
            auto id = s.bodies.at(item.first).id;
            if (!bi.CreateBodyWithID(id, item.second)) throw std::runtime_error("cannot restore Jolt body id");
            bi.AddBody(id, JPH::EActivation::DontActivate);
        }
        w.characters = s.characters;
        for (auto& item : w.characters) {
            auto& c = item.second;
            c.ch = new JPH::CharacterVirtual(c.settings, c.lastPos, JPH::Quat::sIdentity(), item.first, &w.system);
            JPH::StateRecorderImpl recorder; const auto& bytes = s.characterStates.at(item.first);
            recorder.WriteBytes(bytes.data(), bytes.size()); recorder.Rewind(); c.ch->RestoreState(recorder);
        }
        JPH::StateRecorderImpl recorder; recorder.WriteBytes(s.system.data(), s.system.size()); recorder.Rewind();
        if (!w.system.RestoreState(recorder)) throw std::runtime_error("cannot restore Jolt state");
        w.bodies = s.bodies; w.triggers = s.triggers; w.emptyTilemaps = s.emptyTilemaps;
        w.bodyToEntity = s.bodyToEntity; w.bodyContacts = s.contacts; w.pendingRemovals = s.removals;
        w.pendingImpulses = s.impulses; w.steps = s.steps; w.bodySerial = s.serial;
    }
    if (s.box2d) {
        if (!world2d_) world2d_ = std::make_unique<Physics2D>(true);
        world2d_->LoadState(*s.box2d); world2d_->SetTilesets(&tilesets_);
    } else world2d_.reset();
    prevCollisions_ = s.collisions; prevTriggers_ = s.triggerPairs; warnings_ = s.warnings;
}

}  // namespace oe
