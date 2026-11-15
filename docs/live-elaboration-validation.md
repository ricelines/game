# Live S12 validation

`tests/game_elaboration.py` runs against the actual native server and its real fixture child, using only authenticated controls and player actions. It consumes generated private key files without logging their contents or hello packets. Run it against a fresh prepared world:

```sh
.venv/bin/python tests/game_elaboration.py --url ws://127.0.0.1:18080/ws --evidence .cache/integration
```

The latest fresh release run passed in 4.55 seconds on binary SHA256 `cced856e9ff6ec3ddee9b58b1d5b4289f288f5157d731e2b5d3fd58fb6aa3829` and the final prepared plan. Evidence is retained in `.cache/integration/metadata-elaboration-1794696595939412278/elaboration-report.json` and its `elaboration-transcript.json`, with native command/fingerprints/output/state in the adjacent `native/` directory. It ran while Python checks and focused sanitizer compilation shared the host, with production limits unchanged; the owned server then stopped cleanly at step83 with zero script errors. The earlier all-gameplay checkpoint remains separately fingerprinted in `.cache/integration/final-elaboration-2026-11-15/`.

Mira's actual `base.switch` on observed wood floor `o:9.1` returned `not_modeled`. `engine.request_model` retained that original target, requester and capture. Rue listed and claimed the request, then registered actual immutable Luau sources through `register_script`.

The first candidate was deliberately vacuous: both real candidate and baseline worlds observed charge 77 and passed. Its answer was rejected; inspection confirmed unapproved/inactive state, manual enabling was denied, and the live switch remained unmodeled. The useful candidate then passed four real action cases (charge 77/13); all four corresponding baseline worlds remained at zero and failed their comparisons, with no runtime errors. This paired job took 2,396.4 ms on the shared host.

Inspection while the answer waited still showed the candidate inactive. Rue's read query bypassed the waiting mutation; the next mutation stayed behind the answer. Mira independently completed a real balance input in 48.9 ms while fixtures were pending. After the accepted report, the rule became approved/active, Mira received the answered notification, and retrying the original action applied charge 77 to the same floor identity.

This is gameplay fixture evidence. The separate fixture transport probes are not used as proof of these results. The test leaves its accepted rule in the test world and therefore expects a fresh world for another complete run.

The latest run additionally queries the actual S7 source events immediately after the failed action. Record923 is the chosen player action; derived gap924 is explicitly nonchosen, has cause923 and retains root923. Derived gaps therefore do not add another chosen attempt to the frozen measurement input. The actual ActionTurns native regression separately executes a player→Luau `self:act`→installed ScriptHolder action, verifies the descendant is nonchosen with preserved actor/cause/root/result, and covers revoked-script and foreign-player denials. Speech emitted by a handler is likewise derived producer output. These are producer corrections, not changes to the frozen adapter or formulas.
