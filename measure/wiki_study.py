"""Replay the complete-universe draw, project actual histories, and assemble S11 inputs."""

import argparse
import hashlib
import json
import sqlite3
import urllib.parse
from collections import Counter, defaultdict
from datetime import UTC, datetime
from pathlib import Path

from measure.config import WIKI_WORLDS
from measure.wiki_history import COLUMNS, build_creation_records, build_existence
from measure.wiki_sample import METHOD, closure, draw_ordinals
from measure.wiki_sources import API, CATEGORY_SOURCE, sha256, write_json
from measure.wiki_views import (
    DAY,
    DAYS,
    DISALLOWED_SHA256,
    START,
    align_readership,
    desired_titles,
    timestamp,
)


class CachedAPI:
    """Offline proof replay: missing, mismatched, or modified responses fail visibly."""

    def __init__(self, directory):
        self.directory = Path(directory)
        self.checked = {}

    def read(self, key):
        payload = (self.directory / (key + ".json")).read_bytes()
        if key not in self.checked:
            proof = json.loads((self.directory / (key + ".proof.json")).read_text())
            if (
                proof["status"] != 200
                or hashlib.sha256(proof["url"].encode()).hexdigest() != key
                or hashlib.sha256(payload).hexdigest() != proof["sha256"]
                or len(payload) != proof["bytes"]
            ):
                raise ValueError("Cached public response proof mismatch: " + key)
            self.checked[key] = proof
        return json.loads(payload), self.checked[key]

    def query(self, **parameters):
        arguments = {
            "action": "query",
            "format": "json",
            "formatversion": 2,
            "maxlag": 5,
            **parameters,
        }
        url = API + "?" + urllib.parse.urlencode(sorted(arguments.items()))
        key = hashlib.sha256(url.encode()).hexdigest()
        data, _proof = self.read(key)
        return data, key

    def get(self, url):
        key = hashlib.sha256(url.encode()).hexdigest()
        data, _proof = self.read(key)
        return data, key


def recorded_info(api, key, title):
    data, proof = api.read(key)
    query = urllib.parse.parse_qs(urllib.parse.urlsplit(proof["url"]).query)
    if query.get("prop") != ["categoryinfo"] or title not in query["titles"][0].split("|"):
        raise ValueError("Category-info certificate does not query the drawn title")
    normalized = {row["from"]: row["to"] for row in data["query"].get("normalized", [])}
    actual = normalized.get(title, title)
    page = next(row for row in data["query"]["pages"] if row["title"] == actual)
    counts = dict(page.get("categoryinfo", {"pages": 0, "subcats": 0, "files": 0}))
    if actual != title:
        counts["normalized_title"] = actual
    return counts


def verify_draws(records, population, title_at, api, *, worlds=WIKI_WORLDS):
    """Verify the exact deterministic prefix and every eligibility decision from raw proofs."""
    accepted, reasons = [], Counter()
    ordinals = draw_ordinals(population)
    for index, record in enumerate(records, 1):
        ordinal = next(ordinals)
        if record["draw"] != index or record["ordinal"] != ordinal:
            raise ValueError("Draw transcript is not the preregistered distinct ordinal prefix")
        raw = title_at(ordinal)
        try:
            title = "Category:" + raw.decode("utf-8").replace("_", " ")
        except UnicodeDecodeError:
            title = None
        if record["category"] != title:
            raise ValueError("Draw title differs from the complete pinned universe")
        if title is None:
            eligible, reason = False, "invalid_utf8_category_title"
            if record["title_bytes_hex"] != raw.hex():
                raise ValueError("Invalid-title certificate discarded source bytes")
        else:
            counts = recorded_info(api, record["info_proof"], title)
            if counts != record["categoryinfo"]:
                raise ValueError("Category-info counts differ from their response")
            if counts.get("normalized_title"):
                eligible, reason = False, "noncanonical_source_title"
            elif counts["subcats"] == 0 and counts["pages"] < 500:
                eligible, reason = False, "leaf_upper_bound_below500"
            else:
                tree = closure(api, title)
                articles = tree.pop("articles")
                eligible = tree["closed"] and 500 <= len(articles) <= 5000
                reason = tree["reason"]
                if (
                    any(record[name] != value for name, value in tree.items())
                    or record["article_count"] != len(articles)
                    or record["article_ids"] != sorted(articles)
                ):
                    raise ValueError("Category closure certificate differs from raw responses")
                if eligible and record["article_titles"] != {
                    str(key): value for key, value in articles.items()
                }:
                    raise ValueError("Accepted article titles differ from raw closure")
        if record["eligible"] != eligible or record["reason"] != reason:
            raise ValueError("Eligibility decision differs from its certificate")
        reasons[reason] += 1
        if eligible:
            accepted.append(record)
            if len(accepted) == worlds and index != len(records):
                raise ValueError("Transcript continues after the first required eligible worlds")
    if len(accepted) != worlds:
        raise ValueError(f"Draw has {len(accepted)} eligible worlds; requires {worlds}")
    return accepted, dict(reasons)


