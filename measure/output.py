"""One report JSON and one flat, machine-readable CSV row per period."""

import csv
import hashlib
import importlib
import json
from pathlib import Path

from measure.core import compute


def load_adapter(module_name, path=None, **options):
    module = importlib.import_module(
        "measure.adapters." + module_name if "." not in module_name else module_name
    )
    return module.Adapter(path, **options) if hasattr(module, "Adapter") else module


def run(adapter):
    result = compute(adapter.info(), adapter.events())
    result["definitions_source_sha256"] = json.loads(
        Path(__file__).with_name("frozen-v1.json").read_text()
    )["source_sha256"]
    return result


def write_report(report, directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "report.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    records = []
    for period in report["periods"]:
        row = {"period": period["period"], "start_s": period["start_s"], **period["measures"]}
        row.update({"count_" + key: value for key, value in period["counts"].items()})
        row.update({"na_" + key: value for key, value in period["not_available"].items()})
        records.append(row)
    fields = ["period", "start_s"] + sorted(
        set().union(*(row.keys() for row in records)) - {"period", "start_s"}
    )
    with (directory / "periods.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(records)


def digest(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()
