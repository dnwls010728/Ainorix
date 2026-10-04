"""Pack reviewed sprite-gen strips into engine grids without changing poses/timing."""
import argparse
import json
import math
from pathlib import Path
import shutil

from PIL import Image

PROJECT = Path(__file__).resolve().parents[1]
BUILD = PROJECT.parents[1] / "build"
DEST = PROJECT / "assets/sprites/animated"


def lua(value):
    if isinstance(value, dict):
        return "{" + ",".join("[" + json.dumps(k) + "]=" + lua(v) for k, v in value.items()) + "}"
    if isinstance(value, list):
        return "{" + ",".join(map(lua, value)) + "}"
    if isinstance(value, bool):
        return "true" if value else "false"
    return json.dumps(value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", required=True, help="Comma-separated reviewed assets")
    args = parser.parse_args()
    entries = json.loads((DEST / "animations.json").read_text(encoding="utf-8"))
    staged = {}
    for name in args.only.split(","):
        index = json.loads((BUILD / "wickbound-grok/accepted.json").read_text(encoding="utf-8"))
        run = BUILD / "wickbound-grok" / index[name]
        review = json.loads((run / "motion-review.json").read_text(encoding="utf-8"))
        if review.get("accepted") is not True:
            raise ValueError(f"{name}: motion review did not accept this run")
        mapping = review["states"]
        sources = {}
        for state, source in mapping.items():
            folder = run / ("front_diagonal-" + source)
            report = json.loads((folder / "loop.report.json").read_text(encoding="utf-8"))
            if report.get("status") != "passed":
                raise ValueError(f"{name}/{state}: loop report has not passed")
            strip = folder / "loop" / ("front_diagonal-" + source + ".strip.png")
            meta = json.loads(strip.with_suffix(".json").read_text(encoding="utf-8"))
            if max(meta["w"], meta["h"]) > 320:
                raise ValueError(f"{name}/{state}: cell exceeds engine grid; do not independently shrink poses")
            sources[state] = (Image.open(strip).convert("RGBA"), meta)
        count = sum(meta["frames"] for _, meta in sources.values())
        columns, rows = 8, 16 if name.startswith("keeper_") else math.ceil(count / 8)
        if count > columns * rows:
            raise ValueError(f"{name}: too many frames")
        cell = 320
        sheet = Image.new("RGBA", (columns * cell, rows * cell))
        clips, rects, index = {}, {}, 0
        for state, (strip, meta) in sources.items():
            frames = []
            rects[state] = []
            for i in range(meta["frames"]):
                frame = strip.crop((i * meta["w"], 0, (i + 1) * meta["w"], meta["h"]))
                x, y = index % columns * cell, index // columns * cell
                # A fixed standing-height anchor keeps the body near the entity
                # center instead of pushing it down when the grid gets wider.
                baseline = (cell + meta["body_height_target"]) // 2
                if meta["h"] > baseline:
                    raise ValueError(f"{name}/{state}: motion exceeds the fixed anchor canvas")
                sheet.alpha_composite(frame, (x + (cell - meta["w"]) // 2, y + baseline - meta["h"]))
                frames.append(index)
                rects[state].append({"x": x, "y": y, "w": cell, "h": cell})
                index += 1
            clips[state] = {"frames": frames, "fps": 1000 / meta["delay_ms"], "loop": meta["loop"]}
        entry = {"texture": f"assets/sprites/animated/{name}.png", "columns": columns, "rows": rows, "clips": clips, "sizeScale": cell / 256}
        manifest = {"frame_layout": {"sheetWidth": sheet.width, "sheetHeight": sheet.height,
                    "cellWidth": cell, "cellHeight": cell, "rows": rects},
                    "animation": {"rows": clips}, "source": run.relative_to(PROJECT.parents[1]).as_posix(), "provider": "grok-login"}
        staged[name] = (sheet, entry, manifest, review)
    backup = BUILD / "wickbound-grok/installed-before-grok"
    if not backup.exists():
        shutil.copytree(DEST, backup)
    for name, (sheet, entry, manifest, review) in staged.items():
        sheet.save(DEST / (name + ".png"))
        (DEST / (name + ".manifest.json")).write_text(json.dumps(manifest, indent=2) + "\n")
        (DEST / (name + ".motion-review.json")).write_text(json.dumps(review, indent=2) + "\n")
        (DEST / (name + ".qa.md")).write_text(review["notes"] + "\n")
        entries[name] = entry
    (DEST / "animations.json").write_text(json.dumps(entries, indent=2) + "\n")
    (PROJECT / "scripts/lib/sprite_animations.lua").write_text(
        "-- Generated from assets/sprites/animated/animations.json.\nreturn " + lua(entries) + "\n")
    print("Installed reviewed Grok runs: " + ", ".join(staged))


if __name__ == "__main__":
    main()
