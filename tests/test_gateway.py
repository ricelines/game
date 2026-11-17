"""Adapter tests use a labelled S6 test transport, never a fallback game policy."""

import asyncio
import json
from copy import deepcopy

import pytest
from aiohttp import web
from aiohttp.test_utils import TestClient, TestServer
from websockets.asyncio.server import serve

from gateway import cli, mcp
from gateway.authoring import Authoring
from gateway.credentials import read_key_file
from gateway.http import create_app
from gateway.schema import TOOLS, validate
from gateway.session import Session, Sessions


def test_join_cli_saves_private_key_and_continues_the_joined_session(tmp_path, monkeypatch):
    monkeypatch.delenv("AMBERGRIS_JOIN_AUTH", raising=False)

    async def run():
        names = set()
        connections = 0

        async def handler(socket):
            nonlocal connections
            connections += 1
            async for raw in socket:
                message = json.loads(raw)
                if message["t"] == "join":
                    if message["name"] in names:
                        await socket.send(json.dumps({"t": "bye", "reason": "name_taken"}))
                        return
                    names.add(message["name"])
                    await socket.send(
                        json.dumps({"t": "joined", "player": "p:9", "key": "fixture-new-key"})
                    )
                    await socket.send(
                        json.dumps({"t": "welcome", "player": "p:9", "body": "o:9.1"})
                    )
                elif message["t"] == "look":
                    await socket.send(
                        json.dumps(
                            {
                                "t": "obs",
                                "id": message["id"],
                                "things": [],
                                "self": {"name": "New player"},
                            }
                        )
                    )
                elif message["t"] == "do":
                    await socket.send(
                        json.dumps(
                            {"t": "result", "id": message["id"], "ok": True, "effect": "started"}
                        )
                    )

        async with serve(handler, "127.0.0.1", 0) as server:
            sessions = Sessions(f"ws://127.0.0.1:{server.sockets[0].getsockname()[1]}")
            async with TestClient(
                TestServer(create_app(sessions, Authoring(None, "disabled")))
            ) as client:
                address = str(client.make_url("/")).rstrip("/")
                path = tmp_path / "new.key"
                joined = await cli.run(
                    cli.parser().parse_args(
                        ["--gateway", address, "join", "New player", "--key-file", str(path)]
                    )
                )
                assert joined == {"player": "p:9", "key_file": str(path)}
                assert path.stat().st_mode & 0o777 == 0o600
                assert read_key_file(path) == "fixture-new-key"
                look = await cli.run(
                    cli.parser().parse_args(["--gateway", address, "--key-file", str(path), "look"])
                )
                assert look["self"]["name"] == "New player"
                action = await cli.run(
                    cli.parser().parse_args(
                        [
                            "--gateway",
                            address,
                            "--key-file",
                            str(path),
                            "do",
                            "base.move_to",
                            "--args",
                            '{"point":[1,0,0]}',
                        ]
                    )
                )
                assert action["effect"] == "started"
                assert connections == 1
                duplicate = tmp_path / "duplicate.key"
                refused = await cli.run(
                    cli.parser().parse_args(
                        ["--gateway", address, "join", "New player", "--key-file", str(duplicate)]
                    )
                )
                assert refused == {"error": "name_taken"}
                assert not duplicate.exists()
                with pytest.raises(FileExistsError):
                    await cli.run(
                        cli.parser().parse_args(
                            ["--gateway", address, "join", "Another", "--key-file", str(path)]
                        )
                    )
                assert names == {"New player"}

    asyncio.run(run())


def test_join_password_is_sent_only_to_join(tmp_path, monkeypatch):
    async def run():
        async def join(request):
            if request.headers.get("Authorization") != "Basic cGxheWVyOmZpeHR1cmUtcGFzc3dvcmQ=":
                raise web.HTTPUnauthorized()
            return web.json_response({"player": "p:9", "key": "fixture-new-key"})

        async def look(request):
            assert request.headers["Authorization"] == "Bearer fixture-new-key"
            return web.json_response({"things": []})

        app = web.Application()
        app.router.add_post("/join", join)
        app.router.add_post("/tools/look", look)
        async with TestClient(TestServer(app)) as client:
            address = str(client.make_url("/")).rstrip("/")
            path = tmp_path / "new.key"
            arguments = ["--gateway", address, "join", "New player", "--key-file", str(path)]
            monkeypatch.delenv("AMBERGRIS_JOIN_AUTH", raising=False)
            assert await cli.run(cli.parser().parse_args(arguments)) == {
                "error": "unauthorized",
                "status": 401,
            }
            assert not path.exists()
            monkeypatch.setenv("AMBERGRIS_JOIN_AUTH", "player:fixture-password")
            assert (await cli.run(cli.parser().parse_args(arguments)))["player"] == "p:9"
            assert await cli.run(
                cli.parser().parse_args(["--gateway", address, "--key-file", str(path), "look"])
            ) == {"things": []}

    asyncio.run(run())


