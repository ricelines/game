"""MoltNet's published posts/comments schema; disk join avoids millions of Python objects."""

import json
import sqlite3
import tempfile
from pathlib import Path

from measure.io import rows, seconds


class Adapter:
    def __init__(self, path, *, period_s=3600, **_options):
        self.path, self.period_s = Path(path), period_s
        self.source_counts = {
            "input_rows": {},
            "excluded_missing_author": {},
            "excluded_missing_timestamp": {},
        }
        cache = Path(__file__).resolve().parents[2] / ".cache/measure-sort"
        cache.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=cache)
        self.db = sqlite3.connect(Path(self.temporary.name) / "moltbook.sqlite")
        self.db.execute(
            "CREATE TABLE records (id TEXT PRIMARY KEY, t REAL, author TEXT, "
            "parent TEXT, type TEXT)"
        )
        for kind, filename in (("post", "posts"), ("comment", "comments")):
            matches = list(self.path.glob("**/" + filename + ".parquet")) + list(
                self.path.glob("**/" + filename + ".ndjson")
            )
            if len(matches) != 1:
                raise ValueError("Provide exactly one published " + filename + " table")
            columns = ["id", "created_at", "author_id"] + (
                ["post_id", "parent_id"] if kind == "comment" else []
            )

            self.db.executemany(
                "INSERT INTO records VALUES(?,?,?,?,?)", self._entries(matches[0], columns, kind)
            )
        self.db.commit()
        self.t0 = self.db.execute("SELECT MIN(t) FROM records").fetchone()[0] or 0
        self.source_counts["orphan_reply_targets"] = self.db.execute(
            "SELECT COUNT(*) FROM records e LEFT JOIN records p ON e.parent=p.id "
            "WHERE e.parent IS NOT NULL AND p.id IS NULL"
        ).fetchone()[0]
        manifest = self.path / "source-manifest.json"
        self.source_manifest = json.loads(manifest.read_text()) if manifest.exists() else None

    def _entries(self, path, columns, kind):
        for row in rows(path, columns):
            for field in ("input_rows", "excluded_missing_author", "excluded_missing_timestamp"):
                self.source_counts[field].setdefault(kind, 0)
            self.source_counts["input_rows"][kind] += 1
            self.source_counts["excluded_missing_author"][kind] += row["author_id"] is None
            self.source_counts["excluded_missing_timestamp"][kind] += row["created_at"] is None
            if row["author_id"] is None or row["created_at"] is None:
                continue
            parent = (row.get("parent_id") or row.get("post_id")) if kind == "comment" else None
            yield (row["id"], seconds(row["created_at"]), row["author_id"], parent, kind)

    def info(self):
        return {
            "name": "moltbook",
            "t0": self.t0,
            "period_s": self.period_s,
            "seed_actors": [],
            "classes": {"post": "speak", "comment": "speak"},
            "source": str(self.path),
            "source_counts": self.source_counts,
            **({"published_release": self.source_manifest} if self.source_manifest else {}),
            "limitations": [
                "Only published posts/comments are events; votes are not synthesized.",
                "Rows lacking an author or timestamp cannot be attributed and are excluded.",
                "All events are speech; change-based measures are unavailable.",
            ],
        }

    def events(self):
        query = (
            "SELECT e.t,e.author,e.type,e.parent,p.author,p.t FROM records e "
            "LEFT JOIN records p ON e.parent=p.id ORDER BY e.t,e.id"
        )
        try:
            for when, actor, kind, target, maker, created in self.db.execute(query):
                event = {"t": when, "actor": actor, "type": kind, "object": target}
                if maker is not None:
                    event.update({"maker": maker, "created": created})
                yield event
        finally:
            self.db.close()
            self.temporary.cleanup()
