"""Remove rejected installed sheets and restore the original single sprites."""
import json
from pathlib import Path
from install_grok import lua

PROJECT = Path(__file__).resolve().parents[1]
DEST = PROJECT / "assets/sprites/animated"
STATIC = ("keeper_suri", "enemy_gloom", "enemy_skitter", "enemy_stalker", "enemy_leech", "brazier")


def main():
    entries = json.loads((DEST / "animations.json").read_text(encoding="utf-8"))
    for name in STATIC:
        source = PROJECT / "assets/sprites" / (name + ".png")
        if not source.is_file():
            raise ValueError(f"Missing original sprite: {source}")
        entries[name] = {"texture": f"assets/sprites/{name}.png", "columns": 1, "rows": 1, "clips": {}}
    # Delete only these explicit installed artifacts. Generation evidence in build
    # and the original source PNGs are retained.
    for name in STATIC:
        for suffix in (".png", ".manifest.json", ".qa.md", ".motion-review.json", ".sprite-request.json"):
            target = DEST / (name + suffix)
            if target.resolve().parent != DEST.resolve():
                raise ValueError(f"Artifact outside animated folder: {target}")
            target.unlink(missing_ok=True)
    (DEST / "animations.json").write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")
    (PROJECT / "scripts/lib/sprite_animations.lua").write_text(
        "-- Generated from assets/sprites/animated/animations.json.\nreturn " + lua(entries) + "\n", encoding="utf-8")
    print("Restored single sprites: " + ", ".join(STATIC))


if __name__ == "__main__":
    main()
