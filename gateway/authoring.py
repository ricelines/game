"""External sandboxed CAD jobs; completed registrations enter the world input lane."""

import asyncio
import hashlib
import json
import re
import uuid

from bake.parameters import canonical_params, resolve


def model_id(value):
    if not isinstance(value, str) or not re.fullmatch(r"m:[1-9][0-9]*", value):
        raise ValueError("model must be a full m:N identifier")
    return value


class RegistrationError(RuntimeError):
    def __init__(self, reply):
        self.record_id = reply.get("record_id")
        super().__init__(str(reply.get("errors", reply.get("problem", reply.get("error", reply)))))


class Authoring:
    def __init__(self, pool=None, unavailable="Bake service not configured"):
        self.pool = pool
        self.unavailable = unavailable
        self.jobs = {}
        self.tasks = set()

    def _start(self, session, operation, source, work):
        identity = uuid.uuid4().hex
        job = {"job": identity, "operation": operation, "status": "queued"}
        self.jobs[identity] = {
            "owner": (session.key, session.token),
            "public": job,
            "source_hash": hashlib.sha256(source.encode()).hexdigest(),
        }
        task = asyncio.create_task(self._run(session, work, job))
        self.tasks.add(task)
        task.add_done_callback(self.tasks.discard)
        return {"job": identity}

    async def _run(self, session, work, job):
        try:
            if self.pool is None:
                raise RuntimeError(self.unavailable)
            await work(job)
        except Exception as error:
            job["errors"] = [str(error)]
            entry = self.jobs[job["job"]]
            receipt = getattr(error, "record_id", None) or entry.get("record_id")
            if receipt:
                job.update({"status": "failed", "record_id": receipt})
                return
            job["status"] = "registering"
            try:
                async with session.mutations:
                    recorded = await session.request(
                        "authoring_failed",
                        job=job["job"],
                        operation=job["operation"],
                        source_hash=entry["source_hash"],
                        errors=job["errors"],
                    )
                if recorded.get("ok") and recorded.get("record_id"):
                    job["record_id"] = recorded["record_id"]
                else:
                    job["record_error"] = recorded
            except Exception as publication_error:
                job["record_error"] = str(publication_error)
            finally:
                job["status"] = "failed"

    async def _bake(self, source, params, job):
        job["status"] = "baking"
        identity = self.pool.submit(source, params)
        while True:
            result = self.pool.status(identity)
            if result["status"] == "done":
                return result
            if result["status"] == "failed":
                raise RuntimeError(str(result.get("errors", result.get("error", result))))
            job["status"] = result["status"]
            await asyncio.sleep(0.025)

    async def _register(self, session, kind, result, tracking, **fields):
        manifest = result["measured"]
        entry = self.jobs[tracking["job"]]
        entry.pop("record_id", None)
        tracking.pop("record_id", None)
        reply = await session.request(
            kind,
            asset=result["hash"],
            canonical_params=canonical_params(manifest["params"]),
            manifest=manifest,
            **fields,
        )
        if reply.get("record_id"):
            entry["record_id"] = tracking["record_id"] = reply["record_id"]
        if not reply.get("ok"):
            raise RegistrationError(reply)
        return reply

    async def _definition(self, session, identity):
        result = await session.request("inspect", target=model_id(identity))
        if not result.get("ok"):
            raise ValueError(str(result.get("errors", result.get("error", result))))
        source = result.get("source")
        if not isinstance(source, str):
            raise ValueError("Current verified model source is unavailable")
        if hashlib.sha256(source.encode()).hexdigest() != result.get("source_hash"):
            raise ValueError("Inspected source does not match the current model source hash")
        return result

    async def submit_model(self, session, request):
        if not isinstance(request.get("source"), str):
            raise ValueError("source must be Python text")
        if not isinstance(request.get("text"), dict) or not isinstance(
            request["text"].get("name"), str
        ):
            raise ValueError("text.name is required")
        if not isinstance(request.get("params", {}), dict):
            raise ValueError("params must be an object")

        async def work(job):
            result = await self._bake(request["source"], request.get("params", {}), job)
            job["status"] = "registering"
            async with session.mutations:
                reply = await self._register(
                    session,
                    "register_model",
                    result,
                    job,
                    job=job["job"],
                    text=request["text"],
                    material=request.get("material", result["measured"].get("default_material")),
                )
            job.update(
                {
                    "status": "done",
                    "model": reply["model"],
                    "hash": result["hash"],
                    "measured": reply.get("measured", result["measured"]),
                    "sandbox": result.get("sandbox", {}),
                }
            )

        return self._start(session, "model", request["source"], work)

    async def submit_variant(self, session, request):
        identity = model_id(request.get("model"))
        if not isinstance(request.get("params"), dict):
            raise ValueError("params must be an object")
        definition = await self._definition(session, identity)

        async def work(job):
            result = await self._bake(definition["source"], request["params"], job)
            job["status"] = "registering"
            async with session.mutations:
                reply = await self._register(
                    session,
                    "register_variant",
                    result,
                    job,
                    model=identity,
                )
            job.update(
                {
                    **reply,
                    "status": "done",
                    "hash": result["hash"],
                    "measured": reply.get("measured", result["measured"]),
                    "sandbox": result.get("sandbox", {}),
                }
            )

        return self._start(session, "variant", definition["source"], work)

    async def edit_model(self, session, request):
        identity = model_id(request.get("model"))
        source = request.get("source")
        if not isinstance(source, str):
            raise ValueError("source must be Python text")
        definition = await self._definition(session, identity)

        async def work(job):
            # Use the bake subsystem's literal PARAMS resolver: block0 adopts the new
            # defaults; explicit current blocks preserve values and gain new defaults.
            _defaults, schema = resolve(source, {})
            supplied = [
                {},
                *(
                    {
                        key: value
                        for key, value in json.loads(row["canonical_params"]).items()
                        if key in schema
                    }
                    for row in definition["variants"]
                ),
            ]
            parameters = list(
                dict.fromkeys(canonical_params(resolve(source, row)[0]) for row in supplied)
            )
            results = []
            for canonical in parameters:
                job["progress"] = {"completed": len(results), "total": len(parameters)}
                results.append(await self._bake(source, json.loads(canonical), job))
            # Every required CAD execution succeeds before staging a proposal.
            job["status"] = "registering"
            async with session.mutations:
                initial = await self._register(
                    session,
                    "register_model",
                    results[0],
                    job,
                    job=job["job"],
                    text=definition["says"],
                    material=definition["material"],
                    variants=[
                        {
                            "asset": result["hash"],
                            "canonical_params": canonical_params(result["measured"]["params"]),
                            "manifest": result["measured"],
                        }
                        for result in results[1:]
                    ],
                )
                proposal = initial["model"]
            job.update(
                {
                    "status": "done",
                    "effect": "proposal_ready",
                    "model": identity,
                    "proposal": proposal,
                    "hash": results[0]["hash"],
                    "variants": initial["variants"],
                    "progress": {"completed": len(results), "total": len(results)},
                }
            )

        return self._start(session, "edit_model", source, work)

    async def submit_script(self, session, request):
        if not isinstance(request.get("source"), str):
            raise ValueError("source must be Luau text")
        async with session.mutations:
            return await session.request("register_script", source=request["source"])

    def status(self, session, identity):
        row = self.jobs.get(identity)
        if row is None or row["owner"] != (session.key, session.token):
            raise KeyError(identity)
        return dict(row["public"])

    async def close(self):
        await asyncio.gather(*list(self.tasks), return_exceptions=True)
        if self.pool:
            await asyncio.to_thread(self.pool.close)
