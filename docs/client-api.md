# Browser spectator contract

This documents the Godot 4.7.2 single-threaded WebGL2 spectator and the small extensions required around original S6. The client never simulates gameplay or sends direct human body/world actions. Geometry comes from real baked immutable GLBs. During HTTP download/import, a translucent blue box marks the pending asset. It uses measured manifest bounds once available; before the manifest, its neutral half-metre marker is explicitly unmeasured. The box follows authoritative transforms, remains selectable, and is removed when the real GLB loads. Failed assets retain this visibly distinct loading proxy and the actual failure status, without claiming it is finished geometry.

## Wire agreement

Use S6 `/ws` text controls and binary cell streams unchanged. Little-endian handle bits are `generation << 32 | slot`, displayed as `o:slot.generation`. Position integers are centimeters in server Z-up coordinates. A normal cell adds `(cell_i*CELL_M, cell_j*CELL_M, 0)`; the global stream sentinel `(32767,32767)` uses absolute coordinates. Delta position components are zigzag varints relative to the last encoded stream position, not rendered/interpolated positions.

Change masks are `1=position`, `2=rotation`, `4=asset+params`, `8=states`, `16=flags`, in that field order. A states field replaces the full declared state map, including removals. Quaternion smallest-three uses the largest component index in low two bits and remaining components at shifts 2/12/22. The largest component is made nonnegative before encoding; others map `[-1/sqrt(2),1/sqrt(2)]` to integer `[0,1023]`. Reconstruct the missing component with the positive square root and normalize. Both peers must agree before transport integration.

CELL_KEYFRAME replaces that stream's local map. CELL_DELTA applies removals, additions and changes in written order; a just-freed local ID may be reused immediately. CELL_DROP removes the stream. A world handle can retain two stream references briefly during migration, so dropping an old cell must not delete its new membership. Decoder tests use labelled binary fixtures, never invented world objects in a production session.

The renderer downloads actual baked LOD0/1/2 GLBs on demand. Distance divided by the asset bounding radius selects LOD2 above 12, LOD1 above 3, otherwise LOD0; selected objects request LOD0. It retains available geometry until the desired LOD finishes and reevaluates camera distance every 0.4 seconds. Mesh batches include the LOD in their key, and empty batches are released.

The client interpolates transforms by the welcome's `repl_delay_ms` behind the received server timeline, using the latest step and monotonic receive time. Native composition supplies that value from `REPL_DELAY_MS`; the 150ms fallback is for explicitly synthetic fixture welcomes that omit it. Static objects do no recurring transform work. Unknown/malformed binary messages fail explicitly and reconnect for fresh keyframes; no invented default state replaces a failed decode.

## Read-only controls and metadata

The existing hello/welcome, inspect, actions and result shapes remain S6. Watch/debug credentials are server configuration. URL parameters may override `ws`, `key`, `mode` and presentation quality; default connection is `/ws` on the serving origin. A launcher may provide `window.ambergris_config` before engine start with key, purpose_token, mode, gateway and ws values; these override URL settings and are never added to public metrics. A `view` update is camera interest, not a body action:

```json
{"t":"view","pos":[1.0,2.0,3.0]}
```

Coordinates are server meters/Z up. Send only after a meaningful camera move or reconnect. Welcome supplies the normal asset root and may add `agent_gateway` for the separate agent-purpose UI.

S6's entity row has an asset hash but no model/material/author identity. Two submissions can share geometry and have different materials/text. Therefore a text metadata side channel is necessary; do not misuse `params` as a model ID:

```json
{
  "t":"catalog",
  "terms":[{"id":1,"name":"author.quantity","kind":"quantity","unit":"J"}],
  "materials":[{"id":2,"name":"author.material","color":[0.6,0.4,0.2,1],"metallic":0,"roughness":0.7}],
  "models":[{"id":3,"asset":"HASH","material":2,"text":{"name":"Maker's description"},"bindings":{}}]
}
```

The catalog sends initial definitions on welcome and changed/new definitions after authoring. Terms resolve bindings and inspector labels, never select gameplay behavior. Material color/metallic/roughness are authored presentation values, not inferred from a material's spelling. Immutable asset manifests supply measured geometry, sites, parts and declared replication/bindings.

```json
{
  "t":"metadata",
  "objects":[{"h":"o:17.1","model":3,"material":2,"owner":"p:2","parent":null,"text":{"name":"A named instance"}}],
  "agents":[{"id":"p:2","name":"A resident","body":"o:17.1","purpose":"An actual purpose","status":"thinking"}]
}
```

Object rows are full replacements for those handles; omitted objects are not deletions. Binary stream removal controls visibility/lifetime. A metadata request `{t:"metadata",id,handles:[...]}` fills a missed/late row. Initial metadata covers subscribed objects; updates follow changes. `text` is maker's claim, displayed separately from measured properties. Metadata carries no instance scale override: authored geometry parameters require a rebake.

## Generic visual bindings

`bindings.face_motion:true` orients the canonical local +Y server-forward axis (glTF −Z) along actual streamed displacement, retaining the last nonzero heading at rest. Rotation suppression never implies a model-name case.

The manifest's optional `bindings` uses registered term names (or resolved IDs), never model-name cases. The client supports:

```json
{
  "render":[
    {"term":"author.state","property":"color","range":[0,1],"colors":[[0.2,0.2,0.2,1],[1,0.7,0.2,1]]},
    {"term":"author.state","property":"emission","scale":2,"offset":0},
    {"term":"author.visible","property":"visibility","scale":1,"offset":0},
    {"term":"author.alpha","property":"opacity","scale":1,"offset":0}
  ],
  "light":{"intensity":"author.candela","site":[0,0,1],"color":[1,0.7,0.3],"range":12},
  "zone":{"ambient":"author.ambient_lux","priority":10,"shape":{"kind":"box","size":[10,8,3],"offset":[0,0,1.5]}}
}
```

