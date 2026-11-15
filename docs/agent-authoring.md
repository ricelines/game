# Authoring reference for Ambergris prototype agents

This reference describes the implemented catalog and reaction ABI. Read the world's current action declarations, registry and model metadata before acting. Terms and IDs below are schema examples; they do not name a prepared scene or provide an inhabitant policy. Native physical/financial adapters advertise their additional methods separately. Do not invent a host call when that adapter is unavailable.

## Source files and verified declarations

A script is Luau source returning one table. `kind` is `model`, `instance` or `rule`. Keys in `on` are existing fully qualified term names and values are functions. Watches name quantity/flag/relation terms; action declarations name action terms and one of `use`, `take`, `move`, `edit`, `copy`, `transfer`, `destroy` as their right. Event handlers use registered event terms as `on` keys.

```lua
return {
  kind = 'model',
  watches = {},
  contributes = {},
  sets = {},
  slots = {},
  actions = {},
  on = {},
  tests = {},
}
```

`actions` is a map: `['author.action'] = {right='use',right_on='selected',reach=false,mechanics={}}`. `right_on` is `selected` or `self` and chooses the declared permission/reach endpoint. Body-first dispatch still runs code on its actual installed body; it never runs that code as the selected foreign object. Supported mechanics are `navigation`, `attachment`, `grasp`, `release`, `impulse`, `sweep`, `sound`, `transfer`, `avatar_control`. These declarations do not grant an initiating player's authority to timers or ordinary script actions. Its `on['author.action']` function is called as `(self, actor, args)`. A watch/event handler has the same call shape; args is the typed payload. Handles are tagged values, preserve their full generation and compare by identity. `self.handle` is the installed object's handle. Do not parse handles into Lua numbers.

A watch may be a simple string or `{term='author.quantity', along='author.relation', thresholds={{predicate='below',value=2}}}`. `along` is optional. Supported predicates are `above`, `at_least`, `below`, `at_most`; the value uses that term's SI units. Threshold watches schedule actual boundary crossings rather than repeatedly polling every step. Relation-watch deliveries identify the other endpoint, whether added/removed and the edge direction.

The validation worker executes the top level once in a fresh sandbox, with a 50 ms/4 MiB budget, and derives `script.json` from that returned table. The caller cannot submit its own verified declaration. Literal `self:set('name',...)` and `self:rate('name',...)` calls automatically add those terms to the verified `sets`; dynamically selected terms require an explicit `sets` list. Contributions must be explicitly listed in `contributes`. Unknown terms, incorrect state/contribution sources, unknown rights or watched terms without handlers fail validation. Slots name requested rights: the seven object verbs plus spend, build, enter, run_scripts, animate, camera and issue. These additional scope rights cannot be used as object-action permission gates. Installing a script gives it no owner-wallet authority; every spend needs an ordinary explicit capped grant over a player wallet.

A rule additionally needs `applies_to={material='author.material'}` or `{flag='author.flag'}` and at least one fixture test. Tests are data with `given`, `when` and `expect`; `then` is a Luau keyword. Fixtures and architect/elaborator approval are separate from source compilation. Source cannot approve or activate itself. Only active approved registry world-rule dispatch obtains construction authority; attaching a rule file as an ordinary model/instance script does not confer it.

## Implemented pure host methods

| Method | Behavior |
|---|---|
| `self:get(term, target?)` | Read current intrinsic/measured/state/setting/shared value. Optional target must be an acquired handle. Absent values return nil. |
| `self:read(handle, term)` | Read a quantity/flag on an acquired foreign handle through the same source resolver. This grants no mutation authority. |
| `self:setting(term, target?)` | Read a setting using instance → model → plot → world fallback. Explicit zero/false overrides defaults. |
| `self:set(term, value, part?)` | Set declared non-shared state on self, or a declared own-part handle. Cannot write intrinsic, baked/runtime measured or shared terms. Stops that quantity's prior process. |
| `self:rate(term, per_second, lo, hi, part?)` | Set a declared state's steady rate and bounds; folds its current value first. Rates and thresholds use SI units/world seconds. |
| `self:related(relation, 'out' or 'in')` | Return current other-end handles and acquire them for reads/events. |
| `self:contribute(subject, term, amount)` | Replace self's contribution to a declared combined term. Subject must be self or currently related. Amount zero withdraws a previous contribution even after disconnection. |
| `self:get_share(subject, demand_term)` | Read self's current allocation under the paired share rule. |
| `self:emit(target, event, payload)` | Queue an immediate causal event to self, a part or an acquired handle. It runs after this handler returns. |
| `self:after(seconds, event, payload)` | Send an event to self after the delay; zero means next step, never same-handler reentry. |
| `self:act(action, target, args)` | Queue an ordinary action as this script installed on this object. It uses the script's grants, never the triggering player's wallet or physical scope. |
| `self:parts()` | Return the model's ordinary child-object handles. Physical part motion requires the advertised narrow spatial adapter. |
| `self:now()` | World seconds. There is no other clock. |
| `self:random()` | Deterministic per-object generator, persisted through snapshots. There is no global math.random. |

