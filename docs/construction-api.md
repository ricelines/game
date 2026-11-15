# Authored construction and resource exchange

The source specifies that world rules instantiate, that measured material mass is charged, and that stakes are authored. It does not specify an institution protocol for every build. Use an approved world rule responding on the invoking body (or another authorized fabricating object). An authored body declaration selects that receiver even when an existing object is selected for dismantling/editing. The rule contains all stock, energy and refund formulas. The native layer validates geometry, authority and the atomic mechanical commit.

Requiring a central constructor would constrain fabrication behind an extra institution; native fabrication formulas would cross the engine/content boundary. The body/world-rule design uses the existing S4 own-state rules. Authors may set their own state as specified. Approval gates native creation and binding replacement, and source/client JSON cannot forge owner, maker or the body flag.

## Read-only metadata

`self:context()` returns a frozen record with script id, installed object, sender and current owner. An immediate action includes its actual caller and selected endpoint. Only immediate player actions include `actor` and their body; only a checked direct body invocation includes the held endpoint. This metadata lends no mechanical or financial authority. Events, watches, timers and background work have no inherited player actor.

`self:measure_model(model, params?, material?)` resolves the actual published asset/model/parameter block. A model is `m:N` or a positive integral id; material is a registered name or id. It returns `{model,params,material,mass_kg,materials=[{material,mass_kg}],bounds={min,max}}`. Mass sums root and child **disjoint** volume times each actual material density. An explicit material overrides the root, while authored per-part defaults remain. Bounds are the measured assembled bounds, not caller claims. No name/description determines a physical value.

`self:measure_instance(handle)` uses its current root/declared parts and actual bindings, preserving per-instance material overrides. The handle must have been acquired for reading. Mechanical asset descriptors (sites, stored-quantity bindings, local part data) come from `self:asset(handle?)`, resolving the effective variant manifest, never copied model defaults. `self:material(material, term)` reads one intrinsic property. These bindings receive only their required read projections/catalog tables.

`self:meter(subject, term, contributor?, supplier?)` reads the actual delivered allocation integral in SI units. Contributor defaults to this installed object; supplier defaults false. It does not bill offered power or choose payees.

## Generic exchanges

```lua
local exchange = {
  terms = { ['base.charge'] = charge_after },
  state = { [material_id] = stock_kg_after },
}
self:exchange(exchange)
self:create(model, params, material, position, rotation, exchange)
self:remove(selected, exchange)
self:replace(selected, model, params, material, exchange)
```

Terms are ordinary writable state values on `self`; flat-state keys retain the distinction between integer and string keys. Values are finite scalars/booleans/handles, exactly the existing S4 forms. The binding captures each prior answer at the call, rejects repeated keys, and records expected-before/desired-after values. No term spelling, energy constant, recipe or material whitelist is native. `exchange` preflights all prior answers and process bounds before writing any value; existing rates/bounds continue from their settled current value and changed facts use the normal watch path. An exchange cannot silently clamp away a resource. Flat-state writes still obey the VM memory budget.

For a structural operation the same exchange is deferred in its typed request. After the handler returns, the single writer verifies the expected answers, actual model/geometry, available native capacity, permitted build space and ordinary object/land rights. It then performs the prepared nonfailing lifecycle/geometry change and commits the exchange without yielding. A failed preflight changes neither resources nor objects. There is no rollback of earlier unrelated handler effects. Multiple requests commit in call order; a later request based on an already-spent balance fails its expected-answer check instead of overdrawing it.

Only actual active approved world-rule dispatch may create or replace bindings. `remove` additionally checks ordinary destroy authority on a foreign selected object; a direct body invocation uses only that bounded selected endpoint, while a scheduled script uses its installed holder. Approved material rules can remove their own subject/parts as their authored physical response. Creation never accepts owner/maker/time/body flags. Immediate player construction uses that authenticated player; script/event construction uses the current installed object's owner and records the script as its cause. Handler-driven creation is not mislabeled autonomous agent invention.

The structural binding queues requests and returns `{queued=true}`. It does not claim an object already exists or expose a temporary handle. The final action result includes real created handles after commit. Applications needing to use a newly created object do so on the next ordinary action. Parameter replacement uses lifecycle's name-preserving topology plan; the law evaluates complete old/new measured material mass before requesting the replacement.

Creation prepares ordinary child bodies as independent kinematic attachments
before checking native capacity. It installs their actual local transforms
after allocating the complete ordinary-handle closure; no temporary aggregate
or spare native body is required. The same `install_declared_attachments`
adapter is available for initial world placement. Placed doors retain an
independently moving leaf rather than automatically becoming a rigid assembly.

Replacement checks Edit authority and inherited ceilings for the selected
existing subtree, immediate physical reach (or the caller's actual own
inventory), prepared resource answers, prospective geometry, native capacity,
and every retirement owner's preflight before changing anything. Edit does not
silently add a second Build requirement. Native reconstruction may encompass
an enclosing held or dynamic assembly, but this does not widen catalog edits or
authority to its unchanged outer members. The physical adapter preserves
ordinary handles, external occupants, held relationships and dynamic velocity;
it supports physical-to-empty and empty-to-physical logical roots.

Retained handles receive the fixed change-mode lifecycle operation, preserving state and
surviving watch answers. Only genuinely new children receive initialization and
start events. Removed children pass through normal owner cleanup before their
slots are released. Every ordinary newly born handle has its own Create record
with its actual model, material, parameters, maker, owner and plot. Model/player
identities use the same `m:N`/`p:N` representation as the rest of the event log,
so child age and attribution remain observable independently of the assembly.

## Content policy

The initial fabrication rule keeps stock in its own flat state keyed by material id and reads the invoking body's charge. Making raw material, placing, dismantling and parameter edits are authored action handlers. `base.make_material` computes embodied-energy cost from the material property and configured pace/efficiency, then uses the generic exchange. Placement spends measured stock. Dismantling credits actual material mass and refunds no energy. Stakes-off activates a different authored rule with no costs; the native path stays identical. Failed resource checks emit authored explanations rather than fake placement.

Tests must run the real VM and structural/lifecycle adapters with actual baked assets: insufficient stock/charge/build rights/native capacity leaves both sides unchanged, mixed-material mass is counted once, stale expected balance rejects the second request, material/topology edits charge the real delta and keep matching part handles, and a renamed rule/material works without an engine edit. Unit exchange tests do not satisfy that integration gate.
