# Elaboration requests and fixture jobs

This module implements S12. It owns request history, gap counts, each player's last modeling gap, capture references and pending fixture-job descriptors. It receives copied observations, gap descriptions, completed snapshot bytes, authenticated role decisions and immutable script/test metadata. It never reads live reaction, permission, catalog, spatial or network tables. The composition layer calls their separate narrow APIs. The snapshot codec is its only additional module dependency beyond store/record.

The alternatives are running tests on the writer thread, inventing a reduced reaction-only fixture, and launching the real server runner in a subprocess. The first stalls play; the second cannot test actions, physics, construction or permissions faithfully. Use the subprocess. Until that runner is connected, a fixture job reports unavailable and cannot approve a rule.

## Authoritative tables and public operations

`RequestId` is a monotonically allocated uint32, and handles retain their full generation. `Requests` holds the S12 record: id, requester (zero for automatic gaps), attempted action, term/material gap key, target, immutable capture hash, open/claimed/answered/refused status, claimant, creation/answer step and a tagged answer. It also holds the player's request note and final display text. Answer tags are rule artifact hash, patch note, separately authored detail text id, or refusal text id. Rows are retained after closure. An empty capture hash means its captured bytes have not finished publication; such a request can be listed/claimed but not submitted for rule testing.

Separate plain tables are `GapCounts[(term, material)]`, `LastGaps[player]`, `CaptureRefs[hash]` and `PendingFixtures[job_id]`. A last-gap row contains the attempted action, full target handle, key, step, causal Origin and the capture reference prepared at that gap; a later ordinary successful action does not erase it. Captures use SHA-256 of the completed **uncompressed S10 bytes** as their identity. Compression and atomic file publication may happen off-step without changing the hash. Capture references carry pending/ready/failed state and a failure message; failed I/O is visible and never replaced with a snapshot of a later world.

The public surface is a collection of free functions over the named tables, not an owning world/service object:

```cpp
GapOutcome note_gap(GapCounts&, LastGaps&, Requests&, const GapInput&,
                    uint32_t gap_captures);
RequestId request_model(Requests&, const LastGaps&, PlayerId, string note, Step);
vector<Request> requests(const Requests&, const GapCounts&, RoleAccess,
                         uint32_t page, uint32_t page_size);
void claim(Requests&, RoleAccess, RequestId);
CloseOutcome refuse(Requests&, RoleAccess, RequestId, TextRef, string display, Step);
CloseOutcome answer_patch(Requests&, RoleAccess, RequestId, string note, Step);
CloseOutcome answer_detail(Requests&, RoleAccess, RequestId, TextRef, string display, Step);
FixtureJob begin_rule_answer(PendingFixtures&, const Requests&, const CaptureRefs&,
                             RoleAccess, RequestId, const RuleCandidate&, SessionOrder);
CompletionCheck check_fixture_completion(const PendingFixtures&, const Requests&,
                                          RoleAccess, const FixtureResult&);
CloseOutcome close_rule_answer(Requests&, PendingFixtures&, const CompletionCheck&, Step);
```

These signatures are the contract shape; the implementation header will give concrete error/result types and string/span ownership. `RoleAccess` is a copied authenticated player id plus the current elaborator-role decision supplied by composition; it cannot come from client JSON. `GapInput` contains only copied ids, Origin and capture-selection metadata, never table references. `GapOutcome` reports whether this was one of the first `GAP_CAPTURES` occurrences and therefore requires a capture/automatic request. Each unhandled shared-quantity watch increments its pair's count exactly once, including occurrences after the capture limit. A causally originating player also receives `LastGaps` and `not_modeled`; an automatic request still has requester zero. `request_model` copies that player's last gap and attaches its note rather than guessing from the current selection. Explicit requests may share the same immutable capture.

All listing/claim/answer/refuse operations require the elaborator role. Listings include open and claimed unresolved requests: explicit requests first by creation/id; then automatic requests by descending current pair count with creation/id ties. Claiming an open row records the actor. Reclaiming one's own row is harmless; another actor's claim is an error. Answer/refuse may close an open row or one's own claim; they cannot close another actor's claim or an already closed row. Failed testing leaves the request unresolved and its claim intact. No leases, distributed locks or world-state transactions are needed.

`CloseOutcome` is a completed value containing the requester (if any), `answered` notification text and record detail. Composition records it with the input Origin and delivers the event to the requester's current session. Closed request history remains available after reconnect; this does not introduce a separate messaging service. A patch answer documents changes made through ordinary already-authorized actions; answering never replays a patch or mutates a captured world. A detail is appended through a separate catalog detail-text writer, preserving its elaborator/creation attribution and the maker's name/description/writing, before closing the request.

## Capture selection and lifetime

At the writer boundary that detects the gap, composition selects the target, every handle in the requester's last observation (including short-form neighbors), and the direct neighbors of that initial set in either direction of every relation. This is one-hop inclusion, not transitive closure. A mid-chain gap without such an observation starts with objects inside configured `CAPTURE_M` of its subject, then includes the same direct relation neighbors. Selection uses full handles and deduplicates them. Individual module encoders write those instances and incident rows into the actual S10 container, with required model/parameter/script identities and registry/runtime pins. Capture excludes unrelated instances; it is never a full-world snapshot disguised as a fixture.

Reaction processes, watch answers, shared reverse indexes/meters, script scalar state and RNG state must resume from the captured step consistently. The fixture starts at that captured logical step with its own clock; test `steps` are relative advances. Authoritative queue/timer/intent entries are selected by their referenced subjects and preserve due-step offsets. Composition does not serialize pointers, callbacks or a live VM. Immutable models/scripts/registry assets remain content-addressed references available to the worker.