The composition root registers these methods with their narrow table bindings. If a deployment has not installed a method's adapter, a missing method is an availability error, not permission to assume it worked. Events carry a scalar/handle, string, vector, or flat record with at most 64 string-named fields; nested/cyclic tables are rejected. Actions use their registered typed inputs, validated at dispatch.

`self.state` is persistent flat storage, indexed by strings or integers. Integer `1` and string `'1'` differ. Values are numbers, booleans, strings and handles; nil deletes. Tables/functions/buffers/threads are rejected. Generic `for key,value in self.state do` and `#self.state` for the contiguous integer prefix are supported. `table.*` functions are for ordinary scratch tables, not this proxy. Changes take effect immediately and survive an error or 5 ms handler cutoff. Nothing rolls back. Emitted work runs afterward with the completed handler's causal record.

Top-level globals and captured tables are immutable. Assigning captured scalar locals is rejected at compile validation. Use self.state for persistence and ordinary local scratch tables during a handler. There are no files, network, dynamic source loading, coroutine suspension or external clocks.

## Registry and values

Term fields are `id`, `name`, `kind`, `source`, `dimensions` (eight signed exponents for m, kg, s, A, K, mol, cd, custom), `conserved`, `combine`, `along`, `paired`, `priority`, owner/description and action `inputs`. Kinds are `quantity`, `flag`, `event`, `relation`, `action`, `material`; scalar sources are `intrinsic`, `measured`, `state`, `setting`. Combiners are `none`, `sum`, `max`, `any`, `share_equal`, `share_priority`. Input types are `number`, `integer`, `boolean`, `handle`, `vector`, `string`, `term`; each input has name/type, optional quantity `term` and `optional`.

Term IDs append and never get reused. A duplicate proposal must explicitly select an equivalent existing term or explain why none fits. Source classes select authoritative storage. Material intrinsic values come from the registry; geometric volume/capacity come from verified baked assets; mass is solid volume × current material density. Maker text or self.state cannot override those measurements. Runtime measured facts such as illuminance are provider-only. Shared quantities use contributor parts; read their subject entry or the first outgoing `along` subject's entry. Share meters count delivered allocation, including zero delivery when demand is absent.

A `share_equal` or `share_priority` demand term names its supply term in `paired`; priority sharing reads the demander’s declared `priority` setting, with higher values first. Explicit demand watches notice both the total demand and this object's own allocation. Their payload and ordinary reads remain the total; read the allocation with `get_share`. Aggregate thresholds do not hide an own-allocation change. Reconciliation happens after a handler returns, so a contribution followed immediately by `get_share` in that same handler sees the previous completed allocation; the subsequent demand watch sees the new one. Withdrawn live contributors keep stopped meter integrals and continue them on rejoining. Destruction removes incident meters after any authored settlement.

## Geometry, parameters and construction

A CAD source exports `build(params)` and optional `PARAMS`, `MATERIAL`, `SCRIPT`, `SITES`, `PARTS`, `REPLICATED`, `BINDINGS`. It uses real build123d shapes in server Z-up meters, base Z=0 and XY centered. Parameters are `name: (default, minimum, maximum)` finite numbers. See [bake-api.md](bake-api.md) for actual geometry helpers, part selection and manifest schema. A new source/parameter combination bakes a real immutable asset; client-only size overrides do not exist.

Geometry hash identity includes source, canonical resolved params and bake version. Every submission still gets its own model ID, text, material, script and author. Instance `params=0` chooses that model's default asset; nonzero params identify a global parameter block belonging to that model. Parameter blocks may have different named-part sets and logical zero-volume roots. Their child models remain ordinary authored identities. Do not use a model ID as a parameter ID.

Variant submission bakes new parameters using a model's current inspectable
source and returns a parameter block for that same model. Registration changes
no placed instance. Apply the returned block through an ordinary discovered
construction/reconfiguration action. If the source changed while baking, inspect
and submit again; a block from an unrelated source is rejected.

