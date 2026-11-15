"""Durable2024 readership acquisition, aligned by actual public page-move identity."""

import argparse
import hashlib
import json
import time
import urllib.error
import urllib.parse
from collections import defaultdict
from datetime import UTC, datetime
from pathlib import Path

from measure.wiki_sources import PublicAPI, sha256, write_json

START = datetime(2024, 1, 1, tzinfo=UTC).timestamp()
END = datetime(2025, 1, 1, tzinfo=UTC).timestamp()
DAY = 86400
DAYS = 366
DISALLOWED_PATH = Path(__file__).with_name("adapters") / "aqs-disallowed.tsv"
DISALLOWED_SHA256 = sha256(DISALLOWED_PATH)
DISALLOWED = {
    article.lower()
    for project, article in (
        line.split("\t", 1) for line in DISALLOWED_PATH.read_text().splitlines()
    )
    if project == "en.wikipedia"
}


def timestamp(value):
    return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()


def selected_pages(directory):
    record = json.loads((directory / "selected.json").read_text())
    if len(record["selected"]) != 40:
        raise ValueError("Readership acquisition requires the completed40-world draw")
    return {
        str(key): title
        for world in record["selected"]
        for key, title in world["article_titles"].items()
    }


def current_metadata(api, directory, pages):
    path = directory / "current-pages.json"
    if path.exists():
        result = json.loads(path.read_text())
        if set(result["pages"]) != set(pages):
            raise ValueError("Cached metadata belongs to another selected article set")
        return result
    result = {"pages": {}, "proofs": []}
    identities = sorted(pages, key=int)
    for start in range(0, len(identities), 50):
        data, proof = api.query(
            prop="info", pageids="|".join(identities[start : start + 50]), curtimestamp=1
        )
        result["proofs"].append(proof)
        result["end"] = data["curtimestamp"]
        for page in data["query"]["pages"]:
            result["pages"][str(page["pageid"])] = page
    write_json(path, result)
    return result


def acquire_moves(api, directory, end):
    """Complete bounded public move log, without filtering on mutable current titles."""
    path = directory / "moves-progress.json"
    state = (
        json.loads(path.read_text())
        if path.exists()
        else {
            "end": end,
            "proofs": [],
            "continue": {},
            "done": False,
            "rows": 0,
        }
    )
    if state["end"] != end:
        raise ValueError("Move-log continuation belongs to a different time boundary")
    while not state["done"]:
        data, proof = api.query(
            list="logevents",
            letype="move",
            lestart="2024-01-01T00:00:00Z",
            leend=end,
            ledir="newer",
            lelimit=500,
            leprop="ids|title|timestamp|details",
            **state["continue"],
        )
        rows = data["query"]["logevents"]
        state["rows"] += len(rows)
        state["proofs"].append(proof)
        state["continue"] = data.get("continue", {})
        state["done"] = not state["continue"]
        write_json(path, state)
        if len(state["proofs"]) % 50 == 0 or state["done"]:
            print(
                json.dumps(
                    {
                        "stage": "moves",
                        "rows": state["rows"],
                        "through": rows[-1]["timestamp"] if rows else end,
                        "requests": len(state["proofs"]),
                        "done": state["done"],
                    }
                ),
                flush=True,
            )
    return state


