"""Actual public-source formats, sampling proofs and durable HTTP behavior."""

import gzip
import hashlib
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest

from measure.wiki_sample import closure, draw_ordinals
from measure.wiki_sources import PublicAPI, category_rows, download, write_json
from measure.wiki_views import START, acquire_readership, desired_titles, lineages


def test_pinned_mariadb_category_format_preserves_escaped_and_invalid_bytes(tmp_path):
    source = tmp_path / "category.sql.gz"
    source.write_bytes(
        gzip.compress(
            b"CREATE TABLE category ...\nINSERT INTO `category` VALUES\n"
            b"(2,'Colonial_\\'ships\\'',14,2,0),\n"
            b"(3,'Old_\\\\name_\xe9',0,0,0);\n"
            b"/*!40000 ALTER TABLE `category` ENABLE KEYS */;\n"
        )
    )
    assert list(category_rows(source)) == [
        (2, b"Colonial_'ships'", 14, 2, 0),
        (3, b"Old_\\name_\xe9", 0, 0, 0),
    ]
    source.write_bytes(gzip.compress(b"INSERT INTO `category` VALUES\nmalformed;\n"))
    with pytest.raises(ValueError, match="Unexpected pinned"):
        list(category_rows(source))


def test_uniform_draw_is_complete_distinct_deterministic_and_prefix_stable():
    values = list(draw_ordinals(100))
    assert set(values) == set(range(1, 101))
    assert values == list(draw_ordinals(100))
    assert values[:5] == [39, 4, 61, 90, 99]


class GraphAPI:
    def __init__(self):
        self.calls = []

    def query(self, **parameters):
        self.calls.append(parameters)
        title = parameters["gcmtitle"]

        def page(identity, name, ns=0, **extra):
            return {"pageid": identity, "title": name, "ns": ns, **extra}

        if title == "Category:Root" and "gcmcontinue" not in parameters:
            return {
                "query": {
                    "pages": [
                        page(1, "A"),
                        page(10, "Category:Child", 14),
                        page(3, "Redirect", redirect=True),
                    ]
                },
                "continue": {"gcmcontinue": "next"},
            }, "root1"
        if title == "Category:Root":
            return {"query": {"pages": [page(2, "B")]}}, "root2"
        if title == "Category:Child":
            return {
                "query": {
                    "pages": [
                        page(1, "A"),
                        page(11, "Category:Grandchild", 14),
                        page(12, "Category:Root", 14),
                    ]
                }
            }, "child"
        if title == "Category:Grandchild":
            return {"query": {"pages": [page(4, "C"), page(13, "Category:TooDeep", 14)]}}, "grand"
        raise AssertionError("Traversal exceeded declared depth")


def test_actual_generator_pages_continuation_redirects_cycle_and_depth():
    api = GraphAPI()
    result = closure(api, "Category:Root")
    assert result["closed"] and result["articles"] == {1: "A", 2: "B", 4: "C"}
    assert result["proofs"] == ["root1", "root2", "child", "grand"]
    assert [row["depth"] for row in result["categories"]] == [0, 1, 2]
    result = closure(GraphAPI(), "Category:Root", maximum=1)
    assert not result["closed"] and len(result["articles"]) > 1


def test_http_cache_and_validator_bound_resume_use_real_http(tmp_path):
    body = b"public dataset bytes" * 100
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            requests.append(dict(self.headers))
            if self.path == "/query":
                payload = b'{"query":{"pages":[]}}'
                self.send_response(200)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                return
            offset = int(self.headers.get("Range", "bytes=0-")[6:-1])
            self.send_response(206 if offset else 200)
            self.send_header("ETag", '"immutable"')
            self.send_header("Content-Length", str(len(body) - offset))
            if offset:
                self.send_header("Content-Range", f"bytes {offset}-{len(body) - 1}/{len(body)}")
            self.end_headers()
            self.wfile.write(body[offset:])

        def log_message(self, *_arguments):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    try:
        client = PublicAPI(tmp_path / "cache", interval=0)
        data, key = client.get(base + "/query")
        assert data == {"query": {"pages": []}}
        assert client.get(base + "/query") == (data, key)
        assert len(requests) == 1 and client.hits == 1
        proof = json.loads((tmp_path / "cache" / (key + ".proof.json")).read_text())
        assert (
            proof["status"] == 200
            and proof["sha256"]
            == hashlib.sha256((tmp_path / "cache" / (key + ".json")).read_bytes()).hexdigest()
        )
        path = tmp_path / "archive.bz2"
        path.with_suffix(".bz2.partial").write_bytes(body[:137])
        write_json(
            path.with_suffix(".bz2.download.json"),
            {"url": base + "/archive", "etag": '"immutable"'},
        )
        download(
            base + "/archive", path, expected_bytes=len(body), sha1=hashlib.sha1(body).hexdigest()
        )
        assert path.read_bytes() == body
        assert requests[-1]["Range"] == "bytes=137-"
        assert requests[-1]["If-Range"] == '"immutable"'
    finally:
        server.shutdown()
        thread.join()
        server.server_close()


