#include "net.hpp"

#include <algorithm>

namespace amber::net {
namespace {
template <class S> void profile(S &s, Profile &v) {
  snap::field(s, v.id);
  snap::field(s, v.name);
  snap::field(s, v.body);
  snap::field(s, v.roles);
  snap::field(s, v.connection_modes);
  snap::field(s, v.key_hashes);
}
} // namespace
void write_profiles(snap::Output &out, const Profiles &profiles,
                    std::optional<std::span<const PlayerId>> selected) {
  auto size = static_cast<std::uint32_t>(profiles.rows.size());
  out.value4b(size);
  std::vector<Profile> included;
  for (const auto &row : profiles.rows)
    if (row.id &&
        (!selected || std::find(selected->begin(), selected->end(), row.id) != selected->end()))
      included.push_back(row);
  out.container(included, snap::max_collection(), profile<snap::Output>);
}
void read_profiles(snap::Input &in, Profiles &profiles) {
  std::uint32_t size = 0;
  in.value4b(size);
  Profiles loaded;
  loaded.rows.resize(size);
  std::vector<Profile> included;
  in.container(included, snap::max_collection(), profile<snap::Input>);
  for (auto &row : included) {
    if (!row.id || row.id >= size || loaded.rows[row.id].id)
      throw std::runtime_error("invalid profile snapshot identity");
    loaded.rows[row.id] = std::move(row);
  }
  rebuild_key_index(loaded);
  profiles = std::move(loaded);
}
} // namespace amber::net
