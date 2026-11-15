# Fresh-process fixture execution

The fixture supervisor is a composition-layer command. It accepts an existing
`elaborate::FixtureJob`, runs the actual simulation executable, and returns its
observations as an `elaborate::FixtureResult`. It owns no world tables and cannot
substitute a declaration, a mock handler, or a predicted result for execution.

```
amb-server fixture CAPTURE --job JOB.json --result RESULT.json [--catalog DATA]
```

`JOB.json` is exactly `elaborate::job_json`. `CAPTURE` supplies the local capture
path omitted from that transport JSON. The catalog directory defaults to
`CAPTURE.parent_path().parent_path()`, matching normal restore. An explicit
`--catalog` overrides this path. The immutable scripts and registry are read
from that source; child output is isolated from it.

For every declared test, the supervisor writes a case file:

```json
{
  "version": 1,
  "subjects": {"target": 4294967297},
  "test": {
    "given": {"subject": "target", "state": {"example.temperature": 80}},
    "when": {"steps": 20},
    "expect": {"subject": "target", "term": "example.temperature", "below": 60}
  }
}
```

Subject values are complete handle bits from the capture. The single `test`
uses the existing validated `given`/`when`/`expect` schema, including typed
handle references. The simulation child resolves names and terms against the
restored capture and append-only candidate registry. Existing captured terms
and material meanings remain unchanged. Unknown subjects, incompatible
registry terms and failed simulation actions are execution errors, never
successful assertions.

Each test runs twice in fresh processes, with independent output directories:

```
amb-server run --restore CAPTURE --catalog DATA --data UNIQUE_DIRECTORY \
  --no-network --unpaced --fixture-case CASE.json \
  --candidate ARTIFACT --candidate-registry REGISTRY_HASH \
  --candidate-author PLAYER --report CHILD_RESULT.json [--candidate-active]
```

`PLAYER` is the authenticated job actor. The candidate pass adds the active
candidate rule using its actual declaration. The baseline may load its artifact
for inspection but never installs the rule. Both start from the same immutable
capture; no state, timer, RNG, geometry or VM mutation survives between passes.
The child derives its exact simulation duration from `when`, rather than a
second independently supplied step count. The normal run command handles
physics, watches, timers and ordinary action dispatch.

The child report is deliberately small:

```json
{"version":1,"passed":false,"observed":42.0,"error":""}
```

`observed` contains the actual compared scalar (or a diagnostic record if the
comparison could not execute). `passed=false,error=""` is an ordinary comparison
failure. Any nonempty `error`, failed process, missing report, invalid JSON or
timeout is an execution failure. The supervisor never converts one into a
baseline comparison failure.

The supervisor emits existing `elaborate::result_json`, preserving job, request,
capture, candidate and runtime identity. Every completed test contains both
candidate and baseline `TestRun`s. A completed result may contain failed
comparisons; the existing completion checker requires all candidate assertions
to pass and at least one baseline assertion to fail before accepting the rule.
Execution failures produce status `error` (or `unavailable` when the executable
cannot launch), including the actual diagnostic reason.

The existing fixture worker remains the outer total wall-clock limit and
process-group owner. The supervisor uses the same bounded process runner for
children, with the remaining job deadline and bounded diagnostics/report sizes.
Nested children remain in the outer group so cancellation kills the entire
job. Job/case/output files live beside the worker input, inside a job-specific
subdirectory. A temporary result is renamed only after a complete report has
been written.

Running in-process would avoid startup cost but requires exposing the entire
current composition as a resettable callable and makes imperfect cleanup part
of fixture correctness. Fresh processes reuse the real restore path, preserve
the public ownership boundaries, and match the required isolation directly.