Numeric mapping is `value*scale+offset`; an optional `range` normalizes/clamps a color ramp. Visibility is positive/nonpositive. Opacity clamps to `[0,1]`. A binding may contain a numeric constant instead of a term reference. `node` optionally selects a named GLB node; omission targets the whole asset. Actual physical part motion always comes from ordinary child-object transforms. Inspector state rows reflect every declared received term, including terms with no visual binding.

Light and zone bindings provide presentation inputs from the same authored values used by the server. The viewer does not decide authoritative sight or oxygen/power behavior. Ambient samples the baked bounds center, matching the server observer's illuminance sample, and uses the highest containing zone priority, then stable handle order, with an authored fallback. Display calibration maps authored lux to `lux/(lux+exposure_lux)`, with a minimal dark display floor. `exposure_lux` is a renderer setting (default 8 lux), configurable through the URL or injected `window.ambergris_config`; it changes exposure, not simulation light or sight thresholds. Vertex lighting limits fragment cost; exact GPU/CPU photometric equality is unnecessary. No coordinate rectangle names a café or changes light. Opaque geometry can batch by asset/material; per-instance color/emission data preserves individual state without one material per fish/object. Transparent objects use separate sorting as necessary.

## Inspection, history and debug

Inspection uses existing `{t:"inspect",id,target}`; display the S5 measured, maker and rights/action data without giving humans action buttons. Event requests are read-only:

When inspection includes `elaborated:[{id,text,author,created_s,...}]`, the inspector displays a separate **ELABORATED DETAILS** section beside **MAKER'S WORDS** and **MEASURED IN THE WORLD**. These details are shared authored text, without changing any geometry or measured fact. The ordinary `engine.edit_detail` action remains an agent/player tool operation, rather than a direct spectator action.

```json
{"t":"events","id":12,"since":0,"thing":"o:17.1","limit":40,"detail":false}
```

A request can instead include `root:<event-id>` to expand that retained causal chain. Reply `{t:"events",id,events:[S7 records...]}` preserves `cause`, `root`, authors and result. Periodic feeds send `detail:false`, which omits nested observation/measured/source/bytecode/manifest fields after unchanged filtering; explicit chain expansion defaults to full records. Omit `thing` for a recent feed. Debug mode may request `{t:"debug",id,kind:"nav|zones|stats",around:[x,y,z],radius:32}`. Replies contain generic `segments:[[[x,y,z],[x,y,z]],...]`, `triangles:[[[x,y,z],...],...]`, and/or numeric `stats`. These are authoritative diagnostics, not a client navigation substitute. All debug geometry is server Z up. Role checks remain server-side.

The UI offers free orbit/pan/zoom, follow an actual body, object selection, state inspection, recent events, causal-chain expansion and optional bounds/cell/nav overlays. A gateway may later expose `GET /api/agents -> {agents:[...]}` and `POST /api/purpose {agent,purpose}`. That steers a model's actual purpose; it never maps words to an immediate world action. Missing gateway/model service is shown as unavailable rather than claiming an inhabitant acted.

## Verification and performance

The actual delayed HTTP fixture (`python3 client/tests/asset_loading.py assets/HASH`) checks immediate visibility, measured-bound replacement, authoritative transforms, runtime GLB replacement and proxy cleanup. It uses genuine baked artifacts and synthetic entity rows, and is labelled separately from a live-world browser test.

Keep binary decoder tests separate from end-to-end evidence. First validate exact keyframe/delta/drop, local-ID reuse, migration, varints, quaternions and arbitrary states in headless Godot. Then load actual baked GLBs through the real HTTP/WS path, inspect a real object and follow a real agent. Confirm a new term/binding and a newly baked asset work without client source edits. Capture browser screenshots, page errors, startup/download size, memory where available and frame-time distribution.

Default web quality uses Compatibility rendering, no MSAA, restrained shadow/effect cost and reduced internal resolution if needed. Generic batching of repeated geometry is preferred to deleting authoritative objects. Record software-rendering limitations honestly. The browser gate remains smooth play on an ordinary laptop and join within seconds; a synthetic decoder fixture is not evidence that it passed.

Implementation references: Godot's [runtime glTF loading](https://docs.godotengine.org/en/stable/tutorials/io/runtime_file_loading_and_saving.html) and [MultiMesh](https://docs.godotengine.org/en/stable/classes/class_multimesh.html) documentation describe the supported loading and batching paths.

The exported WebGL build also exercises delayed loading in a real Chromium browser:

```sh
AMB_TEST_LABEL=loading-fixture AMB_TEST_ASSET_DELAY_MS=7000 \
  python3 client/tests/run_browser.py
```

This is explicitly a synthetic 225-entity protocol fixture using a real baked GLB, not a world gameplay test. The retained `.cache/client/loading-fixture-report.json` shows all 225 objects rendered as measured proxies during the seven-second GLB delay, then zero remaining proxies and an imported mesh group, with no browser errors. The screenshot is `.cache/client/loading-fixture-loading.png`; follow, inspect, free camera and debug controls also passed. The browser's software SwiftShader renderer measured approximately 100ms median /133.3ms p95 frames in this fixture; this is not an ordinary GPU performance pass. `window.ambergris.loading` exposes neutral/measured proxy counts solely for browser diagnostics. Set `AMB_TEST_URL`, the private watch-key file and an explicit test label to run the same delayed delivery against an actual native world.
