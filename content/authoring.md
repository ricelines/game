# Authoring models and scripts

Scripts are Luau files returning declarations and handlers. Models are build123d programs submitted to the bake service. Authoring files needs no body in the world. Placing, using and changing objects happens through ordinary actions and their permissions.

## A script

```lua
return {
    kind = 'model',
    watches = {'example.enabled'},
    contributes = {'example.supply'},
    actions = {['example.toggle'] = {right='use', reach=true}},
    on = {
        ['example.toggle'] = function(self, sender, args)
            self:set('example.enabled', not self:get('example.enabled'))
        end,
        ['example.enabled'] = function(self)
            local recipient = self:related('example.connected_to', 'out')[1]
            if recipient then
                self:contribute(recipient, 'example.supply', self:get('example.enabled') and 4 or 0)
            end
        end,
    },
}
```

These example terms must exist in the registry. A declaration's `kind` is `model`, `instance` or `rule`. `actions` maps action names to their required object right: `use`, `take`, `move`, `edit`, `copy`, `transfer` or `destroy`; optional `reach=true` checks the selected target's reach. `on` maps registered trigger names to functions.

Handlers receive `(self, sender, args, caller)`. For an action, `caller` is `'p:N'` for a player or a readonly `{script = 's:N', on = handle}` table for a script. Events and timers receive nil. This identifies the caller and grants no authority; a handler still uses only its own grants. A selected object's action receives the acting body or script object as sender. A body's action receives its selected target as sender. Events carry their actual sender. `self.handle` is the installed object and `self.script` its current script ID; both are readonly. Handles preserve their generation and compare by identity. Keep them as handles rather than converting them into numbers.

A watch is a name or `{term='example.level', thresholds={{predicate='below',value=2}}}`. Predicates are `above`, `at_least`, `below`, `at_most`. Shared quantities may follow the relation declared by their term. Thresholds schedule boundary crossings rather than polling every step.

The validation worker compiles the file, runs its top level once under a 50 ms limit, and derives the declaration. Literal `set` and `rate` calls declare their state terms automatically; dynamic terms need a `sets` list. Contributions need a `contributes` list. Unknown terms, wrong sources/kinds and unknown rights are rejected. `slots` describes requested installation permissions; declaring a slot does not grant it.

A rule also needs `applies_to={material='example.material'}` or `{flag='example.flag'}` and nonempty fixture tests. Tests use `given`, `when`, `expect`, because `then` is a Luau keyword. Elaborator approval and activation control where a rule runs. They give it no extra mutation, construction or financial authority.

## Values and reactions

| Call | Result or effect |
|---|---|
| `self:get(term)` | Own quantity/flag from its intrinsic, measured, state, setting or combined source; nil when absent. |
| `self:setting(term)` | Own setting with instance → model → plot → world fallback. Zero and false are real overrides. |
| `self:set(term, value, part?)` | Write declared, non-shared state on self or an actual declared part. Setting a quantity stops its old rate. |
| `self:rate(term, per_second, lo, hi, part?)` | Fold current state, then set its steady rate and bounds. |
| `self:related(relation, 'out' or 'in')` | Current handles at the relation's other end. |
| `self:contribute(subject, term, amount)` | Replace self's contribution to a declared combined term on self or a related subject. Zero can withdraw an old contribution after disconnection. |
| `self:get_share(subject, demand_term)` | Compute self's allocation from current contributions/settings. Earlier writes in this handler are visible. |
| `self:emit(target, event, payload?)` | Queue an event to self, a declared part, a related endpoint or a handle from the current event. Finding an unrelated object with a query does not permit emitting to it. |
| `self:after(seconds, event, payload?)` | Send an event to self later. Zero means the next step. |
| `self:act(action, target?, args?)` | Perform an ordinary checked action synchronously and return its real result. Use nil for targetless actions. |
| `self:parts()` | Actual declared part handles. |
| `self:now()` / `self:random()` | World seconds / deterministic per-object random number. |

Values use SI units. Term kinds are `quantity`, `flag`, `event`, `relation`, `action`, `material`; scalar sources are `intrinsic`, `measured`, `state`, `setting`. Shared combiners are `sum`, `max`, `any`, `share_equal`, `share_priority`. A demand term names its paired supply and optional priority setting. Contributions belong to their contributors: withdraw on disconnection; destroying a contributor removes its contributions.

`self.state` persists a flat mapping of integer/string keys to numbers, booleans, strings and handles. Nil deletes an entry. Tables/functions cannot be stored there. Generic iteration and `#self.state` work; use ordinary local tables for scratch work. State changes are immediate and survive errors/cutoffs. Reading a handle from state does not acquire it for the current run.

Events accept a scalar, handle, string, vector or flat record of at most 64 named fields. Actions use their registered inputs, including structured native records. Luau vectors and handles are converted at that boundary; returned `created` entries are actual typed handles. Quantity reads address this object. Related objects communicate their state through authored events or contributions; physical queries return their documented measurements. Knowing a printed name grants nothing.

## Ordinary actions and authority

`self:act` always acts as this script installed on this object. It never borrows the player who triggered a handler. It needs its own grants for other objects, land, accounts and avatars. Direct state writes stay limited to self and declared parts. Timers have the same authority as other script runs.

```lua
local result = self:act('engine.create', nil, {
    model='m:42', position=vector.create(3, 1, 0),
})
if result.ok then
    self.state.created = result.created[1]
    self.state.tokens = (self.state.tokens or 0) - 1
end
```

