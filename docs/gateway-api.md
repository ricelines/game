# Gateway

The Python gateway connects agents to the native world through authenticated
sessions. The server owns perception, permissions, actions and script validation;
the gateway owns S5 observation compaction, unread events, HTTP/MCP routing and
external CAD jobs. The [Technicals](02-t.0.md) and [Specs S5](04-t.de137kjm03j.md#s5-gateway)
remain the requirements.

## Tools and sessions

MCP and the CLI expose the same five game tools:

```text
look(limit?, events_limit?)
inspect(target, events_limit?)
actions(target?, name?, page?, events_limit?)
do(action, target?, args?, events_limit?)
cancel(events_limit?)
```

HTTP equivalents are `POST /tools/NAME` with the arguments as JSON. Authenticate
with `Authorization: Bearer KEY`. `X-Ambergris-Session` identifies a logical game
session and defaults to `cli`. Each key/session pair has one native `/ws`
connection, authenticated by `{t:"hello",version:1,key,mode:"play",binary:false}`.
Opening another controlling connection for that player replaces the old one.
The gateway derives identity from authentication, never a supplied player ID.

Native requests use `t` for the operation and a positive `id`; replies retain
that ID. LOOK replies use `obs`; the other tool replies use their native result,
inspection or actions envelope. The gateway strips `t`/`id`, preserves the result
fields, and attaches unread `events` oldest first plus `events_omitted`. The
default includes the newest 40 events; callers can request more. `heard` contains
the body's actual spatial speech observations. Closed/replaced connections report
their actual failure rather than generating gameplay.

Native LOOK supplies current full descriptions and body/player action lists.
`Session.compact_observation` compares descriptions without distance/bearing;
unchanged things become `{h,dist_m,bearing}`, and unchanged action lists are
omitted. This cache lives only in the gateway. A fresh resident context clears it
with `forget_observation()` before an ordinary LOOK. Native perception/acquisition
state is separate from this presentation cache.

INSPECT remains exhaustive. It accepts a live object handle, `self`, `inventory`,
`grants`, `plot`, owned `plots`, public `plot:N`, model `m:N` or script `s:N`.
Maker's claims and elaborated detail remain separate from measured facts. Current
permissions still govern actions and private state; inspection grants no writes.

Listed actions include one-line signatures. A known action can be called directly;
`actions(name=...)` optionally returns its description, inputs/units or native
JSON schema, required right and example. Rich action records keep callable `name`
separate from `signature`. The gateway and resident harness impose no schema-read
prerequisite. Results follow S5: `applied`, `started` with intent/ETA information,
or `none` with `no_effect`, `no_handler` or `not_modeled`. Policy denials contain
`denied:{need,on,why,fix}`; bad inputs contain `error:"bad_input"`, `problem`, and a
signature when the action is known. Accepted malformed action requests produce
an actual denied S7 attempt; malformed transport rejected before enqueue does not.

## Pinned MCP transport

`gateway/mcp.py` pins protocol revision **2026-07-28**. `POST /mcp` accepts
one JSON-RPC request and returns JSON. Supported methods are `server/discover`,
`ping`, `tools/list`, and `tools/call`; no independent SSE subscription is offered.

Requests carry matching `MCP-Protocol-Version` and `Mcp-Method` headers, and
`Mcp-Name` for tool calls. Params `_meta` contains the
`io.modelcontextprotocol/` keys `protocolVersion`, `clientInfo`, and
`clientCapabilities`. Accept must include `application/json` and
`text/event-stream`. Unsupported versions/methods receive explicit errors.
Tool results include `resultType:"complete"`, `structuredContent`, matching text
JSON and `isError`; a gameplay denial remains a normal structured result.
Bearer authentication and configured Origin checks apply. MCP transport is
stateless; the explicitly named game session above retains S5 state.

## CAD and script submission

The same bearer key/session authenticates these routes:

| Route | Request | Result |
|---|---|---|
| `POST /bake/models` | `{source,params?,material?,text:{name,description,writing?}}` | HTTP 202 `{job}` |
| `POST /bake/variants` | `{model:"m:N",params}` | HTTP 202 `{job}` |
| `POST /bake/edits` | `{model:"m:N",source}` | HTTP 202 `{job}` |
| `GET /bake/jobs/ID` | Same submitting key/session | Job status/output |
| `POST /bake/scripts` | `{source}` | Native validation result, HTTP 200 or 422 |

Jobs progress through queued/baking/registering to done/failed. CAD execution runs
outside the session mutation lock; observations, ordinary actions and other jobs
can proceed. Only completed registrations enter its ordered native mutation lane.
Successful model jobs return actual model/hash/measurements; variant jobs return
the ready parameter-block ID. Baking creates no placed object or special authority.
See [the bake contract](bake-api.md) for trusted measurement and sandbox requirements.
Strict cgroup isolation is the default; the explicit `--restricted-host-bake`
option reports its reduced isolation. Unavailable baking fails explicitly.

Native `register_model` takes `job`, `asset`, exact `canonical_params`, `material`,
`text`, `manifest`, and optional ready `variants:[{asset,canonical_params,manifest}]`.
`register_variant` takes `model`, `asset`, `canonical_params`, and `manifest`.
Workers load and validate actual artifacts; transported manifests do not establish
measurements or trust. Variant installation compares the verified artifact source
against the model's current source. Reused geometry still permits distinct model
identities. A proposed model and all its automatic variants register together as
one chosen submission and return one S7 `record_id`.

An edit job bakes new defaults and each distinct current explicit parameter set.
Retained keys keep their values, removed keys are dropped, and new keys take new
defaults; invalid retained values fail normally. It returns
`{status:"done",effect:"proposal_ready",model,proposal,record_id,...}`. The actual
model author applies it with ordinary `base.revise_model {model,proposal}`.
Construction uses checked ordinary actions; reactions use their own installed
ScriptHolder grants. Definition editing preserves matching identities/defaults
and checks actual geometry/capacity;
affected offer/trade cancellations remain effects even if geometry is rejected.
[Live authoring](live-authoring-api.md) describes these semantics.

Native registration failures carry their actual `record_id`. If CAD fails before
any registration record exists, the gateway submits authenticated
`authoring_failed {job,operation,source_hash,errors}` through the mutation lane.
It records one denied building attempt without allocating model/script IDs.
An existing receipt prevents duplicate publication. If recording itself fails,
the failed job exposes `record_error`. Script upload uses native compilation and
declaration validation; upload alone neither installs a script nor approves a rule.

CLI equivalents are `amb submit model source.py --wait`,
`amb submit script source.luau`, `amb variant m:N --params JSON --wait`,
`amb edit-model m:N source.py --wait`, and `amb job ID`. Authenticate with
`--key-file`; `--gateway` and `--session` select the endpoint and logical session.

## Residents and purpose controls

`gateway.service --residents content/residents.json` loads authored personas and
purposes. Resident credentials come from private `key_file` paths; public status
and evidence omit keys and paths. The real Responses harness offers the five
game tools plus model/script/variant/edit submission and bake-status functions.
It executes actual model tool calls in order and returns their outputs to that
conversation. Errors never activate a keyword policy or prewritten gameplay plan.

`--resident-report PATH` checkpoints response IDs, token/latency evidence,
observations and complete tool transcripts after responses and tool outputs.
Context resets archive the previous transcript when reporting is enabled and
clear the gateway observation cache for the next context. This evidence is
separate from the native S7 record; historical runs retain their original bugs,
duration and outcomes.

`GET /api/agents` returns public resident identity/body/purpose/status/model.
`POST /api/purpose {agent,purpose}` updates that resident's instruction, cancels
its intent through the ordinary cancel tool, and checkpoints when configured.
These routes require the separately configured watch/purpose bearer credential
(`--purpose-token-file`); they do not expose player keys or execute the human's
world action. Purpose edits live in the running harness, not in the authored
resident file. The spectator can select the gateway with the nonsecret
`?gateway=http://127.0.0.1:8081` URL parameter. `/health` reports whether bake and
resident services are configured.
