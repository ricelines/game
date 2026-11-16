"""Provisioned starting body, real actions and persistent ordinary grant revocation.

The native executable is supplied by the caller. No seed resident or preexisting
private credential is used, and no permission is minted during authentication.
"""

from __future__ import annotations

import argparse
import asyncio
import copy
import hashlib
import json
import pathlib

from game_elaboration import Evidence, Player, require
from game_restart_live import Native


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def make_plan(args, directory):
    plan = json.loads(args.plan.read_text())
    source = args.body_source.read_text()
    next(row for row in plan["scripts"] if row["key"] == "body")["source"] = source
    for row in plan["profiles"]:
        row["key_file"] = str((directory / "keys" / f"{row['id']}.key").resolve())
    player = max(row["id"] for row in plan["profiles"]) + 1
    body = copy.deepcopy(next(row for row in plan["objects"] if row["key"] == "resident.mira"))
    body.update(key="test.new_player", owner=player, maker=player, seed=False, at=[-15, -4, 0])
    plan["objects"].append(body)
    original = next(row for row in plan["objects"] if row["model"] == "cup")
    for label, position, amount in (
        ("source", [-14.5, -3.4, 0.01], 0.0001),
        ("receiver", [-15.3, -3.4, 0.01], 0),
    ):
        item = copy.deepcopy(original)
        item.update(
            key=f"test.{label}",
            owner=player,
            maker=player,
            seed=False,
            plot=3,
            at=position,
            preset=[],
            state={"base.liquid": amount},
        )
        plan["objects"].append(item)
    key = directory / "keys" / "new-player.key"
    plan["profiles"].append(
        dict(
            id=player,
            name="Permission regression player",
            roles=[],
            modes=["play"],
            body=body["key"],
            key_file=str(key.resolve()),
            body_installs=[
                [
                    dict(
                        slot=slot,
                        scope={"owned": f"p:{player}"},
                        rights=[slot],
                        expires_s=60,
                    )
                    for slot in ("use", "take", "move")
                ]
                + [
                    dict(
                        slot=slot,
                        scope={"avatar": body["key"]},
                        rights=[slot],
                        expires_s=60,
                    )
                    for slot in ("animate", "camera")
                ]
            ],
        )
    )
    require(
        not any(row["issuer"] == player for row in plan["grants"]),
        "fresh player accidentally received a seed grant",
    )
    plan_path = directory / "plan.json"
    plan_path.write_text(json.dumps(plan))
    return player, key, plan_path


