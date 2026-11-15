"""Offline stable-page creation and existence evidence from actual-format pinned history rows."""

import bz2
import hashlib
import json
from datetime import UTC, datetime

import pytest

from measure.wiki_history import COLUMNS, build_creation_records, build_existence
from measure.wiki_sources import write_json


def source_row(page, created="", first_edit=""):
    fields = dict.fromkeys(COLUMNS, "")
    fields.update(
        page_id=str(page),
        page_creation_timestamp=created,
        page_first_edit_timestamp=first_edit,
    )
    return ("\t".join(fields[key] for key in COLUMNS) + "\n").encode()


def acquisition(directory, groups, months):
    write_json(
        directory / "selected.json",
        {"selected": [{"article_ids": pages} for pages in groups]},
    )
    pages = {str(page) for group in groups for page in group}
    selection = hashlib.sha256("\n".join(sorted(pages)).encode()).hexdigest()
    records = []
    for index, rows in enumerate(months, 1):
        archive = directory / "history" / f"2026-10.enwiki.2024-{index:02}.tsv.bz2"
        projected = archive.with_suffix("").with_suffix(".selected.tsv")
        projected.parent.mkdir(exist_ok=True)
        payload = b"".join(rows)
        projected.write_bytes(payload)
        records.append(
            {
                "source": str(archive),
                "source_sha256": "a" * 64,
                "selected_pages_sha256": selection,
                "sha256": hashlib.sha256(payload).hexdigest(),
                "retained": len(rows),
            }
        )
    write_json(
        directory / "history/history-projection.json",
        {"selected_pages": len(pages), "months": records},
    )
    return records


def test_creations_use_creation_not_first_edit_and_preserve_unknown_conflicts(tmp_path):
    acquisition(
        tmp_path,
        [[7, 8], [8, 9, 10]],
        [
            [
                source_row(7, "2024-01-10 12:00:00.0", "2000-01-01 00:00:00.0"),
                source_row(7),
                source_row(8, "2024-01-01 00:00:00.0"),
            ],
            [source_row(8, "2024-01-02 00:00:00.0"), source_row(9)],
        ],
    )
    output = tmp_path / "study/page-creations.json"
    result = build_creation_records(tmp_path, output)
    assert json.loads(output.read_text()) == result
    assert result["schema"] == "mediawiki-page-creations-v1"
    assert result["counts"] == {
        "selected_pages": 4,
        "source_rows": 5,
        "known": 1,
        "unknown": 2,
        "conflict": 1,
        "pages_without_rows": 1,
        "null_creation_rows": 2,
        "invalid_creation_rows": 0,
        "invalid_first_edit_rows": 0,
    }
    known = result["pages"]["7"]
    assert known["created_s"] == datetime(2024, 1, 10, 12, tzinfo=UTC).timestamp()
    assert known["first_edit_min_s"] == datetime(2000, 1, 1, tzinfo=UTC).timestamp()
    assert known["creation_timestamps"] == ["2024-01-10T12:00:00Z"]
    assert result["pages"]["8"]["status"] == "conflict"
    assert result["pages"]["8"]["created_s"] is None
    assert result["pages"]["9"]["status"] == result["pages"]["10"]["status"] == "unknown"
    assert result["pages"]["10"]["source_rows"] == 0
    assert all(len(row["sha256"]) == 64 for row in result["sources"])
    assert all(row["source_archive_sha256"] == "a" * 64 for row in result["sources"])


def test_changed_month_cannot_publish_a_partially_verified_creation_projection(tmp_path):
    records = acquisition(
        tmp_path,
        [[7]],
        [[source_row(7, "2024-01-01 00:00:00.0")], [source_row(7)]],
    )
    output = tmp_path / "page-creations.json"
    build_creation_records(tmp_path, output)
    original = output.read_bytes()
    changed = records[-1]
    path = tmp_path / "history/2026-10.enwiki.2024-02.selected.tsv"
    path.write_bytes(source_row(7, "2024-02-01 00:00:00.0"))
    assert hashlib.sha256(path.read_bytes()).hexdigest() != changed["sha256"]
    with pytest.raises(ValueError, match="projection hash/row count"):
        build_creation_records(tmp_path, output)
    assert output.read_bytes() == original


