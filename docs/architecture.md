# Ambergris restart: implementation contract

This supplements the original Technicals and Specs; it does not reduce their scope. Read `restart-constraints.md` first. The prototype remains one C++20 server, one single writer, plain mutable tables, Jolt, Recast/Detour, Luau, uWebSockets, a build123d bake service, a Python gateway, and a Godot browser spectator.

## Boundary decision

Three reasonable implementations were considered. A server class with subsystem methods is convenient to construct but gives every method authority over everything; it caused the rejected implementation. Services with virtual interfaces restrict access but add machinery and indirection to a small single-process game. We use direct functions over explicit table references and small column projections. The program's `main` allocates independent tables and wires libraries; it contains no game rules and is never passed to a library. There is no aggregate World, Core, Store, Runtime, or Services parameter.

Independent engine owners have separate CMake libraries and small public APIs. Related integration sources share one `amber_bindings` library instead of a library per operation or source file. `amber_execution` remains separate because the driver uses VM invocation without depending on the integration library; this keeps the link graph acyclic. The fixture subprocess runner belongs to `amber_elaborate`. Larger owners split related operations into narrow public headers rather than one header exposing every operation's access. Public headers define input/output types and the exact tables an operation accepts. A module cannot reach another module through a global or singleton. Functions do not receive tables merely because another function in that module needs them. No unused interface or dormant alternative implementation is retained.

`store.hpp` defines plain tables and allocation helpers, not a container owning all tables. References to vectors are used for column projections; retaining spans across object creation would invalidate them. Identity validation checks the slot's generation and nonzero live model. Read projections contain const references, and writer projections expose only columns the caller owns:

| Projection | Columns |
| --- | --- |
| Identity | generation, model, material, flags |
| TransformRead / TransformWrite | position, rotation |
| AuthorityRead / AuthorityWrite | generation, owner, group, preset; plot is read-only in both |
| AttributionRead | maker, created_step |
| ParentRead / ParentWrite | parent |

Plot membership has its own writer. Runtime flags have masked writers: space can change physical mode and held bits, and permissions can change escrow/no-copy bits. Only lifecycle creation can introduce the body bit. Each writer preserves all other bits. The specified proxy bit remains part of the wire schema; asset-loading boxes are local renderer state, so there is no unused native proxy writer.

Only object creation/destruction and the object snapshot section receive the complete Objects table. The lifecycle operation initializes/removes rows; it does not implement trade, physics, permission, script behavior, or persistence orchestration.

## Ownership and libraries

| Library | Own mutable state and permitted writes | Explicit reads and outputs |
| --- | --- | --- |
| store | SoA object allocation, sparse-column implementation, plain type definitions | No subsystem dependencies |
| step | foreground/background queues, timer wheel, intent records, current step | Callback results and queued work; no world tables |
| react | term registry and immutable versions, properties, processes, contributions, authored relations, script state and dispatch | Identity, model/material definitions, part relations, named host bindings; emits work, action requests, gaps, completed handler records |
| perm | grants, groups, offers, trade confirmations, wallets, mailboxes; authority column writes | Identity and supplied target scope; outputs ownership/escrow changes for space to mirror |
| space | Jolt/Detour mirrors, navigation jobs and paths; transform/parent writes and four physical relations | Identity, asset geometry, material transparency, intents; returns contacts, zone changes, motion state and query results |
| net | sockets, session transport buffers, encoded per-cell stream history | Immutable replication rows, declared replicated terms and control replies; queues received controls without interpreting gameplay |
| record | writer queue and compressed event file | Completed event values only; no world table access |
| snap | reusable encoding buffers and async compression/file writer | Each section encoder receives its own tables; assembler receives encoded sections, never the world |
| observe | observation response cache and hearing queue | Identity, transforms, measured properties, maker text, physical query operations, action declarations; no mutable game state |
| elaborate | request and gap counters, fixture worker results | Captured section bytes, role check result, immutable script declarations; queues tests in a separate process |

Facts are not a way around ownership. The reaction implementation receives individual property/process/contribution tables. Native bindings for navigation, payments, and spatial queries each retain their own exact references. A VM binding registry can hold a function pointer and binding-local data because Luau requires C callbacks; it never receives a universal host object or a struct containing all adapters' tables. Reaction tests link without Jolt, Detour, or networking.

Native input actions cover mechanical administration only: placement, edits, ownership, grants, wallet payments, offers, installation and registry/elaboration operations. They live beside the module that owns their mutations. The action dispatcher resolves a declaration, validates its typed inputs, checks its declared right once, then calls its handler. Related handlers share a coherent capability: the twelve editing actions use four contexts for script installation, text, properties/relations and registry definitions. Their registration is one editing operation, rather than twelve pieces of composition-root wiring. These contexts contain only their responsibility's tables, not every action's state. Ordinary script actions enter foreground reaction work with an installed-script holder identity and their origin; they drain before the next external input, and dispatch never re-enters an already-running Luau coroutine.

