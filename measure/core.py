"""One forward pass; no model judgments and no causal-chain walks."""

import hashlib
import itertools
import math
from collections import Counter, defaultdict, deque
from statistics import mean

VERSION = 1
MIN_ACTIONS = 20


def quantile(values, fraction):
    values = sorted(values)
    index = (len(values) - 1) * fraction
    low = math.floor(index)
    high = math.ceil(index)
    return values[low] + (values[high] - values[low]) * (index - low)


def jsd(left, right):
    left_total, right_total = sum(left.values()), sum(right.values())
    if not left_total or not right_total:
        raise ValueError("An empty distribution has no JSD")
    total = 0.0
    for key in left.keys() | right.keys():
        p, q = left.get(key, 0) / left_total, right.get(key, 0) / right_total
        middle = (p + q) * 0.5
        if p:
            total += 0.5 * p * math.log2(p / middle)
        if q:
            total += 0.5 * q * math.log2(q / middle)
    return total


class Interval:
    def __init__(self):
        self.events = 0
        self.classes = set()
        self.agent_events = 0
        self.agent_denominator = 0
        self.changes = 0
        self.agent_changes = 0
        self.object_changes = 0
        self.multi_changes = 0
        self.objectless_multi_changes = 0
        self.authors_pairs = set()
        self.lookbacks = []
        self.agent_counts = Counter()
        self.agent_loops = Counter()
        self.ca_types = defaultdict(Counter)
        self.gaps = 0

    def add(self, event, looping, t0):
        self.events += 1
        self.classes.add(event["class"])
        self.authors_pairs.update(itertools.combinations(sorted(event["authors"]), 2))
        chosen = event.get("agent", True)
        if chosen:
            self.agent_events += 1
            self.agent_counts[event["actor"]] += 1
            self.agent_loops[event["actor"]] += int(looping)
            self.agent_denominator += event["class"] in ("change", "move", "speak", "look")
            self.gaps += event.get("result") == "not_modeled"
        change = event["class"] == "change" and event.get("result") in (None, "applied")
        if not change:
            return
        self.changes += 1
        self.object_changes += event["object"] is not None
        multi = len(event["authors"]) >= 2
        self.multi_changes += multi
        self.objectless_multi_changes += multi and event["object"] is None
        if chosen:
            self.agent_changes += 1
            self.ca_types[event["actor"]][event["type"]] += 1
        if event["object"] is not None and not event["seed_object"] and event["t"] > t0:
            self.lookbacks.append((event["t"] - event["oldest"]) / (event["t"] - t0))

    def report(self, roles, supports_gap, previous=None):
        values, unavailable = {}, {}
        active = {actor for actor, count in self.agent_counts.items() if count >= MIN_ACTIONS}
        distributions = {actor: self.ca_types[actor] for actor in active if self.ca_types[actor]}
        counts = {
            "events": self.events,
            "agent_events": self.agent_events,
            "C": self.changes,
            "CA": self.agent_changes,
            "object_bearing_C": self.object_changes,
            "multi_author_C": self.multi_changes,
            "objectless_multi_author_C": self.objectless_multi_changes,
            "authors_pairs_scope": "all events after seed-author removal",
            "agents": len(self.agent_counts),
            "active_agents": len(active),
            "active_agents_without_CA": len(active) - len(distributions),
            "acting_denominator": self.agent_denominator,
            "lookback_samples": len(self.lookbacks),
            "loop_agents": len(active),
        }

        def ratio(name, numerator, denominator, scale=1.0, reason="empty denominator"):
            if denominator:
                values[name] = numerator / denominator * scale
            else:
                values[name] = None
                unavailable[name] = reason

        if self.classes == {"change"}:
            values["acting_share"] = None
            unavailable["acting_share"] = "record logs only changes"
        else:
            ratio("acting_share", self.agent_changes, self.agent_denominator)
        if active:
            values["loop_rate"] = mean(self.agent_loops[a] / self.agent_counts[a] for a in active)
        else:
            values["loop_rate"] = None
            unavailable["loop_rate"] = "no agent has 20 chosen events in this interval"
        ratio(
            "cross_author_share",
            self.multi_changes,
            self.object_changes,
            reason="no object-bearing applied changes",
        )
        values["distinct_author_pairs"] = len(self.authors_pairs) if self.changes else None
        if not self.changes:
            unavailable["distinct_author_pairs"] = "record has no applied changes"
        for name, fraction in (("causal_lookback_median", 0.5), ("causal_lookback_p90", 0.9)):
            if self.lookbacks:
                values[name] = quantile(self.lookbacks, fraction)
            else:
                values[name] = None
                unavailable[name] = "no eligible non-seed applied changes after t0"
        type_count = len(set().union(*(row.keys() for row in distributions.values())))
        pair_count = len(distributions) * (len(distributions) - 1) // 2
        counts["divergence_pairs"] = pair_count
        counts["divergence_types"] = type_count
        if pair_count and type_count >= 2:
            values["role_divergence"] = mean(
                jsd(distributions[a], distributions[b])
                for a, b in itertools.combinations(distributions, 2)
            )
        else:
            values["role_divergence"] = None
            unavailable["role_divergence"] = (
                "fewer than two active agents with CA or fewer than two CA types"
            )
        shares = []
        for actor, allowed in roles.items():
            types = self.ca_types.get(str(actor), {})
            if types:
                shares.append(
                    sum(count for kind, count in types.items() if kind in allowed)
                    / sum(types.values())
                )
        counts["on_role_agents"] = len(shares)
        counts["assigned_agents_without_CA"] = len(roles) - len(shares)
        values["on_role_share"] = mean(shares) if shares else None
        if not shares:
            unavailable["on_role_share"] = "no assigned agent has an applied chosen change"
        persistence = []
        if previous:
            for actor in active:
                if (
                    previous.agent_counts[actor] >= MIN_ACTIONS
                    and self.ca_types[actor]
                    and previous.ca_types[actor]
                ):
                    persistence.append(1 - jsd(self.ca_types[actor], previous.ca_types[actor]))
        values["persistence"] = mean(persistence) if persistence else None
        counts["persistence_agent_pairs"] = len(persistence)
        if not persistence:
            unavailable["persistence"] = "no eligible active agent in this and the preceding period"
        if supports_gap:
            ratio("gap_rate", self.gaps, self.agent_events, 100)
        else:
            values["gap_rate"] = None
            unavailable["gap_rate"] = "gap rate is defined only for Ambergris"
        counts["not_modeled_agent_events"] = self.gaps
        return {"measures": values, "counts": counts, "not_available": unavailable}, persistence


