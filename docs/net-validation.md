# Transport validation

The transport uses pinned uWebSockets/uSockets and bitsery. Native codec tests,
the dedicated socket fixture and the actual running-game probe test different
boundaries; fixture echoes are not reported as game latency.

## Post-refactor native game probe

The [post-refactor prototype load report](../reports/net-prototype-refactor-2026-11-15.json)
uses Release executable
`1900a7c8fcd3183aeee9a83a208b88fe5a6cc7d34c467cc5de3f1a2024943406`
and seeded plan
`1eaa07b36531a3ecfacc13034fecf7e1be36286e5126759a1b531f4be38bf189`.
The actual world at `.cache/world-refactor-prototype` already contained the live
model-revision test's objects and watches. Compilation, browsers, baking, other
native test worlds and model calls were stopped; the serial public-data HTTP
collector continued.

| Actual native game measurement | Result |
| --- | ---: |
| Authenticated watch sockets | 500 |
| Total join time / welcome p95 | 3.789s / 638.884ms |
| Concurrent event-query RTT median / p95 | 602.783 / 925.708ms |
| Minimum live delta frames per viewer | 160 |
| Identical shared-frame comparisons | 167,916 |
| Independently decoded baseline comparisons | 1,226 |
| Replacement keyframes / matching live baselines | 26 / 4 |
| Per-viewer observation duration, median / range | 8.087s / 4.139–9.301s |
| Per-viewer binary bytes, median / p95 | 183,576 / 207,048 |
| Per-viewer binary bytes/s, median / p95 | 22,568 / 23,772 |
| Total binary bytes, including replacement viewer | 90,868,392 |

`tests/net_live.py` opens ordinary authenticated watch connections, subscribes
identical camera interest, receives continuing real world deltas and issues the
actual read-only `events(limit=1)` operation on every socket. It compares shared
cell/frame bytes across viewers and independently decodes complete S6 baselines
on four viewers. A replacement connection must receive a keyframe matching an
existing viewer's decoded delta baseline at the same step. All 500 queries
completed, all probe sockets closed, and the probe made no world mutations.

Each viewer's rate divides its received binary bytes by its own measured
welcome-to-report duration, or welcome-to-orderly-disconnect for the replaced
viewer. The window includes initial keyframes and excludes JSON controls, HTTP
assets and transport overhead. The replacement viewer is included only in the
total byte count. Loopback TCP and Python asyncio scheduling contribute to these
end-to-end latency results; they are not isolated native phase timings.

```sh
UV_CACHE_DIR="$PWD/.tools/uv-cache" uv run --locked --script tests/net_live.py \
  --url ws://127.0.0.1:8080/ws --key-file .runtime/keys/spectator.key
```

The script consumes the credential privately and emits only counts, timings and
byte totals. It neither starts `net_probe` nor sends fixture-only ping/flood
commands. The dedicated script lock pins its WebSocket dependency and hashes.

## Codec and dedicated transport fixture

`net_test` checks labelled little-endian keyframe/delta bytes and complete stream
behavior. The independent decoder covers all mask fields, state replacement and
removal, quaternion angular error over 10,000 orientations, local ID reuse, cell
migration, joins after deltas, hysteresis, stale recovery and cell drops. A cell
with 65,536 entities is rejected before baseline mutation. Empty interest cells
and depleted streams do not accumulate.

`tests/net_socket.py` starts the real native `net_probe` fixture and opens 500
loopback WebSockets. Its retained checkpoint measured 0.389s total join time,
72.1ms welcome p95 and 39.3/45.8ms median/p95 control-echo RTT. Every socket
received 16–19 recurring deltas. The actual shared encoder/grouping benchmark
measured 218/287µs median/p95 for 300 changed rows and 500 recipients, producing
1,815 bytes/update. At 10Hz that controlled workload is 18,150 bytes/s before
initial keyframes and unrelated controls. Those figures describe that fixture,
not authored-world action processing.

```sh
UV_CACHE_DIR="$PWD/.tools/uv-cache" uv run --locked --script tests/net_socket.py \
  .build/release/server/net/net_probe
```

The fixture checks HTTP file delivery, WASM MIME/gzip bytes, immutable caching,
canonical-root traversal rejection, copied JSON controls, bad-key rejection and
incoming binary rejection. A paused TCP reader that crosses the hard buffer
limit receives close 1008/slow_viewer after draining; accepted data is not
silently omitted. Native tests separately check soft-limit stale/keyframe
recovery without depending on kernel buffer timing. Temporary fixture files stay
in the checkout, and the test terminates and joins its owned server.

## Earlier integrated failures and checkpoints

The first actual game probe exposed an event-query cost hidden by the tiny echo
callback: each limited query copied every matching retained JSON record before
discarding excess rows. Only 285/500 replies arrived within 15s, while viewers
continued receiving deltas. The read adapter now selects matching record
pointers and copies only requested final rows. Native regressions check privacy,
root/object/since filters, chronological order and omitted counts. The actual
`viewing_test --events-bench` scans 4,096 detailed retained records and returns
one; its earlier shared-host median/p95 was 430/684µs, excluding sockets and the
simulation phase.

The [first corrected game checkpoint](../reports/net-live-2026-11-15.json)
completed all queries and reconnect checks but did not record individual
observation durations, so it establishes no per-viewer bandwidth rate. The
[later quiet checkpoint](../reports/net-live-final-2026-11-15.json) added measured
per-viewer windows. Both reports retain their earlier executable identities. The preceding
[final prototype measurement](../reports/net-prototype-final-2026-11-15.json)
also remains available with its exact `f74178af…` fingerprint; none is relabelled
as a result for the refactored source.

These tests establish neither public-internet latency nor browser rendering
performance. Actual browser hardware and autonomous-play evidence are reported
separately.
