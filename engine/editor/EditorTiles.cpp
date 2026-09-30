// Tile painting: the Tiles panel (palette of the selected Tilemap's tile
// characters, drawn from its tileset) and the brush in the Scene view. Edits
// go through tilemap.paint / tilemap.fill, one undo step per stroke.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "editor/EditorInternal.h"
#include "editor/EditorText.h"

#include "app/Engine.h"
#include "assets/Assets.h"
#include "core/Log.h"
#include "render/GpuRenderer.h"
#include "scene/Components.h"

namespace oe {

namespace {

// Cells on the line between two cells (Bresenham), so fast drags leave no gaps.
std::vector<std::pair<int, int>> CellLine(int c0, int r0, int c1, int r1) {
    std::vector<std::pair<int, int>> out;
    int dc = std::abs(c1 - c0), dr = -std::abs(r1 - r0);
    int sc = c0 < c1 ? 1 : -1, sr = r0 < r1 ? 1 : -1;
    int err = dc + dr;
    for (int guard = 0; guard < 10000; ++guard) {
        out.push_back({c0, r0});
        if (c0 == c1 && r0 == r1) break;
        int e2 = 2 * err;
        if (e2 >= dr) err += dr, c0 += sc;
        if (e2 <= dc) err += dc, r0 += sr;
    }
    return out;
}

}  // namespace

bool NativeEditor::Impl::TilePainting() const {
    if (!tilePaint) return false;
    const EntityRow* row = Row(Primary());
    return row && row->Has("Tilemap");
}

const Json& NativeEditor::Impl::TileInfo() {
    EntityId id = Primary();
    if (id != tileInfoId || engine.Revision() != tileInfoRevision) {
        tileInfoId = id;
        tileInfoRevision = engine.Revision();
        const EntityRow* row = Row(id);
        tileInfo = Json();
        if (row && row->Has("Tilemap")) {
            Json r = Call("tilemap.info", ObjectOf({{"id", Json(id)}}), true);
            if (Ok(r)) tileInfo = r["result"];
        }
    }
    return tileInfo;
}

bool NativeEditor::Impl::TileCellAt(float x, float y, int& col, int& row) const {
    EntityId id = Primary();
    Scene& scene = engine.GetScene();
    const Tilemap* tm = scene.Get<Tilemap>(id);
    if (!tm) return false;
    Mat4 world = scene.WorldMatrix(id);
    Vec3 origin, dir, hit;
    ScreenRay(sceneViewMat, sceneProjMat, x, y, sceneImageSize.x, sceneImageSize.y, origin, dir);
    Vec3 normal = Normalize(world.TransformDir(Vec3(0, 0, 1)));
    if (!RayPlane(origin, dir, world.TransformPoint(Vec3(0, 0, 0)), normal, hit)) return false;
    Vec3 local = world.Inverse().TransformPoint(hit);
    float ts = std::max(0.001f, tm->tileSize);
    col = static_cast<int>(std::floor(local.x / ts));
    row = static_cast<int>(std::floor(-local.y / ts));
    return true;
}

void NativeEditor::Impl::PaintCells(const std::vector<std::pair<int, int>>& cells, char c) {
    Json list = Json::MakeArray();
    for (const auto& cell : cells) {
        if (cell.first < 0 || cell.second < 0) continue;
        list.push(Json(Json::Array{cell.first, cell.second}));
    }
    if (list.size() == 0) return;
    Call("tilemap.paint", ObjectOf({{"id", Json(Primary())}, {"char", Json(std::string(1, c))}, {"cells", list},
                                    {"merge", Json("tiles:" + std::to_string(tileSerial))}}));
}

void NativeEditor::Impl::TileSceneInput(ImDrawList* dl, bool clickedLeft, bool clickedRight) {
    ImGuiIO& io = ImGui::GetIO();
    const float mx = io.MousePos.x - sceneImagePos.x, my = io.MousePos.y - sceneImagePos.y;
    int col = 0, row = 0;
    tileHover = sceneHovered && TileCellAt(mx, my, col, row);
    if (tileHover) hoverCol = col, hoverRow = row;

    // Start a stroke: left paints (Shift: rectangle, Ctrl: pick the tile under the cursor), right erases.
    if (tileHover && clickedLeft && io.KeyCtrl) {
        const Json& map = selected["components"]["Tilemap"]["map"];
        if (row >= 0 && col >= 0 && row < static_cast<int>(map.size())) {
            const std::string& line = map[row].asString();
            if (col < static_cast<int>(line.size())) tileBrush = line[static_cast<size_t>(col)];
        }
    } else if (tileHover && (clickedLeft || clickedRight)) {
        ++tileSerial;
        strokeCol = col;
        strokeRow = row;
        if (clickedLeft && io.KeyShift) {
            tileStroke = TileStroke::Rect;
        } else {
            tileStroke = clickedLeft ? TileStroke::Paint : TileStroke::Erase;
            PaintCells({{col, row}}, tileStroke == TileStroke::Paint ? tileBrush : ' ');
        }
    }
    if (tileStroke == TileStroke::Paint || tileStroke == TileStroke::Erase) {
        bool held = io.MouseDown[tileStroke == TileStroke::Paint ? ImGuiMouseButton_Left : ImGuiMouseButton_Right];
        if (!held) {
            tileStroke = TileStroke::None;
        } else if (tileHover && (col != strokeCol || row != strokeRow)) {
            PaintCells(CellLine(strokeCol, strokeRow, col, row), tileStroke == TileStroke::Paint ? tileBrush : ' ');
            strokeCol = col;
            strokeRow = row;
        }
    } else if (tileStroke == TileStroke::Rect && !io.MouseDown[ImGuiMouseButton_Left]) {
        tileStroke = TileStroke::None;
        int c0 = std::max(0, std::min(strokeCol, hoverCol)), r0 = std::max(0, std::min(strokeRow, hoverRow));
        int c1 = std::max(strokeCol, hoverCol), r1 = std::max(strokeRow, hoverRow);
        if (c1 >= 0 && r1 >= 0) {
            Call("tilemap.fill", ObjectOf({{"id", Json(Primary())}, {"char", Json(std::string(1, tileBrush))}, {"col", Json(c0)}, {"row", Json(r0)},
                                           {"width", Json(c1 - c0 + 1)}, {"height", Json(r1 - r0 + 1)}, {"merge", Json("tiles:" + std::to_string(tileSerial))}}));
        }
    }

    // Outline of the cell (or rectangle) the brush covers.
    if (!tileHover && tileStroke != TileStroke::Rect) return;
    Scene& scene = engine.GetScene();
    const Tilemap* tm = scene.Get<Tilemap>(Primary());
    if (!tm) return;
    float ts = std::max(0.001f, tm->tileSize);
    int c0 = hoverCol, r0 = hoverRow, c1 = hoverCol, r1 = hoverRow;
    if (tileStroke == TileStroke::Rect) {
        c0 = std::min(strokeCol, hoverCol), c1 = std::max(strokeCol, hoverCol);
        r0 = std::min(strokeRow, hoverRow), r1 = std::max(strokeRow, hoverRow);
    }
    Mat4 vp = sceneProjMat * sceneViewMat;
    Mat4 world = scene.WorldMatrix(Primary());
    auto corner = [&](int c, int r) {
        Vec4 p = vp * Vec4(world.TransformPoint(Vec3(static_cast<float>(c) * ts, -static_cast<float>(r) * ts, 0)), 1.0f);
        float w = std::max(1e-4f, p.w);
        return ImVec2(sceneImagePos.x + (p.x / w * 0.5f + 0.5f) * sceneImageSize.x, sceneImagePos.y + (0.5f - p.y / w * 0.5f) * sceneImageSize.y);
    };
    ImVec2 q[4] = {corner(c0, r0), corner(c1 + 1, r0), corner(c1 + 1, r1 + 1), corner(c0, r1 + 1)};
    bool erase = tileStroke == TileStroke::Erase || (tileStroke == TileStroke::None && io.MouseDown[ImGuiMouseButton_Right]);
    ImU32 col32 = (c0 < 0 || r0 < 0) ? IM_COL32(255, 80, 80, 220) : erase ? IM_COL32(255, 140, 60, 230) : IM_COL32(255, 220, 90, 230);
    dl->AddQuadFilled(q[0], q[1], q[2], q[3], (col32 & 0x00FFFFFF) | 0x30000000);
    dl->AddQuad(q[0], q[1], q[2], q[3], col32, 2.0f);
    char label[48];
    std::snprintf(label, sizeof(label), "%d, %d", hoverCol, hoverRow);
    dl->AddText(ImVec2(q[1].x + 4, q[1].y), IM_COL32(240, 240, 240, 230), label);
}

void NativeEditor::Impl::TilesPanel() {
    if (!ImGui::Begin(TrId("Tiles").c_str(), &showTiles)) {
        ImGui::End();
        return;
    }
    const EntityRow* row = Row(Primary());
    if (!row || !row->Has("Tilemap")) {
        ImGui::TextDisabled("%s", Tr("Select an entity with a Tilemap to paint tiles."));
        ImGui::End();
        return;
    }
    ImGui::Checkbox(Tr("Paint in the Scene view"), &tilePaint);
    HelpTooltip(Tr("Left drag: paint   Right drag: erase   Shift+drag: rectangle   Ctrl+click: pick a tile   Middle drag: pan"));
    const Json& info = TileInfo();
    if (info.isNull()) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("%s", Format(Tr("%d x %d cells"), info["width"].asInt(), info["height"].asInt()).c_str());
    if (info.has("error")) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1), "%s", info["error"].asString().c_str());

    std::shared_ptr<const Texture> tex;
    ImTextureID texId = ImTextureID();
    std::string image = info["image"].asString("");
    if (!image.empty() && engine.Gpu()) {
        tex = engine.Assets().GetTexture(image);
        if (tex) texId = ViewTexture(engine.Gpu()->ImageView(tex));
    }
    const int columns = std::max(1, info["columns"].asInt(1)), rows = std::max(1, info["rows"].asInt(1));
    const float cell = ImGui::GetFrameHeight() * 2.2f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int perRow = std::max(1, static_cast<int>(avail / (cell + ImGui::GetStyle().ItemSpacing.x + 6)));
    int n = 0;
    auto tileButton = [&](char c, int frame, const std::string& tip) {
        if (n++ % perRow != 0) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(static_cast<unsigned char>(c)));
        bool sel = tileBrush == c;
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? ImVec4(1.0f, 0.62f, 0.1f, 0.9f) : ImVec4(0.16f, 0.17f, 0.2f, 1));
        bool pressed;
        if (tex && frame >= 0) {
            int f = frame % (columns * rows);
            ImVec2 uv0(static_cast<float>(f % columns) / static_cast<float>(columns), static_cast<float>(f / columns) / static_cast<float>(rows));
            ImVec2 uv1(uv0.x + 1.0f / static_cast<float>(columns), uv0.y + 1.0f / static_cast<float>(rows));
            pressed = ImGui::ImageButton("##tile", texId, ImVec2(cell, cell), uv0, uv1);
        } else {
            pressed = ImGui::Button(c == ' ' ? "##erase" : std::string(1, c).c_str(), ImVec2(cell + 6, cell + 6));
        }
        ImGui::PopStyleColor();
        // Character label in the corner.
        ImVec2 mn = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(mn.x + 3, mn.y + 1), IM_COL32(255, 255, 255, 230), c == ' ' ? Tr("Erase") : std::string(1, c).c_str());
        if (pressed) {
            tileBrush = c;
            tilePaint = true;
        }
        HelpTooltip(tip);
        ImGui::PopID();
    };
    for (const Json& t : info["tiles"].items()) {
        std::string c = t["char"].asString("");
        if (c.size() != 1) continue;
        std::string tip = "'" + c + "'  " + Tr("collision") + ": " + t["collision"].asString("none");
        if (t.has("autotile")) tip += "  " + std::string(Tr("autotile")) + ": " + t["autotile"].asString();
        if (t.has("variants")) tip += "  " + Format(Tr("%d variants"), t["variants"].asInt());
        tileButton(c[0], t["frame"].asInt(-1), tip);
    }
    tileButton(' ', -1, Tr("Erase (empty cell)"));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Format(Tr("Brush: '%c'"), tileBrush).c_str());
    ImGui::End();
}

}  // namespace oe
