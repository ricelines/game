# /// script
# requires-python = ">=3.11"
# dependencies = ["websockets==15.0.1"]
# ///
"""Actual socket/HTTP measurement for the isolated native transport fixture."""

from __future__ import annotations

import asyncio
import contextlib
import gzip
import json
import pathlib
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

from net_live import Replica
from websockets.asyncio.client import connect

ROOT = pathlib.Path(__file__).resolve().parents[1]


def get(port: int, path: str, **headers: str):
    request = urllib.request.Request(f"http://127.0.0.1:{port}{path}", headers=headers)
    with urllib.request.urlopen(request, timeout=5) as response:
        return response.status, response.headers, response.read()


def check_http(port: int, asset: str, content: bytes):
    status, headers, body = get(port, "/")
    assert status == 200 and body == b"<html>transport fixture</html>"
    assert headers["Cache-Control"] == "no-cache"
    status, headers, body = get(port, "/fixture.wasm", **{"Accept-Encoding": "gzip"})
    assert status == 200 and gzip.decompress(body) == content
    assert headers["Content-Type"] == "application/wasm"
    assert headers["Content-Encoding"] == "gzip"
    status, headers, body = get(port, f"/assets/{asset}/lod0.glb")
    assert status == 200 and body == b"GLB transport bytes"
    assert "immutable" in headers["Cache-Control"]
    for path in ("/../outside.txt", "/assets/../outside.txt", "/%2e%2e/outside.txt"):
        try:
            get(port, path)
        except urllib.error.HTTPError as error:
            assert error.code == 404
        else:
            raise AssertionError(f"path traversal served: {path}")


async def sockets(port: int):
    uri = f"ws://127.0.0.1:{port}/ws"
    connections = []
    receivers = []
    deltas = [0] * 500
    received_bytes = [0] * 500
    ping_sent = {}
    ping_times = []
    latencies = []
    pong_all = asyncio.Event()
    sem = asyncio.Semaphore(50)

    async def receive(index, ws):
        async for message in ws:
            if isinstance(message, bytes):
                received_bytes[index] += len(message)
                if message[0] == 2:
                    deltas[index] += 1
            else:
                row = json.loads(message)
                if row.get("t") == "pong":
                    latencies.append((time.perf_counter() - ping_sent[index]) * 1000)
                    if len(latencies) == 500:
                        pong_all.set()

    async def join(index):
        async with sem:
            started = time.perf_counter()
            ws = await connect(uri, max_size=None, compression=None, ping_interval=None)
            await ws.send(
                json.dumps(
                    {
                        "t": "hello",
                        "version": 1,
                        "key": "net-probe-fixture",
                        "mode": "watch",
                        "binary": True,
                    }
                )
            )
            welcome = json.loads(await asyncio.wait_for(ws.recv(), 5))
            assert welcome["t"] == "welcome"
            ping_times.append((time.perf_counter() - started) * 1000)
            connections.append((index, ws))
            receivers.append(asyncio.create_task(receive(index, ws)))

    try:
        started = time.perf_counter()
        await asyncio.gather(*(join(i) for i in range(500)))
        joined_s = time.perf_counter() - started
        await asyncio.sleep(1.2)
        for index, ws in connections:
            ping_sent[index] = time.perf_counter()
            await ws.send(json.dumps({"t": "ping", "id": index}))
        await asyncio.wait_for(pong_all.wait(), 5)
        await asyncio.sleep(0.4)
        assert min(deltas) >= 5, f"some actual sockets missed recurring streams: {min(deltas)}"
        assert all(not task.done() for task in receivers), "socket receiver terminated early"
        latency_sorted = sorted(latencies)
        print(
            json.dumps(
                {
                    "sockets": 500,
                    "join_total_s": round(joined_s, 3),
                    "welcome_p95_ms": round(sorted(ping_times)[475], 3),
                    "control_rtt_p50_ms": round(statistics.median(latencies), 3),
                    "control_rtt_p95_ms": round(latency_sorted[475], 3),
                    "min_deltas": min(deltas),
                    "max_deltas": max(deltas),
                    "received_bytes": sum(received_bytes),
                },
                sort_keys=True,
            )
        )
    finally:
        await asyncio.gather(*(ws.close() for _, ws in connections), return_exceptions=True)
        await asyncio.gather(*receivers, return_exceptions=True)

    async with connect(uri, max_size=None, compression=None) as ws:
        await ws.send(b"client binary is forbidden")
        await ws.wait_closed()
        assert ws.close_code == 1003
    async with connect(uri, max_size=None, compression=None) as ws:
        await ws.send(json.dumps({"t": "hello", "key": "wrong"}))
        await ws.wait_closed()
        assert ws.close_reason == "bad_key"

    # Pause the actual TCP reader before a large fixture reply, then verify the
    # native hard buffer limit ends the connection with its explicit reason.
    async with connect(uri, max_size=None, compression=None, ping_interval=None) as ws:
        await ws.send(json.dumps({"t": "hello", "key": "net-probe-fixture", "binary": False}))
        await ws.recv()
        ws.transport.pause_reading()
        await ws.send(json.dumps({"t": "flood"}))
        await asyncio.sleep(0.2)
        ws.transport.resume_reading()
        blob = await asyncio.wait_for(ws.recv(), 5)
        assert len(blob) == 12 * 1024 * 1024
        await asyncio.wait_for(ws.wait_closed(), 5)
        assert ws.close_code == 1008 and ws.close_reason == "slow_viewer"


