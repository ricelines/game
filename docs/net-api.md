# Transport and replication boundary

`server/net` owns sockets, copied control frames, viewer interest and quantized per-cell history. It has no object/fact/catalog/permission/reaction projections. The writer supplies immutable replication DTOs assembled from each model's declared replicated terms. The composition adapter authenticates hello, authorizes read-only controls and interprets actions. The transport neither trusts a requested role nor assigns a player/body.

## Public API

The proposed module is `amber::net`, target `amber_net`. Shared `Handle`, `Vec3` and `Quat` are value types from store; no tables enter this API.

```cpp
using SessionId = uint64_t;
using Bytes = vector<uint8_t>;
using AssetHash = array<uint8_t,32>;
struct State { uint16_t term; float value; };
struct Entity {
    Handle object;
    AssetHash asset;
    uint64_t params;
    uint8_t flags;
    Vec3 position;
    Quat rotation;
    float footprint_m;
    bool faces_motion;
    vector<State> states;
};
struct Publication {
    uint32_t step;
    vector<Entity> upserts;
    vector<Handle> removed;
};
struct Settings {
    float cell_m;
    int enter_cells, leave_cells;
    size_t send_soft, send_hard;
};
```

An upsert is the entire current row. The writer sends all visible scene objects initially and only changed rows thereafter; custody items are removed. State lists contain only declared numeric replicated terms, sorted by ID, with at most255 entries. The encoder rejects publications whose final cell exceeds65535 entities before changing any baseline; transport reports a Fault and explicitly closes binary viewers instead of wrapping counts or IDs. The adapter rejects registry IDs exceeding the S6 u16 term range when adding replicated declarations. Asset identity is immutable SHA256 bytes; params remains bake-parameter identity. `footprint_m` is the maximum XY world-bounds width, including physical children when appropriate. A footprint greater than cell_m selects the global stream. `faces_motion` suppresses rotation changes; the row still contains an identity rotation in its fixed-layout Entity field. Flags are opaque to net and come from the reviewed adapter's client presentation bits.

```cpp
struct Cell { int16_t x,y; auto operator<=>(const Cell&) const = default; };
struct Delivery { vector<SessionId> recipients; shared_ptr<const Bytes> bytes; };
class Replicator {
    explicit Replicator(Settings);
    void set_view(SessionId, Vec3);      // called only after root authorizes binary
    void remove_viewer(SessionId);
    void set_writable(SessionId, bool);  // false marks all stream baselines stale
    vector<Delivery> advance(Publication); // exactly one replication boundary
};
uint32_t pack_rotation(Quat);
Quat unpack_rotation(uint32_t);
```

This deterministic encoder is also the test seam. Every changed cell is encoded once per boundary. Its complete baseline is updated by that delta, then any join/recovery keyframe is serialized from precisely that baseline and step. Existing nonstale viewers get the delta; joiners/recovering viewers get the keyframe. No viewer gets both for a cell at that step. Empty clean cells send no recurring packets. Stream-local IDs are reused after removals, including inside one delta. Migration emits an old-cell removal and new-cell addition. Global sentinel is `(32767,32767)`; valid configured world cell coordinates exclude this reserved pair.

```cpp
struct ServerOptions {
    string bind_address;
    uint16_t port;
    filesystem::path client_root, assets_root;
    Settings replication;
    uint32_t max_control_bytes;
};
struct Connected { SessionId session; };
struct Control { SessionId session; string text; };
struct Disconnected { SessionId session; uint16_t code; string reason; };
struct Fault { string reason; };
using Input = variant<Connected,Control,Disconnected,Fault>;
class Server {
    explicit Server(ServerOptions);
    string start();                  // empty on success; waits for listen result
    void stop();                     // close sockets/listener and join thread
    void drain(vector<Input>&);      // writer receives copies, FIFO
    void send(SessionId,string);     // already serialized JSON reply
    void close(SessionId,uint16_t,string);
    void view(SessionId,Vec3);        // authorization gate is outside transport
    void publish(Publication);       // consumes copied rows on socket thread
};
```

