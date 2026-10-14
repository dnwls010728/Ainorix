#include "app/CoordinateQueries.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "app/Engine.h"
#include "render/Renderer.h"
#include "render/UI.h"

namespace oe {
namespace {
[[noreturn]] void Invalid(const std::string& message) {
    throw ApiError("invalid_query", message, "Use the documented camera/ui request with finite points and positive view dimensions.");
}
float Number(const Json& value, const char* label) {
    const double n = value.asNumber(std::numeric_limits<double>::quiet_NaN());
    if (!value.isNumber() || !std::isfinite(n) || std::fabs(n) > 1e20) Invalid(std::string(label) + " must be a finite number");
    return static_cast<float>(n);
}
Vec3 Point(const Json& value, bool three) {
    if (!value.isObject()) Invalid("point must be an object with x/y (and z for world points)");
    return Vec3(Number(value["x"], "x"), Number(value["y"], "y"), three ? Number(value["z"], "z") : 0);
}
Json Vector(const Vec3& v) {
    Json result = Json::MakeObject(); result["x"] = v.x; result["y"] = v.y; result["z"] = v.z; return result;
}
EntityId Entity(const Scene& scene, const Json& reference, const char* label) {
    EntityId id = kNullEntity;
    if (reference.isString()) id = scene.FindByName(reference.asString());
    else if (reference.isNumber()) {
        double n = reference.asNumber();
        if (std::isfinite(n) && n > 0 && n <= std::numeric_limits<EntityId>::max() && std::floor(n) == n) id = static_cast<EntityId>(n);
    }
    if (!scene.Exists(id)) Invalid(std::string(label) + " must name an existing entity");
    return id;
}
Vec3 Divide(const Vec4& point) {
    if (!std::isfinite(point.w) || std::fabs(point.w) <= 1e-6f) Invalid("camera projection cannot be inverted at this point");
    Vec3 result(point.x / point.w, point.y / point.w, point.z / point.w);
    if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z)) Invalid("nonfinite camera result");
    return result;
}
}  // namespace

