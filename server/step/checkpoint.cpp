#include "step.hpp"
#include <algorithm>
#include <set>
#include <tracy/Tracy.hpp>

namespace amber::step::checkpoint {
namespace {
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
                   const net::Profiles &profiles, const perm::SnapshotSelection *selected) {
  perm::write_players(out, players, ledger, plots, groups, selected);
  std::vector<PlayerId> ids;
  if (selected)
    ids.assign(selected->players.begin(), selected->players.end());
  net::write_profiles(out, profiles,
                      selected ? std::optional<std::span<const PlayerId>>(ids) : std::nullopt);
}
void read_players(snap::Input &in, perm::Players &players, perm::Ledger &ledger, perm::Plots &plots,
                  perm::Groups &groups, net::Profiles &profiles) {
  perm::read_players(in, players, ledger, plots, groups);
  net::read_profiles(in, profiles);
}
void write_scst(snap::Output &out, const react::ScriptStates &states,
                const react::Subscriptions &subscriptions, const react::InstanceScripts &installed,
                IdentityRead identity, std::optional<std::span<const Handle>> selected) {
  react::write_script_states(out, states, subscriptions,
                             selected ? snap::ObjectSelection(*selected) : snap::ObjectSelection{});
  react::write_instances(out, installed, identity, selected);
}
void read_scst(snap::Input &in, react::ScriptStates &states, react::Subscriptions &subscriptions,
               react::InstanceScripts &installed, std::size_t slots) {
  react::read_script_states(in, states, subscriptions);
  react::read_instances(in, installed, slots);
}
void write_queues_section(snap::Output &out, const step::WorkQueues &queues,
                          const step::Intents &intents, std::uint64_t next_record,
                          const step::Inputs &inputs, const snap::ObjectSelection &selected,
                          std::span<const std::uint64_t> selected_inputs) {
  step::write_queues(out, queues, intents, next_record, selected, selected_inputs);
  step::write_inputs(out, inputs,
                     selected.all ? std::nullopt
                                  : std::optional<std::span<const std::uint64_t>>(selected_inputs));
}
std::uint64_t read_queues_section(snap::Input &in, step::WorkQueues &queues, step::Intents &intents,
                                  step::Inputs &inputs) {
  auto next_record = step::read_queues(in, queues, intents);
  step::read_inputs(in, inputs);
  return next_record;
}
void write_reqs(snap::Output &out, const elaborate::Requests &requests,
                const elaborate::GapCounts &gaps, const elaborate::LastGaps &last,
                const elaborate::CaptureRefs &captures,
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
}
void read_reqs(snap::Input &in, elaborate::Requests &requests, elaborate::GapCounts &gaps,
               elaborate::LastGaps &last, elaborate::CaptureRefs &captures) {
  elaborate::read_requests(in, requests);
  elaborate::read_gaps(in, gaps);
  elaborate::read_last_gaps(in, last);
  elaborate::read_capture_refs(in, captures);
}

void sample_motion(const space::Space &space, const MechanicalCauses &causes, IdentityRead identity,
                   const CaptureSelection *selection, std::vector<space::MotionRow> &motion,
                   std::vector<BodyCauseRow> &origins) {
  if (selection) {
    space::capture_motion(space, identity, selection->objects, motion);
    capture_motion_causes(causes, space, identity, selection->objects, origins);
  } else {
    space::capture_motion(space, identity, motion);
    std::vector<Handle> moving;
    moving.reserve(motion.size());
    for (const auto &row : motion)
      moving.push_back(row.object);
    capture_motion_causes(causes, space, identity, moving, origins);
  }
}
} // namespace
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
                                   const react::InstanceScripts &installed, IdentityRead identity,
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
void encode_checkpoint(
    snap::Buffer &bytes, const snap::Header &header, const Objects &objects,
    const react::Properties &properties, const react::Processes &processes,
    const react::InstanceSettings &instance_settings, const react::ModelSettings &model_settings,
    const react::PlotSettings &plot_settings, const react::WorldSettings &world_settings,
    const react::AuthoredRelations &authored_relations,
    const space::PhysicalRelations &physical_relations, const lifecycle::Parts &parts,
    const react::SharedColumns &shared, const react::GivesTo &gives,
    const react::AllocationRows &allocations, const react::MeterRows &meters,
    const catalog::Models &models, const catalog::ParameterBlocks &parameters,
    const catalog::Scripts &scripts, const catalog::ModelTexts &model_text,
    const catalog::InstanceTexts &instance_text, const catalog::AuthoredTexts &authored_text,
    const perm::Players &players, const perm::Ledger &ledger, const perm::Plots &plots,
    const perm::Groups &groups, const net::Profiles &profiles, const perm::Grants &grants,
    const perm::Offers &offers, const perm::Trades &trades, const perm::OwnerLimits &limits,
    const react::ScriptStates &states, const react::Subscriptions &subscriptions,
    const react::InstanceScripts &installations, const TimerWheel &timers, const WorkQueues &queues,
    const Intents &intents, const Inputs &inputs, const elaborate::Requests &requests,
    const elaborate::GapCounts &gap_counts, const elaborate::LastGaps &last_gaps,
    const elaborate::CaptureRefs &captures, const react::RandomStates &random,
    const std::vector<space::MotionRow> &motion, const std::vector<BodyCauseRow> &causes,
    std::uint64_t next_record, const CaptureSelection *selection) {

  ZoneScopedN("snapshot serialization");
  bytes.clear();
  snap::Output out{bytes};
  snap::begin(out, header);
  const auto selected =
      selection ? snap::ObjectSelection(selection->objects) : snap::ObjectSelection{};
  const auto handles = selection ? catalog::ObjectSelection(selection->objects) : std::nullopt;
  const auto *catalog_filter = selection ? &selection->catalog : nullptr;
  const auto *permissions = selection ? &selection->permissions : nullptr;
  std::vector<PlotId> plot_ids;
  if (permissions)
    plot_ids.assign(permissions->plots.begin(), permissions->plots.end());
  {
    const auto marker = snap::begin_section(out, snap::tags[0]);
    snap::write_objects(out, objects, handles);
    snap::end_section(out, marker);
  }
  {
    const auto marker = snap::begin_section(out, snap::tags[1]);
    write_prop(out, properties, processes, instance_settings, model_settings, plot_settings,
               world_settings, selected, catalog_filter,
               selection ? std::optional<std::span<const PlotId>>(plot_ids) : std::nullopt);
    snap::end_section(out, marker);
  }
  {
    const auto marker = snap::begin_section(out, snap::tags[2]);
    write_rels(out, authored_relations, physical_relations, parts, identity(objects), handles);
    snap::end_section(out, marker);
  }
  {
    const auto marker = snap::begin_section(out, snap::tags[3]);
    react::write_shared(out, shared, gives, allocations, meters, selected);
    snap::end_section(out, marker);
  }
  {
    const auto marker = snap::begin_section(out, snap::tags[4]);
    write_text(out, models, parameters, scripts, model_text, instance_text, authored_text,
               catalog_filter, handles);
    snap::end_section(out, marker);
  }
  {
    const auto marker = snap::begin_section(out, snap::tags[5]);
    write_players(out, players, ledger, plots, groups, profiles, permissions);
    snap::end_section(out, marker);
  }
  {
    const auto marker = snap::begin_section(out, snap::tags[6]);
    perm::write_grants(out, grants, offers, trades, limits, permissions);
    snap::end_section(out, marker);
  }
  {
    const auto marker = snap::begin_section(out, snap::tags[7]);
    write_scst(out, states, subscriptions, installations, identity(objects), handles);
    snap::end_section(out, marker);
  }
  {
    const auto marker = snap::begin_section(out, snap::tags[8]);
    step::write_timers(out, timers, selected);
    snap::end_section(out, marker);
  }
  {
    const auto marker = snap::begin_section(out, snap::tags[9]);
    write_queues_section(out, queues, intents, next_record, inputs, selected, {});
    snap::end_section(out, marker);
  }
  {
    const auto marker = snap::begin_section(out, snap::tags[10]);
    write_reqs(out, requests, gap_counts, last_gaps, captures, handles);
    snap::end_section(out, marker);
  }
  {
    const auto marker = snap::begin_section(out, snap::tags[11]);
    react::write_random(out, random, selected);
    snap::end_section(out, marker);
  }
  {
    const auto marker = snap::begin_section(out, snap::tags[12]);
    space::write_motion(out, motion, selected);
    write_motion_causes(out, causes, selected);
    snap::end_section(out, marker);
  }
  snap::finish(out, bytes);
}
Decoded decode_checkpoint(
    const snap::View &view, Objects &objects, react::Properties &properties,
    react::Processes &processes, react::InstanceSettings &instance_settings,
    react::ModelSettings &model_settings, react::PlotSettings &plot_settings,
    react::WorldSettings &world_settings, react::AuthoredRelations &authored_relations,
    space::PhysicalRelations &physical_relations, lifecycle::Parts &parts,
    react::SharedColumns &shared, react::GivesTo &gives, react::AllocationRows &allocations,
    react::MeterRows &meters, catalog::Models &models, catalog::ParameterBlocks &parameters,
    catalog::Scripts &scripts, catalog::ModelTexts &model_text,
    catalog::InstanceTexts &instance_text, catalog::AuthoredTexts &authored_text,
    perm::Players &players, perm::Ledger &ledger, perm::Plots &plots, perm::Groups &groups,
    net::Profiles &profiles, perm::Grants &grants, perm::Offers &offers, perm::Trades &trades,
    perm::OwnerLimits &limits, react::ScriptStates &states, react::Subscriptions &subscriptions,
    react::InstanceScripts &installations, TimerWheel &timers, WorkQueues &queues, Intents &intents,
    Inputs &inputs, elaborate::Requests &requests, elaborate::GapCounts &gap_counts,
    elaborate::LastGaps &last_gaps, elaborate::CaptureRefs &captures, react::RandomStates &random,
    Relation &declared_parts) {
  Decoded result;
  {
    auto in = snap::section_input(view, snap::tags[0]);
    snap::read_objects(in, objects);
    snap::check_complete(in);
  }
  {
    auto in = snap::section_input(view, snap::tags[1]);
    read_prop(in, properties, processes, instance_settings, model_settings, plot_settings,
              world_settings, objects.generation.size());
    snap::check_complete(in);
  }
  {
    auto in = snap::section_input(view, snap::tags[2]);
    read_rels(in, authored_relations, physical_relations, parts, identity(objects), declared_parts);
    snap::check_complete(in);
  }
  {
    auto in = snap::section_input(view, snap::tags[3]);
    react::read_shared(in, shared, gives, allocations, meters, objects.generation.size());
    snap::check_complete(in);
  }
  {
    auto in = snap::section_input(view, snap::tags[4]);
    read_text(in, models, parameters, scripts, model_text, instance_text, authored_text);
    snap::check_complete(in);
  }
  {
    auto in = snap::section_input(view, snap::tags[5]);
    read_players(in, players, ledger, plots, groups, profiles);
    snap::check_complete(in);
  }
  {
    auto in = snap::section_input(view, snap::tags[6]);
    perm::read_grants(in, grants, offers, trades, limits, objects.generation.size());
    snap::check_complete(in);
  }
  {
    auto in = snap::section_input(view, snap::tags[7]);
    read_scst(in, states, subscriptions, installations, objects.generation.size());
    snap::check_complete(in);
  }
  {
    auto in = snap::section_input(view, snap::tags[8]);
    step::read_timers(in, timers);
    snap::check_complete(in);
  }
  {
    auto in = snap::section_input(view, snap::tags[9]);
    result.next_record = read_queues_section(in, queues, intents, inputs);
    snap::check_complete(in);
  }
  {
    auto in = snap::section_input(view, snap::tags[10]);
    read_reqs(in, requests, gap_counts, last_gaps, captures);
    snap::check_complete(in);
  }
  {
    auto in = snap::section_input(view, snap::tags[11]);
    react::read_random(in, random);
    snap::check_complete(in);
  }
  {
    auto in = snap::section_input(view, snap::tags[12]);
    space::read_motion(in, result.motion);
    read_motion_causes(in, result.causes);
    snap::check_complete(in);
  }
  return result;
}
bool save_snapshot(
    snap::Writer &writer, SnapshotProgress &progress, const snap::Header &header,
    const Objects &objects, const react::Properties &properties, const react::Processes &processes,
    const react::InstanceSettings &instance_settings, const react::ModelSettings &model_settings,
    const react::PlotSettings &plot_settings, const react::WorldSettings &world_settings,
    const react::AuthoredRelations &authored_relations,
    const space::PhysicalRelations &physical_relations, const lifecycle::Parts &parts,
    const react::SharedColumns &shared, const react::GivesTo &gives,
    const react::AllocationRows &allocations, const react::MeterRows &meters,
    const catalog::Models &models, const catalog::ParameterBlocks &parameters,
    const catalog::Scripts &scripts, const catalog::ModelTexts &model_text,
    const catalog::InstanceTexts &instance_text, const catalog::AuthoredTexts &authored_text,
    const perm::Players &players, const perm::Ledger &ledger, const perm::Plots &plots,
    const perm::Groups &groups, const net::Profiles &profiles, const perm::Grants &grants,
    const perm::Offers &offers, const perm::Trades &trades, const perm::OwnerLimits &limits,
    const react::ScriptStates &states, const react::Subscriptions &subscriptions,
    const react::InstanceScripts &installations, const TimerWheel &timers, const WorkQueues &queues,
    const Intents &intents, const Inputs &inputs, const elaborate::Requests &requests,
    const elaborate::GapCounts &gap_counts, const elaborate::LastGaps &last_gaps,
    const elaborate::CaptureRefs &captures, const react::RandomStates &random,
    const space::Space &space, const MechanicalCauses &mechanical_causes,
    std::uint64_t next_record) {
  ZoneScopedN("snapshot capture");
  auto bytes = writer.acquire();
  if (!bytes)
    return false;
  const auto started = std::chrono::steady_clock::now();
  std::vector<space::MotionRow> motion;
  std::vector<BodyCauseRow> causes;
  sample_motion(space, mechanical_causes, identity(objects), nullptr, motion, causes);
  encode_checkpoint(*bytes, header, objects, properties, processes, instance_settings,
                    model_settings, plot_settings, world_settings, authored_relations,
                    physical_relations, parts, shared, gives, allocations, meters, models,
                    parameters, scripts, model_text, instance_text, authored_text, players, ledger,
                    plots, groups, profiles, grants, offers, trades, limits, states, subscriptions,
                    installations, timers, queues, intents, inputs, requests, gap_counts, last_gaps,
                    captures, random, motion, causes, next_record, nullptr);
  progress.pause_max_ms =
      std::max(progress.pause_max_ms,
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                   .count());
  writer.submit(std::move(*bytes), header.step);
  progress.last_step = header.step;
  return true;
}
CaptureSelection capture_selection(
    const GapDraft &gap, const react::ValueRead &values, const Objects &objects,
    const lifecycle::Parts &parts, const catalog::Models &models,
    const catalog::ParameterBlocks &parameters, const react::InstanceScripts &installations,
    const react::RuleVersions &rules, const perm::Players &players, const perm::Plots &plots,
    const perm::Groups &groups, const perm::Grants &grants, const perm::Offers &offers,
    const perm::Trades &trades, const net::SessionViews &views, const net::Profiles &profiles,
    const space::Space &space, float radius_m) {
  const auto id = identity(objects);
  std::vector<Handle> seeds{gap.target};
  if (gap.requester) {
    for (const auto &[session, view] : views.rows) {
      (void)session;
      if (view.player == gap.requester)
        seeds.insert(seeds.end(), view.last_observation.begin(), view.last_observation.end());
    }
    if (gap.requester < profiles.rows.size())
      seeds.push_back(profiles.rows[gap.requester].body);
  } else {
    std::vector<space::Nearby> nearby;
    space::near(space, id, objects.position[gap.target.slot()], radius_m, {}, nearby);
    for (const auto &row : nearby)
      seeds.push_back(row.object);
  }
  std::vector<const Relation *> edges;
  for (const auto &[term, relation] : values.relations.columns) {
    (void)term;
    edges.push_back(relation);
  }
  CaptureSelection selected;
  const auto local = neighborhood(id, parents(objects), parts, edges, seeds);
  selected.objects =
      priority_neighborhood(id, parents(objects), parts, values.terms, values.shared, local);
  selected.priority_dependencies = selected.objects.size() - local.size();
  selected.catalog = catalog_selection(models, parameters, installations, id, objects.params, rules,
                                       selected.objects);
  const std::array<PlayerId, 1> actors{gap.requester};
  selected.permissions = perm::capture_authority(authority(objects), selected.objects, actors,
                                                 players, plots, groups, grants, offers, trades);
  return selected;
}
void flush_gaps(
    std::vector<GapDraft> &pending, record::Journal &journal, elaborate::CaptureWriter &writer,
    const snap::Header &header, std::size_t capture_limit, float radius_m,
    const react::ValueRead &values, const net::SessionViews &views,
    const react::RuleVersions &rules, const space::Space &space,
    const MechanicalCauses &mechanical_causes, const Objects &objects,
    const react::Properties &properties, const react::Processes &processes,
    const react::InstanceSettings &instance_settings, const react::ModelSettings &model_settings,
    const react::PlotSettings &plot_settings, const react::WorldSettings &world_settings,
    const react::AuthoredRelations &authored_relations,
    const space::PhysicalRelations &physical_relations, const lifecycle::Parts &parts,
    const react::SharedColumns &shared, const react::GivesTo &gives,
    const react::AllocationRows &allocations, const react::MeterRows &meters,
    const catalog::Models &models, const catalog::ParameterBlocks &parameters,
    const catalog::Scripts &scripts, const catalog::ModelTexts &model_text,
    const catalog::InstanceTexts &instance_text, const catalog::AuthoredTexts &authored_text,
    const perm::Players &players, const perm::Ledger &ledger, const perm::Plots &plots,
    const perm::Groups &groups, const net::Profiles &profiles, const perm::Grants &grants,
    const perm::Offers &offers, const perm::Trades &trades, const perm::OwnerLimits &limits,
    const react::ScriptStates &states, const react::Subscriptions &subscriptions,
    const react::InstanceScripts &installations, const TimerWheel &timers, const WorkQueues &queues,
    const Intents &intents, const Inputs &inputs, elaborate::Requests &requests,
    elaborate::GapCounts &gap_counts, elaborate::LastGaps &last_gaps,
    elaborate::CaptureRefs &captures, const react::RandomStates &random) {
  const auto id = identity(objects);
  for (const auto &gap : pending) {
    if (!id.valid(gap.target))
      continue;
    const elaborate::GapKey key{gap.term, id.material[gap.target.slot()]};
    const auto noted = elaborate::note_gap(
        gap_counts, last_gaps, requests,
        {gap.action, key, gap.target, gap.requester, header.step, {}, gap.origin}, capture_limit);
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
      try {
        const auto selected = capture_selection(
            gap, values, objects, parts, models, parameters, installations, rules, players, plots,
            groups, grants, offers, trades, views, profiles, space, radius_m);
        std::vector<space::MotionRow> motion;
        std::vector<BodyCauseRow> causes;
        sample_motion(space, mechanical_causes, id, &selected, motion, causes);
        snap::Buffer bytes;
        encode_checkpoint(
            bytes, header, objects, properties, processes, instance_settings, model_settings,
            plot_settings, world_settings, authored_relations, physical_relations, parts, shared,
            gives, allocations, meters, models, parameters, scripts, model_text, instance_text,
            authored_text, players, ledger, plots, groups, profiles, grants, offers, trades, limits,
            states, subscriptions, installations, timers, queues, intents, inputs, requests,
            gap_counts, last_gaps, captures, random, motion, causes, journal.next_id(), &selected);
        const auto hash = writer.submit(std::move(bytes));
        captures.rows.try_emplace(
            hash, elaborate::CaptureRef{hash, {}, elaborate::CaptureStatus::Pending, {}});
        elaborate::attach_capture(requests, last_gaps, noted, gap.requester, hash);
        event.detail["capture"] = hash;
        event.detail["objects"] = selected.objects.size();
        event.detail["priority_dependencies"] = selected.priority_dependencies;
      } catch (const std::exception &error) {
        event.detail["capture_error"] = error.what();
      }
    }
    journal.append(std::move(event), gap.origin);
  }
  pending.clear();
}
void restore_pending_work(Inputs &inputs, elaborate::CaptureRefs &captures,
                          const std::filesystem::path &catalog_root) {
  // Active fixture subprocesses belong to the old process. Do not turn
  // interrupted approvals into durable workflows or replay their actions.
  std::erase_if(inputs.rows, [](const auto &row) { return row.second.awaiting; });
  for (auto &[id, input] : inputs.rows) {
    (void)id;
    input.session = 0;
    input.sequence = 0;
  }
  for (auto &[hash, capture] : captures.rows)
    if (capture.status == elaborate::CaptureStatus::Pending) {
      capture.path = catalog_root / "captures" / (hash + ".amb.zst");
      if (std::filesystem::is_regular_file(capture.path))
        capture.status = elaborate::CaptureStatus::Ready;
      else {
        capture.status = elaborate::CaptureStatus::Failed;
        capture.error = "capture writing was interrupted before the checkpoint was restored";
      }
    }
}
} // namespace amber::step::checkpoint
