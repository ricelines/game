"""The bake module reads only the shared non-secret prototype configuration."""

import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_config(path: Path | None = None) -> dict:
    with (path or ROOT / "config.toml").open("rb") as stream:
        return tomllib.load(stream)["bake"]
