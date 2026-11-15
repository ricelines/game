"""Frozen S11 formulas and real source-schema adapter behavior."""

import json

import pytest
import zstandard

from measure import compute
from measure.adapters import moltbook, ours, wikipedia
from measure.core import jsd
from measure.output import write_report
from measure.recover import recover

INFO = {
    "name": "fixture",
    "t0": 0,
    "period_s": 20,
    "seed_actors": [],
    "classes": {"x": "change", "y": "change", "walk": "move", "look": "look", "say": "speak"},
}


def event(t, actor="a", action_type="x", target="object", **fields):
    return {"t": t, "actor": actor, "type": action_type, "object": target, **fields}


def test_literal_cross_author_scope_includes_objectless_and_any_event_pairs():
    report = compute(
        INFO,
        [
            event(1, authors=["a", "b"]),
            event(2, target=None, authors=["a", "c"]),
            event(3, action_type="walk", authors=["b", "d"]),
        ],
    )
    assert report["measures"]["cross_author_share"] == 2
    assert report["counts"]["objectless_multi_author_C"] == 1
    assert report["measures"]["distinct_author_pairs"] == 3
    assert report["diagnostics"]["author_pairs_scope"].startswith("all events")


def test_loop_definition_matches_up_to_eight_and_carries_period_history():
    repeated = [event(index, action_type="x" if index % 2 == 0 else "y") for index in range(20)]
    assert compute(INFO, repeated)["measures"]["loop_rate"] == pytest.approx(17 / 20)
    same = compute(INFO, [event(index) for index in range(40)])
    assert same["periods"][0]["measures"]["loop_rate"] == pytest.approx(19 / 20)
    assert same["periods"][1]["measures"]["loop_rate"] == 1
    assert same["measures"]["acting_share"] is None
    assert "only changes" in same["not_available"]["acting_share"]


def test_divergence_persistence_and_undefined_zero_change_agents():
    rows = [event(index, "a", "x") for index in range(20)]
    rows += [event(index, "b", "y") for index in range(20)]
    rows += [event(index, "c", "walk") for index in range(20)]
    rows.sort(key=lambda row: row["t"])
    report = compute({**INFO, "roles": {"a": ["x"], "b": ["x"]}}, rows)
    assert report["measures"]["role_divergence"] == 1
    assert report["counts"]["active_agents_without_CA"] == 1
    assert report["counts"]["divergence_pairs"] == 1
    assert report["measures"]["on_role_share"] == 0.5
    drift = compute(
        INFO, [event(index, action_type="x" if index < 20 else "y") for index in range(40)]
    )
    assert drift["measures"]["persistence"] == 0
    assert drift["counts"]["persistence_agent_pairs"] == 1
    assert jsd({"x": 1}, {"y": 1}) == 1


def test_seed_exclusion_root_adoption_and_later_use_in_creation_period():
    info = {**INFO, "period_s": 10, "seed_actors": ["seed"]}
    rows = [
        event(0, "maker", "look", "new", maker="maker", created=0, kind="new-kind"),
        event(1, "a", "x", "seed-object", maker="seed", created=-100, authors=["seed", "a"]),
        event(12, "script", "x", "new", root_actor="adopter", agent=False),
    ]
    report = compute(info, rows)
    assert report["measures"]["adopted_novelty"] == 1
    assert report["periods"][0]["measures"]["adopted_novelty"] == 1
    assert report["periods"][0]["counts"]["lookback_samples"] == 0
    assert report["counts"]["lookback_samples"] == 1
    assert report["measures"]["causal_lookback_median"] == 1
    assert report["counts"]["C"] == 2
    assert report["counts"]["CA"] == 1


def test_missing_fields_order_and_no_fabricated_values(tmp_path):
    report = compute(INFO, [])
    assert report["measures"]["acting_share"] is None
    assert report["measures"]["cross_author_share"] is None
    write_report(report, tmp_path)
    assert json.loads((tmp_path / "report.json").read_text())["definitions_version"] == 1
    with pytest.raises(ValueError, match="time order"):
        compute(INFO, [event(2), event(1)])
    with pytest.raises(ValueError, match="Missing minimal field"):
        compute(INFO, [{"t": 0}])


