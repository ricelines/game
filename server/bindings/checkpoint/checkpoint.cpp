#include "checkpoint.hpp"
#include "bindings/causality_snapshot.hpp"
#include "bindings/turns/turns.hpp"
#include "bindings/viewing/viewing.hpp"
#include "checkpoint_filter.hpp"

#include <algorithm>
#include <set>
#include <tracy/Tracy.hpp>

namespace amber::bindings::checkpoint {
namespace {
void encode(Encoding &source, snap::Buffer &buffer, const snap::Header &header) {
  if (source.filter.local) {
    space::capture_motion(source.space, source.identity, source.filter.selected, source.motion);
    capture_motion_causes(source.mechanical_causes, source.space, source.identity,
                          source.filter.selected, source.causes);
  } else {
    space::capture_motion(source.space, source.identity, source.motion);
    std::vector<Handle> moving;
    moving.reserve(source.motion.size());
    for (const auto &row : source.motion)
      moving.push_back(row.object);
    capture_motion_causes(source.mechanical_causes, source.space, source.identity, moving,
                          source.causes);
  }
  snap::encode(buffer, header, source.sections);
}
} // namespace

bool save_snapshot(snap::Writer &writer, Encoding &source, SnapshotProgress &progress,
                   const snap::Header &header) {
  ZoneScopedN("snapshot capture");
  auto buffer = writer.acquire();
  if (!buffer)
    return false;
  const auto started = drive::Monotonic::now();
  encode(source, *buffer, header);
  progress.pause_max_ms = std::max(
      progress.pause_max_ms,
      std::chrono::duration<double, std::milli>(drive::Monotonic::now() - started).count());
  writer.submit(std::move(*buffer), header.step);
  progress.last_step = header.step;
  return true;
}

void flush_gaps(GapCaptures &state, const snap::Header &header) {
  const auto &read = state.read;
  const auto &values = read.values;
  const auto identity = values.identity;
  for (const auto &gap : state.pending) {
    if (!identity.valid(gap.target))
      continue;
    const elaborate::GapKey key{gap.term, identity.material[gap.target.slot()]};
    const auto noted = elaborate::note_gap(
        state.counts, state.last, state.requests,
        {gap.action, key, gap.target, gap.requester, header.step, {}, gap.origin},
        state.capture_limit);
    record::Event event;
    event.step = header.step;
    event.kind = record::Kind::Gap;
    event.name = values.terms.rows[gap.term].name;
    if (gap.requester)
      event.actor = record::Actor::player(gap.requester);
    event.things = {gap.target};
    event.detail = {
        {"count", noted.count}, {"term", gap.term}, {"material", key.material}, {"agent", false}};
    if (noted.capture_required) {
      auto &filter = state.encoding.filter;
      try {
        std::vector<Handle> seeds{gap.target};
        if (gap.requester) {
          for (const auto &[session, view] : read.views.rows) {
            (void)session;
            if (view.player == gap.requester)
              seeds.insert(seeds.end(), view.last_observation.begin(), view.last_observation.end());
          }
          if (gap.requester < read.profiles.rows.size())
            seeds.push_back(read.profiles.rows[gap.requester].body);
        } else {
          std::vector<space::Nearby> nearby;
          if (!space::near(state.encoding.space, identity,
                           read.transforms.position[gap.target.slot()], state.radius_m, {},
                           space::Deadline::max(), nearby))
            throw std::runtime_error("capture neighborhood query incomplete");
          for (const auto &row : nearby)
            seeds.push_back(row.object);
        }
        std::vector<const Relation *> edges;
        for (const auto &[term, relation] : values.relations.columns) {
          (void)term;
          edges.push_back(relation);
        }
        const auto local = neighborhood(identity, read.parents, read.parts, edges, seeds);
        filter.local = true;
        filter.selected = priority_neighborhood(identity, read.parents, read.parts, values.terms,
                                                values.shared, local);
        const auto selected = std::span<const Handle>(filter.selected);
        filter.objects = snap::ObjectSelection(selected);
        filter.catalog = catalog_selection(read.models, read.parameters, read.installations,
                                           identity, values.params, read.rules, selected);
        const std::array<PlayerId, 1> actors{gap.requester};
        filter.authority =
            perm::capture_authority(read.authority, selected, actors, read.players, read.plots,
                                    read.groups, read.grants, read.offers, read.trades);
        filter.plots.assign(filter.authority->plots.begin(), filter.authority->plots.end());
        snap::Buffer bytes;
        encode(state.encoding, bytes, header);
        const auto hash = state.writer.submit(std::move(bytes));
        state.captures.rows.try_emplace(
            hash, elaborate::CaptureRef{hash, {}, elaborate::CaptureStatus::Pending, {}});
        elaborate::attach_capture(state.requests, state.last, noted, gap.requester, hash);
        event.detail["capture"] = hash;
        event.detail["objects"] = selected.size();
        event.detail["priority_dependencies"] = selected.size() - local.size();
      } catch (const std::exception &error) {
        event.detail["capture_error"] = error.what();
      }
      filter = {};
    }
    state.journal.append(std::move(event), gap.origin);
  }
  state.pending.clear();
}

std::vector<Handle> neighborhood(IdentityRead identity, ParentRead parents,
                                 const lifecycle::Parts &parts,
                                 std::span<const Relation *const> relations,
                                 std::span<const Handle> seeds) {
  std::vector<Handle> result;
  std::unordered_set<std::uint64_t> seen;
  auto add = [&](Handle object) {
    if (identity.valid(object) && seen.insert(object.bits).second)
      result.push_back(object);
  };
  for (auto seed : seeds)
    add(seed);
  const auto base = result;
  for (auto seed : base)
    for (const auto *relation : relations)
      for (const auto *column : {&relation->out, &relation->in})
        if (auto edges = column->get(seed.slot()))
          for (auto other : *edges)
            add(other);
  // Only structural coherence closes recursively; ordinary graph adjacency is one edge.
  for (std::size_t i = 0; i < result.size(); ++i) {
    auto object = result[i];
    add(parents.parent[object.slot()]);
    if (const auto *children = parts.get(object.slot()))
      for (const auto &child : *children)
        add(child.object);
  }
  std::sort(result.begin(), result.end());
  return result;
}
std::vector<Handle> priority_neighborhood(IdentityRead identity, ParentRead parents,
                                          const lifecycle::Parts &parts, const react::Terms &terms,
                                          const react::SharedColumns &shared,
                                          std::span<const Handle> initial) {
  std::vector<Handle> required(initial.begin(), initial.end());
  for (const auto &term : terms.rows)
    if (term.combine == react::Combine::SharePriority && term.id < shared.size())
      for (auto subject : initial)
        if (const auto *demand = shared[term.id].get(subject.slot()))
          for (const auto &part : demand->parts)
            if (part.amount > 0 && identity.valid(part.from))
              required.push_back(part.from);
  // No ordinary relation expansion from added contributors, nor a second
  // shared-incidence pass: only their necessary assembly/custody closure.
  return neighborhood(identity, parents, parts, {}, required);
}
CatalogSelection catalog_selection(const catalog::Models &models,
                                   const catalog::ParameterBlocks &parameters,
                                   const install::InstanceScripts &installed, IdentityRead identity,
                                   const std::vector<std::uint32_t> &object_parameters,
                                   const react::RuleVersions &rules,
                                   std::span<const Handle> objects) {
  std::set<ModelId> model_ids;
  std::set<std::uint32_t> parameter_ids;
  std::set<ScriptId> script_ids;
  std::vector<ModelId> pending;
  auto add_model = [&](ModelId id) {
    if (id && model_ids.insert(id).second)
      pending.push_back(id);
  };
  for (auto object : objects) {
    add_model(identity.model[object.slot()]);
    if (const auto *scripts = installed.get(object.slot()))
      script_ids.insert(scripts->begin(), scripts->end());
    if (const auto id = object_parameters[object.slot()]) {
      parameter_ids.insert(id);
      const auto &block = parameters.rows.at(id);
      add_model(block.model);
      for (const auto &[name, child] : block.parts) {
        (void)name;
        add_model(child);
      }
    }
  }
  while (!pending.empty()) {
    auto id = pending.back();
    pending.pop_back();
    const auto &model = models.rows.at(id);
    if (model.script)
      script_ids.insert(model.script);
    for (const auto &[name, child] : model.parts) {
      (void)name;
      add_model(child);
    }
  }
  for (const auto &rule : rules.history)
    script_ids.insert(rule.handler.script);
  script_ids.erase(0);
  return {{model_ids.begin(), model_ids.end()},
          {parameter_ids.begin(), parameter_ids.end()},
          {script_ids.begin(), script_ids.end()}};
}
void write_prop(snap::Output &out, const react::Properties &properties,
                const react::Processes &processes, const react::InstanceSettings &instances,
                const react::ModelSettings &models, const react::PlotSettings &plots,
                const react::WorldSettings &world, const snap::ObjectSelection &selected,
                const CatalogSelection *catalog_filter,
                std::optional<std::span<const PlotId>> plot_ids) {
  react::write_properties(out, properties, processes, selected);
  react::write_settings(
      out, instances, models, plots, world, selected,
      catalog_filter ? catalog::ModelSelection(catalog_filter->models) : std::nullopt, plot_ids);
}
void read_prop(snap::Input &in, react::Properties &properties, react::Processes &processes,
               react::InstanceSettings &instances, react::ModelSettings &models,
               react::PlotSettings &plots, react::WorldSettings &world, std::size_t slots) {
  react::read_properties(in, properties, processes, slots);
  react::read_settings(in, instances, models, plots, world, slots);
}
void write_rels(snap::Output &out, const react::AuthoredRelations &authored,
                const space::PhysicalRelations &physical, const lifecycle::Parts &parts,
                IdentityRead identity, std::optional<std::span<const Handle>> selected) {
  snap::ObjectSelection filter =
      selected ? snap::ObjectSelection(*selected) : snap::ObjectSelection{};
  react::write_relations(out, authored, filter);
  space::write_physical_relations(out, physical, filter);
  lifecycle::write_parts(out, parts, identity, selected);
}
void read_rels(snap::Input &in, react::AuthoredRelations &authored,
               space::PhysicalRelations &physical, lifecycle::Parts &parts, IdentityRead identity,
               Relation &declared) {
  const auto slots = identity.generation.size();
  react::read_relations(in, authored, slots);
  space::read_physical_relations(in, physical, slots);
  lifecycle::read_parts(in, parts, identity, declared);
}
void write_text(snap::Output &out, const catalog::Models &models,
                const catalog::ParameterBlocks &parameters, const catalog::Scripts &scripts,
                const catalog::ModelTexts &model_text, const catalog::InstanceTexts &instance_text,
                const catalog::AuthoredTexts &authored_text, const CatalogSelection *selected,
                std::optional<std::span<const Handle>> objects) {
  const auto model_ids = selected ? catalog::ModelSelection(selected->models) : std::nullopt;
  catalog::write_models(out, models, model_ids);
  catalog::write_parameters(
      out, parameters, selected ? catalog::ParameterSelection(selected->parameters) : std::nullopt);
  catalog::write_scripts(out, scripts,
                         selected ? catalog::ScriptSelection(selected->scripts) : std::nullopt);
  catalog::write_text(out, model_text, instance_text, model_ids, objects);
  catalog::write_authored_texts(out, authored_text, objects);
}
void read_text(snap::Input &in, catalog::Models &models, catalog::ParameterBlocks &parameters,
               catalog::Scripts &scripts, catalog::ModelTexts &model_text,
               catalog::InstanceTexts &instance_text, catalog::AuthoredTexts &authored_text) {
  catalog::read_models(in, models);
  catalog::read_parameters(in, parameters);
  catalog::read_scripts(in, scripts);
  catalog::read_text(in, model_text, instance_text);
  catalog::read_authored_texts(in, authored_text);
}
void write_players(snap::Output &out, const perm::Players &players, const perm::Ledger &ledger,
                   const perm::Plots &plots, const perm::Groups &groups,
                   const control::Profiles &profiles, const perm::SnapshotSelection *selected) {
  perm::write_players(out, players, ledger, plots, groups, selected);
  std::vector<PlayerId> ids;
  if (selected)
    ids.assign(selected->players.begin(), selected->players.end());
  control::write_profiles(out, profiles,
                          selected ? std::optional<std::span<const PlayerId>>(ids) : std::nullopt);
}
void read_players(snap::Input &in, perm::Players &players, perm::Ledger &ledger, perm::Plots &plots,
                  perm::Groups &groups, control::Profiles &profiles) {
  perm::read_players(in, players, ledger, plots, groups);
  control::read_profiles(in, profiles);
}
void write_scst(snap::Output &out, const react::ScriptStates &states,
                const react::Subscriptions &subscriptions,
                const install::InstanceScripts &installed, IdentityRead identity,
                std::optional<std::span<const Handle>> selected) {
  react::write_script_states(out, states, subscriptions,
                             selected ? snap::ObjectSelection(*selected) : snap::ObjectSelection{});
  install::write_instances(out, installed, identity, selected);
}
void read_scst(snap::Input &in, react::ScriptStates &states, react::Subscriptions &subscriptions,
               install::InstanceScripts &installed, std::size_t slots) {
  react::read_script_states(in, states, subscriptions);
  install::read_instances(in, installed, slots);
}
void write_queues(snap::Output &out, const step::WorkQueues &queues, const step::Intents &intents,
                  std::uint64_t next_record, const drive::Inputs &inputs,
                  const snap::ObjectSelection &selected,
                  std::span<const std::uint64_t> selected_inputs) {
  step::write_queues(out, queues, intents, next_record, selected, selected_inputs);
  drive::write_inputs(
      out, inputs,
      selected.all ? std::nullopt : std::optional<std::span<const std::uint64_t>>(selected_inputs));
}
std::uint64_t read_queues(snap::Input &in, step::WorkQueues &queues, step::Intents &intents,
                          drive::Inputs &inputs) {
  auto next_record = step::read_queues(in, queues, intents);
  drive::read_inputs(in, inputs);
  return next_record;
}
void write_reqs(snap::Output &out, const elaborate::Requests &requests,
                const elaborate::GapCounts &gaps, const elaborate::LastGaps &last,
                const elaborate::CaptureRefs &captures, const elaborate::PendingFixtures &pending,
                std::optional<std::span<const Handle>> selected) {
  std::set<elaborate::GapKey> gap_ids;
  std::set<elaborate::Hash> hashes;
  auto included = [&](Handle target) {
    return !selected || std::find(selected->begin(), selected->end(), target) != selected->end();
  };
  if (selected) {
    for (const auto &row : requests.rows)
      if (row.id && included(row.target)) {
        gap_ids.insert(row.gap);
        if (!row.capture.empty())
          hashes.insert(row.capture);
      }
    for (const auto &[player, row] : last.rows) {
      (void)player;
      if (included(row.target)) {
        gap_ids.insert(row.gap);
        if (!row.capture.empty())
          hashes.insert(row.capture);
      }
    }
  }
  std::vector<elaborate::GapKey> keys(gap_ids.begin(), gap_ids.end());
  std::vector<elaborate::Hash> capture_ids(hashes.begin(), hashes.end());
  elaborate::write_requests(out, requests, selected);
  elaborate::write_gaps(out, gaps, selected ? elaborate::GapSelection(keys) : std::nullopt);
  elaborate::write_last_gaps(out, last, selected);
  elaborate::write_capture_refs(out, captures,
                                selected ? elaborate::HashSelection(capture_ids) : std::nullopt);
  elaborate::write_pending_fixtures(out, pending, !selected);
}
void read_reqs(snap::Input &in, elaborate::Requests &requests, elaborate::GapCounts &gaps,
               elaborate::LastGaps &last, elaborate::CaptureRefs &captures,
               elaborate::PendingFixtures &pending) {
  elaborate::read_requests(in, requests);
  elaborate::read_gaps(in, gaps);
  elaborate::read_last_gaps(in, last);
  elaborate::read_capture_refs(in, captures);
  elaborate::read_pending_fixtures(in, pending);
}
} // namespace amber::bindings::checkpoint
