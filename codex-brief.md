# Correction brief

This brief lists exact changes. Do only what it says. Where a task does not say
how to do something, choose the smallest change and write the choice in your
report. Do not design new mechanisms.

## Rules for every task

- Do not add documentation files. `docs/` is gone; the design doc copies are
  in `design/` and must not be edited. `content/authoring.md` is game content
  for agents; keep it accurate to S4 when you change the script API.
- Read AGENTS.md first. Its boundary rule changed: owners now keep their tables
  in one `State` struct each (see Phase 2).
- Delete code instead of adding code. Do not keep the old path, a flag, a
  compatibility shim or a migration. When you remove something, remove its
  declarations, bindings, config keys, docs and tests too.
- Do not add tables, indexes, caches, threads, processes, versions or retries.
- Change a test only when it asserts behavior that this brief says is wrong.
  Name each changed test in your report. Never weaken a test to make it pass.
- If a task cannot be done as written, stop that task, leave the code as it
  was, and explain why under "Blocked" in your report. Do not improvise.
- Line numbers in this brief are approximate and drift as you commit. Find
  the named function or file region; do not edit by line number.
- Work one phase at a time. Each task is its own commit. At the end of a phase,
  run the checks below, write `.cache/brief-report-PHASE.md`, and stop. Do not
  start the next phase until told to.

Checks after each phase (all must pass):

```sh
.tools/bin/cmake --build .build/release -j 8
.tools/bin/ctest --test-dir .build/release --output-on-failure
python3 bake/run.py -m pytest -q
.venv/bin/ruff check bake content gateway harness measure tests
.venv/bin/ruff format --check bake content gateway harness measure tests
```

Pytest does not cover the live journeys. After each phase, also run each
`tests/game_*.py` journey against a fresh throwaway world built from a freshly
prepared plan, with newly generated keys, as in the Phase 1 correction pass.
Report each one's result and evidence path. A journey that cannot run is
reported as not run, never as passed.

Report format: one section per task with commit hash, files changed, tests
added or changed (and why), and anything you were unsure of. Then "Blocked".
Then line counts of `server/` before and after.

## Phase 1: concrete spec violations

### 1.1 Body primitives are not player actions

Players reach body verbs only through the body script (Technicals 16, AGENTS.md).

- `engine.*` actions declared in `declare_physical_actions`
  (`server/step/physical_actions.cpp`) accept only script callers. These are
  exactly `engine.grasp`, `engine.release`, `engine.attach`, `engine.impulse`
  and `engine.transfer` (the `react::MechanicAction` family). Every other
  native action is also named `engine.*` (give, offer, grant, install and so
  on). Those stay player actions; do not change them.
- In `begin_action` (`server/step/actions.cpp`), after the action resolves:
  if the caller is a player and the resolution is a `MechanicAction`, deny
  with `{ ok: false, error: bad_input, problem: 'engine.grasp, release,
  attach, impulse and transfer are called by a body script' }`.
- `player_action_ids` (`server/net/read_look.cpp`) leaves out those five
  actions and lists every other native action as before.
- Keep all five actions and the body script's calls to them unchanged. Pour
  and recharge keep using `engine.transfer`. It is an ordinary action checked
  against the body script's grants (Technicals 8), and the existing tests
  depend on that check: after the owner revokes the body script's grant, pour
  is denied.
- Update `tests/physical_causality_test.cpp` (around line 113), where a player
  calls `engine.impulse` directly: make the caller a script, and add one check
  that a player caller is refused.

Done when: a player `do engine.attach` or `do engine.impulse` is refused as
above; `look` lists no player action among those five but still lists
`engine.give`, `engine.offer` and `engine.grant`; and the existing pour and
recharge tests pass unchanged.

### 1.2 Native queries check the run deadline (S4.2, S4.3)

`world.near`, `world.raycast` and `self:plot_objects`
(`server/step/physical_actions.cpp:587-629`, `server/step/metadata.cpp:223`)
check the invocation's deadline while they collect results. When it has
passed, stop and cut off the run exactly as the VM interrupt does (same
`system`/`cutoff` record, effects so far kept).

Done when: a new test runs a script that calls `world.near` in an endless loop
and gets one `system`/`cutoff` record, and the step continues.

### 1.3 Handles in `self.state` grant nothing

Remove the rule that a handle read from `self.state` is added to the run's
acquired set (`server/react/vm.cpp:183-186`). `self:measure_instance` requires
an acquired handle like the other reads (`server/step/metadata.cpp:337-343`).
The pump example in S4.1 still works because `contribute` separately accepts a
subject the script already contributes to.

Done when: a test shows `self:measure_instance(h)` fails for a handle that the
run only read back from `self.state`, and content scripts still pass their tests.

### 1.4 Slow-viewer limits are recorded (S0.3, S6.4)

When a socket crosses `SEND_SOFT` (marked stale) or `SEND_HARD` (closed) in
`server/net/server.cpp`, net emits a notice that the step thread drains and
writes as a record `{ kind: system, name: overflow, detail: { queue: 'send',
session, buffered_bytes, action: 'stale' | 'closed' } }`. The journal is only
written on the step thread.

Done when: a test with a socket that stops reading produces both records.

### 1.5 The `ours` measure adapter respects `changed` (S7, S11.4)

In `measure/adapters/ours.py`, when a record of any kind other than speech,
observation or move has `changed` not true and result `applied` or missing,
set its result to `no_effect`. Its class stays `change`, so it counts in the
acting-share denominator and not in C. Fix `tests/test_measure.py:243-266`
accordingly. Remove the `time_s`/`t` fallback at `ours.py:36`; use the field
S7 writes.

### 1.6 Measure and gateway constants come from config.toml (S0.3)

- Add `measure/config.py` in the style of `bake/config.py`. `measure/core.py`
  and `measure/validate.py` read `MIN_ACTIONS`, `WIKI_WORLDS` and
  `WIKI_BOOTSTRAPS` from it. No literals remain. Fix the "the40" typos.
- `gateway/session.py:124` reads `EVENTS_PER_RESPONSE` from config.toml.
- Add `ACTIONS_PAGE = 100` under `[perception]` and use it at
  `server/step/connections.cpp:180`.

### 1.7 Captures reveal only what the player could see (S12.2, Technicals 12)

Captures (`checkpoint::capture_selection`, `server/perm/snapshot.cpp`,
`server/net/profiles_snapshot.cpp`) no longer contain player profiles, key
hashes, connection modes or ledger balances. Change
`tests/checkpoint_test.cpp:117` to assert they are absent.

### 1.8 A sale tells the old owner what was cleared (S9.2, Technicals 13)

When `accept` or `trade_confirm` clears grants on goods, send the previous
owner a session event `{ kind: changed, cleared: [g:N, ...], on: o:... }`.
If the previous owner is offline, send nothing; the journal record is enough.
Native actions cannot reach sessions directly. Add one field to
`NativeCompletion` (`server/store/store.hpp`): `std::vector<std::pair<PlayerId,
nlohmann::json>> notices`. `step::run` sends each one with
`sessions.event_for_player` after the action completes, as it already does for
expired offers. A gift (`give`) needs no notice: the giver is the actor and
already gets the list in their reply.
Remove the cleared grant ids from the buyer's reply.

### 1.9 Write `denial` and `registry` record kinds (S7)

- A denied action is written with kind `denial`.
- `define_term`, `define_material`, rule activation and a rule answer
  (`server/step/elaboration_rules.cpp:102`) are written with kind `registry`
  and the player as actor.
- Update `measure/adapters/ours.py` so a `registry` record is an agent event of
  class change and its rule counts as a kind for adopted novelty.

### 1.10 Remove dead plot transfer

Delete `perm::transfer_plot` (`server/perm/perm.cpp:440-460`) and
`LandVerb::Transfer` and the grantable plot transfer right
(`server/step/authority.cpp:115`).

### 1.11 A watch change is not lost between planning and running

`check_watch` (`server/react/watches.cpp:143`) updates `binding.last` during
planning. Move that write to the moment the handler run actually starts. Plan
each handler once; `start_invocation` and `applicable()` must not dispatch
again (`server/react/execution.cpp:50-56`).

