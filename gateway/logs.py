"""Streaming S7 record filters, independent of network sessions and credentials."""

from measure.io import json_lines


def records(path, *, since=0, actor=None, kind=None, thing=None):
    for row in json_lines(path):
        if "kind" not in row:
            yield row
            continue
        if row.get("step", 0) < since:
            continue
        if actor is not None and row.get("actor") != actor:
            continue
        if kind is not None and row.get("kind") != kind:
            continue
        if thing is not None and thing not in row.get("things", []):
            continue
        yield row
