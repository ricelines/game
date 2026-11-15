"""The measures read only the shared non-secret prototype configuration."""

import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_config(path: Path | None = None) -> dict:
    with (path or ROOT / "config.toml").open("rb") as stream:
        return tomllib.load(stream)["measure"]


_CONFIG = load_config()
MIN_ACTIONS = _CONFIG["MIN_ACTIONS"]
WIKI_WORLDS = _CONFIG["WIKI_WORLDS"]
WIKI_BOOTSTRAPS = _CONFIG["WIKI_BOOTSTRAPS"]
