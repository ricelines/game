# Authored content contract

Status: reviewed content/engine boundary for the clean restart. The authored package, real CAD bakes, declaration validation and trusted native import now exist; full gameplay integration remains a separate gate. Exact calls are in the linked API documents. This supplements the five original design documents and [architecture.md](architecture.md); read [restart-constraints.md](restart-constraints.md) first.

The deliverable is a playable authorable world, not a café demonstration. The café is its starting workload. Removing the package must remove its power, oxygen, rent, body verbs and institutional behavior while leaving geometry, physics, scheduling, permissions, generic reactions and the ordinary goods swap available.

## Decisions and alternatives

| Decision | Alternatives and tradeoffs | Recommendation |
| --- | --- | --- |
| Where gameplay lives | Native handlers make the first demonstration easy but cannot support newly authored mechanisms without recompilation. A general visual rule language adds another authoring system alongside Luau. | Luau and registry declarations supply behavior; native operations supply mechanics. Use the same submission and installation paths for seed and later authors. |
| How to express thresholds | A native list of interesting quantities recreates the rejected oxygen special case. Polling every consumer every step wastes time. | Consumers declare numeric thresholds on arbitrary terms; the process scheduler predicts crossings. |
| How bodies interact | A native switch on verb names is quick but makes the body script decorative. Giving scripts arbitrary foreign-state writes defeats composition. | Body handlers choose narrow physical operations, events and authorized conserved transfers. Foreign state otherwise remains under its object's handlers. |
| How zones overlap | Adding all ambient values makes an exterior zone brighten an enclosed room accidentally. Requiring disjoint volumes makes editing awkward. | Highest authored priority wins for ambient; ties use stable handle order. Membership can still contain multiple zones. |
| How inhabitants choose | Keyword routing and prepared CAD can stage reliable scenes but cannot establish autonomous invention. A model loop costs time and money and may fail. | Actual model policy is the production path. Keep deterministic drivers in explicitly labelled tests only. Record failures. |

All scene dimensions, rates, thresholds, body strengths, prices and institutional recipients belong to content. Engine constants are mechanical limits such as step frequency, query budgets and maximum payload size. A native algorithm may use a declared role bound to a term ID; it must never infer a role from a term spelling, object name, maker's text, model number or coordinate rectangle.

## Package and installation

The package is ordinary source plus one declarative start file. A small installation client submits those sources through the normal bake, registry and script interfaces, resolves returned identities, then submits placements and relations. It is not linked into an engine library and has no live-table access.

| Planned path | Contents |
| --- | --- |
| `content/start.json` | Both start modes, stakes setting, symbolic seed identities, initial resources, plots, accounts, placements, script installations, grants and relations. |
| `content/registry/` | Material definitions, term declarations, quantity dimensions, contribution combiners and mechanical role bindings. |
| `content/models/` | Build123d source for body, street, café shell, floor, stair, mezzanine, furniture, containers, electrical objects and inhabitants' starting equipment. |
| `content/scripts/body.luau` | Humanoid controls and charge behavior. No native dispatch by these action names. |
| `content/scripts/` | Independent collector, breaker, light, pump, reservoir, school, door, socket, fabrication, material-response and institution scripts. |
| `content/zones/` | Volume geometry, authored names, ambient values, priorities and any descriptive text. |
| `content/agents/` | Model configuration references, persona/purpose text, budgets and initial memory; no solution scripts. |
| `tests/content/` | Directed fixtures, captures and load drivers. These are never selected by a production purpose string. |

The start file uses local symbolic references such as `cafe.floor`, `public.circuit` and `resident.builder`. Its loader resolves them to ordinary returned handles. These symbols never become native constants. Objects, groups and player identities may all be remapped without changing a script that follows the appropriate authored relation. Sources and their dependency hashes remain inspectable.

`seeded` starts with buildings and stocks. `forage` starts with personas and empty hands, using the same registry, bodies, public-access starting land and start file. Records have explicit mode membership rather than separate native initialization functions. The stakes switch selects authored rule/script activation and initial settings; the engine does not contain a global stakes branch. Public slow charging remains available when charge is enabled.

Installation records seed objects and scripts as `seed: true`. It does not fabricate player-chosen actions. Restore loads authoritative state and rebuilds derived structures; it does not rerun the installer or issue money again.

## Geometry, coordinates and starting layout

