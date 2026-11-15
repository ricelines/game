"""Ordered completion DTOs, actual source identity and distinct-param staging."""

import asyncio
import hashlib

import pytest
from aiohttp.test_utils import TestClient, TestServer

from gateway.authoring import Authoring
from gateway.cli import parser
from gateway.http import create_app
from harness.resident import AUTHOR_TOOLS, Residents

SOURCE = (
    "from build123d import Box\nPARAMS={'width':(1,0.1,4)}\n"
    "def build(p):return Box(p['width'],1,1)\n"
)


class ArtifactQueue:
    """A labelled completion fixture, not an executing geometry worker."""

    def __init__(self, *, failed_width=None):
        self.submissions = []
        self.failed_width = failed_width

    def submit(self, source, params):
        self.submissions.append((source, params))
        return len(self.submissions) - 1

    def status(self, identity):
        source, params = self.submissions[identity]
        if params["width"] == self.failed_width:
            return {"status": "failed", "errors": ["actual required parameter rejected"]}
        return {
            "status": "done",
            "hash": hashlib.sha256((source + str(params)).encode()).hexdigest(),
            "measured": {"params": params, "default_material": "novel.material"},
        }

    def close(self):
        pass


class NativeTransport:
    """Exact inspected source/canonical DTO fixture; no world effects are simulated."""

    def __init__(self, *, stale=False):
        self.key, self.token = "fixture", "fixture"
        self.mutations = asyncio.Lock()
        self.calls = []
        self.stale = stale

    async def request(self, kind, **fields):
        self.calls.append((kind, fields))
        if kind == "inspect":
            return {
                "ok": True,
                "model": "m:5",
                "source": SOURCE,
                "source_hash": hashlib.sha256(SOURCE.encode()).hexdigest(),
                "canonical_params": '{"width":1.0}',
                "variants": [
                    {"canonical_params": '{"width":1.0}'},
                    {"canonical_params": '{"width":2.0}'},
                    {"canonical_params": '{"width":2.0}'},
                ],
                "says": {"name": "Authored name", "description": "Claim"},
                "material": "novel.material",
            }
        assert self.mutations.locked(), "only completed registrations own the mutation lane"
        if kind == "authoring_failed":
            return {"ok": True, "record_id": 92}
        if kind == "register_model":
            return {
                "ok": True,
                "model": "m:9",
                "record_id": 90,
                "variants": [
                    {"params": 12 + index, "hash": row["asset"], "measured": row["manifest"]}
                    for index, row in enumerate(fields.get("variants", []))
                ],
            }
        assert kind == "register_variant"
        if self.stale:
            return {"ok": False, "errors": ["model_source_changed"], "record_id": 91}
        return {
            "ok": True,
            "model": fields["model"],
            "params": 12,
            "hash": fields["asset"],
            "measured": fields["manifest"],
        }


def test_variant_uses_current_verified_source_and_native_stale_check():
    async def run():
        pool, session = ArtifactQueue(), NativeTransport()
        author = Authoring(pool)
        result = await author.submit_variant(session, {"model": "m:5", "params": {"width": 3}})
        await asyncio.gather(*list(author.tasks))
        job = author.status(session, result["job"])
        assert job["status"] == "done" and job["model"] == "m:5" and job["params"] == 12
        assert pool.submissions == [(SOURCE, {"width": 3})]
        kind, fields = session.calls[-1]
        assert kind == "register_variant" and "material" not in fields and "text" not in fields
        assert fields["expected_source_hash"] == hashlib.sha256(SOURCE.encode()).hexdigest()
        assert fields["canonical_params"] == '{"width":3.0}'
        session.stale = True
        result = await author.submit_variant(session, {"model": "m:5", "params": {"width": 4}})
        await asyncio.gather(*list(author.tasks))
        assert "model_source_changed" in author.status(session, result["job"])["errors"][0]
        assert author.status(session, result["job"])["record_id"] == 91
        assert not any(kind == "authoring_failed" for kind, _fields in session.calls)
        with pytest.raises(KeyError):
            author.status(
                type("Other", (), {"key": session.key, "token": "another"})(), result["job"]
            )
        await author.close()

    asyncio.run(run())


def test_definition_edit_bakes_each_unique_set_and_stages_without_applying():
    async def run():
        pool, session = ArtifactQueue(), NativeTransport()
        author = Authoring(pool)
        changed = SOURCE + "# Changed source\n"
        result = await author.edit_model(session, {"model": "m:5", "source": changed})
        await asyncio.gather(*list(author.tasks))
        job = author.status(session, result["job"])
        assert job["status"] == "done" and job["effect"] == "revision_staged"
        assert job["model"] == "m:5" and job["revision_model"] == "m:9"
        assert pool.submissions == [(changed, {"width": 1.0}), (changed, {"width": 2.0})]
        assert [kind for kind, _fields in session.calls] == ["inspect", "register_model"]
        assert session.calls[-1][1]["variants"][0]["canonical_params"] == '{"width":2.0}'
        assert job["variants"][0]["params"] == 12 and job["record_id"] == 90
        assert job["expected_source_hash"] == hashlib.sha256(SOURCE.encode()).hexdigest()
        await author.close()

    asyncio.run(run())