Json QueryCoordinates(Engine& engine, const std::string& operation, const Json& a) {
    if (!a.isObject()) Invalid("query request must be an object");
    int width = engine.Input().viewWidth, height = engine.Input().viewHeight;
    if (a.has("view")) {
        if (!a["view"].isObject()) Invalid("view must contain width and height");
        Number(a["view"]["width"], "width"); Number(a["view"]["height"], "height");
        double w = a["view"]["width"].asNumber(), h = a["view"]["height"].asNumber();
        if (w < 1 || h < 1 || w > 16384 || h > 16384 || std::floor(w) != w || std::floor(h) != h) Invalid("view dimensions must be integers in 1..16384");
        width = static_cast<int>(w); height = static_cast<int>(h);
    }
    if (width < 1 || height < 1 || width > 16384 || height > 16384) Invalid("local view dimensions must be in 1..16384");
    const Scene& scene = engine.GetScene();
    Json result = Json::MakeObject();
    if (operation == "ui.screenToCanvas" || operation == "ui.measureText") {
        const auto& canvases = scene.Pool<UICanvas>();
        EntityId canvas = canvases.empty() ? kNullEntity : canvases.begin()->first;
        if (a.has("canvas") && !a["canvas"].isNull() && Entity(scene, a["canvas"], "canvas") != canvas) Invalid("canvas must be the single effective canvas (first UICanvas by ID)");
        const float scale = UIScale(scene, width, height);
        if (!std::isfinite(scale) || scale <= 0) Invalid("invalid canvas scale");
        if (operation == "ui.measureText") {
            if (!a["text"].isString() || a["text"].asString().size() > 65536) Invalid("text must be a string of at most 65536 UTF-8 bytes");
            UIText text; text.text = a["text"].asString();
            if (a.has("style")) {
                const Json& style = a["style"];
                if (!style.isObject()) Invalid("style must be an object");
                for (const auto& field : style.members()) {
                    const std::string& key = field.first; const Json& value = field.second;
                    if (key == "font") {
                        if (!value.isString()) Invalid("font must be a string");
                        text.font = value.asString();
                        if (!text.font.empty() && text.font != "pixel" && text.font != "default") engine.ResolvePath(text.font);
                    } else if (key == "size") text.size = Number(value, "size");
                    else if (key == "letterSpacing") text.letterSpacing = Number(value, "letterSpacing");
                    else if (key == "lineSpacing") text.lineSpacing = Number(value, "lineSpacing");
                    else if (key == "bold" || key == "rich") {
                        if (!value.isBool()) Invalid(key + " must be boolean");
                        if (key == "bold") text.bold = value.asBool(); else text.richText = value.asBool();
                    } else Invalid("unknown measurement style field: " + key);
                }
            }
            float wrap = a.has("wrapWidth") ? Number(a["wrapWidth"], "wrapWidth") : 0;
            if (text.size < 0.125f || text.size > 1024 || text.size * scale > 1024 ||
                std::fabs(text.letterSpacing) > 1024 || text.lineSpacing <= 0 || text.lineSpacing > 16 || wrap < 0 || wrap > 65536)
                Invalid("measurement style/wrap/effective size exceeds documented bounds");
            TextMetrics metrics = MeasureText(text, scale, wrap, &engine.Assets());
            result["width"] = metrics.width; result["height"] = metrics.height;
            result["lines"] = static_cast<int>(metrics.lineWidths.size()); result["lineWidths"] = Json::MakeArray();
            for (float line : metrics.lineWidths) result["lineWidths"].push(line);
            result["scale"] = scale;
            return result;
        }
        Vec3 p = Point(a["screen"], false);
        result["x"] = p.x / scale; result["y"] = p.y / scale;
        result["width"] = static_cast<float>(width) / scale; result["height"] = static_cast<float>(height) / scale;
        result["scale"] = scale; result["canvas"] = canvas == kNullEntity ? Json() : Json(canvas);
        return result;
    }
    EntityId id = Entity(scene, a["camera"], "camera");
    const Camera* camera = scene.Get<Camera>(id);
    if (!camera) Invalid("camera entity requires Camera");
    if (!std::isfinite(camera->nearPlane) || !std::isfinite(camera->farPlane) || camera->nearPlane <= 0 || camera->farPlane <= camera->nearPlane ||
        !std::isfinite(camera->fov) || camera->fov <= 0 || camera->fov >= 180 || !std::isfinite(camera->orthoSize)) Invalid("invalid Camera projection parameters");
    Mat4 world = scene.WorldMatrix(id);
    Vec3 forward = world.TransformDir(Vec3(0, 0, -1)), up = world.TransformDir(Vec3(0, 1, 0));
    if (Length(forward) <= 1e-6f || Length(up) <= 1e-6f || Length(Cross(forward, up)) <= 1e-6f) Invalid("degenerate camera world transform");
    RenderView view; MakeCameraView(scene, id, static_cast<float>(width) / static_cast<float>(height), view);
    const Mat4 matrix = view.proj * view.view;
    // The picture's rectangle on the screen: the whole screen, or the integer-scaled, centered art
    // frame of a pixel-perfect camera (PresentPixelFrame).
    float left = 0, top = 0, viewW = static_cast<float>(width), viewH = static_cast<float>(height);
    if (view.pixelWidth > 0 && view.pixelHeight > 0) {
        const int scale = std::max(1, std::min(width / view.pixelWidth, height / view.pixelHeight));
        viewW = static_cast<float>(view.pixelWidth * scale);
        viewH = static_cast<float>(view.pixelHeight * scale);
        left = static_cast<float>((width - view.pixelWidth * scale) / 2);
        top = static_cast<float>((height - view.pixelHeight * scale) / 2);
    }
    if (operation == "camera.worldToScreen") {
        Vec3 position = Point(a["position"], true);
        Vec4 clip = matrix * Vec4(position, 1);
        const float depth = -(view.view * Vec4(position, 1)).z;
        if (!std::isfinite(depth) || !std::isfinite(clip.w)) Invalid("nonfinite camera transform");
        result["depth"] = depth; result["x"] = Json(); result["y"] = Json(); result["ndcDepth"] = Json();
        bool inside = false;
        if (std::fabs(clip.w) > 1e-6f) {
            Vec3 ndc = Divide(clip);
            result["x"] = left + (ndc.x + 1) * 0.5f * viewW;
            result["y"] = top + (1 - ndc.y) * 0.5f * viewH;
            result["ndcDepth"] = ndc.z;
            inside = ndc.x >= -1 && ndc.x <= 1 && ndc.y >= -1 && ndc.y <= 1 && ndc.z >= -1 && ndc.z <= 1;
        }
        result["visible"] = depth > 0 && inside;
        result["status"] = depth <= 0 ? "behind" : inside ? "visible" : "outside";
        return result;
    }
    Vec3 screen = Point(a["screen"], false);
    Mat4 inverse = matrix.Inverse(), identity = matrix * inverse;
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col)
        if (!std::isfinite(identity.at(row, col)) || std::fabs(identity.at(row, col) - (row == col ? 1.0f : 0.0f)) > 0.01f) Invalid("degenerate camera projection");
    float x = (screen.x - left) * 2 / viewW - 1, y = 1 - (screen.y - top) * 2 / viewH;
    Vec3 origin = Divide(inverse * Vec4(x, y, -1, 1)), far = Divide(inverse * Vec4(x, y, 1, 1));
    Vec3 direction = Normalize(far - origin);
    if (Length(direction) < 0.5f) Invalid("degenerate screen ray");
    if (operation == "camera.screenRay") {
        result["origin"] = Vector(origin); result["direction"] = Vector(direction); return result;
    }
    if (operation != "camera.screenToPlane") Invalid("unknown coordinate operation");
    Vec3 point = Point(a["plane"]["point"], true), normal = Point(a["plane"]["normal"], true);
    if (!std::isfinite(Length(normal)) || Length(normal) <= 1e-6f) Invalid("plane normal must have finite nonzero length");
    normal = Normalize(normal);
    float denominator = Dot(normal, direction);
    result["hit"] = false;
    if (std::fabs(denominator) <= 1e-6f) { result["reason"] = "parallel"; return result; }
    float distance = Dot(point - origin, normal) / denominator;
    if (!std::isfinite(distance)) Invalid("nonfinite plane intersection");
    if (distance < -1e-5f) { result["reason"] = "backward"; return result; }
    distance = std::max(0.0f, distance);
    result["hit"] = true; result["point"] = Vector(origin + direction * distance); result["distance"] = distance;
    return result;
}
}  // namespace oe
