# Catalog and script-validation contract

Status: reviewed contract implemented by `server/catalog/`; concrete signatures are in `catalog.hpp` and `snapshot.hpp`. Catalog owns immutable authoring metadata and imports. It does not own object lifecycle, execute a world action, mutate authority, enter the VM, instantiate physics or send network traffic. It links store/react plain types, JSON and SHA-256 only. The separately built script-validation executable links Luau.

## Independent tables

| Table | Authoritative rows |
|---|---|
| `Assets` | Hash → verified immutable geometry metadata, bounds/volume/capacity/supports/sites/part references, replication/bindings and canonical resolved parameter values. No player author, owner or treasury. |
| `Models` | Monotonic model ID → default asset hash, original source hash, default material, author, created world time, script ID and child-model IDs by authored part name. Two equal-geometry submissions always create two model rows. |
| `ModelTexts` / `InstanceTexts` | Separate model-ID and full-handle name/description/writing maps. Text cannot contribute an authoritative numeric fact. |
| `ParameterBlocks` | Monotonic global uint32 ID → model ID, variant asset hash and canonical parameter values. Zero means that model's default asset. A nonzero block must belong to the selected model. Blocks also carry that variant's named-part model map. They are interned by `(model, asset hash)`, so equal geometry can be shared without confusing model identity. |
| `Scripts` | Monotonic script ID → immutable source/bytecode/declaration artifact hashes, validator/runtime pins, author and creation time. Repeated source submissions retain separate attribution while sharing immutable artifact files. |
| `RegistryPins` | Current hash and immutable historical registry documents. Terms retain their existing numeric IDs; new terms append. Rule replacements append history and increment version. |

Models, parameter blocks, script identity rows, model/instance text and model settings are snapshot data. Asset manifests and validated script artifacts are reloaded by hash. Decoded geometry and term-name indexes are derived. No C++ pointer, VM reference or Jolt identity enters these rows.

`react::Terms`, `Materials`, `RuleVersions`, `AssetMeasurements` and `ModelSettings` remain independent composition-owned tables. Catalog functions populate exactly the affected output, not a catalog facade containing every table. Asset loading never mutates any of these live projections directly.

## Geometry artifact import

Use the actual schema in [bake-api.md](bake-api.md). It has version 1, `hash`, `bake_version`, `coordinates: z_up_meters`, resolved numeric `params`, `parameter_schema`, measured fields, default material/script, parts, sites, bindings, `replicated`, LOD records and collision paths/version. Do not introduce a second manifest format.

A trusted bake completion carries the expected hash and the exact canonical parameter JSON used by Python's existing `bake.parameters.asset_hash`. C++ must not independently guess Python float-to-JSON formatting: exponent and float formatting can differ between JSON libraries. The loader parses that canonical string, checks numeric equality with manifest parameters and recomputes SHA-256 over the exact source bytes, NUL, canonical parameter bytes, NUL and the pinned bake version. On restore the expected canonical bytes come from the persisted parameter/default-asset identity. Root source manifests use that formula; child assets use the existing `SHA256(parent_hash + NUL + "part" + NUL + part_name)` formula and verify the parent/part reference.

The off-step loader reads only within the supplied immutable artifact root. It verifies lowercase hash IDs, regular required files, finite ordered bounds and nonnegative measurements, parameter schema/bounds, expected coordinate/version pins, three allowed LOD names, allowed collision/mesh filenames and valid child hashes. Empty logical assemblies legitimately have zero volume and null collision paths. Native geometry validation remains the bake supervisor's existing actual cgltf/Jolt validation; catalog does not substitute a declared triangle count for that validation or load physics into the step. This game prototype trusts the local supervisor's completed immutable artifacts; a cryptographic attestation service or hostile-filesystem defence is outside this contract.

A valid `PreparedAsset` is a plain worker result. The step accepts its metadata into `Assets` by hash. `submit_model` then adds new model identity rows and ordinary child-model rows from the already imported graph. Each part gets the original submission's author/time, its own baked defaults and a corresponding child model ID. Root and child measured volumes are disjoint; diagnostic `assembly.volume` is never an additional mass charge. Imported geometry may be shared by any number of model rows.

