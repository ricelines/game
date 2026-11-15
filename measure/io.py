"""Public record readers; compressed streams stay streaming."""

import bz2
import gzip
import io
import json
from datetime import UTC, datetime
from pathlib import Path

import zstandard


def seconds(value):
    if isinstance(value, int | float):
        return float(value)
    stamp = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if stamp.tzinfo is None:
        stamp = stamp.replace(tzinfo=UTC)
    return stamp.timestamp()


def text_stream(path):
    path = Path(path)
    if path.suffix == ".zst":
        return io.TextIOWrapper(zstandard.ZstdDecompressor().stream_reader(path.open("rb")))
    if path.suffix == ".bz2":
        return bz2.open(path, "rt")
    if path.suffix == ".gz":
        return gzip.open(path, "rt")
    return path.open()


def json_lines(path):
    with text_stream(path) as source:
        for line in source:
            if line.strip():
                yield json.loads(line)


def rows(path, columns=None):
    path = Path(path)
    if path.suffix == ".parquet":
        import pyarrow.parquet as parquet

        for batch in parquet.ParquetFile(path).iter_batches(columns=columns):
            yield from batch.to_pylist()
    elif path.suffix == ".json":
        with path.open() as source:
            yield from json.load(source)
    else:
        yield from json_lines(path)


def chronological(events):
    """External sort for public dumps larger than memory; no live gameplay database."""
    import sqlite3
    import tempfile

    directory = Path(__file__).resolve().parents[1] / ".cache/measure-sort"
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=directory) as temporary:
        with sqlite3.connect(Path(temporary) / "events.sqlite") as database:
            database.execute("CREATE TABLE events (t REAL, row TEXT)")
            database.executemany(
                "INSERT INTO events VALUES(?,?)",
                ((event["t"], json.dumps(event, separators=(",", ":"))) for event in events),
            )
            for (encoded,) in database.execute("SELECT row FROM events ORDER BY t,rowid"):
                yield json.loads(encoded)
