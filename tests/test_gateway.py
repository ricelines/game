"""Adapter tests use a labelled S6 test transport, never a fallback game policy."""

import asyncio
import json
import subprocess
import sys
from copy import deepcopy
from types import SimpleNamespace

import pytest
from aiohttp.test_utils import TestClient, TestServer
from websockets.asyncio.server import serve

from gateway import mcp
from gateway.authoring import Authoring
from gateway.credentials import read_key_file
from gateway.http import create_app
from gateway.schema import TOOLS
from gateway.session import Session, Sessions
from harness.resident import Resident, Residents


def test_resident_credentials_are_private_files_and_absent_from_public_output(tmp_path):
    path = tmp_path / "fixture.key"
    path.write_text("labelled-test-credential\n")
    path.chmod(0o644)
    with pytest.raises(ValueError, match="0600"):
        read_key_file(path)
    path.chmod(0o600)
    key = read_key_file(path)
    row = Resident("p:1", "fixture", key)
    assert "labelled-test-credential" not in json.dumps(row.public())
    assert "labelled-test-credential" not in repr(row)


def rpc(name="tools/list", arguments=None):
    params = {
        "_meta": {
            mcp.META_PREFIX + "protocolVersion": mcp.VERSION,
            mcp.META_PREFIX + "clientInfo": {"name": "fixture", "version": "1"},
            mcp.META_PREFIX + "clientCapabilities": {},
        }
    }
    headers = {
        "MCP-Protocol-Version": mcp.VERSION,
        "Mcp-Method": name,
        "Accept": "application/json, text/event-stream",
        "Authorization": "Bearer fixture",
    }
    if name == "tools/call":
        params.update({"name": "look", "arguments": arguments or {}})
        headers["Mcp-Name"] = "look"
    return {"jsonrpc": "2.0", "id": 7, "method": name, "params": params}, headers


def test_current_mcp_revision_and_header_body_validation():
    body, headers = rpc("tools/call")
    mcp.validate(headers, body)
    headers["Mcp-Name"] = "inspect"
    with pytest.raises(mcp.RpcError) as failure:
        mcp.validate(headers, body)
    assert failure.value.code == -32020
    headers["Mcp-Name"] = "=?base64?bG9vaw==?="
    mcp.validate(headers, body)
    headers["MCP-Protocol-Version"] = "1900-01-01"
    body["params"]["_meta"][mcp.META_PREFIX + "protocolVersion"] = "1900-01-01"
    with pytest.raises(mcp.RpcError) as failure:
        mcp.validate(headers, body)
    assert failure.value.code == -32022
    assert failure.value.data["supported"] == [mcp.VERSION]
    assert [row["name"] for row in TOOLS] == ["look", "inspect", "actions", "do", "cancel"]
    body, headers = rpc()
    body["params"]["_meta"] = []
    with pytest.raises(mcp.RpcError) as failure:
        mcp.validate(headers, body)
    assert failure.value.code == -32602


def test_real_websocket_gateway_compacts_full_native_observations():
    async def run():
        async def handler(socket):
            async for raw in socket:
                message = json.loads(raw)
                if message["t"] == "hello":
                    assert message["binary"] is False and message["mode"] == "play"
                    await socket.send(
                        json.dumps({"t": "welcome", "body": "o:1.1", "player": "p:1"})
                    )
                    await socket.send(json.dumps({"t": "event", "kind": "changed", "sequence": 1}))
                elif message["t"] == "look":
                    row = {"h": "o:2.1", "dist_m": 1, "bearing": 5, "measured": {"volume": 1}}
                    await socket.send(
                        json.dumps(
                            {
                                "t": "obs",
                                "id": message["id"],
                                "step": 1,
                                "things": [row],
                                "body_actions": [],
                                "player_actions": [],
                            }
                        )
                    )
                else:
                    await socket.send(
                        json.dumps(
                            {
                                "t": "result",
                                "id": message["id"],
                                "ok": True,
                                "effect": "none",
                                "reason": "fixture",
                            }
                        )
                    )

        async with serve(handler, "127.0.0.1", 0) as server:
            port = server.sockets[0].getsockname()[1]
            session = await Session(f"ws://127.0.0.1:{port}", "fixture", "fixture").open()
            first, result = await asyncio.gather(
                session.tool("look", {}), session.tool("do", {"action": "fixture"})
            )
            assert first["things"][0]["measured"] == {"volume": 1}
            assert result["ok"] is True
            assert len(first["events"]) + len(result["events"]) == 1
            second = await session.tool("look", {})
            assert second["things"] == [{"h": "o:2.1", "dist_m": 1, "bearing": 5}]
            session.forget_observation()
            fresh = await session.tool("look", {})
            assert fresh["things"][0]["measured"] == {"volume": 1}
            assert fresh["body_actions"] == []
            assert "body_actions" not in second
            with pytest.raises(ValueError, match="Unknown argument: full"):
                await session.tool("look", {"full": True})
            session.events.extend({"sequence": n} for n in range(50))
            newest = await session.tool("inspect", {"target": "self", "events_limit": 45})
            assert newest["events_omitted"] == 5
            assert newest["events"][0]["sequence"] == 5
            await session.close()

    asyncio.run(run())