def test_historical_title_chain_uses_moved_identity_and_excludes_only_move_day(tmp_path):
    cache = tmp_path / "api"
    cache.mkdir()
    # pageid is the redirect's current identity; logpage is the article actually moved.
    moves = [
        {
            "logid": 1,
            "logpage": 7,
            "pageid": 999,
            "title": "Old title",
            "timestamp": "2024-01-10T12:00:00Z",
            "params": {"target_title": "Middle title"},
        },
        {
            "logid": 2,
            "logpage": 7,
            "pageid": 998,
            "title": "Middle title",
            "timestamp": "2025-07-01T12:00:00Z",
            "params": {"target_title": "Current title"},
        },
    ]
    write_json(cache / "moves.json", {"query": {"logevents": moves}})
    api = type("Cache", (), {"directory": cache})()
    result = lineages(
        api,
        tmp_path,
        {"pages": {"7": {"title": "Current title"}, "999": {"title": "Other article"}}},
        {"proofs": ["moves"], "rows": 2, "end": "2026-11-15T00:00:00Z"},
    )
    titles = desired_titles(result["pages"]["7"])
    assert titles[:9] == ["Old title"] * 9
    assert titles[9] is None and titles[10:] == ["Middle title"] * 356
    assert desired_titles(result["pages"]["999"]) == ["Other article"] * 366


def test_collector_decodes_mature_omissions_without_unnecessary_creation_queries(tmp_path):
    class API:
        directory = tmp_path
        requests, seconds = 0, 0

        def get(self, _url):
            return {"items": []}, "empty-series"

        def query(self, **_arguments):
            raise AssertionError("Omitted zero counts do not require a creation-log campaign")

    record = {
        "pages": {
            "7": {
                "intervals": [{"start": START, "end": START + 366 * 86400, "title": "Article"}],
                "move_days_unavailable": [],
            }
        },
        "move_log_end": "2026-11-15T00:00:00Z",
    }
    acquire_readership(API(), tmp_path, record)
    data = json.loads((tmp_path / "readership/7.json").read_text())
    assert data["daily"] == [0] * 366
    assert data["title_responses"]["Article"]["omitted_days"] == list(range(366))
    assert "creation" not in data


def test_mature_404_is_published_zero_but_protocol_and_request_errors_are_not(tmp_path):
    import urllib.error

    from measure.wiki_sources import sha256
    from measure.wiki_views import DISALLOWED, DISALLOWED_SHA256, fetch_title

    url = (
        "https://wikimedia.org/api/rest_v1/metrics/pageviews/per-article/"
        "en.wikipedia/all-access/user/Article/daily/2024010100/2024123100"
    )
    identity = hashlib.sha256(url.encode()).hexdigest()
    error_path = tmp_path / (identity + ".error.json")
    write_json(error_path, {"url": url, "status": 404, "body": "No matching records"})

    class API:
        directory = tmp_path

        def get(self, _url):
            raise AssertionError("Cached404 and known suppression make no new requests")

    series = fetch_title(API(), "Article")
    assert series["daily"] == [0] * 366
    assert series["status"] == 404 and series["omitted_days"] == list(range(366))
    assert series["error_sha256"] == sha256(error_path)
    suppressed = fetch_title(API(), next(iter(DISALLOWED)))
    assert suppressed["daily"] == [None] * 366
    assert suppressed["status"] == "suppressed"
    assert suppressed["source_sha256"] == DISALLOWED_SHA256
    write_json(error_path, {"url": "another request", "status": 404})
    with pytest.raises(ValueError, match="another article request"):
        fetch_title(API(), "Article")
    error_path.unlink()

    class Failure(API):
        def get(self, _url):
            raise urllib.error.HTTPError(url, 503, "Service Unavailable", {}, None)

    with pytest.raises(urllib.error.HTTPError, match="503"):
        fetch_title(Failure(), "Article")

    class Malformed(API):
        def get(self, _url):
            return {"unexpected": []}, "malformed-proof"

    with pytest.raises(KeyError, match="items"):
        fetch_title(Malformed(), "Article")