def verify_selection(directory):
    directory = Path(directory)
    registration = json.loads((directory / "acquisition-preregistration.json").read_text())
    selection = json.loads((directory / "selected.json").read_text())
    universe = json.loads((directory / "category-universe.json").read_text())
    if registration["method"] != METHOD or selection["method"] != METHOD:
        raise ValueError("Acquisition method differs from its preregistration")
    if any(universe[key] != value for key, value in CATEGORY_SOURCE.items()):
        raise ValueError("Category universe does not identify the pinned complete dump")
    archive = directory / "enwiki-20261101-category.sql.gz"
    with archive.open("rb") as source:
        checksum = hashlib.file_digest(source, "sha1").hexdigest()
    database = directory / "category-universe.sqlite"
    if (
        archive.stat().st_size != CATEGORY_SOURCE["bytes"]
        or checksum != CATEGORY_SOURCE["sha1"]
        or sha256(archive) != universe["sha256"]
        or sha256(database) != universe["sqlite_sha256"]
        or selection["universe"] != universe
    ):
        raise ValueError("Complete candidate universe source/hash mismatch")
    db = sqlite3.connect(f"file:{database}?mode=ro", uri=True)
    try:
        if db.execute("SELECT COUNT(*) FROM categories").fetchone()[0] != universe["categories"]:
            raise ValueError("Candidate universe row count differs")
        records = [
            json.loads(line) for line in (directory / "draws.jsonl").read_text().splitlines()
        ]
        api = CachedAPI(directory / "api-cache")
        accepted, reasons = verify_draws(
            records,
            universe["categories"],
            lambda n: db.execute("SELECT title FROM categories WHERE n=?", (n,)).fetchone()[0],
            api,
        )
    finally:
        db.close()
    distinct = len({page for row in accepted for page in row["article_ids"]})
    if (
        selection["selected"] != accepted
        or selection["draws_evaluated"] != len(records)
        or selection["distinct_articles"] != distinct
    ):
        raise ValueError("Selected sample differs from the verified draw prefix")
    times = [row["retrieved"] for row in api.checked.values()]
    proof = {
        "verified": True,
        "mode": METHOD["mode"],
        "seed": METHOD["seed"],
        "candidate_universe": universe["categories"],
        "complete_eligible_frame_enumerated": False,
        "draws": len(records),
        "reasons": reasons,
        "selected_worlds": len(accepted),
        "distinct_articles": distinct,
        "checked_responses": len(api.checked),
        "membership_retrieval_interval": [min(times), max(times)],
        "preregistration_sha256": sha256(directory / "acquisition-preregistration.json"),
        "draws_sha256": sha256(directory / "draws.jsonl"),
        "selection_sha256": sha256(directory / "selected.json"),
        "universe": universe,
    }
    write_json(directory / "sample-proof.json", proof)
    return selection, proof


def memberships(selected):
    result = defaultdict(list)
    for index, world in enumerate(selected):
        for page in world["article_ids"]:
            result[str(page)].append(index)
    return result