Resolution is explicit rather than a spelling convention. Reserved native administrative terms cannot also be declared by scripts. For other terms, the invoking body's declaration is selected first when present, then the explicitly selected object's declaration; material/property rules on that object are the fallback. A declaration's `right_on` field selects `self` or the selected target for its right/reach check; it never changes the actual receiver or installation identity. A body's physical handler receives the selected world object as an argument but runs on the body. Held objects expose their own actions when explicitly selected as targets; there is no hidden held-item priority. This implements the documented body-first physical controls without a native list of body verbs. All broad-to-narrow applicable handlers for the selected receiver still run.

## Authored behavior

Everything under `content/` is authored game content, not a dependency of an engine library. It contains the seed registry, materials and embodied-energy values, CAD programs, model declarations, body scripts, world rules, zones, institutions and start configuration. The executable loads this data through normal import/installation operations. No native file recognizes café, oxygen, fish, treasury, a named player, or an authored body verb.

The body script declares move_to, hold, release, speak, strike, push, pour, throw and sit. Engine primitives are navigation, attachment/detachment, grasp/release with optional velocity, sound, impulse, and conserved-quantity transfer. Their C++ names describe mechanics, not humanoid actions. The script chooses formulas, target terms, sites and events. Strike computes energy and emits impact. Push applies an impulse. Throw releases with velocity. Sit attaches to an authored site. Pour transfers a requested liquid volume bounded by source amount and destination capacity and handles a remainder according to its script; it never creates an impact.

S4's host API is extended explicitly to make that design possible: `read(handle, term)` and `position(handle)` are read-only; handles come from relations, parts, queries or the current action/event. Transient event context can contain a handle, scalar, vector or bounded typed record. Persistent `self.state` remains numbers, booleans, strings and handles. Globals, captured tables and declarations are frozen, and source validation rejects writes to captured scalar upvalues; persistent mutation belongs in `self.state`. Disallowed retained VM objects cannot become unsaved state.

A direct body invocation contains the player, body, selected target, checked declaration and held handle, not an unrestricted authority token. A binding may use that player's authority only for self, that selected target and the currently held source, and only for the requested physical operation. Required mechanical rights are checked against those endpoints; two-ended conserved transfer checks both before writing either. This bounded endpoint check is needed when the declaration's one target check cannot cover a second object. There are no stored tickets or hidden grants. Timers and later reactions retain causal origin but never inherit the initiating player's authority. `self:act` always uses the script-and-installed-object grant holder. Ordinary scripts receive no owner's implicit spending/transfer authority.

Conserved transfer accepts two valid endpoints, one registered non-shared state term marked conserved, and a finite nonnegative amount. It settles any processes at the current step, checks the endpoints' mutation authority/custody, computes the amount available, then writes equal debit and credit without yielding. A missing destination entry starts at zero. Identical endpoints are a no-op. It does not interpret liquids or clamp to capacity: the authored body/container policy does that. It is an explicit limited exception to foreign state mutation, not a general `set(other, ...)`. The original own-state `set`/`rate` facilities are retained; no extra source/sink permission system is imposed on authored physical laws.

Each body/target declaration identifies the right and whether the physical operation needs reach. A body's physical handler runs on the body with the selected world target in its arguments; the one action permission check applies to that target. A request for movement to a point validates land access through the normal navigation path. Unknown in-range actions fall through to material/model/instance rules and produce a gap if none handles them.

## Generic processes, zones and replication

Effective reads use this resolver, in order, without consulting text:

| Term | Read location |
| --- | --- |
| relation | Specified relation's current edges, including an empty answer |
| combined quantity | Its subject's shared entry; if absent and `along` is declared, the first outgoing related subject's entry; otherwise absent |
| intrinsic quantity/flag | Current material definition |
| baked measured quantity | Effective baked asset's measured value; mass is volume times the current material density |
| runtime measured quantity | The registered derived provider's cached answer for that object/term; assembly mass sums disjoint root/part volumes with their current material densities |
| state quantity/flag | Its process evaluated at the current step, else its explicit state entry, else absent |
| setting | Instance, model, plot, world, then absent |

Terms with a contribution combiner cannot also be independently written state/process quantities. State values and shared totals therefore do not ambiguously override one another. A changed answer includes transitions to/from absence. Along-relation dependence is recomputed when an edge changes.

A script may declare `thresholds = { {term = 'author.quantity', value = number} }` or a list of values for a term it watches. Thresholds belong to that consumer. When a process's rate changes, react evaluates its current value and schedules the next declared crossing for every dependent consumer, including along-relation readers. The wheel retains one pending threshold per full source/consumer/script/watch identity through a derived bucket index, and revision checks still reject obsolete work. Ordinary delayed events remain independent. The index is rebuilt from TIMR rows during restore; equal-due timer ordering is unspecified. No native term name selects this behavior. Reads compute the closed-form value; nothing writes every process each step.