World coordinates use meters and Z up. A model's authored base is Z = 0 and its XY origin is centered, as S8 requires. Instances supply rigid transforms. The single asset conversion maps `(x, y, z)` to glTF `(x, z, -y)`; the viewer uses the inverse mapping for server transforms. Asset geometry, collision, measured bounds, support faces, capacity, sites and parts come from the same bake and parameter identity. The wire `params` field identifies the baked parameter set; it is not a shortcut for a seed model ID.

Changing geometry parameters creates a new baked asset. Changing an instance's rigid transform does not. There is no separate arbitrary size override or browser-only correction. A proxy is permitted while a new bake is pending; the real asset replaces it for collision, navigation, measurement and rendering together.

The following is the proposed seed in one local frame. Values are game content, not engine defaults. Its installer may translate or rotate the whole assembly without changing behavior. Shared dimensions, such as the stair landing and platform opening, are generated from the same content parameters rather than rounded independently.

| Element | Authored geometry and placement |
| --- | --- |
| Street | Solid slab, X `[-64, 64]`, Y `[-14, 0]`, top Z `0`, thickness `0.30`. Its continuous frontage is a content-defined curve used by rent policy, not an engine region name. |
| Forecourt | Slab X `[-3, 3]`, Y `[0, 2]`, top Z `0`, thickness `0.30`. It meets the Street and café floor without a gap or step. |
| Café floor | Slab X `[-8, 8]`, Y `[2, 18]`, top Z `0`, thickness `0.30`. |
| Dome | Half-ellipsoid shell centered at `(0, 10, 0)`, outer semiaxes `(12, 12, 8)` m. CAD subtracts an inner shell and the entry opening. The nominal `0.06` m shell parameter is not represented as a perfectly uniform normal thickness if constructed by subtracting ellipsoids. |
| Entry | A real Boolean opening in the front shell, width `3.0` m and height `2.8` m, with a cutter spanning the full curved shell near Y `[-3, 2]`. A separately baked sliding leaf covers a `2.4` by `2.4` m clear opening at approximately Y `-1.8`. The Street supports the approach through the shell. |
| Mezzanine | Top Z `3.20`, thickness `0.22`, X `[-6, 6]`, Y `[8.5, 16.5]`. Subtract a stair opening X `[-6.2, -3.4]`, Y `[8.5, 11.5]`. Add solid supports that leave the stair and circulation clear. |
| Stair | X `[-6, -3.6]`, Y `[5.5, 11.5]`, width `2.4`, twenty rises of `0.16` and twenty runs of `0.30`. Last tread meets the platform at Y `11.5`, Z `3.20`; the platform opening clears the entire upper flight. |
| Rails | Actual rail geometry, `1.0` m above exposed platform edges, with an opening at the stair landing. No invisible navigation exclusion substitutes for a rail. |
| Aquarium | Static hollow tank X `[3, 7]`, Y `[5, 7]`, base Z `0.85`, outside height `2.0`, walls `0.02`, floor `0.03`; a solid stand supports it. Fill to `85%` of measured capacity, leaving freeboard. The aisle X `[7, 8]` stays open. |
| Fish | `300` ordinary server objects, within the tank's water bounds with radius margin. One controller owns their scripted motion and state. Each fish has its own handle, transform, asset and replicated state; tail motion may be cosmetic. |
| Public equipment | Collector, circuit, slow sockets, lamps, work area and treasury object on the Street's public plots. Exact positions and connections are start-file data. There is no designated player receiving native payments. |
| Humanoid | Height `1.75`, collision radius `0.30`, eye site Z `1.55`, grasp site about `0.35` m forward and Z `1.10`. Dimensions, site transforms and movement settings are body content. |

This layout has testable clearance. At a café floor corner `(8, 18)`, the dome's roof height is about `2.67` m. At the mezzanine's rear outer corner `(6, 16.5)`, it is about `5.41` m, giving about `2.21` m above the platform. Both exceed the proposed body height plus a `0.15` m margin. These calculations guide authoring; the authoritative acceptance test uses baked geometry and the actual body collider.

Static shell, tank, rails and stairs use their real mesh collision. A large convex hull around an entire shell or staircase is invalid: it would close openings or make stairs optional. Holdable/dynamic models use S8's bounded hull; the abstract quantity inside a small cup does not require a fluid collider. Any need for articulated collision is represented by authored parts and their shapes, not a model-name exception.

