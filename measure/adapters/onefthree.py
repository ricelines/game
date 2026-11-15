"""1F3D9 public snapshot v1–v4 envelopes; no private/live service reads."""

import hashlib
import json
from pathlib import Path

from measure.io import chronological, json_lines, seconds

CLASSES = {
    name: "change"
    for name in (
        "action agreement agreement_accession agreement_sign effect_resolved "
        "effect_scheduled flag gazette_printed home_set kind_invented "
        "kind_revised laws_changed moderation payment_repair place_created "
        "place_edited place_renamed register resident_edited rotate "
        "thing_crafted thing_created thing_edited thing_ended thing_moved "
        "thing_upgraded thing_withdrawn trait_coined transfer transfer_offer "
        "world_cancel world_listed world_sale use make give consume"
    ).split()
}
CLASSES.update({name: "speak" for name in "line_said note ping_answered ping_sent talk".split()})
CLASSES.update({"move": "move", "go_home": "move"})


class Adapter:
    def __init__(self, path, *, period_s=3600, seed_actors=(), **_options):
        self.path = Path(path)
        self.period_s, self.seeds = period_s, seed_actors
        self.manifest = json.loads((self.path / "manifest.json").read_text())
        self.objects = {}
        for envelope in json_lines(self.path / "things.ndjson"):
            row = envelope["record"]
            if row.get("status") == "exported":
                self.objects[str(row["id"])] = {
                    "maker": row["made_by"],
                    "created": seconds(row["created_at"]),
                    "kind": "kind:" + str(row["kind_id"])
                    if row.get("kind_id") is not None
                    else "thing:" + str(row["id"]),
                }
        for entry in self.manifest.get("files", []):
            name = entry.get("name", entry.get("path", ""))
            if name in ("events.ndjson", "things.ndjson") and "sha256" in entry:
                actual = hashlib.file_digest((self.path / name).open("rb"), "sha256").hexdigest()
                if actual != entry["sha256"]:
                    raise ValueError("Snapshot artifact hash mismatch: " + name)
        self.t0 = min(
            (
                seconds(row["record"]["at"])
                for row in json_lines(self.path / "events.ndjson")
                if row["record"].get("status") == "exported"
            ),
            default=0,
        )

    def info(self):
        return {
            "name": "onefthree",
            "t0": self.t0,
            "period_s": self.period_s,
            "seed_actors": list(self.seeds),
            "classes": CLASSES,
            "source": str(self.path),
            "snapshot_version": self.manifest["format_version"],
            "source_commit": self.manifest.get("source_commit"),
            "limitations": [
                "Body-free hidden/sequence-gap markers have no attributable event and are omitted.",
                (
                    "Use records without an exported source_thing_id retain "
                    "object:null; no target is invented."
                ),
                "Public snapshots omit some runtime details and cause links.",
            ],
        }

    def events(self):
        yield from chronological(self._events())

    def _events(self):
        for envelope in json_lines(self.path / "events.ndjson"):
            row = envelope["record"]
            if row.get("status") != "exported":
                continue
            detail = row.get("detail", {})
            kind = detail.get("action", row["kind"]) if row["kind"] == "action" else row["kind"]
            thing = (
                detail.get("source_thing_id")
                if kind == "use"
                else detail.get("thing_id", detail.get("source_thing_id"))
            )
            target = "thing:" + str(thing) if thing is not None else None
            event = {
                "t": seconds(row["at"]),
                "actor": row["actor"],
                "type": kind,
                "object": target,
                "agent": row["kind"] not in ("effect_resolved", "effect_scheduled", "thing_ended")
                and row["actor"] != "the city",
            }
            if thing is not None:
                event.update(self.objects.get(str(thing), {}))
            status = detail.get("status")
            if status in ("applied", "blocked", "noop"):
                event["result"] = {"applied": "applied", "blocked": "denied", "noop": "no_effect"}[
                    status
                ]
            yield event
