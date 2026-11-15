"""Join complete native S7 segments without changing events or reseeding a continuation."""

import argparse
import json
import tempfile
from pathlib import Path

from measure.output import digest
from measure.recover import recover

IDENTITY = ("spec", "world", "runtime", "registry_hash", "step_hz", "seed_actors")


def join(sources, output):
    sources, output = [Path(source) for source in sources], Path(output)
    if not sources or output.resolve() in {path.resolve() for path in sources}:
        raise ValueError("Joining requires distinct source segments and a separate output")
    output.parent.mkdir(parents=True, exist_ok=True)
    segments, original_header = [], None
    previous_id, previous_step = 0, 0
    with tempfile.TemporaryDirectory(prefix="s7-segments-", dir=output.parent) as temporary:
        pending = Path(temporary) / "joined.jsonl"
        with pending.open("wb") as combined:
            for index, source in enumerate(sources):
                decoded = Path(temporary) / f"segment-{index}.jsonl"
                proof = recover(source, decoded)
                if proof["incomplete_zstd_frame"] or proof["excluded_final_fragment_bytes"]:
                    raise ValueError("Cannot join an incomplete S7 source: " + str(source))
                first_id = None
                with decoded.open("rb") as rows:
                    header_line = next(rows)
                    header = json.loads(header_line)
                    if not header_line.endswith(b"\n") or any(
                        key not in header for key in IDENTITY
                    ):
                        raise ValueError("S7 segment lacks a complete native identity header")
                    if original_header is None:
                        original_header = header
                        combined.write(header_line)
                    elif any(
                        header.get(key) != original_header.get(key)
                        for key in (*IDENTITY, "roles", "t0")
                    ):
                        raise ValueError("S7 segment world/registry/clock identity differs")
                    if header.get("resumed_step", 0) < previous_step:
                        raise ValueError("S7 segment resumes before the preceding retained record")
                    for line in rows:
                        if not line.strip():
                            continue
                        event = json.loads(line)
                        identity, step = event.get("id"), event.get("step")
                        if type(identity) is not int or identity != previous_id + 1:
                            raise ValueError("S7 segment has a gap or duplicate event ID")
                        if type(step) is not int or step < previous_step:
                            raise ValueError("S7 segment steps are not chronological")
                        if not line.endswith(b"\n"):
                            raise ValueError("Native S7 source lacks its final record newline")
                        first_id = identity if first_id is None else first_id
                        previous_id, previous_step = identity, step
                        combined.write(line)
                segments.append(
                    {
                        "source": str(source),
                        "source_sha256": proof["original_sha256"],
                        "source_bytes": proof["original_bytes"],
                        "decoded_sha256": proof["output_sha256"],
                        "complete_zstd_frames": proof["complete_zstd_frames"],
                        "first_id": first_id,
                        "last_id": previous_id if first_id is not None else None,
                        "header": header,
                    }
                )
        pending.replace(output)
    result = {
        "kind": "Actual complete S7 segments; original header and event bytes retained",
        "output": str(output),
        "output_sha256": digest(output),
        "records": previous_id,
        "last_step": previous_step,
        "segments": segments,
    }
    output.with_suffix(".segments.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True, help="Decoded combined JSONL")
    options = parser.parse_args()
    print(json.dumps(join(options.input, options.output)))


if __name__ == "__main__":
    main()
