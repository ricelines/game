# /// script
# requires-python = ">=3.11"
# dependencies = ["websockets==15.0.1"]
# ///
"""Read-only socket load/consistency check against the actual native game server.

Unlike net_socket.py, this never launches the transport fixture or sends its
fixture-only ping/flood commands. Credentials are consumed privately from a file.
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import math
import pathlib
import statistics
import struct
import time

from websockets.asyncio.client import connect


class Reader:
    def __init__(self, data: bytes):
        self.data, self.at = data, 0

    def take(self, size: int) -> bytes:
        end = self.at + size
        assert end <= len(self.data), "truncated S6 frame"
        result, self.at = self.data[self.at : end], end
        return result

    def number(self, fmt: str):
        return struct.unpack("<" + fmt, self.take(struct.calcsize(fmt)))[0]

    def varint(self):
        word = 0
        for shift in range(0, 35, 7):
            byte = self.number("B")
            word |= (byte & 127) << shift
            if not byte & 128:
                assert word <= 0xFFFFFFFF, "delta exceeds i32"
                return (word >> 1) ^ -(word & 1)
        raise AssertionError("overlong S6 varint")

    def states(self):
        result = {}
        for _ in range(self.number("B")):
            term, value = self.number("H"), self.number("f")
            assert term not in result and math.isfinite(value), "invalid replicated state"
            result[term] = 0.0 if value == 0 else value
        return result

    def entity(self):
        return {
            "handle": self.number("Q"),
            "asset": self.take(32),
            "params": self.number("Q"),
            "flags": self.number("B"),
            "position": tuple(self.number("i") for _ in range(3)),
            "rotation": self.number("I"),
            "states": self.states(),
        }


class Replica:
    def __init__(self):
        self.cells = {}
        self.steps = {}

    def apply(self, data: bytes):
        r = Reader(data)
        kind, cell = r.number("B"), (r.number("h"), r.number("h"))
        if kind == 3:
            self.cells.pop(cell, None)
            self.steps.pop(cell, None)
            assert r.at == len(data), "drop frame has trailing bytes"
            return None
        assert kind in (1, 2), "unknown S6 type"
        step = r.number("I")
        assert step >= self.steps.get(cell, 0), "cell step moved backward"
        if kind == 1:
            rows = self.cells[cell] = {}
        else:
            assert cell in self.cells, "delta arrived before its keyframe"
            rows = self.cells[cell]
            for _ in range(r.number("H")):
                del rows[r.number("H")]
        for _ in range(r.number("H")):
            local = r.number("H")
            assert local not in rows, "duplicate live local ID"
            rows[local] = r.entity()
        if kind == 2:
            for _ in range(r.number("H")):
                row, mask = rows[r.number("H")], r.number("B")
                assert 0 < mask <= 31, "invalid change mask"
                if mask & 1:
                    row["position"] = tuple(v + r.varint() for v in row["position"])
                if mask & 2:
                    row["rotation"] = r.number("I")
                if mask & 4:
                    row["asset"], row["params"] = r.take(32), r.number("Q")
                if mask & 8:
                    row["states"] = r.states()
                if mask & 16:
                    row["flags"] = r.number("B")
        assert r.at == len(data), "S6 frame has trailing bytes"
        assert len({v["handle"] for v in rows.values()}) == len(rows), "duplicate handle"
        self.steps[cell] = step
        normalized = tuple(
            (
                local,
                tuple(
                    (key, tuple(sorted(v.items())) if isinstance(v, dict) else v)
                    for key, v in row.items()
                ),
            )
            for local, row in sorted(rows.items())
        )
        return (cell, step), hashlib.blake2b(repr(normalized).encode(), digest_size=16).digest()


async def run(args):
    # Never include the credential or hello message in diagnostics.
    key = pathlib.Path(args.key_file).read_text().strip()
    connections, receivers = {}, {}
    counts = [[0, 0, 0, 0] for _ in range(args.clients + 1)]
    opened_at = [0.0] * (args.clients + 1)
    closed_at = [0.0] * (args.clients + 1)
    welcomes, rtts, pending = [], [], {}
    frame_hashes, snapshot_hashes = {}, {}
    comparisons = snapshots = recovered_keyframes = 0
    sem = asyncio.Semaphore(50)

    def bounded_compare(cache, token, digest):
        previous = cache.get(token)
        if previous is not None:
            assert previous == digest, "same cell/step diverged between actual sockets"
        else:
            cache[token] = digest
            if len(cache) > 8192:
                del cache[next(iter(cache))]
        return previous is not None

    async def receive(index, ws):
        nonlocal comparisons, snapshots, recovered_keyframes
        replica = Replica() if index < 4 or index == args.clients else None
        async for message in ws:
            if isinstance(message, bytes):
                assert len(message) >= 5, "truncated S6 header"
                kind, x, y = struct.unpack_from("<Bhh", message)
                assert kind in (1, 2, 3), "unknown S6 frame"
                counts[index][kind - 1] += 1
                counts[index][3] += len(message)
                if kind != 3:
                    assert len(message) >= 11, "truncated cell frame"
                    step = struct.unpack_from("<I", message, 5)[0]
                    comparisons += bounded_compare(
                        frame_hashes,
                        (kind, x, y, step),
                        hashlib.blake2b(message, digest_size=16).digest(),
                    )
                if replica is not None:
                    result = replica.apply(message)
                    if result is not None:
                        matched = bounded_compare(snapshot_hashes, *result)
                        snapshots += matched
                        if index == args.clients and kind == 1 and matched:
                            recovered_keyframes += 1
            else:
                row = json.loads(message)
                token = (index, row.get("id"))
                if token in pending:
                    started, future = pending.pop(token)
                    assert row.get("t") == "events" and "error" not in row, "query failed"
                    rtts.append((time.perf_counter() - started) * 1000)
                    future.set_result(None)
                assert row.get("t") != "bye", "server closed an authenticated watcher"
        closed_at[index] = time.perf_counter()

    async def join(index):
        async with sem:
            started = time.perf_counter()
            ws = await connect(args.url, max_size=None, compression=None, ping_interval=None)
            await ws.send(
                json.dumps(
                    {"t": "hello", "version": 1, "key": key, "mode": "watch", "binary": True}
                )
            )
            welcome = json.loads(await asyncio.wait_for(ws.recv(), 15))
            assert welcome.get("t") == "welcome" and welcome.get("mode") == "watch", (
                "hello rejected"
            )
            welcomes.append((time.perf_counter() - started) * 1000)
            connections[index] = ws
            opened_at[index] = time.perf_counter()
            receivers[index] = asyncio.create_task(receive(index, ws))
            await ws.send(json.dumps({"t": "view", "pos": args.position}))

    def check_receivers():
        for task in receivers.values():
            if task.done():
                task.result()
                raise AssertionError("actual socket receiver terminated early")

    try:
        started = time.perf_counter()
        await asyncio.gather(*(join(i) for i in range(args.clients)))
        joined = time.perf_counter() - started
        await asyncio.sleep(args.seconds)
        check_receivers()
        futures = []
        for index, ws in connections.items():
            future = asyncio.get_running_loop().create_future()
            pending[index, 1] = (time.perf_counter(), future)
            futures.append(future)
            await ws.send(json.dumps({"t": "events", "id": 1, "limit": 1}))
        try:
            await asyncio.wait_for(asyncio.gather(*futures), 15)
        except TimeoutError:
            raise AssertionError(
                f"only {len(rtts)}/{args.clients} real events replies arrived within15s; "
                f"live delta counts span {min(row[1] for row in counts[: args.clients])}.."
                f"{max(row[1] for row in counts[: args.clients])}"
            ) from None
        check_receivers()
        assert min(row[0] for row in counts[: args.clients]) > 0, "watcher missed keyframes"
        assert min(row[1] for row in counts[: args.clients]) >= args.min_deltas, (
            "watcher missed live deltas"
        )
        assert snapshots > 0 and comparisons > 0, "no common native cell baselines compared"
        # Replace one viewer with a fresh socket and independently decode its
        # keyframes against the already-running watchers' delta baselines.
        await connections.pop(args.clients - 1).close()
        await receivers.pop(args.clients - 1)
        await join(args.clients)
        await asyncio.sleep(max(1, args.seconds / 2))
        check_receivers()
        assert counts[args.clients][0] > 0, "reconnected viewer missed keyframes"
        assert recovered_keyframes > 0, "reconnected keyframe disagreed with live delta baseline"
        observed_at = time.perf_counter()
        durations = [(closed_at[i] or observed_at) - opened_at[i] for i in range(args.clients)]
        rates = [counts[i][3] / durations[i] for i in range(args.clients)]

        def distribution(values):
            ordered = sorted(values)
            return {
                "min": round(ordered[0], 3),
                "median": round(statistics.median(ordered), 3),
                "p95": round(ordered[int(len(ordered) * 0.95)], 3),
                "max": round(ordered[-1], 3),
            }

        report = {
            "actual_native_sockets": args.clients,
            "join_total_s": round(joined, 3),
            "welcome_p95_ms": round(sorted(welcomes[: args.clients])[int(args.clients * 0.95)], 3),
            "events_rtt_p50_ms": round(statistics.median(rtts), 3),
            "events_rtt_p95_ms": round(sorted(rtts)[int(args.clients * 0.95)], 3),
            "min_live_deltas": min(row[1] for row in counts[: args.clients]),
            "shared_frame_comparisons": comparisons,
            "decoded_baseline_comparisons": snapshots,
            "binary_bytes": sum(row[3] for row in counts),
            "reconnected_keyframes": counts[args.clients][0],
            "reconnected_baselines_compared": recovered_keyframes,
            "per_viewer_binary_observation_s": distribution(durations),
            "per_viewer_binary_bytes": distribution([row[3] for row in counts[: args.clients]]),
            "per_viewer_binary_bytes_s": distribution(rates),
            "bandwidth_window": "welcome to report or orderly disconnect; includes keyframes",
        }
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        print(json.dumps(report, sort_keys=True))
    except BaseException:
        check_receivers()
        raise
    finally:
        await asyncio.gather(*(ws.close() for ws in connections.values()), return_exceptions=True)
        await asyncio.gather(*receivers.values(), return_exceptions=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="ws://127.0.0.1:8080/ws")
    parser.add_argument("--key-file", required=True)
    parser.add_argument("--clients", type=int, default=500)
    parser.add_argument("--seconds", type=float, default=3)
    parser.add_argument("--min-deltas", type=int, default=3)
    parser.add_argument("--position", type=float, nargs=3, default=[0, 0, 2])
    parser.add_argument("--report", type=pathlib.Path)
    arguments = parser.parse_args()
    assert arguments.clients >= 4 and arguments.seconds > 0 and arguments.min_deltas >= 0
    asyncio.run(run(arguments))
