"""Park an actual resident for manual purpose UI validation, without calling a model."""

import argparse
import json
from pathlib import Path

from aiohttp import web

from gateway.authoring import Authoring
from gateway.credentials import read_key_file
from gateway.http import create_app
from gateway.service import write_resident_report
from gateway.session import Sessions
from harness.resident import Resident, Residents


class ParkedModel:
    # No turn loop is started. An accidental model invocation has no substitute policy.
    async def close(self):
        pass


async def application(options):
    sessions = Sessions(options.world)
    key = read_key_file(options.resident_key)
    row = Resident(
        "p:6", "UI check resident", key, purpose="Manual browser check; model turns are paused."
    )
    session = await sessions.get(key, "resident-" + row.identity)
    row.body = session.welcome["body"]
    row.status = "paused for manual UI check"

    def checkpoint(report):
        write_resident_report(
            options.report,
            {**report, "kind": "Manual actual purpose UI path; no model turn scheduled"},
        )

    authoring = Authoring()
    residents = Residents([row], sessions, authoring, client=ParkedModel(), report_sink=checkpoint)
    residents.checkpoint()
    return create_app(
        sessions,
        authoring,
        residents,
        allowed_origins=[options.origin],
        purpose_token=read_key_file(options.watch_key),
        resident_report=checkpoint,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--world", default="ws://127.0.0.1:18080/ws")
    parser.add_argument("--port", type=int, default=18881)
    parser.add_argument("--origin", default="http://127.0.0.1:18080")
    parser.add_argument("--resident-key", type=Path, default=Path(".runtime/keys/rue.key"))
    parser.add_argument("--watch-key", type=Path, default=Path(".runtime/keys/spectator.key"))
    parser.add_argument(
        "--report", type=Path, default=Path(".cache/client/ui-purpose-resident.json")
    )
    options = parser.parse_args()
    print(
        json.dumps(
            {
                "kind": "manual UI gateway; actual Resident context and native cancel",
                "port": options.port,
            }
        ),
        flush=True,
    )
    web.run_app(application(options), host="127.0.0.1", port=options.port, access_log=None)


if __name__ == "__main__":
    main()
