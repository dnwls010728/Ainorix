#include "app/Skeleton2DReview.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "api/Commands.h"
#include "app/Engine.h"
#include "app/Skeleton2DRaster.h"
#include "assets/Assets.h"
#include "core/Image.h"
#include "render/ImageDraw.h"
#include "render/Skeleton2DMesh.h"
#include "scene/Components.h"
#include "scene/Skeleton2D.h"

namespace oe {
namespace {

using namespace raster2d;

struct Subject {
    std::shared_ptr<const Skeleton2DDef> def;
    std::string skin;
};

Subject LoadSubject(Engine& engine, const Json& args) {
    Subject s;
    std::string asset;
    if (args.has("id")) {
        const Json& v = args["id"];
        const EntityId id = v.isString() ? engine.GetScene().FindByName(v.asString()) : static_cast<EntityId>(v.asNumber());
        const Skeleton2D* component = engine.GetScene().Record(id) ? engine.GetScene().Get<Skeleton2D>(id) : nullptr;
        if (!component) throw ApiError("not_found", "no entity with a Skeleton2D", "Pass id (an entity with Skeleton2D) or path (*.skel2d.json).");
        asset = component->asset;
        s.skin = component->skin;
    } else if (args.has("path")) {
        asset = args["path"].asString();
    } else {
        throw ApiError("invalid_args", "pass path or id");
    }
    if (args.has("skin")) s.skin = args["skin"].asString();
    std::string error;
    s.def = asset.empty() ? nullptr : engine.Assets().GetSkeleton2D(asset, &error);
    if (!s.def) throw ApiError("invalid_skeleton", asset.empty() ? std::string("the Skeleton2D has no asset") : error, "skeleton2d.check {path} shows what is wrong.");
    return s;
}

struct Posed {
    std::string label, clip;
    float time = 0;
    Skeleton2DPose pose;
};

// Bones a stress pose bends: those something hangs on (a slot, a weight, or a child that does).
std::vector<int> BendableBones(const Skeleton2DDef& def) {
    std::vector<bool> used(def.bones.size(), false);
    for (const Skeleton2DSlot& slot : def.slots) used[static_cast<size_t>(slot.bone)] = true;
    for (const auto& skin : def.skins)
        for (const auto& slot : skin.second)
            for (const auto& entry : slot.second)
                for (const auto& list : entry.second.weights)
                    for (const auto& influence : list) used[static_cast<size_t>(influence.first)] = true;
    for (size_t b = def.bones.size(); b-- > 1;)
        if (used[b] && def.bones[b].parent > 0) used[static_cast<size_t>(def.bones[b].parent)] = true;
    std::vector<int> bones;
    for (size_t b = 1; b < def.bones.size(); ++b)
        if (used[b]) bones.push_back(static_cast<int>(b));
    return bones;
}

std::string Number(float v) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(v));
    return buffer;
}

std::vector<Posed> MakePoses(const Skeleton2DDef& def, const Json& args) {
    std::vector<Posed> poses;
    const std::string clipName = args["clip"].asString("");
    if (!clipName.empty()) {
        std::vector<const Skeleton2DClip*> clips;
        if (clipName == "*") {
            for (const Skeleton2DClip& c : def.clips) clips.push_back(&c);
            if (clips.empty()) throw ApiError("not_found", "the skeleton has no clips", "Leave out clip for stress poses.");
        } else {
            const Skeleton2DClip* c = def.FindClip(clipName);
            if (!c) throw ApiError("not_found", "no clip '" + clipName + "'", "skeleton2d.get lists the clips.");
            clips.push_back(c);
        }
        for (const Skeleton2DClip* c : clips) {
            std::vector<float> times;
            if (args.has("times")) {
                for (const Json& t : args["times"].items())
                    if (t.isNumber()) times.push_back(t.asFloat());
            } else {
                const int samples = std::clamp(args["samples"].asInt(8), 1, 64);
                // A looping clip's end is its start; a one-shot clip's end is its last pose.
                for (int i = 0; i < samples; ++i)
                    times.push_back(c->duration * static_cast<float>(i) / static_cast<float>(c->loop ? samples : std::max(1, samples - 1)));
            }
            for (float t : times) {
                Posed p;
                p.clip = c->name;
                p.time = t;
                p.label = c->name + " " + Number(t);
                // Not wrapped: the last sample of a one-shot clip stays at its end.
                p.pose = Skeleton2DSample(def, c, c->loop ? t : std::min(t, c->duration));
                poses.push_back(std::move(p));
            }
        }
    } else {
        const float angle = std::clamp(args["angle"].asFloat(45), 1.0f, 180.0f);
        for (int b : BendableBones(def))
            for (float sign : {1.0f, -1.0f}) {
                Posed p;
                p.pose = Skeleton2DSetupPose(def);
                p.pose.bones[static_cast<size_t>(b)].rotation += sign * angle;
                Skeleton2DWorld(def, p.pose);
                p.label = def.bones[static_cast<size_t>(b)].name + (sign > 0 ? " +" : " -") + Number(angle);
                poses.push_back(std::move(p));
            }
    }
    if (poses.size() > 96) poses.resize(96);
    return poses;
}

