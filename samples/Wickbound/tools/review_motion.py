"""Regenerate motion candidates in a separate run; never overwrite installed art."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess
import sys

PROJECT = Path(__file__).resolve().parents[1]
RUNS = PROJECT.parents[1] / "build/wickbound-sprites-v2"

WALK = (
    "An eight-phase grounded WALK in place, not a run, jump or skipping motion. "
    "Frame phases: 1 left foot contact ahead/right behind; 2 left supporting weight; "
    "3 right foot passing beside supporting left; 4 right reaching forward; "
    "5 right contact ahead/left behind; 6 right supporting weight; "
    "7 left passing beside supporting right; 8 left reaching forward before phase 1. "
    "At least one sole contacts the same ground line in EVERY frame. No flight phase. "
    "Keep head orientation, head size, torso length and shoulder height constant; "
    "only 1 percent head bob. Lantern hand stays stable, free arm counter-swings subtly. "
    "Legs alternate; do not repeat the same leg lifted through all eight frames. "
)
MOTION = {
    "enemy_brute": "Slow eight-phase quadruped walk in place. Diagonally opposite paws alternate support, at least two paws on the floor always. Broad torso and head stay rigid and constant size. No bounding or jumping.",
    "enemy_stalker": "Eight-phase quadruped stalking walk in place. Front-left/hind-right then front-right/hind-left step. Always maintain paw contact, spine level, tail softly following. No leaping, crouching or enlarging head.",
    "enemy_leech": "Eight phases of a very subtle looping slug crawl. Keep head, eye antennae, height and total silhouette size fixed. Only a small traveling ripple along the lower belly; keep belly on the same ground baseline. Do not turn it into jumping or squash-stretch blobs.",
    "enemy_gloom": "Eight phases of subtle slime crawling: fixed eye sizes and unchanged main body height, a small traveling ripple in the skirt tentacles while the body stays on the same baseline. No drastic flattening/tall poses, no jump, no expression change.",
    "enemy_shade": "Eight phases of a floating spirit glide. Main body is rigid and fixed in scale and position; only hair tips, side tufts and tiny appendages gently follow a continuous sine cycle. No squash/stretch or rolling or pogo bounce.",
    "enemy_skitter": "Eight-phase little spirit scuttle, tiny feet alternate left-right against fixed floor; furry round head remains completely rigid at the same height and size. No spinning, flattening or changing expression.",
    "enemy_wisp": "Eight-phase flame-spirit hover. Face and main core stay perfectly fixed in shape, scale and position. Only flame tips softly trail in a continuous wave, returning seamlessly. No major head bob, rotating core or changing expression.",
    "boss_eclipse": "Eight-phase massive floating moon glide. Keep round moon sphere completely rigid, fixed radius, face shape and eye size and position. Only the flame crown and small orbit decorations ripple subtly in a seamless cycle. Never squash the sphere or move its center.",
    "boss_king": "Eight-phase slow dignified shuffling walk. Short legs alternate under the cloak, crown/head/torso remain rigid and fixed in size, cloak hem sways slightly. At least one foot grounded, no hopping or drastic forward lean, no expression change.",
}


def command(*args):
    subprocess.run([sys.executable, "-m", "sprite_gen.cli", *map(str, args)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only")
    parser.add_argument("--skip", default="")
    parser.add_argument("--workers", type=int, default=3)
    parser.add_argument("--process", action="store_true")
    args = parser.parse_args()
    sources = sorted((PROJECT / "assets/sprites").glob("*.png"))

    def process(source):
        name = source.stem
        if not name.startswith(("keeper_", "enemy_", "boss_")) and name != "brazier":
            return
        if args.only and name != args.only:
            return
        if name in args.skip.split(","):
            return
        run = RUNS / name
        identity = "Same camera angle, facial expression (eyes OPEN), head silhouette, head pixel size and main body silhouette throughout. Preserve the original number and shape of eyes and limbs. No new details, no smiling, no blinking, no zoom, no camera rotation. Keep all poses equally sized; do not scale down poses with outstretched legs. "
        if name == "keeper_suri":
            identity += "Preserve both butterfly companions close to hair in EVERY pose. "
        idle = {"frames": 4, "fps": 4, "loop": True, "action": identity + "Tiny quiet breathing cycle: neutral, very slight chest inhale, neutral, very slight exhale. Head stays rigid; amplitude under 1 percent. Feet never move. Same open-eye expression throughout, no shape morph."}
        move = {"frames": 8, "fps": 12, "loop": True, "action": identity + (WALK if name.startswith("keeper_") else MOTION.get(name, "")) + "The final phase leads into first naturally; do not duplicate the first frame as the last. No backwards playback or repeated identical poses."}
        states = {"idle": idle, "move": move}
        if name == "brazier":
            states = {"unlit": {"frames": 1, "fps": 1, "loop": True, "action": "Original campfire bowl without fire, same proportions."},
                      "lit": {"frames": 8, "fps": 12, "loop": True, "action": "Eight-phase gentle flame flicker. Stone bowl must be pixel-consistent in scale, position and proportions in every frame AND match the unlit bowl exactly. Only flames change subtly, ember intensity does not pulse drastically. Constant flame height, flame tips drift smoothly left then right then back. No alternating oversized/undersized bowl or flame."}}
        if not (run / "sprite-request.json").exists():
            command("prepare", "--out-dir", run, "--character-id", name, "--base-image", source,
                    "--cell-size", 256, "--safe-margin", 20, "--no-fit-pixel-unfake",
                    "--fit-resample", "lanczos", "--fit-align-x", "bbox-center",
                    "--style", "Original smooth storybook chibi illustration. Production animation, rigid stable facial design.",
                    "--request-json", json.dumps({"states": states}))
        if args.process:
            for stage in ("extract", "compose-atlas", "preview", "inspect"):
                command(stage, "--run-dir", run)
        else:
            command("gen-set", "--run-dir", run, "--provider", "codex", "--concurrency", 2)

    with ThreadPoolExecutor(max_workers=args.workers) as workers:
        list(workers.map(process, sources))


if __name__ == "__main__":
    main()