def project_worlds(selected, monthly, output):
    """Route each actual retained TSV row once, including overlaps between sampled worlds."""
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    by_page = memberships(selected)
    paths = [output / f"{row['ordinal']}.tsv" for row in selected]
    temporary = [path.with_suffix(".partial") for path in paths]
    streams = [path.open("wb") for path in temporary]
    counts = [0] * len(selected)
    page_index = COLUMNS.index("page_id")
    try:
        for source in monthly:
            with Path(source).open("rb") as data:
                for row in data:
                    fields = row.split(b"\t", page_index + 1)
                    page = fields[page_index].decode("ascii")
                    for index in by_page.get(page, []):
                        streams[index].write(row)
                        counts[index] += 1
    finally:
        for stream in streams:
            stream.close()
    result = []
    for path, pending, count in zip(paths, temporary, counts, strict=True):
        pending.replace(path)
        result.append({"history": str(path), "history_sha256": sha256(path), "source_rows": count})
    return result


def identity_existence(record, history, witness):
    """Creation/observation proves existence by an instant, never earlier absence."""
    creation_times = [timestamp(value) for value in history["creation_timestamps"]]
    evidence = {}
    if creation_times:
        evidence["history_creation"] = min(creation_times)
    if "creation" in record:
        creation = record["creation"]
        if str(creation.get("logpage", 0)) != record["page"]:
            raise ValueError("Cached creation log belongs to another stable page ID")
        when = timestamp(creation["timestamp"])
        evidence["creation_log"] = when
        creation_times.append(when)
    if witness is not None:
        evidence["pageviews_complete"] = witness["exists_by_s"]
    if not evidence:
        return {"status": "unknown", "sources": {}}
    return {
        "status": "bounded",
        "exists_by_s": min(evidence.values()),
        "sources": evidence,
        "distinct_creation_times": len(set(creation_times)),
    }


def project_readership(api, original, lineage, creation, witness=None):
    """Project one cached page with the same source/identity policy as final assembly."""
    projected = align_readership(api, lineage)
    bound = identity_existence(original, creation, witness)
    blocked = [
        {"day": day, "views": value}
        for day, value in enumerate(projected["daily"])
        if value is not None
        and value > 0
        and (bound["status"] == "unknown" or START + day * DAY < bound["exists_by_s"])
    ]
    record = {
        "page": original["page"],
        "daily": [
            value
            if value == 0
            or (bound["status"] == "bounded" and START + day * DAY >= bound["exists_by_s"])
            else None
            for day, value in enumerate(projected["daily"])
        ],
        "title_responses": projected["title_responses"],
        "identity_existence": bound,
        "source_creation_status": creation["status"],
        "unbounded_positive_days": blocked,
    }
    if witness is not None:
        record["jan1_witness"] = witness
    return record


def normalize_readership(directory, creations, output, existence=None):
    """Offline stable-ID projection; original API and readership cache bytes stay unchanged."""
    directory, output = Path(directory), Path(output)
    output.mkdir(parents=True, exist_ok=True)
    titles_path = directory / "historical-titles.json"
    titles = json.loads(titles_path.read_text())["pages"]
    if set(titles) != set(creations["pages"]):
        raise ValueError("Creation evidence belongs to another selected article set")
    witnesses = existence["pages"] if existence is not None else {}
    earlier_witnesses = (
        {row["page"]: row for row in existence["earlier_witnesses"]} if existence else {}
    )
    if existence is not None and (
        existence["historical_titles_sha256"] != sha256(titles_path)
        or existence["selected_sha256"] != creations["selected_sha256"]
        or existence["history_projection_sha256"] != creations["history_projection_sha256"]
        or not set(witnesses) <= set(titles)
        or not set(earlier_witnesses) <= set(titles)
    ):
        raise ValueError("Existence evidence belongs to another historical title set")
    witnesses = {**witnesses, **earlier_witnesses}
    api = CachedAPI(directory / "api-cache")
    counts = Counter()
    manifest = hashlib.sha256()
    for identity in sorted(titles, key=int):
        source = directory / "readership" / (identity + ".json")
        original = json.loads(source.read_text())
        if original["page"] != identity:
            raise ValueError("Readership cache belongs to another stable page ID")
        record = project_readership(
            api, original, titles[identity], creations["pages"][identity], witnesses.get(identity)
        )
        bound = record["identity_existence"]
        counts[bound["status"] + "_existence_pages"] += 1
        counts["pages_with_jan1_witness"] += identity in witnesses
        counts["creation_after_jan1_witness_pages"] += identity in earlier_witnesses
        counts["multiple_creation_times_pages"] += bound.get("distinct_creation_times", 0) > 1
        changed = sum(
            before != after
            for before, after in zip(original["daily"], record["daily"], strict=True)
        )
        counts["changed_pages"] += bool(changed)
        counts["changed_page_days"] += changed
        counts["unavailable_page_days"] += sum(value is None for value in record["daily"])
        record["original_readership_sha256"] = sha256(source)
        path = output / (identity + ".json")
        write_json(path, record)
        manifest.update((identity + ":" + sha256(path) + "\n").encode())
    result = {
        "schema": "stable-page-readership-v1",
        "pages": len(titles),
        "counts": dict(counts),
        "projection_sha256_chain": manifest.hexdigest(),
        "historical_titles_sha256": sha256(titles_path),
        "aqs_disallowed_titles_sha256": DISALLOWED_SHA256,
        "definition": (
            "Creation timestamps and exact ID/title observations establish existence by an "
            "instant, not earlier absence. Positive counts before that bound are NA. "
            "Published2024 omitted items/404 decode to0 under the mature-data source contract; "
            "zero title counts attribute no predecessor traffic and require no existence proof. "
            "Unknown/moved/suppressed titles remain NA; request failures never become zero."
        ),
    }
    return result


