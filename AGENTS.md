# Ambergris development instructions

## Source of truth

Read the original [Technicals](docs/02-t.0.md) and the relevant
[Specs](docs/04-t.de137kjm03j.md) before changing a subsystem. Technicals wins
where they disagree. Generated architecture, API and status documents describe
an implementation; they cannot amend these requirements or authorize exceptions.
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

- Follow S0.2's cohesive modules and direct calls. Pass the specific tables a
  function uses, with const reads. Never pass a whole server, store, world,
  service locator, or universal host context.
- Do not simulate module boundaries with callback/interface tables. Keep C
  callbacks required by Luau or another library local to that integration.
- A handler changes its own object and parts. Foreign effects use a declared
  contribution, an event, or an ordinary action checked against the script's
  grants. A triggering player's authority is not inherited by the reaction.
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
- Keep documentation proportional to the implementation. Generated benchmark
  outputs and public-data projections belong in .cache, not source directories.
- State failures, ambiguities and remaining work plainly. Do not call the
  prototype complete while required behavior or acceptance work remains.