The seed includes an opaque partition for the glass/wall sight test and places counters, chairs and supports with navigable clearances. A chair blocking a door is a test arrangement or an inhabitant's action, not a timed seed incident. Navigation diagnostics must identify disconnected landings and unreachable destinations from actual geometry. No portal teleports a body to the mezzanine.

## Content-facing declarations

The registry retains S3's source classes, SI dimensions and contribution combiners. Names below illustrate seed vocabulary; none are recognized by native code. Materials supply density, hardness, transparency and embodied energy. Bake output supplies bounds, solid volume, capacity and supports. Scripts own state; authored settings use the documented instance/model/plot/world cascade.

Bindings are explicit validated metadata that connect registered values to mechanical roles. They do not introduce another gameplay language:

| Binding | Input and effect |
| --- | --- |
| Visibility transmission | Material declaration binds a boolean or scalar term to sight-ray transmission. Transparent glass remains collision-solid. Maker's text cannot change transmission. |
| Light emitter | Model declaration binds an intensity term in candela, an authored emission site, color and influence radius to a point light. The script supplies intensity; space/perception only evaluate the generic light query. |
| Zone ambient | Zone declaration binds an ambient term in lux, a real sensor volume and an authored priority. Highest priority containing zone supplies ambient; a content-authored world fallback covers no-zone positions. |
| Viewer state | `REPLICATED` lists arbitrary registered scalar/flag term IDs. Manifest bindings may map these values to named GLB material/node properties or a declared animation. The inspector shows all received terms even without a visual binding. |
| Quantity storage | A container declaration lists conserved state terms it stores and a payload selector. This exposes the actual stored term to body code without recognizing water, cups or tank models. |
| Body/attachment sites | Manifest sites contain transforms and declared affordance terms. Scripts select sites; generic attach/grasp mechanics consume transforms and identity. |

The renderer supports a small mechanical vocabulary of material color/emission/opacity, node visibility and declared GLB animation weight/rate. Its mappings are numeric scales, clamps and color ramps, with explicit defaults. They never contain action execution or tests of a particular term spelling. A newly declared `author.glow` can drive an emitter or GLB emission without a client source edit. Server-authoritative part transforms drive physical door/fish motion; a visual animation must not stand in for collision motion.

CPU illumination follows Technicals §7: zone ambient plus in-range intensity divided by squared distance for each unoccluded light. A generic near-distance clamp avoids the singularity at the emission site. Authored radius limits candidate collection; geometry rays decide occlusion. The role binding and clamp have documented units. Viewer lights use the same sites, intensity and on/off state. For zone ambient, the viewer derives a per-object ambient contribution from generic zone membership rather than replacing the whole world's ambient when the camera enters the café. A calibrated lux-to-display mapping is presentation data; exact photometric equality with GPU rendering is not a game requirement. Changing a source visibly changes the scene and the server's measured light in the same direction.

Zone names are descriptive metadata returned with their handles. Membership comes only from volume queries. Moving, resizing, renaming or deleting a zone updates membership and observations through the ordinary spatial path. Viewing from outside a room does not make the room use exterior ambient. The client receives zone shapes/bindings as metadata; it contains no café rectangle.

The observer has an authored dim threshold; the tank controller has oxygen thresholds; a body has charge thresholds. No global engine table lists these quantities. Values omitted from a model's replication list are not serialized as a convenience.

## Minimal host ABI additions

These additions close omissions in S4, not permission to supply a universal world object to Luau. Each binding captures only its own narrow operation and table access. [architecture.md](architecture.md) owns the native API boundaries.

