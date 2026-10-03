#!/usr/bin/env python3
"""Keep Belt's self-describing parameter pages complete for generic hosts."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFESTS = (
    ROOT / "modules/audio_fx/belt/module.json",
    ROOT / "modules/sound_generators/belt-in/module.json",
)
EXPECTED_KNOBS = {
    "root": ["key", "scale", "retune", "amount"],
    # midi_mode takes the harmony page's last free slot: it decides what
    # played notes do to these voices, so it belongs beside them
    "harmony": [
        "harm1", "harm2", "harm3", "harm4",
        "harm_level", "spread", "double_amt", "midi_mode",
    ],
    "setup": ["hard", "wet", "formant", "flex", "humanize", "vel_sens"],
}


def check_manifest(path: Path) -> None:
    data = json.loads(path.read_text())
    levels = data["capabilities"]["ui_hierarchy"]["levels"]
    assert set(levels) == set(EXPECTED_KNOBS), f"{path}: unexpected page set"

    for level_name, expected in EXPECTED_KNOBS.items():
        level = levels[level_name]
        actual = level.get("knobs", [])
        assert actual == expected, f"{path}: {level_name} knobs: {actual!r}"
        assert len(actual) <= 8, f"{path}: {level_name} exceeds Move's eight knobs"

        defined = {
            param["key"]
            for param in level.get("params", [])
            if isinstance(param, dict) and "key" in param
        }
        assert set(actual) == defined, (
            f"{path}: {level_name} hides params {sorted(defined - set(actual))}"
        )


for manifest in MANIFESTS:
    check_manifest(manifest)

print("manifest UI validation passed")