Zones are authored volumes with text and ambient illumination properties. Space maintains membership and exposes zone handles. Observations display those handles' authored names and compute illumination from ambient properties and nearby light contributors with occlusion. Moving or renaming the seed leaves the engine unchanged. No coordinate rectangle supplies game meaning.

The replication assembler iterates each object's effective asset manifest's `replicated` term IDs and reads their values. Net only quantizes/encodes the supplied rows. There is no engine list of interesting state names. Catalog side channels supply model, material, text and action metadata; immutable assets supply geometry and manifest bindings. Parameter hashes select baked geometry; measured size and capacity never come from arbitrary client overrides.

## Composition and persistence

The main loop follows S2 directly: advance the clock, take queued work/timers/inputs, drain after every input, give background work at least one item, advance space, build observations, encode replication at the configured cadence (every second step by default), hand completed records to the writer, and serialize a due snapshot at a boundary. Independent writer, bake, navigation and fixture workers receive copies. They never mutate live tables. Per-session action order is maintained while an asynchronous fixture result is pending; other sessions continue.

Related cross-owner work has fixed integration operations. `ObjectLifecycle` defines creation, rebind, initialization/restore and retirement order; callers do not assemble lists of lifecycle callbacks. `SceneProjection` owns visibility, movement, value and illumination maintenance. After physics, the loop projects movement and changed values, advances derived illumination, reconciles its new facts, projects those new facts and publishes them. Engines and VM bindings never receive these coordinators. `CatalogPublication` coalesces definition/metadata changes, maintains the current handshake catalog and inspection terms, and publishes final rows once per drain. Authoring completion and checkpoint capture/saving similarly live in their existing integration modules. `main` constructs these owners and sequences phases rather than repeating each owner's internal steps at every call site.

Each subsystem exports a section encoder/decoder for its own authoritative tables. Snap assembles those bytes in S10 order with the version, registry pin and checksum; its writer compresses off-thread. The composition root calls restoration functions in the specified order. Captures select object handles and close over related rows before invoking the same section encoders, so captures use the real snapshot format. Source and runtime binaries are never embedded. Space supplies moving-body velocities and sleeping state. Restoring rebuilds derived collision, navigation and dispatch data, then resumes intents.

Authoritative model rows are not geometry assets: two submissions of identical geometry retain distinct model author, material, text, settings, script and creation time. Snapshot TEXT includes the model/catalog identity rows and parameter-block references in addition to text. PROP holds processes and their revisions; SCST holds last-delivered watch answers and binding revisions; TIMR holds threshold tokens and their process/binding revisions; QUEU holds typed queued payloads. Immutable asset manifests are reloaded by hash. These payload extensions fill S10's omissions without changing the plain tables to suit persistence. Retirement uses the collected declared-part closure. It settles commerce custody, removes reactions and grants, clears inventory and spatial state, then removes derived projections and releases object rows. Cleanup runs while the original full handles are valid; surviving ancestors' measured mass is refreshed after part removal. A reused generation never inherits the old slot's state.

Money is one integer wallet balance per player, indexed by PlayerId. Objects and groups have no balances; object income goes to its current owner's player wallet. An installed script needs an explicit capped Spend grant on that wallet and never inherits owner wallet authority. Issuance requires an architect-provisioned, capped, period-bound, expiring grant; institutional policy lives in Luau. Ordinary S9.4 goods offers implement Technicals §14's base swap, with income entering the seller's wallet.

## Inhabitants and acceptance

The production harness calls an actual model with the five world tools and authoring submission tools. It supplies personas and budgets, observations and decision history; it does not parse purpose keywords or select authored solutions. CAD examples may be documentation or explicit test fixtures, never an inhabitant's hidden policy. A run records the model, observations, tool decisions and submissions so authorship can be inspected. Mechanical workloads remain separate from autonomous-run evidence.

Before the implementation is accepted:

* Build and test react, step and perm independently of physics/network libraries. Check forbidden include edges and broad aggregate parameters.
* Invent a new quantity, threshold, model-replicated term and body verb entirely in authored content. Their behavior must work without a native edit.
* Rename and translate the seed zones and replace the treasury script/recipient. Observations, membership, illumination and institutional payments must follow the data.
* Exercise real CAD baking and live placement, stairs-only mezzanine access, glass/wall sight, the multi-author power chain, a chair obstructing a door, permissions/delegation/sale/mailbox/trade, unknown-action elaboration, capture tests with/without rules, and snapshot continuation.
* Run actual model inhabitants and compute frozen measures from their resulting record, separately from deterministic workloads.
* Benchmark permission worst case, handler/process density, 300-fish replication, 500 sockets, navigation edits, snapshot pause/restore, sandbox forks, and browser download/startup/frame cost. Report failures and environment limits plainly.

The detailed negative checks and ambiguities are in `architecture-review.md`; authored-content contracts are in `content-contract.md`. Changes to these boundaries require an explicit engineering reason and corresponding checks, not a convenient shared pointer.