async def exercise(args, evidence, player_id, key):
    url = f"ws://127.0.0.1:{args.port}/ws"
    async with Player("new player", url, key, evidence) as player:
        require(player.welcome["player"] == f"p:{player_id}", "wrong authenticated player")
        body = player.welcome["body"]
        observation = await player.request("look", limit=1000)
        owned = []
        for thing in observation.get("things", []) + observation.get("far", []):
            if thing.get("dist_m", float("inf")) > 2.5:
                continue
            row = await player.request("inspect", target=thing["h"])
            if row.get("owner") == f"p:{player_id}" and row.get("h") != body:
                owned.append(row)
        require(len(owned) == 2, "fresh player's two actual containers were not observed")
        source = next(row for row in owned if row["values"]["base.liquid"] > 0)
        receiver = next(row for row in owned if row["h"] != source["h"])
        script = (await player.request("inspect", target=body))["scripts"][0]["script"]
        initial = await player.act("engine.list_grants")
        require(
            initial["held"] == [] and len(initial["given"]) == 2,
            "starting body did not receive exactly its two configured scope grants",
        )
        owned_scope = {"owned": f"p:{player_id}"}
        object_grant = next(row for row in initial["given"] if row["scope"] == owned_scope)
        avatar_grant = next(row for row in initial["given"] if row["scope"] == {"avatar": body})
        require(
            set(object_grant["rights"]) == {"use", "take", "move"}
            and set(avatar_grant["rights"]) == {"animate", "camera"},
            "starting choices expanded or lost authored rights",
        )
        for row in initial["given"]:
            require(
                row["holder"] == {"script": script, "on": body}
                and row["issuer"] == f"p:{player_id}"
                and row["parent"] is None
                and row["id"].startswith("g:")
                and 0 < row["expires_in_s"] <= 60,
                "starting grant lost its actual owner, holder or finite expiry",
            )
        # No engine.install call precedes these first physical actions.
        first_hold = await player.act("base.hold", source["h"])
        require(
            first_hold.get("ok")
            and not first_hold.get("script_errors")
            and (await player.request("look", limit=1))["self"]["holding"] == source["h"],
            "a configured starting body could not immediately grasp its owner's item",
        )
        amount = 0.00002
        first_pour = await player.act("base.pour", receiver["h"], amount=amount)
        require(
            first_pour.get("ok") and not first_pour.get("script_errors"),
            "a configured starting body could not immediately pour",
        )
        require(
            abs(
                (await player.request("inspect", target=source["h"]))["values"]["base.liquid"]
                - (0.0001 - amount)
            )
            < 1e-10
            and abs(
                (await player.request("inspect", target=receiver["h"]))["values"]["base.liquid"]
                - amount
            )
            < 1e-10,
            "starting body transfer failed conservation",
        )
        require(
            (await player.act("engine.revoke", grant=object_grant["id"]))["ok"],
            "owner could not revoke the ordinary starting grant",
        )
        denied_pour = await player.act("base.pour", receiver["h"], amount=amount)
        require(
            denied_pour.get("denied") or denied_pour.get("script_errors"),
            "body transfer survived revoked Move authority",
        )
        require(
            (await player.request("inspect", target=receiver["h"]))["values"]["base.liquid"]
            == amount,
            "denied transfer changed a conserved endpoint",
        )
        require((await player.act("base.release"))["ok"], "held item could not be released")

    async with Player("returning player", url, key, evidence) as player:
        require(player.welcome["body"] == body, "relogin replaced the configured body")
        relogged = await player.act("engine.list_grants")
        require(
            [row["id"] for row in relogged["given"]] == [avatar_grant["id"]],
            "relogin recreated the revoked starting grant",
        )
        denied_relogin = await player.act("base.hold", source["h"])
        require(
            denied_relogin.get("denied") or denied_relogin.get("script_errors"),
            "relogin silently restored revoked Take",
        )
        preview = await player.act("engine.install", body, script=script)
        require(
            {row["slot"] for row in preview["slots"]}
            == {"use", "take", "move", "animate", "camera"},
            "ordinary installation lost the body's declared slots",
        )
        choices = [
            dict(slot=slot, scope=owned_scope, rights=[slot], expires_s=6)
            for slot in ("use", "take", "move")
        ]
        install_request, install_reply = await player.send(
            "do", action="engine.install", target=body, args={"script": script, "choices": choices}
        )
        list_request, list_reply = await player.send("do", action="engine.list_grants", args={})
        installed = await asyncio.wait_for(install_reply, 20)
        listed = await asyncio.wait_for(list_reply, 20)
        require(
            player.arrivals.index(install_request) < player.arrivals.index(list_request),
            "pipelined mutation replies lost their actual session order",
        )
        renewed_ids = {row["grant"] for row in installed["grants"]}
        require(
            installed.get("ok")
            and len(installed["grants"]) == 3
            and len(renewed_ids) == 1
            and object_grant["id"] not in renewed_ids
            and len(listed["given"]) == 2,
            "explicit reinstall did not create one fresh shared-scope finite grant",
        )
        renewed = next(iter(renewed_ids))
        require(renewed.startswith("g:"), "installation returned a noncanonical grant identity")
        require(
            (await player.act("base.hold", source["h"])).get("ok")
            and (await player.request("look", limit=1))["self"]["holding"] == source["h"],
            "explicit renewed Take did not restore actual grasp",
        )
        require((await player.act("base.release"))["ok"], "renewed item could not be released")
        await asyncio.sleep(6.2)
        expired = await player.act("base.hold", source["h"])
        require(
            expired.get("denied") or expired.get("script_errors"),
            "body grasp survived finite Take expiry",
        )
        remaining = await player.act("engine.list_grants")
        require(
            [row["id"] for row in remaining["given"]] == [avatar_grant["id"]],
            "expired goods grant stayed active or unrelated avatar consent disappeared",
        )
        require(
            (await player.request("look", limit=1))["self"]["holding"] is None,
            "expired grasp changed the attachment",
        )
    return dict(
        player=f"p:{player_id}",
        body=body,
        script=script,
        source=source["h"],
        receiver=receiver["h"],
        initial_grants=initial["given"],
        first_hold=first_hold,
        first_pour=first_pour,
        amount=amount,
        denied_after_revoke=denied_pour,
        denied_after_relogin=denied_relogin,
        denied_after_expiry=expired,
        grant_rows_after_relogin=relogged["given"],
        renewed_grant=renewed,
        grant_rows_after_expiry=remaining["given"],
        pipelined_requests=[install_request, list_request],
    )