float Coverage(const View& view, const Frame& frame, float wx, float wy, float radius) {
    const float cx = (wx - view.left) * view.scale, cy = (view.top - wy) * view.scale;
    int inside = 0, opaque = 0;
    for (int y = static_cast<int>(cy - radius); y <= static_cast<int>(cy + radius); ++y)
        for (int x = static_cast<int>(cx - radius); x <= static_cast<int>(cx + radius); ++x) {
            const float dx = static_cast<float>(x) + 0.5f - cx, dy = static_cast<float>(y) + 0.5f - cy;
            if (dx * dx + dy * dy > radius * radius) continue;
            ++inside;
            if (x >= 0 && y >= 0 && x < view.width && y < view.height && frame.piece[static_cast<size_t>(y * view.width + x)] >= 0) ++opaque;
        }
    return inside ? static_cast<float>(opaque) / static_cast<float>(inside) : 0;
}

float Area(const Vec3& a, const Vec3& b, const Vec3& c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

Json Problem(const std::string& kind, const std::string& severity, const std::string& subject, const std::string& message, const std::string& hint) {
    Json p = Json::MakeObject();
    p["kind"] = kind;
    p["severity"] = severity;
    p["subject"] = subject;
    p["message"] = message;
    p["hint"] = hint;
    return p;
}

std::string SlotName(const Skeleton2DDef& def, int slot) { return def.slots[static_cast<size_t>(slot)].name; }

}  // namespace