def compute(info, events):
    t0, period_s = float(info["t0"]), float(info["period_s"])
    if not math.isfinite(period_s) or period_s <= 0:
        raise ValueError("period_s must be finite and positive")
    seeds = {str(actor) for actor in info.get("seed_actors", [])}
    classes = info.get("classes", {})
    roles = {str(actor): set(types) for actor, types in info.get("roles", {}).items()}
    objects, kinds, histories = {}, {}, defaultdict(lambda: deque(maxlen=16))
    whole, periods = Interval(), {}
    unknown = Counter()
    last_time, first_time = -math.inf, None
    inferred = Counter()
    for original in events:
        event = dict(original)
        for required in ("t", "actor", "type", "object"):
            if required not in event:
                raise ValueError(f"Missing minimal field {required}")
        event["t"] = float(event["t"])
        event["actor"] = str(event["actor"])
        if not math.isfinite(event["t"]) or event["t"] < last_time:
            raise ValueError("Adapter events must be finite and in time order")
        last_time = event["t"]
        if first_time is None:
            first_time = last_time
        handle = event["object"]
        if handle is not None and handle not in objects:
            objects[handle] = {
                "maker": str(event.get("maker", event["actor"])),
                "created": float(event.get("created", event["t"])),
                "kind": event.get("kind", handle),
            }
            for field in ("maker", "created", "kind"):
                inferred[field] += field not in event
        metadata = objects.get(handle)
        event["seed_object"] = bool(metadata and metadata["maker"] in seeds)
        author_set = event.get("authors")
        if author_set is None:
            author_set = [event["actor"]]
            if metadata:
                author_set.append(metadata["maker"])
        event["authors"] = {str(author) for author in author_set} - seeds
        event["oldest"] = float(
            event.get("oldest", metadata["created"] if metadata else event["t"])
        )
        event["class"] = event.get("class", classes.get(event["type"], "change"))
        if event["type"] not in classes and "class" not in original:
            label = str(event["type"])
            if len(label) > 128:
                label = "type_sha256:" + hashlib.sha256(label.encode()).hexdigest()
            unknown[label] += 1
        period = math.floor((event["t"] - t0) / period_s)
        interval = periods.setdefault(period, Interval())
        looping = False
        if event.get("agent", True):
            history = histories[event["actor"]]
            history.append((event["type"], handle, event.get("result")))
            recent = list(history)
            looping = any(
                len(recent) >= 2 * width and recent[-width:] == recent[-2 * width : -width]
                for width in range(1, 9)
            )
        whole.add(event, looping, t0)
        interval.add(event, looping, t0)
        if metadata and not event["seed_object"]:
            kind = metadata["kind"]
            if kind not in kinds:
                kinds[kind] = {
                    "created": metadata["created"],
                    "maker": metadata["maker"],
                    "uses": {},
                }
            novelty = kinds[kind]
            if metadata["created"] < novelty["created"]:
                novelty.update({"created": metadata["created"], "maker": metadata["maker"]})
            if event["class"] == "change" and event.get("result") in (None, "applied"):
                actor = str(event.get("root_actor", event["actor"]))
                novelty["uses"].setdefault(actor, event["t"])
    if periods:
        final_period = max(periods)
        if info.get("end_s") is not None:
            final_period = max(final_period, math.ceil((float(info["end_s"]) - t0) / period_s) - 1)
        for period in range(min(0, min(periods)), final_period + 1):
            periods.setdefault(period, Interval())
    support_gap = info.get("supports_gap_rate", False)
    report, _ = whole.report(roles, support_gap)
    period_reports, persistence = [], []
    for period in sorted(periods):
        row, samples = periods[period].report(roles, support_gap, periods.get(period - 1))
        row.update({"period": period, "start_s": t0 + period * period_s})
        period_reports.append(row)
        persistence.extend(samples)
    report["measures"]["persistence"] = mean(persistence) if persistence else None
    report["counts"]["persistence_agent_pairs"] = len(persistence)
    if persistence:
        report["not_available"].pop("persistence", None)
    adopted = Counter()
    new_kinds = Counter()
    for kind in kinds.values():
        if kind["created"] < t0 or kind["created"] > last_time:
            continue
        period = math.floor((kind["created"] - t0) / period_s)
        new_kinds[period] += 1
        if any(
            actor != kind["maker"] and used <= kind["created"] + 2 * period_s
            for actor, used in kind["uses"].items()
        ):
            adopted[period] += 1
    for row in [report, *period_reports]:
        period = row.get("period")
        count = sum(adopted.values()) if period is None else adopted[period]
        available = whole.changes > 0
        row["counts"]["new_non_seed_kinds"] = (
            sum(new_kinds.values()) if period is None else new_kinds[period]
        )
        row["counts"]["adopted_non_seed_kinds"] = count
        row["measures"]["adopted_novelty"] = count if available else None
        active = row["counts"]["active_agents"]
        row["measures"]["adopted_novelty_per_100_active"] = (
            count / active * 100 if available and active else None
        )
        if not available:
            row["not_available"]["adopted_novelty"] = "record has no applied changes"
        if not available or not active:
            row["not_available"]["adopted_novelty_per_100_active"] = (
                "no applied changes or no active agents"
            )
    report.update(
        {
            "definitions_version": VERSION,
            "info": info,
            "time_range": {
                "first": first_time,
                "last": last_time if first_time is not None else None,
            },
            "diagnostics": {
                "inferred_object_metadata": dict(inferred),
                "default_change_types": dict(unknown),
                "cross_author_numerator_scope": "all C",
                "cross_author_denominator_scope": "object-bearing C",
                "author_pairs_scope": "all events after seed-author removal",
            },
            "periods": period_reports,
        }
    )
    return report