@contextlib.asynccontextmanager
async def native(binary: pathlib.Path, web: pathlib.Path, assets: pathlib.Path, *limits: int):
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    process = subprocess.Popen(
        [str(binary), str(port), str(web), str(assets), *map(str, limits)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        ready = await asyncio.wait_for(asyncio.to_thread(process.stdout.readline), 10)
        assert ready == "ready\n", ready
        yield port
    finally:
        process.send_signal(signal.SIGTERM)
        try:
            stdout, stderr = await asyncio.wait_for(asyncio.to_thread(process.communicate), 5)
        except TimeoutError:
            process.kill()
            stdout, stderr = process.communicate()
            raise AssertionError("native transport did not stop/join cleanly") from None
        assert process.returncode == 0, (process.returncode, stdout, stderr)
        assert not stderr, stderr


async def buffering(port: int, cross_soft_limit: bool):
    uri = f"ws://127.0.0.1:{port}/ws"
    # A small receive window plus a paused reader forces the multi-MB frame to
    # leave an unsent suffix in uWS, rather than relying on a slow CI machine.
    reader = socket.socket()
    reader.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
    reader.settimeout(5)
    reader.connect(("127.0.0.1", port))
    reader.setblocking(False)
    replica = Replica()
    frames = []
    async with connect(uri, sock=reader, max_size=None, compression=None, ping_interval=None) as ws:
        await ws.send(json.dumps({"t": "hello", "key": "net-probe-fixture", "binary": True}))
        assert json.loads(await ws.recv())["t"] == "welcome"
        # A control barrier puts every initial keyframe before the test batch.
        await asyncio.sleep(0.15)
        await ws.send(json.dumps({"t": "ping", "id": "initial"}))
        while True:
            message = await asyncio.wait_for(ws.recv(), 10)
            if isinstance(message, bytes):
                replica.apply(message)
            elif json.loads(message).get("id") == "initial":
                break
        assert (0, 0) in replica.cells and (32767, 32767) in replica.cells
        ws.transport.pause_reading()
        await ws.send(json.dumps({"t": "buffer" if cross_soft_limit else "burst"}))
        if cross_soft_limit:
            await ws.send(json.dumps({"t": "view", "x": 4096}))
        await asyncio.sleep(0.35)
        await ws.send(json.dumps({"t": "ping", "id": "buffered"}))
        ws.transport.resume_reading()
        received_buffer = False
        while True:
            message = await asyncio.wait_for(ws.recv(), 15)
            if isinstance(message, bytes):
                replica.apply(message)
                frames.append((message[0], len(message)))
            elif message.startswith("b"):
                assert len(message) == 4 * 1024 * 1024
                received_buffer = True
            elif json.loads(message).get("id") == "buffered":
                break
        if cross_soft_limit:
            assert received_buffer, "uWS lost the buffered frame"
            # Recovery can be scheduled after the control barrier as the TCP
            # buffer drains. It must erase old cells and keyframe all current ones.
            deadline = time.monotonic() + 10
            while (
                (128, 0) not in replica.cells
                or (0, 0) in replica.cells
                or sum(kind == 1 for kind, _ in frames) < 26
            ):
                assert time.monotonic() < deadline, "soft-limit recovery did not finish"
                message = await asyncio.wait_for(ws.recv(), 10)
                assert isinstance(message, bytes)
                replica.apply(message)
                frames.append((message[0], len(message)))
            assert len(replica.cells) == 26
            assert sum(kind == 3 for kind, _ in frames) == 25
            assert sum(kind == 1 for kind, _ in frames) == 26, (
                "crossing SEND_SOFT must recover the stale global stream as well as new cells"
            )
        else:
            assert max(size for _, size in frames) > 3 * 1024 * 1024
            assert all(kind == 2 for kind, _ in frames), (
                "buffered-but-accepted writes below SEND_SOFT caused redundant keyframes"
            )
            assert sum(kind == 2 for kind, _ in frames) >= 2
            assert len(replica.cells[(0, 0)]) == 60000
    print(
        json.dumps(
            {
                "buffered_socket": "soft_recovery" if cross_soft_limit else "accepted",
                "frames": len(frames),
                "bytes": sum(size for _, size in frames),
            }
        )
    )


async def run(binary: pathlib.Path, backpressure_only: bool = False):
    with tempfile.TemporaryDirectory(prefix="net-http-", dir=ROOT / ".build") as temp:
        base = pathlib.Path(temp)
        web, assets = base / "web", base / "assets"
        web.mkdir()
        assets.mkdir()
        (web / "index.html").write_bytes(b"<html>transport fixture</html>")
        content = b"\x00asm transport fixture\n" * 40000
        (web / "fixture.wasm").write_bytes(content)
        (web / "fixture.wasm.gz").write_bytes(gzip.compress(content, mtime=0))
        (base / "outside.txt").write_text("must stay outside roots")
        asset = "ab" * 32
        (assets / asset).mkdir()
        (assets / asset / "lod0.glb").write_bytes(b"GLB transport bytes")
        if not backpressure_only:
            async with native(binary, web, assets) as port:
                await asyncio.to_thread(check_http, port, asset, content)
                await sockets(port)
        async with native(binary, web, assets, 32 * 1024 * 1024, 64 * 1024 * 1024) as port:
            await buffering(port, False)
        async with native(binary, web, assets, 256 * 1024, 16 * 1024 * 1024) as port:
            await buffering(port, True)
    print("actual WebSocket/HTTP transport checks passed")


if __name__ == "__main__":
    asyncio.run(run(pathlib.Path(sys.argv[1]).resolve(), "--backpressure-only" in sys.argv[2:]))