def cached_readership_fixture(directory, identity="7"):
    from datetime import UTC, datetime, timedelta

    url = (
        "https://wikimedia.org/api/rest_v1/metrics/pageviews/per-article/"
        "en.wikipedia/all-access/user/Article/daily/2024010100/2024123100"
    )
    key = hashlib.sha256(url.encode()).hexdigest()
    cache = directory / "api-cache"
    cache.mkdir()
    payload = json.dumps(
        {
            "items": [
                {
                    "timestamp": (datetime(2024, 1, 1, tzinfo=UTC) + timedelta(days=day)).strftime(
                        "%Y%m%d00"
                    ),
                    "views": 7,
                }
                for day in range(366)
            ]
        }
    ).encode()
    (cache / (key + ".json")).write_bytes(payload)
    write_json(
        cache / (key + ".proof.json"),
        {
            "url": url,
            "status": 200,
            "sha256": hashlib.sha256(payload).hexdigest(),
            "bytes": len(payload),
        },
    )
    raw = directory / "readership"
    raw.mkdir()
    write_json(raw / (identity + ".json"), {"page": identity, "daily": [7] * 366, "proofs": [key]})
    write_json(
        directory / "historical-titles.json",
        {
            "pages": {
                identity: {
                    "intervals": [{"start": START, "end": START + 366 * 86400, "title": "Article"}],
                    "move_days_unavailable": [],
                }
            }
        },
    )
    return raw / (identity + ".json"), cache / (key + ".json")


def test_published_zero_needs_no_existence_bound_but_positive_counts_do(tmp_path):
    from measure.wiki_study import normalize_readership

    _original, aqs = cached_readership_fixture(tmp_path)
    body = json.loads(aqs.read_text())
    body["items"] = [
        {"timestamp": "2024010100", "views": 0},
        {"timestamp": "2024010200", "views": 7},
    ]
    aqs.write_text(json.dumps(body))
    proof_path = aqs.with_suffix(".proof.json")
    proof = json.loads(proof_path.read_text())
    proof.update(sha256=hashlib.sha256(aqs.read_bytes()).hexdigest(), bytes=aqs.stat().st_size)
    write_json(proof_path, proof)
    titles_path = tmp_path / "historical-titles.json"
    lineage = json.loads(titles_path.read_text())
    lineage["pages"]["7"]["move_days_unavailable"] = [2]
    write_json(titles_path, lineage)
    creations = {"pages": {"7": {"status": "unknown", "creation_timestamps": []}}}
    output = tmp_path / "corrected"
    normalize_readership(tmp_path, creations, output)
    record = json.loads((output / "7.json").read_text())
    assert record["daily"][:4] == [0, None, None, 0]
    assert record["daily"][4:] == [0] * 362
    assert record["unbounded_positive_days"] == [{"day": 1, "views": 7}]
    response = record["title_responses"]["Article"]
    assert response["status"] == 200 and 0 not in response["omitted_days"]
    assert 3 in response["omitted_days"]


def test_priority_changes_request_order_but_not_full_acquisition_scope(tmp_path):
    called = []

    class API:
        directory = tmp_path
        requests, seconds = 0, 0

        def get(self, url):
            called.append(url.split("/")[-4])
            return {"items": []}, hashlib.sha256(url.encode()).hexdigest()

    lineages = {
        "pages": {
            str(page): {
                "intervals": [{"start": START, "end": START + 366 * 86400, "title": title}],
                "move_days_unavailable": [],
            }
            for page, title in [(1, "Alpha"), (2, "Beta"), (3, "Gamma")]
        }
    }
    acquire_readership(API(), tmp_path, lineages, priority_pages=["3"])
    assert called == ["Gamma", "Alpha", "Beta"]
    assert json.loads((tmp_path / "readership-complete.json").read_text())["pages"] == 3
    assert {path.stem for path in (tmp_path / "readership").glob("*.json")} == {"1", "2", "3"}


