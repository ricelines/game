# Live model authoring

The [Technicals](02-t.0.md) §13 and S8 distinguish a model definition from its
immutable geometry artifacts. A model edit changes the definition and every
current instance that uses it. It does not require the model author to own those
instances. Ordinary per-instance editing remains permission checked separately.

The gateway exposes two authoring submissions:

- `submit_variant {model,params}` reads the model's current public source, bakes
  those parameters, and registers the verified artifact under the same model.
  Registration compares the artifact's actual source with the current model
  source. If they differ, the caller must rebake the current source. No supplied
  expected hash or public manifest grants trust.
- `edit_model {model,source}` bakes the new source's defaults and each distinct
  explicit parameter set of the current model. Retained parameter names keep
  their values; removed names are dropped; new names take the new defaults.
  Out-of-range retained values produce the bake's actual error. Completed
  artifacts are registered together as one ordinary proposal model, with one
  chosen S7 registration record. The result includes `{model,proposal}`.

Baking runs outside the simulation step and outside the gateway mutation lock.
Only completed registrations use the session's ordered mutation lane. Queries
and unrelated actions remain available while CAD runs. Failed sandbox jobs
publish one authenticated failed-authoring record; a native receipt prevents a
second record for a failure already published by registration.

The actual model author applies a ready proposal with the ordinary native action
`base.revise_model {model,proposal}`. A script cannot borrow its installation
owner's model-author authority. There is no saved edit plan, compare-and-swap
hash, resource exchange, editor workflow, or rollback operation.

Applying a definition first cancels affected offers/trades through ordinary
custody. This cancellation remains an explicit effect if the subsequent geometry
edit is rejected. Geometry descriptors, retirement constraints, and combined
native capacity are checked before changing the definition. Each affected rigid
assembly is rebuilt once. Existing object handles, owners, state, instance
settings, and root poses remain; retained named children preserve their authored
material/script/text/settings defaults, while new children use proposal defaults.
Removed children follow ordinary retirement. Hidden goods remain hidden.

Default parameter block `0` adopts the new defaults. Existing explicit block IDs
are updated to their matching baked geometry, so reconfiguration cannot select a
superseded definition. All registered blocks are checked, including blocks not
currently placed. Immutable source and geometry files remain content addressed.
The edit refreshes actual measurements and ordinary watcher, spatial, navigation,
light, observation, and replication projections through the normal lifecycle.

Native `register_variant` accepts `model`, `asset`, `canonical_params`, and
`manifest`. The worker loads verified artifacts outside the step; the writer
checks their source against the actual model. The reply contains the model,
parameter block, asset hash, and measured manifest. Duplicate variants reuse the
existing block. `register_model` can additionally carry ready variant artifacts;
the complete proposed family is validated before allocating its public identity.