`CaptureWriter` is an I/O worker over completed byte buffers and destination paths only. Submission hashes those already completed bytes to return an immediate stable reference; compression and publication happen off-step and return copied completion values. The hash cost is measured separately from snapshot encoding. Live tables and observation callbacks are never passed to the worker. Capture failure is an explicit result. Destruction or movement of the original target does not invalidate an already completed capture or a rule's tests. Automatic requests stop after the first configured examples, while a player-origin gap retains its historical capture so a later explicit request never substitutes a newer world state.

## Real fixture executable contract

Retain the original command prefix:

```text
amb fixture CAPTURE --job JOB.json --result RESULT.json
```

The executable loads the capture through the same snapshot restoration and world-step composition used by normal play, with network listeners disabled. `JOB.json` is immutable input with version 1, job/request ids, capture hash, candidate artifact/script identity, registry pin, subject-name-to-full-handle map, verified tests and explicit configured fixture limits. The initial subject map always includes `target`; other names are copied from the selected capture metadata, and unknown subject names are errors. The registry pin is the current validated registry at job creation, containing the candidate's terms and stable ids; original capture term definitions cannot be rewritten. New appended terms are allowed. Script and geometry assets must have matching pinned runtime versions.

The fixture input parser validates the test data derived from the actual script declaration before launching work; caller-supplied replacement tests are rejected. `given` is one subject record or an array of them, with numeric `contribute` values and numeric/boolean/handle-valued `state`. A handle value is `{handle:'subjectName'}` resolved through the job's subjects. The same form works for `expect.equals`; ordinary string properties are not a Value kind. `when.action` is `{term:'registered.name',target?:'subjectName',args?:flatTypedPayload}`; payload strings/vectors remain valid. For each test, restore the capture independently twice. Apply its `given` state/contribution values through the normal typed state and contribution operations. Run its `when`, either relative steps or one authenticated test action through the actual action dispatcher. Read its `expect` through the ordinary source resolver. `equals` compares the resolved typed value exactly; `above`/`below` are strict numeric comparisons. Non-finite values, missing subjects/terms, illegal `given` writes, missing assets and execution errors are test errors, not a passing comparison. Each run enforces the same authored rule/body/permission semantics as live play.

The candidate run installs the candidate at its declared selector/trigger, replacing the prior active version there as ordinary registry installation would. The baseline restores the same capture and job registry but leaves that prior rule in place, omitting only the candidate. It does not remove every rule on the term. Accept only if **every** candidate test passes and **at least one** baseline test fails a comparison. An empty suite, a baseline execution error or a failed candidate execution never satisfies this criterion.

`RESULT.json` contains version, job/request/capture/candidate identities, runner runtime pin, status (`completed`, `error` or `unavailable`), per-test candidate/baseline pass booleans with observed values or errors, and elapsed time. Exit zero means a complete well-formed comparison report, even if the candidate fails its assertions; nonzero means fixture execution failed. The parent validates the report identities and computes acceptance from individual results rather than trusting a worker-supplied `accepted` boolean. The subprocess writes the result atomically. Stdout/stderr are bounded diagnostics, never the approval channel.

`FixtureWorker` receives copied job bytes, immutable paths, executable argv and a configured timeout/concurrency limit. It uses subprocesses and returns completed result DTOs. No worker accesses or mutates the live VM or tables. Pending jobs retain their originating session sequence; that session's next world input waits for completion while other sessions and the world continue. There is no global lock or wait on the writer thread.

When a result arrives, the writer rechecks the actor's current elaborator role, request still open or claimed by that actor, exact pending job/capture/candidate identity, valid artifact pins and the full with/without acceptance criterion. It then installs the validated rule through the separate registry/trigger/VM activation APIs, writes a new immutable registry pin and closes the request. On rejection or execution failure, `discard_fixture` releases that session's pending input without closing the request or clearing its claim. Current live-world positions/properties need not equal the historical capture. If role/claim has changed, the result is rejected without activation. No fixture may approve itself or supply a native binding.

## Persistence and tests

REQS uses explicit `write/read_requests`, `write/read_gaps`, `write/read_last_gaps`, `write/read_capture_refs` and `write/read_pending_fixtures` functions taking the corresponding individual tables and `snap::Output/Input`. Save id high-water marks, full handles, all statuses/answers, causal Origins and copied pending-job descriptors. Derived request indexes rebuild. Pending capture writes and fixture processes contain no persisted PID, pointer or future; on restart they are requeued from retained completed bytes/files, or marked unavailable if that immutable input was never published. Capture filtering retains requests about selected targets plus their referenced captures; it never imports unrelated pending live jobs into a fixture.

Focused tests cover exact first-three counting, repeated explicit requests from a retained last gap, stable priority ordering, role/claim refusal, distinct detail/maker text, selected captures including short observations and incident relations, restart/id continuity and the acceptance truth table. A subprocess protocol test can use a tiny executable to exercise timeout and malformed/mismatched-result handling, but must be labeled transport-only. The integration gate must use `amb fixture` on a real selected-world capture, observe an actual rule-driven effect with the candidate and its absence without, and reject a vacuous candidate. Report that gate unavailable until the real composition runner exists. Benchmark request ordering/gap updates and separately report capture encode pause, off-step compression and fixture duration; a worker transport benchmark is not a game-performance result.