def test_complete_positive_series_is_unavailable_before_existence_and_preserves_originals(tmp_path):
    from measure.wiki_study import aggregate_views, normalize_readership
    from measure.wiki_views import timestamp

    original, aqs = cached_readership_fixture(tmp_path)
    raw = json.loads(original.read_text())
    raw["daily"][:9] = [0] * 9  # Retained old collector inference is not primary evidence.
    write_json(original, raw)
    before = original.read_bytes(), aqs.read_bytes()
    creations = {
        "pages": {
            "7": {
                "status": "known",
                "creation_timestamps": ["2024-01-10T12:00:00Z"],
                # Older imported/restored edits supply no earlier existence witness.
                "first_edit_min_s": timestamp("2002-01-01T00:00:00Z"),
            }
        }
    }
    output = tmp_path / "corrected"
    report = normalize_readership(tmp_path, creations, output)
    record = json.loads((output / "7.json").read_text())
    assert record["daily"][:10] == [None] * 10
    assert record["daily"][10:] == [7] * 356
    assert record["identity_existence"]["sources"] == {
        "history_creation": timestamp("2024-01-10T12:00:00Z")
    }
    assert report["counts"]["changed_page_days"] == 10
    assert (original.read_bytes(), aqs.read_bytes()) == before
    views = aggregate_views([{"article_ids": [7]}], output)[0]
    assert views["pageviews"][0]["pageviews"] is None
    assert views["pageviews"][1]["pageviews"] is None
    assert views["pageviews"][2]["pageviews"] == 49
    assert views["readership_availability"]["identity_bounds"] == {"bounded": 1}


def test_creation_records_bound_existence_without_proving_earlier_absence(tmp_path):
    from measure.wiki_study import normalize_readership
    from measure.wiki_views import timestamp

    original, _aqs = cached_readership_fixture(tmp_path)
    record = json.loads(original.read_text())
    record["creation"] = {"logpage": 7, "timestamp": "2025-01-01T00:00:00Z"}
    write_json(original, record)
    creations = {"pages": {"7": {"status": "unknown", "creation_timestamps": []}}}
    output = tmp_path / "corrected"
    normalize_readership(tmp_path, creations, output)
    corrected = json.loads((output / "7.json").read_text())
    assert corrected["daily"] == [None] * 366
    assert corrected["identity_existence"]["sources"] == {
        "creation_log": timestamp("2025-01-01T00:00:00Z")
    }
    creations["pages"]["7"] = {
        "status": "known",
        "creation_timestamps": ["2024-01-10T00:00:00Z"],
    }
    normalize_readership(tmp_path, creations, output)
    corrected = json.loads((output / "7.json").read_text())
    assert corrected["daily"][:9] == [None] * 9
    assert corrected["daily"][9:] == [7] * 357
    assert corrected["identity_existence"]["distinct_creation_times"] == 2
    # Restoration can leave multiple creation records; the earliest still proves existence.
    creations["pages"]["7"]["status"] = "conflict"
    creations["pages"]["7"]["creation_timestamps"].append("2025-01-01T00:00:00Z")
    normalize_readership(tmp_path, creations, output)
    assert json.loads((output / "7.json").read_text())["daily"] == corrected["daily"]
    del record["creation"]
    write_json(original, record)
    creations["pages"]["7"] = {"status": "unknown", "creation_timestamps": []}
    normalize_readership(tmp_path, creations, output)
    corrected = json.loads((output / "7.json").read_text())
    assert corrected["daily"] == [None] * 366
    assert corrected["identity_existence"] == {"status": "unknown", "sources": {}}
    record["creation"] = {"logpage": 999, "timestamp": "2024-01-10T00:00:00Z"}
    write_json(original, record)
    with pytest.raises(ValueError, match="another stable page ID"):
        normalize_readership(tmp_path, creations, output)


