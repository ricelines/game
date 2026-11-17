"""Process composition for the independent gateway adapters."""

import argparse
import asyncio
from pathlib import Path

from aiohttp import web

from bake.pool import BakePool
from bake.sandbox import SandboxUnavailable
from gateway.authoring import Authoring
from gateway.http import create_app
from gateway.session import Sessions


def parser():
    root = argparse.ArgumentParser(description="Ambergris MCP, CLI and authoring gateway")
    root.add_argument("--bind", default="127.0.0.1")
    root.add_argument("--port", type=int, default=8081)
    root.add_argument("--world", default="ws://127.0.0.1:8080/ws")
    root.add_argument("--assets", type=Path, default=Path("assets"))
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
    return root


async def application(options):
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
    return create_app(sessions, authoring)


def main():
    options = parser().parse_args()
    web.run_app(application(options), host=options.bind, port=options.port, access_log=None)


if __name__ == "__main__":
    main()
