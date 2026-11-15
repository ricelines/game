"""Fetch one coherent official history snapshot and retain selected actual TSV rows."""

import argparse
import bz2
import concurrent.futures
import csv
import hashlib
import json
import time
from datetime import UTC, datetime
from pathlib import Path

from measure.wiki_sources import download, sha256, write_json

HISTORY_SIZES = [
    834272026,
    918649150,
    568648679,
    529241436,
    530231446,
    534223644,
    593745639,
    564875481,
    551023434,
    596635523,
    599315121,
    611708187,
]
BASE = "https://dumps.wikimedia.org/other/mediawiki_history/2026-10/enwiki/"
COLUMNS = json.loads(Path(__file__).with_name("adapters").joinpath("wiki-columns.json").read_text())
JAN1_BULK_SHA256 = "44a8b02ee71555e90b14f7faaa13a6b14b448b4f367e01f2dca35cd6e919b463"
JAN1_BULK_URL = (
    "https://dumps.wikimedia.org/other/pageview_complete/2024/2024-01/pageviews-20240101-user.bz2"
)


def fetch_month(month, directory):
    name = f"2026-10.enwiki.2024-{month:02}.tsv.bz2"
    path = Path(directory) / name
    started = time.monotonic()
    download(BASE + name, path, expected_bytes=HISTORY_SIZES[month - 1])
    row = {
        "file": str(path),
        "url": BASE + name,
        "bytes": path.stat().st_size,
        "sha256": sha256(path),
        "seconds": time.monotonic() - started,
    }
    write_json(path.with_suffix(".proof.json"), row)
    print(json.dumps(row), flush=True)
    return row


def fetch(directory, workers):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
        files = list(executor.map(lambda month: fetch_month(month, directory), range(1, 13)))
    write_json(
        directory / "history-source.json",
        {
            "snapshot": "2026-10",
            "year": 2024,
            "files": files,
            "compressed_bytes": sum(HISTORY_SIZES),
        },
    )


def filter_month(source, output, pages):
    """Keep raw rows: analysis sees original schema, author IDs and historical titles."""
    source, output = Path(source), Path(output)
    proof = output.with_suffix(".proof.json")
    selection = hashlib.sha256("\n".join(sorted(pages)).encode()).hexdigest()
    if output.exists() and proof.exists():
        existing = json.loads(proof.read_text())
        if existing["selected_pages_sha256"] == selection and sha256(output) == existing["sha256"]:
            return existing
    selected = {page.encode() for page in pages}
    page_index = COLUMNS.index("page_id")
    temporary = output.with_suffix(".partial")
    rows, retained = 0, 0
    started = time.monotonic()
    with bz2.open(source, "rb") as data, temporary.open("wb") as target:
        for line in data:
            rows += 1
            fields = line.split(b"\t", page_index + 1)
            if len(fields) <= page_index:
                raise ValueError("Truncated actual MediaWiki history row")
            if fields[page_index] in selected:
                if line.count(b"\t") != len(COLUMNS) - 1:
                    raise ValueError("Actual history schema differs from pinned78-field serializer")
                target.write(line)
                retained += 1
            if rows % 1000000 == 0:
                print(
                    json.dumps(
                        {
                            "month": source.name,
                            "scanned": rows,
                            "retained": retained,
                            "seconds": time.monotonic() - started,
                        }
                    ),
                    flush=True,
                )
    temporary.replace(output)
    record = {
        "source": str(source),
        "source_sha256": sha256(source),
        "selected_pages_sha256": selection,
        "rows": rows,
        "retained": retained,
        "sha256": sha256(output),
        "seconds": time.monotonic() - started,
    }
    write_json(proof, record)
    return record


def filter_histories(directory, selection, workers):
    directory = Path(directory)
    chosen = json.loads(Path(selection).read_text())["selected"]
    if len(chosen) != 40:
        raise ValueError("History projection requires all40 fixed sampled worlds")
    pages = {str(page) for world in chosen for page in world["article_ids"]}
    files = sorted(directory.glob("2026-10.enwiki.2024-*.tsv.bz2"))
    if len(files) != 12:
        raise ValueError("All12 coherent2024 monthly histories required")
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
        records = list(
            executor.map(
                lambda source: filter_month(
                    source, source.with_suffix("").with_suffix(".selected.tsv"), pages
                ),
                files,
            )
        )
    write_json(
        directory / "history-projection.json", {"selected_pages": len(pages), "months": records}
    )


