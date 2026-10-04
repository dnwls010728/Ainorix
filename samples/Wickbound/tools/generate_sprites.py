"""Prepare/generate Wickbound animation rows with the installed sprite-gen skill.

Run with sprite-gen's virtualenv Python. Working runs stay in build/wickbound-sprites;
only reviewed atlases and manifests are installed into the sample.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import shutil
from concurrent.futures import ThreadPoolExecutor

PROJECT = Path(__file__).resolve().parents[1]
RUNS = PROJECT.parents[1] / "build" / "wickbound-sprites"


def command(*args):
    subprocess.run([sys.executable, "-m", "sprite_gen.cli", *map(str, args)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate", action="store_true")
    parser.add_argument("--only")
    parser.add_argument("--skip", default="", help="Comma-separated runs currently owned by another process")
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--process", action="store_true", help="Extract, compose and preview generated rows")
    parser.add_argument("--install", action="store_true", help="Install atlases after all motion QA notes exist")
    parser.add_argument("--refit", action="store_true", help="Center the complete silhouette inside each cell")
    parser.add_argument("--runs-dir", type=Path, default=RUNS, help="Candidate run directory to process/install")
    args = parser.parse_args()
    sources = sorted((PROJECT / "assets/sprites").glob("*.png"))
    if args.install:
        install(sources, args.runs_dir)
        return
    def process(source):
        name = source.stem
        if not name.startswith(("keeper_", "enemy_", "boss_")) and name != "brazier":
            return
        if args.only and name != args.only:
            return
        if name in args.skip.split(","):
            return
        states = {"idle": {"frames": 4, "fps": 4, "loop": True,
                            "action": "Subtle breathing loop, gentle blink; keep feet and accessory position stable."}}
        if name == "brazier":
            states = {
                "unlit": {"frames": 1, "fps": 1, "loop": True,
                          "action": "Unlit campfire bowl, retain the bowl exactly, no flame or glowing embers."},
                "lit": {"frames": 4, "fps": 8, "loop": True,
                        "action": "Warm flame flickers in a seamless loop; bowl and feet stay completely fixed."},
            }
        else:
            states["move"] = {"frames": 4, "fps": 8, "loop": True,
                              "action": "Four distinct consecutive phases of a gentle in-place movement loop, preserve reference orientation and identity, no travel across canvas; alternate steps for legs, undulate or flutter for floating creatures. Return seamlessly to first pose."}
        run = args.runs_dir / name
        if not (run / "sprite-request.json").exists():
            command("prepare", "--out-dir", run, "--character-id", name,
                    "--base-image", source, "--cell-size", "256", "--safe-margin", "12",
                    "--style", "Smooth illustrated storybook chibi art matching the supplied original, not pixel art",
                    "--no-fit-pixel-unfake", "--fit-resample", "lanczos",
                    "--fit-align-x", "bbox-center",
                    "--request-json", json.dumps({"states": states}))
        if args.generate:
            command("gen-set", "--run-dir", run, "--provider", "codex", "--concurrency", "2")
        if args.refit:
            request_path = run / "sprite-request.json"
            request = json.loads(request_path.read_text(encoding="utf-8"))
            request["fit"]["align_x"] = "bbox-center"
            request_path.write_text(json.dumps(request, indent=2) + "\n", encoding="utf-8")
        if args.process:
            for stage in ("extract", "compose-atlas", "preview", "inspect"):
                command(stage, "--run-dir", run)

    with ThreadPoolExecutor(max_workers=args.workers) as workers:
        list(workers.map(process, sources))


def install(sources, runs_dir=RUNS):
    entries = {}
    for source in sources:
        name = source.stem
        run = runs_dir / name
        if not name.startswith(("keeper_", "enemy_", "boss_")) and name != "brazier":
            continue
        notes = run / "qa-notes.md"
        if not notes.exists():
            raise ValueError(f"Review motion and write {notes} before installing")
        manifest = json.loads((run / "manifest.json").read_text(encoding="utf-8"))
        report = json.loads((run / "sprite-inspect.report.json").read_text(encoding="utf-8"))
        if not report["ok"] or report["errors"]:
            raise ValueError(f"{name}: unresolved sprite inspection findings")
        layout = manifest["frame_layout"]
        width, height = layout["cellWidth"], layout["cellHeight"]
        columns, rows = layout["sheetWidth"] // width, layout["sheetHeight"] // height
        clips = {}
        for state, rects in layout["rows"].items():
            frames = []
            for rect in rects:
                if rect["w"] != width or rect["h"] != height or rect["x"] % width or rect["y"] % height:
                    raise ValueError(f"{name}/{state}: engine requires equal aligned cells")
                frames.append(rect["y"] // height * columns + rect["x"] // width)
            timing = manifest["animation"]["rows"][state]
            clips[state] = {"frames": frames, "fps": timing["fps"], "loop": timing["loop"]}
        # A deliberately single-frame unlit state has no motion to measure.
        unresolved = [warning for warning in report["warnings"]
                      if not any(len(clip["frames"]) == 1 and warning.startswith(state + ": motion presence is too low")
                                 for state, clip in clips.items())]
        if unresolved:
            verdict_path = run / "motion-review.json"
            verdict = json.loads(verdict_path.read_text(encoding="utf-8")) if verdict_path.exists() else {}
            unresolved = [warning for warning in unresolved if warning not in verdict.get("accepted_warnings", [])]
            if unresolved:
                raise ValueError(f"{name}: unresolved inspection findings: {unresolved}")
        verdict_path = run / "motion-review.json"
        if not verdict_path.exists():
            raise ValueError(f"{name}: explicit motion review is required before installing")
        verdict = json.loads(verdict_path.read_text(encoding="utf-8"))
        for state in clips:
            if verdict.get("states", {}).get(state, {}).get("accepted") is not True:
                raise ValueError(f"{name}/{state}: candidate has not passed motion review")
        entries[name] = {"texture": f"assets/sprites/animated/{name}.png", "columns": columns,
                         "rows": rows, "clips": clips}
    # Runtime keeper selection swaps only texture, so all keeper layouts must match.
    keepers = [entry for name, entry in entries.items() if name.startswith("keeper_")]
    for entry in keepers[1:]:
        if any(entry[key] != keepers[0][key] for key in ("columns", "rows", "clips")):
            raise ValueError("Keeper grids and clips must match for runtime selection")
    destination = PROJECT / "assets/sprites/animated"
    destination.mkdir(parents=True, exist_ok=True)
    for name in entries:
        for source_name, target_name in (("sprite-sheet-alpha.png", name + ".png"),
                                         ("manifest.json", name + ".manifest.json"),
                                         ("qa-notes.md", name + ".qa.md")):
            shutil.copyfile(runs_dir / name / source_name, destination / target_name)
        for source_name in ("motion-review.json", "sprite-request.json"):
            if (runs_dir / name / source_name).exists():
                shutil.copyfile(runs_dir / name / source_name, destination / (name + "." + source_name))
    (destination / "animations.json").write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