| Operation | Mechanical contract |
| --- | --- |
| `read(handle, term)`, `position(handle)` | Read live properties/transforms for handles acquired from the current action/event, a relation, parts or an explicit query. Read does not confer mutation authority. No retained trigger snapshot. |
| Transient event context | Bounded typed record containing handles, numbers, booleans, registered terms and vectors, plus sender and causal record. Only immediate player actions expose actor/body/selected/held through `context()`; events and timers never inherit them. Persistent `self.state` remains S4's serializable flat scalars/handles. |
| Navigation intent | Set/cancel a destination and requested speed for the invoking body's own motion. Normal geometry, land access and character collision still apply. Return Trying/arrived/blocked through the existing intent path. |
| Attachment/grasp/release | Create/remove the physical relation at declared sites, preserving custody/rights and optional release linear/angular velocity. No interpretation of `sit` or `throw`. |
| Impulse | Apply a vector impulse at a point to an authorized physical body. Jolt decides motion; a static body can report no effect. No automatic material damage is inferred from the caller's action name. |
| Own-part sweep | Attempt a rigid transform of self or a declared part and return achieved transform plus obstruction contact. Use the real collision shape. Script chooses whether to stop, reverse or retry. |
| Sound | Emit a typed audible event with authored text/timbre and radius. Ordinary hearing uses distance. |
| Conserved transfer | Move a specified nonnegative amount of one declared conserved, non-shared state quantity from source to destination. Check both sides' rights/custody scopes before mutation, settle processes at the current step, and reject insufficient source before mutation. The body chooses the amount from available quantity and measured receiver capacity. The source decrease and destination increase happen together. This operation cannot create or silently delete quantity. Ordinary own-object initialization and authored `set`/`rate` retain S4 semantics; no extra global source/sink permission system is introduced. |
| Account operations | Read permitted balances and make authorized transfer/issuance requests. Money is one whole-unit wallet per player; an object has no balance. Spending and issuance capabilities have caps, periods and expiries; script installation never implies its owner's financial authority. |

A direct body handler receives the initiating actor's authority only for the immediate requested mechanical operation and documented target/custody scope. It cannot retain that authority in a timer. A later handler uses its installed-script identity and its own ordinary grants. `self:act` always follows the installed script's grants. `self:set` still writes only self/parts; conserved transfer is an explicit mechanical exception with its own source and destination check, not arbitrary foreign-state assignment.

The action declaration states its checked right, selected world target, physical reach requirement and any acquired source/custody scope. A body input runs on the body while its selected target remains the world object named by the call. Host calls cannot choose arbitrary unrelated handles using that actor's permission. The permission review must finalize this invocation scope before host implementation; a broad actor token is not acceptable.

Generic transfer does not decide whether a destination is a container, whether substances mix, the destination's capacity, or whether a remainder is spilled. Those are script decisions using measured properties and declared stored terms. It only preserves the requested quantity within its explicit authority. A full recipient may therefore receive zero by body policy even though the primitive itself does not recognize capacity.

## Authored humanoid controls

The body declares `move_to`, `hold`, `release`, `speak`, `strike`, `push`, `pour`, `throw`, `sit` and `stand` as Luau handlers. Their declarations make them discoverable; native code never recognizes these spellings. An alternate body may omit them or implement different actions.

| Action | Authored calculation and mechanical result |
| --- | --- |
| Strike | Resolve held object or striking body part; read measured mass and material hardness. Compute swing energy `0.5 * mass * swing_speed^2`, with swing speed a body setting. Emit a typed impact event at the reached target carrying energy, source material properties and contact point. The target's material/property handlers decide consequences. A weak soft object is ineffective through those values, regardless of its name. |
| Push | Choose direction, strength and duration from body settings and arguments; request impulse in N·s at the contact point. This changes physical motion. It does not emit strike damage merely because both actions touch something. |
| Pour | Discover the held container's stored conserved term and available amount. Read the recipient's measured capacity and current compatible contents. Compute accepted volume, request the authorized conserved transfer, and choose whether to retain or spill the remainder. No impact event occurs. |
| Throw | Release the held item with authored linear/angular velocity. Its real flight may miss or hit intervening geometry. Later Jolt contact emits measured collision information; a target material rule may react. Do not emit an immediate impact at a selected distant target. |
| Sit | Resolve a reachable seat site, request attachment, and establish the ordinary visible animate/camera grants specified by the seat/body interaction. Occupancy follows physical attachment/geometry, not a native seating minigame. Standing detaches and expires the grants. A body without this handler cannot sit. |

For pouring, the default humanoid uses an upright-container approximation. It transfers a chosen volume between compatible reservoirs and retains every unaccepted remainder at the source. It emits no impact event and creates no fake spill. A future authored spill policy would need an actual recipient parcel and ordinary creation/transfer authority; soaking or evaporation is not assumed by this body.

The impact rule reads numeric physical quantities rather than item classes. The body can spend its own charge based on calculated work. The handler's ordinary failure/cutoff keeps completed effects, as S4 requires. The primitive transfer itself cannot leave half a quantity move; this is one conserved mechanical update, not a reaction rollback framework.

Removing `strike` from the installed body removes its implementation and discovery entry. Replacing its formula changes downstream effects without rebuilding the engine. A new `nudge` or other body verb uses the same host mechanics and normal gap behavior for unsupported target responses.