def test_s7_handlers_are_not_chosen_and_submitted_model_is(tmp_path):
    records = [{"run": "fixture", "step_hz": 20, "seed_actors": ["seed"]}]
    records += [
        {
            "id": 1,
            "step": 1,
            "kind": "create",
            "of": "model",
            "model": "m:1",
            "actor": "a",
            "result": "applied",
            "authors": ["a"],
        }
    ]
    records += [
        {
            "id": 2,
            "step": 2,
            "kind": "action",
            "actor": "b",
            "name": "author.switch",
            "things": ["o:1.1"],
            "result": "applied",
            "root": 2,
            "authors": ["a", "b"],
        }
    ]
    records += [
        {
            "id": 3,
            "step": 3,
            "kind": "handler",
            "actor": "s:1",
            "name": "arbitrary.reaction",
            "things": ["o:2.1"],
            "result": "applied",
            "root": 2,
            "authors": ["a", "b"],
        }
    ]
    source = tmp_path / "events.jsonl.zst"
    source.write_bytes(
        zstandard.ZstdCompressor().compress("\n".join(json.dumps(row) for row in records).encode())
    )
    adapter = ours.Adapter(source)
    rows = list(adapter.events())
    assert rows[0]["object"] == "m:1" and rows[0]["agent"] is True
    assert rows[-1]["agent"] is False and rows[-1]["root_actor"] == "b"
    report = compute(adapter.info(), rows)
    assert report["counts"]["C"] == 3 and report["counts"]["CA"] == 2


def test_s7_nullable_oldest_uses_unchanged_frozen_metadata_fallback(tmp_path):
    source = tmp_path / "nullable.jsonl"
    records = [
        {"step_hz": 1, "seed_actors": []},
        {
            "id": 1,
            "step": 1,
            "kind": "create",
            "of": "model",
            "model": "m:1",
            "actor": "a",
            "authors": ["a"],
            "oldest": None,
            "result": "applied",
        },
        {
            "id": 2,
            "step": 2,
            "kind": "action",
            "actor": "b",
            "things": ["m:1"],
            "name": "novel.use",
            "authors": None,
            "oldest": None,
            "cause": None,
            "root": None,
            "result": None,
        },
        {
            "id": 3,
            "step": 3,
            "kind": "action",
            "actor": "b",
            "things": ["m:1"],
            "name": "novel.use",
            "authors": ["a", "b"],
            "oldest": 0,
            "result": "applied",
        },
    ]
    source.write_text("\n".join(json.dumps(row) for row in records))
    adapter = ours.Adapter(source)
    rows = list(adapter.events())
    assert "oldest" not in rows[0] and "oldest" not in rows[1]
    assert rows[2]["oldest"] == 0
    assert rows[1]["result"] is None and rows[1]["authors"] is None
    report = compute(adapter.info(), rows)
    assert report["counts"]["C"] == 3 and report["counts"]["CA"] == 3
    assert report["measures"]["causal_lookback_median"] == 0.5


def test_s7_explicit_unchanged_diagnostics_are_not_applied_changes(tmp_path):
    source = tmp_path / "diagnostics.jsonl"
    records = [{"step_hz": 1, "seed_actors": ["seed"]}]
    records += [
        {
            "id": 1,
            "step": 1,
            "kind": "system",
            "name": "seed.metadata",
            "actor": "seed",
            "changed": False,
        },
        {
            "id": 2,
            "step": 2,
            "kind": "system",
            "name": "ambiguous_anchor",
            "changed": False,
            "result": None,
        },
        {
            "id": 3,
            "step": 3,
            "kind": "action",
            "actor": "a",
            "name": "arbitrary.attempt",
            "changed": False,
        },
        {
            "id": 4,
            "step": 4,
            "kind": "handler",
            "actor": "s:1",
            "name": "novel.changed",
            "changed": True,
        },
        {
            "id": 5,
            "step": 5,
            "kind": "action",
            "actor": "a",
            "name": "explicit.outcome",
            "changed": False,
            "result": "applied",
        },
        {
            "id": 6,
            "step": 6,
            "kind": "create",
            "of": "model",
            "actor": "a",
            "model": "m:1",
            "name": "register_model",
            "result": "denied",
            "changed": False,
        },
    ]
    source.write_text("\n".join(json.dumps(row) for row in records))
    adapter = ours.Adapter(source)
    rows = list(adapter.events())
    assert [row["result"] for row in rows[:3]] == ["no_effect"] * 3
    assert rows[2]["agent"] is True and rows[2]["class"] == "change"
    assert rows[4]["result"] == "applied" and rows[5]["result"] == "denied"
    report = compute(adapter.info(), rows)
    assert report["counts"]["C"] == 2 and report["counts"]["CA"] == 1
    assert report["counts"]["agent_events"] == 3


