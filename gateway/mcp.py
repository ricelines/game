"""Current stateless MCP 2026-07-28 JSON-response transport."""

import base64
import binascii
import json

from gateway.schema import TOOLS

VERSION = "2026-07-28"
META_PREFIX = "io.modelcontextprotocol/"


class RpcError(Exception):
    def __init__(self, code, message, status=400, data=None):
        self.code, self.message, self.status, self.data = code, message, status, data
        super().__init__(message)

    def response(self, identity=None):
        error = {"code": self.code, "message": self.message}
        if self.data is not None:
            error["data"] = self.data
        return {"jsonrpc": "2.0", "id": identity, "error": error}


def _header(value):
    if value and value.startswith("=?base64?") and value.endswith("?="):
        try:
            return base64.b64decode(value[9:-2], validate=True).decode()
        except (ValueError, UnicodeError, binascii.Error) as error:
            raise RpcError(-32020, "Invalid encoded header") from error
    return value


def validate(headers, body):
    if (
        not isinstance(body, dict)
        or body.get("jsonrpc") != "2.0"
        or not isinstance(body.get("method"), str)
    ):
        raise RpcError(-32600, "Expected a single JSON-RPC 2.0 request")
    if "id" not in body:
        return
    params = body.get("params", {})
    if not isinstance(params, dict):
        raise RpcError(-32602, "params must be an object")
    meta = params.get("_meta", {})
    if not isinstance(meta, dict):
        raise RpcError(-32602, "_meta must be an object")
    version = headers.get("MCP-Protocol-Version")
    if not version or version != meta.get(META_PREFIX + "protocolVersion"):
        raise RpcError(-32020, "MCP-Protocol-Version header/body mismatch")
    if version != VERSION:
        raise RpcError(
            -32022,
            "Unsupported protocol version",
            data={"requested": version, "supported": [VERSION]},
        )
    if headers.get("Mcp-Method") != body["method"]:
        raise RpcError(-32020, "Mcp-Method header/body mismatch")
    if body["method"] == "tools/call" and _header(headers.get("Mcp-Name")) != params.get("name"):
        raise RpcError(-32020, "Mcp-Name header/body mismatch")
    if not isinstance(meta.get(META_PREFIX + "clientInfo"), dict) or not isinstance(
        meta.get(META_PREFIX + "clientCapabilities"), dict
    ):
        raise RpcError(-32602, "Required clientInfo/clientCapabilities _meta is missing")
    accepts = headers.get("Accept", "")
    if "application/json" not in accepts or "text/event-stream" not in accepts:
        raise RpcError(-32600, "Accept must include application/json and text/event-stream", 406)


async def dispatch(body, get_session):
    method = body["method"]
    params = body.get("params", {})
    if method == "server/discover":
        return {
            "resultType": "complete",
            "supportedVersions": [VERSION],
            "capabilities": {"tools": {}},
            "_meta": {META_PREFIX + "serverInfo": {"name": "ambergris", "version": "0.1.0"}},
            "instructions": (
                "Use the five game tools to perceive and act through your ordinary "
                "body. Authoring is available separately over /bake."
            ),
            "ttlMs": 60000,
            "cacheScope": "public",
        }
    if method == "ping":
        return {"resultType": "complete"}
    if method == "tools/list":
        return {"resultType": "complete", "tools": TOOLS}
    if method == "tools/call":
        session = await get_session()
        try:
            result = await session.tool(params.get("name"), params.get("arguments", {}))
        except ValueError as error:
            raise RpcError(-32602, str(error)) from error
        return {
            "resultType": "complete",
            "content": [{"type": "text", "text": json.dumps(result, separators=(",", ":"))}],
            "structuredContent": result,
            "isError": bool(result.get("error")),
        }
    raise RpcError(-32601, "Method not found; supported protocol: " + VERSION, 404)