def lineages(api, directory, metadata, moves):
    by_page = defaultdict(list)
    hidden = 0
    for proof in moves["proofs"]:
        data = json.loads((api.directory / (proof + ".json")).read_text())
        for row in data["query"]["logevents"]:
            # logpage is the moved page ID. pageid can identify the newly-created redirect.
            identity = str(row.get("logpage", 0))
            if "params" not in row or "title" not in row:
                hidden += 1
            if identity in metadata["pages"]:
                by_page[identity].append(row)
    result = {
        "pages": {},
        "unattributed_hidden_move_rows": hidden,
        "move_log_rows": moves["rows"],
        "move_log_end": moves["end"],
    }
    for identity, page in metadata["pages"].items():
        if "missing" in page:
            result["pages"][identity] = {
                "not_available": "Selected page was deleted before metadata"
            }
            continue
        title = page["title"]
        changes = sorted(by_page[identity], key=lambda row: (row["timestamp"], row["logid"]))
        coherent = True
        for row in reversed(changes):
            if row.get("params", {}).get("target_title") != title:
                coherent = False
                break
            title = row["title"]
        if not coherent:
            result["pages"][identity] = {
                "not_available": "Public move chain does not join current title"
            }
            continue
        intervals, start, excluded = [], START, set()
        for row in changes:
            when = timestamp(row["timestamp"])
            if when >= END:
                break
            intervals.append({"start": start, "end": when, "title": title})
            if (when - START) % DAY:
                excluded.add(int((when - START) // DAY))
            start, title = when, row["params"]["target_title"]
        intervals.append({"start": start, "end": END, "title": title})
        result["pages"][identity] = {
            "intervals": intervals,
            "move_days_unavailable": sorted(excluded),
            "moves": [
                {
                    "id": row["logid"],
                    "at": row["timestamp"],
                    "from": row["title"],
                    "to": row["params"]["target_title"],
                }
                for row in changes
            ],
        }
    write_json(directory / "historical-titles.json", result)
    return result


def desired_titles(lineage):
    result = [None] * DAYS
    if "not_available" in lineage:
        return result
    for interval in lineage["intervals"]:
        for day in range(DAYS):
            when = START + day * DAY
            if interval["start"] <= when < interval["end"]:
                result[day] = interval["title"]
    for day in lineage["move_days_unavailable"]:
        result[day] = None
    return result


def fetch_title(api, title):
    """Decode published2024 counts: mature omitted items/404 are zero, not a load failure."""
    if title.replace(" ", "_").lower() in DISALLOWED:
        return {
            "daily": [None] * DAYS,
            "status": "suppressed",
            "source_sha256": DISALLOWED_SHA256,
        }
    encoded = urllib.parse.quote(title.replace(" ", "_"), safe="")
    url = (
        "https://wikimedia.org/api/rest_v1/metrics/pageviews/per-article/"
        f"en.wikipedia/all-access/user/{encoded}/daily/2024010100/2024123100"
    )
    key = hashlib.sha256(url.encode()).hexdigest()
    error_path = api.directory / (key + ".error.json")
    cached_error = json.loads(error_path.read_text()) if error_path.exists() else None
    status, proof, data = 200, key, None
    if cached_error is not None and cached_error["status"] == 404:
        if cached_error["url"] != url:
            raise ValueError("Cached404 belongs to another article request")
        status = 404
    else:
        try:
            data, proof = api.get(url)
        except urllib.error.HTTPError as error:
            if error.code != 404:
                raise
            status = 404
    values, omitted = [0] * DAYS, set(range(DAYS))
    if status == 200:
        for row in data["items"]:
            date = datetime.strptime(row["timestamp"][:8], "%Y%m%d").replace(tzinfo=UTC)
            day = int((date.timestamp() - START) // DAY)
            if 0 <= day < DAYS:
                values[day] = int(row["views"])
                omitted.discard(day)
    record = {"daily": values, "proof": proof, "status": status, "omitted_days": sorted(omitted)}
    if status == 404:
        record["error_sha256"] = sha256(error_path)
    return record


def align_readership(api, lineage):
    """Share raw source decoding and canonical/moved-day alignment across collection and replay."""
    titles = desired_titles(lineage)
    responses = {
        title: fetch_title(api, title) for title in sorted({value for value in titles if value})
    }
    return {
        "daily": [
            responses[title]["daily"][day] if title is not None else None
            for day, title in enumerate(titles)
        ],
        "title_responses": {
            title: {field: value for field, value in response.items() if field != "daily"}
            for title, response in responses.items()
        },
    }


def acquire_readership(api, directory, lineages_record, *, priority_pages=()):
    output = directory / "readership"
    output.mkdir(exist_ok=True)
    started = time.monotonic()
    missing_days, completed = 0, 0
    priority = set(priority_pages)
    identities = sorted(priority, key=int) + sorted(
        set(lineages_record["pages"]) - priority, key=int
    )
    for identity in identities:
        path = output / (identity + ".json")
        if path.exists():
            record = json.loads(path.read_text())
        else:
            lineage = lineages_record["pages"][identity]
            record = {"page": identity, **align_readership(api, lineage)}
            record["unavailable_days"] = sum(value is None for value in record["daily"])
            write_json(path, record)
        missing_days += record["unavailable_days"]
        completed += 1
        if completed % 100 == 0 or (priority and completed == len(priority)):
            status = {
                "stage": "readership",
                "completed_pages": completed,
                "total_pages": len(lineages_record["pages"]),
                "unavailable_page_days": missing_days,
                "requests": api.requests,
                "request_seconds": api.seconds,
                "elapsed_seconds": time.monotonic() - started,
                "priority_pages": len(priority),
                "priority_complete": completed >= len(priority),
            }
            write_json(directory / "readership-progress.json", status)
            print(json.dumps(status), flush=True)
    write_json(
        directory / "readership-complete.json",
        {
            "pages": completed,
            "unavailable_page_days": missing_days,
            "elapsed_seconds": time.monotonic() - started,
            "definition": "all-access/user daily canonical-title intervals; move days unavailable",
        },
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path(".cache/measure-wikipedia"))
    parser.add_argument("--wait-for-sample", action="store_true")
    parser.add_argument("--priority-worlds", type=int, nargs="*", default=[])
    options = parser.parse_args()
    # Waiting does not turn a failed or partial draw into a complete one.
    while options.wait_for_sample:
        path = options.output / "selected.json"
        if path.exists() and len(json.loads(path.read_text())["selected"]) == 40:
            break
        time.sleep(30)
    pages = selected_pages(options.output)
    api = PublicAPI(options.output / "api-cache")
    metadata = current_metadata(api, options.output, pages)
    moves = acquire_moves(api, options.output, metadata["end"])
    histories = lineages(api, options.output, metadata, moves)
    selected = json.loads((options.output / "selected.json").read_text())["selected"]
    priority = {
        str(page)
        for world in selected
        if world["ordinal"] in options.priority_worlds
        for page in world["article_ids"]
    }
    acquire_readership(api, options.output, histories, priority_pages=priority)


if __name__ == "__main__":
    main()