Done when: a test changes a watched value, makes the first run not start (for
example the object's script is disabled before it runs), re-enables it, and
the handler still fires once.

### 1.12 Far salience ranks by size, not distance (S5.3, gate "dome")

In `server/space/perception.cpp` around line 255-281, rank `far` candidates
by angular size (largest first), then take `FAR_MAX`. Moving and bright
candidates keep their `why`. A test: from a Street point, with 20 chairs
between 31 and 60 m and the dome at 85 m, `far` contains the dome with
`why: large`.

## Phase 1 follow-ups

Do these after Phase 1, each as its own commit, before Phase 2.

### F.1 Rent salvage after task 1.3

Task 1.3 stopped handles read from `self.state` from counting as acquired.
`salvage` in `content/scripts/rent.luau` (around lines 60-80) calls
`self:measure_instance(object)` on a handle it saved in `self.state` while the
goods were held, so it now fails. Fix the script, not the engine:
- In `hold_contents`, when `engine.hold_goods` succeeds, the handle came from
  `self:plot_objects` in the same run and is acquired. Call
  `self:measure_instance(object)` there. For each row of its `materials`,
  store `self.state['held_kg:'..count..':'..row.material] = row.mass_kg`.
- In `salvage`, do not measure. Call `self:act('engine.destroy', object, {})`
  when the hold expires. If it succeeds, add each stored `held_kg` value to
  `self.state[material]`. If the reply's `problem` is `stale_target`, the
  object is already gone; credit nothing. In both cases clear the row's keys,
  including its `held_kg` keys. On any other failure, keep the row.
- Add a test that holds goods, lets the hold expire and checks the credited
  material masses.

### F.2 Capacity is measured and read in one place

Today pour needs both objects to declare `BINDINGS["container"]["stored"]`
(`content/models/cup.py`, `content/models/tank.py`), and code reads
`asset.capacity` directly in several places. Capacity is a measured property
of an instance (Technicals 9: nobody gives a thing capacity by declaring it).
Make every read go through the existing instance read, so the measurement can
change later without touching callers:
- The one read point is `react::read(values, object, capacity_term)`
  (`server/react/values.cpp`), using the `measurements.capacity` role. It
  takes the instance handle. Do not change what it returns.
- `self:measure_instance(h)` gains a `capacity_m3` field from that read.
- Observations use that read for `capacity_l`, not `asset.capacity`
  (`server/net/presentation.cpp`, around line 73).
- In `content/scripts/body.luau`, `base.pour` always moves `base.liquid`.
  Delete the `bindings.container` lookup and its `stored` check. The default
  amount and `to_max` come from `self:measure_instance(...).capacity_m3`, not
  `self:asset(...).capacity`. Any held object can pour into any target; one
  with zero capacity simply holds nothing.
- Remove the `stored` key from `BINDINGS["container"]` in the content models
  and from the bake pipeline. Leave the tank's `shape` key as it is.
- Add nothing else: no new terms, tables or host calls.

Done when: `grep -rn "asset\.capacity\|\.capacity\b" server content` finds
only the code that loads the manifest and projects the measurement
(`server/store/catalog.cpp`, `server/react/catalog.cpp`) and the unrelated
`feed.capacity` in `server/net/read_events.cpp`; `grep -rn "stored" content`
finds nothing; and a test pours between a cup and a model with no `BINDINGS`.

## Phase 2: one State struct per owner

Goal: no function takes more than about eight parameters. This is a mechanical
regrouping. Behavior must not change; do not rename tables or change logic.

1. In each owner's public header, declare `struct State` holding that owner's
   tables, taken from the locals in `step::run` (`server/step/run.cpp:95-187`):
   - `catalog::State`: assets, models, scripts, parameters, model_text,
     instance_text, authored_text, baked_measurements.
   - `react::State`: terms, materials, properties, processes, the four
     settings tables, shared, gives_to, allocations, meters, authored_relations,
     relations, runtime_measurements, states, random, triggers, rules,
     subscriptions, installations.
   - `perm::State`: players, groups, plots, grants, ledger, owner_limits,
     offers, trades.
   - `elaborate::State`: requests, gap_counts, last_gaps, captures, fixtures.
   - `net::State`: profiles, sessions, session_views, replication.
   - `step::State`: queues, timers, intents, inputs, intent_endings.
   - The store's types are in namespace `amber`, not `store`. Put `Objects`,
     `lifecycle::Parts` and `declared_parts` in `struct ObjectState` in
     `server/store/store.hpp`.
   - `net::Sessions` is constructed from a reference to `profiles`. Declare
     `profiles` before `sessions` in `net::State`, and construct `sessions` from
     it in `State`'s constructor.
   Put everything else that `run` declares (illumination, masses, travel,
   causes, navigation requests) in the owner that writes it; write the choice in
   the report.
2. `step::run` holds one of each State and passes them. Rewrite every function
   with more than eight parameters to take the States it uses (`const &` for
   reads). Start with `flush_gaps`, `encode_checkpoint`, `decode_checkpoint`,
   `save_snapshot`, `answer_query`, `refresh_connections`, `native_*`,
   `revise_model`, `replace_instance`, `place_existing`, `create_instance`.
3. Withdrawn. Snapshots keep their existing per-table helpers; do not add
   per-owner section functions.
4. Never declare a struct in `server/` that holds more than one owner's State.
   Tests are composition roots like `step::run`: a test fixture may hold
   several owner states.
5. Add to `tools/check_architecture.py`: fail on a declaration in a server
   header with more than 10 parameters, and on a struct with members of two or
   more different owners' `State` types.

Done when: the checks pass, the new architecture checks pass, and a snapshot
written before Phase 2 restores after it.

### Phase 2 completion

The first pass left 24 declarations above 10 parameters. Most are over
because they take an owner state and also separate pieces of it, or many
scalars. Fix them only in these ways:

1. Do not pass a table next to the State that holds it. Remove the separate
   parameter and read it from the State: `space::PhysicalRelations` from
   `space::State`, `net::Profiles` from `net::State`, `Intents` from
   `step::State`, and the same for any other case the checker shows.
2. Move `space::Space` into `space::State` as a member, and pass
   `space::State` instead of both.
3. Do not pass `react::ValueRead` next to the states it views. Add one
   function that builds it from `const ObjectState &`, `const catalog::State &`,
   `const react::State &` and `const space::State &`, and call it inside the
   function that needs it. `ValueRead` stays a read view; do not store it in a
   State.
4. Pass the loaded settings struct (the one `read_settings` returns) or its
   existing sub-struct instead of separate settings numbers such as
   `actions_page`, `capture_limit`, `radius_m` and `reach_m`. Pass `now` and
   `hz` as the existing `Clock` where a function takes both.
5. `encode_checkpoint`, `decode_checkpoint` and `save_snapshot` write or read
   every owner by definition (S10). If they are still above 10 after items
   1 to 4, add exactly these three names to an explicit allowlist in
   `tools/check_architecture.py`, with that reason as a comment. No other name
   may be allowlisted.

Behavior must not change. Done when: the architecture check passes, all other
checks and journeys are as good as or better than the Phase 2 report, and a
pre-Phase-2 snapshot still restores.

## Phases 3 to 7: root, land, the market and joining

These five phases replace the land, money and account code. Read these first;
all of them changed:
- Technicals 13: "A grant's scope", "Root and the architects", the check's
  worst case, and the player actions;
- Technicals 18: "The Street and its land", "Land rent", "Land is bought and
  sold", the return paragraph, "Tenancy", "Money is issued", and
  "Institutions";
- Technicals: "Joining" and "Sessions";
- Specs S0.3, S1.7, S4.1, S4.2, S4.5, S5.1, S5.3 (`player_actions`), S5.4,
  S6.2, S8.1, S9 and S13.

The design copies are `design/technicals.md` and `design/specs.md`.

Each phase must leave the game working: all checks pass and the journeys run.
Do the phases in order. Each task is its own commit unless it says otherwise.
Bump `runtime_version` (`server/step/step.hpp:24`) once per phase, in the
phase's first commit that changes the snapshot layout. Old snapshots are
rejected; there is no migration.

The old design is gone. Do not keep any of it for compatibility: no
`public.treasury`, no `engine.issue`, no issuance scope, no leases in
`rent.luau`, no fixed plots in the start file. Delete the tests that check
the old design, and name each one in your report.

Where this brief gives a file and line, the line is approximate.

## Phase 3: root and authority

### 3.1 Root's account

Player 0 is root. Its account, `p:0`, is the money account. It has unlimited
money and holds none.

In `server/perm/perm.cpp`:
- `pay` (around line 480):
  - A payer of `Account::player(0)` is never short. Skip the balance test and
    do not subtract.
  - A recipient of `Account::player(0)` is not credited. The money leaves
    circulation.
  - If both are root, nothing happens.
  - Today `pay` rejects any account whose `subject` is 0. Keep that rejection
    for every account except `Account::player(0)`.
- `check_account` (around line 295): today `!target.controller` returns
  `UnknownHolder`. For the root account the controller is 0, so only a grant
  on `Scope::account(Account::player(0))` can allow it. No actor is ever 0, so
  the owner shortcut never matches.
- Root's balance is never stored. `read_players` (`server/perm/snapshot.cpp`
  around line 227) already rejects a stored balance for subject 0; keep that.

In `server/step/authority.cpp`, `step::account` accepts `"p:0"` and returns
`{Account::player(0), 0}`. Do not change `step::identifier`: it must keep
rejecting 0 everywhere else. Every place that takes an actor or a player
holder keeps rejecting `p:0`: `player()` in `commerce_admin.cpp:8`,
`commerce_detail::player` and `player_id` in `commerce_private.hpp`, and
`installed_holder`.

