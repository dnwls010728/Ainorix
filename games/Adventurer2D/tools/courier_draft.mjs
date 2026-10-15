// Applies the courier draft edits a template run cannot know, after:
//   oe exec games/Adventurer2D skeleton2d.draft_template "{\"kind\":\"humanoid-side\",\"image\":\"art/courier.png\",
//     \"out\":\"art/courier.skel2d-draft.json\",\"joints\":<art/courier-joints.json>}"
// Run: node games/Adventurer2D/tools/courier_draft.mjs games/Adventurer2D/art/courier.skel2d-draft.json

// Courier draft edits the template cannot know: the far arm is hidden (no part may cut it out of the torso), and
// the ponytail and the satchel get parts and bones of their own (springs swing them).
import fs from "node:fs";
const path = process.argv[2];
const draft = JSON.parse(fs.readFileSync(path, "utf8"));
draft.parts = draft.parts.filter((p) => p.name !== "arm_back" && p.name !== "hand_back");
draft.joints.tail_root = [440, 120];
draft.joints.tail_tip = [300, 400];
draft.joints.bag_root = [470, 615];
draft.joints.bag_tip = [420, 800];
draft.bones.push({ name: "ponytail", parent: "head", from: "tail_root", to: "tail_tip" });
draft.bones.push({ name: "satchel", parent: "chest", from: "bag_root", to: "bag_tip" });
// The ponytail behind everything, the satchel in front of the torso but behind the near arm.
draft.parts.unshift({
  name: "ponytail",
  polygon: [[350, 25], [470, 25], [480, 135], [430, 210], [405, 335], [335, 450], [230, 415], [205, 300], [215, 180], [265, 45]],
  bone: "ponytail",
});
const torso = draft.parts.findIndex((p) => p.name === "torso");
draft.parts.splice(torso + 1, 0, { name: "satchel", polygon: [[345, 605], [500, 595], [505, 820], [345, 820]], bone: "satchel" });
fs.writeFileSync(path, JSON.stringify(draft, null, 2) + "\n");
// The near leg is traced along its outline (a band around the bones would take the far leg it overlaps), down to
// the ankle: the boot's foot belongs to foot_front.
{
  const d = JSON.parse(fs.readFileSync(path, "utf8"));
  const leg = d.parts.find((p) => p.name === "leg_front");
  leg.polygon = [[468, 790], [565, 790], [560, 900], [545, 1000], [532, 1100], [515, 1180], [515, 1300], [540, 1395],
                 [388, 1395], [390, 1290], [395, 1190], [410, 1100], [430, 1000], [455, 900]];
  fs.writeFileSync(path, JSON.stringify(d, null, 2) + "\n");
}
// The near arm and hand lie over the tunic: traced along their outlines so they do not carry tunic and leggings with them.
{
  const d = JSON.parse(fs.readFileSync(path, "utf8"));
  d.parts.find((p) => p.name === "arm_front").polygon = [[440, 450], [555, 450], [560, 520], [515, 560], [515, 700], [545, 790], [548, 830],
                                                       [510, 835], [480, 760], [468, 640], [462, 540], [440, 520]];
  d.parts.find((p) => p.name === "hand_front").polygon = [[505, 790], [555, 795], [575, 850], [565, 905], [535, 905], [515, 860], [500, 820]];
  fs.writeFileSync(path, JSON.stringify(d, null, 2) + "\n");
}