def test_gateway_cache_tracks_descriptions_across_motion_visibility_and_reconnect():
    async def run():
        session = Session("ws://unused", "fixture", "fixture")
        observation = {
            "things": [
                {
                    "h": "o:2.1",
                    "dist_m": 1,
                    "bearing": 5,
                    "lit": True,
                    "measured": {"mass_kg": 2},
                    "says": {"name": "Cup"},
                }
            ],
            "far": [],
            "body_actions": ["move_to(point: vector)"],
            "player_actions": [],
        }
        first = session.compact_observation(deepcopy(observation))
        assert first["things"][0]["says"]["name"] == "Cup"
        observation["things"][0].update(dist_m=3, bearing=90)
        moved = session.compact_observation(deepcopy(observation))
        assert moved["things"] == [{"h": "o:2.1", "dist_m": 3, "bearing": 90}]
        assert "body_actions" not in moved and "player_actions" not in moved
        away = deepcopy(observation)
        away["things"] = []
        session.compact_observation(away)
        assert len(session.compact_observation(deepcopy(observation))["things"][0]) == 3
        observation["things"][0]["lit"] = False
        assert "measured" in session.compact_observation(deepcopy(observation))["things"][0]
        observation["things"][0]["says"]["name"] = "Renamed cup"
        assert session.compact_observation(deepcopy(observation))["things"][0]["says"] == {
            "name": "Renamed cup"
        }
        observation["body_actions"].append("sit(target: handle)")
        changed = session.compact_observation(deepcopy(observation))
        assert len(changed["body_actions"]) == 2 and "player_actions" not in changed
        session.forget_observation()
        fresh = session.compact_observation(deepcopy(observation))
        assert fresh["things"][0]["says"]["name"] == "Renamed cup"
        assert "body_actions" in fresh and "player_actions" in fresh
        session._welcomed.cancel()

    asyncio.run(run())


def test_native_hearing_survives_gateway_event_merge():
    async def run():
        session = Session("ws://unused", "fixture", "fixture")
        session.events.append({"kind": "changed", "sequence": 2})
        result = session.attach_events(
            {"events": [{"kind": "sound", "data": "hello"}], "events_omitted": 3}, 2
        )
        assert result["events"] == [
            {"kind": "sound", "data": "hello"},
            {"kind": "changed", "sequence": 2},
        ]
        assert result["events_omitted"] == 3
        assert not session.events
        session._welcomed.cancel()

    asyncio.run(run())