Delete issuance completely: `ScopeKind::Issuance`, `Scope::issuance()`,
`perm::issue`, `step::issue` and `engine.issue` (`server/step/finance.cpp`),
`ScopeControl::issuer_is_architect` and every parameter that carries it, the
`issue` right and slot name, and the `{"issuance": true}` scope. The sites are:
- `store.hpp:1242`, `1258` and `1372`;
- `perm.hpp:57`;
- `perm.cpp:201`, `362` and `500-513`;
- `perm/snapshot.cpp:55`;
- `authority.cpp:50`, `88-89` and `96-98`;
- `commerce_admin.cpp:121-130`, `161`, `247`, `267`, `297` and `337`;
- `step.hpp:344`, `347`, `422` and `805`;
- `finance.cpp`;
- `native_actions.cpp:132`, `219`, `234`, `240` and `425`;
- `run.cpp:572` and `590`;
- `start_permissions.cpp:141-146`;
- the slot and right name lists in `start_catalog.cpp:31`,
  `fixture_cases.cpp:103`, `react/authoring_worker.cpp:147`,
  `react/script_worker.cpp:41` and `react/artifacts.cpp:112`;
- `editing_scripts.cpp:128` and `201`;
- `commerce_schema_admin.cpp:37`;
- `read_inspect.cpp:68`.

In tests, delete the issuance parts of `tests/perm_test.cpp`
(around 25-28, 223-225 and 352-366) and `tests/financial_test.cpp`
(around 59-67, 141-169, 224-241 and 283-285). Replace them with tests for
root's account:
- a payment from root needs a root spend grant, never runs short, and adds to
  the recipient;
- a payment to root removes the money;
- a spend grant on `p:0` is capped per period;
- `p:0` is refused as an actor and as a grant holder.

### 3.2 Root's grants come from the start file

A start-file grant with `"issuer": 0` is root's.

Add `perm::create_root_grant(Grants &, Players &, const GrantRequest &,
const HolderControl &, Step)`. It applies `create_grant`'s checks:
- the scope mask;
- the duplicate key;
- a script holder must be installed;
- the expiry rules;
- monetary scopes need a period.

It skips the controller and delegation checks. The grant's issuer is 0, and
it is added to `players.rows[0].grants_given`.

Call it only from `install_grants` (`start_permissions.cpp`) when the row's
issuer is 0. `create_grant` keeps rejecting `request.issuer == 0` for
players. A script on an object whose owner is 0 does not exist after this
phase.

Revoking (`commerce_admin.cpp`, `revoke`, around line 319):
- a player may revoke a grant they issued;
- an architect may revoke any grant whose issuer is not 0;
- nobody may revoke a grant whose issuer is 0, except a script revoking a
  grant it holds or gave, as today.

Grant notices (`commerce_admin.cpp` around 217-221) are never sent to
player 0.

### 3.3 Groups and model groups

The architects group is an ordinary group whose controller is 0. Nobody can
add members to it in play; `group_member` already refuses everyone when the
controller is 0. A member may still leave.

Start file:
- `"groups"`: `[{"id": 1, "controller": 0, "name": "architects",
  "members": [1]}]`.
- `install_land` (`start_permissions.cpp:34-46`) must sort each player's
  `groups` after loading. The checks binary-search it.
- Objects get a new optional field, `"group_rights"`: a list of object right
  names stored in preset bits 8-14. Set it in `start_objects.cpp` next to
  `"preset"`, around line 27.
- Models get a new optional field, `"group"` (a group id), stored in
  `catalog::Model` as `GroupId group`. Write it in snapshots
  (`snap/catalog.cpp` around line 30).

Editing a model's definition is allowed to its author or to any member of its
group. Change the two checks: `server/step/editing_private.hpp:56` and
`server/step/model_definition.cpp:128`. They need the caller's groups from
`perm::Players`; pass `const perm::Players &`.

### 3.4 Two new scopes

Add `ScopeKind::Owned` and `ScopeKind::AllLand` to `store.hpp`:
- `Scope::owned(PlayerId owner)` is the issuer's objects anywhere. Its JSON is
  `{"owned": "p:N"}`. The issuer must be N. Its rights are object rights.
- `Scope::all_land()` covers every lot's land and everything on every lot.
  - Its JSON is `{"all_land": true}`.
  - Only `create_root_grant` may create it. `step::scope` refuses it for every
    other caller.
  - Its rights are object right names (bits 0-6) and land right names
    (bits 8-12). Write land rights as `land_build`, `land_enter`,
    `land_run_scripts`, `land_transfer` and `land_return`.

Add `PlotKind kind` to `perm::Plot` now (Phase 4 then creates plots by
surveying): `street`, `alley` or `lot`. The start file's plots get `"kind"`:
plot 3 is `street`, the rest are `lot`. Write `kind` in snapshots
(`perm/snapshot.cpp` around 127-134).

The check needs to know whether an object stands on a lot:
- Add `bool on_lot` to `perm::Target`, after `owner_limit`.
- Give `perm::target(...)` a `const Plots &` parameter and fill `on_lot` from
  it. There are 13 server call sites and 19 test call sites; the survey list
  is in `server/step/replacement.cpp:20`, `authority.cpp:55`,
  `commerce_admin.cpp:413`, `commerce_private.hpp:142` and `144`,
  `goods.cpp:51` and `73`, `physical_actions.cpp:335`, `387` and `440`,
  `actions.cpp:151`, `read_actions.cpp:93` and `read_inspect.cpp:190`, plus
  `tests/`.
- Add `bool on_lot` to `ScopeControl`, set from the target in `step::scope`'s
  object branch.

`perm::check` (`perm.cpp:237`), step 5, in this order:
1. a grant on `Scope::owned(target.owner)`;
2. a grant on `Scope::owned_on_plot(target.owner, target.plot)`;
3. if `target.on_lot`, a grant on `Scope::all_land()` with the verb's bit
   (0-6).

`perm::check_land` (`perm.cpp:267`):
- `LandVerb` becomes `Build, Enter, RunScripts, Transfer, Return`, with bits
  1, 2, 4, 8 and 16. The plot grant mask is 31 (`create_grant`'s
  `scope_mask`, and `step::scope`'s plot branch, which today wrongly has
  `7 | bit(Transfer)`).
- `step::rights` for a plot scope accepts `transfer` and `return`.
- Presets never allow `Transfer` or `Return`. Test the preset and group bits
  only for `Build`, `Enter` and `RunScripts`.
- After the plot grant, if `plot.kind` is `lot`, look up the actor's grant on
  `Scope::all_land()` with the verb's bit shifted up by 8.

Delegation (`create_grant` and `covered`, around `perm.cpp:203`):
- A parent on `Owned(o)` covers an `Object` scope whose controller is `o`,
  and an `OwnedOnPlot(o, any plot)` scope.
- A parent on `AllLand` covers an `Object` scope with `control.on_lot`, with
  object verbs only, and a `Plot` scope on a lot, with land verbs only.
- In `create_grant`'s parent lookup, after the existing two `find` calls, try
  `Scope::owned(control.controller)` for an object scope, and then
  `Scope::all_land()`.

Snapshots: `perm/snapshot.cpp` has a scope-selection switch (around 36-58)
that needs cases for both kinds. Grant JSON output needs them too:
`commerce_admin.cpp` `scope_json` and `rights_json`, `read_inspect.cpp`
around 68, and the validators in `commerce_schema_admin.cpp` 36-55.

### 3.5 Handlers are told who called

Handlers are called as `handler(self, sender, args, caller)` (S4.5). Keep the
first three arguments exactly as they are today.

In `server/react/vm.cpp`, `handler_entry` (around 524-563):
- push a fourth argument from `inv.action_caller`: the string `"p:N"` for a
  player, a read-only table `{script = 's:N', on = <handle>}` for a script,
  or nil;
- call `lua_callyieldable(L, 4, 0)`.

`action_caller` is already filled for actions in `server/step/plans.cpp:81`
and `server/step/construction.cpp:38`. Events and timers leave it empty.

Update `content/authoring.md` around line 29 to say this. Add a react test:
- a player's action shows `caller` as `p:N`;
- a script's `self:act` shows the script and object;
- an event handler gets nil.

### 3.6 The start file: root, architects, power and grants

All of this is in `content/start.json` (and `content/prepare.py` where it
validates).

Delete the treasury and the rent rule:
- the objects `public.treasury` and `public.rent`;
- the models `treasury` and `rent`;
- the scripts `treasury` and `rent`, and `content/scripts/treasury.luau` and
  `content/scripts/rent.luau`;
- the bootstrap rows for both;
- the `rent` script_state row;
- the three relations that name them (`sells_to` collector to treasury,
  `procures` treasury to circuit, `procures` rent to treasury);
- every grant they hold or that names them;
- the registry terms used only by them: `base.issuance_rate`,
  `base.rent_baseline`, `base.rent_frontage`, `base.rent_decay`,
  `base.rent_tick` and `base.rent_controller`.

