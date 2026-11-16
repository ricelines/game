"""Process composition for the independent gateway adapters."""

import argparse
import asyncio
import json
from pathlib import Path

from aiohttp import web

from bake.pool import BakePool
from bake.sandbox import SandboxUnavailable
from gateway.authoring import Authoring
from gateway.credentials import read_key_file
from gateway.http import create_app
from gateway.session import Sessions


def write_resident_report(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    staging = path.with_suffix(path.suffix + ".tmp")
    staging.write_text(json.dumps(data, indent=2))
    staging.replace(path)


def parser():
    root = argparse.ArgumentParser(description="Ambergris MCP, CLI, authoring and resident gateway")
    root.add_argument("--bind", default="127.0.0.1")
    root.add_argument("--port", type=int, default=8081)
    root.add_argument("--world", default="ws://127.0.0.1:8080/ws")
    root.add_argument("--assets", type=Path, default=Path("assets"))
    root.add_argument(
        "--origin", action="append", default=["http://127.0.0.1:8080", "http://localhost:8080"]
    )
    purpose = root.add_mutually_exclusive_group()
    purpose.add_argument("--purpose-token")
    purpose.add_argument("--purpose-token-file", type=Path)
    root.add_argument("--bake", action="store_true")
    root.add_argument(
        "--restricted-host-bake",
        action="store_true",
        help=(
            "Explicitly select namespaces+Landlock+seccomp+rlimits without cgroup; reported "
            "as restricted host"
        ),
    )
    root.add_argument("--cgroup-root", type=Path)
    root.add_argument(
        "--residents", type=Path, help="Content-authored JSON resident definitions; never .env"
    )
    root.add_argument("--model", help="Override resident model IDs explicitly")
    root.add_argument(
        "--resident-report",
        type=Path,
        help="Checkpoint actual model/tool transcripts and token/latency evidence after each call",
    )
    root.add_argument("--authoring-reference", type=Path, default=Path("content/authoring.md"))
    return root


async def application(options):
    def report(data):
        write_resident_report(options.resident_report, data)

    pool = None
    unavailable = "Bake disabled; launch --bake to enable the sandbox service"
    if options.bake:
        try:
            pool = await asyncio.to_thread(
                BakePool,
                options.assets,
                require_cgroup=not options.restricted_host_bake,
                cgroup_root=options.cgroup_root,
            )
        except SandboxUnavailable as error:
            unavailable = "Sandbox unavailable: " + str(error)
            print(unavailable, flush=True)
    sessions = Sessions(options.world)
    authoring = Authoring(pool, unavailable)
    residents = None
    if options.residents:
        from harness.resident import Resident, Residents

        definitions = json.loads(options.residents.read_text())
        rows = [
            Resident(
                identity=row["id"],
                name=row["name"],
                key=read_key_file(row["key_file"]),
                purpose=row["purpose"],
                persona=row.get("persona", ""),
                model=options.model or row.get("model", "gpt-5.4-mini"),
                interval_s=float(row.get("interval_s", 8)),
            )
            for row in definitions["agents"]
        ]
        reference = (
            options.authoring_reference.read_text()
            if options.authoring_reference.exists()
            else (
                "Authoring reference unavailable. Use existing actions and report missing "
                "documentation rather than inventing a host API."
            )
        )
        residents = Residents(
            rows,
            sessions,
            authoring,
            reference=reference,
            report_sink=report if options.resident_report else None,
        )
        residents.start()

    return create_app(
        sessions,
        authoring,
        residents,
        allowed_origins=options.origin,
        purpose_token=(
            read_key_file(options.purpose_token_file)
            if options.purpose_token_file
            else options.purpose_token
        ),
        resident_report=report if options.resident_report else None,
    )


def main():
    options = parser().parse_args()
    web.run_app(application(options), host=options.bind, port=options.port, access_log=None)


if __name__ == "__main__":
    main()
