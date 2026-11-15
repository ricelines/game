"""One real Responses conversation per resident, without purpose-to-action policy."""

import asyncio
import json
import time
from dataclasses import dataclass, field

from openai import AsyncOpenAI

from gateway.schema import TOOLS, object_schema

AUTHOR_TOOLS = [
    {
        "name": "submit_model",
        "description": (
            "Author arbitrary build123d Python geometry through the real sandboxed "
            "bake pipeline. Returns a job, not a placed object. Inspect your "
            "plot/rules and actions to construct legitimately."
        ),
        "inputSchema": object_schema(
            {
                "source": {"type": "string"},
                "params": {"type": "object"},
                "material": {"type": "string"},
                "text": object_schema(
                    {
                        "name": {"type": "string"},
                        "description": {"type": "string"},
                        "writing": {"type": "string"},
                    },
                    ["name", "description"],
                ),
            },
            ["source", "text"],
        ),
    },
    {
        "name": "submit_variant",
        "description": (
            "Bake different parameters of an existing model using its exact current source. "
            "Poll the job for a ready numeric params block; apply it only through ordinary "
            "authored actions and their material/permission requirements."
        ),
        "inputSchema": object_schema(
            {"model": {"type": "string"}, "params": {"type": "object"}}, ["model", "params"]
        ),
    },
    {
        "name": "edit_model",
        "description": (
            "Stage an existing model definition edit from new build123d source, rebaking "
            "every distinct current parameter set. The completed job returns proposal; "
            "it does not change instances. The model's author applies it with "
            "base.revise_model{model,proposal}."
        ),
        "inputSchema": object_schema(
            {"model": {"type": "string"}, "source": {"type": "string"}}, ["model", "source"]
        ),
    },
    {
        "name": "submit_script",
        "description": (
            "Upload arbitrary Luau source for native validation and immutable "
            "registration; errors are real compiler/ABI feedback. Upload does not "
            "authorize installation or construction."
        ),
        "inputSchema": object_schema({"source": {"type": "string"}}, ["source"]),
    },
    {
        "name": "bake_status",
        "description": (
            "Poll a previously returned bake job for model/hash/measured output or errors."
        ),
        "inputSchema": object_schema({"job": {"type": "string"}}, ["job"]),
    },
]
SYSTEM = """You inhabit Ambergris through an ordinary body. Your purpose is a reason to act, not a
guaranteed outcome. Discover available actions and inspect actual measurements, rights,
supplies, money and state. Maker's claims can be wrong. Respect failed actions, path obstruction
and denials; solve through available permissions or ask other residents. Use tools to act; prose
alone changes nothing. Never invent successful effects or object handles. You may author
arbitrary CAD and Luau via authoring tools when existing objects are insufficient. Follow the
supplied host/authoring reference, and use validation errors to revise. You are not an
administrator and cannot assume construction, mint or install powers. Persist with useful,
coherent activity and avoid repeatedly issuing identical actions while an intent is active.
Speak in the world only through its actual speech action. When waiting, conclude this turn so
the world can advance."""


@dataclass
class Resident:
    identity: str
    name: str
    key: str = field(repr=False)
    purpose: str = ""
    persona: str = ""
    model: str = "gpt-5.4-mini"
    interval_s: float = 8.0
    body: str = ""
    status: str = "starting"
    transcript: list = field(default_factory=list, repr=False)
    archived_transcripts: list = field(default_factory=list, repr=False)
    observations: list = field(default_factory=list, repr=False)
    context_resets: int = 0
    revision: int = 0
    refresh_observation: bool = field(default=True, repr=False)
    wake: asyncio.Event = field(default_factory=asyncio.Event, repr=False)

    def public(self):
        return {
            "id": self.identity,
            "name": self.name,
            "body": self.body,
            "purpose": self.purpose,
            "status": self.status,
            "model": self.model,
        }