Server's single uWebSockets thread owns Replicator and all socket pointers. Commands use Loop::defer and own all strings/rows they reference. Callers may send after close; absence is simply ignored. `drain` swaps a mutex-protected FIFO; socket callbacks never inspect live tables. Incoming binary frames are a protocol error because clients send only controls. Incoming control text is copied, not parsed on the socket thread. The shared network.CONTROL_MAX supplies the per-message byte limit. This keeps per-session arrival order; the adapter preserves action order while asynchronous validation is pending. No automatic welcome or inferred authentication exists here.

The endpoint is `/ws`. HTTP GET serves client_root with index.html fallback only for `/`; `/assets/HASH/FILENAME` serves immutable baked files below assets_root. Paths must resolve below the configured root. Root passes port8080 and client/dist, assets/ for the local prototype. MIME types cover HTML/JS/WASM/PCK/GLB/JSON/Jolt data. Asset hashes get immutable cache headers; index.html does not. If an accepted `.gz` sibling exists for a web payload, serve it with Content-Encoding:gzip and Vary:Accept-Encoding. Large responses are streamed through uWS onWritable with owned buffers and abort cleanup rather than blocking the loop on network backpressure.

## Exact S6 encoding

All fixed integers are little-endian, matching bitsery's default wire configuration. Frame types1/2/3 and Entity/Change layouts are exactly S6.3. A keyframe position is signed whole centimeters relative to the cell corner in XY and world zero in Z. Global-stream coordinates are absolute centimeters. Round to nearest integer before comparing baselines. Delta offsets are signed differences from previous encoded integers, zigzag then unsigned LEB128 (at most5 bytes per i32 component).

Change mask bits are1position,2rotation,4asset+params,8states,16flags, emitted in that field order. Mask8 is a complete replacement of the state list, including an empty list; it is not a merge. This handles removal of a replication declaration without an extra wire extension. States are compared as encoded float32 values. Quaternions use largest-component index in low2bits; the three remaining components, in XYZW order excluding that index, occupy bits2/12/22. Negate the whole quaternion when largest component is negative. Map `[-1/sqrt(2),1/sqrt(2)]` linearly to0..1023 with nearest rounding. Reconstruct the missing positive component and normalize.

Interest is the S6 Chebyshev cell block: enter within2 and retain existing cells within3, with values from shared config. Every authorized binary viewer receives the global stream. A view change takes effect at the next publication, never inventing a between-step keyframe.

Before each publication, Server reads each socket's buffered bytes. Above SEND_SOFT it stops all binary sends for that viewer and marks membership stale. Once buffer drains to at most SEND_SOFT it drops obsolete cell memberships and keyframes all current cells from that publication's baseline. Above SEND_HARD it closes with an explicit slow-viewer reason. Authoritative control responses are not silently discarded. A send returning BACKPRESSURE is accepted by uWS but makes the next boundary stale. The library's silent-drop threshold is disabled; our explicit hard-cap checks leave the close frame able to carry its reason, and a DROPPED result is still treated as closure rather than accepted delivery.

## Generic metadata and controls

`docs/client-api.md` is the agreed spectator metadata contract. Catalog terms/materials/models, object metadata, agent summaries, events and debug are JSON controls passed unchanged by net. The adapter uses `send` to provide them. `view` input also remains adapter-authorized before calling Server::view. No seed IDs, state names, geometry primitives or material names appear in transport. A late metadata request is handled by the adapter without disturbing binary stream baselines.

## Alternatives and verification

A writer-thread encoder would remove socket-thread encoding work but require transferring subscriptions/backpressure and maintaining step-consistent recovery across two owners. A network-thread encoder is chosen for the prototype: one owner of all per-viewer history and every encoded cell is reused for its viewers. A full-world snapshot every publication is simpler to call but makes inert objects recurring work; changed-row publication keeps that cost with actual changes.

Tests use an independent reader and labelled fixture bytes for keyframes, masks, varints, quaternions, state replacement, immediate local-ID reuse, migration and join-after-delta consistency. Deterministic backpressure tests exercise stale recovery/drop. An actual500-WebSocket loopback test measures fan-out, controls, encoded bytes and latency; synthetic stream tests do not substitute for sockets. Asset HTTP and gzip are checked separately. Measurements record build type, scene density and environment.

Pinned API references are the local uWebSockets App.h/WebSocket.h/Loop.h and bitsery serializer/buffer adapter headers in third_party. S6 remains the source of truth for the three binary frames.