Json AnalyzeSkeleton2D(Engine& engine, const Json& args) {
    const Subject subject = LoadSubject(engine, args);
    const Skeleton2DDef& def = *subject.def;
    std::vector<Posed> poses = MakePoses(def, args);
    if (poses.empty()) throw ApiError("invalid_args", "no poses to analyze", "The skeleton has no bones to bend; give a clip.");
    Skeleton2DPose setupPose = Skeleton2DSetupPose(def);
    Skeleton2DWorld(def, setupPose);
    const Shown setup = Show(def, subject.skin, setupPose, engine.Assets());
    std::vector<Shown> shown;
    for (const Posed& p : poses) shown.push_back(Show(def, subject.skin, p.pose, engine.Assets()));
    std::vector<const Shown*> all{&setup};
    for (const Shown& s : shown) all.push_back(&s);
    const View view = FitView(all, 768, std::max(0.01f, def.pixelsPerUnit));
    // Hidden texels are kept in blocks about one frame pixel wide.
    const int block = std::max(1, static_cast<int>(std::ceil(def.pixelsPerUnit / view.scale)));
    auto blockOf = [&](const Texture& t, int texel) {
        const int bw = (t.width + block - 1) / block;
        return (texel / t.width) / block * bw + (texel % t.width) / block;
    };

    // The setup pose: which texels of each attachment lie under a later part, and under which.
    struct Hidden {
        std::vector<uint8_t> hidden, revealed;
        std::map<int, int> under;  // covering slot -> pixels
        int visible = 0;            // pixels it shows in the setup pose
        const Texture* texture = nullptr;
        int slot = 0;
        int worst = 0;            // most revealed pixels in one pose
        std::string worstLabel;
    };
    std::map<const Skeleton2DAttachment*, Hidden> hidden;
    const Frame setupFrame = Draw(view, setup);
    for (size_t i = 0; i < setup.pieces.size(); ++i) {
        const Skeleton2DPiece& piece = setup.pieces[i];
        if (!piece.texture) continue;
        Hidden& h = hidden[piece.attachment];
        h.texture = piece.texture.get();
        h.slot = piece.slot;
        const size_t blocks = static_cast<size_t>((h.texture->width + block - 1) / block) * static_cast<size_t>((h.texture->height + block - 1) / block);
        h.hidden.assign(blocks, 0);
        h.revealed.assign(blocks, 0);
    }
    Rasterize(view, setup, [&](int piece, int pixel, int texel, uint32_t) {
        const int top = setupFrame.piece[static_cast<size_t>(pixel)];
        Hidden& h = hidden[setup.pieces[static_cast<size_t>(piece)].attachment];
        if (top == piece) {
            ++h.visible;
        } else if (top > piece && !setup.pieces[static_cast<size_t>(piece)].attachment->filled) {  // a repainted picture has nothing wrong to reveal
            h.hidden[static_cast<size_t>(blockOf(*h.texture, texel))] = 1;
            ++h.under[setup.pieces[static_cast<size_t>(top)].slot];
        }
    });
    int figurePixels = 0;
    for (int p : setupFrame.piece) figurePixels += p >= 0;

    // Joints: where each bone turns, and how filled the figure is around it in the setup pose.
    const float radius = std::max(3.0f, 0.02f * static_cast<float>(view.height));
    std::vector<float> setupCoverage(def.bones.size(), 0);
    for (size_t b = 1; b < def.bones.size(); ++b) {
        const Vec3 o = setupPose.world[b].TransformPoint(Vec3());
        setupCoverage[b] = Coverage(view, setupFrame, o.x, o.y, radius);
    }

    struct Worst {
        float value = 0;
        std::string label;
        int pose = -1;
    };
    std::vector<Worst> gaps(def.bones.size());
    std::map<const Skeleton2DAttachment*, Worst> flips, stretch, squash;
    std::vector<float> poseScore(poses.size(), 0);
    std::vector<Frame> frames;
    for (size_t p = 0; p < poses.size(); ++p) {
        const Shown& s = shown[p];
        Frame frame = Draw(view, s);
        // Revealed: the shown texel was under another part in the setup pose.
        std::map<const Skeleton2DAttachment*, int> revealed;
        for (size_t i = 0; i < frame.piece.size(); ++i) {
            const int piece = frame.piece[i];
            if (piece < 0) continue;
            auto h = hidden.find(s.pieces[static_cast<size_t>(piece)].attachment);
            if (h == hidden.end() || !h->second.texture) continue;
            const size_t b = static_cast<size_t>(blockOf(*h->second.texture, frame.texel[i]));
            if (b < h->second.hidden.size() && h->second.hidden[b]) {
                ++revealed[h->first];
                h->second.revealed[b] = 1;
            }
        }
        for (const auto& r : revealed) {
            Hidden& h = hidden[r.first];
            if (r.second > h.worst) h.worst = r.second, h.worstLabel = poses[p].label;
            poseScore[p] += static_cast<float>(r.second) / static_cast<float>(std::max(1, figurePixels));
        }
        // Gaps around joints.
        for (size_t b = 1; b < def.bones.size(); ++b) {
            if (setupCoverage[b] < 0.9f) continue;
            const Vec3 o = poses[p].pose.world[b].TransformPoint(Vec3());
            const float drop = setupCoverage[b] - Coverage(view, frame, o.x, o.y, radius);
            if (drop > gaps[b].value) gaps[b] = {drop, poses[p].label, static_cast<int>(p)};
            if (drop > 0.15f) poseScore[p] += drop;
        }
        // Triangles that fold or stretch.
        for (size_t i = 0; i < s.pieces.size(); ++i) {
            const Skeleton2DPiece& piece = s.pieces[i];
            if (!piece.attachment->IsMesh()) continue;
            int flipped = 0;
            float most = 1, least = 1;
            for (size_t t = 0; t + 2 < piece.triangles.size(); t += 3) {
                const uint32_t a = piece.triangles[t], b = piece.triangles[t + 1], c = piece.triangles[t + 2];
                const float before = Area(piece.positions[a], piece.positions[b], piece.positions[c]);
                if (std::fabs(before) < 1e-8f) continue;
                const float after = Area(s.posed[i][a], s.posed[i][b], s.posed[i][c]);
                if (after * before <= 0) ++flipped;
                const uint32_t e[3][2] = {{a, b}, {b, c}, {c, a}};
                for (const auto& edge : e) {
                    const float l0 = Length(piece.positions[edge[1]] - piece.positions[edge[0]]);
                    if (l0 < 1e-6f) continue;
                    const float ratio = Length(s.posed[i][edge[1]] - s.posed[i][edge[0]]) / l0;
                    most = std::max(most, ratio), least = std::min(least, ratio);
                }
            }
            if (flipped > flips[piece.attachment].value) flips[piece.attachment] = {static_cast<float>(flipped), poses[p].label, static_cast<int>(p)};
            if (most > stretch[piece.attachment].value) stretch[piece.attachment] = {most, poses[p].label, static_cast<int>(p)};
            Worst& sq = squash[piece.attachment];
            if (sq.pose < 0 || least < sq.value) sq = {least, poses[p].label, static_cast<int>(p)};
            if (flipped) poseScore[p] += 0.5f;
        }
        frames.push_back(std::move(frame));
    }

    // Problems, the most serious first.
    Json problems = Json::MakeArray();
    std::vector<std::pair<float, Json>> found;
    for (auto& entry : hidden) {
        Hidden& h = entry.second;
        if (h.worst == 0) continue;
        const float ofSlot = 100.0f * static_cast<float>(h.worst) / static_cast<float>(std::max(1, h.visible));
        const float ofFigure = 100.0f * static_cast<float>(h.worst) / static_cast<float>(std::max(1, figurePixels));
        if (ofSlot < 0.5f && ofFigure < 0.1f) continue;
        int coverer = -1, most = 0;
        for (const auto& u : h.under) if (u.second > most) most = u.second, coverer = u.first;
        const std::string slot = SlotName(def, h.slot);
        Json p = Problem("revealed", ofSlot > 3 && ofFigure > 0.2f ? "high" : "low", slot,
                         slot + " shows " + std::to_string(h.worst) + " px (" + Number(ofSlot) + "% of it) that were hidden" +
                             (coverer >= 0 ? " under " + SlotName(def, coverer) : std::string()) + " in the setup pose, worst at \"" + h.worstLabel + "\"",
                         "That picture was never meant to be seen: cut edges or a copy of the covering part. Repaint it (analyze with masks:true gives the "
                         "area; skeleton2d.variant_requests turns it into an image request), make the parts overlap less in the draft, or bend less there.");
        p["pixels"] = h.worst;
        p["percentOfSlot"] = std::round(ofSlot * 10) / 10;
        p["pose"] = h.worstLabel;
        if (coverer >= 0) p["hiddenUnder"] = SlotName(def, coverer);
        found.emplace_back(ofFigure, p);
    }
    for (size_t b = 1; b < def.bones.size(); ++b) {
        if (gaps[b].value <= 0.15f) continue;
        const std::string bone = def.bones[b].name;
        Json p = Problem("gap", gaps[b].value > 0.3f ? "high" : "low", bone,
                         "the figure opens around the " + bone + " joint: " + Number(100 * gaps[b].value) + "% less of it is filled at \"" + gaps[b].label + "\"",
                         "The parts meeting at this joint do not overlap enough. Grow both regions past the joint in the draft, or make one part a mesh "
                         "weighted to both bones so it bends across the joint.");
        p["drop"] = std::round(gaps[b].value * 100) / 100;
        p["pose"] = gaps[b].label;
        found.emplace_back(gaps[b].value * 10, p);
    }
    for (const auto& f : flips) {
        if (f.second.value <= 0) continue;
        Json p = Problem("flipped", "high", f.first->name, std::to_string(static_cast<int>(f.second.value)) + " triangles of " + f.first->name + " fold over at \"" + f.second.label + "\"",
                         "The bend is too sharp for this mesh: weight the vertices near the joint more evenly between the bones (a smaller cell gives "
                         "more vertices to share it), or turn the bone less.");
        p["triangles"] = static_cast<int>(f.second.value);
        p["pose"] = f.second.label;
        found.emplace_back(100 + f.second.value, p);
    }
    for (const auto& s : stretch) {
        const float most = s.second.value, least = squash[s.first].value;
        if (most <= 1.6f && least >= 0.5f) continue;
        const bool high = most > 2 || least < 0.4f;
        const bool stretched = most - 1 >= 1 / std::max(0.01f, least) - 1;
        const std::string pose = stretched ? s.second.label : squash[s.first].label;
        Json p = Problem("stretched", high ? "high" : "low", s.first->name,
                         s.first->name + (stretched ? " stretches to " + Number(most) + "x" : " squashes to " + Number(least) + "x") + " at \"" + pose + "\"",
                         "Its picture is pulled out of shape: spread the weights over more of the part, or let the neighbouring bone carry more of the move.");
        p["ratio"] = std::round((stretched ? most : least) * 100) / 100;
        p["pose"] = pose;
        found.emplace_back(stretched ? most : 1 / std::max(0.01f, least), p);
    }
    // Feet against the ground line of the setup pose (clips only: stress poses bend legs on purpose).
    Json ground;
    if (args.has("clip")) {
        std::vector<int> feet;
        if (args["feet"].isArray()) {
            for (const Json& f : args["feet"].items()) {
                const int b = def.BoneIndex(f.asString());
                if (b <= 0) throw ApiError("not_found", "no bone '" + f.asString() + "' for feet");
                feet.push_back(b);
            }
        } else {
            for (const char* name : {"foot_l", "foot_r", "foot_front", "foot_back"})
                if (def.BoneIndex(name) > 0) feet.push_back(def.BoneIndex(name));
        }
        auto lowest = [&](const Skeleton2DPose& pose, int foot) {
            const Mat4& m = pose.world[static_cast<size_t>(foot)];
            return std::min(m.TransformPoint(Vec3()).y, m.TransformPoint(Vec3(def.bones[static_cast<size_t>(foot)].length, 0, 0)).y);
        };
        if (!feet.empty()) {
            // The figure's height: its bones from feet to head in the setup pose (parts may not cover them all).
            float top = -1e30f, bottom = 1e30f;
            for (size_t b = 0; b < def.bones.size(); ++b)
                for (float x : {0.0f, def.bones[b].length}) {
                    const float y = setupPose.world[b].TransformPoint(Vec3(x, 0, 0)).y;
                    top = std::max(top, y), bottom = std::min(bottom, y);
                }
            const float figure = std::max(1e-3f, top - bottom);
            float line = 1e30f;
            for (int f : feet) line = std::min(line, lowest(setupPose, f));
            float sink = 0, rise = 1e30f;
            std::string sinkPose;
            for (size_t p = 0; p < poses.size(); ++p) {
                float low = 1e30f;
                for (int f : feet) low = std::min(low, lowest(poses[p].pose, f));
                if (line - low > sink) sink = line - low, sinkPose = poses[p].label;
                rise = std::min(rise, low - line);
            }
            // How fast a planted foot (within 2% of the figure of the ground) moves along the ground: walking in place, the
            // entity must move at minus that speed for the feet not to slide.
            double travel = 0, time = 0;
            for (size_t p = 1; p < poses.size(); ++p) {
                if (poses[p].clip != poses[p - 1].clip || poses[p].time <= poses[p - 1].time) continue;
                // The foot carrying the body: the lowest one at the start of the step.
                int planted = feet[0];
                for (int f : feet)
                    if (lowest(poses[p - 1].pose, f) < lowest(poses[p - 1].pose, planted)) planted = f;
                for (int f : {planted}) {
                    const float a = lowest(poses[p - 1].pose, f), b = lowest(poses[p].pose, f);
                    if (std::fabs(a - line) > 0.02f * figure || std::fabs(b - line) > 0.02f * figure) continue;
                    travel += poses[p].pose.world[static_cast<size_t>(f)].TransformPoint(Vec3()).x - poses[p - 1].pose.world[static_cast<size_t>(f)].TransformPoint(Vec3()).x;
                    time += poses[p].time - poses[p - 1].time;
                }
            }
            ground = Json::MakeObject();
            ground["line"] = line;
            if (time > 0) ground["plantedSpeed"] = std::round(travel / time * 1000) / 1000;
            if (sink > 0.02f * figure) {
                Json p = Problem("ground", sink > 0.04f * figure ? "high" : "low", "feet",
                                 "a foot goes " + Number(100 * sink / figure) + "% of the figure below the ground of the setup pose at \"" + sinkPose + "\"",
                                 "Raise the hip at that time (translate y) or bend the knee less, so the lowest foot stays on the ground line.");
                p["pose"] = sinkPose;
                found.emplace_back(sink / figure * 10, p);
            }
            bool airborne = false;
            for (const Skeleton2DClip& c : def.clips)
                if (c.name == args["clip"].asString())
                    for (const AnimationMarker& m : c.events) airborne = airborne || m.name == "takeoff";
            if (!airborne && rise > 0.03f * figure && rise < 1e29f) {
                Json p = Problem("ground", "low", "feet", "no foot touches the ground in any pose (the lowest is " + Number(100 * rise / figure) + "% of the figure above it)",
                                 "Lower the hip (translate y) so a foot reaches the ground line, or add a takeoff event if the clip is a jump.");
                found.emplace_back(rise / figure * 5, p);
            }
        }
    }
    // A looping clip should end where it starts.
    if (args.has("clip") && args["clip"].asString() != "*")
        if (const Skeleton2DClip* clip = def.FindClip(args["clip"].asString()))
            if (clip->loop && clip->duration > 0) {
                const Skeleton2DPose a = Skeleton2DSample(def, clip, 0), b = Skeleton2DSample(def, clip, clip->duration - 1e-3f);
                float worst = 0;
                std::string bone;
                const float figure = static_cast<float>(view.height) / view.scale;
                for (size_t i = 1; i < def.bones.size(); ++i) {
                    const float move = Length(a.world[i].TransformPoint(Vec3()) - b.world[i].TransformPoint(Vec3())) / figure;
                    const Vec3 xa = a.world[i].TransformDir(Vec3(1, 0, 0)), xb = b.world[i].TransformDir(Vec3(1, 0, 0));
                    const float turn = std::fabs(Degrees(std::atan2(xa.x * xb.y - xa.y * xb.x, xa.x * xb.x + xa.y * xb.y))) / 300.0f;  // 3 degrees ~ 1%
                    if (std::max(move, turn) > worst) worst = std::max(move, turn), bone = def.bones[i].name;
                }
                if (worst > 0.01f) {
                    Json p = Problem("seam", "high", clip->name, clip->name + " loops but its end does not match its start (" + bone + " jumps)",
                                     "Key the end of the clip (at its duration) with the same values as time 0, or set loop: false.");
                    p["bone"] = bone;
                    found.emplace_back(50, p);
                }
            }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        const bool ha = a.second["severity"].asString() == "high", hb = b.second["severity"].asString() == "high";
        return ha != hb ? ha : a.first > b.first;
    });
    bool passed = true;
    for (auto& f : found) {
        passed = passed && f.second["severity"].asString() != "high";
        problems.push(f.second);
    }

    Json result = Json::MakeObject();
    result["poses"] = static_cast<int>(poses.size());
    result["passed"] = passed;
    result["problems"] = problems;
    Json frame = Json::MakeObject();
    frame["width"] = view.width;
    frame["height"] = view.height;
    frame["pixelsPerUnit"] = view.scale;
    frame["figurePixels"] = figurePixels;
    result["frame"] = frame;
    if (!ground.isNull()) result["ground"] = ground;

    // Masks of the revealed texels, one per attachment, at the size of its image.
    if (args["masks"].asBool()) {
        Json masks = Json::MakeArray();
        for (const auto& entry : hidden) {
            const Hidden& h = entry.second;
            if (h.worst == 0) continue;
            Image mask;
            mask.width = h.texture->width;
            mask.height = h.texture->height;
            mask.rgba.assign(static_cast<size_t>(mask.width) * static_cast<size_t>(mask.height) * 4, 0);
            int count = 0;
            for (int y = 0; y < mask.height; ++y)
                for (int x = 0; x < mask.width; ++x)
                    if (h.revealed[static_cast<size_t>(blockOf(*h.texture, y * mask.width + x))]) {
                        uint8_t* px = &mask.rgba[(static_cast<size_t>(y) * static_cast<size_t>(mask.width) + static_cast<size_t>(x)) * 4];
                        px[0] = px[1] = px[2] = px[3] = 255;
                        ++count;
                    }
            Json m = Json::MakeObject();
            m["slot"] = SlotName(def, h.slot);
            m["attachment"] = entry.first->name;
            m["image"] = entry.first->image;
            Json maskArgs = Json::MakeObject();
            maskArgs["out"] = ".oe/vision/revealed/" + SlotName(def, h.slot) + "-" + entry.first->name + ".png";
            m["mask"] = WriteToolImage(engine, maskArgs, maskArgs["out"].asString(), mask);
            m["texels"] = count;
            masks.push(m);
        }
        result["masks"] = masks;
    }

    // A picture of the setup pose and the worst poses, revealed picture in magenta and opened joints circled.
    if (args["render"].asBool(true)) {
        std::vector<size_t> order(poses.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return poseScore[a] > poseScore[b]; });
        order.resize(std::min<size_t>(order.size(), 3));
        const int textScale = view.width >= 400 ? 2 : 1;
        const int panels = static_cast<int>(order.size()) + 1, labelHeight = 8 + TextHeight(textScale);
        Image image;
        image.width = view.width * panels + 4 * (panels - 1);
        image.height = view.height + labelHeight;
        image.rgba.assign(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4, 0);
        FillRect(image, 0, 0, image.width, image.height, {24, 26, 32, 255});
        auto panel = [&](int index, const Frame& f, const std::string& label, int pose) {
            const int ox = index * (view.width + 4);
            Paint(image, ox, labelHeight, view, f);
            DrawText(image, ox + 4, 4, label, {255, 255, 255, 255}, textScale);
            if (pose < 0) return;
            const Shown& s = shown[static_cast<size_t>(pose)];
            for (int y = 0; y < view.height; ++y)
                for (int x = 0; x < view.width; ++x) {
                    const size_t i = static_cast<size_t>(y * view.width + x);
                    if (f.piece[i] < 0) continue;
                    auto h = hidden.find(s.pieces[static_cast<size_t>(f.piece[i])].attachment);
                    if (h == hidden.end() || !h->second.texture) continue;
                    const size_t b = static_cast<size_t>(blockOf(*h->second.texture, f.texel[i]));
                    if (b < h->second.hidden.size() && h->second.hidden[b]) BlendPixel(image, ox + x, labelHeight + y, {255, 0, 255, 170});
                }
            for (size_t b = 1; b < def.bones.size(); ++b)
                if (gaps[b].pose == pose && gaps[b].value > 0.15f) {
                    const Vec3 o = poses[static_cast<size_t>(pose)].pose.world[b].TransformPoint(Vec3());
                    DrawCircle(image, static_cast<float>(ox) + (o.x - view.left) * view.scale, static_cast<float>(labelHeight) + (view.top - o.y) * view.scale,
                               radius + 2, {255, 60, 60, 255}, false);
                }
        };
        panel(0, setupFrame, "setup", -1);
        for (size_t i = 0; i < order.size(); ++i) panel(static_cast<int>(i) + 1, frames[order[i]], poses[order[i]].label, static_cast<int>(order[i]));
        Json renderArgs = Json::MakeObject();
        renderArgs["out"] = args["out"].asString(".oe/vision/analyze.png");
        result["image"] = WriteToolImage(engine, renderArgs, renderArgs["out"].asString(), image);
    }
    result["next"] = passed ? "No serious problems in these poses. Try others (clip, angle) before trusting the rig."
                            : "Fix the high problems first (each has a hint), rebuild, and analyze again.";
    return result;
}

