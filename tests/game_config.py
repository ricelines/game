"""Explicit native launch profiles and configuration evidence for gameplay checks."""

import argparse
import hashlib
import math
import tomllib
from pathlib import Path


def positive_seconds(value):
    seconds = float(value)
    if not math.isfinite(seconds) or seconds <= 0:
        raise argparse.ArgumentTypeError("duration must be positive and finite")
    return seconds


def add_arguments(parser):
    parser.add_argument("--config", type=Path, default=Path("config.toml"))
    parser.add_argument(
        "--profile",
        choices=("production", "development"),
        default="production",
        help="development uses the relaxed watchdog; production retains strict timing limits",
    )


def launch_arguments(args):
    return ["--config", str(args.config.resolve()), "--profile", args.profile]


def configuration(args):
    source = args.config.read_bytes()
    document = tomllib.loads(source.decode())
    section = "development" if args.profile == "development" else "scripts"
    return {
        "profile": args.profile,
        "handler_ms": document[section]["HANDLER_MS"],
        "step_hz": document["clock"]["STEP_HZ"],
        "validation_mode": "functional" if args.profile == "development" else "production_limits",
        "config_sha256": hashlib.sha256(source).hexdigest(),
    }


def check_configuration(state, args):
    expected = configuration(args)
    if state["configuration"] != {key: expected[key] for key in ("profile", "handler_ms")}:
        raise AssertionError("native run did not use the requested profile and handler allowance")
