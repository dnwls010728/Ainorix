"""Writes tools/art_prompts.json: the prompts used with Codex image generation for every picture of
the game (characters, trees, plants, terrain and house textures, mountains, clouds). The generated
full-size images go to tools/art_src/codex/<name>.png; tools/import_art.py turns them into the
game's textures.

    python tools/art_prompts.py
"""
import json
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

SPRITE = ("16-bit SNES-era JRPG pixel art for an HD-2D game (Octopath-like village), crisp square pixel clusters, "
          "dark one-logical-pixel outline tinted by the neighbouring colour, light from the upper left, restrained palette, "
          "no anti-aliasing, no smooth gradients. Genuine transparent background (alpha), no ground, no cast shadow, no halo, "
          "no text, no labels, no watermark, no border or frame. The whole subject fits inside with a transparent margin.")
PLAIN = ("16-bit SNES-era JRPG pixel art texture, flat albedo with even neutral lighting (no directional light, no cast "
         "shadows, no vignette, no perspective), crisp square pixel clusters, no anti-aliasing or blur, fully opaque. "
         "No text, border, frame or watermark.")
TILE = PLAIN + " Seamless: everything leaving one edge continues on the opposite edge."


def sheet(desc):
    return ("Asset type: production pixel-art CHARACTER SPRITE SHEET. Create ONE sheet, exactly 4 columns x 3 rows = twelve "
            "separate sprites in equal cells of 24x36 logical pixels (whole logical sheet 96x108; it may be enlarged only with "
            "uniform integer square pixel blocks). Character: " + desc + " Chibi JRPG proportions: big head (about 13 px), "
            "body about 32 logical pixels tall, IDENTICAL design, proportions and palette in every cell. Row 1: all face the "
            "camera (down). Row 2: all face RIGHT in true profile. Row 3: all face AWAY (up, back view). In each row column 1 "
            "and column 3 are the neutral standing pose; column 2 steps with the left leg forward, column 4 with the right leg "
            "forward (opposite arm swing). Same head height and same foot baseline in every cell of a row; feet centred at the "
            "cell bottom; no overlap between cells; no grid lines. " + SPRITE)


def single(desc, size):
    return ("Asset type: ONE billboard game sprite, logical size " + size + " pixels (may be enlarged only with uniform square "
            "pixel blocks). Orthographic front view. Subject: " + desc + " " + SPRITE)


def tile(desc, size="32x32"):
    return ("Asset type: seamlessly tileable " + size + " logical pixel texture (may be enlarged with uniform square pixel "
            "blocks). " + desc + " " + TILE)


def strip(desc):
    return ("Asset type: horizontal seamless panorama strip, 1024x128 logical pixels (may be enlarged with uniform square "
            "pixel blocks), for a sky backdrop. " + desc + " Fully transparent sky above the silhouette. The left and right "
            "ends join seamlessly. Pixel art, no sky colour, no sun, no text or frame.")