def test_s7_failed_authoring_is_chosen_without_invented_object_or_creation(tmp_path):
    source = tmp_path / "authoring.jsonl"
    records = [
        {"step_hz": 20, "seed_actors": []},
        {
            "id": 1,
            "step": 1,
            "kind": "create",
            "of": "model",
            "actor": "a",
            "name": "register_model",
            "result": "denied",
            "changed": False,
            "oldest": None,
            "authors": ["a"],
        },
        {
            "id": 2,
            "step": 2,
            "kind": "create",
            "of": "script",
            "actor": "a",
            "name": "register_script",
            "result": "denied",
            "changed": False,
            "oldest": None,
            "authors": ["a"],
        },
        {
            "id": 3,
            "step": 3,
            "kind": "create",
            "of": "instance",
            "actor": "a",
            "name": "create",
            "things": ["o:1.1"],
            "model": "m:7",
            "maker": "a",
            "result": "applied",
            "changed": True,
            "oldest": None,
            "authors": ["a"],
        },
    ]
    source.write_text("\n".join(json.dumps(row) for row in records))
    adapter = ours.Adapter(source)
    rows = list(adapter.events())
    assert rows[0]["agent"] and rows[1]["agent"]
    assert rows[0]["object"] is None and rows[1]["object"] is None
    assert "created" not in rows[0] and "created" not in rows[1]
    assert rows[2]["agent"] is False and rows[2]["object"] == "o:1.1"
    report = compute(adapter.info(), rows)
    assert report["counts"]["agent_events"] == 2 and report["counts"]["CA"] == 0
    assert report["counts"]["C"] == 1 and report["measures"]["acting_share"] is None
    assert report["not_available"]["acting_share"] == "record logs only changes"


def test_explicit_s7_prefix_recovery_reports_interruption_and_never_skips_middle(tmp_path):
    source = tmp_path / "interrupted.zst"
    prefix = tmp_path / "prefix.jsonl"
    header = b'{"step_hz":20}\n'
    row = b'{"id":1,"step":2,"kind":"action","actor":"a","oldest":null}\n'
    tail = b'{"id":2,"unfinished":"'
    compressor = zstandard.ZstdCompressor().compressobj()
    compressed = compressor.compress(header + row + tail) + compressor.flush(
        zstandard.COMPRESSOBJ_FLUSH_BLOCK
    )
    source.write_bytes(compressed)
    provenance = recover(source, prefix)
    assert source.read_bytes() == compressed
    assert prefix.read_bytes() == header + row
    assert provenance["records_including_header"] == 2
    assert provenance["incomplete_zstd_frame"] is True
    assert provenance["excluded_final_fragment_bytes"] == len(tail)
    assert provenance["nullable_fields"] == {"oldest": 1}
    assert provenance["last_complete_record"]["id"] == 1
    source.write_bytes(zstandard.ZstdCompressor().compress(header + b"{broken}\n" + row))
    with pytest.raises(json.JSONDecodeError):
        recover(source, prefix)


def test_s7_prefix_recovery_accepts_complete_concatenated_frames(tmp_path):
    source = tmp_path / "closed.zst"
    output = tmp_path / "closed-prefix.jsonl"
    compressor = zstandard.ZstdCompressor()
    source.write_bytes(
        compressor.compress(b'{"step_hz":20}\n')
        + compressor.compress(b'{"id":1,"step":2,"kind":"action"}')
    )
    provenance = recover(source, output)
    assert provenance["complete_zstd_frames"] == 2
    assert provenance["incomplete_zstd_frame"] is False
    assert provenance["excluded_final_fragment_bytes"] == 0
    assert provenance["records_including_header"] == 2