## Independent café mechanisms

Each mechanism is authored separately and communicates through terms and relations. Shared subject objects such as circuits and reservoirs are ordinary objects. Scripts follow their declared connections, never seed IDs or one another's model names.

| Mechanism | Reads | Own result/contribution |
| --- | --- | --- |
| Collector | Measured surface area, authored daylight value and efficiency | Supply contribution in watts to its connected circuit. |
| Breaker | Own `on` state and upstream allocation | Demand upstream; contribute the allocated amount downstream when closed. Two distinct circuit subjects prevent feeding its output back into itself. |
| Lamp | Own switch, rated demand and allocated watts | Demand contribution and own luminous intensity in candela; generic emitter binding exposes that intensity. |
| Pump | Own switch, rated demand, allocated watts and container relation | Demand contribution and oxygen mass-flow contribution to its current reservoir. Withdraw old recipient when moved/disconnected. |
| Reservoir | Water volume, oxygen inflow and aggregate oxygen consumption | Oxygen-concentration process: `(inflow - consumption) / water_volume`, bounded by authored limits. |
| School controller | Reservoir concentration, declared thresholds and outgoing `aq.school` relations | One motion timer uses ordinary per-fish Move grants. Status events tell each fish's own handler its health rate; the controller never writes foreign health. |
| Observer body | Generic illumination measurement and authored dim boundary | Structured perception and optionally an authored reaction; the environment remains independent of an observer's session. |

Supply/demand uses S3's paired scarce-allocation terms, not independent totals that can manufacture power. A breaker forwards only its allocated upstream watts. A consumer switching off withdraws demand. Contribution destruction/recipient changes use the normal contribution cleanup path. A newly authored load can join through the same relation and quantity declarations.

Initial oxygen concentration is `0.008 kg/m³`, bounded by `[0, 0.008]`, with stress at `0.003` and a severe boundary at `0.0005`. Total school consumption is `0.0005 kg/s` and powered pump inflow `0.0008 kg/s`. These are intentionally paced game values, not claims about real fish physiology. At the proposed tank volume they produce consequences over minutes, leaving time for agents to perceive and respond. The content installer records actual measured water volume; it never substitutes a seed literal in the engine.

Thresholds belong to consumers and are ordinary declaration data, for example:

```lua
watches = {
    {term='aq.oxygen', along='aq.reservoir', thresholds={
        {predicate='below',value=0.003}, {predicate='at_least',value=0.003},
        {predicate='below',value=0.0005}, {predicate='at_least',value=0.0005}}}
}
```

The implemented declaration semantics are: declared scalar values on own or along-relation process reads; both crossing directions wake the consumer; relation/rate/boundary changes invalidate old timer revisions; adding/enabling a rule catches up existing subjects. A boundary is scheduled from the closed-form process, not discovered by per-step polling. The handler reads current live state when it runs. Health recovery/death policy belongs to the controller; switching power back on does not automatically undo prior effects.

Fish are ordinary non-solid kinematic objects sharing one actual baked asset, not CAD parts. The manifest declares collision solidity. One controller updates their transforms under explicit per-endpoint Move grants; the viewer interpolates the real positions. Each fish owns its closed-form health process. Dormant objects have no recurring handler cost; only the school needs a motion timer.

## Authored door motion

The door is a model with a frame and a moving leaf part, not an engine door type. Its Luau script declares opening/closing actions, speed, travel direction and retry policy. A close command schedules short own-part sweep increments. Each sweep uses the leaf's real shape and returns an achieved transform and contact. The script updates its own open fraction and stops when blocked; a chair is just an ordinary obstruction. The next-step contact event can also trigger the same stop path.

The script emits its own state changes; generic geometry invalidation updates sight, light queries and affected navigation tiles. No native code checks `door.open`, searches for chairs or grants a passage because a displayed animation looks open. Removal/replacement of this script removes/replaces the stop/retry behavior while physical collision remains.

## Stakes and replaceable institutions

The embodied-energy table comes from the original Technicals §18 starter values, stored as material content: stone `1`, concrete `1.1`, ceramic `6.5`, timber `8.5`, glass `15`, steel `20`, plastic `77`, aluminium `155` MJ/kg. The source document explicitly treats it as one chosen older table, not a precise current environmental assessment. Its pace factor is content. Fabrication consumes material stock and charge using measured mass; dismantling returns matter and not spent energy. There is no item recipe whitelist.

