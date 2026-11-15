#include "credits.hpp"

#include <algorithm>

namespace amber::bindings {
std::vector<record::Credit> resolve_credits(const void *context, std::span<const Handle> objects,
                                            ScriptId script, double hz) {
  const auto &read = *static_cast<const MetadataCredits *>(context);
  std::vector<record::Credit> result;
  auto add = [&](PlayerId author, double time) {
    const bool seed = std::find(read.seed_authors.begin(), read.seed_authors.end(), author) !=
                      read.seed_authors.end();
    result.push_back({author, time, seed});
  };
  for (auto object : objects)
    if (read.identity.valid(object)) {
      const auto slot = object.slot();
      add(read.attribution.maker[slot],
          static_cast<double>(read.attribution.created_step[slot]) / hz);
      const auto &model = read.models.rows.at(read.identity.model[slot]);
      add(model.author, model.created_s);
    }
  if (script) {
    const auto &row = read.scripts.rows.at(script);
    add(row.author, row.created_s);
  }
  return result;
}
} // namespace amber::bindings