def test_invalid_timestamp_is_reported_unknown_without_inventing_a_creation_time(tmp_path):
    acquisition(tmp_path, [[7]], [[source_row(7, "not-a-date", "also-not-a-date")]])
    result = build_creation_records(tmp_path, tmp_path / "page-creations.json")
    page = result["pages"]["7"]
    assert page["status"] == "unknown" and page["created_s"] is None
    assert page["invalid_creation_rows"] == page["invalid_first_edit_rows"] == 1
    assert result["counts"]["null_creation_rows"] == 0


def existence_fixture(directory):
    acquisition(
        directory,
        [[7, 8, 9, 10, 11, 12]],
        [[source_row(10, "2024-01-02 00:00:00.0")]],
    )
    creations = build_creation_records(directory, directory / "study/page-creations.json")
    names = {
        "7": "Alpha page",
        "8": 'A "quote"',
        "9": "Zero",
        "10": "New",
        "11": "Moved",
        "12": "Absent",
    }
    write_json(
        directory / "historical-titles.json",
        {
            "pages": {
                page: {
                    "intervals": [{"start": 1704067200, "end": 1735689600, "title": title}],
                    "move_days_unavailable": [0] if page == "11" else [],
                }
                for page, title in names.items()
            }
        },
    )
    # Actual Spark-style space-delimited CSV, including an escaped quote in a quoted title.
    payload = (
        b"commons.wikimedia Before 1 desktop 1 A1\n"
        b'en.wikipedia "A_\\"quote\\"" 8 desktop 3 A3\n'
        b"en.wikipedia Alpha_page 7 desktop 2 A2\n"
        b"en.wikipedia Alpha_page 7 mobile-web 4 B4\n"
        b"en.wikipedia Alpha_page 70 desktop 99 A99\n"
        b"en.wikipedia Alpha_page null mobile-app 99 A99\n"
        b"en.wikipedia Alpha_page mobile-app 99 A99\n"
        b"en.wikipedia Alias 7 desktop 99 A99\n"
        b"en.wikipedia Moved 11 desktop 6 A6\n"
        b"en.wikipedia New 10 desktop 1 A1\n"
        b"en.wikipedia Zero 9 desktop 0 A0\n"
        b"fr.wikipedia After 3 desktop 1 A1\n"
    )
    source = directory / "pageviews-20240101-user.bz2"
    source.write_bytes(bz2.compress(payload))
    checksum = hashlib.sha256(source.read_bytes()).hexdigest()
    return creations, source, checksum


def test_existence_requires_positive_exact_id_and_title_without_filling_absent_rows(tmp_path):
    creations, source, checksum = existence_fixture(tmp_path)
    output = tmp_path / "study/page-existence-bounds.json"
    result = build_existence(tmp_path, creations, source, output, expected_sha256=checksum)
    assert json.loads(output.read_text()) == result
    assert set(result["pages"]) == {"7", "8"}
    assert result["pages"]["7"] == {
        "exists_by_s": 1704153600,
        "title": "Alpha_page",
        "views": 6,
        "access_methods": ["desktop", "mobile-web"],
    }
    assert result["pages"]["8"]["title"] == 'A_"quote"'
    assert result["earlier_witnesses"] == [
        {
            "page": "10",
            "created_s": 1704153600,
            "exists_by_s": 1704153600,
            "title": "New",
            "views": 1,
            "access_methods": ["desktop"],
        }
    ]
    assert result["source"]["sha256"] == checksum
    assert result["counts"]["unknown_creation_pages"] == 5
    assert result["counts"]["observed_before_creation_record"] == 1


def test_existence_hash_failure_preserves_previous_verified_projection(tmp_path):
    creations, source, checksum = existence_fixture(tmp_path)
    output = tmp_path / "page-existence-bounds.json"
    build_existence(tmp_path, creations, source, output, expected_sha256=checksum)
    verified = output.read_bytes()
    with pytest.raises(ValueError, match="compressed source hash"):
        build_existence(tmp_path, creations, source, output, expected_sha256="0" * 64)
    assert output.read_bytes() == verified
