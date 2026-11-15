import json

import zstandard

from gateway.cli import parser
from gateway.logs import records


def test_s7_filters_stream_compressed_records_and_preserve_header(tmp_path):
    rows = [
        {"spec": 1, "run": "real-format-fixture"},
        {"id": 1, "step": 3, "kind": "action", "actor": "p:2", "things": ["o:9.1"]},
        {"id": 2, "step": 4, "kind": "handler", "actor": "s:2", "things": ["o:9.1"]},
        {"id": 3, "step": 5, "kind": "action", "actor": "p:2", "things": ["o:10.1"]},
        {"id": 4, "step": 6, "kind": "action", "actor": "p:2", "things": ["o:9.2"]},
        {"id": 5, "step": 7, "kind": "action", "actor": "p:2", "things": ["o:9.1"]},
    ]
    path = tmp_path / "events.jsonl.zst"
    path.write_bytes(zstandard.ZstdCompressor().compress("\n".join(map(json.dumps, rows)).encode()))
    assert list(records(path)) == rows
    assert list(records(path, since=4, actor="p:2", kind="action", thing="o:9.1")) == [
        rows[0],
        rows[5],
    ]
    options = parser().parse_args(["log", "--file", str(path), "--since", "4"])
    assert options.key is None and options.key_file is None and options.since == 4


def test_fixture_cli_routes_existing_native_runner_without_a_player_key():
    options = parser().parse_args(
        ["fixture", "capture.amb.zst", "--job", "job.json", "--result", "result.json"]
    )
    assert options.key is None and str(options.capture) == "capture.amb.zst"