class Residents:
    def __init__(
        self,
        specs,
        sessions,
        authoring,
        *,
        reference="",
        client=None,
        max_calls=12,
        max_output_tokens=8192,
        max_responses=0,
        report_sink=None,
    ):
        # AsyncOpenAI reads injected process environment; no credential file is opened here.
        self.client = client or AsyncOpenAI()
        self.rows = {row.identity: row for row in specs}
        self.sessions, self.authoring = sessions, authoring
        self.reference, self.max_calls, self.max_output_tokens = (
            reference,
            max_calls,
            max_output_tokens,
        )
        self.report_sink = report_sink
        self.max_responses, self.api_attempts = max_responses, 0
        self.tasks = []
        self.measurements = []
        self.stopping = False
        self.tools = [
            {
                "type": "function",
                "name": tool["name"],
                "description": tool["description"],
                "parameters": tool["inputSchema"],
                "strict": False,
            }
            for tool in TOOLS + AUTHOR_TOOLS
        ]

    def public(self):
        return [row.public() for row in self.rows.values()]

    def report(self):
        """Actual model/tool evidence, explicitly excluding every resident credential."""
        return {
            "kind": "actual Responses API resident evidence",
            "agents": [
                {
                    **row.public(),
                    "transcript": row.transcript,
                    "archived_transcripts": row.archived_transcripts,
                    "context_resets": row.context_resets,
                    "observations": row.observations,
                }
                for row in self.rows.values()
            ],
            "responses": self.measurements,
            "api_attempts": self.api_attempts,
            "max_api_attempts": self.max_responses,
        }

    @property
    def budget_reached(self):
        return self.max_responses > 0 and self.api_attempts >= self.max_responses

    def checkpoint(self):
        if self.report_sink:
            self.report_sink(self.report())

    def start(self):
        self.tasks = [
            asyncio.create_task(self._loop(row), name="resident-" + row.identity)
            for row in self.rows.values()
        ]

    async def set_purpose(self, identity, purpose):
        row = self.rows[identity]
        row.purpose = purpose
        row.revision += 1
        row.wake.set()
        session = await self.sessions.get(row.key, "resident-" + row.identity)
        await session.tool("cancel", {})
        self.checkpoint()

    async def _execute(self, session, name, args):
        if name == "submit_model":
            return await self.authoring.submit_model(session, args)
        if name == "submit_script":
            return await self.authoring.submit_script(session, args)
        if name == "submit_variant":
            return await self.authoring.submit_variant(session, args)
        if name == "edit_model":
            return await self.authoring.edit_model(session, args)
        if name == "bake_status":
            return self.authoring.status(session, args["job"])
        return await session.tool(name, args)

    async def turn(self, row):
        session = await self.sessions.get(row.key, "resident-" + row.identity)
        row.body = session.welcome.get("body", "")
        fresh_context = row.refresh_observation or not row.transcript
        if fresh_context:
            session.forget_observation()
        observation = await session.tool("look", {})
        row.refresh_observation = False
        things = observation.get("things", []) + observation.get("far", [])
        full_rows = sum(bool(record.keys() - {"h", "dist_m", "bearing"}) for record in things)
        row.observations.append(
            {
                "fresh_context": fresh_context,
                "step": observation.get("step"),
                "full_rows": full_rows,
                "short_rows": len(things) - full_rows,
                "body_actions_present": "body_actions" in observation,
                "player_actions_present": "player_actions" in observation,
            }
        )
        if not row.transcript:
            row.transcript.append(
                {
                    "role": "developer",
                    "content": SYSTEM
                    + "\n\n"
                    + row.persona
                    + "\n\nAUTHORING REFERENCE\n"
                    + self.reference,
                }
            )
        row.transcript.append(
            {
                "role": "user",
                "content": "Current purpose: "
                + row.purpose
                + "\nActual observation: "
                + json.dumps(observation, separators=(",", ":")),
            }
        )
        self.checkpoint()
        revision = row.revision
        for _round in range(self.max_calls):
            if self.budget_reached:
                row.status = "API attempt budget reached"
                self.stopping = True
                break
            row.status = "thinking"
            began = time.perf_counter()
            self.api_attempts += 1
            response = await self.client.responses.create(
                model=row.model,
                input=row.transcript,
                tools=self.tools,
                parallel_tool_calls=False,
                max_output_tokens=self.max_output_tokens,
                reasoning={"effort": "medium"},
                store=False,
                include=["reasoning.encrypted_content"],
            )
            self.measurements.append(
                {
                    "agent": row.identity,
                    "model": row.model,
                    "response": response.id,
                    "seconds": time.perf_counter() - began,
                    "usage": response.usage.model_dump() if response.usage else {},
                }
            )
            output = [item.model_dump(exclude_none=True) for item in response.output]
            row.transcript.extend(output)
            self.checkpoint()
            calls = [item for item in response.output if item.type == "function_call"]
            if not calls:
                row.status = "waiting"
                break
            for call in calls:
                if row.revision != revision:
                    result = {
                        "error": "purpose_changed",
                        "problem": "Action not executed. Observe again and follow the new purpose.",
                    }
                else:
                    row.status = "using " + call.name
                    try:
                        result = await self._execute(session, call.name, json.loads(call.arguments))
                    except (ValueError, KeyError, RuntimeError, OSError, TimeoutError) as error:
                        result = {"error": "tool_error", "problem": str(error)}
                row.transcript.append(
                    {
                        "type": "function_call_output",
                        "call_id": call.call_id,
                        "output": json.dumps(result, separators=(",", ":")),
                    }
                )
                self.checkpoint()
            if row.revision != revision:
                row.status = "purpose updated"
                break
        else:
            row.status = "turn budget reached"
        # Keep complete call/output pairs; restart with actual full observations when
        # memory gets large.
        if sum(len(json.dumps(item)) for item in row.transcript) > 250000:
            if self.report_sink:
                row.archived_transcripts.append(row.transcript)
            row.transcript = []
            row.context_resets += 1
        self.checkpoint()

    async def _loop(self, row):
        while not self.stopping:
            try:
                await self.turn(row)
            except asyncio.CancelledError:
                raise
            except Exception as error:
                # API failures never invoke a substitute policy.
                row.status = "unavailable: " + type(error).__name__
                self.checkpoint()
            try:
                await asyncio.wait_for(row.wake.wait(), timeout=row.interval_s)
            except TimeoutError:
                pass
            row.wake.clear()

    async def close(self):
        self.stopping = True
        for task in self.tasks:
            task.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)
        await self.client.close()
