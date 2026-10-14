#include "render/Light2D.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "assets/Assets.h"
#include "render/Mesh.h"
#include "scene/Components.h"
#include "scene/Scene.h"
#include "scene/TileGrid.h"

namespace oe {

namespace {

float Cross2(const Vec3& a, const Vec3& b) { return a.x * b.y - a.y * b.x; }

uint8_t EncodeLight(float value) { return static_cast<uint8_t>(Clamp(value * 0.5f, 0, 1) * 255.0f + 0.5f); }
uint8_t EncodeAxis(float value) { return static_cast<uint8_t>(Clamp(value * 0.5f + 0.5f, 0, 1) * 255.0f + 0.5f); }

void AppendBytes(std::string& key, const void* data, size_t size) { key.append(static_cast<const char*>(data), size); }

bool InsidePolygon(const std::vector<Vec3>& points, const Vec3& p) {
    bool inside = false;
    for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
        const Vec3& a = points[i];
        const Vec3& b = points[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    return inside;
}

bool SolidCell(const Occluders2D::Shape& shape, const Vec3& world) {
    const Vec3 local = shape.toLocal.TransformPoint(world);
    const int col = static_cast<int>(std::floor(local.x)), row = static_cast<int>(std::floor(-local.y));
    if (col < 0 || row < 0 || col >= shape.cols || row >= shape.rows) return false;
    return shape.solid[static_cast<size_t>(row) * static_cast<size_t>(shape.cols) + static_cast<size_t>(col)] != 0;
}

bool Blocked(const Occluders2D& o, const Vec3& light, const Vec3& point, const std::vector<int>* subset, int ignoreShape) {
    const Vec3 ray = point - light;
    auto test = [&](const Occluders2D::Edge& edge) {
        if (edge.shape == ignoreShape) return false;
        const Vec3 segment = edge.b - edge.a;
        const float denom = Cross2(ray, segment);
        if (std::fabs(denom) < 1e-9f) return false;
        const Vec3 delta = edge.a - light;
        const float t = Cross2(delta, segment) / denom, u = Cross2(delta, ray) / denom;
        return t > 1e-4f && t < 0.9999f && u >= 0 && u <= 1;
    };
    if (subset) {
        for (int index : *subset)
            if (test(o.edges[static_cast<size_t>(index)])) return true;
        return false;
    }
    for (const auto& edge : o.edges)
        if (test(edge)) return true;
    return false;
}

struct LightSource {
    Light2D settings;
    Vec3 position;
};

struct CachedMap {
    std::string key;
    std::shared_ptr<const Light2DMap> map;
};
std::mutex g_mapMutex;
std::vector<CachedMap> g_maps;  // a few recent maps (the editor shows several scenes)

}  // namespace

Occluders2D GatherOccluders2D(const Scene& scene, AssetManager* assets) {
    Occluders2D out;
    auto addShape = [&](EntityId id, const std::vector<Vec3>& points, bool closed) {
        Occluders2D::Shape shape;
        shape.id = id;
        if (closed) shape.points = points;
        const int index = static_cast<int>(out.shapes.size());
        const size_t count = closed ? points.size() : (points.empty() ? 0 : points.size() - 1);
        for (size_t i = 0; i < count && out.edges.size() < static_cast<size_t>(kMaxOccluderEdges2D); ++i)
            out.edges.push_back({points[i], points[(i + 1) % points.size()], index});
        out.shapes.push_back(std::move(shape));
    };
    // One pass over the entities: the edge budget goes to the lowest ids, in a stable order.
    for (const auto& entity : scene.Entities()) {
        if (out.edges.size() >= static_cast<size_t>(kMaxOccluderEdges2D)) break;
        const EntityId id = entity.first;
        const Collider2D* c = scene.Get<Collider2D>(id);
        if (c && !c->isTrigger && !scene.Get<RigidBody2D>(id) && !scene.Get<CharacterBody2D>(id)) {
            const Mat4 world = scene.WorldMatrix(id) * Mat4::Translation(c->center) * Mat4::RotationZ(Radians(c->angle));
            std::vector<Vec3> p;
            if (c->shape == "polygon" || c->shape == "edge" || c->shape == "sprite") {
                const SpriteAssets sprites = assets ? assets->SpriteLookup() : SpriteAssets();
                const Json outline = Collider2DOutline(scene, id, *c, &sprites);  // kept alive for the loop
                for (const Json& v : outline.items())
                    if (v.isArray() && v.size() == 2) p.push_back(Vec3(v[0].asFloat(), v[1].asFloat(), 0));
            } else if (c->shape == "circle" || c->shape == "capsule") {
                const float extension = c->shape == "capsule" ? std::max(0.0f, c->height / 2 - c->radius) : 0.0f;
                for (int i = 0; i < 16; ++i) {
                    const float a = static_cast<float>(i) * 2 * kPi / 16;
                    p.push_back(Vec3(std::cos(a) * c->radius, std::sin(a) * c->radius + (std::sin(a) >= 0 ? extension : -extension), 0));
                }
            } else {
                const float x = c->size.x / 2, y = c->size.y / 2;
                p = {Vec3(-x, -y, 0), Vec3(x, -y, 0), Vec3(x, y, 0), Vec3(-x, y, 0)};
            }
            for (Vec3& v : p) v = world.TransformPoint(v);
            const bool closed = c->shape != "edge" || c->loop;
            if (p.size() >= 2) addShape(id, p, closed);
        }
        if (const Tilemap* tm = scene.Get<Tilemap>(id)) {
            TilesetLookup lookup = assets ? assets->Tilesets() : TilesetLookup();
            TileRules rules = BuildTileRules(*tm, &lookup);
            for (auto& rule : rules.tiles)
                if (!rule.second.shadow) rule.second.collision = TileCollision::None;  // water, glass: light passes
            const float ts = std::max(0.001f, tm->tileSize);
            const Mat4 world = scene.WorldMatrix(id) * Mat4::Scale(Vec3(ts, ts, 1));
            Occluders2D::Shape grid;
            grid.id = id;
            grid.tilemap = true;
            grid.toLocal = world.Inverse();
            MapSize(*tm, grid.cols, grid.rows);
            grid.solid.assign(static_cast<size_t>(grid.cols) * static_cast<size_t>(grid.rows), 0);
            for (int row = 0; row < grid.rows; ++row)
                for (int col = 0; col < grid.cols; ++col)
                    grid.solid[static_cast<size_t>(row) * static_cast<size_t>(grid.cols) + static_cast<size_t>(col)] =
                        CollisionAt(*tm, rules, col, row) == TileCollision::Solid ? 1 : 0;
            const int index = static_cast<int>(out.shapes.size());
            out.shapes.push_back(std::move(grid));
            auto edgesOf = [&](const std::vector<Vec3>& points, bool closed) {
                const size_t count = closed ? points.size() : points.size() - 1;
                for (size_t i = 0; i < count && out.edges.size() < static_cast<size_t>(kMaxOccluderEdges2D); ++i)
                    out.edges.push_back({world.TransformPoint(points[i]), world.TransformPoint(points[(i + 1) % points.size()]), index});
            };
            for (const auto& loop : SolidOutlines(*tm, rules)) {
                std::vector<Vec3> p;
                for (const auto& v : loop) p.push_back(Vec3(v.x, v.y, 0));
                if (p.size() >= 2) edgesOf(p, true);
            }
            // Shaped cells and one-way tops are their own shapes (never "inside": thin or partial cells).
            for (const auto& cell : ShapedCells(*tm, rules)) {
                std::vector<Vec3> p;
                for (const auto& v : cell.points) p.push_back(world.TransformPoint(Vec3(v.x, v.y, 0)));
                if (p.size() >= 3) addShape(id, p, true);
            }
            for (const auto& run : OneWayRuns(*tm, rules)) {
                const float y = -static_cast<float>(run.row);
                addShape(id, {world.TransformPoint(Vec3(static_cast<float>(run.col), y, 0)),
                              world.TransformPoint(Vec3(static_cast<float>(run.col + run.width), y, 0))}, false);
            }
        }
    }
    return out;
}

bool Occluded2D(const Occluders2D& o, const Vec3& light, const Vec3& point, const std::vector<int>* edges) {
    Vec3 target = point;
    int ignore = -1;
    for (size_t s = 0; s < o.shapes.size(); ++s) {
        const Occluders2D::Shape& shape = o.shapes[s];
        if (shape.tilemap) {
            if (!SolidCell(shape, target)) continue;
            // Inside a wall: walk back toward the light to where the wall begins and test from there.
            const Vec3 toLight = light - target;
            const float length = std::sqrt(toLight.x * toLight.x + toLight.y * toLight.y);
            if (length < 1e-6f) return false;
            const Vec3 axis = shape.toLocal.Inverse().TransformDir(Vec3(1, 0, 0));
            const float step = std::max(1e-3f, std::sqrt(axis.x * axis.x + axis.y * axis.y) * 0.125f);
            const Vec3 dir = toLight * (1.0f / length);
            float walked = 0;
            while (walked < length && SolidCell(shape, target)) {
                target = target + dir * step;
                walked += step;
            }
            if (walked >= length) return false;  // the light itself is inside the wall
        } else if (shape.points.size() >= 3 && InsidePolygon(shape.points, target)) {
            ignore = static_cast<int>(s);
        }
    }
    return Blocked(o, light, target, edges, ignore);
}

float Visibility2D(const Occluders2D& o, const Vec3& light, float softness, const Vec3& point, const std::vector<int>* edges) {
    const float dx = point.x - light.x, dy = point.y - light.y, length = std::sqrt(dx * dx + dy * dy);
    if (!(softness > 0) || length < 1e-6f) return Occluded2D(o, light, point, edges) ? 0.0f : 1.0f;
    const Vec3 side(-dy / length * softness, dx / length * softness, 0);
    // A part of the light behind a wall (a torch next to it) does not shine: such points are skipped.
    int lit = 0, used = 0;
    auto sample = [&](int k) {
        const Vec3 source = light + side * (static_cast<float>(k) / 3.0f - 1.0f);
        if (k != 3 && Blocked(o, light, source, edges, -1)) return;
        ++used;
        lit += Occluded2D(o, source, point, edges) ? 0 : 1;
    };
    sample(0), sample(3), sample(6);
    if (lit == 0 || lit == used) return static_cast<float>(lit / used);
    sample(1), sample(2), sample(4), sample(5);
    return static_cast<float>(lit) / static_cast<float>(used);
}

std::vector<int> OccluderEdgesNear(const Occluders2D& o, const Vec3& center, float radius) {
    std::vector<int> near;
    for (size_t i = 0; i < o.edges.size(); ++i) {
        const auto& e = o.edges[i];
        if (std::max(e.a.x, e.b.x) < center.x - radius || std::min(e.a.x, e.b.x) > center.x + radius ||
            std::max(e.a.y, e.b.y) < center.y - radius || std::min(e.a.y, e.b.y) > center.y + radius)
            continue;
        near.push_back(static_cast<int>(i));
    }
    return near;
}

std::shared_ptr<const Light2DMap> GetLight2DMap(const Scene& scene, AssetManager* assets) {
    bool used = false;
    for (const auto& kv : scene.Pool<Sprite>()) used = used || (kv.second.light2D && kv.second.visible);
    for (const auto& kv : scene.Pool<Tilemap>()) used = used || (kv.second.light2D && kv.second.visible);
    if (!used) return nullptr;

    std::vector<LightSource> lights;
    std::string key;
    for (const auto& kv : scene.Pool<Light2D>()) {
        const Light2D& light = kv.second;
        if (!light.enabled || light.mode == "darkness" || !(light.radius > 0) || !(light.intensity > 0)) continue;
        if (lights.size() == static_cast<size_t>(kMaxLights2D)) break;
        const Vec3 position = scene.WorldMatrix(kv.first).TransformPoint(Vec3());
        if (!std::isfinite(position.x) || !std::isfinite(position.y)) continue;
        lights.push_back({light, position});
        const float values[] = {position.x, position.y, light.color.r, light.color.g, light.color.b, light.intensity,
                                light.height, light.radius, light.inner, light.shadows ? 1.0f : 0.0f, light.mode == "add" ? 1.0f : 0.0f,
                                light.softness};
        AppendBytes(key, values, sizeof(values));
    }
    if (lights.empty()) return nullptr;
    bool shadows = false;
    for (const auto& light : lights) shadows = shadows || light.settings.shadows;
    const Occluders2D occluders = shadows ? GatherOccluders2D(scene, assets) : Occluders2D();
    for (const auto& edge : occluders.edges) AppendBytes(key, &edge, sizeof(Vec3) * 2);
    for (const auto& shape : occluders.shapes)
        if (shape.tilemap) AppendBytes(key, shape.solid.data(), shape.solid.size());

    std::lock_guard<std::mutex> lock(g_mapMutex);
    for (const auto& cached : g_maps)
        if (cached.key == key) return cached.map;

    // The area the lights reach, one texel of margin so the border stays dark.
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (const auto& light : lights) {
        x0 = std::min(x0, light.position.x - light.settings.radius);
        y0 = std::min(y0, light.position.y - light.settings.radius);
        x1 = std::max(x1, light.position.x + light.settings.radius);
        y1 = std::max(y1, light.position.y + light.settings.radius);
    }
    const float cell = std::max({(x1 - x0) / (kMaxLightMap2D - 2), (y1 - y0) / (kMaxLightMap2D - 2), 1.0f / 16.0f});
    auto map = std::make_shared<Light2DMap>();
    map->cellW = map->cellH = cell;
    map->x0 = x0 - cell;
    map->y0 = y0 - cell;
    map->cols = std::min(kMaxLightMap2D, static_cast<int>(std::ceil((x1 - x0) / cell)) + 2);
    map->rows = std::min(kMaxLightMap2D, static_cast<int>(std::ceil((y1 - y0) / cell)) + 2);
    const size_t texels = static_cast<size_t>(map->cols) * static_cast<size_t>(map->rows);
    std::vector<Color> colored(texels, Color(0, 0, 0)), added(texels, Color(0, 0, 0));
    std::vector<Vec3> direction(texels, Vec3(0, 0, 0));
    for (const auto& light : lights) {
        const Light2D& s = light.settings;
        // Only the edges near this light can block it.
        const float softness = Clamp(s.softness, 0, 10);
        const std::vector<int> near = s.shadows ? OccluderEdgesNear(occluders, light.position, s.radius + softness) : std::vector<int>();
        const int c0 = std::max(0, static_cast<int>(std::floor((light.position.x - s.radius - map->x0) / cell)));
        const int c1 = std::min(map->cols - 1, static_cast<int>(std::ceil((light.position.x + s.radius - map->x0) / cell)));
        const int r0 = std::max(0, static_cast<int>(std::floor((light.position.y - s.radius - map->y0) / cell)));
        const int r1 = std::min(map->rows - 1, static_cast<int>(std::ceil((light.position.y + s.radius - map->y0) / cell)));
        const float inner = Clamp(s.inner, 0, 0.99f);
        const bool add = s.mode == "add";
        for (int row = r0; row <= r1; ++row) {
            for (int col = c0; col <= c1; ++col) {
                const Vec3 p(map->x0 + (static_cast<float>(col) + 0.5f) * cell, map->y0 + (static_cast<float>(row) + 0.5f) * cell, 0);
                const float dx = light.position.x - p.x, dy = light.position.y - p.y;
                const float dist = std::sqrt(dx * dx + dy * dy);
                if (dist >= s.radius) continue;
                float falloff = Clamp((1 - dist / s.radius) / (1 - inner), 0, 1) * s.intensity;
                if (falloff <= 0) continue;
                if (s.shadows) falloff *= Visibility2D(occluders, light.position, softness, p, &near);
                if (falloff <= 0) continue;
                const Vec3 l = Normalize(Vec3(dx, dy, std::max(0.01f, s.height)));
                const Color radiance = s.color * falloff;
                const size_t i = static_cast<size_t>(row) * static_cast<size_t>(map->cols) + static_cast<size_t>(col);
                if (add) added[i] = added[i] + radiance * l.z;
                else colored[i] = colored[i] + radiance * l.z;
                direction[i] = direction[i] + l * (radiance.r * 0.2126f + radiance.g * 0.7152f + radiance.b * 0.0722f);
            }
        }
    }
    auto texture = std::make_shared<Texture>();
    texture->width = map->cols;
    texture->height = map->rows * 2;
    texture->texels.resize(texels * 2);
    for (size_t i = 0; i < texels; ++i) {
        const float length = Length(direction[i]);
        const Vec3 d = length > 1e-6f ? direction[i] * (1.0f / length) : Vec3(0, 0, 1);
        const Color& c = colored[i];
        const Color& a = added[i];
        texture->texels[i] = EncodeLight(c.r) | (EncodeLight(c.g) << 8) | (EncodeLight(c.b) << 16) | (static_cast<uint32_t>(EncodeAxis(d.x)) << 24);
        texture->texels[texels + i] =
            EncodeLight(a.r) | (EncodeLight(a.g) << 8) | (EncodeLight(a.b) << 16) | (static_cast<uint32_t>(EncodeAxis(d.y)) << 24);
    }
    map->texture = texture;
    if (g_maps.size() >= 4) g_maps.erase(g_maps.begin());
    g_maps.push_back({key, map});
    return map;
}

Color ShadeLight2D(const Light2DMap* map, const Vec3& wpos, const Vec3& normal, bool normalMapped, const Color& base, const Color& ambient) {
    if (!map || !map->texture) return base * ambient;
    // Bilinear between texel centers, clamped at the edges (texelFetch in mesh_fs).
    const float gx = Clamp((wpos.x - map->x0) / map->cellW - 0.5f, 0, static_cast<float>(map->cols - 1));
    const float gy = Clamp((wpos.y - map->y0) / map->cellH - 0.5f, 0, static_cast<float>(map->rows - 1));
    const int ix = std::min(static_cast<int>(gx), map->cols - 2 < 0 ? 0 : map->cols - 2);
    const int iy = std::min(static_cast<int>(gy), map->rows - 2 < 0 ? 0 : map->rows - 2);
    const float fx = map->cols > 1 ? gx - static_cast<float>(ix) : 0.0f, fy = map->rows > 1 ? gy - static_cast<float>(iy) : 0.0f;
    const int jx = std::min(ix + 1, map->cols - 1), jy = std::min(iy + 1, map->rows - 1);
    auto fetch = [&](int x, int y, int half) {
        const uint32_t t = map->texture->texels[static_cast<size_t>(y + half * map->rows) * static_cast<size_t>(map->cols) + static_cast<size_t>(x)];
        return Vec4(static_cast<float>(t & 255) / 255.0f, static_cast<float>((t >> 8) & 255) / 255.0f, static_cast<float>((t >> 16) & 255) / 255.0f,
                    static_cast<float>(t >> 24) / 255.0f);
    };
    auto sample = [&](int half) {
        const Vec4 a = fetch(ix, iy, half), b = fetch(jx, iy, half), c = fetch(ix, jy, half), d = fetch(jx, jy, half);
        const Vec4 bottom = a * (1 - fx) + b * fx, top = c * (1 - fx) + d * fx;
        return bottom * (1 - fy) + top * fy;
    };
    const Vec4 colored = sample(0), added = sample(1);
    const float dx = colored.w * 2 - 1, dy = added.w * 2 - 1;
    const Vec3 dir(dx, dy, std::sqrt(std::max(0.0f, 1 - dx * dx - dy * dy)));
    const float factor = normalMapped ? Clamp(Dot(normal, dir) / std::max(dir.z, 0.05f), 0, 4) : 1.0f;
    const Color light(colored.x * 2, colored.y * 2, colored.z * 2), glow(added.x * 2, added.y * 2, added.z * 2);
    return base * (ambient + light * factor) + glow * factor;
}

}  // namespace oe
