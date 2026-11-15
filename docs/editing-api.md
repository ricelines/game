# Live editing actions

`bindings/editing` supplies ordinary native player administration through the existing `do` tool. It contains no physical body verbs or institution policy. `declare_editing(Terms&,NativeActions&)` appends the reserved `engine.*` action terms and their truthful structured input schemas **before** the registry is pinned and initial scripts are validated. `register_actions(NativeOperations&, Declarations, ScriptEditing&, TextEditing&, PropertyEditing&, RegistryEditing&)` registers all twelve operations over four editing capabilities. Validation and handlers are private to the module; the composition supplies the four contexts once. No context combines script, text, property and registry mutation.

The twelve operations are:

| Action | Arguments | Current authority |
| --- | --- | --- |
| `engine.install` | `script:"s:N", choices?:[...]` | Edit on target; RunScripts on its current plot when committing |
| `engine.uninstall` | `script:"s:N"` | Edit on target |
| `engine.model_script` | `model:"m:N", script:"s:N" or null` | Actual model author |
| `engine.edit_text` | `text:{name?,description?,writing?}` | Edit on target |
| `engine.edit_detail` | `id:positive numeric text ID, text:string` | Edit on target; detail must belong to this exact live target |
| `engine.model_text` | `model:"m:N", text:{...}` | Actual model author |
| `engine.set_setting` | `values:{registered_term:scalar or null,...}` | Edit on target |
| `engine.model_setting` | `model:"m:N", values:{...}` | Actual model author |
| `engine.patch_state` | `values:{registered_term:scalar,...}` | Architect/elaborator role **and** Edit on target |
| `engine.relate` | `term:name or ID, other:full_handle, present:boolean` | Edit on outgoing source target; destination must be live |
| `engine.define_term` | `proposal:{name,kind,...}, choice?:{same_as or none_fits}` | Architect/elaborator role |
| `engine.define_material` | `material:name or ID, values:{intrinsic_term:scalar,...}` | Architect/elaborator role |

Model operations identify their model in arguments; the control envelope's optional target remains an ordinary object handle. Model-default changes update current instances on this rare edit path and apply to future instances. They do not rewrite explicit instance bindings or override instance settings/text. There is no model-author or architect bypass of object permissions for instance edits.

The state patch is specifically the Technicals §12 elaborator repair path: “An elaborator may also patch world state ... through the ordinary edit actions and permissions.” Ordinary builders choose material, measured shape, settings and scripts; they cannot write intrinsic, measured or state values through settings. A repair accepts only non-shared state quantities/flags and uses the existing setter, including stopping its current rate process and emitting ordinary changed facts. It adds no global source/sink permission layer for authored state.

Text edits merge the supplied fields into the currently effective text. Empty strings intentionally clear a field. Model/instance claims remain separate from actual measurements and have no mechanical effect. Setting `null` removes an override and exposes the normal instance → model → plot → world precedence. Flags require booleans; quantities require finite numbers. All fields are validated before a property batch mutates.

The `TextEditing` capability owns instance claims, model defaults and elaborated details. Its detail rewrite implements the Technicals §12 ordinary rewrite path. It changes only the selected public detail's text, preserves its original author and creation time, and invalidates the object's observation/presentation cache. The normal completed native action records the editor and rewrite arguments. Refusal status entries are not details and cannot be rewritten through this action. Detail text remains separate from maker claims and measured facts.

Relations edit only the source's outgoing authored connection. The context receives the four actual configured Space-owned relation IDs (contains/supports/attached_to/in_zone); those cannot be forged. A newly used authored relation updates `RelationsRead` immediately. Both actual endpoints emit changed facts, allowing existing along-watch rebinding and allocation reconciliation to work. Destination ownership is not an invented extra gate.

## Installing behavior and slot grants

Calling `engine.install` without `choices` returns slot names and neutral suggested object scope/cap/expiry fields without installing. Calling again with `choices:[]` explicitly installs without optional grants. A choice is:

```json
{"slot":"move","scope":{"object":"o:17.1"},"rights":["move"],"cap":0,"period_s":0,"expires_s":3600}
```

