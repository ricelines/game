# Ambergris development instructions

## Source of truth

The design doc is the only source of truth:
https://docs.google.com/document/d/1zJ2WDWIEMRXXinCnkyQ4KbKp2xhuI_rLnsroY57X0Mg.
Read its Technicals tab (`t.0`) and the relevant parts of its Specs tab
(`t.de137kjm03j`) before changing a subsystem. Technicals wins where they
disagree. Never edit the doc, and keep no local copies of it.
The user requires a playable, maintainable modular monolith without unnecessary
machinery. A passing implementation test is not proof of design compliance.

## Non-goals

The following come from the Technicals' non-goals and consistency rules:

- No pluggable engines or services, distributed transactions, or atomicity across
  regions. One region and one writer are enough for this prototype.
- No transaction framework, compare-and-swap state exchange, prepare/commit
  protocol, versioned mutable rows, rollback, or immutable world structures.
- No determinism, replay or reproducibility guarantees for gameplay.
- Measure rarely and coarsely. No per-operation cost accounting or continuous
  owner/job metering. Keep the explicitly specified script and overload limits.
- No new scale target, dynamic sharding, or compatibility with industrial asset
  pipelines. Do not build infrastructure for hypothetical future requirements.
- Agents use text actions, not cameras or keyboards.
- No required fork-and-continue or stronger ordinary-state crash safety than a
  normal game. Snapshots adapt to the store, not the reverse.
- No expensive gameplay with an equivalent cheap substitute, such as a script
  per fish. A handler can move its object's parts.
- Do not try to prove an effect impossible when no rule exists. Log the gap.

The three consistency tiers are: valuables change hands all or nothing; a player
sees their own actions in order; everything else favors availability and speed.
Inline prerequisite checks followed by writes on the single writer are enough
for valuable exchanges. A handler error keeps effects already applied. Object
handle generations and explicitly versioned world rules remain required; they
do not justify versions on ordinary mutable rows.

## Boundaries and gameplay

- Follow S0.2's cohesive modules and direct calls. Each owner (store, catalog,
  react, perm, space, net, record, snap, elaborate, step) keeps its tables in
  one plain `State` struct declared in its own public header. A function takes
  the owner states it actually uses, `const` when it only reads, plus scalars.
  Pass a single table instead only when that is all the function touches.
  Never define or pass a struct that holds several owners' states, a service
  locator, or a host context. More than about eight parameters is a review
  flag: pass the owner state instead of listing its tables one by one.
- Do not simulate module boundaries with callback/interface tables. Keep C
  callbacks required by Luau or another library local to that integration.
- A handler changes its own object and parts. Foreign effects use a declared
  contribution, an event, or an ordinary action checked against the script's
  grants. A triggering player's authority is not inherited by the reaction.
- The user approved bounded script delegation to other recipients for autonomous
  leases and avatar consent. It requires an explicit parent grant, remains one
  level deep, narrows scope and rights, shares spending caps, and cannot outlive
  the parent. A script's installation owner does not confer grant authority.
- Keep the script API to S4 and the body-neutral mechanics explicitly required
  by Technicals 16. Do not add a second permission system through host calls.
  Body verbs remain authored scripts; no native strike/push/pour/sit scenarios.
- Keep content, zone names, institutional policy and body semantics out of the
  generic engine. Measured properties come from trusted measurement of geometry,
  never from declarations or code executed by the author.
- Each table has its specified primary order. Relations keep both directions.
  Add another index only after measuring a real hot path and documenting the
  precise index in the relevant spec. Prefer a loop on the rarer path.
- Match S5's observations, result shapes and gateway-owned session compaction.
  One-line action signatures support acting directly; schema discovery is
  optional. Protective-limit drops must be recorded as specified.
- Keep named constants in config.toml. Do not quietly move them into content or
  substitute different defaults. Leave unknowns to actual prototype measurements.

## Working practice

- Work only in this repository. Never read .env or potentially secret-containing
  files without permission. Do not put credentials or runtime keys into Git.
- Authorized OpenAI calls use /home/dev/.local/bin/with-openai-key. Never print,
  log, or directly decrypt the encrypted credential.
- Remove obsolete code and contracts when replacing behavior. No dormant
  alternatives, compatibility wiring, or unused abstractions.
- Review source changes against the original design, including staged and
  unstaged changes. Keep version control and reviewable changes; do not recreate
  a history that did not exist.
- Run appropriate formatting, lint, tests and focused benchmarks after source
  changes. Benchmarks must name workload and exclusions. Small tests, software
  rendering and unavailable data cannot stand in for unmet acceptance gates.
- Use the `dev` CMake preset for ordinary edit/build/test iterations. It keeps
  assertions and line information, uses `-g1`, and disables ASan/UBSan. Use
  `debug` for sanitizer checks, memory/undefined-behavior investigations, and
  interactive debugging that needs local variables. After substantial native
  changes, build `check-binaries` and run the full CTest suite with `debug`;
  passing `dev` tests does not replace sanitizer validation. Use optimized
  Release builds for performance measurements and gameplay checks whose real
  handler deadlines require optimized code; neither Debug profile establishes
  performance acceptance. Check the executable paths before integration runs:
  gameplay/content tools and the harness default to `.build/release`, regardless
  of the selected CMake preset. Build the actual binaries a test will launch.
- Keep native types in their owner's public header rather than collecting them
  in `store.hpp`. Put changeable non-template helper bodies in `.cpp` files;
  reserve header definitions for templates and small, stable value operations.
- Keep build directories between iterations. Queue every target needed for the
  current batch in one build invocation so Ninja can schedule their work
  together, for example `cmake --build --preset dev --target amb-server
  amb_script_validate commerce_test perm_test` using `.tools/bin/cmake`.
  Include required worker/probe executables in that same invocation before
  running the selected tests. Do not build targets one at a time or run multiple
  build processes against the same build directory. The build presets without
  explicit targets queue all validation binaries through `check-binaries`.
  Run all builds and tests needed for final validation after substantial batches.
- Do not add documentation files: no API, architecture, status, report or
  profiling documents. README.md says only how to build, run and test.
  content/authoring.md is game content that agents read, not documentation.
  Generated benchmark outputs and public-data projections belong in .cache.
- State failures, ambiguities and remaining work plainly. Do not call the
  prototype complete while required behavior or acceptance work remains.
