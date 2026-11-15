# Authored seed package and import plan

The package under `content/` is independent of engine libraries. `python -m content.prepare` reads one `start.json`, selects `seeded` or `forage` and stakes on/off, resolves registry names, and bakes actual CAD through BakePool. It produces a JSON import plan for the native launch adapter. It never reads or writes live world tables, approves scripts, pretends a bake succeeded, or implements gameplay in Python. Native launch verifies artifacts and script declarations, assigns IDs/handles and applies ordinary module installation operations before starting the run. Restore bypasses this producer and never repeats initial balances or issuance.

The producer's explicit `--restricted-host-bake` option selects the already implemented namespaces/Landlock/seccomp/rlimit supervisor without a delegated cgroup. Strict default still fails on this host's read-only cgroup mount. Each asset completion and the aggregate plan retain that limitation and actual timing. Neither restricted mode nor successful CAD tests claim the full original sandbox gate.

## Source layout and symbols

`start.json` contains mode membership, plot/wallet/profile data, model/source declarations, script declarations, placements, relations, installed-script grants and bootstrap events. `registry/terms.json` defines ordered terms with name references; `registry/materials.json` contains intrinsic values including the original chosen embodied-MJ/kg table; `registry/mechanical.json` explicitly binds roles. CAD sources live in `models/`, Luau in `scripts/`, and actual-model persona/purpose content in `residents.json`. All café dimensions, resource rates, prices and recipients are content.

Local symbolic references use objects such as `cafe.floor`, `public.circuit`, `resident.mira`, models such as `body`, and scripts such as `body`. They are never native identifiers. The importer resolves these within their typed fields, not by replacing arbitrary strings in Luau/text. Names in action arguments and maker claims remain unchanged. Registry term IDs are the same across both starts/stakes settings; enabling/disabling content does not renumber terms.

## Plan version 1

The native input has this exact outer schema:

```json
{
  "version": 1,
  "source": {"start_sha256":"...","mode":"seeded","stakes":true},
  "sandbox": {"restricted_host":true,"cgroup":{"available":false,"error":"..."}},
  "registry": {"version":1,"terms":[],"materials":[],"rules":[],"mechanical":{}},
  "scripts": [{"key":"body","source":"return {...}","author":1,"world_rule":false}],
  "models": [{"key":"body","asset":"HASH","canonical_params":"{}","material":"base.fabric","script":"body","text":{"name":"...","description":"..."}}],
  "profiles": [{"id":2,"name":"Mira","roles":[],"modes":["play"],"key_file":".runtime/keys/mira.key","body":"resident.mira"}],
  "plots": [{"id":1,"owner":2,"bounds":[-8,2,8,18],"access":{},"settings":{}}],
  "groups": [],
  "balances": [{"holder":"p:2","amount":1000}],
  "world_settings": {},
  "objects": [{"key":"resident.mira","model":"body","owner":2,"plot":1,"at":[0,0,0],"rotation":[0,0,0,1],"flags":["body"],"state":{},"settings":{},"seed":true}],
  "relations": [{"term":"base.powered_by","from":"cafe.lamp","to":"cafe.circuit"}],
  "grants": [],
  "bootstrap": [{"target":"cafe.lamp","event":"base.start","payload":null}],
  "script_state": [],
  "asset_timings": []
}
```

This is a trusted launch/content import, not a new unguarded remote create endpoint. `models.asset` and exact canonical parameter bytes select already supervisor-verified artifacts; native catalog verification remains required. Model/script references are local keys until native registration assigns actual IDs. The actual native validator derives script declarations. `world_rule:true` requests launch-authorized fixture validation/approval; the source cannot self-approve. Model defaults point to the returned script IDs. All created seed records carry seed attribution. Public metadata receives profile name/body/roles as needed, never keys/key_file paths.

Profile credentials use `control::create_key_file` for new mode-0600 files or `hash_key_file` for existing files. The plan contains only paths. Relative paths resolve from the launcher working directory. The Python producer does not open those files. Native PLYR persists hashes; gateway resident definitions refer to those same private paths. A watcher/debug profile has independently permitted connection modes and no body; game roles such as architect/elaborator do not imply modes.

Objects select generic physics with flags and manifest bindings. Static floors, shell, stairs, rails and tank use actual mesh collision. Sliding leaf uses its ordinary moving child asset. Fish are 300 ordinary objects sharing one baked fish asset, related to one controller with `aq.school`, and explicitly grant that installed controller Move on each endpoint. They are not declared CAD parts. Their tiny model handler accepts status only from their actual related controller and owns its health/process state. One controller computes all movement/health policy and invokes the generic `pose_batch`; no engine school or native tank confinement exists.