Body charge has authored resting, distance/mass and work costs. A zero-charge threshold selects an authored slow speed instead of stranding the body. A socket contributes charge according to its circuit allocation and charging relation. Collector yield, cheap-land upkeep and embodied-energy pace are the three main tuning settings identified by the design. No native `if charge == 0` chooses a humanoid action.

Money is one whole-unit wallet per player. An object or group has no balance, and income from an object goes to its current owner’s wallet. A script does not inherit its object's owner's wallet. Ordinary grants authorize spending with cap, period and expiry. Issuance is a separate explicit capability on a currency/account scope, also capped, expiring and revocable. It is not implied by account ownership, `treasury` text, an elaborator role or permission to install a script. The seed file grants it to one installed issuer with an explicit initial policy; replacing that issuer requires a normal authorized grant change. Wallet operations record the player and actual installed script attribution.

The treasury is one inspectable and optionally owner-locked authored object whose script uses its current owner’s wallet under an explicit capped Spend grant. It is never prefunded as an object. Its script chooses issuance schedule, public supply procurement, rent policy and payees. Public supplier arrangements are ordinary authored relations and script state. Settlement reads actual delivered resource amounts, not name, model identity or a fixed recipient. Generic process integration exposes actual served allocation, not offered supply: `100 W` offered with `30 W` demand meters only `30 W`; no demand meters zero. Supplier attribution is proportional to each supplier's accepted contribution unless an authored allocation policy declares otherwise. A piecewise-constant accumulator folds elapsed steps only when allocation changes or the meter is read and is snapshotted. Changing allocation mid-period never pays for unavailable power. This is resource accounting data, not a native procurement service.

Rent reads plot area/frontage and distance along a content-defined continuous Street curve to content-defined ports. The authored script computes price, debt, grace and return/salvage timers. Ownership return still requires the generic land-transfer authority; a script cannot seize arbitrary plots. Contents are held/returned/salvaged under explicit grants and policy. None of those values or account IDs are in native code.

There is a source ambiguity to preserve explicitly: Technicals §14 and S9.4 describe ordinary placed/vendor goods offers with custody and an atomic swap; Technicals §18 says the engine has no contract/escrow feature while preserving the base swap. For this prototype, retain the specified ordinary goods-offer custody and bilateral atomic item/money swap. Interpret §18 as excluding a general contract/invoice/bank/institution escrow engine. More elaborate stores, banks, conditional deposits and public institutions are authored scripts, accounts and ordinary grants. Do not silently remove the base vendor journey or grow it into a general contract subsystem.

Replacing the treasury source, owner and handle must change policy without a C++ edit. A second provider supplying a public circuit must become eligible through the authored relation and measured contribution. The engine never pays a particular player periodically on its own. Snapshot/restore preserves player wallets, grants, institution script state, processes, relation links and due timers without repeating issuance or settlement.

## Actual model inhabitants

Production inhabitants are asynchronous model clients. Each receives its current purpose, personality/preferences, neutral tool descriptions, recent observations, ordinary memory and a finite budget. The harness offers the five world tools plus normal authoring/submission clients. The model chooses calls and writes the actual submitted CAD/Luau source. No purpose parser selects a table/cup design, predetermined route, repair incident or fixed tool sequence. Model work never blocks the 20 Hz writer.

The initial cast gives differing interests, not hidden solutions:

| Persona | Starting direction | What the harness must not supply |
| --- | --- | --- |
| Mira, host/builder | Make a place others choose to visit; prefer inviting, practical spatial designs; seek feedback from users. | A table source file, required furniture list or scripted building sequence. |
| Sol, practical engineer | Earn trust and income by making useful systems reliable; investigate measurements before changing equipment. | A known breaker incident, hardcoded repair branch or automatic power toggles. |
| Ivo, aquarist | Maintain a healthy living display and improve its surroundings; value observable living conditions. | A rule that reacts to the word “oxygen” by following a prepared tool sequence. |
| Nia, trader | Find something others need, make or acquire it, and negotiate a viable business with ordinary permissions. | A canned cup, preselected sale offer or guaranteed buyer. |
| Rue, elaborator | Investigate requested gaps, propose coarse consistent laws and test them before activation; preserve others' work where possible. | Hidden direct-table edits, an automatic answer rule or bypassed fixture approval. |
| Visiting maker | Explore a personal creative purpose unrelated to the café and learn from existing residents' work. | A compulsory café task or an adversarial incident schedule. |