The action has finished before the next Lua statement. There is no queued construction request or transaction around the handler. Check `result.ok` before stock/charge bookkeeping. A later error does not undo a completed action or earlier writes. If an action retires this script's state, its suspended handler ends instead of continuing through a deleted row.

Results include `effect='applied'`, `effect='started'`, or `effect='none'` with `no_effect`, `no_handler` or `not_modeled`. Denials contain `need`, `on`, `why`, `fix`. Bad inputs carry a signature and specific problem. An undeclared action is still tried through ordinary Use/reach checks and world-rule dispatch.

The starting native operations include:

| Action | Inputs and purpose |
|---|---|
| `engine.create` | `{model, params?, material?, position, rotation?}`; destination Build. Returns actual `created` handles. |
| `engine.reconfigure` | Target plus `{model, params?, material?}`; Edit and reach. |
| `engine.destroy` | Target, `{}`; Destroy, including authorized hidden inventory. |
| `engine.grasp` | Item plus `{local_pose?}`; Take and reach. |
| `engine.attach` | Moving object plus `{parent, local_pose?}`; Move on subject, Use/reach on parent. |
| `engine.release` | Attached object plus `{linear?, angular?}`; relinquish a current held item or use its motion authority. |
| `engine.impulse` | Object plus `{momentum, point}`; real physics under Move/reach. Foreign avatar animation needs its separate Avatar grant. |
| `engine.transfer` | Source plus `{to, term, amount, to_max?}`; conserved quantity, source Move and recipient Use. Availability and bounds cap the transfer; returns actual `amount`. |
| `engine.hold_goods` | Goods root, `{}`; Transfer on it and its parts; stores it for its existing owner. |

Native action poses use `{position={x,y,z}, rotation={x,y,z,w}}`, with both inner values arrays. Luau vectors also work for positions/velocities. `local_pose` is relative to its parent. Other action families cover scoped grants/revocation, money, ownership/trade, installation and elaboration. Their signatures appear in ordinary action listings; a known action needs no extra schema-discovery step.

## Physical primitives and exact reads

Body actions are authored Lua like other behavior. These primitives operate on ordinary physical objects:

| Call | Behavior |
|---|---|
| `self:navigate(point_or_handle, {speed_m_s=...}?)` | Start own movement, bounded by its mechanical profile. Returns `effect='started'` and intent ID. |
| `self:cancel_motion()` | Cancel own movement. |
| `self:sweep(self_or_part, translation)` | Collision-checked translation; returns actual position, status and obstruction. |
| `self:move_part(part, position, rotation?)` | Own independent part's world pose. Rotation uses `{x=...,y=...,z=...,w=...}`. |
| `self:move_part(rows)` | Batch `{object=part,position=vector,rotation?...}` rows. This enables no foreign motion. |
| `self:sound(payload, radius, local_site?, {kind='speech'}?)` | Spatial sound from self. Explicit speech supplies public speaker metadata; sound alone is not a stored-world change. |
| `world.near(self, radius)` | Exact nearby handles in `objects`. |
| `world.raycast(from, to)` | Exact first hit with object, position and fraction when present. |

The VM interrupt limits handler execution. Path and perception queries retain their configured count limits; native queries do not create a separate wall-clock budget.

Readonly helpers are `self:object(handle?)` (model/material/params/owner/parent, position, quaternion array, installed scripts), `self:asset(handle?)` (immutable authored metadata and bake references, excluding geometry measurements), `self:material(material, term)`, `self:measure_model(model, params?, material?)`, `self:measure_instance(handle?)`. Measurements include disjoint material masses and `volume_m3`, the summed solid volume of the root and declared parts, distinct from container capacity; model measurements also expose local support faces as `supports`; instance measurements include declared parts, expose measured `capacity_m3`, and require a live handle acquired in the current run. `self:meter(subject, demand_term, contributor, supplier?)`, `self:plot(plot_id?)`, `self:plot_objects(plot_id)` supply settlement facts. These reads grant no rights. Poses are available as `self:object(handle).position` and `.rotation`.

## Assets, definition edits and limits

CAD programs export `build(params)` and optional `PARAMS`, `MATERIAL`, `SCRIPT`, `SITES`, `PARTS`, `REPLICATED`, `BINDINGS`. Geometry uses Z-up meters and base Z=0. A default asset has `params=0`; nonzero parameter-block IDs belong to that model family, and are not model IDs. Named moving parts are ordinary objects with their own handles. Collision and visible meshes come from real baked geometry.

A variant registers baked parameters without changing placed objects. Apply it through an ordinary reconfiguration action. A model author can submit a baked proposal and call `base.revise_model` with `{model,proposal}`; its instances switch while keeping ownership, poses, matching part handles and instance state. This is an author operation, without a stock exchange or player authority forwarded through a rule.

A handler has a 5 ms protection deadline and a 4 MiB script memory limit. Nested actions keep the caller's running deadline. Earlier changes survive an error/cutoff, which is recorded. Only exchanges of valuables are all or nothing. Globals and captured tables are readonly; captured scalar mutation is rejected. There are no files, network, dynamic source loading or outside clocks. Scripts cannot create/resume coroutines; ordinary `act` suspension stays on the active handler call stack and is never saved in the world.

The physical primitives `engine.grasp`, `engine.release`, `engine.attach`,
`engine.impulse` and `engine.transfer` are called only by body scripts. Players
use their body's authored verbs, such as `base.hold`, `base.push` and `base.pour`.
Other native actions, including `engine.give`, `engine.offer` and `engine.grant`,
remain available to players. Physical primitives still check the script's own
grants; they do not inherit the triggering player's authority.