def test_external_cad_does_not_hold_world_mutation_lane():
    class Pool:
        def __init__(self):
            self.done = False

        def submit(self, source, params):
            return source

        def status(self, identity):
            if not self.done:
                return {"status": "baking"}
            return {"status": "done", "hash": identity, "measured": {"params": {"small": 1e-8}}}

        def close(self):
            pass

    class Transport:
        def __init__(self):
            self.key, self.token = "fixture", "fixture"
            self.mutations = asyncio.Lock()
            self.order = []

        async def request(self, kind, **arguments):
            self.order.append((kind, arguments))
            return {"ok": True, "model": "m:1"}

    async def run():
        pool, session = Pool(), Transport()
        author = Authoring(pool)
        one = await author.submit_model(session, {"source": "asset-one", "text": {"name": "one"}})
        two = await author.submit_model(session, {"source": "asset-two", "text": {"name": "two"}})

        async def mutation():
            async with session.mutations:
                await session.request("do")

        pending = asyncio.create_task(mutation())
        await session.request("look")
        await asyncio.wait_for(pending, 2)
        assert [kind for kind, _ in session.order] == ["look", "do"]
        pool.done = True
        await asyncio.gather(*list(author.tasks))
        assert [kind for kind, _ in session.order] == [
            "look",
            "do",
            "register_model",
            "register_model",
        ]
        assert session.order[2][1]["canonical_params"] == '{"small":1e-08}'
        assert author.status(session, one["job"])["status"] == "done"
        assert author.status(session, two["job"])["status"] == "done"
        await author.close()

    asyncio.run(run())


def test_http_mcp_shape_origins_auth_and_missing_services():
    async def run():
        sessions = Sessions("ws://127.0.0.1:1")
        authoring = Authoring()
        client = TestClient(
            TestServer(
                create_app(
                    sessions, authoring, allowed_origins=["http://fixture"], purpose_token="watch"
                )
            )
        )
        await client.start_server()
        body, headers = rpc()
        async with client.post("/mcp", json=body, headers=headers) as reply:
            result = await reply.json()
            assert reply.status == 200
            assert len(result["result"]["tools"]) == 5
            assert result["result"]["resultType"] == "complete"
            assert "Mcp-Session-Id" not in reply.headers
        async with client.get("/mcp") as reply:
            assert reply.status == 405
        async with client.post(
            "/mcp", json=body, headers={**headers, "Origin": "http://evil"}
        ) as reply:
            assert reply.status == 403
        async with client.post("/mcp", json=body) as reply:
            assert reply.status == 400
        async with client.post(
            "/api/purpose",
            json={"agent": "p:1", "purpose": "arbitrary"},
            headers={"Authorization": "Bearer watch"},
        ) as reply:
            assert reply.status == 503
            assert (await reply.json())["error"] == "model_service_unavailable"
        await client.close()

    asyncio.run(run())


def test_actual_model_adapter_never_executes_a_fallback_on_api_failure():
    class BrokenAPI:
        async def create(self, **_arguments):
            raise RuntimeError("labelled API failure")

    class Tools:
        def __init__(self):
            self.welcome = {"body": "o:1.1"}
            self.calls = []

        def forget_observation(self):
            pass

        async def tool(self, name, arguments):
            self.calls.append(name)
            return {"things": [], "events": []}

    class SessionRows:
        async def get(self, *_args):
            return session

    async def run():
        row = Resident(
            "p:1", "fixture", "test-credential-not-for-reports", purpose="An arbitrary purpose"
        )
        client = SimpleNamespace(responses=BrokenAPI())
        service = Residents([row], SessionRows(), Authoring(), client=client)
        report = service.report()
        assert report["agents"][0]["purpose"] == row.purpose
        assert report["responses"] == []
        assert row.key not in json.dumps(report)
        with pytest.raises(RuntimeError, match="labelled API failure"):
            await service.turn(row)
        assert session.calls == ["look"]

    session = Tools()
    asyncio.run(run())


def test_purpose_update_checkpoints_actual_resident_context_without_a_model_call():
    class SessionRows:
        async def get(self, key, token):
            assert key == "private-fixture" and token == "resident-p:6"
            return self

        async def tool(self, name, arguments):
            assert name == "cancel" and arguments == {}
            return {"ok": True}

    async def run():
        reports = []
        row = Resident("p:6", "Parked UI resident", "private-fixture", purpose="Initial purpose")
        service = Residents(
            [row], SessionRows(), Authoring(), client=object(), report_sink=reports.append
        )
        await service.set_purpose("p:6", "An arbitrary purpose supplied by the observer")
        assert row.revision == 1 and row.wake.is_set()
        assert reports[-1]["agents"][0]["purpose"] == row.purpose
        assert reports[-1]["responses"] == []
        assert "private-fixture" not in json.dumps(reports)

    asyncio.run(run())