Grants are declarative inputs to the ordinary permission operations:

```json
{"issuer":4,"holder":{"script":"school","on":"aquarium.controller"},"scope":{"object":"fish.000"},"rights":["move"],"cap":0,"period_s":0,"expires_s":0}
```

Wallet grants use `scope:{"account":"p:N"}` and Spend rights, with explicit caps, periods and expiry; issuance is its separate capped scope. There is one balance per player. The treasury object is not prefunded: its ordinary script needs an explicit Spend grant over its owner’s wallet, and capped issuance credits that player wallet. Plot grants use `scope:{"plot":1}` and appropriate land rights. Ordinary placed/vendor offers remain the existing atomic goods swap; this plan adds no contract/bank engine.

Initial `state`/`settings` maps contain registered scalar values. A value `{"object":"SYMBOL"}` is an explicitly typed handle reference; other strings remain strings. Bootstrap payloads support the actual flat typed event ABI. Initial flat per-script state (for resource stock) is a separate `script_state` array of `{target,script,entries:[{key:INTEGER_OR_STRING,value:SCALAR_OR_TYPED_OBJECT}]}`; numeric and string keys stay distinct. Root owns native conversion, and startup sources never claim player-chosen actions.

Forage keeps authored registry, body/constructive laws, public land and enough renewable public slow charge to prevent stranding; it omits private buildings, useful stocks and pre-existing businesses. Seeded adds the café and stocks. Stakes-off selects different authored rule activation/settings, rather than a native stakes conditional. The selected plan exposes its exact mode/stakes/hash for reproducibility. No purpose string selects a scenario or a prepared resident design.

## Reviewed host needs

Existing actual methods cover state/processes, relation reads, contributions/shares, events/timers/actions, navigation, attachment, grasp/release, impulse, translational sweep, conserved transfer, spatial queries, sound and account operations. Their names/arguments are in the implemented binding references. Handler's second argument is the sender handle; it is not a player ID. Content does not infer identity from it.

Root-approved additions are read-only `context()` for immediate-action metadata; verified `measure_model(model,params?,material?)` returning disjoint per-material masses/bounds; generic `exchange({terms,state})`; approved-rule `create/remove/replace` with optional atomic exchange; generic pose batches with per-endpoint ordinary Move grants; delivered-allocation metering; and plot metadata/authorized transfer. Exact constructor syntax is root's contract, not a seed-native handler.

Final names are `object(handle?)`, `asset(handle?)`, `context()`, `measure_model`, `measure_instance` and `material`, as specified in construction-api.md. `object` includes current pose, material/model/params/owner and actual default/instance script IDs. `asset` returns the effective verified variant manifest. Container pouring reads `bindings.container.stored` and measured capacity. The school rotates local water-space paths using the actual tank pose.

## Geometry and checks

The authored geometry follows content-contract.md: continuous Street/forecourt/floor support, Boolean shell doorway, actual sliding leaf, twenty-rise stair with matching platform void, exposed-edge rails, realistic shell clearance, hollow tank with freeboard and 300 independently addressed fish. A real opaque partition and transmissive glass provide optical tests. Shared local dimensions generate aligned parts; translation/rotation is an installer transform, not a special coordinate recognizer.

Native integration must verify the actual floor/stair/door collision, construction stock/charge conservation, shared power/oxygen thresholds, obstruction, socket charge, served-power payments, rent/return, both starts/stakes settings and restored institutions. Actual-model runs are separate evidence: resident purpose/persona content supplies no CAD, routes, target handles or repair recipes. Useful authored designs must appear in real model output and subsequent world records.

## Native section adapter

`server/bindings/start/start.hpp` contains separate registry, script, asset, model, object, property, relation, profile, land, money, grant, script-state and bootstrap functions. `Symbols` stores only resolved startup identities. Allocation goes through `lifecycle::Instances`; the adapter never receives `Objects`. Native grant controls come from current identity/authority, actual default/instance/applicable world-rule installations, profile roles and owner tables, not plan assertions. Rules remain inactive without a separate trusted-launch `SeedRuleApproval` carrying explicit evidence. Initial import bypasses neither artifact verification nor ordinary grant creation. Restore does not invoke this adapter.

Source record membership uses `starts:["seeded"]` or `starts:["forage"]`; this is distinct from profiles.modes connection permissions. `stakes` selects the two authored fabrication policies. Objects carry explicit maker and ordinary preset verb lists. Grant scopes additionally support `owned_on_plot:{owner,plot}` for owner-authorized custody/salvage. Source-only flat-state `key_term` names resolve to integer term IDs before native import. Source-only initial `{asset_measure:"capacity",scale:0.85}` resolves from the selected real baked variant. Neither descriptor survives as invented world state.
