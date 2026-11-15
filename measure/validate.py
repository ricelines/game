"""Preregister first, then run the two frozen S11.5 procedures without a model judge."""

import argparse
import json
import math
import random
from datetime import UTC, datetime
from pathlib import Path

import numpy as np
from scipy.stats import spearmanr

from measure.output import digest, load_adapter, run, write_report

SEED = 20261115
EXPECTATIONS = {
    "inaction": {
        "measure": "acting_share",
        "extreme": "lowest",
        "world": "s1-agentworld-openai-db",
    },
    "violence": {
        "measure": "cross_author_share",
        "extreme": "highest",
        "world": "s1-agentworld-grok-db",
    },
    "yes98": {
        "measure": "role_divergence",
        "extreme": "lowest",
        "world": "s1-agentworld-claude-db",
    },
}


def preregister(output):
    # This is deliberately the first study operation; no input data/results have been opened.
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(UTC).isoformat()
    record = {
        "definitions_version": 1,
        "timestamp": stamp,
        "seed": SEED,
        "expectations": EXPECTATIONS,
        "wikipedia": {
            "worlds": 40,
            "article_count": [500, 5000],
            "category_depth": 2,
            "year": 2024,
            "statistic": "Spearman(delta measure, delta log pageviews), pooled worlds",
            "bootstrap": "1000 resamples of worlds",
            "interval": 0.95,
        },
        "emergence": {
            "unit": "season and model",
            "significance_test": False,
            "expectation_source": "https://arxiv.org/html/2606.08367v1#S5.SS3",
        },
    }
    filename = "preregistration-" + stamp.replace(":", "-") + ".json"
    (output / filename).write_text(json.dumps(record, indent=2) + "\n")
    return filename


def emergence(specification, output):
    if not specification:
        return {"not_available": "No public Emergence input manifest supplied"}
    table, rows = [], {}
    for world in specification["worlds"]:
        path = Path(world["file"])
        actual = digest(path)
        if world.get("sha256") and actual != world["sha256"]:
            raise ValueError("Input hash mismatch for " + world["world"])
        report = run(load_adapter("emergence", path, name=world["world"]))
        write_report(report, output / "emergence" / world["world"])
        measures = report["measures"]
        row = {
            "world": world["world"],
            "source_sha256": actual,
            "source": world.get("source"),
            "events": report["counts"]["events"],
            **{
                key: measures[key]
                for key in (
                    "acting_share",
                    "loop_rate",
                    "role_divergence",
                    "persistence",
                    "cross_author_share",
                )
            },
        }
        table.append(row)
        rows[row["world"]] = row
    expectations = {}
    for name, expectation in EXPECTATIONS.items():
        key, target = expectation["measure"], expectation["world"]
        values = {world: row[key] for world, row in rows.items() if row[key] is not None}
        if target not in values:
            expectations[name] = {
                "not_available": "Expected world or measure unavailable",
                **expectation,
            }
            continue
        extreme = (
            min(values.values()) if expectation["extreme"] == "lowest" else max(values.values())
        )
        expectations[name] = {
            **expectation,
            "held": values[target] == extreme,
            "expected_world_value": values[target],
            "observed_extreme": extreme,
            "worlds_at_extreme": [world for world, value in values.items() if value == extreme],
        }
    return {
        "table": table,
        "expectations": expectations,
        "significance_test": False,
        "license": specification.get("license"),
        "commit": specification.get("commit"),
    }


def pooled_statistic(worlds):
    x = [value for world in worlds for value in world[0]]
    y = [value for world in worlds for value in world[1]]
    if len(x) < 3 or len(set(x)) < 2 or len(set(y)) < 2:
        return None
    value = float(spearmanr(x, y).statistic)
    return value if math.isfinite(value) else None


