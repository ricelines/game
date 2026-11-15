# Bake subsystem contract

Version 1, implementing original S8. This is the interface between the Python bake worker, asset consumers and the HTTP adapter. Baking has no access to live world tables. A successful completion is an ordinary model-registration input on the server's writer. The server assigns the model ID and attribution; geometry cache identity never substitutes for model identity.

## Requests and completion

`POST /bake/models` accepts `{source: string, params?: object, material?: term, text: {name, description, writing?}}` under the caller's authenticated identity and returns `202 {job}`. The adapter passes only source and parameter values into the geometry worker. Material/text/author belong to the per-submission model row. `GET /bake/jobs/ID` returns `{status: queued|baking|done|failed, job, hash?, measured?, errors?, timings?}`. A done result also contains the model ID after server registration, not a Python-generated ID.

The Python boundary is `BakePool.submit(source, params) -> job_id`, `BakePool.status(job_id) -> result`, and `BakePool.close()`. Completion supplies an immutable asset directory/hash and extracted model defaults. A separate adapter joins caller metadata and registers a model. Cache hits still create distinct model submissions. Script validation/submission belongs to the Luau compiler interface and never executes Luau in Python. The supervisor creates validated staging directories on the asset filesystem and atomically renames them; worker output may be on a different filesystem.

Parameters are resolved from the program's `PARAMS` declarations. Unknown names, non-finite values, malformed declarations and values outside declared bounds fail with a structured error. Hash input is UTF-8 source, canonical JSON of resolved parameter values and `BAKE_VERSION`, separated by NUL bytes. Canonical JSON sorts keys, uses compact separators and forbids NaN/Infinity. Model material/text/author are excluded from geometry identity. A literal declaration permits a cache lookup without executing the source. Computed declarations are supported but evaluated only in the sandbox child.

Numeric parameters are normalized to f64, so JSON `1` and `1.0` select the same parameter set. Integer-valued step counts are accepted by the stair helper. `BAKE_VERSION` is the cache salt: change it when pinned CAD/native libraries or geometry-affecting tessellation/LOD/capacity settings change. Tests with altered geometry settings use separate temporary asset stores.

## Asset directory

`assets/HASH/` contains immutable `manifest.json`, `source.py`, `lod0.glb`, `lod1.glb`, `lod2.glb`, `collision.bin` and `convex.bin`. The first collision file is the real static mesh at the 40% LOD; the second is a Jolt convex shape with at most `CONVEX_POINTS_MAX` support points. Space chooses by the generic instance collision mode, never a model name. Both use Jolt 5.6.0 `Shape::SaveWithChildren`/`sRestoreWithChildren` binary streams and server Z-up meters. They are pinned to the native dependency version and rebaked when it changes. No hand-written approximate box replaces them.

GLBs use standard glTF 2.0, embedded buffers, meter scale and Y up. Every vertex maps server `(x,y,z)` to glTF `(x,z,-y)`. Node/part identity is stable across LODs. An asset's static mesh arrays are also available as `mesh.bin` for Recast and source-independent geometry queries:

```text
8 bytes magic "AMBMESH1"
u32 vertex_count, u32 index_count
vertex_count * (f32 x, f32 y, f32 z)  # server Z-up meters
index_count * u32                    # triangle-list indices, LOD1
```

All integers/floats are little-endian. `index_count` is divisible by three. The native helper validates index bounds and finite coordinates before collision creation.

`manifest.json` has this shape (omitted optional arrays are empty):

```json
{
  "version": 1,
  "hash": "64-lowercase-hex",
  "geometry_hash": "64-lowercase-hex",
  "bake_version": "ambergris-bake-4",
  "coordinates": "z_up_meters",
  "params": {"height": 0.1},
  "parameter_schema": {"height": [0.1, 0.05, 0.3]},
  "default_material": "base.ceramic",
  "default_script": null,
  "bounds": {"min": [-0.04, -0.04, 0], "max": [0.04, 0.04, 0.1]},
  "size": [0.08, 0.08, 0.1],
  "volume": 0.0001,
  "capacity": 0.0002,
  "capacity_method": "standard_exact",
  "supports": [{"height": 0.1, "area": 0.01, "polygon": [[0,0,0.1],[1,0,0.1],[0,1,0.1]]}],
  "sites": {},
  "parts": {},
  "replicated": [],
  "bindings": {},
  "lods": [{"file":"lod0.glb","triangles":200},{"file":"lod1.glb","triangles":80},{"file":"lod2.glb","triangles":20}],
  "collision": {"static":"collision.bin","dynamic":"convex.bin","mesh":"mesh.bin","jolt":"5.6.0"}
}
```

The sample polygon/numbers illustrate field types, not a geometrically valid cup. Bounds/size/volume/capacity/supports are measured, never copied from claimed metadata. `mass` is not stored: the engine evaluates measured solid volume times current material density. Static mesh/convex artifacts are geometry only. Site/part/render/state declarations are extracted as JSON-compatible data; native consumers resolve registered term names and validate their roles.

`geometry_hash` is SHA-256 over the three ordered GLB files, each preceded by its little-endian u64 byte length. It identifies render geometry independently of authored asset identity, text, material and settings. The viewer shares GLB imports and opaque instance batches by this digest and LOD; different materials retain separate batches. Distinct part manifests remain independently inspectable.

## Geometry execution

A program exports `PARAMS`, optional `MATERIAL`, `SCRIPT`, `SITES`, `PARTS`, `REPLICATED`, `BINDINGS`, and `build(params)`. The result is a valid build123d solid or compound of closed solids, in meters, base Z=0 and centered in XY. All source code runs in a forked job child, including declaration evaluation. No metadata extraction executes untrusted code in the supervisor.