PROMPTS = {
    "hero": sheet("young hero adventurer, auburn red-brown messy hair, royal blue tunic with a gold belt and gold collar trim, "
                  "brown trousers, dark brown boots, short red cape visible on the back and in profile."),
    "npc_villager": sheet("cheerful village man, short golden-blond hair, red tunic with cream collar and cream belt, slate grey "
                          "trousers, dark boots."),
    "npc_farmer": sheet("village farmer, dark brown hair, wide straw sun hat with a gold band, green tunic with gold belt, brown "
                        "trousers, brown boots."),
    "npc_girl": sheet("village girl, long pink hair down to the waist, purple dress with a knee-length skirt and white collar, "
                      "dark boots."),
    "oak": single("a lush broadleaf oak tree: big rounded layered canopy of clumped foliage lumps in five greens (dark #1e4a2a "
                  "to light #a4d66a), sturdy brown trunk with a slight root flare, two short branches visible under the canopy. "
                  "Trunk base centred at the bottom edge.", "64x76"),
    "oak_lime": single("a young broadleaf tree with a bright lime-green rounded clumpy canopy (#2a5a2a to #d0f080) and a brown "
                       "trunk, trunk base centred at the bottom edge.", "64x76"),
    "oak_autumn": single("an autumn broadleaf tree with a rounded clumpy canopy in orange, rust and golden yellow (#6a2418 to "
                         "#ffd47c) and a brown trunk, trunk base centred at the bottom edge.", "64x76"),
    "oak_blossom": single("a cherry blossom tree with a rounded clumpy canopy of soft pink blossoms (#8a3a5a to #ffdde4) and a "
                          "dark brown trunk, trunk base centred at the bottom edge.", "64x76"),
    "pine": single("a tall conifer pine tree: six stacked tiers of dark teal-green needle foliage narrowing to a point, short "
                   "brown trunk, trunk base centred at the bottom edge.", "44x84"),
    "birch": single("a slender birch tree with a white bark trunk with black marks and a light lime-green rounded clumpy canopy, "
                    "trunk base centred at the bottom edge.", "48x72"),
    "bush": single("a low round green bush made of three leafy lumps. Base flat and centred at the bottom edge.", "30x18"),
    "bush_berry": single("a low round lime-green bush made of three leafy lumps with a few small red berries. Base flat and "
                         "centred at the bottom edge.", "30x18"),
    "rock": single("a small grey boulder made of two rounded stones in cool slate greys with a tiny tuft of moss on top. Base "
                   "flat and centred at the bottom edge.", "24x16"),
    "plants": ("Asset type: ONE pixel-art sprite strip, exactly 6 columns x 1 row of equal 16x16 logical pixel cells (logical "
               "image 96x16; may be enlarged with uniform square pixel blocks). Cell 1: a small grass tuft of five or six thin "
               "upright blades in greens. Cells 2-6: a single small flower on a short green stem with two leaves, flower head "
               "in, respectively, pink, yellow, white, light blue, violet, each with a pale yellow centre. Every plant stands "
               "on the bottom edge of its cell, centred, no overlap, no grid lines. " + SPRITE),
    "lamp": single("a village street lamp: slender dark iron post on a small square foot, a square lantern cage at the top with "
                   "warm glowing yellow glass (#ffd070, centre #fff2c0) and a little pointed cap. Base centred at the bottom "
                   "edge. No external glow.", "12x32"),
    "grass_top": tile("Top-down meadow grass surface, mid greens (#4a9438 to #9adc5e) in soft posterised blotches with a few "
                      "tiny bright grass blade marks and darker specks."),
    "grass_side": tile("Side face of a raised terrain block: the top quarter is a fringe of green grass blades hanging over the "
                       "edge with a darker green lower rim, the rest is brown layered earth (#6e5038 to #a07d58) with soft "
                       "horizontal strata and small pebbles. Must tile horizontally (left/right edges match); the top edge is "
                       "the grass, the bottom edge is earth."),
    "earth": tile("Side face of a terrain block: brown layered earth (#6e5038 to #a07d58), soft horizontal strata, small darker "
                  "and lighter pebbles."),
    "stone": tile("Grey rocky cliff stone (#646a78 to #a0a6b2) with soft horizontal strata and a few thin dark cracks."),
    "sand": tile("Top-down pale beach sand (#d0b87c to #f1e0aa) in soft blotches with small darker grain dashes."),
    "cobble": tile("Top-down warm beige cobblestone plaza: about sixteen irregular rounded stones (#a9967c to #d6c6aa) separated "
                   "by thin dark brown mortar lines (#6e5f4c)."),
    "plaster": tile("Half-timbered house wall: cream plaster (#e0d2b0 to #f7efdc) with a dark brown timber beam along the left "
                    "edge, the top edge and the bottom edge, and one diagonal brace beam rising from lower left to upper right."),
    "roof": tile("Red clay roof tiles seen straight on: four rows of staggered rectangular tiles in reds (#7c2f28 to #e07a5c), "
                 "each with a lighter top highlight and a dark lower edge."),
    "door": ("Asset type: one 16x24 logical pixel texture of a wooden house door seen straight on, filling the whole image: "
             "vertical brown planks, dark frame, two dark horizontal braces, small gold round handle on the right. " + PLAIN),
    "window": ("Asset type: one 16x16 logical pixel texture of a small house window seen straight on, filling the whole image: "
               "dark brown wooden frame, four panes of warm glowing yellow glass (lighter at the top, more orange at the "
               "bottom) divided by a dark cross. " + PLAIN),
    "mountains_far": strip("A soft rolling distant mountain range silhouette filled in near-white light grey (#ffffff at the "
                           "peaks fading to #a0a0aa at the bottom), gentle shading only, no trees."),
    "mountains_near": strip("A nearer hill ridge silhouette with a serrated line of tiny conifer tree tips along its crest, "
                            "filled in light grey (#d6d6dc at the top fading to #a0a0aa at the bottom)."),
    "moon": ("Asset type: ONE round full moon disc for a night sky, logical size 32x32 pixels (may be enlarged only with uniform "
             "square pixel blocks). Pale cream-white moon (#fff8e0 to #d8d0b8) with a few soft grey craters and maria, crisp pixel-art "
             "circle edge, no outline, no glow or halo, no stars. Genuine transparent background, no text, frame or watermark."),
    "clouds": ("Asset type: seamlessly tileable 256x256 greyscale cloud coverage texture for a sky shader: soft fluffy cumulus "
               "cloud shapes in white on black, about 40 percent coverage, varied sizes, soft painterly edges. Tiles seamlessly "
               "in both axes. Fully opaque, no text, border or frame."),
}

if __name__ == "__main__":
    out = {"native_image_generation": True, "output_dir": "tools/art_src/codex", "prompts": PROMPTS}
    with open(os.path.join(ROOT, "tools", "art_prompts.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(out, f, indent=1)
        f.write("\n")
    print(len(PROMPTS), "prompts")
