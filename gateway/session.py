"""Multiplex one authoritative S6 connection and queue its unread hearing events."""

import asyncio
import contextlib
import json
import tomllib
from collections import deque
from pathlib import Path

from websockets.asyncio.client import connect

from gateway.schema import validate

with (Path(__file__).resolve().parents[1] / "config.toml").open("rb") as _stream:
    EVENTS_PER_RESPONSE = tomllib.load(_stream)["perception"]["EVENTS_PER_RESPONSE"]


class SessionError(RuntimeError):
    pass


class Session:
    def __init__(self, url, key, token, timeout=30):
        self.url, self.key, self.token = url, key, token
        self.timeout = timeout
        self.socket = None
        self.reader = None
        self.welcome = {}
        self.pending = {}
        self.next_id = 0
        self.events = deque()
        self.mutations = asyncio.Lock()
        self.closed_reason = ""
        self.known = {}
        self.action_lists = {}
        self._welcomed = asyncio.get_running_loop().create_future()

    async def open(self):
        self.socket = await connect(self.url, max_size=8 * 1024 * 1024, open_timeout=5)
        self.reader = asyncio.create_task(self._read())
        await self.socket.send(
            json.dumps(
                {"t": "hello", "version": 1, "key": self.key, "mode": "play", "binary": False}
            )
        )
        await asyncio.wait_for(asyncio.shield(self._welcomed), 5)
        return self

    async def _read(self):
        reason = "Connection closed"
        try:
            async for raw in self.socket:
                if not isinstance(raw, str):
                    continue
                message = json.loads(raw)
                kind = message.get("t")
                if kind == "welcome":
                    self.welcome = message
                    if not self._welcomed.done():
                        self._welcomed.set_result(message)
                elif kind == "event":
                    self.events.append({key: value for key, value in message.items() if key != "t"})
                elif kind == "bye":
                    reason = message.get("reason", "Connection closed")
                    break
                elif message.get("id") in self.pending:
                    future = self.pending.pop(message["id"])
                    if not future.done():
                        future.set_result(message)
        except Exception as error:
            reason = f"{type(error).__name__}: {error}"
        finally:
            self.closed_reason = reason
            failure = SessionError(reason)
            if not self._welcomed.done():
                self._welcomed.set_exception(failure)
            for future in self.pending.values():
                if not future.done():
                    future.set_exception(failure)
            self.pending.clear()

    async def request(self, kind, **arguments):
        if self.closed_reason:
            raise SessionError(self.closed_reason)
        self.next_id += 1
        request_id = self.next_id
        future = asyncio.get_running_loop().create_future()
        self.pending[request_id] = future
        try:
            await self.socket.send(json.dumps({"t": kind, "id": request_id, **arguments}))
            reply = await asyncio.wait_for(future, self.timeout)
            return {key: value for key, value in reply.items() if key not in ("t", "id")}
        finally:
            self.pending.pop(request_id, None)

    def attach_events(self, result, limit):
        queued = list(result.get("events", [])) + list(self.events)
        count = len(queued)
        self.events.clear()
        result["events"] = queued[-limit:]
        result["events_omitted"] = result.get("events_omitted", 0) + max(0, count - limit)
        return result

    def forget_observation(self):
        self.known.clear()
        self.action_lists.clear()

    def compact_observation(self, result):
        for field in ("things", "far"):
            for index, row in enumerate(result.get(field, [])):
                handle = row["h"]
                description = {
                    key: value for key, value in row.items() if key not in ("dist_m", "bearing")
                }
                if self.known.get(handle) == description:
                    result[field][index] = {key: row[key] for key in ("h", "dist_m", "bearing")}
                self.known[handle] = description
        for field in ("body_actions", "player_actions", "action_spaces"):
            if field not in result:
                continue
            actions = result[field]
            if self.action_lists.get(field) == actions:
                del result[field]
            else:
                self.action_lists[field] = actions
        return result

    async def tool(self, name, arguments):
        validate(name, arguments)
        arguments = dict(arguments)
        event_limit = arguments.pop("events_limit", EVENTS_PER_RESPONSE)
        # Native mutations serialize; pending CAD execution stays outside this lane.
        if name in ("do", "cancel"):
            async with self.mutations:
                result = await self.request(name, events_limit=event_limit, **arguments)
        else:
            result = await self.request(name, events_limit=event_limit, **arguments)
        if name == "look" and "things" in result:
            self.compact_observation(result)
        return self.attach_events(result, event_limit)

    async def close(self):
        if self.socket:
            await self.socket.close()
        if self.reader:
            self.reader.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self.reader
        if self._welcomed.done() and not self._welcomed.cancelled():
            self._welcomed.exception()


class Sessions:
    def __init__(self, url):
        self.url = url
        self.rows = {}
        self.opening = {}

    async def get(self, key, token="cli"):
        identity = (key, token)
        async with self.opening.setdefault(identity, asyncio.Lock()):
            if identity in self.rows:
                session = self.rows[identity]
                if session.closed_reason == "replaced":
                    raise SessionError("replaced")
                if not session.closed_reason:
                    return session
                await session.close()
            session = Session(self.url, key, token)
            try:
                await session.open()
            except Exception:
                await session.close()
                raise
            self.rows[identity] = session
            return session

    async def close(self):
        await asyncio.gather(*(row.close() for row in self.rows.values()))
