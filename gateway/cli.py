"""amb CLI: the same JSON operations as the agent gateway, without SDK wrappers."""

import argparse
import asyncio
import json
import os
import sys
from pathlib import Path

from aiohttp import ClientSession

from gateway.credentials import read_key_file


def parser():
    root = argparse.ArgumentParser(prog="amb")
    root.add_argument(
        "--gateway", default=os.environ.get("AMBERGRIS_GATEWAY", "http://127.0.0.1:8081")
    )
    credential = root.add_mutually_exclusive_group()
    credential.add_argument("--key", default=os.environ.get("AMBERGRIS_KEY"))
    credential.add_argument("--key-file", type=Path, default=os.environ.get("AMBERGRIS_KEY_FILE"))
    root.add_argument("--session", default=os.environ.get("AMBERGRIS_SESSION", "cli"))
    commands = root.add_subparsers(dest="command", required=True)
    look = commands.add_parser("look")
    look.add_argument("--limit", type=int)
    inspect = commands.add_parser("inspect")
    inspect.add_argument("target")
    actions = commands.add_parser("actions")
    actions.add_argument("--target")
    actions.add_argument("--name")
    actions.add_argument("--page", type=int)
    action = commands.add_parser("do")
    action.add_argument("action")
    action.add_argument("--target")
    action.add_argument("--args", type=json.loads, default={})
    commands.add_parser("cancel")
    for name in ("look", "inspect", "actions", "do", "cancel"):
        commands.choices[name].add_argument("--events-limit", type=int)
    submit = commands.add_parser("submit")
    submit.add_argument("kind", choices=("model", "script"))
    submit.add_argument("file", type=Path)
    submit.add_argument("--params", type=json.loads, default={})
    submit.add_argument("--material")
    submit.add_argument("--name")
    submit.add_argument("--description", default="")
    submit.add_argument("--writing")
    submit.add_argument("--wait", action="store_true")
    variant = commands.add_parser("variant", help="Bake a ready parameter block of a live model")
    variant.add_argument("model")
    variant.add_argument("--params", type=json.loads, required=True)
    variant.add_argument("--wait", action="store_true")
    edit = commands.add_parser(
        "edit-model", help="Stage a rebaked definition; apply with an action"
    )
    edit.add_argument("model")
    edit.add_argument("file", type=Path)
    edit.add_argument("--wait", action="store_true")
    job = commands.add_parser("job")
    job.add_argument("identity")
    log = commands.add_parser("log", help="Read the native S7 event record locally")
    log.add_argument("--file", type=Path, default=Path(".runtime/events.jsonl.zst"))
    log.add_argument("--since", type=int, default=0)
    log.add_argument("--actor")
    log.add_argument("--kind")
    log.add_argument("--thing")
    fixture = commands.add_parser("fixture", help="Run actual paired native rule fixtures")
    fixture.add_argument("capture", type=Path)
    fixture.add_argument("--job", type=Path, required=True)
    fixture.add_argument("--result", type=Path, required=True)
    fixture.add_argument("--catalog", type=Path)
    return root


async def run(options):
    if options.command == "fixture":
        executable = Path(__file__).resolve().parents[1] / ".build/release/amb-server"
        arguments = [
            str(executable),
            "fixture",
            str(options.capture),
            "--job",
            str(options.job),
            "--result",
            str(options.result),
        ]
        if options.catalog:
            arguments += ["--catalog", str(options.catalog)]
        process = await asyncio.create_subprocess_exec(
            *arguments, stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.PIPE
        )
        _, diagnostics = await process.communicate()
        if process.returncode:
            return {"error": "fixture_failed", "problem": diagnostics.decode(errors="replace")}
        return json.loads(options.result.read_text())
    key = read_key_file(options.key_file) if options.key_file else options.key
    if not key:
        raise ValueError("Provide --key-file or --key to authenticate your player")
    headers = {"Authorization": "Bearer " + key, "X-Ambergris-Session": options.session}
    async with ClientSession(headers=headers) as client:

        async def request(method, path, body=None):
            async with client.request(
                method, options.gateway.rstrip("/") + path, json=body
            ) as response:
                raw = await response.text()
                try:
                    return json.loads(raw)
                except json.JSONDecodeError:
                    return {"error": "http_error", "status": response.status, "problem": raw}

        if options.command in ("look", "inspect", "actions", "do", "cancel"):
            fields = ("limit", "target", "name", "page", "action", "args", "events_limit")
            arguments = {
                field: getattr(options, field)
                for field in fields
                if getattr(options, field, None) is not None
            }
            return await request("POST", "/tools/" + options.command, arguments)
        if options.command == "job":
            return await request("GET", "/bake/jobs/" + options.identity)
        if options.command == "variant":
            body = {"model": options.model, "params": options.params}
            path = "/bake/variants"
        elif options.command == "edit-model":
            body = {"model": options.model, "source": options.file.read_text()}
            path = "/bake/edits"
        else:
            body = {"source": options.file.read_text()}
            path = "/bake/" + ("models" if options.kind == "model" else "scripts")
        if options.command == "submit" and options.kind == "model":
            body.update(
                {
                    "params": options.params,
                    "text": {
                        "name": options.name or options.file.stem,
                        "description": options.description,
                    },
                }
            )
            if options.material:
                body["material"] = options.material
            if options.writing:
                body["text"]["writing"] = options.writing
        result = await request("POST", path, body)
        if options.wait and result.get("job"):
            identity = result["job"]
            while True:
                result = await request("GET", "/bake/jobs/" + identity)
                if result.get("status") not in ("queued", "baking", "registering"):
                    break
                await asyncio.sleep(0.1)
        return result


def main():
    options = parser().parse_args()
    try:
        if options.command == "log":
            from gateway.logs import records

            for row in records(
                options.file,
                since=options.since,
                actor=options.actor,
                kind=options.kind,
                thing=options.thing,
            ):
                print(json.dumps(row, ensure_ascii=False, separators=(",", ":")))
            return 0
        result = asyncio.run(run(options))
    except (OSError, ValueError, TimeoutError) as error:
        result = {"error": "client_error", "problem": str(error)}
    print(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
    return 1 if result.get("error") else 0


if __name__ == "__main__":
    sys.exit(main())
