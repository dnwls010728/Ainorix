"""Generates the game's source pictures with Codex image generation, one `codex exec` per prompt of
tools/art_prompts.json, and copies each result to tools/art_src/codex/<name>.png. Codex only calls
its image tool (read-only sandbox, no shell); this script finds the picture in
~/.codex/generated_images/<session>/. Existing files are skipped, so a run can be resumed.

    python tools/gen_art.py [name ...] [--jobs 3]
"""
import glob
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "tools", "art_src", "codex")
GENERATED = os.path.join(os.path.expanduser("~"), ".codex", "generated_images")

CANVAS = {
    "hero": "portrait 1024x1536", "npc_villager": "portrait 1024x1536", "npc_farmer": "portrait 1024x1536",
    "npc_girl": "portrait 1024x1536", "pine": "portrait 1024x1536", "birch": "portrait 1024x1536",
    "lamp": "portrait 1024x1536", "plants": "landscape 1536x1024", "bush": "landscape 1536x1024",
    "bush_berry": "landscape 1536x1024", "rock": "landscape 1536x1024", "door": "portrait 1024x1536",
    "mountains_far": "landscape 1536x1024", "mountains_near": "landscape 1536x1024",
}


def generate(name, prompt):
    target = os.path.join(OUT, name + ".png")
    if os.path.exists(target):
        return name, "exists"
    canvas = CANVAS.get(name, "square 1024x1024")
    text = ("Generate exactly one image with your native image generation tool. Do not run any shell commands and do not "
            "read or write files; only call the image generation tool once and then reply with the word DONE.\n\n"
            "Canvas: " + canvas + ".\n\nImage prompt:\n" + prompt + "\n")
    cmd = "codex exec --skip-git-repo-check -c \"sandbox_mode='read-only'\" -"
    run = subprocess.run(cmd, input=text, capture_output=True, text=True, encoding="utf-8", errors="replace",
                         shell=True, timeout=1500)
    log = run.stdout + run.stderr
    match = re.search(r"session id:\s*([0-9a-f-]{36})", log)
    if not match:
        return name, "no session id"
    images = sorted(glob.glob(os.path.join(GENERATED, match.group(1), "*.png")), key=os.path.getmtime)
    if not images:
        return name, "no image (" + log.strip().splitlines()[-1][:120] + ")"
    os.makedirs(OUT, exist_ok=True)
    shutil.copyfile(images[-1], target)
    return name, "ok " + images[-1]


def main():
    args = sys.argv[1:]
    jobs = 3
    if "--jobs" in args:
        i = args.index("--jobs")
        jobs = int(args[i + 1])
        del args[i:i + 2]
    with open(os.path.join(ROOT, "tools", "art_prompts.json"), encoding="utf-8") as f:
        prompts = json.load(f)["prompts"]
    names = args or list(prompts)
    os.makedirs(OUT, exist_ok=True)
    with ThreadPoolExecutor(jobs) as pool:
        for name, status in pool.map(lambda n: generate(n, prompts[n]), names):
            print(name, status, flush=True)


if __name__ == "__main__":
    main()
