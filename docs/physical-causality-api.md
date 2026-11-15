# Physical causal receipts

S7 requires queued reactions to inherit the record that caused them. Physical
bindings mutate native bodies before the handler record exists, then finalize
their causal receipts at record publication. Immediate attachment changes enter
the invocation's ordinary relation effects; later contact and membership work
retains bounded native motion tokens. Read consultation is separate and cannot
identify which bodies an operation changed.

The existing pre-change checkpoint is recorded separately: 47 native test/probe
targets built, and all 43 serial ASan/UBSan CTest cases passed in 17.13 seconds on
2026-11-15. The completed receipt extension's fresh dependent build passed all
44 serial CTest cases under ASan/UBSan in 15.60 seconds; its 47 native test/probe
targets, including the real physical causal regression, were rebuilt. Evidence:
`reports/native-sanitizer-causal-2026-11-15.json` and its retained test log.

## Publication boundary

Append this record-independent callback receipt to `react::Effect`:

```cpp
struct CausalReceipt {
  void *owner;
  std::uint64_t receipt;
  void (*finalize)(void *, std::uint64_t, const record::Origin &);
};
```

`publish_effects` invokes it with the completed handler or native-action Origin.
It is not serialized: a receipt is consumed before the current invocation leaves
the writer thread. Its stable owner is an individual binding context or the
causal-summary table, never a world host. It contains no player authority.
Successful operations append receipts even when they are own-navigation control
and therefore do not set `effects.changed`. Denials and mechanical failures do
not append receipts.

Navigation's binding-owned payload identifies an exact full body handle and
IntentId. Finalization updates only a still-matching intent; it cannot retag a
replacement. Cancellation/replacement ending payloads identify their exact
IntentId as well. Attachment/grasp/release take a local vector of actual returned
relation changes, convert it to ordinary `WatchWork` in the current effects, and
use the existing final-Origin publication. No shared relation-change slice is
left unowned. `AttachmentBinding` replaces its shared changes vector with four
resolved relation term IDs in `PhysicalRelationTerms`.

A typed mechanical alternative in `react::Effect` would be easier to inspect,
but would make react understand space operations and their lifetime rules. The
small callback receipt keeps typed payloads with their owner and reuses the
existing deferred-publication seam.

## Record-free space interface

```cpp
using CauseToken = std::uint64_t; // zero means unavailable/system
struct CausePair { CauseToken first = 0, second = 0; };
enum class CauseScope { Step, Motion };

// Appended fields with defaults:
// Contact::causes, RelationChange::causes : CausePair
// MoveRequest::cause : CauseToken

CauseToken motion_cause(const Space &, IdentityRead, Handle);
void active_cause_tokens(const Space &, std::vector<CauseToken> &);
OperationResult restore_motion_cause(Space &, IdentityRead, Handle, CauseToken);
```

`set_pose`, `translate`, `impulse`, `attach`, `detach` and the grasp promotion
`assemble` overload receive a trailing CauseToken with default zero. Each stamps
only its actual mechanically affected native owner, after validation and before
Jolt calls that can emit contact callbacks. A default/uninstrumented call clears
the previous token for the mutation rather than borrowing it. Compound members
resolve through the existing physical compound index; ordinary inventory or
parent relationships never imply mechanical ownership. Attached poses copy a
parent token only when native attachment propagation actually moves the child.

Rigid contact callbacks copy both native participants' tokens at callback time.
The real pinned `CharacterContactListener` is also registered on each
CharacterVirtual: its actual collision callbacks supply controller/wall contact
truth, while sensors are excluded and inner-body duplicates are reduced without
reordering callback state changes. Incoming rigid CCD impacts against an avatar's
inner body remain real contacts even when its virtual controller updated earlier.
Contact removal
captures current participant tokens, rather than reusing an indefinitely old
contact-start cause.

Step-scoped static/kinematic pose tokens expire after that step's contact and
relation callbacks. Dynamic motion tokens expire on native sleep. Character
tokens remain while their actual controller is active or they are airborne,
then expire once grounded without an active controller. Already-copied callback
tokens remain resolvable until that result batch has been published. Resting
doors/platforms therefore cannot credit an old pose action for a later walker.
Expiry work visits tagged/active objects, never all inert bodies.

## Bounded summaries and phase adapter

`bindings::MechanicalCauses`, declared in `bindings/causality.hpp`, owns only a
token-to-flat-summary map. A summary contains one bounded Origin and an
`ambiguous` bit. Pending operation receipts retain the previous summary only
until finalization; there is no persistent provenance graph.