def aggregate_views(selected, directory):
    by_page = memberships(selected)
    totals = [[0] * DAYS for _row in selected]
    missing = [[0] * DAYS for _row in selected]
    unavailable_articles = [set() for _row in selected]
    identity_bounds = [Counter() for _row in selected]
    creation_status = [Counter() for _row in selected]
    existence_bounds = [0 for _row in selected]
    for page, worlds in by_page.items():
        record = json.loads((Path(directory) / (page + ".json")).read_text())
        values = record["daily"]
        if record["page"] != page or len(values) != DAYS:
            raise ValueError("Readership record belongs to another page or year")
        for index in worlds:
            if "identity_existence" in record:
                identity_bounds[index][record["identity_existence"]["status"]] += 1
                creation_status[index][record["source_creation_status"]] += 1
            existence_bounds[index] += "jan1_witness" in record
        for day, value in enumerate(values):
            if value is not None and (type(value) is not int or value < 0):
                raise ValueError("Readership must contain nonnegative integer counts or NA")
            for index in worlds:
                if value is None:
                    missing[index][day] += 1
                    unavailable_articles[index].add(page)
                else:
                    totals[index][day] += value
    result = []
    for index, world in enumerate(selected):
        daily = [None if missing[index][day] else totals[index][day] for day in range(DAYS)]
        weeks = []
        for start in range(0, DAYS, 7):
            days = daily[start : start + 7]
            weeks.append(
                {
                    "period": start // 7,
                    "pageviews": None if None in days else sum(days),
                    "unavailable_days": sum(value is None for value in days),
                    "unavailable_page_days": sum(missing[index][start : start + 7]),
                    "days": len(days),
                }
            )
        result.append(
            {
                "pageviews": weeks,
                "readership_availability": {
                    "articles": len(world["article_ids"]),
                    "articles_with_unavailable_days": len(unavailable_articles[index]),
                    "unavailable_article_ids": sorted(unavailable_articles[index], key=int),
                    "available_world_days": sum(value is not None for value in daily),
                    "available_world_weeks": sum(row["pageviews"] is not None for row in weeks),
                    "known_zero_weeks": sum(row["pageviews"] == 0 for row in weeks),
                    "unavailable_page_days": sum(missing[index]),
                    "identity_bounds": dict(identity_bounds[index]),
                    "source_creation_status": dict(creation_status[index]),
                    "articles_with_jan1_witness": existence_bounds[index],
                },
            }
        )
    return result