Json Skeleton2DContactSheet(Engine& engine, const Json& args) {
    const Subject subject = LoadSubject(engine, args);
    const Skeleton2DDef& def = *subject.def;
    std::vector<Posed> poses = MakePoses(def, args);
    if (poses.empty()) throw ApiError("invalid_args", "no poses to show", "Give a clip, or a skeleton with bones to bend.");
    std::vector<Shown> shown;
    for (const Posed& p : poses) shown.push_back(Show(def, subject.skin, p.pose, engine.Assets()));
    const int cell = std::clamp(args["cell"].asInt(256), 64, 1024);
    const int columns = std::clamp(args["columns"].asInt(std::min<int>(4, static_cast<int>(poses.size()))), 1, 16);
    const int rows = (static_cast<int>(poses.size()) + columns - 1) / columns, labelHeight = 14;
    int focus = -1;
    if (args.has("focus")) {
        focus = def.BoneIndex(args["focus"].asString());
        if (focus < 0) throw ApiError("not_found", "no bone '" + args["focus"].asString() + "'");
    }
    std::vector<const Shown*> all;
    for (const Shown& s : shown) all.push_back(&s);
    const View whole = FitView(all, cell, 1e6f);
    const float zoom = std::clamp(args["zoom"].asFloat(3), 1.0f, 16.0f);
    Image image;
    image.width = columns * (cell + 4) + 4;
    image.height = rows * (cell + labelHeight + 4) + 4;
    image.rgba.assign(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4, 0);
    FillRect(image, 0, 0, image.width, image.height, {24, 26, 32, 255});
    Json frames = Json::MakeArray();
    for (size_t i = 0; i < poses.size(); ++i) {
        View view;
        view.width = view.height = cell;
        if (focus >= 0) {
            // Around the joint, the same zoom for every pose.
            const Vec3 o = poses[i].pose.world[static_cast<size_t>(focus)].TransformPoint(Vec3());
            view.scale = whole.scale * zoom;
            view.left = o.x - static_cast<float>(cell) * 0.5f / view.scale;
            view.top = o.y + static_cast<float>(cell) * 0.5f / view.scale;
        } else {
            view.scale = whole.scale;
            view.left = whole.left - (static_cast<float>(cell) / whole.scale - (static_cast<float>(whole.width) / whole.scale)) * 0.5f;
            view.top = whole.top + (static_cast<float>(cell) / whole.scale - (static_cast<float>(whole.height) / whole.scale)) * 0.5f;
        }
        const Frame frame = Draw(view, shown[i]);
        const int ox = 4 + static_cast<int>(i % static_cast<size_t>(columns)) * (cell + 4);
        const int oy = 4 + static_cast<int>(i / static_cast<size_t>(columns)) * (cell + labelHeight + 4);
        DrawText(image, ox + 2, oy + 3, poses[i].label, {255, 255, 255, 255});
        Paint(image, ox, oy + labelHeight, view, frame);
        Json f = Json::MakeObject();
        f["label"] = poses[i].label;
        if (!poses[i].clip.empty()) {
            f["clip"] = poses[i].clip;
            f["time"] = poses[i].time;
        }
        frames.push(f);
    }
    Json out = Json::MakeObject();
    out["out"] = WriteToolImage(engine, args, ".oe/vision/poses.png", image);
    out["width"] = image.width;
    out["height"] = image.height;
    out["frames"] = frames;
    return out;
}

}  // namespace oe