`amb.shapes` provides box, slab, cylinder, sphere, hollow_box, hollow_cylinder and stair. These return actual build123d shapes. Exact capacity provenance is retained only while the returned topology is unchanged; Boolean edits or arbitrary geometry use voxel capacity. Standard stair construction validates rise/run/width from config. Arbitrary build123d remains supported.

Solid volume is OCCT volume. Supports are upward planar faces within the configured normal/area criteria. Tessellation uses configured absolute/relative tolerance and fails beyond the triangle/bounds limits. Meshoptimizer generates 40% and 10% target LODs with topology-preserving error limits; actual counts are recorded because topology may prevent the exact target. General capacity implements S8's surface-conservative voxel marking and priority flood at `CAPACITY_GRID`, with one padded exterior layer. Standard hollows have exact parameter capacity. No model/term/name recognizer supplies a result.

Within one job, exact float32 mesh vertices/indices and the fixed native request select reusable compiled collision, capacity and GLB artifacts. Every part still receives freshly measured CAD bounds, volume and supports. The imported complete assembly is topology-validated once; selecting its solids and applying rigid locations does not require repeating that same validation. Supervisor publication similarly validates identical artifact bytes once per publication while checking every manifest's own triangle counts. Neither cache survives the job or uses authored claims as a key.

### Moving parts are ordinary child assets

`PARTS = {"leaf": {"solid": 1, "origin": [0, 0, 0]}}` selects a zero-based solid from `build(params).solids()` and an explicit local pivot. Selected solids are removed from the root render mesh and root collider. Each selected solid is baked as a separate immutable asset with vertices relative to its pivot. Its hash is SHA-256 of the parent hash, NUL, `part`, NUL and the part name. A part manifest has the same geometry/measurement fields and records its parent asset hash; it does not recursively copy other parts. Part names are validated path-safe identifiers. Duplicate solid selection is an error.

The root manifest's `parts` entries contain `{asset: HASH, position: [x,y,z], rotation: [0,0,0,1]}`. Optional per-part `material`, `script`, `sites`, `replicated` and `bindings` declarations become that child asset's defaults; the parent's script is never installed again on every child. A child records its parent hash, part name, selected solid and pivot for inspection. The ordinary placement operation creates the root and one ordinary child object per entry with that asset/model reference, parent relation and transform. Those children use ordinary generation handles, physics, replication and destruction. No separate part transport or part physics engine exists. `self:parts()` resolves these child handles, and own-part authority follows the declared parent relation.

Root `volume` counts only root solids; part `volume` counts only that part. `assembly` records complete-source bounds/volume for diagnostics, not a second mass charge. Thus root mass plus child masses equals assembled solid mass exactly once. Root sites remain in the root frame. A root with no remaining solids is a logical assembly node: zero volume, null collision/mesh artifact paths and an empty valid glTF scene; its child objects supply geometry. It is not replaced with a colliding box.

Example trace: author submits a compound frame and leaf, selecting the leaf as a part. The bake publishes root and leaf asset directories. Placement creates root `o:A` and child `o:B`. The viewer draws only the frame for A and only the leaf for B. A's Luau script sweeps B; space updates B's ordinary transform/collider and net sends B's transform delta. Closing cannot collide with a duplicate leaf hidden in A. The full assembly is charged its summed root/child material mass, never its diagnostic assembly volume again.

## Sandbox and failure

One warm trusted Python worker imports build123d before jobs. Every job forks a new child. Submitted code requires mount/user/network isolation with bubblewrap, no-new-privileges, seccomp denying new sockets/process execution, Landlock limiting writable paths to that job's output, a per-job delegated cgroup memory limit and the configured wall-clock deadline. Supervisor validation checks all published outputs before atomic directory publication. A job cannot inspect the repository, credentials, environment files or another job's outputs.

Startup probes each required facility and reports specific failed operations. Strict production mode requires cgroup delegation, namespaces, bubblewrap, seccomp and Landlock; it never silently executes in the service process. Explicit `require_cgroup=False` selects a restricted-host test mode that still enforces namespaces, Landlock, seccomp, address-space and CPU limits; every result marks that limitation. It is not a pass of the original cgroup gate. The trusted pipeline may also be exercised directly by labelled internal geometry tests. Passing those tests does not count as passing any sandbox gate.

Errors have `{stage, message}` and optional numeric diagnostics; no traceback with environment/secret data is returned to a caller. Benchmarks report coarse total job time separately from warm import and cache hits. Timeouts terminate the job cgroup, not an arbitrary process-name match. Partial directories are never published. Cache hits validate the final manifest.

## Sources and checks

Use pinned build123d 0.12.0 and the repository's pinned Jolt/meshoptimizer. Build123d's [import/export documentation](https://build123d.readthedocs.io/en/stable/import_export.html) describes its actual geometry exporters; [kernel Landlock documentation](https://docs.kernel.org/userspace-api/landlock.html) defines the filesystem restriction ABI. The implementation must use those interfaces rather than infer behavior from mesh labels.

Focused tests cover exact standard geometry/capacity, arbitrary edited geometry capacity, solid validity/bounds, meter/axis consistency, support extraction, actual GLB parsing and LOD counts, native Jolt restore, caching/parameter identity, failures/timeouts and denial of network/foreign-file access. Benchmarks report a fresh import separately from warm jobs and cache hits. Host facility failures remain explicit in test/benchmark reports.