def test_published_moltnet_parent_schema_keeps_talk_only(tmp_path):
    posts = [{"id": f"p{i}", "created_at": i, "author_id": "poster"} for i in range(20)]
    comments = [
        {
            "id": f"c{i}",
            "created_at": 20 + i,
            "author_id": "commenter",
            "parent_id": None,
            "post_id": "p0",
        }
        for i in range(20)
    ]
    for filename, rows in (("posts", posts), ("comments", comments)):
        (tmp_path / (filename + ".ndjson")).write_text("\n".join(json.dumps(row) for row in rows))
    adapter = moltbook.Adapter(tmp_path, period_s=100)
    info = adapter.info()
    rows = list(adapter.events())
    assert rows[-1]["maker"] == "poster" and rows[-1]["object"] == "p0"
    report = compute(info, rows)
    assert report["measures"]["acting_share"] == 0
    assert report["measures"]["loop_rate"] == pytest.approx(0.95)
    for name in (
        "cross_author_share",
        "causal_lookback_median",
        "adopted_novelty",
        "role_divergence",
    ):
        assert name in report["not_available"]


def test_mediawiki_named_projection_talk_revert_and_real_column_schema(tmp_path):
    rows = [
        {
            "event_entity": "revision",
            "event_timestamp": "2024-01-02T00:00:00Z",
            "event_user_id": "7",
            "event_type": "create",
            "page_id": "12",
            "page_namespace_historical": "0",
            "page_first_edit_timestamp": "2020-01-01T00:00:00Z",
            "revision_is_identity_revert": "true",
        },
        {
            "event_entity": "revision",
            "event_timestamp": "2024-01-01T00:00:00Z",
            "event_user_id": "9",
            "event_type": "create",
            "page_id": "13",
            "page_namespace_historical": "1",
            "page_first_edit_timestamp": "2023-01-01T00:00:00Z",
        },
    ]
    source = tmp_path / "history.ndjson"
    rows.extend(
        [
            {**rows[0], "event_user_id": "0", "event_user_is_anonymous": "true"},
            {**rows[0], "event_user_id": "", "event_user_is_anonymous": "true"},
        ]
    )
    source.write_text("\n".join(json.dumps(row) for row in rows))
    adapter = wikipedia.Adapter(source)
    projected = list(adapter.events())
    assert projected[0]["class"] == "speak"
    assert projected[1]["type"] == "revert"
    assert len(projected) == 2
    assert adapter.source_counts["input_rows"] == 4
    assert adapter.source_counts["excluded_missing_author_or_page"] == 2
    assert "page_artificial_id" not in wikipedia.COLUMNS
    assert len(wikipedia.COLUMNS) == 78


def test_loop_width_limit_and_unclamped_history():
    info = {**INFO, "classes": {str(index): "change" for index in range(9)}}
    cycle8 = [event(index, action_type=str(index % 8)) for index in range(20)]
    cycle9 = [event(index, action_type=str(index % 9)) for index in range(20)]
    assert compute(info, cycle8)["measures"]["loop_rate"] == 0.25
    assert compute(info, cycle9)["measures"]["loop_rate"] == 0
    history = compute(INFO, [event(10, created=-10)])
    assert history["measures"]["causal_lookback_median"] == 2


def test_validation_preregistration_and_world_cluster_bootstrap(tmp_path):
    from measure.validate import correlate_worlds, preregister
    from measure.validate import wikipedia as validate_wikipedia

    name = preregister(tmp_path)
    record = json.loads((tmp_path / name).read_text())
    assert record["definitions_version"] == 1
    assert record["wikipedia"]["worlds"] == 40
    assert record["emergence"]["significance_test"] is False
    # Independently ranked/Pearson-checked reference: clusters have unequal
    # lengths, tied ranks, an empty world and a constant world. Row resampling
    # cannot reproduce the whole-world interval/undefined-replicate count.
    result = correlate_worlds(
        [([1, 2, 2, 4], [4, 2, 3, 1]), ([-2, 3], [-1, 5]), ([], []), ([0, 0], [1, 1])]
    )
    assert result["spearman"] == pytest.approx(0.5555978981153942)
    assert result["interval_95"] == pytest.approx([-0.9486832980505138, 1.0])
    assert result["worlds"] == 4 and result["worlds_with_paired_changes"] == 3
    assert result["paired_changes"] == 8
    assert result["bootstrap_world_resamples"] == 1000
    assert result["defined_bootstrap_samples"] == 932
    assert "not_available" in validate_wikipedia(None, tmp_path)
    assert "not_available" in validate_wikipedia({"sampling_frame_complete": False}, tmp_path)
