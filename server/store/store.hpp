#pragma once

#include <cassert>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace amber {

// A script run's time limit (S4.3): CPU time used by the thread running it.
struct RunDeadline {
  std::chrono::steady_clock::time_point wall = std::chrono::steady_clock::time_point::max();
  std::chrono::nanoseconds cpu_end{};
  static RunDeadline after(double milliseconds);
  bool passed();
};

using Step = std::uint64_t;
using PlayerId = std::uint32_t;
using ModelId = std::uint32_t;
using TermId = std::uint32_t;
using ScriptId = std::uint32_t;
using GroupId = std::uint32_t;
using PlotId = std::uint32_t;
using GrantId = std::uint32_t;

// Carried by queued work and timers; no later record lookup is needed.
struct Origin {
  std::uint64_t cause = 0;
  std::uint64_t root = 0;
  std::vector<PlayerId> authors;
  std::optional<double> oldest;
  bool more = false;
  bool operator==(const Origin &) const = default;
};

struct Handle {
  std::uint64_t bits = 0;

  static constexpr Handle make(std::uint32_t slot, std::uint32_t generation) {
    return {std::uint64_t{generation} << 32 | slot};
  }
  constexpr std::uint32_t slot() const { return static_cast<std::uint32_t>(bits); }
  constexpr std::uint32_t generation() const { return static_cast<std::uint32_t>(bits >> 32); }
  constexpr explicit operator bool() const { return bits != 0; }
  auto operator<=>(const Handle &) const = default;
};

std::string to_string(Handle handle);
std::optional<Handle> parse_handle(std::string_view text);

struct Vec3 {
  float x = 0;
  float y = 0;
  float z = 0;

  auto operator<=>(const Vec3 &) const = default;
  Vec3 operator+(Vec3 other) const { return {x + other.x, y + other.y, z + other.z}; }
  Vec3 operator-(Vec3 other) const { return {x - other.x, y - other.y, z - other.z}; }
  Vec3 operator*(float scale) const { return {x * scale, y * scale, z * scale}; }
  float length_squared() const { return x * x + y * y + z * z; }
  float length() const { return std::sqrt(length_squared()); }
};

struct Quat {
  float x = 0;
  float y = 0;
  float z = 0;
  float w = 1;
  auto operator<=>(const Quat &) const = default;
};
// Rigid transforms use unit quaternions, normalized once at input boundaries.
Quat compose(Quat, Quat);
Vec3 rotate(Quat, Vec3);

inline constexpr std::uint32_t no_position = std::numeric_limits<std::uint32_t>::max();

// Object-keyed columns have one dense iteration order and one slot lookup.
template <class T> struct SparseColumn {
  std::vector<std::uint32_t> slot_of;
  std::vector<T> value;
  std::vector<std::uint32_t> pos_of;

  const T *get(std::uint32_t slot) const {
    if (slot >= pos_of.size() || pos_of[slot] == no_position)
      return nullptr;
    return &value[pos_of[slot]];
  }

  T *get(std::uint32_t slot) { return const_cast<T *>(std::as_const(*this).get(slot)); }

  template <class U> T &set(std::uint32_t slot, U &&entry) {
    if (slot >= pos_of.size())
      pos_of.resize(static_cast<std::size_t>(slot) + 1, no_position);
    auto &position = pos_of[slot];
    if (position == no_position) {
      position = static_cast<std::uint32_t>(value.size());
      slot_of.push_back(slot);
      value.emplace_back(std::forward<U>(entry));
    } else {
      value[position] = std::forward<U>(entry);
    }
    return value[position];
  }

  bool erase(std::uint32_t slot) {
    if (slot >= pos_of.size() || pos_of[slot] == no_position)
      return false;
    const auto position = pos_of[slot];
    if (position + 1 != value.size()) {
      value[position] = std::move(value.back());
      slot_of[position] = slot_of.back();
      pos_of[slot_of[position]] = position;
    }
    value.pop_back();
    slot_of.pop_back();
    pos_of[slot] = no_position;
    return true;
  }

  void clear() {
    slot_of.clear();
    value.clear();
    pos_of.clear();
  }

  // Snapshot payloads contain dense pairs; reconstruct the derived lookup.
  void rebuild_positions(std::size_t slots) {
    pos_of.assign(slots, no_position);
    for (std::uint32_t position = 0; position < slot_of.size(); ++position) {
      assert(slot_of[position] < slots && pos_of[slot_of[position]] == no_position);
      pos_of[slot_of[position]] = position;
    }
  }
};