def correlate_worlds(series, *, seed=SEED, resamples=1000):
    point = pooled_statistic(series)
    if point is None:
        return {
            "not_available": "Insufficient/nonvarying paired weekly changes",
            "worlds": len(series),
            "worlds_with_paired_changes": sum(bool(world[0]) for world in series),
            "paired_changes": sum(len(world[0]) for world in series),
        }
    rng = np.random.default_rng(seed)
    samples = []
    for _index in range(resamples):
        selected = [series[index] for index in rng.integers(0, len(series), size=len(series))]
        value = pooled_statistic(selected)
        if value is not None:
            samples.append(value)
    interval = [float(value) for value in np.quantile(samples, [0.025, 0.975])] if samples else None
    return {
        "spearman": point,
        "interval_95": interval,
        "worlds": len(series),
        "worlds_with_paired_changes": sum(bool(world[0]) for world in series),
        "paired_changes": sum(len(world[0]) for world in series),
        "bootstrap_world_resamples": resamples,
        "defined_bootstrap_samples": len(samples),
    }


def choose_wikipedia_worlds(specification):
    sampling = specification.get("sampling", {})
    if sampling.get("mode") == "complete-universe-uniform-rejection-v1":
        from measure.wiki_study import verify_selection

        selection, proof = verify_selection(sampling["acquisition_directory"])
        if proof != sampling["proof"]:
            raise ValueError("Study sampling proof differs from current verified acquisition")
        categories = {world["id"]: world for world in specification["categories"]}
        expected = selection["selected"]
        if len(categories) != 40:
            raise ValueError("Study does not contain exactly the40 selected worlds")
        chosen = []
        for row in expected:
            world = categories[row["ordinal"]]
            if (
                world["category"] != row["category"]
                or sorted(set(world["articles"])) != row["article_ids"]
                or world["depth"] != 2
            ):
                raise ValueError("Study category membership differs from verified selection")
            chosen.append(world)
        return chosen, {
            "sampling_mode": sampling["mode"],
            "eligible_sampling_frame": None,
            "complete_eligible_frame_enumerated": False,
            "acquisition_proof": proof,
        }
    if not specification.get("sampling_frame_complete"):
        return None, {
            "not_available": "Requires an enumerated complete eligible frame or the "
            "verified complete-universe uniform rejection acquisition; an API subset cannot pass"
        }
    eligible = [
        world
        for world in specification["categories"]
        if 500 <= len(set(world["articles"])) <= 5000 and world["depth"] == 2
    ]
    eligible.sort(key=lambda row: row["category"])
    if len(eligible) < 40:
        return None, {
            "not_available": "Fewer than40 eligible category worlds",
            "eligible": len(eligible),
        }
    return random.Random(SEED).sample(eligible, 40), {
        "sampling_mode": "enumerated-complete-eligible-frame",
        "eligible_sampling_frame": len(eligible),
        "complete_eligible_frame_enumerated": True,
    }


def wikipedia_history(world, output):
    if world.get("history_sha256") and digest(world["history"]) != world["history_sha256"]:
        raise ValueError("Selected world's actual history projection hash differs")
    adapter = load_adapter(
        "wikipedia",
        world["history"],
        pages=world["articles"] + world.get("talk_pages", []),
        period_s=604800,
        t0=1704067200,
        end_s=1735689600,
    )
    report = run(adapter)
    identity = str(world.get("id", world["category"])).replace("/", "_")
    write_report(report, output / "wikipedia" / identity)
    return report


def wikipedia_histories(specification, output):
    """Compute actual frozen weekly measures; no pageviews or correlations are inferred."""
    if not specification:
        return {"not_available": "Wikipedia acquisition/input manifest is absent"}
    chosen, sampling = choose_wikipedia_worlds(specification)
    if chosen is None:
        return sampling
    worlds = []
    for world in chosen:
        report = wikipedia_history(world, output)
        worlds.append(
            {
                "id": world.get("id"),
                "category": world["category"],
                "articles": len(world["articles"]),
                "history_sha256": digest(world["history"]),
                "counts": report["counts"],
                "source_counts": report["info"]["source_counts"],
                "weekly_periods": len(
                    [row for row in report["periods"] if 0 <= row["period"] <= 52]
                ),
            }
        )
    return {
        "stage": "history_only",
        "selected_worlds": 40,
        **sampling,
        "worlds": worlds,
        "correlations": "Not computed: this stage has no readership input",
    }