async def run(args):
    directory = args.output.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    player_id, key, plan = make_plan(args, directory)
    evidence = Evidence(directory)
    native_args = copy.copy(args)
    native_args.plan = plan
    active = None
    report = dict(
        status="failed",
        binary_sha256=digest(args.server),
        base_plan_sha256=digest(args.plan),
        body_source_sha256=digest(args.body_source),
        plan_sha256=digest(plan),
        journey="configured_starting_body_and_persistent_revocation",
    )
    try:
        active = Native(native_args, directory / "world", directory, "initial")
        await active.start()
        checks = await exercise(args, evidence, player_id, key)
        report["checks"] = checks
        expected_errors = sum(
            len(checks[name].get("script_errors", []))
            for name in ("denied_after_revoke", "denied_after_relogin", "denied_after_expiry")
        )
        state = await active.stop(script_errors=expected_errors)
        report["native_state"] = {
            name: state[name] for name in ("step", "script_errors", "timers", "allocations")
        }
        report["expected_permission_handler_errors"] = expected_errors
        active = None
        active = Native(native_args, directory / "world", directory, "restored")
        await active.start()
        async with Player(
            "restored player", f"ws://127.0.0.1:{args.port}/ws", key, evidence
        ) as player:
            require(
                player.welcome["body"] == checks["body"], "restore replaced the configured body"
            )
            await player.request("look", limit=1000)
            grants = await player.act("engine.list_grants")
            require(
                grants["given"]
                and [row["id"] for row in grants["given"]]
                == [checks["grant_rows_after_expiry"][0]["id"]],
                "restore recreated revoked or expired starting grants",
            )
            denied = await player.act("base.hold", checks["source"])
            require(
                denied.get("denied") or denied.get("script_errors"),
                "restore silently restored body Take authority",
            )
            report["restore"] = dict(given=grants["given"], denied=denied)
        restored_state = await active.stop(script_errors=len(denied.get("script_errors", [])))
        report["restored_native_state"] = {
            name: restored_state[name]
            for name in ("step", "script_errors", "timers", "allocations")
        }
        active = None
        report["status"] = "passed"
    except Exception as problem:
        report["problem"] = str(problem)
    finally:
        if active is not None:
            try:
                await active.stop()
            except Exception as problem:
                report["shutdown_problem"] = str(problem)
        (directory / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        (directory / "transcript.json").write_text(json.dumps(evidence.events, indent=2) + "\n")
    require(report["status"] == "passed", report.get("problem", "body permission journey failed"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--server", type=pathlib.Path, default=pathlib.Path(".build/release/amb-server")
    )
    parser.add_argument(
        "--plan", type=pathlib.Path, default=pathlib.Path(".cache/content/seeded-plan.json")
    )
    parser.add_argument(
        "--body-source", type=pathlib.Path, default=pathlib.Path("content/scripts/body.luau")
    )
    parser.add_argument("--port", type=int, default=18103)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    asyncio.run(run(parser.parse_args()))
