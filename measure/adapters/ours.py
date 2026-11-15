"""S7 native log adapter; causal metadata is read, never reconstructed recursively."""

from pathlib import Path

from measure.io import json_lines


class Adapter:
    def __init__(self, path, *, period_s=3600, source_integrity=None, **_options):
        self.path = Path(path)
        self.header = next(json_lines(path), {})
        self.period_s = period_s
        self.source_integrity = source_integrity

    def info(self):
        return {
            "name": "ours",
            "t0": self.header.get("t0", 0),
            "period_s": self.period_s,
            "seed_actors": self.header.get("seed_actors", []),
            "roles": self.header.get("roles", {}),
            "classes": {"move_to": "move", "base.move_to": "move"},
            "supports_gap_rate": True,
            "source": str(self.path),
            **({"source_integrity": self.source_integrity} if self.source_integrity else {}),
        }

    def events(self):
        roots, objects = {}, {}
        step_hz = float(self.header.get("step_hz", 20))
        for record in json_lines(self.path):
            if "kind" not in record:
                continue
            kind = record["kind"]
            actor = record.get("actor", record.get("maker", "system"))
            identity = record.get("id")
            roots[identity] = actor
            when = float(record.get("time_s", record.get("t", record.get("step", 0) / step_hz)))
            things = record.get("things", [])
            target = things[0] if things else None
            if kind == "create" and record.get("of") in ("model", "script"):
                target = record.get(record["of"], record.get("object", target))
            if kind == "create" and target is not None:
                objects[target] = {
                    "maker": record.get("maker", actor),
                    "created": when,
                    "kind": record.get("model", target),
                }
            event = {
                "t": when,
                "actor": actor,
                "type": record.get("name", kind),
                "object": target,
                "agent": record.get(
                    "agent",
                    kind in ("action", "speech", "observation", "denial", "gap")
                    or kind == "create"
                    and record.get("of") in ("model", "script"),
                ),
                "id": identity,
            }
            event.update(objects.get(target, {}))
            for field in ("result", "authors", "oldest", "cause", "root"):
                # Native S7 has no non-seed oldest time when it writes null.
                # That is absent optional metadata in S11, not a numeric value.
                if field in record and not (field == "oldest" and record[field] is None):
                    event[field] = record[field]
            # S7 diagnostics can omit result while explicitly reporting no stored
            # change. Preserve chosen attempts and explicit outcomes, but do not
            # turn such diagnostics into applied changes via S11's missing default.
            if record.get("changed") is False and record.get("result") is None:
                event["result"] = "no_effect"
            event["root_actor"] = roots.get(record.get("root"), actor)
            if kind == "speech":
                event["class"] = "speak"
            elif kind == "observation":
                event["class"] = "look"
            elif event["type"] in ("move_to", "base.move_to"):
                event["class"] = "move"
            else:
                event["class"] = "change"
            yield event
