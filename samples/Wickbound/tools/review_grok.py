"""Record the inspected Grok candidates; never infer motion acceptance from tests."""
import hashlib
import json
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
RUNS = PROJECT.parents[1] / "build/wickbound-grok"
ACCEPTED = {
    "keeper_ada": ("wide", False, "Alternating foot poses and continuous intermediate steps replace the four kick poses. Head/lantern remain substantially stable. Residual small sideways step discontinuity was reported before RIFE installation; physically accurate contact is not certified."),
    "keeper_bram": ("wide", False, "Continuous shuffling steps preserve the broad body and lantern. Sprite-gen reports 1.17% head displacement at one step; this is recorded as residual motion, not silently treated as perfect registration."),
    "enemy_brute": ("wide-retry", False, "The first flat/nonperiodic motion failed. The revised 31-frame quadruped cycle alternates paw positions without the large squash/jump poses of the first atlas. Residual 0.93% head displacement and gait-review warning remain; contact physics is not certified."),
    "enemy_shade": ("wide", True, "Continuous floating animation replaces independent full-body pose fitting. Round core remains substantially stable while tips/tufts flow. Idle and movement intentionally use the same floating loop."),
    "enemy_wisp": ("wide", True, "Continuous fire-tip motion keeps a substantially stable face/core. Idle and movement intentionally share a floating flame cycle rather than inventing walking legs."),
    "boss_eclipse": ("wide", True, "The original single-eyed moon identity is retained. Fire crown moves continuously around a substantially stable sphere. Idle/movement share the hover cycle."),
    "boss_king": ("wide", False, "Continuous short shuffling steps replace separated oversized/undersized cloak poses. Crown/face remain substantially stable; physical contact is not certified."),
}


def main():
    index = {}
    for name, (variant, floating, note) in ACCEPTED.items():
        run = RUNS / (name + "-" + variant)
        states = {"idle": "idle", "move": "idle" if floating else "walk"}
        evidence = {}
        for state in set(states.values()):
            folder = run / ("front_diagonal-" + state)
            report = json.loads((folder / "loop.report.json").read_text(encoding="utf-8"))
            assert report["status"] == "passed", (name, state)
            strip = folder / "loop" / ("front_diagonal-" + state + ".strip.png")
            evidence[state] = {"sha256": hashlib.sha256(strip.read_bytes()).hexdigest(),
                               "report": (folder / "loop.report.json").relative_to(PROJECT.parents[1]).as_posix()}
        review = {"accepted": True, "basis": "Visual frame and native playback review; improvement over the rejected first atlas, with documented residual limitations.",
                  "states": states, "evidence": evidence, "notes": note}
        (run / "motion-review.json").write_text(json.dumps(review, indent=2) + "\n")
        index[name] = run.name
    (RUNS / "accepted.json").write_text(json.dumps(index, indent=2) + "\n")


if __name__ == "__main__":
    main()
