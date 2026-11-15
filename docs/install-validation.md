# Installation and reaction adapter evidence

The `amber_install` library builds independently of physics/networking. It owns the sparse instance-script installation column and rebuildable typed dependency links; dispatch rows, script declarations, last watch answers, rule history and VM state retain their separate owners. The source dependency check passes.

`install` CTest uses actual compiled Luau handlers, the real state/read bindings, subscription comparison, trigger projections and bitsery codecs. It exercises:

* A newly named quantity changing an authored state term through a real handler, with unchanged-answer suppression and one invocation per subscribed consumer.
* Material/model/instance dispatch order, projection of event/action functions absent the watch list, and no historical event replay on installation.
* Explicit first-edge relation rebinds preserving old values, threshold predicates, acquired foreign reads on the installed receiver, detach-to-absence and protection against reading slot zero.
* Term-along fallback, explicit zero own rows taking precedence, and own-row removal returning to the related source.
* Process crossing timers and rate-revision invalidation; selective installation/watch-state snapshots; unchanged saved revisions and no duplicate timer/callback on restore.
* Actual late-rule execution, current selector rechecks, disabled catch-up cancellation, immutable replacement versions, and removal of one replaced watch while the old script's other active trigger remains.
* Flag-dependent subscription activation after a quantity mutation in the same completed handler batch, delivering the current answer once and removing the subscription when the selector stops matching.

At the isolated installation checkpoint, the optimized warning-as-error and Address/UndefinedBehavior Sanitizer suites passed 23/23 tests (0.81 and 6.07 seconds respectively), including the VM handle-read change, snapshot filtering, pre-publication selector synchronization and actual threshold-turn routing.

The optimized `install_bench` measured 154 ns per changed watch comparison, 77 ns for unchanged indexed relation refresh with 10,000 unrelated objects, and about 10 ns for validation of a current catch-up item. Each loop checks a delivered/valid count. These are module timings under a shared development host, not the full 20 Hz world gate. Actual Luau event planning, execution and persistent state mutation measured 2.39 microseconds in `drive_bench`.

The actual runner now calls dependency refresh, comparison, planning, execution, structural application and publication in order, with selector/lifecycle synchronization. These module measurements remain distinct from [the real S12 journey](live-elaboration-validation.md) and [automatic world recovery](live-restart-validation.md). The latest live-authoring batch and final-binary reruns are tracked separately rather than retroactively included in the original 23-test checkpoint.
