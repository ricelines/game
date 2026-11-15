#include "snapshot.hpp"
#include "snap/codec.hpp"

#include <unordered_set>

namespace amber::lifecycle {
namespace {
template <class S> void names(S &s, std::vector<NamedPart> &parts) {
  s.container(parts, snap::max_collection(), [](S &in, NamedPart &part) {
    snap::field(in, part.name);
    in.object(part.object);
  });
}
} // namespace
void write_parts(snap::Output &out, const Parts &parts, IdentityRead identity,
                 std::optional<std::span<const Handle>> selected) {
  std::unordered_set<std::uint64_t> included;
  if (selected)
    for (auto object : *selected)
      included.insert(object.bits);
  Parts filtered;
  const Parts *source = &parts;
  if (selected) {
    source = &filtered;
    for (std::size_t i = 0; i < parts.value.size(); ++i) {
      const auto root = identity.at(parts.slot_of[i]);
      if (!identity.valid(root) || !included.contains(root.bits))
        continue;
      auto row = parts.value[i];
      std::erase_if(row, [&](const auto &part) { return !included.contains(part.object.bits); });
      if (!row.empty())
        filtered.set(root.slot(), std::move(row));
    }
  }
  snap::write_sparse(out, *source, names<snap::Output>);
}
void read_parts(snap::Input &in, Parts &parts, IdentityRead identity, Relation &declared) {
  snap::read_sparse(in, parts, names<snap::Input>);
  parts.rebuild_positions(identity.generation.size());
  rebuild_declared_parts(parts, identity, declared);
}
} // namespace amber::lifecycle