def test_independent_existence_witness_keeps_aqs_counts_and_missing_days(tmp_path):
    from measure.wiki_sources import sha256
    from measure.wiki_study import aggregate_views, normalize_readership

    original, _aqs = cached_readership_fixture(tmp_path)
    titles_path = tmp_path / "historical-titles.json"
    titles = json.loads(titles_path.read_text())
    titles["pages"]["7"]["move_days_unavailable"] = [8]
    write_json(titles_path, titles)
    creations = {
        "selected_sha256": "selected-proof",
        "history_projection_sha256": "history-proof",
        "pages": {"7": {"status": "unknown", "creation_timestamps": []}},
    }
    witness = {
        "exists_by_s": START + 86400,
        "title": "Article",
        "views": 19,
        "access_methods": ["desktop"],
    }
    existence = {
        "historical_titles_sha256": sha256(titles_path),
        "selected_sha256": "selected-proof",
        "history_projection_sha256": "history-proof",
        "pages": {"7": witness},
        "earlier_witnesses": [],
    }
    before = original.read_bytes()
    output = tmp_path / "corrected"
    report = normalize_readership(tmp_path, creations, output, existence)
    corrected = json.loads((output / "7.json").read_text())
    assert corrected["daily"][0] is None  # Daily witness proves only existence by next midnight.
    assert corrected["daily"][1:8] == [7] * 7  # Bulk's19 never replaces actual AQS counts.
    assert corrected["daily"][8] is None  # Existing ambiguous move-day exclusion survives.
    assert corrected["daily"][9:] == [7] * 357
    assert corrected["identity_existence"]["status"] == "bounded"
    assert corrected["source_creation_status"] == "unknown"
    assert corrected["jan1_witness"] == witness
    assert report["counts"]["bounded_existence_pages"] == 1
    assert original.read_bytes() == before
    availability = aggregate_views([{"article_ids": [7]}], output)[0]["readership_availability"]
    assert availability["articles_with_jan1_witness"] == 1
    # Earlier observation remains usable when creation describes a later incarnation.
    creations["pages"]["7"] = {
        "status": "known",
        "creation_timestamps": ["2024-01-11T00:00:00Z"],
    }
    existence["pages"] = {}
    existence["earlier_witnesses"] = [{"page": "7", "created_s": START + 10 * 86400, **witness}]
    report = normalize_readership(tmp_path, creations, output, existence)
    earlier = json.loads((output / "7.json").read_text())
    assert earlier["daily"] == corrected["daily"]
    assert earlier["identity_existence"]["exists_by_s"] == START + 86400
    assert report["counts"]["creation_after_jan1_witness_pages"] == 1
    existence["historical_titles_sha256"] = "different-lineage"
    with pytest.raises(ValueError, match="another historical title set"):
        normalize_readership(tmp_path, creations, output, existence)


def test_draw_transcript_replay_rejects_omissions_reordering_and_tampered_closures():
    from copy import deepcopy

    from measure.wiki_study import verify_draws

    class Certificates:
        def read(self, _key):
            return {
                "query": {
                    "pages": [
                        {
                            "title": "Category:Leaf",
                            "categoryinfo": {"pages": 2, "subcats": 0, "files": 0},
                        },
                        {
                            "title": "Category:A",
                            "categoryinfo": {"pages": 500, "subcats": 0, "files": 0},
                        },
                        {
                            "title": "Category:B",
                            "categoryinfo": {"pages": 500, "subcats": 0, "files": 0},
                        },
                    ]
                }
            }, {
                "url": "https://en.wikipedia.org/w/api.php?prop=categoryinfo&titles=Category%3ALeaf%7CCategory%3AA%7CCategory%3AB"
            }

        def query(self, **parameters):
            title = parameters["gcmtitle"]
            return {
                "query": {
                    "pages": [
                        {"ns": 0, "pageid": index, "title": f"Article{index}"}
                        for index in range(1, 501)
                    ]
                }
            }, title

    api = Certificates()
    ordinals = list(draw_ordinals(100))[:3]
    titles = dict(zip(ordinals, [b"Leaf", b"A", b"B"], strict=True))
    records = [
        {
            "draw": 1,
            "ordinal": ordinals[0],
            "category": "Category:Leaf",
            "categoryinfo": {"pages": 2, "subcats": 0, "files": 0},
            "info_proof": "info",
            "eligible": False,
            "reason": "leaf_upper_bound_below500",
        }
    ]
    for index, name in enumerate(("A", "B"), 2):
        tree = closure(api, "Category:" + name)
        articles = tree.pop("articles")
        records.append(
            {
                "draw": index,
                "ordinal": ordinals[index - 1],
                "category": "Category:" + name,
                "categoryinfo": {"pages": 500, "subcats": 0, "files": 0},
                "info_proof": "info",
                "eligible": True,
                "article_ids": sorted(articles),
                "article_count": 500,
                "article_titles": {str(k): v for k, v in articles.items()},
                **tree,
            }
        )
    accepted, reasons = verify_draws(records, 100, titles.__getitem__, api, worlds=2)
    assert accepted == records[1:] and reasons == {
        "leaf_upper_bound_below500": 1,
        "complete_closure": 2,
    }
    with pytest.raises(ValueError, match="requires 2"):
        verify_draws(records[:2], 100, titles.__getitem__, api, worlds=2)
    changed = deepcopy(records)
    changed[1]["article_ids"][-1] = 999
    with pytest.raises(ValueError, match="closure certificate"):
        verify_draws(changed, 100, titles.__getitem__, api, worlds=2)
    with pytest.raises(ValueError, match="ordinal prefix"):
        verify_draws([records[1], records[0], records[2]], 100, titles.__getitem__, api, worlds=2)