def wikipedia(specification, output):
    if not specification:
        return {"not_available": "Wikipedia acquisition/input manifest is absent"}
    chosen, sampling = choose_wikipedia_worlds(specification)
    if chosen is None:
        return sampling
    (output / "wikipedia-sample.json").write_text(
        json.dumps({"seed": SEED, "categories": chosen, **sampling}, indent=2) + "\n"
    )
    by_measure, availability = {}, []
    # Fixed seven-day periods anchored on Monday 2024-01-01. All periods intersecting2024,
    # including the final two-day partial period, are explicit. Unknown inputs remain NA.
    for world in chosen:
        report = wikipedia_history(world, output)
        views = {
            int(row["period"]): float(row["pageviews"]) if row["pageviews"] is not None else None
            for row in world["pageviews"]
        }
        if any(
            value is not None and (not math.isfinite(value) or value < 0)
            for value in views.values()
        ):
            raise ValueError("Pageviews must be nonnegative finite counts or NA")
        periods = {
            row["period"]: row["measures"] for row in report["periods"] if 0 <= row["period"] <= 52
        }
        paired = {}
        for name in report["measures"]:
            x, y = [], []
            for period in range(1, 53):
                before, after = (
                    periods.get(period - 1, {}).get(name),
                    periods.get(period, {}).get(name),
                )
                left, right = views.get(period - 1), views.get(period)
                if (
                    before is None
                    or after is None
                    or left is None
                    or right is None
                    or left <= 0
                    or right <= 0
                ):
                    continue
                x.append(after - before)
                y.append(math.log(right) - math.log(left))
            by_measure.setdefault(name, []).append((x, y))
            paired[name] = len(x)
        availability.append(
            {
                "id": world.get("id"),
                "category": world["category"],
                "articles": len(world["articles"]),
                "events": report["counts"]["events"],
                "available_view_weeks": sum(value is not None for value in views.values()),
                "zero_view_weeks": sum(value == 0 for value in views.values()),
                "paired_changes": paired,
                "readership": world.get("readership_availability"),
            }
        )
    return {
        "selected_worlds": 40,
        **sampling,
        "period_alignment": "2024-01-01 +7-day periods; final partial period included",
        "missing_policy": "Any unknown article-day makes its summed world-day/week unavailable; "
        "log change requires two positive known weekly sums",
        "world_availability": availability,
        "results": {name: correlate_worlds(series) for name, series in by_measure.items()},
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--input", type=Path, help="JSON with emergence and/or wikipedia source manifests"
    )
    parser.add_argument("--output", type=Path, default=Path("measure-validation"))
    parser.add_argument(
        "--history-only", action="store_true", help="Compute Wikipedia histories without views"
    )
    options = parser.parse_args()
    registration = preregister(options.output)
    specification = json.loads(options.input.read_text()) if options.input else {}
    result = {
        "definitions_version": 1,
        "preregistration": registration,
        "input_sha256": digest(options.input) if options.input else None,
        "adapter_class_sha256": digest(
            Path(__file__).parent / "adapters/emergence-classes-v1.json"
        ),
    }
    if options.history_only:
        result["wikipedia"] = wikipedia_histories(specification.get("wikipedia"), options.output)
    else:
        result["emergence"] = emergence(specification.get("emergence"), options.output)
        result["wikipedia"] = wikipedia(specification.get("wikipedia"), options.output)
    (options.output / "validation.json").write_text(
        json.dumps(result, indent=2, allow_nan=False) + "\n"
    )
    print(
        json.dumps(
            {
                "definitions_version": 1,
                "output": str(options.output),
                "preregistration": registration,
            }
        )
    )


if __name__ == "__main__":
    main()
