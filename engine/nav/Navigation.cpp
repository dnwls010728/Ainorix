#include "nav/Navigation.h"

#include <algorithm>
#include <cmath>

#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

namespace oe {

namespace {

constexpr int kTileCells = 64;        // cells along a tile's side
constexpr int kMaxTiles = 1 << 14;    // Detour's 32-bit polygon ids leave 22 bits for tile + polygon
constexpr int kMaxPathPolys = 1024;
constexpr unsigned short kWalkFlag = 1;

struct HeightfieldFree { void operator()(rcHeightfield* p) const { rcFreeHeightField(p); } };
struct CompactFree { void operator()(rcCompactHeightfield* p) const { rcFreeCompactHeightfield(p); } };
struct ContoursFree { void operator()(rcContourSet* p) const { rcFreeContourSet(p); } };
struct PolyMeshFree { void operator()(rcPolyMesh* p) const { rcFreePolyMesh(p); } };
struct DetailFree { void operator()(rcPolyMeshDetail* p) const { rcFreePolyMeshDetail(p); } };

int BitsFor(int count) {
    int bits = 1;
    while ((1 << bits) < count) ++bits;
    return bits;
}

Vec3 ToVec3(const float* v) { return Vec3(v[0], v[1], v[2]); }

}  // namespace

struct Navigation::Impl {
    dtNavMesh* mesh = nullptr;
    dtNavMeshQuery* query = nullptr;
    dtQueryFilter filter;
    // Recast lifts the surface it hands over by one height cell; results are given without that.
    float bias = 0.0f;