Keep `base.purchase_per_joule`, `base.settle`, `base.sells_to`,
`base.procures`, `base.settlement_seconds`, `base.rent_grace_days`,
`base.rent_hold_days` and `base.day_seconds`; later phases use them.

Add a power buyer:
- a model `power_buyer` (`models/equipment.py`, like the old treasury) with
  `"group": 1`;
- its script `content/scripts/power_buyer.luau`, which is `treasury.luau`
  without issuance. It pays each supplier with
  `engine.pay {from = 'p:0', to = <supplier owner>, amount}`;
- object `public.power_buyer` where `public.treasury` stood, owner 1,
  group 1, with `"group_rights"` of all seven rights;
- relations `base.sells_to` from `public.collector` to `public.power_buyer`,
  and `base.procures` from `public.power_buyer` to `public.circuit`;
- a bootstrap `base.start` row for it;
- a root grant (issuer 0) to `power_buyer` on `public.power_buyer`: scope
  `{"account": "p:0"}`, rights `["spend"]`, cap 100000, `period_s` 86400,
  `expires_s` 31536000.

Public objects (`public.street`, `public.forecourt`, `public.circuit`,
`public.socket`, `public.lamp`, `public.power_buyer` and both
`public.ground`) keep owner 1 and get `"group": 1` and `"group_rights"` of
all seven rights. Their models (`street`, `forecourt`, `circuit`, `socket`,
`lamp`, `land_seeded`, `land_forage` and `power_buyer`) get `"group": 1`.

Plots (temporary until Phase 4):
- plot 3 gets owner 0 and kind `street`;
- plots 1, 2 and 4 to 7 get kind `lot`;
- in the seeded mode, plot 1 belongs to 2, plot 2 to 3, and plots 4 to 7 to
  4 to 7;
- in the forage mode, all of them belong to 0.

Write each plot row twice, with `"starts": ["seeded"]` and
`"starts": ["forage"]`; `prepare.py` `select` already filters list rows by
`starts`.

Root gives each architect (today only player 1) an allowance: scope
`{"account": "p:0"}`, rights `["spend"]`, cap 1000000, `period_s` 604800,
`expires_s` 31536000.

Grants that change:
- Delete every grant with a `caller` (the 18 lease rows per mode), every grant
  held by or naming the rent script, and the treasury's two grants.
- Each resident's `stakes` and `stakes_free` scripts: replace their
  `owned_on_plot` grants (one per plot) with one grant each, issued by the
  resident: scope `{"owned": "p:N"}`, rights `["edit", "destroy"]`, the same
  expiry, the same `stakes` filter.
- Each resident's `body_installs`: replace the seven `use`/`take`/`move`
  groups scoped `owned_on_plot` with one group scoped `{"owned": "p:N"}`.
  Keep the avatar group.
- Each resident's `body`, `stakes` and `stakes_free` scripts need `build` on
  their owner's plot in the seeded mode (the lease rows gave it before). Add a
  grant issued by the resident, scope `{"plot": N}` for their plot, rights
  `["build"]`, `"starts": ["seeded"]`, and the same `stakes` filter as the
  script. Do not give them build on the Street.
- Delete the residents' spend grants to the rent script. Phase 5 adds new ones
  to the land script.

Tests that must change in this phase, and how:
- `tests/test_content.py::test_leases_and_starting_body_choices_have_real_scopes`:
  replace it with checks of the new grants above.
- `tests/test_content.py::test_institution_account_has_an_explicit_script_allowance`:
  check the power buyer's root grant instead.
- `tests/test_content.py::test_rent_holds_and_salvages_measured_materials_without_reacquiring`:
  delete it. Phase 5 adds the land script's test.
- `tests/test_content.py::test_all_sources_pass_actual_luau_declaration_worker`:
  remove `engine.issue` (around line 293).
- `tests/start_test.cpp`:
  - stop adding a balance for `public.treasury` (around 115-123);
  - check body grants on the owned scope (around 150-164);
  - `wrong_owner` and `wrong_installation` must pick a script-held grant by
    holder, not `plan["grants"][0]`;
  - update the bootstrap count.
- `tests/game_body_permissions.py` and `tests/game_actions.py` build
  `body_installs` scoped `owned_on_plot`; change them to the owned scope.
  `game_actions.py` has the landlord p:1 grant `plot=1` (around 310); plot 1
  now belongs to Mira (2), so use Mira's key for that grant.

Done when: tests show all of the following:
- a root grant from the start file is created, and the same request through
  `engine.grant` by the architect is denied;
- the architect cannot revoke a root grant;
- the architect can revoke another player's grant;
- another architect-group member can edit a group-1 model, and a non-member
  cannot;
- the power buyer pays a supplier from `p:0` up to its cap;
- an owned-scope grant lets a body script move its owner's thing on any plot;
- an all-land grant lets its holder destroy two owners' things on a lot, and
  not a thing on the Street;
- a script holding all land passes on a grant on one object, and that grant
  still works after the object is held.

### 3.7 Notices wait in the mailbox

Today a notice to a player without a play session is dropped
(`server/net/sessions.cpp:282`, `Sessions::event_for_player`). From now on it
waits in the player's mailbox and is sent at their next login (S9.4).

- Add `std::vector<Json> notices` and `std::uint32_t notices_dropped` to
  `perm::Player` (`store.hpp`, around 1269). Write both in snapshots, with the
  rest of the player (`perm/snapshot.cpp`, the `Player` serialization).
- Add one step function, `notify(perm::Players &, net::Sessions &, PlayerId,
  Json)`.
  - If the player has a live play session, it sends the event as today.
  - Otherwise it appends the event to `notices`. If `notices` already holds
    `MAILBOX_MAX` entries, it drops the oldest and adds one to
    `notices_dropped`.
  - Notices to player 0 are never sent or kept.
- Replace every call that sends a notice to one player with `notify`:
  - `commerce_admin.cpp:565` (grant notices);
  - `commerce_offers.cpp:209` (an expired offer);
  - `native_actions.cpp:468` and `run.cpp:506` (elaboration);
  - `run.cpp:667` (`NativeCompletion.notices`).

  Leave the intent-end events (`physical_events.cpp:268`: arrived, blocked,
  cancelled) and the navigation notices (`run.cpp:731`) on
  `event_for_player`. They describe the live session and are stale at the
  next login.
- At login, where `deliver_mailbox` runs (`run.cpp` around 401), also send
  every waiting notice as an event, oldest first. If `notices_dropped` is not
  0, add `"dropped": N` to the first notice sent. Then clear both.

Tests:
- a plot's old owner who is offline when the plot changes owner logs in and
  receives the notice of the removed grants;
- more than `MAILBOX_MAX` notices keeps the newest `MAILBOX_MAX`, and the
  first notice delivered carries the dropped count;
- notices survive a snapshot restore;
- waiting notices do not stop a give from reaching the mailbox.

## Phase 4: the Street, lots and terrain

### Follow-ups from earlier phases

Do these first, one commit each, before task 4.1. They are follow-ups: they
fix what Phase 3 left, not new Phase 4 behavior.

#### 4.F1 A revoke that names only a holder means the caller's own grants

Today an architect who calls `engine.revoke {holder = X}` with no `grant`
and no `scope` revokes every grant that X holds and root did not give,
including other players' grants (`server/step/commerce_admin.cpp`, `revoke`,
the branch `else if (issuer.kind == perm::HolderKind::Player && !architect)`
and the full-table loop after it). The design does not say this, and it
means an architect tidying up their own grants to a script also wipes out
other players' grants to it.

- Change that branch back to `else if (issuer.kind == perm::HolderKind::Player)`.
  A player's holder-only revoke, an architect's included, removes only the
  grants in `players.rows[who].grants_given` with that holder. The final
  `else` loop is then reached only by scripts, as before Phase 3.
- An architect still revokes another issuer's grant by `grant` id, or by
  `holder` and `scope`. The `allowed` lambda already permits both; keep it.