def build_creation_records(directory, output):
    """Project source creation timestamps from verified histories, without network requests.

    Neither these records nor first-edit dates prove initial stable-ID absence:
    restoration or merging can preserve an ID observed before a creation record.
    """
    directory, output = Path(directory), Path(output)
    selected_path = directory / "selected.json"
    projection_path = directory / "history/history-projection.json"
    selected = json.loads(selected_path.read_text())["selected"]
    pages = {str(page) for world in selected for page in world["article_ids"]}
    projection = json.loads(projection_path.read_text())
    selection_hash = hashlib.sha256("\n".join(sorted(pages)).encode()).hexdigest()
    if projection["selected_pages"] != len(pages):
        raise ValueError("Creation projection belongs to another selected article set")
    rows = {
        page: {
            "creation": {},
            "first_edit_min_s": None,
            "source_rows": 0,
            "null_creation_rows": 0,
            "invalid_creation_rows": 0,
            "invalid_first_edit_rows": 0,
        }
        for page in pages
    }
    page_index = COLUMNS.index("page_id")
    creation_index = COLUMNS.index("page_creation_timestamp")
    first_edit_index = COLUMNS.index("page_first_edit_timestamp")

    def instant(value):
        parsed = datetime.fromisoformat(value.decode())
        if parsed.tzinfo is None:
            parsed = parsed.replace(tzinfo=UTC)
        parsed = parsed.astimezone(UTC)
        return parsed.timestamp(), parsed.isoformat().replace("+00:00", "Z")

    sources = []
    for month in projection["months"]:
        if month["selected_pages_sha256"] != selection_hash:
            raise ValueError("Creation source selection hash mismatch")
        path = Path(month["source"]).with_suffix("").with_suffix(".selected.tsv")
        checksum = hashlib.sha256()
        count = 0
        with path.open("rb") as stream:
            for line in stream:
                checksum.update(line)
                fields = line.rstrip(b"\r\n").split(b"\t")
                if len(fields) != len(COLUMNS):
                    raise ValueError("Creation source differs from the pinned78-field schema")
                row = rows[fields[page_index].decode("ascii")]
                row["source_rows"] += 1
                count += 1
                if fields[creation_index]:
                    try:
                        created_s, stamp = instant(fields[creation_index])
                    except (ValueError, UnicodeDecodeError):
                        row["invalid_creation_rows"] += 1
                    else:
                        row["creation"][created_s] = stamp
                else:
                    row["null_creation_rows"] += 1
                if fields[first_edit_index]:
                    try:
                        edited_s, _stamp = instant(fields[first_edit_index])
                    except (ValueError, UnicodeDecodeError):
                        row["invalid_first_edit_rows"] += 1
                    else:
                        previous = row["first_edit_min_s"]
                        row["first_edit_min_s"] = (
                            edited_s if previous is None else min(previous, edited_s)
                        )
        actual = checksum.hexdigest()
        if actual != month["sha256"] or count != month["retained"]:
            raise ValueError("Creation source projection hash/row count mismatch")
        sources.append(
            {
                "path": str(path),
                "sha256": actual,
                "rows": count,
                "source_archive_sha256": month["source_sha256"],
            }
        )
    result = {}
    for page in sorted(rows, key=int):
        row = rows[page]
        creation = row.pop("creation")
        status = (
            "conflict"
            if len(creation) > 1
            else "known"
            if creation and not row["invalid_creation_rows"]
            else "unknown"
        )
        result[page] = {
            "status": status,
            "created_s": next(iter(creation)) if status == "known" else None,
            "creation_timestamps": [creation[key] for key in sorted(creation)],
            **row,
        }
    record = {
        "schema": "mediawiki-page-creations-v1",
        "selected_sha256": sha256(selected_path),
        "history_projection_sha256": sha256(projection_path),
        "sources": sources,
        "counts": {
            "selected_pages": len(result),
            "source_rows": sum(source["rows"] for source in sources),
            **{
                status: sum(row["status"] == status for row in result.values())
                for status in ("known", "unknown", "conflict")
            },
            "pages_without_rows": sum(not row["source_rows"] for row in result.values()),
            **{
                key: sum(row[key] for row in result.values())
                for key in (
                    "null_creation_rows",
                    "invalid_creation_rows",
                    "invalid_first_edit_rows",
                )
            },
        },
        "pages": result,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    write_json(output, record)
    return record


def build_existence(directory, creations, bulk_path, output, *, expected_sha256=JAN1_BULK_SHA256):
    """Extract Jan1 ID/title witnesses; neither absent rows nor counts supply readership.

    Page IDs come from the contemporaneous X-Analytics header. A positive row
    with the selected ID and canonical historical title proves existence by
    Jan2, not an initial creation date or an identity-pure Jan1 daily count.
    """
    directory, bulk_path, output = Path(directory), Path(bulk_path), Path(output)
    selected_path = directory / "selected.json"
    titles_path = directory / "historical-titles.json"
    if sha256(selected_path) != creations["selected_sha256"]:
        raise ValueError("Existence evidence belongs to another selected article set")
    lineages = json.loads(titles_path.read_text())["pages"]
    if set(lineages) != set(creations["pages"]):
        raise ValueError("Existence titles differ from the creation projection's selected IDs")
    start = datetime(2024, 1, 1, tzinfo=UTC).timestamp()
    cutoff = start + 86400
    titles = {}
    for page, lineage in lineages.items():
        if "not_available" in lineage or 0 in lineage["move_days_unavailable"]:
            continue
        for interval in lineage["intervals"]:
            if interval["start"] <= start < interval["end"]:
                titles[page.encode()] = interval["title"].replace(" ", "_").encode()
                break
    matched, checksum = {}, hashlib.sha256()
    decoder, remainder = bz2.BZ2Decompressor(), b""
    prefix_done, previous_project = False, b""
    compressed_bytes, english_rows = 0, 0
    with bulk_path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            checksum.update(chunk)
            compressed_bytes += len(chunk)
            if prefix_done:
                continue
            lines = (remainder + decoder.decompress(chunk)).split(b"\n")
            remainder = lines.pop()
            for line in lines:
                project = line.partition(b" ")[0]
                if project < previous_project:
                    raise ValueError("Pageview dump project ordering differs from generation SQL")
                previous_project = project
                if project > b"en.wikipedia":
                    prefix_done = True
                    break
                if project != b"en.wikipedia":
                    continue
                english_rows += 1
                fields = (
                    [
                        part.encode()
                        for part in next(
                            csv.reader([line.decode()], delimiter=" ", escapechar="\\")
                        )
                    ]
                    if line.startswith(b'en.wikipedia "')
                    else line.split(b" ")
                )
                if len(fields) == 5:  # Documented legacy rows without a page ID.
                    continue
                if len(fields) != 6:
                    raise ValueError("Pageview dump differs from its six-field CSV schema")
                _project, title, page, method, total, _hours = fields
                if titles.get(page) != title or int(total) <= 0:
                    continue
                row = matched.setdefault(
                    page.decode(), {"title": title.decode(), "views": 0, "access_methods": set()}
                )
                row["views"] += int(total)
                row["access_methods"].add(method.decode())
    if not prefix_done:
        raise ValueError("Pageview dump lacks the complete sorted English prefix")
    if checksum.hexdigest() != expected_sha256:
        raise ValueError("Pageview dump compressed source hash mismatch")
    pages, earlier_witnesses = {}, []
    for page in sorted(matched, key=int):
        row = matched[page]
        witness = {"exists_by_s": cutoff, **row, "access_methods": sorted(row["access_methods"])}
        creation = creations["pages"][page]
        if creation["status"] == "unknown":
            pages[page] = witness
        elif creation["creation_timestamps"]:
            first = min(
                datetime.fromisoformat(stamp.replace("Z", "+00:00")).timestamp()
                for stamp in creation["creation_timestamps"]
            )
            if first >= cutoff:
                earlier_witnesses.append({"page": page, "created_s": first, **witness})
    record = {
        "schema": "mediawiki-page-existence-v1",
        "selected_sha256": creations["selected_sha256"],
        "historical_titles_sha256": sha256(titles_path),
        "history_projection_sha256": creations["history_projection_sha256"],
        "source": {
            "path": str(bulk_path),
            "sha256": checksum.hexdigest(),
            "bytes": compressed_bytes,
            "day": "2024-01-01",
            "url": JAN1_BULK_URL,
        },
        "counts": {
            "unknown_creation_pages": sum(
                row["status"] == "unknown" for row in creations["pages"].values()
            ),
            "english_rows": english_rows,
            "matching_positive_ids": len(matched),
            "unknown_with_witness": len(pages),
            "observed_before_creation_record": len(earlier_witnesses),
        },
        "pages": pages,
        "earlier_witnesses": earlier_witnesses,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    write_json(output, record)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("fetch", "filter"))
    parser.add_argument("--output", type=Path, default=Path(".cache/measure-wikipedia/history"))
    parser.add_argument(
        "--selection", type=Path, default=Path(".cache/measure-wikipedia/selected.json")
    )
    parser.add_argument("--workers", type=int, choices=(1, 2, 3), default=3)
    options = parser.parse_args()
    if options.stage == "fetch":
        fetch(options.output, options.workers)
    else:
        filter_histories(options.output, options.selection, options.workers)


if __name__ == "__main__":
    main()
