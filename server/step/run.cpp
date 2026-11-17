#include "action_private.hpp"
#include "catalog/catalog.hpp"
#include "recovery.hpp"
#include "run_private.hpp"
#include "settings.hpp"
#include "start_private.hpp"
#include <algorithm>
#include <csignal>
#include <fstream>
#include <iostream>
#include <numeric>
#include <thread>
#include <tracy/Tracy.hpp>

namespace amber::step {
namespace {
using Json = nlohmann::json;
std::array<std::uint8_t, 32> hash_bytes(const catalog::Hash &text) {
  std::array<std::uint8_t, 32> out{};
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = static_cast<std::uint8_t>(std::stoul(text.substr(i * 2, 2), nullptr, 16));
  return out;
}
catalog::Hash hash_text(const std::array<std::uint8_t, 32> &bytes) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out(64, '0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const auto n = static_cast<unsigned>(bytes[i]);
    out[i * 2] = hex[n >> 4];
    out[i * 2 + 1] = hex[n & 15];
  }
  return out;
}
} // namespace
int run(const RunOptions &launch, const volatile std::sig_atomic_t &stopping) {
  auto restore_path = launch.restore;
  if (restore_path.empty())
    restore_path = recovery::latest_snapshot(launch.data);
  std::optional<fixture::Case> fixture_case;
  if (!launch.fixture_case.empty()) {
    if (restore_path.empty() || launch.network || launch.realtime || launch.report.empty())
      throw std::invalid_argument("fixture child requires restore, no network, unpaced and report");
    fixture_case = fixture::read_case(launch.fixture_case);
  }
  auto settings = read_settings(launch.config, launch.profile);
  const auto hz = settings.clock.step_hz;
  ObjectState objects_state;
  catalog::State catalog_state;
  react::State react_state(settings.scripts);
  perm::State perm_state;
  elaborate::State elaborate_state;
  net::State net_state({"127.0.0.1", launch.port, launch.client, launch.assets, settings.network,
                        settings.control_max});
  space::State space_state(settings.space, settings.light_cell_m);
  step::State step_state;

  const auto plan = start::read_plan(launch.plan);
  settings.land.way_group = plan.at("land").at("way_group").get<GroupId>();
  std::filesystem::create_directories(launch.data);
  auto &terms = react_state.terms;
  auto &materials = react_state.materials;
  auto &mechanical = catalog_state.mechanical;
  snap::Buffer restored_bytes;
  std::optional<snap::View> restored;
  const auto catalog_root =
      launch.catalog.empty()
          ? (restore_path.empty() ? launch.data : restore_path.parent_path().parent_path())
          : launch.catalog;
  auto &registry = catalog_state.registry;
  if (restore_path.empty()) {
    registry = start::load_registry(plan, launch.data / "registry", terms, materials, mechanical);
  } else {
    restored_bytes = snap::read_file(restore_path);
    const std::array<std::string_view, 1> versions{runtime_version};
    restored = snap::parse(restored_bytes, versions);
    registry = catalog::read_registry(catalog_root / "registry",
                                      hash_text(restored->header.registry_hash));
    catalog::load_terms(registry, terms);
    catalog::load_materials(registry, terms, materials);
    mechanical.values = registry.data.at("mechanical");
  }
  if (fixture_case)
    fixture::extend_vocabulary(terms, materials, mechanical, catalog_root / "registry",
                               launch.candidate_registry);

  auto &native_declarations = react_state.native_declarations;
  editing::declare_editing(terms, native_declarations);
  declare_commerce(terms, native_declarations);
  declare_admin(terms, native_declarations);
  declare_elaboration(terms, native_declarations);
  declare_construction_actions(terms, native_declarations);
  declare_model_edit(terms, native_declarations);
  declare_physical_actions(terms, native_declarations);
  const auto role = [&](const char *group, const char *name) {
    return mechanical.values.at(group).at(name).get<TermId>();
  };
  const std::array<TermId, 4> relation_terms{
      role("relations", "contains"), role("relations", "supports"),
      role("relations", "attached_to"), role("relations", "in_zone")};
  const auto registry_directory = launch.data / "registry";
  auto &objects = objects_state.objects;
  const auto ids = identity(objects);
  const auto poses = transforms(objects);
  const auto auth = authority(std::as_const(objects));
  const auto parent = parents(objects);
  auto &parts = objects_state.parts;
  auto &declared_parts = objects_state.declared_parts;
  auto &assets = catalog_state.assets;
  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;
  auto &parameters = catalog_state.parameters;
  auto &model_text = catalog_state.model_text;
  auto &instance_text = catalog_state.instance_text;
  auto &baked_measurements = catalog_state.baked_measurements;
  auto &runtime_measurements = react_state.runtime_measurements;

  auto &processes = react_state.processes;

  auto &authored_relations = react_state.authored_relations;
  auto &relations = react_state.relations;
  auto &physical_relations = space_state.physical_relations;
  start::bind_relation_reads(relations, authored_relations, terms, physical_relations,
                             relation_terms);
  const auto values = react::value_read(objects_state, catalog_state, react_state);
  auto &states = react_state.states;
  auto &triggers = react_state.triggers;
  auto &rules = react_state.rules;
  auto &subscriptions = react_state.subscriptions;

  auto &players = perm_state.players;
  auto &groups = perm_state.groups;
  auto &plots = perm_state.plots;

  auto &travel_access = step_state.travel_access;
  auto &ledger = perm_state.ledger;

  auto &profiles = net_state.profiles;
  auto &queues = step_state.queues;
  auto &timers = step_state.timers;
  auto &intents = step_state.intents;
  auto &inputs = step_state.inputs;
  auto &requests = elaborate_state.requests;

  auto &captures = elaborate_state.captures;
  auto &fixtures = elaborate_state.fixtures;

  auto &clock = step_state.clock;
  clock.hz = hz;
  auto &space = space_state.space;
  auto &geometry = step_state.geometry;
  auto &relation_changes = space_state.relation_changes;
  auto &hearing = space_state.hearing;

  auto &delivery = step_state.delivery;
  auto &lights = space_state.lights;
  auto &light_subscriptions = space_state.light_subscriptions;
  auto &zones = space_state.zones;
  auto &active_views = space_state.active_views;
  auto &replication = net_state.replication;
  auto &masses = step_state.masses;
  masses =
      AssemblyMasses{{role("measurements", "mass"), role("measurements", "assembly_mass")}, {}, {}};
  auto &session_views = net_state.session_views;
  auto &illumination_sources = step_state.illumination_sources;
  auto &navigation_requests = step_state.navigation_requests;
  auto &vm = react_state.vm;

  react::bind_values(vm, values, react_state);
  react::bind_events(vm, terms);
  bind_metadata(objects_state, catalog_state, react_state, perm_state,
                settings.land.port_spacing_m);
  bind_physical(vm, objects_state, step_state, perm_state, settings.part_batch_max, space_state,
                catalog_state);
  if (!restored)
    registry = catalog::read_registry(
        registry_directory,
        catalog::save_registry(registry_directory, terms, materials, {}, mechanical));
  start::Symbols symbols;
  std::vector<Handle> initial;
  react::DeferredEffects setup;
  std::uint64_t next_record = 1;
  checkpoint::Decoded decoded{};
  if (restored) {
    decoded = checkpoint::decode_checkpoint(*restored, objects_state, react_state, space_state,
                                            catalog_state, perm_state, net_state, step_state,
                                            elaborate_state);
    next_record = decoded.next_record;
    for (const auto &plot : plots.rows)
      if (plot.id)
        space::ensure_terrain(space_state, plot.bounds);

    clock.step = restored->header.step;
    checkpoint::restore_pending_work(inputs, captures, catalog_root);
    catalog::restore_scripts(scripts, vm, rules, registry, terms, settings.pins,
                             catalog_root / "scripts", launch.data / "scripts");
    catalog::restore_assets(assets, models, parameters, terms, settings.pins, launch.assets);
    start::bind_relation_reads(relations, authored_relations, terms, physical_relations,
                               relation_terms);
    for (std::uint32_t slot = 1; slot < ids.model.size(); ++slot)
      if (ids.valid(ids.at(slot)))
        initial.push_back(ids.at(slot));
  } else {
    start::install_catalog(plan, react_state, registry.hash, settings.pins, settings.scripts,
                           launch.assets, launch.data / "scripts", catalog_state, symbols);
    start::install_land(plan, perm_state, space_state, settings.land);
    initial =
        start::install_objects(plan, symbols, catalog_state, terms, objects_state, clock.step);
    start::install_properties(plan, symbols, react_state, clock.step, hz);
    start::install_relations(plan, symbols, terms, authored_relations, setup);
    start::install_script_state(plan, symbols, states);
    start::install_profiles(plan, symbols, profiles, players, std::filesystem::current_path());
    perm::update_plots(objects, plots, initial, true);
    start::install_money(plan, symbols, ids, auth, players, groups, ledger);
    start::install_grants(plan, symbols, objects_state, perm_state, catalog_state, react_state,
                          clock);
    start::provision_body_grants(plan, symbols, catalog_state, objects_state, perm_state, profiles,
                                 clock.step, hz);
  }

  react::RuleChange fixture_rule_change;
  if (fixture_case)
    fixture_rule_change = fixture::install_candidate(
        scripts, react_state, settings.pins, catalog_root / "scripts", launch.data / "scripts",
        launch.candidate, launch.candidate_registry, launch.candidate_author,
        launch.candidate_active, clock);
  for (const auto &model : models.rows)
    if (model.id) {
      catalog::project_measurements(
          baked_measurements, model.id, 0, assets.rows.at(model.asset),
          {role("measurements", "volume"), role("measurements", "capacity")});
    }
  for (const auto &block : parameters.rows)
    if (block.id)
      catalog::project_measurements(
          baked_measurements, block.model, block.id, assets.rows.at(block.asset),
          {role("measurements", "volume"), role("measurements", "capacity")});
  registry = catalog::read_registry(
      launch.data / "registry",
      catalog::save_registry(launch.data / "registry", terms, materials, rules, mechanical));
  for (const auto &[hash, entry] : assets.rows) {
    (void)hash;
    import_geometry_asset(geometry, space, entry, launch.assets);
  }
  const PhysicsDefaults physics_defaults{settings.default_friction, settings.default_restitution};
  const PhysicalRelationTerms physical_terms{relation_terms[0], relation_terms[1],
                                             relation_terms[2], relation_terms[3]};
  auto visible =
      initialize_physics(step_state, catalog_state, react_state, objects_state, players,
                         space_state, initial, decoded, physics_defaults, restored.has_value());
  std::vector<PlayerId> seed_authors;
  for (const auto &row : plan.at("objects"))
    seed_authors.push_back(row.at("maker").get<PlayerId>());
  std::sort(seed_authors.begin(), seed_authors.end());
  seed_authors.erase(std::unique(seed_authors.begin(), seed_authors.end()), seed_authors.end());
  record::Journal journal(settings.authors_max, seed_authors);
  journal.restore_next_id(next_record);
  recovery::rotate_record(launch.data / "events.jsonl.zst");
  record::Writer writer(
      launch.data / "events.jsonl.zst",
      {{"spec", 1},
       {"run", launch.data.filename().string()},
       {"world", catalog::sha256(plan.dump())},
       {"started",
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()},
       {"runtime", runtime_version},
       {"step_hz", hz},
       {"registry_hash", registry.hash},
       {"resumed_step", clock.step},
       {"seed_actors",
        [&] {
          Json result = Json::array();
          for (auto author : seed_authors)
            result.push_back("p:" + std::to_string(author));
          return result;
        }()}},
      settings.record_queue_batches, settings.record_zstd);
  if (!restored)
    start::record_seed(journal, terms, models, scripts, initial, ids, auth, attribution(objects));

  react::initialize_reactions(catalog_state, react_state, values, initial, clock.step, hz,
                              restored.has_value(), setup);
  invalidate_mass(masses, ids, declared_parts, initial);
  refresh_mass(masses, values, declared_parts, runtime_measurements, clock.step, hz, {}, setup);
  sync_illuminance_reads(light_subscriptions, subscriptions, ids, poses,
                         role("measurements", "illuminance"), initial);
  register_illumination(illumination_sources, space_state, poses, catalog_state, values, visible,
                        settings.light_source_radius_m, clock);
  for (auto object : visible)
    net::register_replication(replication, object, ids, poses, models, parameters, assets, values);
  if (fixture_case)
    fixture::initialize_case(*fixture_case, react_state, values, scripts, fixture_rule_change,
                             queues, clock, setup);
  sample_illuminance(light_subscriptions, space, ids, lights, zones, runtime_measurements,
                     outdoor_lux(react_state), setup);
  publish_effects(std::move(setup), {}, clock.step, queues, timers, journal,
                  settings.event_queue_max);
  if (!restored)
    start::queue_bootstrap(plan, symbols, terms, queues, settings.event_queue_max);
  auto &server = net_state.server;
  auto &sessions = net_state.sessions;
  elaborate::FixtureWorker fixture_worker(std::filesystem::canonical("/proc/self/exe"),
                                          launch.data / "fixtures", settings.fixture_workers,
                                          launch.config, launch.profile);
  step::authoring::Worker authoring({launch.assets, settings.pins, settings.scripts});
  auto &mutated = step_state.mutated;
  auto &gaps = step_state.gaps;
  auto &inspection_terms = react_state.inspection_terms;
  for (const auto &term : terms.rows)
    if (term.id && term.kind == react::Kind::Quantity)
      inspection_terms.push_back(term.id);
  const net::MaterialAppearanceRoles appearance{
      role("appearance", "red"),   role("appearance", "green"),    role("appearance", "blue"),
      role("appearance", "alpha"), role("appearance", "metallic"), role("appearance", "roughness")};
  auto &motion = space_state.motion;
  auto &previous_positions = space_state.previous_positions;
  for (auto object : initial)
    previous_positions.set(object.slot(), std::pair{object, poses.position[object.slot()]});
  auto &feed = net_state.feed;
  elaborate::CaptureWriter capture_writer(launch.data / "captures", settings.snapshot_zstd);
  snap::Writer snapshot_writer(launch.data / "snapshots", settings.snapshot_keep,
                               settings.snapshot_zstd);
  auto &snapshot_progress = step_state.snapshot_progress;
  const auto snapshot_every = static_cast<Step>(std::llround(settings.snapshot_seconds * hz));
  if (launch.network) {
    const auto error = server.start();
    if (!error.empty())
      throw std::runtime_error(error);
  }
  std::cout << Json{{"status", "running"},
                    {"objects", initial.size()},
                    {"port", launch.network ? Json(launch.port) : Json(nullptr)},
                    {"step_hz", hz}}
                   .dump()
            << std::endl;
  PhaseCosts costs(settings.clock.average_steps);
  const auto started = Monotonic::now();
  const auto final_step = fixture_case   ? clock.step + fixture::steps(*fixture_case)
                          : launch.steps ? clock.step + *launch.steps
                                         : std::numeric_limits<Step>::max();

  std::optional<NativeCall> fixture_action;
  if (fixture_case)
    fixture_action = fixture::prepare_action(*fixture_case, terms, native_declarations, ids, auth,
                                             profiles, clock.step, hz);
  std::deque<net::Input> incoming;
  std::deque<step::authoring::Completion> authored;
  std::deque<elaborate::FixtureResult> fixture_results;
  std::size_t script_errors = 0, phase_cursor = 0;
  std::vector<double> phase_ms;
  constexpr std::size_t phase_samples_max = 4096;
  while (!stopping && (fixture_action || clock.step < final_step)) {
    FrameMarkStart("step work");
    for (const auto &completed : capture_writer.take())
      elaborate::complete_capture(captures, completed);
    const bool fixture_action_phase = fixture_action.has_value();
    const auto frame_started = Monotonic::now();
    auto frame = fixture_action_phase
                     ? Frame{clock.step,
                             frame_started,
                             Monotonic::time_point::max(),
                             Monotonic::time_point::max(),
                             0,
                             0}
                     : begin_frame(clock, settings.clock, costs, queues, timers, {}, frame_started);
    advance_permission_deadlines(perm_state, travel_access, sessions, journal, clock.step, hz,
                                 settings.grant_reminder_seconds, settings.deadlines_per_step,
                                 settings.mailbox_max);
    expire_sale_offers(perm_state, ids, permission_flags(objects), session_views, sessions, journal,
                       clock.step, settings.deadlines_per_step, settings.mailbox_max);

    for (auto &result : fixture_worker.take())
      fixture_results.push_back(std::move(result));

    for (auto &completion : authoring.poll())
      authored.push_back(std::move(completion));

    std::vector<net::Input> network_inputs;
    if (launch.network)
      server.drain(network_inputs);
    for (auto &input : network_inputs)
      if (const auto *notice = std::get_if<net::SendOverflow>(&input))
        record_send_overflow(journal, *notice, clock.step);
      else
        incoming.push_back(std::move(input));
    std::size_t admitted_network = 0;
    while (!incoming.empty() &&
           ((!admitted_network && !frame.foreground) || admits(frame, Monotonic::now()))) {
      ++admitted_network;
      sessions.accept(incoming.front());
      incoming.pop_front();
      std::vector<net::Invocation> ready;
      std::vector<net::SessionNotice> notices;
      std::vector<net::SessionOutput> outputs;
      sessions.drain(ready, notices, outputs);
      for (const auto &notice : notices)
        if (const auto play =
                connection_notice(notice, net_state, space_state, delivery, catalog_state,
                                  react_state, appearance, objects_state, clock, settings)) {
          auto delivered =
              deliver_mailbox(players, sessions, objects_state, space, play->principal.player,
                              play->principal.body, clock.step);
          change_custody(objects_state, space_state, relation_terms, plots, illumination_sources,
                         delivered);
          refresh_object_plots(space, objects_state, plots, react_state, {}, delivered);
          invalidate_mass(masses, ids, declared_parts, delivered.changed_objects);
          refresh_mass(masses, values, declared_parts, runtime_measurements, clock.step, hz, {},
                       delivered.effects);
          project_visibility(illumination_sources, space_state, replication, poses, catalog_state,
                             values, delivered, settings.light_source_radius_m, clock);
          publish_object_changes(sessions, session_views, ids, auth, parent, model_text,
                                 instance_text, delivered.changed_objects);
          amber::Origin delivery_origin;
          for (auto &record : delivered.records)
            delivery_origin = journal.append(std::move(record));
          reconcile_effects(values, react_state, step_state, declared_parts, scripts, clock,
                            delivered.effects, delivery_origin);
          publish_effects(std::move(delivered.effects), delivery_origin, clock.step, queues, timers,
                          journal, settings.event_queue_max);
        }
      for (auto &request : ready)
        if (!queue_connection_mutation(inputs, queues, request))
          answer_query(net_state, space_state, journal, catalog_state, react_state, objects_state,
                       step_state, perm_state, settings, request);
      send_connection_outputs(outputs, server, net_state, objects_state, catalog_state);
    }

    std::deque<ExecutionFrame> execution;
    if (fixture_action) {
      fixture_action->origin = {};
      fixture_action->deadline = RunDeadline::after(settings.scripts.handler_ms);
      execution.emplace_back(begin_action(catalog_state, react_state, profiles, perm_state,
                                          objects_state, space_state, intents, journal,
                                          settings.perception.reach_m, std::move(*fixture_action)));
      fixture_action.reset();
    }
    for (;;) {
      if (execution.empty()) {
        if (fixture_action_phase)
          break;
        auto admitted = next_work(queues, frame, fixture_results, authored, Monotonic::now());
        if (!admitted)
          break;
        std::vector<PlannedInvocation> plans;
        if (auto *item = std::get_if<ForegroundItem>(&*admitted)) {
          if (auto *input_work = std::get_if<InputWork>(&item->work)) {
            const auto id = input_work->id;
            const auto found = inputs.rows.find(id);
            if (found == inputs.rows.end() || found->second.awaiting) {
              --frame.foreground;
              continue;
            }
            auto &input = found->second;
            auto prepared = prepare_input(authoring, react_state, registry, step_state, journal,
                                          input, clock.step, hz);
            if (auto *call = std::get_if<NativeCall>(&prepared)) {
              call->origin = input.origin;
              call->deadline = RunDeadline::after(settings.scripts.handler_ms);
              execution.emplace_back(begin_action(catalog_state, react_state, profiles, perm_state,
                                                  objects_state, space_state, intents, journal,
                                                  settings.perception.reach_m, std::move(*call)));
            } else {
              auto reply = std::move(std::get<InputReply>(prepared));
              if (!reply.deferred)
                deliver_input(inputs, sessions, hearing, settings.perception.events_per_response,
                              id, {std::move(reply.fields), reply.origin.value_or(input.origin)});
            }
          } else {
            auto prepared = prepare_reactive(react_state, values, scripts, step_state,
                                             std::move(*item), clock.step, hz);
            if (!prepared) {
              --frame.foreground;
              continue;
            }
            plans = std::move(*prepared);
          }

        } else if (auto *result = std::get_if<elaborate::FixtureResult>(&*admitted)) {
          const auto pending = fixtures.rows.find(result->job);
          if (pending == fixtures.rows.end()) {
            --frame.foreground;
            continue;
          }
          const auto actor = pending->second.actor;
          const bool elaborator =
              actor < profiles.rows.size() &&
              std::find(profiles.rows[actor].roles.begin(), profiles.rows[actor].roles.end(),
                        "elaborator") != profiles.rows[actor].roles.end();
          auto completed = complete_fixture(elaborate_state, step_state, scripts, react_state,
                                            values, {actor, elaborator}, *result, clock);
          if (!completed.changed_scripts.empty()) {
            registry = catalog::read_registry(
                registry_directory,
                catalog::save_registry(registry_directory, terms, materials, rules, mechanical));
            for (auto &[session, row] : session_views.rows) {
              (void)session;
              row.pending.ready = false;
            }
          }
          reconcile_effects(values, react_state, step_state, declared_parts, scripts, clock,
                            completed.effects, completed.origin);
          publish_effects(std::move(completed.effects), completed.origin, clock.step, queues,
                          timers, journal, settings.event_queue_max);
          if (completed.notification) {
            const auto &closed = *completed.notification;
            notify(players, sessions, settings.mailbox_max, closed.requester,
                   {{"kind", closed.kind},
                    {"request", "request:" + std::to_string(closed.request)},
                    {"text", closed.text}});
            const auto target = requests.rows.at(closed.request).target;
            net::invalidate(session_views, std::span{&target, 1});
          }
          if (completed.input && completed.input->session)
            sessions.complete(completed.input->session, completed.input->sequence,
                              std::move(completed.input->fields));
          continue;
        } else if (auto *completion = std::get_if<step::authoring::Completion>(&*admitted)) {
          const auto found = inputs.rows.find(completion->input_id);
          if (found == inputs.rows.end()) {
            --frame.foreground;
            continue;
          }
          const auto input = found->second;
          auto [installed, origin] = complete_authoring(
              catalog_state, react_state, step_state, space, journal, input, std::move(*completion),
              !input.session || sessions.principal(input.session).has_value(), launch.assets,
              launch.data / "scripts");
          for (auto model : installed.changed_models)
            react::project_model(triggers, models, scripts, terms, model);
          publish_catalog_changes(sessions, session_views, models, model_text, terms, materials,
                                  appearance, outdoor_lux(react_state), installed);
          deliver_input(inputs, sessions, hearing, settings.perception.events_per_response,
                        input.id, {std::move(installed.reply), origin});
          continue;
        } else {
          auto &background = std::get<react::BackgroundItem>(*admitted);
          if (!react::catch_up_valid(background, rules, values, clock.step, hz)) {
            --frame.background;
            continue;
          }
          auto planned = plan_background(react_state, values, scripts, background, clock.step, hz);
          for (auto &dirty : planned.changes.dirty)
            enqueue_watch(queues, std::move(dirty));
          for (auto &timer : planned.changes.timers)
            schedule(timers, std::move(timer));
          plans = std::move(planned.invocations);
        }
        // The deque retains active Invocation addresses across nested calls.
        for (auto plan = plans.rbegin(); plan != plans.rend(); ++plan) {
          // Admission uses the frame estimate; a running handler gets its own
          // protective limit. Nested actions inherit that handler's deadline.
          plan->input.deadline = {};
          execution.emplace_back(std::in_place_type<HandlerRun>);
          std::get<HandlerRun>(execution.back()).plan = std::move(*plan);
        }
        if (execution.empty())
          continue;
      }
      auto *selected =
          advance_execution(execution, react_state, objects_state, catalog_state, journal);
      if (!selected)
        continue;
      if (auto *action = std::get_if<ActionRun>(selected)) {
        if (!action->outcome) {
          if (action->resolution.route == react::Route::Native) {
            NativeCompletion completed;
            std::span<const std::string> roles;
            if (const auto *player = std::get_if<PlayerId>(&action->call.request.caller);
                player && *player < profiles.rows.size())
              roles = profiles.rows[*player].roles;
            const bool architect = std::ranges::find(roles, "architect") != roles.end();
            try {
              completed = std::visit(
                  [&](auto operation) {
                    using Operation = decltype(operation);
                    if constexpr (std::is_same_v<Operation, react::MechanicAction>) {
                      return native_mechanics(objects_state, react_state, perm_state, space_state,
                                              physical_terms, settings.perception.reach_m,
                                              operation, action->call);
                    } else if constexpr (std::is_same_v<Operation, react::ConstructionAction>) {
                      return native_construction(objects_state, catalog_state, react_state,
                                                 perm_state, geometry, space_state,
                                                 physics_defaults, operation, action->call);
                    } else if constexpr (std::is_same_v<Operation, react::ExchangeAction>) {
                      return native_exchange(objects_state, perm_state, space_state, delivery,
                                             settings, operation, action->call,
                                             react_state.terms.by_name.at("base.plot_changed"));
                    } else if constexpr (std::is_same_v<Operation, react::PermissionAction>) {
                      return native_permissions(catalog_state, react_state, perm_state,
                                                objects_state, settings.land, architect, operation,
                                                action->call);
                    } else if constexpr (std::is_same_v<Operation, react::ScriptAction>) {
                      return native_scripts(catalog_state, react_state, perm_state, objects_state,
                                            role("events", "start"), operation, action->call);
                    } else if constexpr (std::is_same_v<Operation, react::ValueAction>) {
                      return native_values(players, models, values, react_state, relation_terms,
                                           roles, operation, action->call);
                    } else if constexpr (std::is_same_v<Operation, react::WritingAction>) {
                      return native_writing(catalog_state, react_state, players, ids, operation,
                                            action->call);
                    } else if constexpr (std::is_same_v<Operation, react::RegistryAction>) {
                      return native_registry(catalog_state, react_state, registry_directory, values,
                                             geometry, space_state, physics_defaults, roles,
                                             operation, action->call);
                    } else if constexpr (std::is_same_v<Operation, react::ElaborationAction>) {
                      return native_elaboration(catalog_state, values, react_state, net_state,
                                                queues, elaborate_state, fixture_worker,
                                                settings.fixture_limits, operation, action->call);
                    }
                  },
                  action->resolution.native_operation);
            } catch (const std::exception &error) {
              completed.result = record::Result::Denied;
              completed.reply = {
                  {"ok", false},
                  {"error", "bad_input"},
                  {"problem", error.what()},
                  {"signature", react::action_signature(terms, native_declarations,
                                                        action->call.request.action)}};
            }
            change_custody(objects_state, space_state, relation_terms, plots, illumination_sources,
                           completed);
            discard_script_work(queues, timers, completed.hidden_objects);
            auto &removed = completed.removed_objects;
            std::sort(removed.begin(), removed.end());
            removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
            if (!removed.empty()) {
              // Every owner sees the old identities until all cleanup is complete.
              retire_commerce(perm_state, objects_state, removed, clock.step, completed);
              react::retire_reactions(models, values, react_state, removed, clock.step, hz,
                                      action->call.origin, completed.effects);
              invalidate_mass(masses, ids, declared_parts, removed);
              retire_illuminance_reads(light_subscriptions, runtime_measurements,
                                       role("measurements", "illuminance"), removed);
              retire_illumination(illumination_sources, lights, zones, light_subscriptions,
                                  active_views, space, removed);
              retire_physics(space, objects_state, physical_relations, step_state, removed,
                             relation_terms, action->call.origin, completed);
              retire_publication(replication, session_views, instance_text, removed);
              for (auto object : removed)
                retire_mass(masses, runtime_measurements, object);
              std::erase_if(runtime_measurements.values, [&](const auto &row) {
                return std::binary_search(removed.begin(), removed.end(), row.first.subject);
              });
              lifecycle::release_rows(objects, parts, declared_parts, removed);
            }
            editing::rebind_scripts(catalog_state, react_state, values, perm_state,
                                    role("events", "start"), clock, action->call.origin, completed);
            invalidate_execution(execution, ids, states);
            refresh_object_plots(space, objects_state, plots, react_state, action->call.origin,
                                 completed);
            for (auto object : completed.changed_objects)
              react::reconcile_object_priorities(values, react_state, object, completed.effects,
                                                 clock.step, hz);
            invalidate_mass(masses, ids, declared_parts, completed.changed_objects);
            refresh_mass(masses, values, declared_parts, runtime_measurements, clock.step, hz,
                         action->call.origin, completed.effects);
            sync_illuminance_reads(light_subscriptions, subscriptions, ids, poses,
                                   role("measurements", "illuminance"), completed.changed_objects);
            move_illuminance_reads(light_subscriptions, poses, role("measurements", "illuminance"),
                                   completed.changed_objects);
            project_visibility(illumination_sources, space_state, replication, poses, catalog_state,
                               values, completed, settings.light_source_radius_m, clock);
            if (completed.world_ambient_changed)
              invalidate_world_illumination(space_state);
            publish_catalog_changes(sessions, session_views, models, model_text, terms, materials,
                                    appearance, outdoor_lux(react_state), completed);
            publish_object_changes(sessions, session_views, ids, auth, parent, model_text,
                                   instance_text, completed.changed_objects);
            for (const auto &[recipient, notice] : completed.notices)
              notify(players, sessions, settings.mailbox_max, recipient, notice);
            for (auto ancestor = execution.rbegin(); ancestor != execution.rend(); ++ancestor)
              if (const auto *initiator = std::get_if<ActionRun>(&*ancestor))
                if (const auto *player = std::get_if<PlayerId>(&initiator->call.request.caller)) {
                  // A notice uses the real initiating player, never installation-owner authority.
                  queue_navigation(navigation_requests, space, ids, poses,
                                   completed.changed_objects, *player, initiator->call.body);
                  break;
                }
            for (auto term : completed.changed_terms)
              if (terms.rows[term].kind == react::Kind::Quantity &&
                  std::ranges::find(inspection_terms, term) == inspection_terms.end())
                inspection_terms.push_back(term);
            for (auto object : completed.created_objects)
              completed.effects.ordered.emplace_back(
                  react::EventWork{object, {}, role("events", "start"), {}, action->call.origin});
            if (const auto *operation =
                    std::get_if<react::ConstructionAction>(&action->resolution.native_operation);
                operation && *operation == react::ConstructionAction::PlaceExisting &&
                completed.changed)
              for (auto object : completed.visible_objects)
                completed.effects.ordered.emplace_back(
                    react::EventWork{object, {}, role("events", "start"), {}, action->call.origin});
            const auto first_changed = mutated.size();
            reconcile_effects(values, react_state, step_state, declared_parts, scripts, clock,
                              completed.effects, action->call.origin);
            publish_value_changes(replication, session_views, processes,
                                  std::span(mutated).subspan(first_changed));
            action->changed = completed.changed || completed.effects.changed;
            action->created = completed.created_objects;
            action->outcome = finish_native(*action, completed, ids, attribution(objects), models,
                                            scripts, journal);
            for (auto &record : completed.records)
              journal.append(std::move(record), action->outcome->origin);
            publish_effects(std::move(completed.effects), action->outcome->origin, clock.step,
                            queues, timers, journal, settings.event_queue_max);
          } else
            action->outcome = finish_action(*action, intents, space, ids, poses, journal, gaps);
        }
        if (auto completed = return_action(execution, vm, ids, declared_parts))
          deliver_input(inputs, sessions, hearing, settings.perception.events_per_response,
                        completed->first.input, std::move(completed->second));
        continue;
      }
      auto &handler = std::get<HandlerRun>(*selected);
      std::erase_if(handler.completed.effects.ordered, [&](const react::Effect &effect) {
        const auto *timer = std::get_if<react::Timer>(&effect);
        return timer && ids.valid(timer->event.receiver) &&
               (ids.flags[timer->event.receiver.slot()] & Held) &&
               !objects.plot[timer->event.receiver.slot()];
      });
      const auto first_changed = mutated.size();
      reconcile_effects(values, react_state, step_state, declared_parts, scripts, clock,
                        handler.completed.effects, handler.completed.origin);
      publish_value_changes(replication, session_views, processes,
                            std::span(mutated).subspan(first_changed));
      if (auto call = finish_handler_segment(execution, journal, queues, timers, clock.step, hz,
                                             settings.event_queue_max)) {
        const auto &waiting = std::get<HandlerRun>(execution.back());
        call->origin = waiting.completed.origin;
        call->deadline = waiting.invocation.deadline;
        execution.emplace_back(begin_action(catalog_state, react_state, profiles, perm_state,
                                            objects_state, space_state, intents, journal,
                                            settings.perception.reach_m, std::move(*call)));
      }
    }
    if (fixture_action_phase) {
      FrameMarkEnd("step work");
      continue;
    }
    const snap::Header header{runtime_version, clock.step, hash_bytes(registry.hash)};
    checkpoint::flush_gaps(step_state, journal, capture_writer, header, settings, net_state,
                           react_state, objects_state, space_state, catalog_state, perm_state,
                           elaborate_state);
    const auto physics_start = Monotonic::now();
    const auto installed_tiles = space::install_ready_navigation(space);
    for (auto &notice : publish_navigation(navigation_requests, space, ids, poses, installed_tiles))
      sessions.event_for_player(notice.author, std::move(notice.event));
    for (const auto &error : installed_tiles.errors)
      std::cerr << "navigation: " << error << '\n';
    auto physics = advance_motion(space, objects_state, physical_relations, step_state, perm_state,
                                  clock, settings.goal_tolerance_m, settings.perception.reach_m);
    const auto crossed_plots = perm::update_plots(objects, plots, physics.moved, true);
    for (const auto object : crossed_plots) {
      const auto origin = move_origin(intents, object);
      react::DeferredEffects effects;
      for (const auto &term : terms.rows)
        if (term.source == react::Source::Setting)
          effects.ordered.emplace_back(react::WatchWork{{object, term.id}, origin});
      reconcile_effects(values, react_state, step_state, declared_parts, scripts, clock, effects,
                        origin);
      publish_effects(std::move(effects), origin, clock.step, queues, timers, journal,
                      settings.event_queue_max);
    }
    physics.relations.insert(physics.relations.end(), relation_changes.begin(),
                             relation_changes.end());
    relation_changes.clear();
    publish_physical(step_state, journal,
                     {role("events", "contact"),
                      {role("relations", "contains"), role("relations", "supports"),
                       role("relations", "attached_to"), role("relations", "in_zone")}},
                     settings.event_queue_max, physics, clock);
    finish_motion(step_state, physics.intents);
    publish_intent_endings(step_state, terms, role("events", "arrived"), role("events", "blocked"),
                           role("events", "cancelled"), objects_state, sessions, journal,
                           clock.step, settings.event_queue_max);
    sample_motion(motion, previous_positions, physics.moved, space, ids, poses, hz);

    for (auto object : physics.moved)
      net::moved(replication, object, poses);
    react::DeferredEffects light_effects;
    update_frame_illumination(step_state, space_state, poses, values, react_state, physics.moved,
                              physics.dirty_bounds, role("measurements", "illuminance"),
                              light_effects);
    reconcile_effects(values, react_state, step_state, declared_parts, scripts, clock,
                      light_effects, {});
    publish_effects(std::move(light_effects), {}, clock.step, queues, timers, journal,
                    settings.event_queue_max);
    publish_value_changes(replication, session_views, processes, mutated);
    mutated.clear();
    refresh_connections(net_state, space_state, step_state, catalog_state, react_state,
                        objects_state, settings.perception, clock);
    if (launch.network && clock.step % settings.repl_every == 0)
      server.publish(net::publication(replication, ids, poses, values, clock.step, hz));
    flush_connections(net_state, server, step_state, objects_state, catalog_state);
    record_frame(journal, writer, feed, ids, poses, script_errors);
    if ((snapshot_every && clock.step % snapshot_every == 0) ||
        (launch.snapshot_at_end && clock.step == final_step)) {
      if (!checkpoint::save_snapshot(
              snapshot_writer, snap::Header{runtime_version, clock.step, hash_bytes(registry.hash)},
              objects_state, react_state, space_state, catalog_state, perm_state, net_state,
              step_state, elaborate_state, journal.next_id())) {
        record::Event dropped;
        dropped.step = clock.step;
        dropped.kind = record::Kind::System;
        dropped.name = "overflow";
        dropped.detail = {
            {"queue", "snapshot_buffers"}, {"reason", "writer_busy"}, {"skipped", "snapshot"}};
        journal.append(std::move(dropped));
      }
    }
    const auto now = Monotonic::now();
    const auto duration = std::chrono::duration<double, std::milli>(now - frame.started).count();
    if (phase_ms.size() < phase_samples_max)
      phase_ms.push_back(duration);
    else {
      phase_ms[phase_cursor] = duration;
      phase_cursor = (phase_cursor + 1) % phase_samples_max;
    }
    const auto next =
        finish_frame(clock, costs, frame,
                     std::chrono::duration<double, std::milli>(now - physics_start).count(), now);
    TracyPlot("step work ms", duration);
    TracyPlot("foreground items", static_cast<std::int64_t>(frame.foreground));
    TracyPlot("background items", static_cast<std::int64_t>(frame.background));
    TracyPlot("step overruns", static_cast<std::int64_t>(clock.overruns));
    FrameMarkEnd("step work");
    if (launch.realtime)
      std::this_thread::sleep_until(next);
    FrameMark;
  }
  server.stop();
  fixture_worker.finish();
  if (!fixture_case && (launch.network || launch.snapshot_at_end) &&
      snapshot_progress.last_step != clock.step)
    while (!checkpoint::save_snapshot(
        snapshot_writer, snap::Header{runtime_version, clock.step, hash_bytes(registry.hash)},
        objects_state, react_state, space_state, catalog_state, perm_state, net_state, step_state,
        elaborate_state, journal.next_id()))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  snapshot_writer.finish();
  capture_writer.finish();
  for (const auto &completed : capture_writer.take())
    elaborate::complete_capture(captures, completed);
  writer.submit(journal.take());
  writer.finish();
  const auto elapsed = std::chrono::duration<double>(Monotonic::now() - started).count();
  if (fixture_case)
    fixture::write_report(launch.report, values, *fixture_case, clock.step, hz, script_errors);
  else if (!launch.report.empty()) {
    auto report =
        state_report(objects_state, catalog_state, react_state, clock, script_errors, timers);
    report["configuration"] = {{"profile", launch.profile},
                               {"handler_ms", settings.scripts.handler_ms}};
    std::ofstream output(launch.report);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << report.dump(2) << '\n';
  }
  std::sort(phase_ms.begin(), phase_ms.end());
  const auto percentile = [&](double p) {
    return phase_ms.empty() ? 0 : phase_ms[static_cast<std::size_t>((phase_ms.size() - 1) * p)];
  };
  std::cout << Json{{"status", "stopped"},
                    {"profile", launch.profile},
                    {"handler_ms", settings.scripts.handler_ms},
                    {"step", clock.step},
                    {"elapsed_s", elapsed},
                    {"overruns", clock.overruns},
                    {"records", writer.stats().records},
                    {"queued", queues.causal.size() + queues.turns.size()},
                    {"gaps", gaps.size()},
                    {"script_errors", script_errors},
                    {"phase_p50_ms", percentile(.5)},
                    {"phase_p95_ms", percentile(.95)},
                    {"phase_sample_steps", phase_ms.size()},
                    {"snapshot_pause_max_ms", snapshot_progress.pause_max_ms}}
                   .dump()
            << '\n';
  return 0;
}
} // namespace amber::step
