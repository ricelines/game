#include "react.hpp"
#include "snap/snap.hpp"
#include <algorithm>

namespace amber::react {
template <class S> void serialize(S &s, Process &v) {
  snap::field(s, v.v0);
  snap::field(s, v.step0);
  snap::field(s, v.rate);
  snap::field(s, v.lo);
  snap::field(s, v.hi);
}
template <class S> void serialize(S &s, Contribution &v) {
  snap::field(s, v.from);
  snap::field(s, v.amount);
}
template <class S> void serialize(S &s, Shared &v) {
  snap::field(s, v.total);
  snap::field(s, v.parts);
}
template <class S> void serialize(S &s, SubjectTerm &v) {
  snap::field(s, v.subject);
  snap::field(s, v.term);
}
template <class S> void serialize(S &s, WorldScript &v) {
  snap::field(s, v.script);
  snap::field(s, v.space);
  snap::field(s, v.description);
  snap::field(s, v.writing);
}
template <class S> void serialize(S &s, Answer &v) {
  snap::field(s, v.present);
  snap::field(s, v.value);
}
template <class S> void serialize(S &s, SharedWatchAnswer &v) {
  snap::field(s, v.aggregate);
  snap::field(s, v.own_allocation);
}
template <class S> void serialize(S &s, RelationAnswer &v) {
  snap::field(s, v.out);
  snap::field(s, v.in);
}

namespace {
template <class Map, class Pred> void write_map(snap::Output &out, const Map &map, Pred include) {
  std::vector<std::pair<typename Map::key_type, typename Map::mapped_type>> entries;
  for (const auto &[key, value] : map)
    if (include(key))
      entries.emplace_back(key, value);
  out.container(entries, snap::max_collection(), [](snap::Output &s, auto &entry) {
    snap::field(s, entry.first);
    snap::field(s, entry.second);
  });
}
template <class Map> void read_map(snap::Input &in, Map &map) {
  map.clear();
  std::vector<std::pair<typename Map::key_type, typename Map::mapped_type>> entries;
  in.container(entries, snap::max_collection(), [](snap::Input &s, auto &entry) {
    snap::field(s, entry.first);
    snap::field(s, entry.second);
  });
  for (auto &entry : entries)
    map.emplace(std::move(entry.first), std::move(entry.second));
}
template <class T>
void write_sparse(snap::Output &out, const SparseColumn<T> &column,
                  const SnapshotSelection &selected) {
  auto field = [](snap::Output &s, T &value) { snap::field(s, value); };
  if (selected.all) {
    snap::write_sparse(out, column, field);
    return;
  }
  SparseColumn<T> capture;
  for (auto slot : selected.slots)
    if (const auto *value = column.get(slot))
      capture.set(slot, *value);
  snap::write_sparse(out, capture, field);
}
template <class T> void read_sparse(snap::Input &in, SparseColumn<T> &column, std::size_t slots) {
  snap::read_sparse(in, column, [](snap::Input &s, T &value) { snap::field(s, value); });
  column.rebuild_positions(slots);
}
template <class T>
void write_columns(snap::Output &out, const std::vector<SparseColumn<T>> &columns,
                   const SnapshotSelection &selected) {
  out.container(columns, snap::max_collection(),
                [&](snap::Output &s, auto &column) { write_sparse(s, column, selected); });
}
template <class T>
void read_columns(snap::Input &in, std::vector<SparseColumn<T>> &columns, std::size_t slots) {
  in.container(columns, snap::max_collection(),
               [&](snap::Input &s, auto &column) { read_sparse(s, column, slots); });
}
void write_values(snap::Output &out, const Values &values) {
  write_map(out, values, [](auto) { return true; });
}
void read_values(snap::Input &in, Values &values) {
  read_map(in, values);
}
} // namespace
void write_properties(snap::Output &out, const Properties &properties, const Processes &processes,
                      const SnapshotSelection &selected) {
  write_columns(out, properties, selected);
  write_columns(out, processes, selected);
}
void read_properties(snap::Input &in, Properties &properties, Processes &processes,
                     std::size_t slots) {
  read_columns(in, properties, slots);
  read_columns(in, processes, slots);
}
void write_relations(snap::Output &out, const AuthoredRelations &relations,
                     const SnapshotSelection &selected) {
  std::vector<TermId> ids;
  for (const auto &[id, relation] : relations) {
    (void)relation;
    ids.push_back(id);
  }
  out.container(ids, snap::max_collection(), [&](snap::Output &s, TermId &id) {
    s.value4b(id);
    auto write_edges = [&](const auto &column) {
      if (selected.all) {
        write_sparse(s, column, selected);
        return;
      }
      SparseColumn<std::vector<Handle>> kept;
      for (auto slot : selected.slots)
        if (const auto *row = column.get(slot)) {
          auto edges = *row;
          std::erase_if(edges, [&](Handle other) { return !selected.contains(other); });
          if (!edges.empty())
            kept.set(slot, std::move(edges));
        }
      write_sparse(s, kept, selected);
    };
    write_edges(relations.at(id).out);
    write_edges(relations.at(id).in);
  });
}
void read_relations(snap::Input &in, AuthoredRelations &relations, std::size_t slots) {
  relations.clear();
  std::vector<TermId> ids;
  in.container(ids, snap::max_collection(), [&](snap::Input &s, TermId &id) {
    s.value4b(id);
    auto &relation = relations[id];
    read_sparse(s, relation.out, slots);
    read_sparse(s, relation.in, slots);
  });
}
void write_shared(snap::Output &out, const SharedColumns &shared, const GivesTo &gives,
                  const AllocationRows &allocations, const SnapshotSelection &selected) {
  write_columns(out, shared, selected);
  write_sparse(out, gives, selected);
  std::vector<FactKey> pairs;
  for (const auto &[key, shares] : allocations.values)
    if (selected.contains(key.subject) ||
        std::any_of(shares.begin(), shares.end(),
                    [&](const auto &share) { return selected.contains(Handle{share.first}); }))
      pairs.push_back(key);
  out.container(pairs, snap::max_collection(), [&](snap::Output &s, FactKey &key) {
    snap::field(s, key);
    write_map(s, allocations.values.at(key), [&](std::uint64_t contributor) {
      return selected.contains(key.subject) || selected.contains(Handle{contributor});
    });
  });
}
void read_shared(snap::Input &in, SharedColumns &shared, GivesTo &gives,
                 AllocationRows &allocations, std::size_t slots) {
  read_columns(in, shared, slots);
  read_sparse(in, gives, slots);
  allocations.values.clear();
  std::vector<FactKey> pairs;
  in.container(pairs, snap::max_collection(), [&](snap::Input &s, FactKey &key) {
    snap::field(s, key);
    read_map(s, allocations.values[key]);
  });
}
void write_script_states(snap::Output &out, const ScriptStates &states,
                         const WatchAnswers &watch_answers, const Placements &placed,
                         const SnapshotSelection &selected) {
  std::vector<ScriptKey> keys;
  for (const auto &[key, state] : states) {
    (void)state;
    if (selected.all || (key.object && selected.contains(key.object)))
      keys.push_back(key);
  }
  out.container(keys, snap::max_collection(), [&](snap::Output &s, ScriptKey &key) {
    snap::field(s, key);
    write_map(s, states.at(key).values, [](const auto &) { return true; });
  });
  write_map(out, watch_answers,
            [&](const FactKey &source) { return selected.contains(source.subject); });
  write_map(out, placed, [&](std::uint64_t object) { return selected.contains(Handle{object}); });
}
void read_script_states(snap::Input &in, ScriptStates &states, WatchAnswers &watch_answers,
                        Placements &placed) {
  states.clear();
  std::vector<ScriptKey> keys;
  in.container(keys, snap::max_collection(), [&](snap::Input &s, ScriptKey &key) {
    snap::field(s, key);
    ScriptState::Entries entries;
    read_map(s, entries);
    states[key].restore(std::move(entries));
  });
  read_map(in, watch_answers);
  read_map(in, placed);
}
void write_world_scripts(snap::Output &out, const std::vector<WorldScript> &world) {
  auto rows = world;
  snap::field(out, rows);
}
void read_world_scripts(snap::Input &in, std::vector<WorldScript> &world) {
  snap::field(in, world);
}
void write_random(snap::Output &out, const RandomStates &random,
                  const SnapshotSelection &selected) {
  write_map(out, random, [&](std::uint64_t key) { return selected.contains(Handle{key}); });
}
void read_random(snap::Input &in, RandomStates &random) {
  read_map(in, random);
}
void write_settings(snap::Output &out, const InstanceSettings &instances,
                    const ModelSettings &models, const PlotSettings &plots,
                    const WorldSettings &world, const SnapshotSelection &selected,
                    std::optional<std::span<const ModelId>> model_ids,
                    std::optional<std::span<const PlotId>> plot_ids) {
  write_columns(out, instances.columns, selected);
  auto rows = [&](const auto &table, auto included) {
    std::vector<std::uint32_t> ids;
    for (const auto &[id, value] : table) {
      (void)value;
      if (!included || std::find(included->begin(), included->end(), id) != included->end())
        ids.push_back(id);
    }
    out.container(ids, snap::max_collection(), [&](snap::Output &s, std::uint32_t &id) {
      snap::field(s, id);
      write_values(s, table.at(id));
    });
  };
  rows(models.rows, model_ids);
  rows(plots.rows, plot_ids);
  write_values(out, world.values);
}
void read_settings(snap::Input &in, InstanceSettings &instances, ModelSettings &models,
                   PlotSettings &plots, WorldSettings &world, std::size_t slots) {
  read_columns(in, instances.columns, slots);
  auto rows = [&](auto &table) {
    table.clear();
    std::vector<std::uint32_t> ids;
    in.container(ids, snap::max_collection(), [&](snap::Input &s, std::uint32_t &id) {
      snap::field(s, id);
      read_values(s, table[id]);
    });
  };
  rows(models.rows);
  rows(plots.rows);
  read_values(in, world.values);
}
} // namespace amber::react