def test_offline_cached_proofs_detect_body_and_url_tampering(tmp_path):
    from measure.wiki_study import CachedAPI

    url = "https://en.wikipedia.org/w/api.php?action=query"
    key = hashlib.sha256(url.encode()).hexdigest()
    payload = b'{"query":{"pages":[]}}'
    (tmp_path / (key + ".json")).write_bytes(payload)
    write_json(
        tmp_path / (key + ".proof.json"),
        {
            "url": url,
            "status": 200,
            "sha256": hashlib.sha256(payload).hexdigest(),
            "bytes": len(payload),
        },
    )
    assert CachedAPI(tmp_path).read(key)[0] == {"query": {"pages": []}}
    (tmp_path / (key + ".json")).write_bytes(payload + b" ")
    with pytest.raises(ValueError, match="proof mismatch"):
        CachedAPI(tmp_path).read(key)


def test_world_projection_preserves_overlaps_and_unknown_readership(tmp_path):
    from measure.adapters.wikipedia import COLUMNS
    from measure.wiki_study import aggregate_views, project_worlds

    selected = [{"ordinal": 39, "article_ids": [7, 8]}, {"ordinal": 4, "article_ids": [8, 9]}]
    source = tmp_path / "month.tsv"
    rows = []
    for page in (7, 8, 9):
        fields = [""] * len(COLUMNS)
        fields[COLUMNS.index("page_id")] = str(page)
        rows.append("\t".join(fields) + "\n")
    source.write_text("".join(rows))
    projected = project_worlds(selected, [source], tmp_path / "worlds")
    assert [row["source_rows"] for row in projected] == [2, 2]
    assert (tmp_path / "worlds/39.tsv").read_text() == rows[0] + rows[1]
    assert (tmp_path / "worlds/4.tsv").read_text() == rows[1] + rows[2]
    for page, daily in ((7, [1] * 366), (8, [None] + [2] * 365), (9, [0] * 366)):
        write_json(tmp_path / f"{page}.json", {"page": str(page), "daily": daily})
    views = aggregate_views(selected, tmp_path)
    assert views[0]["pageviews"][0]["pageviews"] is None
    assert views[0]["pageviews"][1]["pageviews"] == 21
    assert views[1]["pageviews"][1]["pageviews"] == 14
    assert views[0]["pageviews"][-1] == {
        "period": 52,
        "pageviews": 6,
        "unavailable_days": 0,
        "unavailable_page_days": 0,
        "days": 2,
    }
    assert views[0]["readership_availability"]["unavailable_article_ids"] == ["8"]
    assert views[0]["readership_availability"]["available_world_weeks"] == 52