A parameter edit bakes the original model source with the new declared values. `add_variant` verifies the model's source hash and part identity before allocating a global parameter block. The root ModelId stays unchanged. Each distinct variant receives ordinary child model rows for its actual part graph, using the original source author/time; its part map is independent of the default model part map. Names may appear/disappear, and the root may transition to or from a zero-volume logical assembly. Composition compares old/new maps, rebinds matching named child handles and creates/removes the rest through the authored construction law after aggregate actual-mass preflight. Applying the block to an object is an independently authorized authored construction-law operation; catalog never edits Objects. Rigid transform changes need no parameter block.

Registry mechanical bindings explicitly identify volume/capacity/density/mass and other supported physical roles with term IDs and expected dimensions. Native code does not infer roles from names. `project_measurements` fills a model-default row or a nonzero parameter row from the selected baked asset. `project_settings` accepts only setting terms. The value resolver selects a parameter row directly, with no per-instance copies of measurements.

Replication resolves manifest names to existing quantity/flag IDs, requires scalar-compatible terms and the wire's uint16 term range, rejects duplicates and enforces the wire's uint8 state-count range. Render/physical bindings resolve their terms and validate the supported mechanical role's kind/dimensions; their authored numbers remain data. Unknown term references fail before registration. Unknown presentation extensions can remain inspectable metadata but cannot silently activate an unknown native physical role.

## Script validation worker and immutable artifacts

The geometry worker does not execute Luau. Provide a native `amb_script_validate` CLI/library entry, called off-step by the future bake HTTP adapter. It receives source, a registry pin and runtime/host-ABI pins. In a fresh sandboxed VM it compiles and runs the top level once with the original 50 ms/4 MiB limits, then derives the declaration from the returned table. Request-supplied `watches`, `sets`, `contributes`, `actions`, kind, selectors or tests are never accepted as that declaration.

The validator extracts `kind`, `applies_to`, `watches`, `contributes`, `slots`, action rights and the actual `on` functions. It checks existing term kinds, sources, dimensions, rights and rule tests. Literal `self:set`/`self:rate` sites supply declared state terms when the original source omits a separate `sets` list. Calls with dynamically selected terms must select from an explicit declaration; runtime bindings still enforce the verified allowed set. This preserves the original pump example while preventing callers from granting an undeclared state/contribution capability through JSON. AST checks reject writes to captured scalar locals; captured tables/globals freeze as in the reaction contract.

The worker returns/publishes `source.luau`, compiled bytecode and `script.json` under one artifact hash. Identity is SHA-256 of exact source, registry pin, Luau version and host-ABI version with NUL separators. `script.json` includes those pins, source/bytecode digests and the derived normalized declaration, including symbolic term names and resolved IDs for that pinned registry. It includes no authority/approval claim. Rule approval/activation is an independently authorized registry operation, following fixtures; a source saying `kind='rule'` does not approve itself.

Catalog verifies artifact digests/pins and imports the already verified declaration. It never invokes Luau while handling a model registration. Runtime `react::load_script` executes the verified module once to construct immutable closures, using its accepted declaration; it does not learn declarations from caller JSON. The validation worker never mutates the region VM. The writer loads the verified immutable module at startup or an input boundary, measures and records that pause, and does not claim off-thread installation. The top level retains its 50 ms cap, so a pathological newly accepted script may cause a measured one-time pause beyond the ordinary step budget; subsequent handlers retain the 5 ms limit. A separate validation VM is not shared with the region VM. Runtime approval/activation comes from the current registry and can be changed only through its authorized input path.

## Concrete public surface

The names below are plain records or exact-table operations. `PreparedAsset` and `PreparedScript` contain copied immutable results, not service objects or callback collections.

