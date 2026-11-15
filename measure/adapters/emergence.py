"""Published tool calls with a source-audited class table; S11 formulas stay unchanged."""

import json
import zipfile
from pathlib import Path

from measure.io import seconds

CLASSES = json.loads(Path(__file__).with_name("emergence-classes-v1.json").read_text())
CLASSIFICATION = json.loads(Path(__file__).with_name("emergence-classification.json").read_text())


class Adapter:
    def __init__(self, path, *, period_s=3600, name=None, roles=None, **_options):
        self.path = Path(path)
        if self.path.suffix == ".zip":
            with zipfile.ZipFile(self.path) as archive:
                files = [
                    name
                    for name in archive.namelist()
                    if name.endswith(".json") and not name.startswith("__MACOSX/")
                ]
                if len(files) != 1:
                    raise ValueError("Expected one world JSON database in the published archive")
                self.rows = json.loads(archive.read(files[0]))
        else:
            self.rows = json.loads(self.path.read_text())
        if not isinstance(self.rows, list):
            raise ValueError("Expected a published list of tool call rows")
        self.rows.sort(key=lambda row: seconds(row["timestamp"]))
        self.period_s, self.name, self.roles = period_s, name or self.path.stem, roles or {}

    def info(self):
        return {
            "name": "emergence:" + self.name,
            "t0": seconds(self.rows[0]["timestamp"]) if self.rows else 0,
            "period_s": self.period_s,
            "seed_actors": [],
            "roles": self.roles,
            "classes": CLASSES,
            "source": str(self.path),
            "classification": CLASSIFICATION,
            "adapter_revision": CLASSIFICATION["adapter_revision"],
            "source_schema": "name,timestamp,tool_name,target_agent,location",
            "limitations": [
                (
                    "Unstructured tool_response text is not guessed into "
                    "applied/denied results; missing results follow frozen S11."
                ),
                "Unrecorded maker/creation metadata uses frozen first-event inference.",
                "No causal links; chain measures are lower bounds.",
            ],
        }

    def events(self):
        for row in self.rows:
            yield {
                "t": seconds(row["timestamp"]),
                "actor": row["name"],
                "type": row["tool_name"],
                "object": row.get("target_agent")
                if row.get("target_agent") is not None
                else row.get("location"),
            }
