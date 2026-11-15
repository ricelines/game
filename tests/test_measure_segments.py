"""Real zstd frames, immutable headers and strict native continuation boundaries."""

import json

import pytest
import zstandard

from measure.segments import join


def segment(path, ids, *, resumed=0, world="same-world"):
    header = {
        "spec": 1,
        "world": world,
        "runtime": "fixture",
        "registry_hash": "same-registry",
        "step_hz": 20,
        "seed_actors": ["p:1"],
        "resumed_step": resumed,
        "started": resumed,
    }
    rows = [header] + [
        {"id": identity, "step": identity, "kind": "action", "actor": "p:2"} for identity in ids
    ]
    raw = b"".join((json.dumps(row) + "\n").encode() for row in rows)
    path.write_bytes(zstandard.ZstdCompressor().compress(raw))
    return raw


def test_complete_segments_keep_original_header_bytes_and_all_real_event_ids(tmp_path):
    one, two, output = (tmp_path / name for name in ("one.zst", "two.zst", "joined.jsonl"))
    first = segment(one, [1, 2])
    second = segment(two, [3, 4], resumed=2)
    original = one.read_bytes(), two.read_bytes()
    report = join([one, two], output)
    assert output.read_bytes() == first + second.split(b"\n", 1)[1]
    assert (one.read_bytes(), two.read_bytes()) == original
    assert report["records"] == 4 and report["last_step"] == 4
    assert [row["first_id"] for row in report["segments"]] == [1, 3]
    assert [row["last_id"] for row in report["segments"]] == [2, 4]


@pytest.mark.parametrize("ids", ([2, 3], [4, 5]))
def test_join_rejects_overlap_or_gap_without_a_combined_output(tmp_path, ids):
    one, two, output = (tmp_path / name for name in ("one.zst", "two.zst", "joined.jsonl"))
    segment(one, [1, 2])
    segment(two, ids, resumed=2)
    with pytest.raises(ValueError, match="gap or duplicate"):
        join([one, two], output)
    assert not output.exists()


def test_join_rejects_mismatched_world_and_incomplete_frame(tmp_path):
    one, two, output = (tmp_path / name for name in ("one.zst", "two.zst", "joined.jsonl"))
    segment(one, [1, 2])
    segment(two, [3], resumed=2, world="different")
    with pytest.raises(ValueError, match="identity differs"):
        join([one, two], output)
    segment(two, [3], resumed=2)
    two.write_bytes(two.read_bytes()[:-1])
    with pytest.raises(ValueError, match="incomplete"):
        join([one, two], output)
    assert not output.exists()