    Impl() {
        filter.setIncludeFlags(kWalkFlag);
        filter.setExcludeFlags(0);
    }
    ~Impl() { Free(); }
    void Free() {
        dtFreeNavMeshQuery(query);
        dtFreeNavMesh(mesh);
        query = nullptr;
        mesh = nullptr;
    }
    // The polygon nearest to `point` and the point on it.
    bool Snap(const Vec3& point, float range, dtPolyRef* ref, float* snapped) const {
        if (!query) return false;
        const float at[3] = {point.x, point.y, point.z};
        const float extents[3] = {range, range * 2.0f, range};
        *ref = 0;
        return dtStatusSucceed(query->findNearestPoly(at, extents, &filter, ref, snapped)) && *ref != 0;
    }
};

Navigation::Navigation() : impl_(std::make_unique<Impl>()) {}
Navigation::~Navigation() = default;

bool Navigation::Baked() const { return impl_->query != nullptr; }

void Navigation::Clear() {
    impl_->Free();
    stats_ = Json::MakeObject();
}

bool Navigation::Build(const NavGeometry& geometry, const NavSettings& settings, std::string* error) {
    Clear();
    settings_ = settings;
    auto fail = [&](const std::string& message) {
        Clear();
        if (error) *error = message;
        return false;
    };
    const int vertexCount = static_cast<int>(geometry.vertices.size() / 3);
    const int triangleCount = static_cast<int>(geometry.triangles.size() / 3);
    if (!vertexCount || !triangleCount) return fail("no geometry to walk on");

    float bmin[3], bmax[3];
    rcCalcBounds(geometry.vertices.data(), vertexCount, bmin, bmax);
    // Heights are kept in a quarter of a cell, coarser only where a world is taller than Recast's 13-bit spans reach.
    const float cs = settings.cellSize, ch = std::max(settings.cellSize * 0.25f, (bmax[1] - bmin[1]) / 8000.0f);
    impl_->bias = ch;
    int gridWidth = 0, gridHeight = 0;
    rcCalcGridSize(bmin, bmax, cs, &gridWidth, &gridHeight);
    const int tilesX = std::max(1, (gridWidth + kTileCells - 1) / kTileCells), tilesZ = std::max(1, (gridHeight + kTileCells - 1) / kTileCells);
    if (static_cast<long long>(tilesX) * tilesZ > kMaxTiles)
        return fail("the geometry spans " + std::to_string(gridWidth) + " x " + std::to_string(gridHeight) + " cells, more than a navigation mesh holds");

    rcConfig base{};
    base.cs = cs;
    base.ch = ch;
    base.walkableSlopeAngle = settings.maxSlope;
    base.walkableHeight = static_cast<int>(std::ceil(settings.agentHeight / ch));
    base.walkableClimb = static_cast<int>(std::floor(settings.stepHeight / ch));
    base.walkableRadius = static_cast<int>(std::ceil(settings.agentRadius / cs));
    base.maxEdgeLen = static_cast<int>(12.0f / cs);
    base.maxSimplificationError = 1.3f;
    base.minRegionArea = static_cast<int>(settings.minRegion / (cs * cs));
    base.mergeRegionArea = 400;
    base.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
    base.tileSize = kTileCells;
    base.borderSize = base.walkableRadius + 3;  // room for the erosion to see the neighbouring tiles
    base.width = base.height = kTileCells + base.borderSize * 2;
    base.detailSampleDist = cs * 6.0f;
    base.detailSampleMaxError = ch;
    const float tileWorld = static_cast<float>(kTileCells) * cs, border = static_cast<float>(base.borderSize) * cs;

    impl_->mesh = dtAllocNavMesh();
    dtNavMeshParams params{};
    rcVcopy(params.orig, bmin);
    params.tileWidth = params.tileHeight = tileWorld;
    params.maxTiles = tilesX * tilesZ;
    params.maxPolys = 1 << (22 - BitsFor(params.maxTiles));
    if (!impl_->mesh || dtStatusFailed(impl_->mesh->init(&params))) return fail("out of memory for the navigation mesh");

    // Triangles by the tiles their bounds touch (with the border), in triangle order.
    std::vector<std::vector<int>> bins(static_cast<size_t>(tilesX) * static_cast<size_t>(tilesZ));
    for (int t = 0; t < triangleCount; ++t) {
        float low[2] = {1e30f, 1e30f}, high[2] = {-1e30f, -1e30f};
        for (int k = 0; k < 3; ++k) {
            const float* v = &geometry.vertices[static_cast<size_t>(geometry.triangles[static_cast<size_t>(t * 3 + k)]) * 3];
            low[0] = std::min(low[0], v[0]); high[0] = std::max(high[0], v[0]);
            low[1] = std::min(low[1], v[2]); high[1] = std::max(high[1], v[2]);
        }
        const int x0 = std::max(0, static_cast<int>(std::floor((low[0] - border - bmin[0]) / tileWorld)));
        const int x1 = std::min(tilesX - 1, static_cast<int>(std::floor((high[0] + border - bmin[0]) / tileWorld)));
        const int z0 = std::max(0, static_cast<int>(std::floor((low[1] - border - bmin[2]) / tileWorld)));
        const int z1 = std::min(tilesZ - 1, static_cast<int>(std::floor((high[1] + border - bmin[2]) / tileWorld)));
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) bins[static_cast<size_t>(z * tilesX + x)].push_back(t);
    }