```cpp
struct CauseSummary { record::Origin origin; bool ambiguous = false; };
class MechanicalCauses {
public:
  explicit MechanicalCauses(std::size_t authors_max);
  space::CauseToken intern(const record::Origin &);
  CauseSummary lookup(space::CauseToken) const;
  void retain(std::span<const space::CauseToken>);
};
struct PhysicalRelationTerms { TermId contains, supports, attached_to, in_zone; };
struct PhysicalEventTerms { TermId contact; PhysicalRelationTerms relations; };
struct PhysicalPublication {
  MechanicalCauses &causes;
  step::WorkQueues &queues;
  record::Journal &journal;
  PhysicalEventTerms terms;
  std::size_t event_limit;
  double hz;
  CreditResolver credits; // existing narrow metadata callback; no tables
};
void publish_physical(PhysicalPublication &, const space::StepResult &, Step);
void collect_physical_causes(MechanicalCauses &, const space::Space &);
```

The physical binding contexts for attachment, impulse, sweep, pose_batch and
move_part receive one required `MechanicalCauses&`. Navigation keeps its narrow
intent-only receipt payloads. Main supplies
`causes.intern(intent.origin)` as the new MoveRequest field and calls
`publish_physical` to publish contact/relation work with its recorded causes.
It collects unused tokens only after the result batch has been published.
Relation drafts are reduced by FactKey within that publication before enqueueing,
so two current contributors cannot be selected by endpoint iteration order. If a
watch was already scheduled by an earlier turn/step, step's existing coalescing
keeps the Origin that first scheduled that work. This convention identifies the
scheduling cause; it does not promise a complete multi-source watch history.

Replace-pose/release operations replace their command summary. Additive impulses
merge active contributors, bounded by AUTHORS_MAX. A common root keeps that root
and the most recent contributing record, with the union of bounded authors and
minimum oldest time. Different roots produce an explicitly ambiguous summary;
neither participant order nor a nearby/read handle chooses a winner. Publication
starts a System physical-interaction record with `causal_ambiguity:true`, retained
bounded authors/oldest, and a new system root; downstream work inherits that
record. It never claims an arbitrary player root. No contact-history graph is
stored. This attributes explicit native command/controller motion, not inferred
per-contact solver momentum transfers between previously uncommanded objects.

## Other mutation callers and persistence

Structural create/replace/remove, commerce custody/reveal and material-property
changes are separate callers. Their existing explicit relation effects retain
their final native-action Origin. Their uninstrumented native mutations clear
old body tokens before contact removal or shape changes, including affected
external occupants; they do not acquire a previous motion's cause. Exact
attribution of those contact side effects remains unavailable until that caller
provides its own receipt. This is an explicit boundary of the prototype batch.

An independent `bindings/causality_snapshot.hpp` owns:

```cpp
struct BodyCauseRow { Handle object; CauseSummary summary; };
void capture_motion_causes(const MechanicalCauses &, const space::Space &,
                           IdentityRead, std::span<const Handle>,
                           std::vector<BodyCauseRow> &);
void restore_motion_causes(MechanicalCauses &, space::Space &, IdentityRead,
                           std::span<const BodyCauseRow>);
void write_motion_causes(snap::Output &, const std::vector<BodyCauseRow> &,
                         const snap::ObjectSelection & = {});
void read_motion_causes(snap::Input &, std::vector<BodyCauseRow> &);
```

Main appends this owned chunk after space's existing MOVE rows and restores it
after ordinary bodies, attachments, promoted compounds and velocities. Tokens
are regenerated; no volatile Jolt IDs or callback pointers are saved. Captures
use exactly their selected handle closure, with an empty selection producing no
rows. Like MOVE velocities, only independent dynamic/controller roots persist;
transient kinematic pose tags and derived compound children do not. Lifecycle
removal clears native tags before
slot reuse, while the small causal map is reclaimed after publication.

Tests must prove final nonempty navigation Origin; exact attachment watches;
delayed impulse/release/pose and real CharacterVirtual contacts; separate calls
within one phase retaining the correct captured token; stationary pose and
sleeping-body expiry; bounded multi-root ambiguity; no credit from consulted
objects; uninstrumented lifecycle/custody suppression; stale-slot cleanup; and
restored in-flight cause/author/oldest preservation. Density probes must confirm
expiry/collection cost follows active causes rather than inert population.

## Measured causal density check

The actual Release Jolt step plus cause expiry/collection, 200 steps per case,
measured 1.30/1.31µs p50/p95 with 10,000 inert bodies and 1.22/1.47µs with
100,000 inert bodies. With 32 commanded moving rigid bodies those costs were
43.9/57.2µs and 59.6/86.4µs. Other native compilation was active; cold maintenance
maxima and the executable hash are retained in
`reports/physical-causality-density-2026-11-15.json`. This measures simulation
and causal collection, not renderer or maximum contact/crowd capacity.