def test_definition_default_changes_and_new_parameters_use_actual_bake_resolution():
    async def run():
        pool, session = ArtifactQueue(), NativeTransport()
        author = Authoring(pool)
        changed = SOURCE.replace("(1,0.1,4)", "(1.5,0.1,4), 'depth':(0.25,0.1,1)")
        result = await author.edit_model(session, {"model": "m:5", "source": changed})
        await asyncio.gather(*list(author.tasks))
        job = author.status(session, result["job"])
        assert job["status"] == "done" and job["progress"] == {"completed": 3, "total": 3}
        assert [params for _source, params in pool.submissions] == [
            {"depth": 0.25, "width": 1.5},
            {"depth": 0.25, "width": 1.0},
            {"depth": 0.25, "width": 2.0},
        ]
        assert session.calls[1][1]["manifest"]["params"]["width"] == 1.5
        await author.close()

    asyncio.run(run())


def test_failed_required_set_publishes_one_attempt_without_native_registration():
    async def run():
        pool, session = ArtifactQueue(failed_width=2), NativeTransport()
        author = Authoring(pool)
        result = await author.edit_model(session, {"model": "m:5", "source": SOURCE})
        await asyncio.gather(*list(author.tasks))
        assert author.status(session, result["job"])["status"] == "failed"
        assert [kind for kind, _fields in session.calls] == ["inspect", "authoring_failed"]
        kind, fields = session.calls[-1]
        assert fields["job"] == result["job"] and fields["operation"] == "edit_model"
        assert fields["source_hash"] == hashlib.sha256(SOURCE.encode()).hexdigest()
        assert fields["errors"] and author.status(session, result["job"])["record_id"] == 92
        await author.close()

    asyncio.run(run())


@pytest.mark.parametrize(
    "denial, explanation",
    [
        (
            {"errors": ["native request preparation rejected"]},
            "native request preparation rejected",
        ),
        ({"error": "bad_input", "problem": "text.description is required"}, "text.description"),
    ],
)
def test_immediate_native_denial_gets_one_failure_but_recorded_denial_is_not_duplicated(
    denial, explanation
):
    class ImmediateDenial(NativeTransport):
        async def request(self, kind, **fields):
            if kind == "register_model":
                self.calls.append((kind, fields))
                return {"ok": False, **denial}
            return await super().request(kind, **fields)

    async def run():
        pool, session = ArtifactQueue(), ImmediateDenial()
        author = Authoring(pool)
        result = await author.submit_model(
            session, {"source": SOURCE, "params": {"width": 1}, "text": {"name": "fixture"}}
        )
        await asyncio.gather(*list(author.tasks))
        assert [kind for kind, _fields in session.calls] == [
            "register_model",
            "authoring_failed",
        ]
        assert author.status(session, result["job"])["record_id"] == 92
        assert explanation in author.status(session, result["job"])["errors"][0]
        assert explanation in session.calls[-1][1]["errors"][0]
        await author.close()

    asyncio.run(run())


def test_error_after_recorded_registration_retains_receipt_without_extra_failure():
    class RecordedButIncomplete(NativeTransport):
        async def request(self, kind, **fields):
            if kind == "register_model":
                self.calls.append((kind, fields))
                return {"ok": True, "record_id": 77}
            return await super().request(kind, **fields)

    async def run():
        author, session = Authoring(ArtifactQueue()), RecordedButIncomplete()
        result = await author.submit_model(
            session, {"source": SOURCE, "params": {"width": 1}, "text": {"name": "fixture"}}
        )
        await asyncio.gather(*list(author.tasks))
        assert author.status(session, result["job"])["status"] == "failed"
        assert author.status(session, result["job"])["record_id"] == 77
        assert [kind for kind, _fields in session.calls] == ["register_model"]
        await author.close()

    asyncio.run(run())


def test_baked_job_is_not_done_until_its_native_registration_completes():
    async def run():
        pool, session = ArtifactQueue(), NativeTransport()
        author = Authoring(pool)
        await session.mutations.acquire()
        result = await author.submit_variant(session, {"model": "m:5", "params": {"width": 2}})
        await asyncio.sleep(0)
        status = author.status(session, result["job"])
        assert status["status"] == "registering" and "params" not in status
        assert [kind for kind, _fields in session.calls] == ["inspect"]
        session.mutations.release()
        await asyncio.gather(*list(author.tasks))
        assert author.status(session, result["job"])["status"] == "done"
        await author.close()

    asyncio.run(run())


def test_http_cli_and_harness_expose_same_authoring_operations(tmp_path):
    assert parser().parse_args(["look", "--full"]).full is True
    assert parser().parse_args(["variant", "m:5", "--params", '{"width":2}']).params == {"width": 2}
    assert parser().parse_args(["edit-model", "m:5", str(tmp_path / "source.py")]).model == "m:5"
    assert {"submit_variant", "edit_model"} <= {tool["name"] for tool in AUTHOR_TOOLS}

    async def run():
        pool, session = ArtifactQueue(), NativeTransport()

        class Sessions:
            async def get(self, _key, _token):
                return session

            async def close(self):
                pass

        author = Authoring(pool)
        async with TestClient(TestServer(create_app(Sessions(), author))) as client:
            headers = {"Authorization": "Bearer fixture"}
            response = await client.post(
                "/bake/variants", json={"model": "m:5", "params": {"width": 2}}, headers=headers
            )
            assert response.status == 202 and "job" in await response.json()
            response = await client.post(
                "/bake/edits", json={"model": "m:5", "source": SOURCE}, headers=headers
            )
            assert response.status == 202 and "job" in await response.json()
            with pytest.raises(ValueError, match="full m:N"):
                await author.submit_variant(session, {"model": "5", "params": {}})
            # Execute through the actual harness dispatcher, without any model API call.
            residents = Residents([], Sessions(), author, client=object())
            result = await residents._execute(
                session, "submit_variant", {"model": "m:5", "params": {"width": 3}}
            )
            assert "job" in result

    asyncio.run(run())