Personas are short prompt text and may be revised. Human purpose changes replace/steer the purpose supplied to that model; they do not call a native action or start a keyword scenario. Give agents the same neutral CAD/API reference available to authors, with generic minimal examples clearly identified as documentation. Submitted useful designs must arise in the recorded model output during that run.

Choose an actual configured provider/model before a production run and record its identifier, prompt version, request/response usage, tool decisions, source submissions and world record links. An unavailable model pauses/retries within the configured budget and reports unavailable status; it does not fall back to a deterministic resident. Budget and context compaction policy are harness settings, not world mechanics. Model credentials use the authorized wrapper without entering world observations, logs or source assets.

Success is not guaranteed by a persona. The playable prototype must support the requested activities; evidence of autonomous invention requires an actual run in which those choices occurred. Directed functional fixtures and load generators remain valuable but are labelled separately and excluded from autonomous-run claims and measures.

## Extension and performance acceptance

Contract review precedes source implementation. Once implemented, the following use real production paths and artifacts rather than replacement simulators:

| Check | Required result |
| --- | --- |
| Empty/minimal content | No café, oxygen, power, body-verb or treasury behavior exists without its scripts. Physics, generic authority, placement and base goods swap remain. |
| New quantity | An author invents a differently named/dimensioned process, a consumer threshold and a replicated state without a native edit. Its crossing wakes only the declared consumer and the viewer displays its state. |
| Alternate body | Remove `sit`, replace strike energy, and add a new body action in Luau. Discovery and real effects follow the installed code. Native source has no body-verb dispatch cases. |
| Pour and conservation | Partial/full/incompatible recipients, empty source, denied source/destination and failed spill creation produce explicit outcomes. Source + recipients + spill remains constant; no impact record is generated by pouring. |
| Distinct mechanics | Push moves through impulse, strike invokes material response, throw follows flight/collision, sit uses attachment/grants. A soft renamed “hammer” gains nothing from its text. |
| Geometry | Runtime GLBs and Jolt collision match; glass passes sight but blocks bodies, opaque partition blocks sight, the shell has a real opening, and mezzanine access uses actual stairs. Rotating/translating the seed preserves these results. |
| Door obstruction | An ordinary placed chair blocks the leaf's real sweep; authored behavior stops it. Remove/replace the script and observe the corresponding behavior change. |
| Composition | Independently authored breaker, lamp, pump and school compose through terms/relations. A new consumer and moved/destroyed contributor update the same shared state without seed references. |
| Zones/light | Add and move a novel zone and emitter through authoring tools. Observation names, membership, CPU lux and viewer appearance follow declarations. No server/client coordinate recognizer is added. |
| Institutions | Replace treasury script/identity; add another actual supplier; exercise caps/revocation/expiry and snapshot continuation. Payments follow authored policy and measured delivery. Ordinary vendor/direct trades remain atomic. |
| Real inhabitants | Record an actual model's previously absent submitted design, placement and another agent's use/acquisition. Preserve failed attempts; no test driver counts as autonomous play. |
| Threshold cost | Idle processes incur no per-step materialization. Benchmark many unrelated terms and consumers, rate changes and invalidated crossings, not just oxygen. |
| School/transport | One real Luau controller drives `300` real fish; report its handler time, per-viewer bandwidth and browser frame cost. Target the original approximately `100 kbps` per-viewer gate. |
| Whole prototype | Preserve the original 20 Hz, sub-step snapshot pause, seconds-scale restore/join, occupied navigation edit and 500-viewer gates. Report p50/p95/p99, step overruns, environment and failures; do not substitute synthetic handlers for expensive real paths. |

Native/client source audit accompanies the tests: no seed coordinates, model IDs, named inhabitants, oxygen/fish branches or treasury recipients. Mechanical bindings may mention generic role names; their term IDs come from declarations. The first implementation slice should be a genuinely new baked object, one authored body interaction and a generic viewer update, before adding the full seed.

The remaining ABI review points are narrow: invocation/custody authority for conserved transfer; the exact declaration shape for along-relation thresholds and role bindings; and the generic allocation-integral interface used for supplier settlement. They must be closed in the root contracts before those host bindings are coded. They are not reasons to add a native pour, oxygen, light-model or treasury special case.
