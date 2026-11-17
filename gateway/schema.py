"""The five S5 tools share one schema in CLI and MCP."""


def object_schema(properties=None, required=()):
    return {
        "type": "object",
        "properties": properties or {},
        "required": list(required),
        "additionalProperties": False,
    }


def _tool(name, description, properties=None, required=()):
    fields = dict(properties or {})
    fields["events_limit"] = {"type": "integer", "minimum": 1}
    return {
        "name": name,
        "description": description,
        "inputSchema": object_schema(fields, required),
    }


TOOLS = [
    _tool(
        "look",
        "Observe what your body can currently perceive. Unchanged things may be abbreviated.",
        {"limit": {"type": "integer", "minimum": 1}},
    ),
    _tool(
        "inspect",
        (
            "Read one thing's full measured record, maker's claims, rights and state; or "
            "self/inventory/grants/plot. Land lists lots anywhere, in plot-ID order, with "
            "optional owner (self or p:N), for_sale, cursor and limit filters. Use the returned "
            "next_cursor to continue; inspect plot:N for one lot."
        ),
        {
            "target": {"type": "string"},
            "owner": {"type": "string"},
            "for_sale": {"type": "boolean"},
            "cursor": {"type": "integer", "minimum": 0},
            "limit": {"type": "integer", "minimum": 1},
        },
        ["target"],
    ),
    _tool(
        "actions",
        (
            "Optional help: list available actions or read one action's description, inputs, "
            "right and example. Target land or plot:N for land commands. Listed one-line "
            "signatures can be called directly with do without opening an action space."
        ),
        {
            "target": {"type": "string"},
            "name": {"type": "string"},
            "page": {"type": "integer", "minimum": 0},
        },
    ),
    _tool(
        "do",
        (
            "Request an action through your player/body. The world checks rights and reports "
            "applied, started, no effect or denial."
        ),
        {"action": {"type": "string"}, "target": {"type": "string"}, "args": {"type": "object"}},
        ["action"],
    ),
    _tool("cancel", "Cancel your current body intent, such as move_to."),
]
BY_NAME = {tool["name"]: tool for tool in TOOLS}


def validate(name, arguments):
    if name not in BY_NAME:
        raise ValueError(f"Unknown tool: {name}")
    if not isinstance(arguments, dict):
        raise ValueError("Tool arguments must be an object")
    schema = BY_NAME[name]["inputSchema"]
    for required in schema["required"]:
        if required not in arguments:
            raise ValueError(f"{required} is required")
    for field, value in arguments.items():
        declaration = schema["properties"].get(field)
        if declaration is None:
            raise ValueError(f"Unknown argument: {field}")
        kind = declaration["type"]
        if kind == "string" and not isinstance(value, str):
            raise ValueError(f"{field} must be a string")
        if kind == "object" and not isinstance(value, dict):
            raise ValueError(f"{field} must be an object")
        if kind == "boolean" and not isinstance(value, bool):
            raise ValueError(f"{field} must be a boolean")
        if kind == "integer" and (
            isinstance(value, bool)
            or not isinstance(value, int)
            or value < declaration.get("minimum", 0)
        ):
            raise ValueError(f"{field} must be an integer >= {declaration.get('minimum', 0)}")
