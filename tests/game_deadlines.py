"""Real native offer/grant deadlines and authored-build navigation notifications.

Starts an owned fresh world, uses normal authenticated inputs, and retains every
reply, server log, executable/plan hash and final state on success or failure.
Authentication material is consumed privately by the shared Player helper.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import pathlib
import time

from game_elaboration import Evidence, Player, require
from game_restart_live import Native

ESCROW = 128


def identity(row):
    return {key: row[key] for key in ("h", "model", "maker", "created_s", "owner", "parent")}


def baked(row):
    return {
        key: row["measured"][key]
        for key in ("asset", "volume_m3", "capacity_m3", "size_m", "params", "material")
    }


async def notice(player, kind, **matching):
    until = time.perf_counter() + 20
    while time.perf_counter() < until:
        for event in player.events:
            if event.get("kind") == kind and all(event.get(k) == v for k, v in matching.items()):
                return event
        await asyncio.sleep(0.025)
    raise AssertionError(f"actual {kind} notification did not arrive")


async def deadline_records(architect, evidence, offer, grant):
    wanted = {
        "offer_expired": ("offer", offer),
        "grant_expiring": ("grant", grant),
        "grant_expired": ("grant", grant),
    }
    until = time.perf_counter() + 3
    selected = {}
    while time.perf_counter() < until:
        history = await architect.request("events", detail=True, limit=1000)
        for row in history["events"]:
            name = row.get("name")
            if name in wanted:
                key, value = wanted[name]
                if row.get(key) == value:
                    selected.setdefault(name, {})[row["id"]] = row
        if all(name in selected for name in wanted):
            break
        await asyncio.sleep(0.05)
    require(set(selected) == set(wanted), "completed S7 deadline records are missing")
    for name, rows in selected.items():
        require(len(rows) == 1, f"deadline {name} was recorded more than once")
        row = next(iter(rows.values()))
        require(
            row["kind"] == "system" and isinstance(row["id"], int) and row.get("agent") is False,
            "deadline notices must preserve real S7 system kind/numeric ID and stay nonchosen",
        )
    evidence.checks["s7"] = {name: next(iter(rows.values())) for name, rows in selected.items()}


async def deadlines(args, evidence, mira, sol, architect):
    observed = await mira.request("look", full=True, limit=1000)
    chairs = [
        row["h"]
        for row in observed.get("things", []) + observed.get("far", [])
        if row.get("says", {}).get("name") == "Café chair"
    ]
    item = None
    for handle in chairs:
        candidate = await mira.request("inspect", target=handle)
        if candidate["owner"] == mira.welcome["player"]:
            item = candidate
            break
    require(item is not None, "fresh live perception needs an owned ordinary sale item")
    handle = item["h"]
    offered = await mira.act("engine.offer", handle, price=40, expires_s=2)
    require(offered.get("ok") and offered.get("effect") == "applied", "real offer failed")
    offer = offered["offer"]
    rows = await mira.act("engine.offers", handle)
    quote = next(row for row in rows["offers"] if row["id"] == offer)
    before = await mira.request("inspect", target=handle)
    require(before["flags"] & ESCROW, "actual offered item did not enter escrow")
    require(
        (await mira.request("look", limit=1))["step"] < quote["expires_step"],
        "live pre-deadline check must precede its real due step",
    )
    created = await mira.act(
        "engine.grant",
        holder=sol.welcome["player"],
        scope={"account": mira.welcome["player"]},
        rights=["spend"],
        cap=2,
        period_s=60,
        expires_s=4,
    )
    require(created.get("ok"), "ordinary owner could not grant temporary wallet access")
    grant = created["grant"]
    due = created["details"]["expires_step"]
    paid = await sol.act("engine.pay", to="p:1", amount=1, **{"from": mira.welcome["player"]})
    require(paid.get("ok") and paid.get("amount") == 1, "actual pre-expiry Spend grant denied")
    reminder = await notice(mira, "grant_expiring", id=grant)
    require(
        (await mira.request("look", limit=1))["step"] < due,
        "actual spending warning arrived after expiry",
    )
    expired_offer = await notice(mira, "offer_expired", offer=offer)
    after = await mira.request("inspect", target=handle)
    require(not after["flags"] & ESCROW, "deadline did not clear actual escrow flags")
    require(
        identity(before) == identity(after)
        and before["position"] == after["position"]
        and baked(before) == baked(after),
        "offer expiry changed original goods identity, world pose or baked properties",
    )
    require(
        all(row["id"] != offer for row in (await mira.act("engine.offers", handle))["offers"]),
        "expired sale remains listed",
    )
    again = await mira.act("engine.offer", handle, price=41)
    require(again.get("ok"), "actual expired item remains locked against a new offer")
    require(
        (await mira.act("engine.cancel_offer", handle, offer=again["offer"])).get("ok"),
        "replacement sale could not be cancelled normally",
    )
    expired_grant = await notice(mira, "grant_expired", id=grant)
    denied = await sol.act("engine.pay", to="p:1", amount=1, **{"from": mira.welcome["player"]})
    require(denied.get("ok") is False, "expired grant still authorized real wallet spending")
    held = await sol.act("engine.list_grants")
    given = await mira.act("engine.list_grants")
    require(
        all(
            row["id"] != grant
            for reply in (held, given)
            for key in ("held", "given")
            for row in reply[key]
        ),
        "expired grant remains in current held/given indices",
    )
    await asyncio.sleep(0.15)
    await deadline_records(architect, evidence, offer, grant)
    s7 = evidence.checks["s7"]
    require(
        s7["offer_expired"]["step"] == quote["expires_step"]
        and s7["grant_expiring"]["step"] < due
        and s7["grant_expired"]["step"] == due,
        "uncontended deadline maintenance did not run at the exact scheduled frame",
    )
    require(
        sum(row.get("kind") == "offer_expired" and row.get("offer") == offer for row in mira.events)
        == 1
        and sum(
            row.get("kind") == "grant_expired" and row.get("id") == grant for row in mira.events
        )
        == 1,
        "a completed deadline sent repeated expiry notifications",
    )
    evidence.checks["offer"] = {
        "id": offer,
        "due_step": quote["expires_step"],
        "before": before,
        "after": after,
        "notice": expired_offer,
        "reoffer": again,
    }
    evidence.checks["grant"] = {
        "id": grant,
        "due_step": due,
        "before_payment": paid,
        "reminder": reminder,
        "expired": expired_grant,
        "after_payment": denied,
        "held": held,
        "given": given,
    }


def bake_platform():
    # Uses the same verified, isolated CAD pipeline as public authoring. The source
    # is new content, and never edits the live world's tables or player poses.
    from bake.pool import BakePool

    source = "from amb.shapes import box\nPARAMS={}\ndef build(p): return box(2,2,.05)\n"
    with BakePool(pathlib.Path("assets"), require_cgroup=False) as pool:
        job = pool.submit(source)
        until = time.perf_counter() + 90
        while time.perf_counter() < until:
            result = pool.status(job)
            if result["status"] in {"done", "failed"}:
                require(result["status"] == "done", f"actual platform CAD failed: {result}")
                return result
            time.sleep(0.05)
    raise AssertionError("actual platform bake exceeded test deadline")


async def navigation(evidence, mira):
    baked = await asyncio.to_thread(bake_platform)
    registered = await mira.request(
        "register_model",
        job=baked["job"],
        asset=baked["hash"],
        canonical_params="{}",
        material="base.wood",
        text={"name": "Detached walking platform", "description": "Authored navigation test slab."},
        manifest=baked["measured"],
    )
    require(registered.get("ok"), "actual newly baked model registration failed")
    placed = await mira.act(
        "base.place", model=registered["model"], position=[-4, -7, 3], material="base.wood"
    )
    require(
        placed.get("ok") and placed.get("effect") == "applied" and placed.get("created"),
        "ordinary approved construction law did not create real elevated geometry",
    )
    created = set(placed["created"])
    until = time.perf_counter() + 20
    diagnostic = None
    while time.perf_counter() < until:
        diagnostic = next(
            (
                event
                for event in mira.events
                if event.get("kind") == "navigation" and created.intersection(event["objects"])
            ),
            None,
        )
        if diagnostic:
            break
        await asyncio.sleep(0.05)
    require(diagnostic is not None, "actual author received no completed navigation report")
    require(
        diagnostic["complete"]
        and diagnostic["status"] == "unreachable"
        and diagnostic["reference_navigable"]
        and any(not area["reachable"] for area in diagnostic["areas"]),
        "raised independent platform must be diagnosed from actual disconnected Detour polygons",
    )
    inspected = [await mira.request("inspect", target=handle) for handle in placed["created"]]
    require(
        all(
            row["owner"] == mira.welcome["player"] and row["position"][2] == 3 for row in inspected
        ),
        "navigation report must describe the real construction result",
    )
    evidence.checks["navigation"] = {
        "bake": baked,
        "registration": registered,
        "placed": placed,
        "inspection": inspected,
        "notice": diagnostic,
    }


async def run(args, evidence):
    native = Native(args, evidence.directory / "world", evidence.directory, "native")
    try:
        await native.start()
        url = f"ws://127.0.0.1:{args.port}/ws"
        async with (
            Player("Mira", url, args.mira_key_file, evidence) as mira,
            Player("Sol", url, args.sol_key_file, evidence) as sol,
            Player("Architect", url, args.architect_key_file, evidence) as architect,
        ):
            await deadlines(args, evidence, mira, sol, architect)
            if not args.skip_navigation:
                await navigation(evidence, mira)
    finally:
        if native.process:
            evidence.checks["native_shutdown"] = await native.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--server", type=pathlib.Path, default=pathlib.Path(".build/release/amb-server")
    )
    parser.add_argument(
        "--plan", type=pathlib.Path, default=pathlib.Path(".cache/content/seeded-plan.json")
    )
    parser.add_argument("--mira-key-file", default=".runtime/keys/mira.key")
    parser.add_argument("--sol-key-file", default=".runtime/keys/sol.key")
    parser.add_argument("--architect-key-file", default=".runtime/keys/architect.key")
    parser.add_argument("--port", type=int, default=18081)
    parser.add_argument("--skip-navigation", action="store_true")
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=pathlib.Path(".cache/integration/deadlines-report.json"),
    )
    args = parser.parse_args()
    artifacts = args.output.parent / f"deadlines-artifacts-{time.time_ns()}"
    evidence = Evidence(artifacts)
    report = {"passed": False, "artifacts": str(artifacts)}
    try:
        asyncio.run(run(args, evidence))
        report["passed"] = True
    except Exception as problem:
        report["problem"] = str(problem)
    report["checks"] = evidence.checks
    report["elapsed_s"] = round(time.perf_counter() - evidence.started, 3)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    (artifacts / "transcript.json").write_text(json.dumps(evidence.events, indent=2) + "\n")
    print(json.dumps({key: value for key, value in report.items() if key != "checks"}))
    if not report["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
