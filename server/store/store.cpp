#include "store.hpp"

#include <charconv>
#include <time.h>

namespace amber {
namespace {
std::chrono::nanoseconds thread_cpu_time() {
  timespec value{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value);
  return std::chrono::seconds(value.tv_sec) + std::chrono::nanoseconds(value.tv_nsec);
}
} // namespace
RunDeadline RunDeadline::after(double milliseconds) {
  const auto budget = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double, std::milli>(milliseconds));
  const auto cpu_end = thread_cpu_time() + budget;
  return {std::chrono::steady_clock::now() + budget, cpu_end};
}
bool RunDeadline::passed() {
  const auto now = std::chrono::steady_clock::now();
  if (now < wall)
    return false;
  const auto cpu = thread_cpu_time();
  if (cpu >= cpu_end)
    return true;
  wall = now + (cpu_end - cpu);
  return false;
}

Quat compose(Quat a, Quat b) {
  return {
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Vec3 rotate(Quat q, Vec3 v) {
  auto cross = [](Vec3 a, Vec3 b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
  };
  const Vec3 u{q.x, q.y, q.z};
  return v + cross(u, v) * (2 * q.w) + cross(u, cross(u, v)) * 2;
}

std::string to_string(Handle handle) {
  return "o:" + std::to_string(handle.slot()) + "." + std::to_string(handle.generation());
}

std::optional<Handle> parse_handle(std::string_view text) {
  if (!text.starts_with("o:"))
    return std::nullopt;
  const auto separator = text.find('.', 2);
  if (separator == std::string_view::npos)
    return std::nullopt;
  std::uint32_t slot = 0;
  std::uint32_t generation = 0;
  const auto slot_result = std::from_chars(text.data() + 2, text.data() + separator, slot);
  const auto generation_result =
      std::from_chars(text.data() + separator + 1, text.data() + text.size(), generation);
  if (slot_result.ec != std::errc{} || slot_result.ptr != text.data() + separator ||
      generation_result.ec != std::errc{} || generation_result.ptr != text.data() + text.size() ||
      slot == 0 || generation == 0)
    return std::nullopt;
  return Handle::make(slot, generation);
}

IdentityRead identity(const Objects &o) {
  return {o.generation, o.model, o.material, o.flags};
}
AssetBindingWrite asset_bindings(Objects &o) {
  return {o.model, o.material, o.params};
}
TransformRead transforms(const Objects &o) {
  return {o.position, o.rotation};
}
TransformWrite transforms(Objects &o) {
  return {o.position, o.rotation};
}
AuthorityRead authority(const Objects &o) {
  return {o.generation, o.owner, o.group, o.preset, o.plot};
}
AuthorityWrite authority(Objects &o) {
  return {o.generation, o.owner, o.group, o.preset, o.plot};
}
AttributionRead attribution(const Objects &o) {
  return {o.maker, o.created_step};
}
ParentRead parents(const Objects &o) {
  return {o.parent};
}
ParentWrite parents(Objects &o) {
  return {o.parent};
}
PlotWrite plot_membership(Objects &o) {
  return {o.plot};
}
SpaceFlags space_flags(Objects &o) {
  return SpaceFlags{o.flags};
}
PermissionFlags permission_flags(Objects &o) {
  return PermissionFlags{o.flags};
}

Handle create(Objects &o, const ObjectInit &init) {
  assert(init.model != 0);
  std::uint32_t slot;
  if (o.free_slots.empty()) {
    slot = static_cast<std::uint32_t>(o.generation.size());
    o.generation.push_back(0);
    o.position.emplace_back();
    o.rotation.emplace_back();
    o.model.push_back(0);
    o.material.push_back(0);
    o.owner.push_back(0);
    o.maker.push_back(0);
    o.created_step.push_back(0);
    o.group.push_back(0);
    o.preset.push_back(0);
    o.plot.push_back(0);
    o.parent.emplace_back();
    o.flags.push_back(0);
    o.params.push_back(0);
  } else {
    slot = o.free_slots.back();
    o.free_slots.pop_back();
  }
  ++o.generation[slot];
  o.position[slot] = init.position;
  o.rotation[slot] = init.rotation;
  o.model[slot] = init.model;
  o.material[slot] = init.material;
  o.owner[slot] = init.owner;
  o.maker[slot] = init.maker;
  o.created_step[slot] = init.created_step;
  o.group[slot] = init.group;
  o.preset[slot] = init.preset;
  o.plot[slot] = init.plot;
  o.parent[slot] = init.parent;
  o.flags[slot] = init.flags;
  o.params[slot] = init.params;
  return Handle::make(slot, o.generation[slot]);
}

void destroy(Objects &o, Handle handle) {
  assert(identity(o).valid(handle));
  const auto slot = handle.slot();
  o.model[slot] = 0;
  o.material[slot] = 0;
  o.owner[slot] = 0;
  o.maker[slot] = 0;
  o.created_step[slot] = 0;
  o.group[slot] = 0;
  o.preset[slot] = 0;
  o.plot[slot] = 0;
  o.parent[slot] = {};
  o.flags[slot] = 0;
  o.params[slot] = 0;
  o.position[slot] = {};
  o.rotation[slot] = {};
  o.free_slots.push_back(slot);
}

} // namespace amber
