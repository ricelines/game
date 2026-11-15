"""Explicitly export complete decoded S7 records after an interrupted writer.

The original is never modified. A malformed newline-terminated record fails;
only a final incomplete fragment may be excluded, with its exact byte count.
This is source recovery, not a change to any frozen measurement definition.
"""

import argparse
import hashlib
import json
from collections import Counter
from pathlib import Path

import zstandard


def recover(source: Path, output: Path):
    if source.resolve() == output.resolve():
        raise ValueError("Recovery output must differ from the original")
    output.parent.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    decoder = None
    pending = b""
    frames = decoded_bytes = records = 0
    nulls = Counter()
    last = None

    def record(line):
        nonlocal records, last
        if not line.strip():
            return
        row = json.loads(line)
        if not isinstance(row, dict):
            raise ValueError("S7 records must be JSON objects")
        records += 1
        nulls.update(key for key, value in row.items() if value is None)
        last = {key: row[key] for key in ("id", "step", "kind") if key in row}

    with source.open("rb") as original, output.open("wb") as prefix:
        while chunk := original.read(1024 * 1024):
            digest.update(chunk)
            while chunk:
                if decoder is None:
                    decoder = zstandard.ZstdDecompressor().decompressobj()
                decoded = decoder.decompress(chunk)
                decoded_bytes += len(decoded)
                pending += decoded
                while (newline := pending.find(b"\n")) >= 0:
                    line, pending = pending[: newline + 1], pending[newline + 1 :]
                    record(line)
                    prefix.write(line)
                if decoder.eof:
                    frames += 1
                    chunk = decoder.unused_data
                    decoder = None
                else:
                    chunk = b""
        tail_bytes = 0
        if pending:
            try:
                record(pending)
            except (json.JSONDecodeError, UnicodeDecodeError):
                tail_bytes = len(pending)
            else:
                prefix.write(pending)
    with output.open("rb") as prefix:
        output_digest = hashlib.file_digest(prefix, "sha256").hexdigest()
    provenance = {
        "kind": "explicit complete decoded S7 prefix; original unchanged",
        "original": str(source),
        "original_sha256": digest.hexdigest(),
        "original_bytes": source.stat().st_size,
        "output": str(output),
        "output_sha256": output_digest,
        "decoded_bytes": decoded_bytes,
        "retained_bytes": output.stat().st_size,
        "records_including_header": records,
        "last_complete_record": last,
        "complete_zstd_frames": frames,
        "incomplete_zstd_frame": decoder is not None,
        "excluded_final_fragment_bytes": tail_bytes,
        "nullable_fields": dict(nulls),
    }
    output.with_suffix(".provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    return provenance


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(recover(args.input, args.output), indent=2))


if __name__ == "__main__":
    main()
