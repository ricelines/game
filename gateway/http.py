"""HTTP routing only. Sessions and authoring remain independent services."""

import json

from aiohttp import web

from gateway import mcp
from gateway.session import SessionError


def player_key(request):
    authorization = request.headers.get("Authorization", "")
    if not authorization.startswith("Bearer ") or not authorization[7:]:
        raise web.HTTPUnauthorized(text="Bearer player key required")
    return authorization[7:]


def create_app(sessions, authoring):
    @web.middleware
    async def origin_check(request, handler):
        # The gateway serves non-browser clients; no supplied origin is accepted.
        if "Origin" in request.headers:
            raise web.HTTPForbidden(text="Origin not allowed")
        try:
            return await handler(request)
        except ValueError as error:
            return web.json_response({"error": "bad_input", "problem": str(error)}, status=400)
        except (SessionError, OSError, TimeoutError) as error:
            return web.json_response(
                {"error": "server_unavailable", "problem": str(error)}, status=503
            )

    app = web.Application(middlewares=[origin_check], client_max_size=2 * 1024 * 1024)

    async def get_session(request):
        return await sessions.get(
            player_key(request), request.headers.get("X-Ambergris-Session", "cli")
        )

    async def rpc(request):
        identity = None
        try:
            body = await request.json()
            identity = body.get("id") if isinstance(body, dict) else None
            mcp.validate(request.headers, body)
            player_key(request)
            if "id" not in body:
                return web.Response(status=202)
            result = await mcp.dispatch(body, lambda: get_session(request))
            return web.json_response({"jsonrpc": "2.0", "id": identity, "result": result})
        except json.JSONDecodeError:
            return web.json_response(mcp.RpcError(-32700, "Parse error").response(), status=400)
        except mcp.RpcError as error:
            return web.json_response(error.response(identity), status=error.status)
        except (SessionError, OSError, TimeoutError) as error:
            failure = mcp.RpcError(
                -32603,
                "World service unavailable",
                status=503,
                data={"kind": "server_unavailable", "problem": str(error)},
            )
            return web.json_response(failure.response(identity), status=503)

    async def tool(request):
        session = await get_session(request)
        return web.json_response(
            await session.tool(request.match_info["name"], await request.json())
        )

    async def model(request):
        session = await get_session(request)
        result = await authoring.submit_model(session, await request.json())
        return web.json_response(result, status=202)

    async def script(request):
        session = await get_session(request)
        result = await authoring.submit_script(session, await request.json())
        return web.json_response(result, status=200 if result.get("ok") else 422)

    async def variant(request):
        session = await get_session(request)
        result = await authoring.submit_variant(session, await request.json())
        return web.json_response(result, status=202)

    async def edit_model(request):
        session = await get_session(request)
        result = await authoring.edit_model(session, await request.json())
        return web.json_response(result, status=202)

    async def job(request):
        session = await get_session(request)
        try:
            result = authoring.status(session, request.match_info["identity"])
        except KeyError as error:
            raise web.HTTPNotFound(text="No such job for this session") from error
        return web.json_response(result)

    async def health(_request):
        return web.json_response(
            {
                "ok": True,
                "service": "gateway",
                "bake": "ready" if authoring.pool else "unavailable",
            }
        )

    async def cleanup(_app):
        await authoring.close()
        await sessions.close()

    app.router.add_post("/mcp", rpc)
    app.router.add_post("/tools/{name}", tool)
    app.router.add_post("/bake/models", model)
    app.router.add_post("/bake/scripts", script)
    app.router.add_post("/bake/variants", variant)
    app.router.add_post("/bake/edits", edit_model)
    app.router.add_get("/bake/jobs/{identity}", job)
    app.router.add_get("/health", health)
    app.on_cleanup.append(cleanup)
    return app
