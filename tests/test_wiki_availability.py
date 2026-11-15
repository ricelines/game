"""Actual cached-response certificates distinguish source obstructions from unfinished input."""

import hashlib
import json
import urllib.parse
from datetime import UTC, datetime, timedelta

import pytest

from measure.wiki_sources import sha256, write_json
from measure.wiki_study import build_availability, certified_unavailable_views
from measure.wiki_views import DAY, START


def cached_response(directory, url, body):
    key = hashlib.sha256(url.encode()).hexdigest()
    payload = json.dumps(body).encode()
    path = directory / "api-cache" / (key + ".json")
    path.parent.mkdir(exist_ok=True)
    path.write_bytes(payload)
    write_json(
        path.with_suffix(".proof.json"),
        {"url": url, "status": 200, "sha256": sha256(path), "bytes": len(payload)},
    )
    return key, path


def fixture(directory):
    worlds = [
        {"ordinal": 1, "category": "A", "article_ids": [7, 8]},
        {"ordinal": 2, "category": "B", "article_ids": [9]},
        {"ordinal": 3, "category": "C", "article_ids": [10]},
        {"ordinal": 4, "category": "D", "article_ids": [11]},
    ]
    write_json(directory / "selected.json", {"selected": worlds})
    titles = {}
    response_paths = {}
    for page in (7, 8, 9, 10, 11):
        title = str(page)
        titles[title] = {
            "intervals": [{"start": START, "end": START + DAY * 366, "title": title}],
            "move_days_unavailable": [],
            "moves": [],
        }
        if page == 8:
            continue
        items = (
            [{"timestamp": "2024010200", "views": 2}]
            if page == 7
            else []
            if page == 9
            else [
                {
                    "timestamp": (datetime(2024, 1, 1, tzinfo=UTC) + timedelta(days=day)).strftime(
                        "%Y%m%d00"
                    ),
                    "views": 7,
                }
                for day in range(366)
            ]
        )
        url = (
            "https://wikimedia.org/api/rest_v1/metrics/pageviews/per-article/"
            f"en.wikipedia/all-access/user/{urllib.parse.quote(title)}/daily/2024010100/2024123100"
        )
        _key, response_paths[page] = cached_response(directory, url, {"items": items})
        (directory / "readership").mkdir(exist_ok=True)
        # These old values are deliberately not the primary response evidence.
        write_json(directory / "readership" / f"{page}.json", {"page": title, "daily": [0] * 366})
    titles["11"]["move_days_unavailable"] = [7]
    titles["11"]["moves"] = [{"id": 99, "at": "2024-01-08T12:00:00Z", "from": "Before", "to": "11"}]
    write_json(directory / "historical-titles.json", {"pages": titles})
    move_key, _path = cached_response(
        directory,
        "https://en.wikipedia.org/w/api.php?action=query&list=logevents&letype=move",
        {
            "query": {
                "logevents": [
                    {
                        "logid": 99,
                        "logpage": 11,
                        "timestamp": "2024-01-08T12:00:00Z",
                        "title": "Before",
                        "params": {"target_title": "11"},
                    }
                ]
            }
        },
    )
    write_json(directory / "moves-progress.json", {"proofs": [move_key]})
    (directory / "history").mkdir()
    write_json(directory / "history/history-projection.json", {"fixture": "verified sources"})
    creations = {
        "selected_sha256": sha256(directory / "selected.json"),
        "history_projection_sha256": sha256(directory / "history/history-projection.json"),
        "pages": {
            str(page): {
                "status": "known" if page in (10, 11) else "unknown",
                "creation_timestamps": ["2020-01-01T00:00:00Z"] if page in (10, 11) else [],
            }
            for page in (7, 8, 9, 10, 11)
        },
    }
    path = directory / "page-creations.json"
    write_json(path, creations)
    return path, response_paths


def test_certificate_uses_actual_shared_projection_and_never_drops_uncollected_article(tmp_path):
    creations, _responses = fixture(tmp_path)
    output = tmp_path / "availability.json"
    record = build_availability(tmp_path, creations, None, output)
    a, b, c, d = record["worlds"]
    assert record["cached_pages"] == 4 and record["uncollected_pages"] == 1
    assert a["periods"][0]["status"] == "proven_unavailable"
    obstruction = a["periods"][0]["obstruction"]
    assert obstruction["page"] == "7" and obstruction["date"] == "2024-01-02"
    assert obstruction["views"] == obstruction["response"]["item"]["views"] == 2
    assert obstruction["identity_existence"]["status"] == "unknown"
    assert all(row["status"] == "uncollected" for row in a["periods"][1:])
    assert all(row["pageviews"] == 0 for row in b["periods"])
    assert c["periods"][0]["pageviews"] == 49
    assert c["periods"][-1] == {"period": 52, "days": 2, "status": "available", "pageviews": 14}
    assert d["periods"][1]["obstruction"]["reason"] == "move_ambiguity"
    assert record["move_responses"][99]["item"]["logpage"] == 11
    assert json.loads(output.read_text())["worlds"] == record["worlds"]


def test_new_existence_witness_does_not_make_missing_article_disappear_and_bad_proof_fails(
    tmp_path,
):
    creations, responses = fixture(tmp_path)
    existence = tmp_path / "page-existence.json"
    source = json.loads(creations.read_text())
    write_json(
        existence,
        {
            "selected_sha256": source["selected_sha256"],
            "history_projection_sha256": source["history_projection_sha256"],
            "historical_titles_sha256": sha256(tmp_path / "historical-titles.json"),
            "pages": {"7": {"exists_by_s": START + DAY}},
            "earlier_witnesses": [],
        },
    )
    output = tmp_path / "availability.json"
    record = build_availability(tmp_path, creations, existence, output)
    assert all(row["status"] == "uncollected" for row in record["worlds"][0]["periods"])
    before = output.read_bytes()
    responses[7].write_text('{"items":[]}')
    with pytest.raises(ValueError, match="proof mismatch"):
        build_availability(tmp_path, creations, existence, output)
    assert output.read_bytes() == before


def test_partial_acquisition_can_certify_na_but_never_promotes_uncollected_to_na(tmp_path):
    creations, responses = fixture(tmp_path)
    output = tmp_path / "availability.json"
    record = build_availability(tmp_path, creations, None, output)
    with pytest.raises(ValueError, match="actual obstruction"):
        certified_unavailable_views(record["worlds"][0])  # Only the first week has an obstruction.
    with pytest.raises(ValueError, match="actual obstruction"):
        certified_unavailable_views(record["worlds"][1])  # Published zero is available, not NA.
    # One actual positive/unbounded page can obstruct every sum even with another page uncollected.
    proof = json.loads(responses[7].with_suffix(".proof.json").read_text())
    cached_response(tmp_path, proof["url"], json.loads(responses[10].read_text()))
    record = build_availability(tmp_path, creations, None, output)
    world = record["worlds"][0]
    assert world["uncollected_articles"] == 1
    result = certified_unavailable_views(world)
    assert len(result["pageviews"]) == 53
    assert all(row["pageviews"] is None for row in result["pageviews"])
    assert result["readership_availability"]["obstruction_article_ids"] == ["7"]
    assert result["readership_availability"]["complete_annual_acquisition"] is False
