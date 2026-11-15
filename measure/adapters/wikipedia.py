"""Official MediaWiki history TSV order, or its named-field JSON projection."""

import csv
import json
from pathlib import Path

from measure.io import chronological, json_lines, seconds, text_stream

COLUMNS = json.loads(Path(__file__).with_name("wiki-columns.json").read_text())


class Adapter:
    def __init__(
        self,
        path,
        *,
        period_s=604800,
        t0=1704067200,
        end_s=None,
        pages=None,
        columns=None,
        **_options,
    ):
        self.path, self.period_s, self.t0 = Path(path), period_s, t0
        self.end_s = end_s
        self.pages = {str(page) for page in pages} if pages is not None else None
        self.columns = columns or COLUMNS
        self.source_counts = {
            "input_rows": 0,
            "excluded_other_entity": 0,
            "excluded_missing_author_or_page": 0,
            "excluded_outside_selected_pages": 0,
            "events": 0,
        }

    def info(self):
        return {
            "name": "wikipedia",
            "t0": self.t0,
            "period_s": self.period_s,
            **({"end_s": self.end_s} if self.end_s is not None else {}),
            "seed_actors": [],
            "classes": {
                "create": "change",
                "create-page": "change",
                "delete": "change",
                "move": "change",
                "restore": "change",
                "merge": "change",
                "revert": "change",
            },
            "class_rule": "talk namespaces (odd numeric namespace) are speak; all others change",
            "source": str(self.path),
            "source_counts": self.source_counts,
            "limitations": [
                (
                    "Anonymous/deleted event_user_id is unavailable and "
                    "excluded; IP addresses are not substituted."
                ),
                "No recorded maker uses the frozen first-event inference.",
                "No cause links; cross-author/lookback rely on minimal-record metadata.",
            ],
        }

    def _rows(self):
        if self.path.name.endswith((".ndjson", ".jsonl", ".jsonl.zst")):
            yield from json_lines(self.path)
        else:
            with text_stream(self.path) as source:
                for fields in csv.reader(source, delimiter="\t", quoting=csv.QUOTE_NONE):
                    if fields and fields[0] == "wiki_db":
                        self.columns = fields
                        continue
                    if len(fields) != len(self.columns):
                        raise ValueError(
                            f"MediaWiki TSV has {len(fields)} columns; "
                            "provide exact columns in --options instead of guessing"
                        )
                    yield dict(zip(self.columns, fields, strict=True))

    def events(self):
        yield from chronological(self._events())

    def _events(self):
        for row in self._rows():
            self.source_counts["input_rows"] += 1
            if row["event_entity"] not in ("page", "revision"):
                self.source_counts["excluded_other_entity"] += 1
                continue
            if (
                row.get("event_user_id") in (None, "", "0", 0)
                or str(row.get("event_user_is_anonymous", "")).lower() == "true"
                or not row.get("page_id")
            ):
                self.source_counts["excluded_missing_author_or_page"] += 1
                continue
            if self.pages is not None and str(row["page_id"]) not in self.pages:
                self.source_counts["excluded_outside_selected_pages"] += 1
                continue
            kind = (
                "revert"
                if str(row.get("revision_is_identity_revert", "")).lower() == "true"
                else row["event_type"]
            )
            event = {
                "t": seconds(row["event_timestamp"]),
                "actor": str(row["event_user_id"]),
                "type": kind,
                "object": str(row["page_id"]),
                "class": "speak" if int(row["page_namespace_historical"]) % 2 else "change",
            }
            if row.get("page_first_edit_timestamp"):
                event["created"] = seconds(row["page_first_edit_timestamp"])
            if int(row["page_namespace_historical"]) in (10, 828):
                event["kind"] = str(row["page_id"])
            self.source_counts["events"] += 1
            yield event