// Canonical SI scalar values. Text and transient event records live separately.
struct Value {
  enum class Kind : std::uint8_t { None, Number, Integer, Boolean, Object };
  Kind kind = Kind::None;
  union {
    double number;
    std::int64_t integer;
    bool boolean;
    Handle object;
  };
  constexpr Value() : integer(0) {}
  constexpr Value(double value) : kind(Kind::Number), number(value) {}
  constexpr Value(std::int64_t value) : kind(Kind::Integer), integer(value) {}
  constexpr Value(bool value) : kind(Kind::Boolean), boolean(value) {}
  constexpr Value(Handle value) : kind(Kind::Object), object(value) {}
  constexpr bool present() const { return kind != Kind::None; }
  constexpr bool is_numeric() const {
    return kind == Kind::Number || kind == Kind::Integer || kind == Kind::Boolean;
  }
  double numeric() const {
    switch (kind) {
    case Kind::Number:
      return number;
    case Kind::Integer:
      return static_cast<double>(integer);
    case Kind::Boolean:
      return boolean ? 1.0 : 0.0;
    default:
      assert(false && "value is not numeric");
      return 0.0;
    }
  }
  bool operator==(const Value &other) const {
    if (kind != other.kind)
      return false;
    switch (kind) {
    case Kind::None:
      return true;
    case Kind::Number:
      return number == other.number;
    case Kind::Integer:
      return integer == other.integer;
    case Kind::Boolean:
      return boolean == other.boolean;
    case Kind::Object:
      return object == other.object;
    }
    return false;
  }
};
static_assert(sizeof(Value) == 16);

struct TermRef {
  TermId id = 0;
  bool operator==(const TermRef &) const = default;
};
using EventAtom = std::variant<Value, Vec3, std::string, TermRef>;
struct EventField {
  std::string name;
  EventAtom value;
  bool operator==(const EventField &) const = default;
};
using EventPayload = std::variant<Value, Vec3, std::string, std::vector<EventField>>;

struct FactKey {
  Handle subject;
  TermId term = 0;
  bool operator==(const FactKey &) const = default;
};
struct FactKeyHash {
  std::size_t operator()(const FactKey &key) const {
    return std::hash<std::uint64_t>{}(key.subject.bits ^
                                      (std::uint64_t{key.term} * 0x9e3779b97f4a7c15ULL));
  }
};
struct ScriptKey {
  Handle object;
  ScriptId script = 0;
  bool operator==(const ScriptKey &) const = default;
};
struct ScriptKeyHash {
  std::size_t operator()(const ScriptKey &key) const {
    return FactKeyHash{}({key.object, key.script});
  }
};
struct ScriptHolder {
  ScriptId script = 0;
  Handle object;
  bool operator==(const ScriptHolder &) const = default;
};
using Caller = std::variant<PlayerId, ScriptHolder>;
struct ActionRequest {
  Caller caller = PlayerId{0};
  TermId action = 0;
  Handle target;
  EventPayload args;
};

struct Relation {
  SparseColumn<std::vector<Handle>> out, in;
};

enum ObjectFlag : std::uint32_t {
  Static = 1,
  Dynamic = 2,
  Body = 4,
  ScriptedMotion = 8,
  Held = 16,
  Proxy = 32,
  NoCopy = 64,
  Escrow = 128,
};

struct ObjectInit {
  Vec3 position;
  Quat rotation;
  ModelId model = 0;
  TermId material = 0;
  PlayerId owner = 0;
  PlayerId maker = 0;
  Step created_step = 0;
  GroupId group = 0;
  std::uint32_t preset = 0;
  PlotId plot = 0;
  Handle parent;
  std::uint32_t flags = Static;
  std::uint32_t params = 0;
};

struct Objects {
  std::vector<std::uint32_t> generation{0};
  std::vector<Vec3> position{{}};
  std::vector<Quat> rotation{{}};
  std::vector<ModelId> model{0};
  std::vector<TermId> material{0};
  std::vector<PlayerId> owner{0};
  std::vector<PlayerId> maker{0};
  std::vector<Step> created_step{0};
  std::vector<GroupId> group{0};
  std::vector<std::uint32_t> preset{0};
  std::vector<PlotId> plot{0};
  std::unordered_map<PlotId, std::vector<std::uint32_t>> on_plot;
  std::vector<Handle> parent{{}};
  std::vector<std::uint32_t> flags{0};
  std::vector<std::uint32_t> params{0};
  std::vector<std::uint32_t> free_slots;
};

struct IdentityRead {
  const std::vector<std::uint32_t> &generation;
  const std::vector<ModelId> &model;
  const std::vector<TermId> &material;
  const std::vector<std::uint32_t> &flags;

  bool valid(Handle handle) const {
    return handle.slot() < generation.size() && model[handle.slot()] != 0 &&
           generation[handle.slot()] == handle.generation();
  }
  Handle at(std::uint32_t slot) const { return Handle::make(slot, generation[slot]); }
};

struct TransformRead {
  const std::vector<Vec3> &position;
  const std::vector<Quat> &rotation;
};