- `tests/commerce_admin_test.cpp` around line 430 ("architect holder
  revocation removes another issuer's grants..."): revoke the ordinary grant
  by `{"grant": "g:N"}` instead. Before that, add a check that
  `run(4, ids.revoke, {}, {{"holder", "p:2"}})` leaves the ordinary grant
  live.

#### 4.F2 `MAILBOX_MAX` is a setting, not part of `Players`

`perm::Players` must hold only tables. Remove `std::size_t mailbox_max` from
`Players` (`server/store/store.hpp` around 1279) and its assignment in
`run.cpp` around line 54. Pass the limit as a scalar, the way `give` and
`accept` already do:
- `void notify(perm::Players &, net::Sessions &, std::size_t mailbox_max,
  PlayerId, Json)` (`step.hpp:262`, `commerce_mailbox.cpp`);
- add `std::size_t mailbox_max` after `limit` to
  `advance_permission_deadlines` (`commerce_admin.cpp:559`,
  `run_private.hpp:61`) and to `expire_sale_offers` (`commerce_offers.cpp:200`,
  `run_private.hpp:64`), and pass `settings.mailbox_max` from `run.cpp` at
  lines 367 and 369;
- `run.cpp` lines 506 and 666 pass `settings.mailbox_max`;
- `tests/commerce_test.cpp`: drop `f.players.mailbox_max = 2` and
  `restored.mailbox_max = ...`, and pass `2` to each `cm::notify` call.

#### 4.F3 The worker transport test assumes an ordering it does not wait for

`elaborate_worker_transport` fails about one run in four, also when run
alone: "slow fixture does not block other sessions"
(`tests/elaborate_worker_test.cpp:47`). The slow job's timeout is 100 ms
(line 32). The other worker's three subprocess jobs can take longer than
that, so the slow job's timeout comes back first and the test reads it as
blocking. Set `slow.limits.timeout_ms = 2000`; the loop already waits 5 s.
Do not change `FIXTURE_TIMEOUT_MS`.

#### 4.F4 Plot membership checks the object's current plot first

`perm::update_plots` (`server/perm/perm.cpp:41`) scans every plot for every
object it is given. `move_part` calls it for each part it moves
(`server/step/physical_actions.cpp` around 566), so the aquarium school's
`aq.swim` scans all plots for each of its 300 fish ten times a second. Today
there are 7 plots; after task 4.1 the plot count grows with every surveyed
block, and this cost grows with it.

In `update_plots`, before the loop over `plots.rows`: if the object's current
plot `membership.plot[slot]` is not 0, is below `plots.rows.size()`, has
`plots.rows[current].id == current`, and contains the point, keep it and skip
the scan. Plots never overlap (S13.1), so the answer is the same. This is a
check on the common path, not a new index. Add a perm test: an object moved
within its plot keeps the plot; moved onto another plot it gets the new one;
after its plot row is cleared (`id = 0`) it gets the plot the scan finds.

#### 4.F5 The handler limit counts CPU time, not time on the clock

Read S0.3 (`HANDLER_MS`) and S4.3 again; both changed. A handler run is cut
off once it has used `HANDLER_MS` of CPU time on the step thread. Today every
check compares `steady_clock::now()` with a `time_point`, so when the host is
busy and the OS pauses the step thread mid-handler, an ordinary handler is
cut off. Measured on this host: the aquarium's `aq.swim` (300 fish) takes
1-2 ms, and it is cut off at 5 ms several times per journey when the CPU is
shared. This is why `game_spatial`, `game_live_authoring`,
`game_authoring_restore` and the `physical_causality` ctest fail under load.

Add one type to `server/store/store.hpp`, above `struct Invocation` (line
852), with its two functions defined in `server/store/store.cpp`:

```cpp
// A script run's time limit (S4.3): CPU time used by the thread running it.
struct RunDeadline {
  std::chrono::steady_clock::time_point wall = std::chrono::steady_clock::time_point::max();
  std::chrono::nanoseconds cpu_end{};
  static RunDeadline after(double milliseconds); // starts now, on this thread
  bool passed();                                 // may move `wall` out
};
```

- `after(ms)`: read the thread's CPU time with
  `clock_gettime(CLOCK_THREAD_CPUTIME_ID, ...)`; set `cpu_end` to that plus
  `ms`, and `wall` to `steady_clock::now()` plus `ms`.
- `passed()`: if `steady_clock::now() < wall`, return false (the cheap common
  case). Otherwise read the thread's CPU time. If it is at least `cpu_end`,
  return true. Otherwise set `wall = now + (cpu_end - cpu)` and return false.
- A default `RunDeadline` never passes (`wall` is the maximum). It replaces
  today's two meanings: "unset" in `Invocation`/`InvocationInput` and "no
  limit" in `QueryFilter`. Where code tests "is a deadline set", test
  `wall != time_point::max()`.
- `after` and `passed` must run on the same thread. Every site below does:
  handlers and native queries on the step thread, and the declaration run on
  whatever thread loads the script.
- Copies are safe: `cpu_end` is absolute, so a copy whose `wall` was not
  moved only reads the CPU clock once more.

Replace the type and every check. There is no other deadline mechanism
afterwards; do not keep a wall-clock path.
- Fields: `Invocation::deadline` (`store.hpp:867`),
  `InvocationInput::deadline` (898), `NativeCall::deadline` (1631),
  `space::QueryFilter::deadline` (`server/space/space.hpp:179`).
- Creation: `handler_deadline` in `server/step/run.cpp:17` becomes
  `RunDeadline::after(settings.scripts.handler_ms)` at its two call sites
  (431, 458), and the helper is deleted; the default in `run_handler`
  (`server/react/vm.cpp:939-941`); the declaration run in `vm.cpp:846` with
  `DECLARATION_MS`.
- Checks, each `steady_clock::now() >= X.deadline` becomes
  `X.deadline.passed()`: `vm.cpp:124` (interrupt), `973`, `1046`;
  `server/step/metadata.cpp:234`; `server/step/physical_actions.cpp:642`,
  `648`, `663`; `server/space/space.cpp:262`, `280`, `1220`, `1230` (the
  `Rays` and `Candidates` collectors hold a `RunDeadline` member instead of a
  `time_point`; `filter` is already taken by value, so call `passed()` on it).
- `vm.cpp:909`: a run that ended in an error is `Cutoff` if
  `inv.deadline.passed()`, otherwise `Error`.
- Pass-through sites only change type: `run.cpp:713`, `actions.cpp:50` and
  `194`, `react/execution.cpp:85`.
- Do not touch `server/step/phase.cpp:50` (`frame.handler_deadline`). That is
  the step budget of S2.3, and it stays on the clock: a busy host makes the
  world slower there, not by cutting scripts off. Do not touch the fixture and
  elaboration process timeouts in `server/elaborate/process.cpp`.

Tests (react):
- `RunDeadline::after(5)`, then `std::this_thread::sleep_for(20ms)`:
  `passed()` is false (sleeping uses no CPU). Then spin until `passed()`;
  the thread's CPU time since `after` is at least 5 ms.
- A handler `while true do end` run with the configured limit is still
  `Cutoff`.
- A handler given a `RunDeadline` whose `wall` is already in the past but
  whose `cpu_end` is 5 ms ahead completes with `Ok`.
- `world.near` and `world.raycast` still stop early when the deadline has
  passed (keep the existing deadline tests; change their setup to a
  `RunDeadline` whose `cpu_end` has passed).

Done when, in addition to the usual checks:
- `ctest --preset debug -j8` passes five runs in a row;
- all journeys pass while `ctest --preset debug -j8 --repeat until-fail:3`
  runs at the same time to load the host, with no `cutoff` records in any
  journey.

### 4.1 Layout and surveying

Add the constants of S0.3 to `config.toml` and to the loaded settings struct:
`STREET_WIDTH_M = 100`, `PORT_SPACING_M = 1000`, `BLOCK_M = 72`, `WAY_M = 8`,
`LOT_FRONT_M = 16`, `TERRAIN_TILE_M = 256` and `POOL_LOTS_MIN = 4`.

Add `server/step/land.cpp` with two functions:
- `survey_block(perm::State &, space::State &, const LandSettings &, Block)`
  creates one block's plots and terrain;
- `fill_pool(perm::State &, space::State &, const LandSettings &)` surveys
  blocks until the pool is full, as below.

It follows S13.1 exactly:
- Blocks are `(i, j)` with `j != 0`. Block `(i, j)` for `j >= 1` covers
  `i * BLOCK_M <= x < (i + 1) * BLOCK_M` and
  `STREET_WIDTH_M / 2 + (j - 1) * BLOCK_M <= y < STREET_WIDTH_M / 2 + j * BLOCK_M`.
  `(i, -j)` is its mirror across `y = 0`.
- In a north block:
  - the alley is `x < i * BLOCK_M + WAY_M`, full depth;
  - the back way is the last `WAY_M` of y, for the rest of x;
  - the lots are two rows of `LOT_FRONT_M` wide lots, each half the remaining
    depth.

  Mirror this for south blocks.
- The Street piece of column `i` is
  `i * BLOCK_M <= x < (i + 1) * BLOCK_M`,
  `-STREET_WIDTH_M / 2 <= y < STREET_WIDTH_M / 2`. Create it with the first
  block of that column.
- Alley and back-way plots are kind `alley`. The Street piece is kind
  `street`. All three are ways: owner 0, preset everyone `enter` and
  `run_scripts`, group = the start file's `"land": {"way_group": 1}`, with the
  group bit for `build`.
- Lots are kind `lot`, owner 0, preset everyone `enter`.
- Choosing the next block:
  - the candidate blocks are the unsurveyed ones whose neighbor toward the
    Street in the same column (`j - 1` for north, `j + 1` for south) is
    surveyed, or which have `|j| = 1`;
  - for Street lots, only `|j| = 1`;
  - pick the candidate whose center is nearest the origin;
  - ties go north before south, then to the higher `i`.
- Surveying keeps the pool at least `POOL_LOTS_MIN` lots of each kind. Street
  lots are lots fronting the Street; back lots are every other lot. Survey
  one block at a time until both kinds have enough pool lots.
- Plot ids follow survey order.

Survey at world creation:
1. First, the blocks of the lots the start file gives to players (below).
2. Then until the pool is full.

Also survey after each sale of a pool lot (Phase 5 calls it). Do not survey
on restore: plots are saved.

There is no stored survey state. A block is surveyed if any plot lies in it.

Delete `"plots"` from the start file and from `install_land`. In its place,
`"land"` has `"way_group"` and an optional list
`"lots": [{"at": [x, y], "owner": N, "starts": [...]}]`. Each `at` is a lot's
minimum corner. `install_land` surveys, then sets each named lot's owner.
Refuse a start file whose `at` is not the minimum corner of a lot.

Order at world creation (`run.cpp:211-222`): `install_land` must run before
`install_objects`, because objects are assigned to plots by position. Delete
the objects' `"plot"` field from the start file and from `start_objects.cpp`
(around line 25). After all objects are installed, call `perm::update_plots`
for every object. `tests/test_content.py::test_body_pours_into_a_model_without_container_bindings`
reads `body["plot"]` (around line 459); compute the plot from the position
instead.

### 4.2 Terrain

Terrain is not an object (S13.1). Put it in `space::State`:
`std::set<std::pair<std::int32_t, std::int32_t>> terrain`, the tiles that
exist.

Add `space::add_terrain(space::State &, std::int32_t i, std::int32_t j)`:
- It creates one Jolt static box body, `TERRAIN_TILE_M` square and 1 m thick,
  with its top at `z = 0`, on the static layer, with `mUserData = 0`.
- Today every body has an object handle. The paths that look a body up by
  handle already skip unknown bodies (`hits()` `space.cpp:282`, `near()`
  1239, `wake()` 600, the navigation gather `navigation.cpp:309`). Keep
  that, and make sure contacts with user data 0 are ignored in the contact
  listener (`space.cpp:556-564`).
- It adds two triangles for the tile's top face to navigation:
  - in `install_ready_navigation` (`navigation.cpp:300-326`), add a
    walkable-surface `NavInstance` for every terrain tile that overlaps the
    job's region, with geometry built once for the tile size;
  - mark the tile's bounds dirty with `dirty_navigation`.

Surveying adds the tiles that any new plot overlaps and does not yet have.
On restore, rebuild terrain from the plots before navigation runs: every tile
any plot overlaps. Terrain is not saved.

Send terrain to viewers (S6.2):
`{ t: terrain, tile_m: 256, tiles: [[i, j], ...] }`. Send it after `welcome`
(`server/step/connections.cpp`, `connection_notice`), and send the new tiles
to every viewer when a survey adds any. The gateway ignores it.

The client is Godot (`client/`):
- `main.gd _text()` (around 420-482) handles `terrain`;
- `world_view.gd` adds one flat `PlaneMesh` per tile, in a neutral ground
  color, at `z = 0`, below objects.

Add a case to `client/tests/protocol_test.gd`.

### 4.3 Outdoor light

Add the registry term `base.outdoor_lux` (quantity, setting, dimensionless)
and `world_settings` `"base.outdoor_lux": 120`.

Replace the hard-coded world ambient of 0 with this setting:
- `illumination.cpp:295`;
- `run.cpp:298`;
- `connections.cpp:188-189` and `305-306`.

Send it to clients as `world.ambient_lux` in the `catalog` message, which
`client/main.gd:439` already reads (`connections.cpp:24-27`,
`lifecycle.cpp:246-253`).

### 4.4 The seed world on the new layout

Move the seed world into the new layout, all in the start file:
- Delete the objects `public.street`, `public.forecourt` and both
  `public.ground`, and the models `street`, `forecourt`, `land_seeded` and
  `land_forage`. Delete `content/models/street.py` and
  `content/models/land.py` if nothing else uses them; check `slab.py`
  before deleting it.
- Move every remaining object by `(+16, +48, 0)`. This puts:
  - the café on the Street lot whose minimum corner is `(8, 50)`;
  - Sol's collector on the lot at `(24, 50)`;
  - the residents and public objects on the Street.

  Before you commit, check with a script that every moved object lies in the
  plot you expect.
- Lots for the seeded mode:
  - Mira (2) `(8, 50)`;
  - Sol (3) `(24, 50)`;
  - Ivo (4) `(40, 50)`;
  - Nia (5) `(56, 50)`;
  - Rue (6) `(-16, 50)`;
  - Oren (7) `(-32, 50)`.

  The forage mode gives no lots.
- In start-file grants, a plot scope may be written `{"plot_at": [x, y]}`.
  `install_grants` replaces it with `{"plot": id}` of the lot whose minimum
  corner is `(x, y)` before calling `step::scope`, and refuses the start file
  if there is no such lot. Rewrite the Phase 3 `build` grants for the
  residents' scripts with `plot_at` and the lot corners above. Plot ids follow
  survey order, so never write them by hand.
- The café floor's top and the terrain are both at `z = 0`. Leave this; it is
  cosmetic.

### 4.5 Plot records and host calls

`inspect('land')`, `inspect('plot')` and `inspect('plot:N')` return the
record of S5.1:

```
{ plot, kind, owner, bounds, area_m2, fronts, port, port_x, dist_m,
  bearing, for_sale, rights }
```

- `bounds` is `[x0, y0, x1, y1]`, replacing today's `{min, max}`
  (`read_inspect.cpp:92-99`).
- `fronts` is `street` if a cell of the plot shares an edge with a street
  plot. Otherwise it is `back way` or `alley` by the first way plot it
  shares an edge with (a back way runs along x, an alley along y). If it
  shares an edge with none, it is `none`.
- `port` and `port_x` belong to the port nearest the plot's center.
- `dist_m` and `bearing` are from the caller's body to the nearest point of
  the plot. They are 0 when the body is on it.
- `for_sale` comes in Phase 5; leave it out until then.
- `rights` lists every live grant on `Scope::plot(id)`, and every root grant
  on all land if the plot is a lot, that has `transfer` or `return`. Show its
  holder and verbs.
- `land` lists plots whose rectangle is within `SIGHT_M` of the body, nearest
  first. Put the `land` case before `plot:` in `read_inspect.cpp`
  (around 275-299).
- Delete `plots` (the caller's own plots); nothing in the doc has it. Change
  `tests/game_live_authoring.py` (`place`, around 255) to use
  `inspect('land')` and the first plot owned by the caller.

Script host calls (S4.2), in `server/step/metadata.cpp` next to the existing
`plot` and `plot_objects` bindings:
- `self:plot(id)` returns the same record without `dist_m`, `bearing` and
  `for_sale`;
- `self:plots()` returns every plot id;
- `self:plot_objects(id)` stays as it is.

Remove these three from "Not in this brief".

Done when: tests show all of the following:
- a fresh forage world has the Street piece, the alley, the back way and
  8 lots of block (0, 1), and the lots have the bounds S13.1 gives;
- after a test sets owners on all four Street lots of a block and calls
  `fill_pool`, the next |j| = 1 block is surveyed, with the expected ids;
- terrain exists under every plot, a body stands on it, and a path is found
  across it;
- terrain is rebuilt identically after a snapshot restore;
- outdoor things are lit at 120 lux;
- each seeded resident owns the expected lot;
- `inspect('land')` near the café lists the café's lot first, and `fronts` is
  `street`.

## Phase 5: the land market

### 5.1 A change of owner

Add `perm::change_plot_owner(perm::State &, PlotId, PlayerId owner)`:
1. If the old owner is not 0, remove every live grant issued by the old owner
   whose scope is `Scope::plot(id)` or `Scope::owned_on_plot(old, id)`, with
   their children (`remove_marked`). Return the removed ids.
2. Close every live offer on this plot.
3. Set the owner.

Callers send the old owner a notice of the removed grants, as in
`commerce_detail::cleared_notice`, and write a record with kind system and
name `plot_owner`.

Only `accept` of a plot offer (5.2) and `engine.abandon_plot` (5.4) call it.

### 5.2 Plot offers and prices

`perm::Offer` (`store.hpp:1398`) gets `PlotId plot = 0` and `Holder maker`.
Write both in snapshots (`perm/snapshot.cpp` 149-159, and the offer selection
at 80-87 must also select the maker's player or script object).

Set `maker` for object offers too: it is the actor.

New action `engine.offer_plot {plot, price, buyer?, expires_s?}`:
- right `""`, so it needs no target;
- the plot must be a lot;
- the actor must pass `check_land(actor, Transfer, plot)`;
- refuse a second live offer on the same plot;
- the offer's `seller` is the plot's owner;
- declare it in `commerce_schema.cpp` like `engine.offer`, with a validator,
  a field in `CommerceDeclarations` (`step.hpp:244`), dispatch in
  `native_actions.cpp`, and a new `ExchangeAction` value;
- its result is `{"offer": "offer:N", "plot": N}`.

New action `engine.set_offer_price {offer, price}`, right `""`:
- allowed to the offer's maker, or to a player who is its seller;
- not allowed for a trade's offers;
- record name `set_offer_price`.

`engine.accept` gets a required `max_price`:
- In `perm::accept`, refuse with a new `Reason::PriceChanged` (add its text
  to `reason_text`) when `row.price > max_price`, before anything changes.
- Update the schema (`commerce_schema.cpp` around 186-191), the validator
  (85-94) and the callers: `tests/commerce_test.cpp:250`, `257` and `262`,
  and `tests/perm_test.cpp:444` and `812`.

`engine.accept` of a plot offer is a separate branch in
`commerce_offers.cpp::accept` and a new `perm::accept_plot`. It checks, in
order:
1. the offer is live and not expired;
2. the buyer matches;
3. `max_price`;
4. the plot's owner is still `row.seller`;
5. `check_land(row.maker, Transfer, plot)` still passes.

Then it pays the price with `pay` from the payer to
`Account::player(row.seller)`. For a pool lot that is root, so the money is
burnt. Then it calls `change_plot_owner` to the beneficiary and closes the
offer. The beneficiary must be a player. There is no presence or distance
check.

After a pool lot is sold, call `fill_pool`.

`engine.offers` lists plot offers with `"plot": N` instead of `"goods"`.

A plot record's `for_sale` is
`{offer: "offer:N", price, buyer: "p:N" or null}`. It appears only when the
offer's buyer is 0 or the caller.

### 5.3 Returning

New `perm::return_goods(perm::State &, const ReturnRequest &, Step)`, where
`ReturnRequest` holds `Holder actor`, `Target goods`,
`std::span<const Target> parts`, `PlotId plot`, `Handle owner_body`,
`bool present` and `bool forced`.

It refuses, unless `forced`:
- a body;
- goods owned by the actor;
- goods whose owner holds a live `build` grant on `Scope::plot(plot.id)`
  (direct or passed on: look up `(Holder::player(owner), Scope::plot(id))`);
- an actor who is neither the plot's owner nor passes
  `check_land(actor, Return, plot)`.

It ignores `MAILBOX_MAX` and `Escrow`. If the goods are in escrow, it closes
their offer.

It moves the goods like `store_goods`: to the inventory when present,
otherwise to the mailbox.

New action `engine.return {}` with a target and right `""`. The handler
(new file `server/step/returns.cpp`) works like `hold_goods`
(`server/step/goods.cpp`):
- collect declared descendants;
- check `space::can_custody` with the owner's body as custodian;
- if that refuses, check it again with no custodian (mailbox);
- if that still refuses, the occupants that block it stay where they are.

For the last point, add a `stay_static` flag to `space::can_release_occupants`
and `space::release_occupants` (`server/space/custody.cpp:12-62`), used only
by returns. With it, an occupant that has no hull, or no native capacity,
keeps its pose and stays static instead of refusing. Bodies seated on the
goods are released as today and fall. Bodies standing on it fall when the
goods leave the world, as today.

A return must never be refused for a physical reason. Add a test for each
case: a seated body, a standing body, a static attached thing with no hull,
and an absent owner with a full mailbox.

### 5.4 Abandoning, splitting and joining

`engine.abandon_plot {plot}`, right `""`:
- the actor must be the owner, or pass `check_land(actor, Transfer, plot)`;
- the plot must be a lot not owned by 0.

First, return everything on the plot with `return_goods(..., forced = true)`.
Use `plot_objects`' selection: not bodies, not held, not declared parts. Then
call `change_plot_owner(..., 0)`.

`engine.split_plot {plot, x}` or `{plot, y}`, right `""`. The actor is as
for abandoning, and the plot must be a lot. The cut is a whole meter strictly
inside the bounds. Then:
- the plot keeps its id for the lower half;
- the upper half gets the next id, with the same owner, group, preset and
  banned list;
- copy every live grant whose scope is `Scope::plot(id)` or
  `owned_on_plot(*, id)` for the new id, with the same issuer, holder, verbs,
  cap, spent, period and expiry, and copy its children under the copies;
- close offers on the plot;
- re-run `update_plots` for the objects on it;
- refuse a cut that would leave a piece with no cell.

`engine.join_plots {plots: [a, b]}`, right `""`:
- both plots are lots with the same owner;
- the actor is as for abandoning, for both plots;
- they share a whole edge, and the union is a rectangle.

The lower id keeps the union. Remove every grant scoped to the other plot,
with children, and notify their issuers. Close offers on both plots. Delete
the other plot's row (`id = 0`); ids are never reused. Re-run
`update_plots`.

Each of these writes a record and is in `player_actions` automatically
(`read_look.cpp:75-86`).

### 5.5 The land script

Add a model `land_post` (`models/equipment.py`, `"group": 1`) whose script
is `content/scripts/land.luau` (kind `model`). Add an object
`public.land_post` on the Street near Port Zero, owned by 1, group 1, with all
seven group rights, and a bootstrap `base.start` row.

Root grant to `land` on `public.land_post`: scope `{"all_land": true}`,
rights `["transfer", "destroy", "land_transfer"]`, `expires_s` 31536000.

In the seeded mode, each resident gives `land` on `public.land_post` a spend
grant on their wallet: cap 10000, `period_s` 86400, `expires_s` 31536000.

Registry terms (quantity, setting) and `world_settings` defaults, from S13:
- `base.land_base` 1;
- `base.land_front` 9;
- `base.land_back` 1;
- `base.land_along` 250;
- `base.land_depth` 16;
- `base.land_port_falloff` 0.01;
- `base.land_rent_rate` 0.01;
- `base.land_tier_value` 5000;
- `base.land_tier_multiplier` 2;
- `base.land_scarcity` 1;
- `base.land_on_offer` 2;
- `base.land_start_multiple` 3;
- `base.land_halving_days` 1.

Copy each term row's layout from `base.rent_grace_days`. Add an event term
`base.land_tick`.

`land.luau` does exactly S13.2 to S13.4. Every `base.settlement_seconds`:
1. Read every plot with `self:plots()` and `self:plot(id)`. Keep each lot's V
   in state, keyed by id and bounds, and compute it again when the bounds
   change. For a lot:
   - cells are 1 m;
   - `n` and `port_x` come from the record;
   - `a = |x + 0.5 - port_x|`;
   - for a Street lot, `d` is the cell's distance in whole meters from the
     lot's edge that touches the Street: `y - min_y` for a lot north of the
     Street, `max_y - 1 - y` for one south of it.
2. Rent. For each owned lot in id order, with W the sum of V over the owner's
   lots, compute f(W) and the rent per day exactly as S13.3. Add rent times
   elapsed days to the debt. Pay the whole units with
   `engine.pay {from = 'p:N', to = 'p:0', amount}`. If the payment fails, the
   debt stays.
   - When the debt reaches 1, note the time.
   - When the debt falls below 1, clear the note.
   - After `base.rent_grace_days` at 1 or more:
     1. hold the owner's things on the lot, as the old `rent.luau`
        `hold_contents` did: grant itself `transfer` and `destroy` on each
        object, then `engine.hold_goods`;
     2. `engine.abandon_plot`, which returns everyone else's things;
     3. salvage the held things after `base.rent_hold_days`, keeping the
        held-mass bookkeeping (F.1).
   - When a lot's owner differs from the stored owner, reset its debt to 0.
3. Sales, as S13.4:
   - the classes are Street lots and back lots;
   - compute s and K per class;
   - keep `land_on_offer` pool lots of each class on offer, lowest id first,
     with `engine.offer_plot`;
   - store each offer's time, and set its price with
     `engine.set_offer_price` when it changes;
   - forget a lot's offer time once it is sold.
4. Writing. Set its own object's writing with
   `engine.edit_text {text = {writing = ...}}`. One line per owned lot:
   `plot N: V v, rent r a day (rate, tier)`. One line per pool lot on offer:
   `plot N: for sale at P`.

Tests (`tests/test_content.py`, in the style of the deleted rent test, with
short days):
- the reference values of S13.2: 2440.01, 992.34 and 1476.01, computed by the
  script for lots built at those positions;
- the tier: one owner of two Street lots pays more than two owners of one
  each;
- rent paid to root, and the money gone from circulation;
- an unpaid lot returned to the pool after the grace period, with the
  owner's things held and later salvaged, and a third party's thing returned;
- a new owner's debt starting at 0;
- in the forage mode, two lots of each class on offer;
- a price falling from the start to the floor at the expected times;
- a sold lot replaced on offer by the next one, with a higher K;
- a lot bought by a player far from it, then charged rent;
- the posted writing lists the rent.

### 5.6 The lease script

Add `content/scripts/lease.luau`, kind `instance`. Its slots are `build`,
`enter`, `run_scripts` and `return`. In `editing_scripts.cpp`, `preview`
(around 210) maps `return` to the object's plot as it does `build`.

A plot owner installs it on any object of theirs on the plot with
`engine.install`. It declares two actions, with action terms in the
registry:
- `base.lease_terms {tenant: string, rent: number, grace_days: number}`,
  right `edit`. The handler stores the terms only if `caller` is the plot's
  owner (`self:plot(...)` of its object's plot).
- `base.lease_sign {}`, right `use`. The handler accepts only if `caller` is
  the stored tenant.

Then every `base.settlement_seconds` (event term `base.lease_tick`):
1. If the lease no longer holds `build` on the plot (`engine.list_grants`),
   or the plot's owner changed, it ends. It stops charging and does nothing
   more, without an error.
2. If signed and the tenant has no build grant from it, pass on `build`,
   `enter` and `run_scripts` to the tenant with `engine.grant`. The grant's
   expiry is no later than its own.
3. Charge the rent with `engine.pay {from = tenant, to = owner}`. The tenant
   gives the lease a capped spend grant on their wallet when they sign.
4. After `grace_days` unpaid:
   1. revoke the tenant's grant;
   2. `engine.return` each of the tenant's things on the plot.

Tests:
- a tenant paying the owner and keeping build rights;
- terms refused from someone other than the owner;
- a signature refused from someone other than the tenant;
- an unpaid tenant evicted, with their things returned, while the plot stays
  the owner's;
- the owner still owing land rent;
- the lease ending without an error when the plot returns to the pool.

Done when: the checks pass, every test above passes, and `game_live_authoring`
and `game_authoring_restore` pass.

## Phase 6: joining and the operator

### 6.1 Joining

Protocol (S6.2): `{ t: join, version: 1, name, persona }` parsed in
`server/net/protocol.cpp` next to `hello` (around 80-103). The server replies
`{ t: joined, player: "p:N", key }`, then the session continues exactly as
after `hello` in play mode. A refused join is closed with
`{ t: bye, reason: refused }`.

Add one function, `bool join_allowed(const net::JoinRequest &)`, in
`server/net/sessions.cpp`. It returns true. Call it before anything is
created. Write a record with kind system and name `join_refused` when it
refuses. Do not add a rate limit, a captcha or any other check.

The join is one input on the step thread, like other inputs. It:
1. makes the next player id;
2. creates a key the way `net::create_key_file` does (32 random bytes as
   hex) without writing a file, and stores only its hash with
   `net::put_profile`;
3. stores `persona` on the profile, and shows it in `inspect('self')`;
4. resizes `perm::Players`;
5. creates a body object from the start file's
   `"newcomer": {"body_model": "body", ...}`, owned by the player, at
   `(0, 0, 0)`, with flags `body`. Reuse what `install_objects` does for one
   object;
6. issues the start file's `"newcomer"` `"body_installs"` from the player to
   the body's script. Reuse `editing::detail::slot_choices` and `fill_slots`,
   as `provision_body_grants` does;
7. pays `"newcomer": {"money": N}` from root, if N > 0;
8. sends the `base.start` bootstrap event to the body;
9. writes a record with kind system and name `join`.

Its newcomer block, in both modes:

```
"newcomer": {"body_model": "body", "money": 0, "body_installs": [
  [{"slot": "animate", "scope": {"avatar": "$body"}, ...},
   {"slot": "camera", "scope": {"avatar": "$body"}, ...}],
  [{"slot": "use", "scope": {"owned": "$player"}, ...}, ...]]}
```

`$body` and `$player` are replaced at join. Use the same rights, expiry,
cap and period as the residents' rows. Profiles in the start file keep their
own `body` and `body_installs`.

Gateway:
- an unauthenticated `POST /join` in `gateway/http.py`, before `player_key`;
- `Sessions.join(name, persona)` in `gateway/session.py`, which opens a
  websocket, sends `join`, and returns the key;
- `amb join NAME --persona TEXT --key-file PATH` in `gateway/cli.py`, before
  the key check (around 96-98). It writes the key to PATH with mode 0600.

### 6.2 The operator

At world start, the server creates `.runtime/keys/operator.key` like a
player key, but it maps to no player.

A session that sends `{ t: operator, version: 1, key, op, ... }` with that
key may:
- `{op: "role", player, role, on}`: set or clear `architect` or
  `elaborator` in the profile's roles. For `architect`, also add or remove
  the player in the architects group;
- `{op: "key", player}`: replace the player's key and return the new one
  once.

Each op writes a record with kind system and name `operator`.

Add `amb operator role p:N architect on|off` and `amb operator key p:N`.
They read the operator key file.

Done when: tests show all of the following:
- a join creates a player whose body stands at Port Zero, holds the newcomer
  grants, has 0 money in forage, and can `look` and `do`;
- a second join gets the next id;
- a refused join, with `join_allowed` stubbed in the test, creates nothing;
- an operator gives the architect role and group membership at runtime, and
  the new architect can then edit a group-1 model;
- a replaced key works and the old one gets `bad_key`;
- `amb join` writes a key file that `amb look` then uses.

## Phase 7: the journeys

Every `tests/game_*.py` journey must pass on the new seed world. Expect
changes in these:
- `game_body_permissions.py`: objects on `plot=3` (around 46), and the owned
  scope (around 65 and 114);
- `game_actions.py`: the landlord grant (around 310);
- `game_live_authoring.py`: `place` (around 255);
- `game_deadlines.py`: pays p:1 (around 111 and 138), which still works;
- `game_spatial.py`: the architect rebinds the `breaker` and `pump` models,
  which still works because the architect is their author.

Positions in journeys that assume the old layout move by `(+16, +48, 0)`.
Do not loosen any check. Report each journey's result and evidence path.

## Phase 8: remove unspecified derived state

Each item is one commit. Bump `runtime_version` once, in the
first commit that changes the snapshot layout. Old snapshots are rejected; no
migration.

1. Remove cause tokens from physics. Delete `MechanicalCauses`,
   `server/space/causality.cpp`, `CauseToken` from every space API, the motion
   cause rows in MOVE and the `causal_ambiguity` records. A contact, zone or
   support event carries the Origin of the move_to intent of the body that
   moved, if it has one, and otherwise an empty Origin (cause null).
2. Remove `TravelAccess` (`server/step/travel.cpp`, `step.hpp:39-51`). Compute
   forbidden plot boundaries when a move_to starts or replans. A grant that
   expires during a walk takes effect at the next replan or the next action.
3. Replace per-binding `dependencies` and `refresh_dependencies`
   (`server/react/install_watches.cpp:257`) with S1.5's rule: for a term with
   `along`, a change on subject s wakes s and every object in `in[s]` of that
   relation. No loop over all subscriptions.
4. Stop saving pending network `Inputs` in snapshots
   (`server/step/checkpoint.cpp:91-103, 607-626`). After a restore, sessions
   reconnect and resend.
5. Do this item last. It is the largest; if any rule test's expected behavior
   would change, stop and report instead of changing the test. World rules get
   no per-object watch rows (Technicals 12: "nothing is attached to objects").
   Today `reconcile_effects` (`server/step/handlers.cpp`) calls
   `react::sync_rule_watches` after every handler. That rebuilds
   `latest_rules(rules)` from the whole history and binds watch rows on each
   matching object (`server/react/install_watches.cpp:205-252`). Instead, a
   changed `(subject, term)` dispatches like any trigger (S3.4): look up
   `Triggers[term]`, then run `by_material[material(subject)]` if enabled, the
   matching `by_flag` entries, `by_model` and `by_instance`. Delete
   `sync_rule_watches`, `rule_watch_terms` and the rule watch rows in
   `plan_background` (`server/step/plans.cpp:130-143`). `latest_rules` may
   remain only on rule activation and rule answer paths.

Done when: the checks pass, `grep -rn "CauseToken\|MechanicalCauses\|
TravelAccess\|refresh_dependencies\|sync_rule_watches" server tests` finds
nothing, and `latest_rules` is called only from `server/react/activation.cpp`
and `server/react/install_dispatch.cpp`.

## Not in this brief

Do not touch these; they need a ruling from the owner first:
- `AllocationRows` and `MeterRows` (with `self:meter`). They are coupled: each
  allocation change also updates a meter (`server/react/values.cpp:379-420`).
- `AssemblyMasses`, and where illuminance lives.
- The `self:object` host call.
- Whether `engine.transfer` and `engine.impulse` should exist at all, or be
  replaced by events.
- The rest of the bake `BINDINGS` declaration (character mass and size, door
  motion, the tank's containment `shape`), and stair checks for fused solids.
- The navigation corridor code and the fixture process layers.
- The duplicate `validate_*` functions.
- The monorail. It is content that architects build later.
- Terrain heightmaps.
- Any check in `join_allowed`, such as a captcha or a puzzle.
