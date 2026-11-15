"""Bounded offline-measure workload, separate from native server-step benchmarks."""

import json
import resource
import statistics
import time
from pathlib import Path

from measure import compute


def workload(count):
    for index in range(count):
        yield {
            "t": index * 0.1,
            "actor": str(index % 100),
            "type": "change" if index % 7 else "look",
            "object": str(index % 1000),
        }


def main():
    info = {
        "name": "benchmark",
        "t0": 0,
        "period_s": 3600,
        "seed_actors": [],
        "classes": {"change": "change", "look": "look"},
    }
    samples = []
    for _index in range(5):
        started = time.perf_counter()
        report = compute(info, workload(100000))
        samples.append(time.perf_counter() - started)
        assert report["counts"]["events"] == 100000
    result = {
        "kind": "synthetic offline measure benchmark, not gameplay or external validation",
        "events": 100000,
        "agents": 100,
        "repetitions": 5,
        "seconds": {"p50": statistics.median(samples), "p95_observed_max_small_n": max(samples)},
        "peak_rss_mib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024,
    }
    output = Path(".cache/measure-benchmark.json")
    output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