struct TransformWrite {
  std::vector<Vec3> &position;
  std::vector<Quat> &rotation;
  operator TransformRead() const { return {position, rotation}; }
};

struct AuthorityRead {
  const std::vector<std::uint32_t> &generation;
  const std::vector<PlayerId> &owner;
  const std::vector<GroupId> &group;
  const std::vector<std::uint32_t> &preset;
  const std::vector<PlotId> &plot;
};

struct AuthorityWrite {
  const std::vector<std::uint32_t> &generation;
  std::vector<PlayerId> &owner;
  std::vector<GroupId> &group;
  std::vector<std::uint32_t> &preset;
  const std::vector<PlotId> &plot;
  operator AuthorityRead() const { return {generation, owner, group, preset, plot}; }
};

struct AssetBindingWrite {
  std::vector<ModelId> &model;
  std::vector<TermId> &material;
  std::vector<std::uint32_t> &params;
};

// Runtime writers cannot change another module's flag bits or create a body.
template <std::uint32_t Mask> class FlagWrite {
public:
  explicit FlagWrite(std::vector<std::uint32_t> &flags) : flags_(flags) {}
  void set(std::uint32_t slot, std::uint32_t bits) const {
    assert((bits & ~Mask) == 0);
    flags_[slot] = (flags_[slot] & ~Mask) | (bits & Mask);
  }
  std::uint32_t get(std::uint32_t slot) const { return flags_[slot] & Mask; }

private:
  std::vector<std::uint32_t> &flags_;
};

using SpaceFlags = FlagWrite<Static | Dynamic | ScriptedMotion | Held>;
using PermissionFlags = FlagWrite<NoCopy | Escrow>;

struct AttributionRead {
  const std::vector<PlayerId> &maker;
  const std::vector<Step> &created_step;
};

struct ParentRead {
  const std::vector<Handle> &parent;
};

struct ParentWrite {
  std::vector<Handle> &parent;
  operator ParentRead() const { return {parent}; }
};

IdentityRead identity(const Objects &objects);
TransformRead transforms(const Objects &objects);
TransformWrite transforms(Objects &objects);
AuthorityRead authority(const Objects &objects);
AuthorityWrite authority(Objects &objects);
AttributionRead attribution(const Objects &objects);
ParentRead parents(const Objects &objects);
ParentWrite parents(Objects &objects);
AssetBindingWrite asset_bindings(Objects &objects);
SpaceFlags space_flags(Objects &objects);
PermissionFlags permission_flags(Objects &objects);

Handle create(Objects &objects, const ObjectInit &init);
void destroy(Objects &objects, Handle handle);
void set_plot(Objects &, std::uint32_t slot, PlotId);
void rebuild_plot_membership(Objects &);

} // namespace amber

namespace amber::lifecycle {
struct NamedPart {
  std::string name;
  Handle object;
  bool operator==(const NamedPart &) const = default;
};
using Parts = SparseColumn<std::vector<NamedPart>>;
struct PartTemplate {
  std::string name;
  ModelId model = 0;
  TermId material = 0;
  Vec3 position;
  Quat rotation;
};
struct AssemblyTemplate {
  ModelId model = 0;
  std::uint32_t params = 0;
  TermId material = 0;
  std::vector<PartTemplate> parts;
};

struct Created {
  Handle root;
  std::vector<Handle> objects;
};
Created instantiate(Objects &, Parts &, Relation &declared_parts, const AssemblyTemplate &,
                    ObjectInit root);
std::vector<Handle> collect(const Parts &, IdentityRead, std::span<const Handle> roots);
std::vector<Handle> collect(const Parts &, IdentityRead, Handle root);
// Step coordinates owner cleanup before row release.
// The input is the already collected full closure, not an independent root list.
void release_rows(Objects &, Parts &, Relation &declared_parts, std::span<const Handle>);
struct Binding {
  ModelId model = 0;
  TermId material = 0;
  std::uint32_t params = 0;
  bool operator==(const Binding &) const = default;
};
struct Rebinding {
  Handle object;
  Binding before, after;
  Vec3 position;
  Quat rotation;
};
struct Replacement {
  Handle root;
  Binding original;
  std::vector<NamedPart> original_parts;
  std::vector<Rebinding> retained;
  std::vector<PartTemplate> added;
  std::vector<Handle> removed;
};
// Binding/name/material decisions only; retained poses are filled by plan_replace.

// Step applies returned row changes and retirement before resuming the caller.
Created apply_replacement(Objects &, Parts &, Relation &declared_parts, const Replacement &,
                          PlayerId edit_maker, Step now);
void rebuild_declared_parts(const Parts &, IdentityRead, Relation &);

} // namespace amber::lifecycle

namespace amber {
struct ObjectState {
  Objects objects;
  lifecycle::Parts parts;
  Relation declared_parts;
};
} // namespace amber