def test_operator_cli_uses_private_host_key(tmp_path):
    path = tmp_path / "operator.key"
    path.write_text("fixture-operator-key\n")
    path.chmod(0o600)

    async def run():
        received = []

        async def handler(socket):
            message = json.loads(await socket.recv())
            received.append(message)
            await socket.send(json.dumps({"t": "operator", "ok": True}))

        async with serve(handler, "127.0.0.1", 0) as server:
            world = f"ws://127.0.0.1:{server.sockets[0].getsockname()[1]}"
            result = await cli.run(
                cli.parser().parse_args(
                    [
                        "operator",
                        "--key-file",
                        str(path),
                        "--world",
                        world,
                        "role",
                        "p:9",
                        "architect",
                        "on",
                    ]
                )
            )
            assert result == {"ok": True}
            assert received == [
                {
                    "t": "operator",
                    "version": 1,
                    "key": "fixture-operator-key",
                    "op": "role",
                    "player": "p:9",
                    "role": "architect",
                    "on": True,
                }
            ]

    asyncio.run(run())


def test_credentials_require_private_files(tmp_path):
    path = tmp_path / "fixture.key"
    path.write_text("labelled-test-credential\n")
    path.chmod(0o644)
    with pytest.raises(ValueError, match="0600"):
        read_key_file(path)
    path.chmod(0o600)
    assert read_key_file(path) == "labelled-test-credential"


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


def test_land_filters_use_the_existing_inspection_tool_and_cli():
    fields = {"target": "land", "owner": "self", "for_sale": False, "cursor": 17, "limit": 5}
    validate("inspect", fields)
    options = cli.parser().parse_args(
        ["inspect", "land", "--owner", "self", "--no-for-sale", "--cursor", "17", "--limit", "5"]
    )
    assert {name: getattr(options, name) for name in fields} == fields
    for name, value in (("for_sale", 1), ("cursor", -1), ("limit", 0)):
        with pytest.raises(ValueError, match=name):
            validate("inspect", {**fields, name: value})

    async def run():
        session = Session("ws://unused", "fixture", "fixture")
        forwarded = []

        async def request(kind, **arguments):
            forwarded.append((kind, arguments))
            return {"land": [], "more": False, "next_cursor": None}

        session.request = request
        result = await session.tool("inspect", fields)
        assert result["land"] == [] and result["next_cursor"] is None
        assert forwarded == [("inspect", {**fields, "events_limit": 40})]
        session._welcomed.cancel()

    asyncio.run(run())


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
            "action_spaces": [{"target": "land", "description": "Land and its tax"}],
        }
        first = session.compact_observation(deepcopy(observation))
        assert first["things"][0]["says"]["name"] == "Cup"
        observation["things"][0].update(dist_m=3, bearing=90)
        moved = session.compact_observation(deepcopy(observation))
        assert moved["things"] == [{"h": "o:2.1", "dist_m": 3, "bearing": 90}]
        assert "body_actions" not in moved and "player_actions" not in moved
        assert "action_spaces" not in moved
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
        assert fresh["action_spaces"] == observation["action_spaces"]
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
        client = TestClient(TestServer(create_app(sessions, authoring)))
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
            "/mcp", json=body, headers={**headers, "Origin": "http://localhost:8080"}
        ) as reply:
            assert reply.status == 403
        async with client.post("/mcp", json=body) as reply:
            assert reply.status == 400
        async with client.get("/health") as reply:
            assert reply.status == 200
            assert (await reply.json())["bake"] == "unavailable"
        await client.close()

    asyncio.run(run())
