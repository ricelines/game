# Actual authored-world spatial gates

`tests/game_spatial.py` starts its own native process on port 18081 with the real prepared plan and collision assets. All mutations are ordinary authenticated player actions; all avatar movement uses `base.move_to`. It retains the complete public transcript, sampled navigation traces, commands, executable/plan fingerprints, native logs and final world snapshot, including failed runs. Private keys and hello packets are never logged.

```sh
.venv/bin/python tests/game_spatial.py --require-terminal-events --output .cache/integration/final-spatial-report.json
```

The final release run in `.cache/integration/final-spatial-report.json` passed at step1009 with zero handler errors, using binary SHA256 `0c3d6599a61188a8e2ee124b7864d70eb2763b36e049a53712fecf864d6d6ae0` and the final prepared plan. Its retained artifact directory is recorded in the report.

* Mira entered through the opened physical doorway, climbed the actual stair collision to the 3.2 m mezzanine and descended again. The ascent contains 26 intermediate-height samples inside the baked stair footprint; maximum sampled height was 3.208 m. The destination is clear floor, separate from the upstairs table/lamp cluster.
* Sol and Ivo independently registered real breaker and pump Luau sources. The model author adopted those immutable sources through the normal model-script action. Switching actual power off withdrew pump aeration from 0.000636 to zero, and sampled tank oxygen fell from 0.007998 to 0.007919. Restoring power raised oxygen from 0.007918 to 0.007939. Actual downstream tank handler records include both source authors, p:3 and p:4; the original seeded sources alone all have the same author and are not presented as a multi-author test.
* The observed cup's actual `through` list contains the closed glass leaf, whose measured transmission is 0.88. Mira collected her existing opaque mezzanine slab and placed it as a screen using ordinary goods actions. The same cup disappeared with enough processed sight coverage to rule out a truncated observation, then reappeared after the slab was retrieved and restored. No material values were injected for the test.
* Mira placed her existing chair in the real sliding leaf's path. The authored sweep stopped at open fraction 0.57256, stayed there and emitted the obstruction sound heard by the player. Retrieving that same chair allowed the same door to close fully.

The first real run exposed a content error: the original leaf track intersected the dome beside the doorway after approximately 0.3 m. The authored geometry now puts the track outside the shell. The content regression checks the complete swept volume against the real Boolean shell, with the old intersecting track as a negative control. Engine collision exemptions were not added. Original failure artifacts remain under `.cache/integration/spatial-artifacts-1794689839708878779`.

The final run used `--require-terminal-events`. It first attempted entry while the physical glass leaf was closed: navigation stayed outside and delivered the player's own `blocked` push. Opening the door and completing the approach, entry, stair ascent and descent produced the corresponding own-player `arrived` pushes. These checks use ordinary navigation and live event delivery, rather than polling alone.