Other ordinary scopes are `{owned_on_plot:{owner:"p:N",plot:N}}`, `{account:"p:N"}`, `{avatar:"o:S.G"}`, `{plot:N}` and `{issuance:true}`. Object rights are use/take/move/edit/copy/transfer/destroy; accounts use spend; issuance uses issue; avatars use animate/camera; plots use build/enter/run_scripts/transfer. Monetary scopes require a positive period and bounded cap under the existing permission module. Every script grant expires. A slot is a declared right name and its choice grants exactly that right. The authenticated player is always issuer; scope controllers, next-owner ceilings and actual installed holder are resolved from live tables. Issuance requires the actual architect role. There is no wallet or object-owner authority inherited by a script.

Choices undergo normal `perm::create_grant` checks. Installation can succeed with unfilled optional slots; the reply explicitly lists each missing/denied slot and reason plus each granted ID. It never claims that a denied choice succeeded. Successful earlier grants are retained if a later slot is denied, matching ordinary non-valuable edit semantics. No money moves during installation.

Installation projects actual action/event indexes, binds watches against current values, and emits the configured startup event only if the new script handles it. The internal trailing `EventWork.recipient_script` routes this event only to that actual newly installed script; zero keeps ordinary broadcast semantics. This field survives queue/timer snapshots and is never accepted from an untrusted S5 request. A stale receiver or removed installation discards the targeted event without replaying another script’s startup. Removing the last reference to a script removes its watches, flat state and ordinary holder grants. Default and explicit references to the same script deduplicate; removing one reference preserves the remaining behavior. Contributions are authoritative per object+term: removing a script withdraws its declared term only when no remaining installed script declares that term. There is no invented per-script contribution provenance.

`ScriptEditing` updates instance installations and model-default indexes together, compares actual installed script sets, and holds the permission/grant tables needed for explicit slot requests. `PropertyEditing` owns instance/model settings, ordinary role-gated state repair and authored relations; it has no text or script-installation writers. `TextEditing` owns the three public text tables and reads current edit/model-author authority. All four capabilities retain the actual `MetadataChanges` backlog for direct presentation/cache invalidation. `RegistryCommit` remains one narrow immutable-registry publication operation, including its native material preflight and failure result. Native completions return ordinary effects for the composition's existing watch, selector and allocation reconciliation; invalidation does not run gameplay.

## Structured native discovery

`command::NativeDeclaration.inputs_schema` is a JSON Schema record used by native actions. `actions()` returns it and its example. This is distinct from authored Luau's flat typed input atoms; native controls already use a separate raw-JSON validator. Encoding grant choices into a JSON string would avoid the metadata extension but burden every agent with an artificial second serialization step. Structured descriptors keep the five-tool surface and accurately describe the accepted records.

## Registry edits

New term proposals take a short name. The server prefixes the authenticated player’s stable `pN` namespace; a proposal cannot choose `base` or `engine`, owner, ID or comparison history. Calling without a choice returns the exact S3 duplicate set: same kind, dimensions, source and combiner. An explicit same_as returns the existing term; a nonempty none_fits creates a new append-only ID and stores the comparison and distinction. Along/paired/priority and action-unit references resolve against real registered terms. Sharing requires an actual compatible combined supply and one demand per supply; priority names a quantity setting. Material definitions accept only intrinsic quantities/flags, never measured geometry or ordinary state.

`RegistryEditing` owns Terms and Materials with read-only ValueRead/Profiles, `RegistryCommit` and cache invalidation. It cannot mutate object properties, installations, text, money or geometry. The publisher writes the entire canonical registry to its new immutable hash before updating its current pin/derived indexes, then returns that actual hash. A publication failure restores the pending term/material edit. The composition rebuilds allocation term indexes after a new sharing declaration and refreshes actual affected material physics/presentation mirrors. Material edits emit changed intrinsic and dependent mass facts for current instances, preserving normal watch behavior. Source editing grants no authority to approve uploaded world rules.

`RightsDeclaration.slot_names` adds the wallet/land/avatar/issuance slot vocabulary separately from the seven object-action rights. A script can ask for spend in a slot; it cannot use spend as an object action’s permission gate.
