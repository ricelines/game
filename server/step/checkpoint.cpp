#include "catalog/catalog.hpp"
#include "settings.hpp"
#include "step.hpp"
#include <algorithm>
#include <set>
#include <tracy/Tracy.hpp>

namespace amber::step::checkpoint {
namespace {
void write_prop(snap::Output &out, const amber::react::State &react_state,
                const snap::ObjectSelection &selected, const CatalogSelection *catalog_filter,
                std::optional<std::span<const PlotId>> plot_ids) {
  auto &properties = react_state.properties;
  auto &processes = react_state.processes;
  auto &instances = react_state.instance_settings;
  auto &models = react_state.model_settings;
  auto &plots = react_state.plot_settings;
  auto &world = react_state.world_settings;

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
void write_text(snap::Output &out, const amber::catalog::State &catalog_state,
                const CatalogSelection *selected, std::optional<std::span<const Handle>> objects) {
  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &scripts = catalog_state.scripts;
  auto &model_text = catalog_state.model_text;
  auto &instance_text = catalog_state.instance_text;
  auto &authored_text = catalog_state.authored_text;

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
  net::write_profiles(out, profiles, selected ? &selected->objects : nullptr);
}
void read_players(snap::Input &in, perm::Players &players, perm::Ledger &ledger, perm::Plots &plots,
                  perm::Groups &groups, net::Profiles &profiles) {
  perm::read_players(in, players, ledger, plots, groups);
  net::read_profiles(in, profiles);
}
void write_scst(snap::Output &out, const react::State &react_state, IdentityRead identity,
                std::optional<std::span<const Handle>> selected) {
  react::write_script_states(out, react_state.states, react_state.subscriptions,
                             selected ? snap::ObjectSelection(*selected) : snap::ObjectSelection{});
  react::write_instances(out, react_state.installations, identity, selected);
  const std::vector<react::WorldScript> empty;
  react::write_world_scripts(out, selected ? empty : react_state.world_scripts);
}
void read_scst(snap::Input &in, react::State &react_state, std::size_t slots) {
  react::read_script_states(in, react_state.states, react_state.subscriptions);
  react::read_instances(in, react_state.installations, slots);
  react::read_world_scripts(in, react_state.world_scripts);
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
    snap::Buffer &bytes, const snap::Header &header, const amber::ObjectState &objects_state,
    const amber::react::State &react_state, const amber::space::State &space_state,
    const amber::catalog::State &catalog_state, const amber::perm::State &perm_state,
    const amber::net::State &net_state, const amber::step::State &step_state,
    const amber::elaborate::State &elaborate_state, const std::vector<space::MotionRow> &motion,
    std::uint64_t next_record, const CaptureSelection *selection) {
  auto &physical_relations = space_state.physical_relations;
  auto &profiles = net_state.profiles;

  auto &objects = objects_state.objects;

  auto &authored_relations = react_state.authored_relations;
  auto &parts = objects_state.parts;
  auto &shared = react_state.shared;
  auto &gives = react_state.gives_to;
  auto &allocations = react_state.allocations;

  auto &players = perm_state.players;
  auto &ledger = perm_state.ledger;
  auto &plots = perm_state.plots;
  auto &groups = perm_state.groups;
  auto &grants = perm_state.grants;
  auto &offers = perm_state.offers;
  auto &trades = perm_state.trades;
  auto &limits = perm_state.owner_limits;
  auto &timers = step_state.timers;
  auto &queues = step_state.queues;
  auto &intents = step_state.intents;
  auto &requests = elaborate_state.requests;
  auto &gap_counts = elaborate_state.gap_counts;
  auto &last_gaps = elaborate_state.last_gaps;
  auto &captures = elaborate_state.captures;
  auto &random = react_state.random;

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
    write_prop(out, react_state, selected, catalog_filter,
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
    react::write_shared(out, shared, gives, allocations, selected);
    snap::end_section(out, marker);
  }
  {
    const auto marker = snap::begin_section(out, snap::tags[4]);
    write_text(out, catalog_state, catalog_filter, handles);
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
    write_scst(out, react_state, identity(objects), handles);
    snap::end_section(out, marker);
  }
  {
    const auto marker = snap::begin_section(out, snap::tags[8]);
    step::write_timers(out, timers, selected);
    snap::end_section(out, marker);
  }
  {
    const auto marker = snap::begin_section(out, snap::tags[9]);
    step::write_queues(out, queues, intents, next_record, selected);
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
    snap::end_section(out, marker);
  }
  snap::finish(out, bytes);
}
Decoded decode_checkpoint(const snap::View &view, amber::ObjectState &objects_state,
                          amber::react::State &react_state, amber::space::State &space_state,
                          amber::catalog::State &catalog_state, amber::perm::State &perm_state,
                          amber::net::State &net_state, amber::step::State &step_state,
                          amber::elaborate::State &elaborate_state) {
  auto &physical_relations = space_state.physical_relations;
  auto &profiles = net_state.profiles;

  auto &objects = objects_state.objects;
  auto &properties = react_state.properties;
  auto &processes = react_state.processes;
  auto &instance_settings = react_state.instance_settings;
  auto &model_settings = react_state.model_settings;
  auto &plot_settings = react_state.plot_settings;
  auto &world_settings = react_state.world_settings;
  auto &authored_relations = react_state.authored_relations;
  auto &parts = objects_state.parts;
  auto &shared = react_state.shared;
  auto &gives = react_state.gives_to;
  auto &allocations = react_state.allocations;
  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &scripts = catalog_state.scripts;
  auto &model_text = catalog_state.model_text;
  auto &instance_text = catalog_state.instance_text;
  auto &authored_text = catalog_state.authored_text;
  auto &players = perm_state.players;
  auto &ledger = perm_state.ledger;
  auto &plots = perm_state.plots;
  auto &groups = perm_state.groups;
  auto &grants = perm_state.grants;
  auto &offers = perm_state.offers;
  auto &trades = perm_state.trades;
  auto &limits = perm_state.owner_limits;
  auto &timers = step_state.timers;
  auto &queues = step_state.queues;
  auto &intents = step_state.intents;
  auto &requests = elaborate_state.requests;
  auto &gap_counts = elaborate_state.gap_counts;
  auto &last_gaps = elaborate_state.last_gaps;
  auto &captures = elaborate_state.captures;
  auto &random = react_state.random;
  auto &declared_parts = objects_state.declared_parts;

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
    react::read_shared(in, shared, gives, allocations, objects.generation.size());
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
    perm::read_grants(in, grants, players, offers, trades, limits, objects.generation.size());
    snap::check_complete(in);
  }
  {
    auto in = snap::section_input(view, snap::tags[7]);
    read_scst(in, react_state, objects.generation.size());
    snap::check_complete(in);
  }
  {
    auto in = snap::section_input(view, snap::tags[8]);
    step::read_timers(in, timers);
    snap::check_complete(in);
  }
  {
    auto in = snap::section_input(view, snap::tags[9]);
    result.next_record = step::read_queues(in, queues, intents);
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
    snap::check_complete(in);
  }
  return result;
}
bool save_snapshot(snap::Writer &writer, const snap::Header &header,
                   const amber::ObjectState &objects_state, const amber::react::State &react_state,
                   const amber::space::State &space_state,
                   const amber::catalog::State &catalog_state, const amber::perm::State &perm_state,
                   const amber::net::State &net_state, amber::step::State &step_state,
                   const amber::elaborate::State &elaborate_state, std::uint64_t next_record) {

  auto &space = space_state.space;
  auto &progress = step_state.snapshot_progress;

  auto &objects = objects_state.objects;

  ZoneScopedN("snapshot capture");
  auto bytes = writer.acquire();
  if (!bytes)
    return false;
  const auto started = std::chrono::steady_clock::now();
  std::vector<space::MotionRow> motion;
  space::capture_motion(space, identity(objects), motion);
  encode_checkpoint(*bytes, header, objects_state, react_state, space_state, catalog_state,
                    perm_state, net_state, step_state, elaborate_state, motion, next_record,
                    nullptr);
  progress.pause_max_ms =
      std::max(progress.pause_max_ms,
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                   .count());
  writer.submit(std::move(*bytes), header.step);
  progress.last_step = header.step;
  return true;
}
CaptureSelection capture_selection(const GapDraft &gap, const amber::ObjectState &objects_state,
                                   const amber::catalog::State &catalog_state,
                                   const amber::react::State &react_state,
                                   const amber::perm::State &perm_state,
                                   const amber::net::State &net_state, const space::Space &space,
                                   float radius_m) {
  const auto values = amber::react::value_read(objects_state, catalog_state, react_state);

  auto &objects = objects_state.objects;
  auto &parts = objects_state.parts;
  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &installations = react_state.installations;
  auto &rules = react_state.rules;

  auto &views = net_state.session_views;
  auto &profiles = net_state.profiles;

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
  const std::array<PlayerId, 1> actors{gap.requester};
  selected.permissions =
      perm::capture_authority(authority(objects), selected.objects, actors, perm_state);
  for (auto bits : selected.permissions.objects.objects) {
    const Handle object{bits};
    if (std::ranges::find(selected.objects, object) == selected.objects.end())
      selected.objects.push_back(object);
  }
  selected.catalog = catalog_selection(models, parameters, installations, id, objects.params, rules,
                                       selected.objects);
  return selected;
}
void flush_gaps(amber::step::State &step_state, record::Journal &journal,
                elaborate::CaptureWriter &writer, const snap::Header &header,
                const amber::Settings &settings, const amber::net::State &net_state,
                const amber::react::State &react_state, const amber::ObjectState &objects_state,
                const amber::space::State &space_state, const amber::catalog::State &catalog_state,
                const amber::perm::State &perm_state, amber::elaborate::State &elaborate_state) {
  const auto values = amber::react::value_read(objects_state, catalog_state, react_state);

  auto &space = space_state.space;
  const auto capture_limit = settings.gap_captures;
  const auto radius_m = settings.capture_m;

  auto &pending = step_state.gaps;

  auto &objects = objects_state.objects;

  auto &requests = elaborate_state.requests;
  auto &gap_counts = elaborate_state.gap_counts;
  auto &last_gaps = elaborate_state.last_gaps;
  auto &captures = elaborate_state.captures;

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
        const auto selected = capture_selection(gap, objects_state, catalog_state, react_state,
                                                perm_state, net_state, space, radius_m);
        std::vector<space::MotionRow> motion;
        space::capture_motion(space, id, selected.objects, motion);
        snap::Buffer bytes;
        encode_checkpoint(bytes, header, objects_state, react_state, space_state, catalog_state,
                          perm_state, net_state, step_state, elaborate_state, motion,
                          journal.next_id(), &selected);
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
void restore_pending_captures(elaborate::CaptureRefs &captures,
                              const std::filesystem::path &catalog_root) {
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