def build_availability(directory, creations_path, existence_path, output):
    """Certify actual unavailable weeks without treating uncollected pages as evidence.

    Uses the final assembler's per-page projection and verifies the same raw
    response proofs. One actual obstruction suffices for a full-world sum;
    all articles must be collected before a week can be called available.
    """
    directory, output = Path(directory), Path(output)
    selected_path = directory / "selected.json"
    titles_path = directory / "historical-titles.json"
    selected = json.loads(selected_path.read_text())["selected"]
    titles = json.loads(titles_path.read_text())["pages"]
    creations = json.loads(Path(creations_path).read_text())
    pages = set(memberships(selected))
    if (
        pages != set(titles)
        or pages != set(creations["pages"])
        or creations["selected_sha256"] != sha256(selected_path)
        or creations["history_projection_sha256"]
        != sha256(directory / "history/history-projection.json")
    ):
        raise ValueError("Availability inputs belong to different selected article sets")
    witnesses = {}
    if existence_path is not None:
        existence = json.loads(Path(existence_path).read_text())
        if (
            existence["selected_sha256"] != creations["selected_sha256"]
            or existence["history_projection_sha256"] != creations["history_projection_sha256"]
            or existence["historical_titles_sha256"] != sha256(titles_path)
        ):
            raise ValueError("Availability existence evidence belongs to another source set")
        witnesses = {
            **existence["pages"],
            **{row["page"]: row for row in existence["earlier_witnesses"]},
        }
    by_page = memberships(selected)
    paths = {
        path.stem: path for path in (directory / "readership").glob("*.json") if path.stem in pages
    }
    api = CachedAPI(directory / "api-cache")
    obstructions = [{} for _world in selected]
    totals = [[0] * 53 for _world in selected]
    cached = [0] * len(selected)
    source_chain = hashlib.sha256()
    for identity in sorted(paths, key=int):
        original = json.loads(paths[identity].read_text())
        if original["page"] != identity:
            raise ValueError("Availability readership cache belongs to another page ID")
        record = project_readership(
            api, original, titles[identity], creations["pages"][identity], witnesses.get(identity)
        )
        original_hash = sha256(paths[identity])
        source_chain.update((identity + ":" + original_hash + "\n").encode())
        title_days = desired_titles(titles[identity])
        positive = {row["day"]: row["views"] for row in record["unbounded_positive_days"]}
        for index in by_page[identity]:
            cached[index] += 1
        for day, value in enumerate(record["daily"]):
            week = day // 7
            if value is not None:
                for index in by_page[identity]:
                    totals[index][week] += value
                continue
            reason = (
                "positive_without_existence_bound"
                if day in positive
                else "move_ambiguity"
                if day in titles[identity].get("move_days_unavailable", [])
                else "unavailable_title"
            )
            recipients = [
                index
                for index in by_page[identity]
                if week not in obstructions[index]
                or (
                    reason == "positive_without_existence_bound"
                    and obstructions[index][week]["reason"] != reason
                )
            ]
            if not recipients:
                continue
            when = datetime.fromtimestamp(START + day * DAY, UTC)
            proof = {
                "reason": reason,
                "page": identity,
                "day": day,
                "date": when.date().isoformat(),
                "title": title_days[day],
                "identity_existence": record["identity_existence"],
                "readership_projection": {"path": str(paths[identity]), "sha256": original_hash},
            }
            if day in positive:
                response_key = record["title_responses"][title_days[day]]["proof"]
                body, response = api.read(response_key)
                item = next(
                    row for row in body["items"] if row["timestamp"][:8] == when.strftime("%Y%m%d")
                )
                proof.update(
                    views=positive[day],
                    response={
                        "key": response_key,
                        "url": response["url"],
                        "sha256": response["sha256"],
                        "item": item,
                    },
                )
            elif reason == "move_ambiguity":
                proof["moves"] = [
                    row
                    for row in titles[identity]["moves"]
                    if int((timestamp(row["at"]) - START) // DAY) == day
                ]
                if not proof["moves"]:
                    raise ValueError("Availability move day lacks an actual recorded move")
            else:
                proof["source"] = titles[identity].get(
                    "not_available", record["title_responses"].get(title_days[day])
                )
            for index in recipients:
                obstructions[index][week] = proof
    move_ids = {
        move["id"]
        for world in obstructions
        for row in world.values()
        for move in row.get("moves", [])
    }
    move_sources = {}
    if move_ids:
        progress = json.loads((directory / "moves-progress.json").read_text())
        for key in progress["proofs"]:
            body, response = api.read(key)
            for row in body["query"]["logevents"]:
                if row["logid"] in move_ids:
                    move_sources[row["logid"]] = {
                        "key": key,
                        "url": response["url"],
                        "sha256": response["sha256"],
                        "item": row,
                    }
            if len(move_sources) == len(move_ids):
                break
        if set(move_sources) != move_ids:
            raise ValueError("Availability move obstruction lacks its raw response proof")
    worlds = []
    statuses = Counter()
    for index, world in enumerate(selected):
        periods = []
        missing = len(world["article_ids"]) - cached[index]
        for week in range(53):
            status = (
                "proven_unavailable"
                if week in obstructions[index]
                else "uncollected"
                if missing
                else "available"
            )
            row = {
                "period": week,
                "days": min(7, DAYS - week * 7),
                "status": status,
                "pageviews": totals[index][week] if status == "available" else None,
            }
            if week in obstructions[index]:
                row["obstruction"] = obstructions[index][week]
            statuses[status] += 1
            periods.append(row)
        worlds.append(
            {
                "category": world["category"],
                "ordinal": world["ordinal"],
                "articles": len(world["article_ids"]),
                "cached_articles": cached[index],
                "uncollected_articles": missing,
                "periods": periods,
            }
        )
    frozen_path = Path(__file__).with_name("frozen-v1.json")
    frozen = json.loads(frozen_path.read_text())
    record = {
        "schema": "wikipedia-worldweek-availability-v1",
        "definitions_version": frozen["definitions_version"],
        "definitions_source_sha256": frozen["source_sha256"],
        "sources": {
            "selected_sha256": sha256(selected_path),
            "historical_titles_sha256": sha256(titles_path),
            "creation_records": {"path": str(creations_path), "sha256": sha256(creations_path)},
            "existence_records": (
                {"path": str(existence_path), "sha256": sha256(existence_path)}
                if existence_path is not None
                else None
            ),
            "projection_source_sha256": sha256(Path(__file__)),
            "decoding_source_sha256": sha256(Path(__file__).with_name("wiki_views.py")),
            "cached_projection_sha256_chain": source_chain.hexdigest(),
        },
        "selected_pages": len(pages),
        "cached_pages": len(paths),
        "uncollected_pages": len(pages) - len(paths),
        "period_counts": dict(statuses),
        "move_responses": move_sources,
        "worlds": worlds,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    write_json(output, record)
    return record


def history_inputs(directory, output):
    """Verify and project the complete selected histories without requiring readership."""
    directory, output = Path(directory), Path(output)
    selected, proof = verify_selection(directory)
    projection = json.loads((directory / "history/history-projection.json").read_text())
    pages = {str(page) for row in selected["selected"] for page in row["article_ids"]}
    selection_hash = hashlib.sha256("\n".join(sorted(pages)).encode()).hexdigest()
    if len(projection["months"]) != 12 or projection["selected_pages"] != len(pages):
        raise ValueError("History projection does not contain all12 months for this sample")
    monthly = []
    for row in projection["months"]:
        path = Path(row["source"]).with_suffix("").with_suffix(".selected.tsv")
        if row["selected_pages_sha256"] != selection_hash or sha256(path) != row["sha256"]:
            raise ValueError("History projection source/selection mismatch")
        monthly.append(path)
    histories = project_worlds(selected["selected"], monthly, output / "histories")
    worlds = []
    for row, history in zip(selected["selected"], histories, strict=True):
        worlds.append(
            {
                "id": row["ordinal"],
                "category": row["category"],
                "articles": row["article_ids"],
                "depth": 2,
                **history,
            }
        )
    record = {
        "wikipedia": {
            "sampling": {
                "mode": METHOD["mode"],
                "acquisition_directory": str(directory),
                "proof": proof,
            },
            "categories": worlds,
            "history": {
                "source": json.loads((directory / "history/history-source.json").read_text()),
                "projection": projection,
            },
        }
    }
    output.mkdir(parents=True, exist_ok=True)
    write_json(output / "history-input.json", record)
    return record


def certified_unavailable_views(coverage):
    """An actual obstruction proves a sum unavailable regardless of unfinished other terms."""
    periods = coverage["periods"]
    if any(row["status"] != "proven_unavailable" for row in periods):
        raise ValueError("Every world-week needs an actual obstruction, not uncollected input")
    return {
        "pageviews": [
            {"period": row["period"], "days": row["days"], "pageviews": None} for row in periods
        ],
        "readership_availability": {
            "articles": coverage["articles"],
            "cached_articles": coverage["cached_articles"],
            "uncollected_articles": coverage["uncollected_articles"],
            "available_world_weeks": 0,
            "proven_unavailable_world_weeks": len(periods),
            "obstruction_article_ids": sorted(
                {row["obstruction"]["page"] for row in periods}, key=int
            ),
            "complete_annual_acquisition": False,
        },
    }


def assemble(directory, output, *, existence_bulk=None, certified_unavailable=False):
    directory, output = Path(directory), Path(output)
    record = history_inputs(directory, output)
    worlds = record["wikipedia"]["categories"]
    pages = {str(page) for world in worlds for page in world["articles"]}
    creations_path = output / "page-creations.json"
    creations = build_creation_records(directory, creations_path)
    existence = None
    existence_path = output / "page-existence-bounds.json"
    if existence_bulk is not None:
        existence = build_existence(directory, creations, existence_bulk, existence_path)
    if certified_unavailable:
        availability_path = output / "availability.json"
        availability = build_availability(
            directory,
            creations_path,
            existence_path if existence is not None else None,
            availability_path,
        )
        for world, coverage in zip(worlds, availability["worlds"], strict=True):
            if world["id"] != coverage["ordinal"]:
                raise ValueError("Availability certificate belongs to another sampled world")
            world.update(certified_unavailable_views(coverage))
        record["wikipedia"]["readership"] = {
            "complete": False,
            "selected_pages": len(pages),
            "cached_pages": availability["cached_pages"],
            "uncollected_pages": availability["uncollected_pages"],
            "availability_certificate": {
                "path": str(availability_path),
                "sha256": sha256(availability_path),
                "period_counts": availability["period_counts"],
            },
            "reason": (
                "Every sampled world-week has an actual cached source obstruction. "
                "Uncollected values cannot repair an already undefined sum; "
                "complete annual readership acquisition is not claimed."
            ),
        }
        write_json(output / "study-input.json", record)
        return record
    complete = json.loads((directory / "readership-complete.json").read_text())
    if complete["pages"] != len(pages):
        raise ValueError("Readership acquisition has not completed the selected article set")
    selected = [{"article_ids": world["articles"]} for world in worlds]
    bounded = output / "readership-identity"
    normalization = normalize_readership(directory, creations, bounded, existence)
    normalization["creation_evidence_sha256"] = sha256(creations_path)
    if existence is not None:
        normalization["existence_evidence"] = {
            "path": str(existence_path),
            "sha256": sha256(existence_path),
            "source": existence["source"],
            "counts": existence["counts"],
        }
    write_json(bounded / "normalization.json", normalization)
    for world, readership in zip(worlds, aggregate_views(selected, bounded), strict=True):
        world.update(readership)
    record["wikipedia"]["readership"] = {
        **complete,
        "complete": True,
        "identity_normalization": normalization,
    }
    write_json(output / "study-input.json", record)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("verify", "histories", "assemble", "availability"))
    parser.add_argument("--input", type=Path, default=Path(".cache/measure-wikipedia"))
    parser.add_argument("--output", type=Path, default=Path(".cache/measure-wikipedia/study"))
    parser.add_argument(
        "--existence-bulk",
        type=Path,
        help="Optional verified Jan1 user dump for stable-ID witnesses",
    )
    parser.add_argument(
        "--certified-unavailable",
        action="store_true",
        help="Assemble partial acquisition only when actual obstructions prove every world-week NA",
    )
    options = parser.parse_args()
    if options.stage == "verify":
        _selection, proof = verify_selection(options.input)
        print(json.dumps(proof))
    elif options.stage == "availability":
        existence = options.output / "page-existence-bounds.json"
        record = build_availability(
            options.input,
            options.output / "page-creations.json",
            existence if existence.exists() else None,
            options.output / "availability.json",
        )
        print(json.dumps({"cached_pages": record["cached_pages"], **record["period_counts"]}))
    else:
        record = (
            history_inputs(options.input, options.output)
            if options.stage == "histories"
            else assemble(
                options.input,
                options.output,
                existence_bulk=options.existence_bulk,
                certified_unavailable=options.certified_unavailable,
            )
        )
        print(
            json.dumps(
                {"output": str(options.output), "worlds": len(record["wikipedia"]["categories"])}
            )
        )


if __name__ == "__main__":
    main()
