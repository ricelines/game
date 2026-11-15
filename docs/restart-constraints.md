# Restart requirements

The user rejected and ordered deletion of the first implementation. It has been removed. The five original design documents remain the product and technical specification. Do not restore the rejected implementation or its architecture.

The specific failures were:

1. A central Core publicly owned every table and subsystem, and implemented gameplay, transport controls, perception, persistence, and seed setup. Splitting its methods into files did not restrict subsystem access.
2. Generic reaction code recognized `aq.oxygen`, and replication selected oxygen and fish health in native code. Both must work for newly authored quantities using declarations.
3. Native strike, push, pour, throw, and sit branches bypassed authored body behavior. Pouring was incorrectly modeled as an impact. Body scripts must implement these verbs using general engine primitives.
4. Coordinate tests supplied named zones and café lighting, and native code paid player 3 from the treasury. Zones, illumination, and institutions must be authored and replaceable.
5. Default inhabitants recognized purpose keywords and selected prewritten table/cup CAD. This did not demonstrate autonomous invention. Production inhabitants must use an actual model policy; deterministic scenarios belong only in explicit tests.

The user requires a complete playable prototype, not a toy, and prioritizes responsive game code without needless machinery. Architectural lines must be enforced by APIs and dependencies. No universal World/Core/Context/Services object, public aggregate store, blanket subsystem access, or dormant alternative implementation is acceptable. The composition root may allocate and wire tables, but is never passed into a subsystem and contains no game rules.

Modules expose explicit table access or narrow operations. Authored content owns game-specific semantics. New quantities, replicated properties, zones, institutions, and body verbs must not require native-engine edits. Validation must demonstrate these extension boundaries as well as functional behavior and performance.

Work only inside this repository. Do not read environment files or credentials. Authorized model calls use `/home/dev/.local/bin/with-openai-key` without exposing the key. Run appropriate formatters, linters, tests, and benchmarks after source changes. Delete obsolete code rather than preserving compatibility scaffolding.
