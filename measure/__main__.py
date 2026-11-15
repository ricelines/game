"""python -m measure ADAPTER --input PATH --output DIRECTORY."""

import argparse
import json
from pathlib import Path

from measure.output import load_adapter, run, write_report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("adapter")
    parser.add_argument("--input", type=Path)
    parser.add_argument("--output", type=Path, default=Path("measure-output"))
    parser.add_argument("--period", type=float)
    parser.add_argument(
        "--options", type=Path, help="Explicit JSON adapter options such as pages/roles/columns"
    )
    args = parser.parse_args()
    options = json.loads(args.options.read_text()) if args.options else {}
    if args.period is not None:
        options["period_s"] = args.period
    adapter = load_adapter(args.adapter, args.input, **options)
    report = run(adapter)
    write_report(report, args.output)
    print(
        json.dumps(
            {
                "definitions_version": 1,
                "output": str(args.output),
                "events": report["counts"]["events"],
            }
        )
    )


if __name__ == "__main__":
    main()