def test_resident_report_checkpoints_responses_and_retains_reset_context():
    class CompletedAPI:
        async def create(self, **_arguments):
            return SimpleNamespace(id="labelled-response", usage=None, output=[])

    class Tools:
        welcome = {"body": "o:1.1"}
        calls = []

        def forget_observation(self):
            pass

        async def tool(self, name, arguments):
            self.calls.append((name, arguments))
            return {"things": [], "fixture_padding": "x" * 250001}

    class SessionRows:
        async def get(self, *_args):
            return session

    async def run():
        reports = []
        row = Resident("p:1", "fixture", "never-record-this-credential")
        service = Residents(
            [row],
            SessionRows(),
            Authoring(),
            client=SimpleNamespace(responses=CompletedAPI()),
            report_sink=lambda report: reports.append(json.loads(json.dumps(report))),
        )
        await service.turn(row)
        assert len(reports) == 3
        assert reports[0]["responses"] == []
        assert reports[0]["agents"][0]["observations"][0]["fresh_context"] is True
        assert reports[1]["responses"][0]["response"] == "labelled-response"
        assert reports[1]["agents"][0]["transcript"]
        final = reports[-1]["agents"][0]
        assert final["context_resets"] == 1 and not final["transcript"]
        assert len(final["archived_transcripts"]) == 1
        assert "fixture_padding" in final["archived_transcripts"][0][-1]["content"]
        assert row.key not in json.dumps(reports)
        await service.turn(row)
        assert session.calls == [("look", {}), ("look", {})]

    session = Tools()
    asyncio.run(run())


def test_resumed_transcript_gets_full_observation_and_concurrent_budget_is_exact():
    class API:
        calls = 0

        async def create(self, **_arguments):
            self.calls += 1
            identity = self.calls
            await asyncio.sleep(0)
            return SimpleNamespace(id=f"labelled-budget-{identity}", usage=None, output=[])

    class SessionRows:
        welcome = {"body": "o:1.1"}

        def __init__(self):
            self.looks = []

        async def get(self, *_arguments):
            return self

        def forget_observation(self):
            pass

        async def tool(self, name, arguments):
            self.looks.append((name, arguments))
            return {"things": [], "body_actions": ["novel.call()"], "player_actions": []}

    async def run():
        api, sessions = API(), SessionRows()
        rows = [Resident(f"p:{i}", "fixture", "private-fixture") for i in range(6)]
        retained = {"role": "assistant", "content": "An actual retained fixture response"}
        for row in rows:
            row.transcript = [retained.copy()]
        residents = Residents(
            rows,
            sessions,
            Authoring(),
            client=SimpleNamespace(responses=api),
            max_responses=4,
        )
        await asyncio.gather(*(residents.turn(row) for row in rows))
        assert api.calls == residents.api_attempts == len(residents.measurements) == 4
        assert residents.budget_reached and residents.stopping
        assert sessions.looks == [("look", {})] * 6
        assert all(row.transcript[0] == retained for row in rows)
        assert all(row.observations[0]["fresh_context"] for row in rows)
        assert "private-fixture" not in json.dumps(residents.report())

    asyncio.run(run())


def test_report_checkpoint_survives_process_killed_without_cleanup(tmp_path):
    report_path = tmp_path / "actual-report-writer-fixture.json"
    program = """
import os, signal, sys
from pathlib import Path
from gateway.service import write_resident_report
from harness.resident import Resident, Residents
row=Resident('p:1','labelled fixture','never-output-fixture-key')
row.transcript=[{'type':'function_call_output','call_id':'fixture-call','output':'{}'}]
residents=Residents([row],None,None,client=object(),
    report_sink=lambda data:write_resident_report(Path(sys.argv[1]),data))
residents.measurements=[{'response':'fixture-response','agent':'p:1'}]
residents.checkpoint()
os.kill(os.getpid(),signal.SIGKILL)
"""
    process = subprocess.run([sys.executable, "-c", program, str(report_path)], check=False)
    assert process.returncode == -9
    data = json.loads(report_path.read_text())
    assert data["responses"][0]["response"] == "fixture-response"
    assert data["agents"][0]["transcript"][0]["call_id"] == "fixture-call"
    assert "never-output-fixture-key" not in report_path.read_text()
