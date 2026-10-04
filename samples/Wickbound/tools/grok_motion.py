"""Generate Grok motion from original RGBA sprites without redrawing the base.

Only the input chroma plate is composited here. sprite-gen owns canvas placement,
generation, extraction, cycle detection and loop export. Nothing is installed.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

from PIL import Image
from sprite_gen.video.batch import run_set, run_video_cli, build_prompt

PROJECT = Path(__file__).resolve().parents[1]
RUNS = PROJECT.parents[1] / "build/wickbound-grok"


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", default="keeper_ada")
    parser.add_argument("--states", default="idle,walk")
    parser.add_argument("--variant", default="wide")
    parser.add_argument("--batch", action="store_true")
    parser.add_argument("--source", type=Path)
    args = parser.parse_args()
    if args.batch:
        names = [p.stem for p in (PROJECT / "assets/sprites").glob("*.png")
                 if p.stem.startswith(("enemy_", "boss_")) and p.stem != "enemy_shade"]
        def worker(name):
            states = "idle" if name in ("enemy_wisp", "boss_eclipse") else "idle,walk"
            return subprocess.run([sys.executable, __file__, "--only", name,
                                   "--states", states]).returncode
        with ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(worker, names))
        raise SystemExit(int(any(results)))
    name = args.only
    source = args.source or PROJECT / "assets/sprites" / (name + ".png")
    run = RUNS / (name + "-" + args.variant)
    run.mkdir(parents=True, exist_ok=True)
    image = Image.open(source).convert("RGBA")
    # Fixed flat green plate for purple/yellow characters; original RGB/alpha
    # remain unchanged in the source. Green creatures need a separate key review.
    key = "magenta" if name == "enemy_leech" else "green"
    color = (255, 0, 255, 255) if key == "magenta" else (0, 255, 0, 255)
    plate = Image.new("RGBA", image.size, color)
    plate.alpha_composite(image)
    base = run / f"base-{key}.png"
    plate.convert("RGB").save(base)
    env = dict(os.environ, PYTHONUTF8="1")
    tools = PROJECT.parents[1] / "build/tools/libwebp-1.6.0-windows-x64/bin"
    links = Path.home() / "AppData/Local/Microsoft/WinGet/Links"
    env["PATH"] = os.pathsep.join((str(tools), str(links), env.get("PATH", "")))
    os.environ.update(env)
    command = [sys.executable, "-m", "sprite_gen.cli", "video-set",
               "--base", f"front_diagonal={base}", "--states", args.states,
               "--out-dir", str(run), "--character", "The original storybook character",
               "--resolution", "480p", "--duration", "3", "--body-height", "216",
               "--key", "green", "--anchor", "body", "--shape", "wide", "--walk-start", "as-given",
               "--still-provider", "grok", "--concurrency", "1", "--align-cycles", "off"]
    (run / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    def generate(image, prompt, out, report, **kwargs):
        state = out.parent.name.rsplit("-", 1)[1]
        motions = {
            "enemy_shade": "The round spirit floats gently in place. Its round body and eyes keep exactly the original size, shape, orientation and center. Only the flame tips and side tufts flow softly in a continuous wave. No walking, no legs, no spinning, no changing facial expression.",
            "enemy_wisp": "The flame spirit floats gently in place. The face and round core stay rigid in shape and scale. Only the flame tips drift softly in a seamless wave. Preserve all colors and expression, no legs, no turning.",
            "boss_eclipse": "The massive round moon floats in place. Its round sphere, single eye, mouth and decoration stay rigid in scale and shape. Only its flame crown gently flickers. No new limbs or new face, no turning or squash/stretch.",
            "brazier_flame": "The isolated warm orange flame flickers softly in place with stable overall height. Only its tips gently curl and wave. No bowl, no ground, no object, no new sparks, no smoke, no camera movement. Keep the base of the flame fixed at exactly the same point.",
        }
        motion = motions.get(name)
        if state == "walk" and name == "enemy_leech":
            motion = "The original slug crawls naturally in place with a small repeated ripple traveling along its underside. Repeat three complete identical gentle crawling cycles at an even rhythm. Keep its belly grounded, head and antennae stable. No legs, no bouncing or stretching the entire body, no ground line or shadow."
        if state == "walk" and name == "enemy_gloom":
            motion = "The original three-eyed slime crawls naturally in place. A gentle ripple travels through its skirt while all three eyes and the main body stay stable in scale and expression. No bouncing or growing taller. Background is entirely flat green, including under the belly. No floor, no ground line, no ellipse, no outline below the character, no shadow, no platform."
        if state == "walk" and name == "enemy_brute":
            motion = "The original round purple bear walks naturally on all four paws in place with an evenly repeating quadruped gait. Show three complete full gait cycles at the same steady pace. Keep its head and broad torso rigid in shape and scale. Paws alternate support, no hopping or rearing up."
        if state == "walk" and name == "enemy_stalker":
            motion = "The original fox walks naturally on all four paws in place, with evenly repeating full quadruped gait cycles. Keep the head at a constant center and height and constant size through all frames. No abrupt head sways, no lunging, no bounding. Maintain the exact original camera angle."
        if state == "walk" and name == "keeper_suri":
            motion = "The original lantern keeper walks naturally in place with alternating grounded feet. Keep eyes open in all frames. Keep the head rigid in shape and scale and at a stable center. Preserve both butterfly companions, keep them near the head, no rapid flutter or shape changes. No high kicks, no hopping."
        if motion:
            prompt = build_prompt("front_diagonal", state, "The original round spirit", motion=motion)
        (out.parent / "motion-prompt.txt").write_text(prompt, encoding="utf-8")
        return run_video_cli(image, prompt, out, report, **kwargs)
    result = run_set(bases={"front_diagonal": base}, states=args.states.split(","),
        root=run, character="The original storybook character", duration=3,
        resolution="480p", key=key, concurrency=1, force=False, gap=2,
        video_runner=generate, shape="wide", anchor="body", body_height=216,
        walk_start="as-given", still_provider="grok", align_cycles="off")
    if not all(item.get("ok") for item in result["items"]):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
