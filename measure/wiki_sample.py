"""Uniform rejection from a complete pinned category universe, with replayable proofs."""

import argparse
import json
import random
import sqlite3
import time
from collections import deque
from datetime import UTC, datetime
from pathlib import Path

from measure.wiki_sources import PublicAPI, category_universe, write_json

SEED = 20261115
METHOD = {
    "mode": "complete-universe-uniform-rejection-v1",
    "seed": SEED,
    "worlds": 40,
    "universe": "Every row of the complete English category SQL table, snapshot20261101",
    "draw": "Python Random(seed).randrange(1,N+1); discard duplicate ordinals; retain draw order",
    "eligibility": "500..5000 distinct namespace0 nonredirect pages at category depths0,1,2",
    "membership": "Actual serial API closure, cached retrieval interval; not a historical graph",
    "rejection": (
        "Leaf categoryinfo pages upper bound <500, or complete closure <500, or >5000 proven"
    ),
    "selection": "First40 eligible distinct draws; no outcome data consulted",
    "complete_eligible_frame_enumerated": False,
    "historical_views": (
        "2024 all-access user daily views; align titles to historical page identity"
    ),
    "weeks": "2024-01-01 +7days, including final2-day partial period",
    "statistic": "Spearman(delta measure,delta log summed pageviews), pooled worlds",
    "bootstrap": "1000 fixed-seed resamples of worlds,95% interval",
    "talk_pages": (
        "Not added: category world consists of articles, not their uncategorized talk pages"
    ),
    "missing": "Unavailable attribution/view days are reported, never fabricated as zero",
}


def draw_ordinals(count, seed=SEED):
    rng, seen = random.Random(seed), set()
    while len(seen) < count:
        ordinal = rng.randrange(1, count + 1)
        if ordinal not in seen:
            seen.add(ordinal)
            yield ordinal


def category_info(api, titles):
    result, proof = api.query(prop="categoryinfo", titles="|".join(titles))
    info = {
        row["title"]: row.get("categoryinfo", {"pages": 0, "subcats": 0, "files": 0})
        for row in result["query"]["pages"]
    }
    for row in result.get("query", {}).get("normalized", []):
        if row["from"] != row["to"]:
            info[row["from"]] = {**info[row["to"]], "normalized_title": row["to"]}
    return info, proof


def closure(api, title, maximum=5000):
    pending, queued = deque([(title, 0)]), {title}
    articles, categories, proofs = {}, [], []
    while pending:
        current, depth = pending.popleft()
        continuation = {}
        while True:
            data, proof = api.query(
                generator="categorymembers",
                gcmtitle=current,
                gcmnamespace="0|14",
                gcmlimit=500,
                prop="info",
                **continuation,
            )
            proofs.append(proof)
            for page in data.get("query", {}).get("pages", []):
                if page["ns"] == 0 and "redirect" not in page:
                    articles[page["pageid"]] = page["title"]
                elif page["ns"] == 14 and depth < 2 and page["title"] not in queued:
                    queued.add(page["title"])
                    pending.append((page["title"], depth + 1))
            if len(articles) > maximum:
                return {
                    "articles": articles,
                    "closed": False,
                    "proofs": proofs,
                    "categories": categories,
                    "reason": "more_than5000_proven",
                }
            continuation = data.get("continue")
            if not continuation:
                break
        categories.append({"title": current, "depth": depth})
    return {
        "articles": articles,
        "closed": True,
        "proofs": proofs,
        "categories": categories,
        "reason": "complete_closure",
    }


def sample(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    registration = directory / "acquisition-preregistration.json"
    if registration.exists():
        if json.loads(registration.read_text())["method"] != METHOD:
            raise ValueError("Existing acquisition registration differs")
    else:
        write_json(registration, {"registered": datetime.now(UTC).isoformat(), "method": METHOD})
    database, universe = category_universe(directory)
    db = sqlite3.connect(database)
    api = PublicAPI(directory / "api-cache")
    log = directory / "draws.jsonl"
    previous = [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
    existing = {row["ordinal"]: row for row in previous}
    selected, distinct = [], set()
    ordinals = iter(draw_ordinals(universe["categories"]))
    started = time.monotonic()
    while len(selected) < 40:
        batch = []
        for _index in range(50):
            try:
                ordinal = next(ordinals)
            except StopIteration:
                break
            row = db.execute("SELECT title FROM categories WHERE n=?", (ordinal,)).fetchone()
            try:
                title = "Category:" + row[0].decode("utf-8").replace("_", " ")
            except UnicodeDecodeError:
                title = None
            batch.append((ordinal, title, row[0].hex()))
        if not batch:
            raise ValueError("Complete candidate universe exhausted before40 eligible worlds")
        new = [title for ordinal, title, _raw in batch if ordinal not in existing and title]
        information, info_proof = category_info(api, new) if new else ({}, None)
        for ordinal, title, raw in batch:
            if ordinal in existing:
                record = existing[ordinal]
            else:
                counts = information[title] if title else None
                record = {
                    "draw": len(existing) + 1,
                    "ordinal": ordinal,
                    "category": title,
                    "categoryinfo": counts,
                    "info_proof": info_proof,
                }
                if title is None:
                    record.update(
                        {
                            "eligible": False,
                            "reason": "invalid_utf8_category_title",
                            "title_bytes_hex": raw,
                        }
                    )
                elif counts.get("normalized_title"):
                    record.update({"eligible": False, "reason": "noncanonical_source_title"})
                elif counts["subcats"] == 0 and counts["pages"] < 500:
                    record.update({"eligible": False, "reason": "leaf_upper_bound_below500"})
                else:
                    tree = closure(api, title)
                    articles = tree.pop("articles")
                    record.update(tree)
                    record["article_count"] = len(articles)
                    record["eligible"] = tree["closed"] and 500 <= len(articles) <= 5000
                    # The >5000 lower-bound certificate must retain its actual unique IDs.
                    record["article_ids"] = sorted(articles)
                    if record["eligible"]:
                        record["article_titles"] = {str(k): v for k, v in articles.items()}
                with log.open("a") as output:
                    output.write(json.dumps(record, ensure_ascii=False) + "\n")
                existing[ordinal] = record
            if record["eligible"]:
                selected.append(record)
                distinct.update(record["article_ids"])
                write_json(
                    directory / "selected.json",
                    {
                        "method": METHOD,
                        "universe": universe,
                        "draws_evaluated": record["draw"],
                        "selected": selected,
                        "distinct_articles": len(distinct),
                    },
                )
                print(
                    json.dumps(
                        {
                            "selected": len(selected),
                            "draws": record["draw"],
                            "category": title,
                            "articles": record["article_count"],
                            "distinct_articles": len(distinct),
                            "requests": api.requests,
                            "request_seconds": api.seconds,
                            "elapsed_seconds": time.monotonic() - started,
                        }
                    ),
                    flush=True,
                )
                if len(selected) == 40:
                    break
        print(
            json.dumps(
                {
                    "progress_draws": len(existing),
                    "selected": len(selected),
                    "requests": api.requests,
                    "request_seconds": api.seconds,
                    "elapsed_seconds": time.monotonic() - started,
                }
            ),
            flush=True,
        )
    db.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path(".cache/measure-wikipedia"))
    options = parser.parse_args()
    sample(options.output)


if __name__ == "__main__":
    main()