    rcContext context(false);
    std::vector<int> triangles;
    std::vector<unsigned char> areas;
    for (int tz = 0; tz < tilesZ; ++tz) {
        for (int tx = 0; tx < tilesX; ++tx) {
            const std::vector<int>& bin = bins[static_cast<size_t>(tz * tilesX + tx)];
            if (bin.empty()) continue;
            rcConfig cfg = base;
            cfg.bmin[0] = bmin[0] + static_cast<float>(tx) * tileWorld - border;
            cfg.bmin[1] = bmin[1];
            cfg.bmin[2] = bmin[2] + static_cast<float>(tz) * tileWorld - border;
            cfg.bmax[0] = bmin[0] + static_cast<float>(tx + 1) * tileWorld + border;
            cfg.bmax[1] = bmax[1];
            cfg.bmax[2] = bmin[2] + static_cast<float>(tz + 1) * tileWorld + border;

            std::unique_ptr<rcHeightfield, HeightfieldFree> solid(rcAllocHeightfield());
            if (!solid || !rcCreateHeightfield(&context, *solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
                return fail("out of memory while baking");
            const int count = static_cast<int>(bin.size());
            triangles.resize(bin.size() * 3);
            areas.assign(bin.size(), 0);
            for (size_t i = 0; i < bin.size(); ++i)
                for (size_t k = 0; k < 3; ++k) triangles[i * 3 + k] = geometry.triangles[static_cast<size_t>(bin[i]) * 3 + k];
            rcMarkWalkableTriangles(&context, cfg.walkableSlopeAngle, geometry.vertices.data(), vertexCount, triangles.data(), count, areas.data());
            for (size_t i = 0; i < bin.size(); ++i)
                if (!geometry.walkable[static_cast<size_t>(bin[i])]) areas[i] = RC_NULL_AREA;
            if (!rcRasterizeTriangles(&context, geometry.vertices.data(), vertexCount, triangles.data(), areas.data(), count, *solid, cfg.walkableClimb))
                return fail("out of memory while baking");
            rcFilterLowHangingWalkableObstacles(&context, cfg.walkableClimb, *solid);
            rcFilterLedgeSpans(&context, cfg.walkableHeight, cfg.walkableClimb, *solid);
            rcFilterWalkableLowHeightSpans(&context, cfg.walkableHeight, *solid);

            std::unique_ptr<rcCompactHeightfield, CompactFree> compact(rcAllocCompactHeightfield());
            if (!compact || !rcBuildCompactHeightfield(&context, cfg.walkableHeight, cfg.walkableClimb, *solid, *compact))
                return fail("out of memory while baking");
            solid.reset();
            if (!rcErodeWalkableArea(&context, cfg.walkableRadius, *compact) || !rcBuildDistanceField(&context, *compact) ||
                !rcBuildRegions(&context, *compact, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea))
                return fail("out of memory while baking");

            std::unique_ptr<rcContourSet, ContoursFree> contours(rcAllocContourSet());
            if (!contours || !rcBuildContours(&context, *compact, cfg.maxSimplificationError, cfg.maxEdgeLen, *contours))
                return fail("out of memory while baking");
            if (!contours->nconts) continue;
            std::unique_ptr<rcPolyMesh, PolyMeshFree> polys(rcAllocPolyMesh());
            if (!polys || !rcBuildPolyMesh(&context, *contours, cfg.maxVertsPerPoly, *polys)) return fail("out of memory while baking");
            std::unique_ptr<rcPolyMeshDetail, DetailFree> detail(rcAllocPolyMeshDetail());
            if (!detail || !rcBuildPolyMeshDetail(&context, *polys, *compact, cfg.detailSampleDist, cfg.detailSampleMaxError, *detail))
                return fail("out of memory while baking");
            if (!polys->npolys) continue;
            if (polys->nverts >= 0xffff) return fail("a tile of the navigation mesh has too many vertices; raise NavMesh.cellSize");
            for (int i = 0; i < polys->npolys; ++i) polys->flags[i] = polys->areas[i] == RC_WALKABLE_AREA ? kWalkFlag : 0;

            dtNavMeshCreateParams create{};
            create.verts = polys->verts;
            create.vertCount = polys->nverts;
            create.polys = polys->polys;
            create.polyAreas = polys->areas;
            create.polyFlags = polys->flags;
            create.polyCount = polys->npolys;
            create.nvp = polys->nvp;
            create.detailMeshes = detail->meshes;
            create.detailVerts = detail->verts;
            create.detailVertsCount = detail->nverts;
            create.detailTris = detail->tris;
            create.detailTriCount = detail->ntris;
            create.walkableHeight = settings.agentHeight;
            create.walkableRadius = settings.agentRadius;
            create.walkableClimb = settings.stepHeight;
            create.tileX = tx;
            create.tileY = tz;
            create.tileLayer = 0;
            rcVcopy(create.bmin, polys->bmin);
            rcVcopy(create.bmax, polys->bmax);
            create.cs = cfg.cs;
            create.ch = cfg.ch;
            create.buildBvTree = true;
            unsigned char* data = nullptr;
            int size = 0;
            if (!dtCreateNavMeshData(&create, &data, &size)) continue;
            if (dtStatusFailed(impl_->mesh->addTile(data, size, DT_TILE_FREE_DATA, 0, nullptr))) dtFree(data);
        }
    }

    // What came out, for callers to check a bake by numbers.
    uint64_t polygonCount = 0, vertexTotal = 0, tileCount = 0;
    double area = 0.0;
    Vec3 low(1e30f, 1e30f, 1e30f), high(-1e30f, -1e30f, -1e30f);
    const dtNavMesh* mesh = impl_->mesh;
    for (int i = 0; i < mesh->getMaxTiles(); ++i) {
        const dtMeshTile* tile = mesh->getTile(i);
        if (!tile || !tile->header) continue;
        ++tileCount;
        vertexTotal += static_cast<uint64_t>(tile->header->vertCount);
        for (int p = 0; p < tile->header->polyCount; ++p) {
            const dtPoly& poly = tile->polys[p];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;
            ++polygonCount;
            const Vec3 first = ToVec3(&tile->verts[poly.verts[0] * 3]);
            for (int k = 0; k < poly.vertCount; ++k) {
                const Vec3 v = ToVec3(&tile->verts[poly.verts[k] * 3]);
                low = Vec3(std::min(low.x, v.x), std::min(low.y, v.y), std::min(low.z, v.z));
                high = Vec3(std::max(high.x, v.x), std::max(high.y, v.y), std::max(high.z, v.z));
                if (k >= 2) {
                    const Vec3 a = ToVec3(&tile->verts[poly.verts[k - 1] * 3]) - first, b = v - first;
                    area += 0.5 * static_cast<double>(std::fabs(a.z * b.x - a.x * b.z));  // seen from above
                }
            }
        }
    }
    if (!polygonCount) return fail("nothing is walkable for an agent of this size");

    impl_->query = dtAllocNavMeshQuery();
    if (!impl_->query || dtStatusFailed(impl_->query->init(impl_->mesh, 4096))) return fail("out of memory for navigation queries");
    stats_["polygons"] = polygonCount;
    stats_["vertices"] = vertexTotal;
    stats_["tiles"] = tileCount;
    stats_["area"] = std::round(area * 100.0) / 100.0;
    stats_["boundsMin"] = Json(Json::Array{low.x, low.y - ch, low.z});
    stats_["boundsMax"] = Json(Json::Array{high.x, high.y - ch, high.z});
    return true;
}

NavPoint Navigation::Nearest(const Vec3& point, float range) const {
    NavPoint out;
    dtPolyRef ref = 0;
    float snapped[3];
    if (!impl_->Snap(point, range, &ref, snapped)) return out;
    out.found = true;
    out.point = ToVec3(snapped) - Vec3(0, impl_->bias, 0);
    return out;
}

NavPath Navigation::FindPath(const Vec3& from, const Vec3& to, float range) const {
    NavPath out;
    dtPolyRef startRef = 0, endRef = 0;
    float start[3], end[3];
    if (!impl_->Snap(from, range, &startRef, start) || !impl_->Snap(to, range, &endRef, end)) return out;
    std::vector<dtPolyRef> polys(kMaxPathPolys);
    int polyCount = 0;
    const dtStatus status = impl_->query->findPath(startRef, endRef, start, end, &impl_->filter, polys.data(), &polyCount, kMaxPathPolys);
    if (dtStatusFailed(status) || polyCount <= 0) return out;
    out.found = true;
    if (polys[static_cast<size_t>(polyCount - 1)] != endRef) {
        // Unreachable (another island) or further than the search looks: stop where the corridor ends.
        out.partial = true;
        float closest[3];
        if (dtStatusSucceed(impl_->query->closestPointOnPoly(polys[static_cast<size_t>(polyCount - 1)], end, closest, nullptr))) dtVcopy(end, closest);
    }
    std::vector<float> corners(static_cast<size_t>(kMaxPathPolys) * 3);
    std::vector<unsigned char> flags(kMaxPathPolys);
    std::vector<dtPolyRef> refs(kMaxPathPolys);
    int cornerCount = 0;
    impl_->query->findStraightPath(start, end, polys.data(), polyCount, corners.data(), flags.data(), refs.data(), &cornerCount, kMaxPathPolys, 0);
    for (int i = 0; i < cornerCount; ++i) {
        const Vec3 p = ToVec3(&corners[static_cast<size_t>(i) * 3]) - Vec3(0, impl_->bias, 0);
        if (!out.points.empty()) out.length += Length(p - out.points.back());
        out.points.push_back(p);
    }
    if (out.points.empty()) out.points.push_back(ToVec3(start) - Vec3(0, impl_->bias, 0));
    return out;
}

NavHit Navigation::Raycast(const Vec3& from, const Vec3& to, float range) const {
    NavHit out;
    dtPolyRef startRef = 0;
    float start[3];
    if (!impl_->Snap(from, range, &startRef, start)) return out;
    out.valid = true;
    const float end[3] = {to.x, to.y, to.z};
    float t = 0.0f, normal[3] = {0, 0, 0};
    std::vector<dtPolyRef> polys(kMaxPathPolys);
    int polyCount = 0;
    if (dtStatusFailed(impl_->query->raycast(startRef, start, end, &impl_->filter, &t, normal, polys.data(), &polyCount, kMaxPathPolys))) {
        out.valid = false;
        return out;
    }
    out.hit = t <= 1.0f;  // FLT_MAX when the end was reached
    const float along = out.hit ? t : 1.0f;
    float point[3] = {start[0] + (end[0] - start[0]) * along, start[1] + (end[1] - start[1]) * along, start[2] + (end[2] - start[2]) * along};
    float height = point[1];
    if (polyCount > 0 && dtStatusSucceed(impl_->query->getPolyHeight(polys[static_cast<size_t>(polyCount - 1)], point, &height))) point[1] = height;
    out.point = ToVec3(point) - Vec3(0, impl_->bias, 0);
    if (out.hit) out.normal = ToVec3(normal);
    out.distance = Length(Vec3(point[0] - start[0], 0, point[2] - start[2]));
    return out;
}

void Navigation::AppendLines(std::vector<DebugLine>& lines) const {
    if (!impl_->mesh) return;
    const Color edge(0.2f, 0.75f, 1.0f), inner(0.1f, 0.38f, 0.6f);
    const Vec3 lift(0, 0.05f - impl_->bias, 0);  // just above the ground it lies on
    const dtNavMesh* mesh = impl_->mesh;
    for (int i = 0; i < mesh->getMaxTiles(); ++i) {
        const dtMeshTile* tile = mesh->getTile(i);
        if (!tile || !tile->header) continue;
        for (int p = 0; p < tile->header->polyCount; ++p) {
            const dtPoly& poly = tile->polys[p];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;
            for (int k = 0; k < poly.vertCount; ++k) {
                const Vec3 a = ToVec3(&tile->verts[poly.verts[k] * 3]), b = ToVec3(&tile->verts[poly.verts[(k + 1) % poly.vertCount] * 3]);
                lines.push_back({a + lift, b + lift, poly.neis[k] ? inner : edge});
            }
        }
    }
}

}  // namespace oe
