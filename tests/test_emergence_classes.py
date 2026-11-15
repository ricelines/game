"""Exercise class corrections through the adapter and frozen measure computation."""

import json

from measure.adapters.emergence import Adapter
from measure.core import compute


def test_lookup_corrections_do_not_count_as_world_changes(tmp_path):
    names = [
        "read_landmark_info",
        "search_tool",
        "memory_breach_scan",
        "overall_tool_usage_analytics",
        "put_brick_in_pixel",
        "agent_directory",
        "phenotypic_telemetry_tracker",
        "unseen_world_operation",
    ]
    path = tmp_path / "calls.json"
    path.write_text(
        json.dumps(
            [
                {
                    "name": "agent",
                    "timestamp": 100 + i,
                    "tool_name": name,
                    "target_agent": None,
                    "location": "landmark",
                }
                for i, name in enumerate(names)
            ]
        )
    )
    adapter = Adapter(path)
    report = compute(adapter.info(), list(adapter.events()))
    assert report["counts"]["CA"] == 4
    assert report["counts"]["acting_denominator"] == 8
    assert report["measures"]["acting_share"] == 0.5
    assert report["diagnostics"]["default_change_types"] == {"unseen_world_operation": 1}
    assert report["info"]["adapter_revision"] == 2


def test_unresolved_dispatch_failures_keep_the_frozen_missing_result_rule(tmp_path):
    path = tmp_path / "calls.json"
    path.write_text(
        json.dumps(
            [
                {
                    "name": "agent",
                    "timestamp": 100,
                    "tool_name": "check_balance",
                    "tool_response": "DISPATCH_FAILED: Unknown function",
                    "location": "landmark",
                },
                {
                    "name": "agent",
                    "timestamp": 101,
                    "tool_name": "read_landmark_info",
                    "tool_response": "Existing landmark description",
                    "location": "landmark",
                },
            ]
        )
    )
    adapter = Adapter(path)
    events = list(adapter.events())
    assert all("result" not in event for event in events)
    report = compute(adapter.info(), events)
    assert report["counts"]["CA"] == 1
    assert report["measures"]["acting_share"] == 0.5