def test_validation_retains_worlds_with_unknown_views_without_inventing_pairs(tmp_path):
    from measure.validate import wikipedia

    history = tmp_path / "empty.tsv"
    history.write_text("")
    categories = [
        {
            "category": f"Category:Fixture{index}",
            "depth": 2,
            "articles": list(range(500)),
            "history": str(history),
            "pageviews": [{"period": 0, "pageviews": None}, {"period": 1, "pageviews": 1}],
        }
        for index in range(40)
    ]
    report = wikipedia({"sampling_frame_complete": True, "categories": categories}, tmp_path)
    assert report["selected_worlds"] == 40
    assert all(row["available_view_weeks"] == 1 for row in report["world_availability"])
    assert all(
        row["worlds_with_paired_changes"] == 0 and row["paired_changes"] == 0
        for row in report["results"].values()
    )


def test_history_only_uses_real_78_column_adapter_without_readership(tmp_path):
    from measure.adapters.wikipedia import COLUMNS
    from measure.validate import wikipedia_histories

    history = tmp_path / "actual-format.tsv"
    rows = []
    for actor, date in (
        (11, "2024-01-02T12:00:00Z"),
        (12, "2024-01-09T12:00:00Z"),
        (11, "2024-12-20T12:00:00Z"),
        (0, "2024-12-20T13:00:00Z"),
    ):
        row = dict.fromkeys(COLUMNS, "")
        row.update(
            event_entity="revision",
            event_type="create",
            event_timestamp=date,
            event_user_id=str(actor),
            page_id="7",
            page_namespace_historical="0",
            page_first_edit_timestamp="2010-01-01T00:00:00Z",
        )
        rows.append("\t".join(row[key] for key in COLUMNS) + "\n")
    history.write_text("".join(rows))
    categories = [
        {
            "id": index,
            "category": f"Category:HistoryFixture{index}",
            "depth": 2,
            "articles": list(range(500)),
            "history": str(history),
        }
        for index in range(40)
    ]
    report = wikipedia_histories(
        {"sampling_frame_complete": True, "categories": categories}, tmp_path
    )
    assert report["stage"] == "history_only" and report["selected_worlds"] == 40
    assert "results" not in report and "pageviews" not in report
    assert all(
        row["counts"]["events"] == 3
        and row["weekly_periods"] == 53
        and row["source_counts"]["excluded_missing_author_or_page"] == 1
        for row in report["worlds"]
    )
    actual = json.loads((tmp_path / "wikipedia/0/report.json").read_text())
    assert actual["periods"][0]["counts"]["events"] == 1
    assert actual["periods"][1]["counts"]["events"] == 1
    assert actual["periods"][2]["counts"]["events"] == 0
    assert actual["periods"][50]["counts"]["events"] == 1
    assert actual["periods"][52]["counts"]["events"] == 0


def test_acquisition_and_replay_follow_shared_sample_configuration(tmp_path, monkeypatch):
    import importlib

    from measure import config, wiki_history, wiki_sample, wiki_study, wiki_views

    modules = (wiki_history, wiki_sample, wiki_study, wiki_views)
    try:
        with monkeypatch.context() as patch:
            patch.setattr(config, "WIKI_WORLDS", 2)
            patch.setattr(config, "WIKI_BOOTSTRAPS", 7)
            for module in modules:
                importlib.reload(module)
            write_json(
                tmp_path / "selected.json",
                {
                    "selected": [
                        {"article_ids": [1], "article_titles": {"1": "A"}},
                        {"article_ids": [2], "article_titles": {"2": "B"}},
                    ]
                },
            )
            assert wiki_views.selected_pages(tmp_path) == {"1": "A", "2": "B"}
            # The configured sample passes the count gate and reaches the source check.
            with pytest.raises(ValueError, match="All12"):
                wiki_history.filter_histories(tmp_path, tmp_path / "selected.json", 1)
            with pytest.raises(ValueError, match="requires 2"):
                wiki_study.verify_draws([], 10, None, None)
            assert wiki_sample.METHOD["worlds"] == 2
            assert wiki_sample.METHOD["selection"].startswith("First2 ")
            assert wiki_sample.METHOD["bootstrap"].startswith("7 ")
            write_json(tmp_path / "selected.json", {"selected": []})
            with pytest.raises(ValueError, match="completed 2-world"):
                wiki_views.selected_pages(tmp_path)
    finally:
        for module in modules:
            importlib.reload(module)