```cpp
PreparedAsset read_asset(path root, Hash expected, string canonical_params,
                        const RuntimePins&);
PreparedScript read_script(path root, Hash expected, const RuntimePins&,
                           Hash registry_pin);
void import_asset(Assets&, PreparedAsset);
ScriptId import_script(Scripts&, PreparedScript, PlayerId author, double created_s);
ModelRegistration submit_model(Models&, ModelTexts&, react::ModelSettings&,
    const Assets&, const Scripts&, const react::Terms&, const ModelSubmission&);
uint32_t add_variant(ParameterBlocks&, Models&, ModelTexts&, react::ModelSettings&,
    const Assets&, const Scripts&, const react::Terms&, ModelId, Hash variant_asset,
    string canonical_params);
EffectiveAsset resolve_asset(const Models&, const ParameterBlocks&,
                              ModelId, uint32_t params);
void project_measurements(react::AssetMeasurements&, ModelId, uint32_t params,
    const PreparedAsset&, const MeasurementBindings&);
void project_settings(react::ModelSettings&, const react::Terms&, ModelId,
                      const react::Values&);
TermId append_term(react::Terms&, const TermProposal&, const DuplicateChoice&);
void set_material(react::Materials&, const react::Terms&, TermId,
                  const react::Values&);
Hash save_registry(path root, const react::Terms&, const react::Materials&,
    const react::RuleVersions&, const MechanicalBindings&);
void load_terms(const RegistryDocument&, react::Terms&);
void load_materials(const RegistryDocument&, const react::Terms&, react::Materials&);
void load_rules(const RegistryDocument&, const Scripts&, react::RuleVersions&);
```

A registry document is a validated immutable JSON value, not a live aggregate of table references. Its semantic term identities are stable; redefining a term's kind/source/dimensions requires a new appended term. Material intrinsic values and rule activation/version may change through a new registry pin. Registry authority, Journal records, trigger rebuilding, reactivity invalidation, object bindings and snapshot orchestration stay in their owning modules/root adapters.

## Acceptance checks

* Two imports of identical geometry yield different root/child model IDs, authors and text while retaining the same asset hashes.
* Same model with two parameter blocks reads/render-resolves two actual assets and different measured mass/capacity. A block belonging to another model is rejected. Model IDs cannot stand in for parameter blocks.
* Root/part imports preserve the baked pivot, separate collision/GLB assets and summed mass exactly once. Logical assemblies remain zero-volume/noncolliding.
* Missing artifacts, wrong source/parameter identity, changed runtime pins, unknown replicated terms and out-of-wire-range IDs fail before live-table mutation. Validated geometry/artifact failures stay explicit.
* Forged caller declarations do not change the worker's extracted declaration. Unknown terms, wrong state/contribution sources, unknown rights, missing rule tests and retained hidden state fail validation.
* Immutable source/bytecode/declaration pins survive save/load; approval is supplied only by the active registry. Import code can compile/link without space, permission or networking.
* Invented registry names can bind every supported mechanical role and replication without native edits. Text containing a measured quantity never overrides its asset/provider source.


## Validator transport and checks

`amb_script_validate --request FILE --output DIR` reads a JSON object with `source`, the canonical native `registry` document, `registry_pin`, `pins:{luau,host_abi,bake?,jolt?}`, and required `limits:{script_kb,declaration_ms,handler_ms,event_fields_max}` from shared configuration. The current pinned Luau/host ABI strings are `0.747` and `ambergris-host-1`. The worker verifies the registry pin, writes `source.luau`, `bytecode.luau`, `script.json` into the supplied staging directory and prints `{hash,source_hash,bytecode_hash,declaration,files}`. A failure prints `{errors:[...]}` and exits nonzero. The supervising adapter publishes the completed directory under its returned hash. No request approval/declaration field enters trusted metadata.

`VmSettings` is explicit at region/worker construction. `set_activation` updates the loaded declaration after authorized registry changes. `HandlerRef.world_rule` is also required for a world-rule invocation; an approved rule artifact cannot gain construction authority by being attached as an ordinary model script. Catalog rejects rule-kind model defaults.

Snapshot codecs use distinct table arguments and explicit capture selectors. Absent selector means full snapshot; an explicitly empty span means empty capture. ID allocator high-water marks, parameter-specific part maps/default canonical bytes, script pins, watch answers/revisions, process revisions, integral meters and RNG states survive restore. Derived incident indexes rebuild. Runtime measurements and immutable assets/code are not serialized as mutable instance facts.