A definition edit stages a separately attributed baked revision model, including
the parameter sets currently in use. Applying it is an ordinary authored action
by the original model's author. The starting action is `base.revise_model` with
`model`, `revision_model` and `expected_source_hash`. It updates all instances
referencing the definition, including other owners' instances. The authored law
settles their aggregate material difference against the editor's stock. A failed
preflight leaves the live definition, objects and stock unchanged. The edit
preserves existing ownership, poses, matching named-part handles, material
overrides and separately edited script/text/setting defaults. Another source edit
or a newly used unbaked parameter set requires a fresh proposal.

Approved rules can use `self:measure_model_change(model, revision_model)` to read
`instances`, `before.materials` and `after.materials` (each a list of
`{material,mass_kg}`), and `self:revise_model(model, revision_model,
expected_source_hash, exchange?)` to queue the corresponding structural change.
Measurements use actual disjoint volumes and retained material overrides. These
methods expose no foreign state writer or owner impersonation. Creation,
replacement and revision share the approved-world-rule gate.

Construction and mass/material-changing binding edits use authored world-rule actions, discovered like other actions. The initial body declaration routes these actions to its approved fabrication rule. That rule checks actual measured mass and its own stock/charge policy, then requests a generic resource exchange plus structural commit. Native preflight checks geometry and plot authority before either side changes. Ordinary instance/model scripts cannot call unconditional instantiate. Stakes-off is a different authored rule using the same native path. See `construction-api.md` for the concrete mechanical surface; the prototype adds no separate mandatory constructor institution or own-state anti-cheat layer.

Use `inspect` target `plots` to read your owned land and its actual bounds, or `plot:N` for a public descriptor by ID; `plot` reads your current land. Object inspection includes `script_states` for its owner or a player with current Edit. These are actual flat state entries for installed/default/applicable-rule scripts, with typed values and integer/string keys preserved. An authored constructor may store material quantities under numeric material IDs there; read its actual source and measurements rather than assuming a recipe or a stock layout.

Structural commands run after the VM returns. A script cannot inspect a not-yet-created handle during that same handler. The direct action can return its created handle after the same-input drain; later events can refer to it. State debits and earlier writes remain after errors/cutoffs. There are no reservation tickets, cancellation hooks or transactions around a reaction chain. Valuables exchanges use the separately advertised atomic commerce operations.


## Live elaboration and valuables

Reserved `engine.*` actions are discoverable through the same `actions` tool as authored actions. Every listing includes a one-line signature with input types, optional `?` markers and registered SI units, so ordinary discovery does not require reading a schema. Rich action records preserve `name` as the exact callable action name and `signature` as its readable argument list; native records additionally expose `inputs_schema` for structured arguments. A known action's bad-input reply includes its signature and a specific problem. `engine.request_model{note}` refers to your actual last not-modeled result. Elaborators use `engine.requests{page?,page_size?}`, `engine.claim{request}`, and `engine.answer{request,kind,...}`. A rule answer supplies `kind='rule'` and the registered immutable `script='s:N'`, optionally a `subjects` map of aliases to complete captured handles. Tests come only from that script's verified declaration; caller-supplied tests and replacement `target`/`body` aliases are rejected. The original target/requester body are supplied by the server.

A rule answer keeps that session's mutation lane waiting while independent world steps and other sessions continue. The server runs every verified test in real captured worlds with and without the candidate. Every candidate test must pass, at least one baseline comparison must fail, and neither run may fail at runtime. Approval also rechecks the current elaborator role and claim. `kind='patch'` records a note describing already authorized changes; `kind='detail'` appends attributed target text. `engine.refuse{request,text}` closes with attributed refusal text. `engine.rule_activation{script,active}` enables/disables an already approved version; it cannot approve untested source.

Money has exactly one integer balance per player. Installed scripts have no implicit access to their owner's wallet; payment/issuance require the corresponding ordinary scope grant. Ordinary native commerce actions cover pay, balance/inventory, complete-item give/offer/accept/cancel, two-party trade and `engine.place_existing` for previously owned inventory. Read their exact declarations before calling them. Existing placement requires the caller's current Move and destination Build authority and does not fabricate or charge for the same goods twice. Offers retain their actual seller as income recipient. Items given to an absent player wait in that player's bounded mailbox and enter hidden inventory on authenticated login, keeping their original identities/authorship.
