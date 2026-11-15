#include "bindings/authoring/authoring.hpp"
#include "bindings/authoring/failures.hpp"
#include "bindings/authority/authority.hpp"
#include "bindings/causality_snapshot.hpp"
#include "bindings/checkpoint/checkpoint.hpp"
#include "bindings/commerce/admin.hpp"
#include "bindings/commerce/commerce.hpp"
#include "bindings/construction/construction.hpp"
#include "bindings/controls/controls.hpp"
#include "bindings/editing/editing.hpp"
#include "bindings/elaboration/elaboration.hpp"
#include "bindings/execution/credits.hpp"
#include "bindings/finance/finance.hpp"
#include "bindings/fixture/fixture.hpp"
#include "bindings/goods.hpp"
#include "bindings/illumination.hpp"
#include "bindings/installation/installation.hpp"
#include "bindings/measurements.hpp"
#include "bindings/metadata/metadata.hpp"
#include "bindings/navigation.hpp"
#include "bindings/physical.hpp"
#include "bindings/projection.hpp"
#include "bindings/replication.hpp"
#include "bindings/revision/revision.hpp"
#include "bindings/scene.hpp"
#include "bindings/start/start.hpp"
#include "bindings/structural/revision.hpp"
#include "bindings/travel.hpp"
#include "bindings/viewing/viewing.hpp"
#include "checkpoint_filter.hpp"
#include "fixture.hpp"
#include "recovery.hpp"
#include "settings.hpp"
#include "snap/objects.hpp"

#include <tracy/Tracy.hpp>

#include <csignal>
#include <fstream>
#include <iostream>
#include <numeric>
#include <thread>

using namespace amber;
namespace bi = amber::bindings;
namespace {
using Json = nlohmann::json;
volatile std::sig_atomic_t stopping = 0;
void stop(int) {
  stopping = 1;
}
struct Launch {
  std::filesystem::path config = "config.toml", plan = ".cache/content/seeded-plan.json";
  std::filesystem::path assets = "assets", data = ".runtime", client = "client/dist";
  std::filesystem::path report;
  std::filesystem::path restore, catalog, fixture_case;
  catalog::Hash candidate, candidate_registry;
  PlayerId candidate_author = 0;
  bool candidate_active = false;
  bool snapshot_at_end = false;
  std::uint64_t steps = 0;
  std::uint16_t port = 8080;
  bool network = true, realtime = true;
};
Launch arguments(int argc, char **argv) {
  Launch launch;
  for (int i = 1; i < argc; ++i) {
    const std::string name = argv[i];
    if (name == "run")
      continue;
    if (name == "--no-network")
      launch.network = false;
    else if (name == "--unpaced")
      launch.realtime = false;
    else if (name == "--candidate-active")
      launch.candidate_active = true;
    else if (name == "--snapshot-at-end")
      launch.snapshot_at_end = true;
    else {
      if (++i == argc)
        throw std::invalid_argument("missing value for " + name);
      const std::string value = argv[i];
      if (name == "--config")
        launch.config = value;
      else if (name == "--plan")
        launch.plan = value;
      else if (name == "--assets")
        launch.assets = value;
      else if (name == "--data")
        launch.data = value;
      else if (name == "--client")
        launch.client = value;
      else if (name == "--report")
        launch.report = value;
      else if (name == "--restore")
        launch.restore = value;
      else if (name == "--fixture-case")
        launch.fixture_case = value;
      else if (name == "--candidate")
        launch.candidate = value;
      else if (name == "--candidate-registry")
        launch.candidate_registry = value;
      else if (name == "--candidate-author")
        launch.candidate_author = std::stoul(value);
      else if (name == "--catalog")
        launch.catalog = value;
      else if (name == "--steps")
        launch.steps = std::stoull(value);
      else if (name == "--port") {
        const auto port = std::stoul(value);
        if (port > 65535)
          throw std::invalid_argument("port exceeds 65535");
        launch.port = port;
      } else
        throw std::invalid_argument("unknown argument: " + name);
    }
  }
  return launch;
}
struct ViewTransport {
  net::Server &server;
  bi::ReplicationIndex &replication;
  bi::viewing::SessionViews &views;
  IdentityRead identity;
  AuthorityRead authority;
  ParentRead parents;
  const catalog::ModelTexts &model_text;
  const catalog::InstanceTexts &instance_text;
};
void send(void *local, net::SessionId session, std::string text) {
  static_cast<ViewTransport *>(local)->server.send(session, std::move(text));
}
void close(void *local, net::SessionId session, std::uint16_t code, std::string reason) {
  static_cast<ViewTransport *>(local)->server.close(session, code, std::move(reason));
}
void transport_view(ViewTransport &b, net::SessionId session, Vec3 position) {
  const auto before = b.replication.interests.find(session);
  const auto previous =
      before == b.replication.interests.end() ? std::set<net::Cell>{} : before->second;
  bi::view_interest(b.replication, session, position);
  b.server.view(session, position);
  std::vector<Handle> visible;
  const auto collect = [&](net::Cell cell) {
    if (auto found = b.replication.cells.find(cell); found != b.replication.cells.end())
      for (auto bits : found->second)
        visible.push_back({bits});
  };
  for (auto cell : b.replication.interests.at(session))
    if (!previous.contains(cell))
      collect(cell);
  if (previous.empty() && !b.replication.interests.at(session).contains(net::global_cell))
    collect(net::global_cell);
  if (!visible.empty())
    b.server.send(session,
                  Json{{"t", "metadata"},
                       {"objects", bi::object_metadata(b.identity, b.authority, b.parents,
                                                       b.model_text, b.instance_text, visible)}}
                      .dump());
}
void view(void *local, net::SessionId session, Vec3 position) {
  auto &b = *static_cast<ViewTransport *>(local);
  bi::viewing::view_changed(b.views, session, position);
  transport_view(b, session, position);
}
struct ObserverInterest {
  const control::Sessions &sessions;
  ViewTransport &transport;
};
void observer_interest(void *local, net::SessionId session, Vec3 position) {
  auto &b = *static_cast<ObserverInterest *>(local);
  if (const auto principal = b.sessions.principal(session); principal && principal->binary)
    transport_view(b.transport, session, position);
}
struct Welcome {
  const drive::Clock &clock;
  double hz;
  float cell_m;
  std::uint32_t repl_delay_ms;
};
Json welcome(void *local, const control::Principal &) {
  const auto &b = *static_cast<Welcome *>(local);
  return {{"step", b.clock.step},
          {"step_hz", b.hz},
          {"cell_m", b.cell_m},
          {"repl_delay_ms", b.repl_delay_ms},
          {"assets", "/assets/"}};
}
std::array<std::uint8_t, 32> hash_bytes(std::string_view hash) {
  if (!catalog::valid_hash(hash))
    throw std::invalid_argument("invalid registry hash");
  const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  std::array<std::uint8_t, 32> result{};
  for (std::size_t i = 0; i < result.size(); ++i)
    result[i] = (nibble(hash[2 * i]) << 4) | nibble(hash[2 * i + 1]);
  return result;
}
std::string hash_text(std::span<const std::uint8_t> hash) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(hash.size() * 2);
  for (auto byte : hash) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 15]);
  }
  return result;
}
struct ObserveRecord {
  record::Journal &journal;
  const drive::Clock &clock;
  bi::CreditResolver credits;
  double hz;
};
struct PermissionChanges {
  bi::TravelAccess &travel;
  const perm::Grants &grants;
  bi::viewing::SessionViews &views;
  const drive::Clock &clock;
};
void permissions_changed(void *local) {
  auto &b = *static_cast<PermissionChanges *>(local);
  bi::invalidate_travel(b.travel, b.grants, b.clock.step);
  for (auto &[session, view] : b.views.rows) {
    (void)session;
    view.pending = {};
    observe::forget(view.sent);
  }
}
void activate_rule(void *local, ScriptId script, bi::elaboration::RuleUpdate update, Step now,
                   double hz, const record::Origin &origin, react::DeferredEffects &effects) {
  const auto mode = update == bi::elaboration::RuleUpdate::Validated ? bi::RuleMode::Validated
                    : update == bi::elaboration::RuleUpdate::Enable  ? bi::RuleMode::Enable
                                                                     : bi::RuleMode::Disable;
  bi::update_rule(*static_cast<bi::RuleActivation *>(local), script, mode, now, hz, origin,
                  effects);
}
struct ElaborationNotifications {
  control::Sessions &sessions;
  const elaborate::Requests &requests;
  bi::MetadataChanges &editing;
};
void elaboration_notification(void *local, const elaborate::CloseOutcome &closed) {
  auto &b = *static_cast<ElaborationNotifications *>(local);
  b.sessions.event_for_player(closed.requester,
                              {{"kind", closed.kind},
                               {"request", "request:" + std::to_string(closed.request)},
                               {"text", closed.text}});
  const auto object = b.requests.rows.at(closed.request).target;
  bi::metadata_changed(b.editing, std::span{&object, 1}, {});
}
struct ImportGeometry {
  bi::GeometryAssets &geometry;
  space::Space &space;
  std::filesystem::path root;
};
void import_geometry(void *local, const catalog::Asset &asset) {
  auto &b = *static_cast<ImportGeometry *>(local);
  bi::import_geometry_asset(b.geometry, b.space, asset, b.root);
}
void record_observation(void *local, PlayerId player, Handle, std::span<const Handle> things,
                        const Json &response) {
  auto &b = *static_cast<ObserveRecord *>(local);
  record::Event event;
  event.step = b.clock.step;
  event.kind = record::Kind::Observation;
  event.actor = record::Actor::player(player);
  event.name = "look";
  event.things.assign(things.begin(), things.end());
  event.detail = {{"observation", response}};
  event.credits = b.credits.resolve(b.credits.context, things, 0, b.hz);
  b.journal.append(std::move(event));
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc > 1 && std::string_view(argv[1]) == "fixture")
      return fixture::command(argc, argv, std::filesystem::canonical("/proc/self/exe"));
    auto launch = arguments(argc, argv);
    if (launch.restore.empty())
      launch.restore = recovery::latest_snapshot(launch.data);
    std::optional<bi::fixture::Case> fixture_case;
    if (!launch.fixture_case.empty()) {
      if (launch.restore.empty() || launch.network || launch.realtime || launch.report.empty())
        throw std::invalid_argument(
            "fixture child requires restore, no network, unpaced and report");
      fixture_case = bi::fixture::read_case(launch.fixture_case);
    }
    const auto settings = read_settings(launch.config);
    const auto hz = settings.clock.step_hz;
    const auto plan = bi::start::read_plan(launch.plan);
    std::filesystem::create_directories(launch.data);
    react::Terms terms;
    react::Materials materials;
    catalog::MechanicalBindings mechanical;
    snap::Buffer restored_bytes;
    std::optional<snap::View> restored;
    const auto catalog_root =
        launch.catalog.empty()
            ? (launch.restore.empty() ? launch.data : launch.restore.parent_path().parent_path())
            : launch.catalog;
    catalog::RegistryDocument registry;
    if (launch.restore.empty()) {
      registry =
          bi::start::load_registry(plan, launch.data / "registry", terms, materials, mechanical);
    } else {
      restored_bytes = snap::read_file(launch.restore);
      const std::array<std::string_view, 1> versions{"ambergris-0.1"};
      restored = snap::parse(restored_bytes, versions);
      registry = catalog::read_registry(catalog_root / "registry",
                                        hash_text(restored->header.registry_hash));
      catalog::load_terms(registry, terms);
      catalog::load_materials(registry, terms, materials);
      mechanical.values = registry.data.at("mechanical");
    }
    if (fixture_case) {
      const auto candidate_registry =
          catalog::read_registry(catalog_root / "registry", launch.candidate_registry);
      react::Terms candidate_terms;
      catalog::load_terms(candidate_registry, candidate_terms);
      if (candidate_terms.rows.size() < terms.rows.size() ||
          candidate_registry.data.at("mechanical") != mechanical.values)
        throw std::invalid_argument(
            "candidate registry is not an extension of the capture vocabulary");
      for (std::size_t id = 1; id < terms.rows.size(); ++id)
        if (catalog::term_json(terms.rows[id]) != catalog::term_json(candidate_terms.rows[id]))
          throw std::invalid_argument("candidate changed a captured term definition");
      const auto previous_size = terms.rows.size();
      terms = std::move(candidate_terms);
      react::Materials candidate_materials;
      catalog::load_materials(candidate_registry, terms, candidate_materials);
      for (auto &[id, data] : candidate_materials.rows)
        if (id >= previous_size)
          materials.rows.emplace(id, std::move(data));
    }
    command::NativeActions native_declarations;
    drive::NativeOperations native_operations;
    const auto editing_declarations = bi::editing::declare_editing(terms, native_declarations);
    const auto commerce_declarations = bi::commerce::declare_commerce(terms, native_declarations);
    const auto admin_declarations = bi::commerce::declare_admin(terms, native_declarations);
    const auto elaboration_declarations =
        bi::elaboration::declare_elaboration(terms, native_declarations);
    if (!restored)
      registry = catalog::read_registry(
          launch.data / "registry",
          catalog::save_registry(launch.data / "registry", terms, materials, {}, mechanical));
    const auto role = [&](const char *group, const char *name) {
      return mechanical.values.at(group).at(name).get<TermId>();
    };

    Objects objects;
    const auto ids = identity(objects);
    const auto poses = transforms(objects);
    const auto auth = authority(std::as_const(objects));
    const auto parent = parents(objects);
    const auto physical_flags = space_flags(objects);
    lifecycle::Parts parts;
    Relation declared_parts;
    lifecycle::Instances lifetimes(objects, parts, declared_parts);
    catalog::Assets assets;
    catalog::Models models;
    catalog::Scripts scripts;
    catalog::ParameterBlocks parameters;
    catalog::ModelTexts model_text;
    catalog::InstanceTexts instance_text;
    catalog::AuthoredTexts authored_text;
    react::AssetMeasurements baked_measurements;
    react::RuntimeMeasurements runtime_measurements;
    react::Properties properties;
    react::Processes processes;
    react::InstanceSettings instance_settings;
    react::ModelSettings model_settings;
    react::PlotSettings plot_settings;
    react::WorldSettings world_settings;
    react::SharedColumns shared;
    react::GivesTo gives_to;
    react::AllocationRows allocations;
    react::MeterRows meters;
    react::PriorityLinks priorities;
    bi::AllocationReconciliation allocation{allocations, meters, priorities};
    react::AuthoredRelations authored_relations;
    react::RelationsRead relations;
    space::PhysicalRelations physical_relations;
    for (const auto &term : terms.rows)
      if (term.kind == react::Kind::Relation && term.id)
        relations.columns[term.id] = &authored_relations[term.id];
    relations.columns[role("relations", "contains")] = &physical_relations.contains;
    relations.columns[role("relations", "supports")] = &physical_relations.supports;
    relations.columns[role("relations", "attached_to")] = &physical_relations.attached_to;
    relations.columns[role("relations", "in_zone")] = &physical_relations.in_zone;
    const react::ValueRead values{terms,
                                  ids,
                                  objects.plot,
                                  objects.params,
                                  materials,
                                  baked_measurements,
                                  properties,
                                  processes,
                                  instance_settings,
                                  model_settings,
                                  plot_settings,
                                  world_settings,
                                  shared,
                                  relations,
                                  runtime_measurements,
                                  role("measurements", "density")};
    react::ScriptStates states;
    react::RandomStates random;
    react::Triggers triggers;
    react::RuleVersions rules;
    react::Subscriptions subscriptions;
    install::WatchLinks watch_links;
    install::InstanceScripts installations;
    perm::Players players;
    perm::Groups groups;
    perm::Plots plots;
    perm::Grants grants;
    bi::TravelAccess travel_access;
    perm::Ledger ledger;
    perm::OwnerLimits owner_limits;
    perm::Offers offers;
    perm::Trades trades;
    control::Profiles profiles;
    step::WorkQueues queues;
    step::TimerWheel timers;
    step::Intents intents;
    drive::Inputs inputs;
    elaborate::Requests requests;
    elaborate::GapCounts gap_counts;
    elaborate::LastGaps last_gaps;
    elaborate::CaptureRefs captures;
    elaborate::PendingFixtures fixtures;
    std::vector<step::IntentEnding> intent_endings;
    drive::Clock clock;
    space::Space space(settings.space);
    bi::MechanicalCauses mechanical_causes(settings.authors_max);
    bi::GeometryAssets geometry;
    std::vector<space::RelationChange> relation_changes;
    observe::Hearing hearing;
    std::vector<observe::Listener> listeners;
    bi::DeliveryTargets delivery;
    observe::Lights lights(settings.light_cell_m);
    observe::LightSubscriptions light_subscriptions(settings.light_cell_m);
    observe::ZoneAmbients zones;
    observe::ActiveViews active_views(settings.light_cell_m);
    bi::ReplicationIndex replication(settings.network);
    bi::AssemblyMasses masses{
        {role("measurements", "mass"), role("measurements", "assembly_mass")}, {}, {}};
    bi::viewing::SessionViews session_views;
    bi::viewing::PublicSources sources;
    bi::IlluminanceReads illuminance_reads{light_subscriptions, subscriptions, ids, poses,
                                           role("measurements", "illuminance")};
    bi::IlluminanceSampling illuminance_sampling{
        light_subscriptions,        space, values, lights, zones, runtime_measurements, 0,
        settings.scripts.handler_ms};
    bi::RunningLights running_lights;
    bi::RunningZones running_zones;
    bi::IlluminationProcesses illumination_processes{running_lights, running_zones, lights, zones,
                                                     processes};
    bi::MetadataChanges editing_changes{session_views, illuminance_reads, {}, {}};
    PermissionChanges permission_changes{travel_access, grants, session_views, clock};
    bi::LightProjection light_projection{
        lights,       light_subscriptions, ids,    poses, objects.params,
        models,       parameters,          assets, terms, settings.light_source_radius_m,
        &active_views};
    bi::ZoneProjection zone_projection{zones,      ids,    objects.params, models,
                                       parameters, assets, terms};
    bi::MassProjection mass_projection{masses, runtime_measurements, ids, declared_parts};
    bi::ReplicationProjection replication_projection{replication, poses,  models,
                                                     parameters,  assets, values};
    bi::SceneProjection scene_projection({light_projection, zone_projection, mass_projection,
                                          replication_projection, illumination_processes,
                                          illuminance_reads, values, editing_changes});
    react::Vm vm(settings.scripts);

    react::ReadBinding read{values};
    react::StateBinding state{properties, processes, terms};
    react::RelationBinding related{relations, terms};
    react::ContributionBinding contribute{shared, gives_to, terms, relations};
    react::ShareBinding share{allocations, terms};
    bi::ContextBinding context{auth};
    bi::ObjectBinding object{ids, poses, auth, parent, objects.params, models, installations};
    bi::AssetReadBinding asset{ids, objects.params, models, parameters, assets};
    bi::MaterialReadBinding material{terms, materials};
    bi::ModelMeasureBinding measure_model{terms,      materials, models,
                                          parameters, assets,    values.density};
    bi::InstanceMeasureBinding measure_instance{
        ids,       poses,  objects.params, declared_parts, terms,
        materials, models, parameters,     assets,         values.density};
    bi::MeterReadBinding meter{meters, terms, ids};
    bi::PlotReadBinding plot{plots, auth};
    bi::ExchangeBinding exchange{properties, processes, terms, settings.exchange_max};
    bi::ConstructionBinding construction{models,     parameters, terms,
                                         properties, processes,  settings.exchange_max};
    bi::RemovalBinding removal{ids, terms, properties, processes, settings.exchange_max};
    bi::ModelRevisionRead revision_read{ids,    objects.params, parts, models,        parameters,
                                        assets, materials,      terms, values.density};
    bi::ModelRevisionMeasure revision_measure{revision_read};
    bi::ModelRevisionQueue revision_queue{models, properties, processes, terms,
                                          settings.exchange_max};
    bi::PlotTransferBinding transfer_plot{plots, grants, players, &permission_changes,
                                          permissions_changed};
    bi::BalanceBinding balance{ledger, ids, auth, players};
    bi::PaymentBinding payment{ledger, grants, players};
    bi::IssuanceBinding issuance{ledger, grants, players};
    bi::NavigationBinding navigation{intents, intent_endings, ids,   auth,          owner_limits,
                                     players, grants,         plots, declared_parts};
    bi::AttachmentBinding attachment{space,
                                     ids,
                                     poses,
                                     parent,
                                     physical_flags,
                                     physical_relations,
                                     {role("relations", "contains"), role("relations", "supports"),
                                      role("relations", "attached_to"),
                                      role("relations", "in_zone")},
                                     declared_parts,
                                     auth,
                                     owner_limits,
                                     players,
                                     grants,
                                     settings.perception.reach_m,
                                     mechanical_causes};
    bi::ImpulseBinding impulse{space,   ids,    auth,           owner_limits,
                               players, grants, declared_parts, mechanical_causes};
    bi::SweepBinding sweep{
        space, ids, poses, auth, owner_limits, players, grants, declared_parts, mechanical_causes};
    bi::PoseBatchBinding pose_batch{space,
                                    ids,
                                    poses,
                                    auth,
                                    owner_limits,
                                    players,
                                    grants,
                                    declared_parts,
                                    settings.pose_batch_max,
                                    mechanical_causes};
    bi::PartPoseBinding part_pose{space, ids, poses, declared_parts, mechanical_causes};
    bi::QuantityBinding transfer{
        properties,   processes, terms,          ids,
        auth,         parent,    declared_parts, physical_relations.attached_to,
        owner_limits, players,   grants};
    bi::PositionBinding position{ids, poses};
    bi::QueryBinding query{space, ids, settings.perception.far_m};
    bi::SoundBinding sound{hearing, ids, poses, listeners};
    bi::AvatarControlBinding avatar{grants, players, ids, auth, installations, models};
    bi::PlotObjectsBinding plot_objects{ids, objects.plot, declared_parts, plots};
    bi::HoldGoodsBinding hold_goods{space,
                                    ids,
                                    auth,
                                    parent,
                                    physical_flags,
                                    physical_relations,
                                    declared_parts,
                                    owner_limits,
                                    grants,
                                    players,
                                    delivery,
                                    settings.mailbox_max,
                                    relation_changes,
                                    scene_projection};
    const auto bind = [&](const char *name, auto function, auto &local) {
      react::register_binding(vm, name, function, &local);
    };
    react::bind_events(vm, terms);
    bind("get", react::get_binding, read);
    bind("read", react::read_binding, read);
    bind("setting", react::setting_binding, read);
    bind("set", react::set_binding, state);
    bind("rate", react::rate_binding, state);
    bind("related", react::related_binding, related);
    bind("contribute", react::contribute_binding, contribute);
    bind("get_share", react::share_binding, share);
    bind("context", bi::context_binding, context);
    bind("object", bi::object_binding, object);
    bind("asset", bi::asset_binding, asset);
    bind("material", bi::material_binding, material);
    bind("measure_model", bi::measure_model_binding, measure_model);
    bind("measure_instance", bi::measure_instance_binding, measure_instance);
    bind("meter", bi::meter_binding, meter);
    bind("plot", bi::plot_binding, plot);
    bind("exchange", bi::exchange_binding, exchange);
    bind("create", bi::create_binding, construction);
    bind("replace", bi::replace_binding, construction);
    bind("remove", bi::remove_binding, removal);
    bind("measure_model_change", bi::measure_model_change_binding, revision_measure);
    bind("revise_model", bi::revise_model_binding, revision_queue);
    bind("transfer_plot", bi::transfer_plot_binding, transfer_plot);
    bind("balance", bi::balance_binding, balance);
    bind("pay", bi::payment_binding, payment);
    bind("issue", bi::issuance_binding, issuance);
    bind("navigate", bi::navigate_binding, navigation);
    bind("cancel_motion", bi::cancel_motion_binding, navigation);
    bind("attach", bi::attach_binding, attachment);
    bind("grasp", bi::grasp_binding, attachment);
    bind("release", bi::release_binding, attachment);
    bind("impulse", bi::impulse_binding, impulse);
    bind("sweep", bi::sweep_binding, sweep);
    bind("pose_batch", bi::pose_batch_binding, pose_batch);
    bind("move_part", bi::move_part_binding, part_pose);
    bind("transfer", bi::transfer_binding, transfer);
    bind("position", bi::position_binding, position);
    bind("transform", bi::transform_binding, position);
    bind("near", bi::near_binding, query);
    bind("ray", bi::ray_binding, query);
    bind("sound", bi::sound_binding, sound);
    bind("allow_avatar", bi::allow_avatar_binding, avatar);
    bind("revoke_avatar", bi::revoke_avatar_binding, avatar);
    bind("plot_objects", bi::plot_objects_binding, plot_objects);
    bind("hold_goods", bi::hold_goods_binding, hold_goods);

    bi::start::Symbols symbols;
    std::vector<Handle> initial;
    react::DeferredEffects setup;
    std::uint64_t next_record = 1;
    std::vector<space::MotionRow> restored_motion;
    std::vector<bi::BodyCauseRow> restored_causes;
    if (restored) {
      const snap::SectionReaders readers{
          [&objects](snap::Input &in) { snap::read_objects(in, objects); },
          [&properties, &processes, &instance_settings, &model_settings, &plot_settings,
           &world_settings, ids](snap::Input &in) {
            bi::checkpoint::read_prop(in, properties, processes, instance_settings, model_settings,
                                      plot_settings, world_settings, ids.generation.size());
          },
          [&authored_relations, &physical_relations, &parts, &declared_parts,
           ids](snap::Input &in) {
            bi::checkpoint::read_rels(in, authored_relations, physical_relations, parts, ids,
                                      declared_parts);
          },
          [&shared, &gives_to, &allocations, &meters, ids](snap::Input &in) {
            react::read_shared(in, shared, gives_to, allocations, meters, ids.generation.size());
          },
          [&models, &parameters, &scripts, &model_text, &instance_text,
           &authored_text](snap::Input &in) {
            bi::checkpoint::read_text(in, models, parameters, scripts, model_text, instance_text,
                                      authored_text);
          },
          [&players, &ledger, &plots, &groups, &profiles](snap::Input &in) {
            bi::checkpoint::read_players(in, players, ledger, plots, groups, profiles);
          },
          [&grants, &offers, &trades, &owner_limits, ids](snap::Input &in) {
            perm::read_grants(in, grants, offers, trades, owner_limits, ids.generation.size());
          },
          [&states, &subscriptions, &installations, ids](snap::Input &in) {
            bi::checkpoint::read_scst(in, states, subscriptions, installations,
                                      ids.generation.size());
          },
          [&timers](snap::Input &in) { step::read_timers(in, timers); },
          [&queues, &intents, &next_record, &inputs](snap::Input &in) {
            next_record = bi::checkpoint::read_queues(in, queues, intents, inputs);
          },
          [&requests, &gap_counts, &last_gaps, &captures, &fixtures](snap::Input &in) {
            bi::checkpoint::read_reqs(in, requests, gap_counts, last_gaps, captures, fixtures);
          },
          [&random](snap::Input &in) { react::read_random(in, random); },
          [&restored_motion, &restored_causes](snap::Input &in) {
            space::read_motion(in, restored_motion);
            bi::read_motion_causes(in, restored_causes);
          }};
      snap::decode(*restored, readers);
      clock.step = restored->header.step;
      for (auto &[id, input] : inputs.rows) {
        bool fixture_pending = false;
        for (auto &[job_id, job] : fixtures.rows) {
          (void)job_id;
          if (job.order.session == std::to_string(input.session) &&
              job.order.sequence == input.sequence && input.awaiting) {
            job.order.session = "0";
            job.order.sequence = id;
            fixture_pending = true;
          }
        }
        input.session = 0;
        input.sequence = fixture_pending ? id : 0;
        if (input.awaiting && !fixture_pending) {
          input.awaiting = false;
          step::append_inputs(queues, {step::InputWork{id}});
        }
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
      catalog::load_rules(registry, scripts, rules);
      const react::RightsDeclaration rights{
          {"use", "take", "move", "edit", "copy", "transfer", "destroy"},
          {"spend", "build", "enter", "run_scripts", "animate", "camera", "issue"}};
      for (const auto &script : scripts.rows) {
        if (!script.id)
          continue;
        auto prepared = catalog::read_script(catalog_root / "scripts", script.artifact,
                                             settings.pins, script.registry_pin, terms, rights);
        const auto loaded = react::load_script(vm, prepared.compiled, script.declaration);
        if (loaded.status != react::RunOutcome::Status::Ok)
          throw std::runtime_error(loaded.error);
        sources.scripts[script.id] = prepared.source;
        catalog::write_script(launch.data / "scripts" / prepared.hash, prepared);
      }
      const auto load_asset = [&](const catalog::Hash &hash, const std::string &canonical) {
        if (assets.rows.contains(hash))
          return;
        auto prepared = catalog::read_asset(launch.assets, hash, canonical, settings.pins);
        catalog::resolve_manifest_terms(prepared, terms);
        sources.assets[hash] = prepared.source;
        catalog::import_asset(assets, std::move(prepared));
      };
      for (const auto &model : models.rows)
        if (model.id)
          load_asset(model.asset, model.canonical_params);
      for (const auto &block : parameters.rows)
        if (block.id)
          load_asset(block.asset, block.canonical_params);
      // Reconstruct pointer-only views after the relation owner rebuilt its map.
      relations.columns.clear();
      for (auto &[id, relation] : authored_relations)
        relations.columns[id] = &relation;
      for (const auto &term : terms.rows)
        if (term.id && term.kind == react::Kind::Relation && !relations.columns.contains(term.id))
          relations.columns[term.id] = &authored_relations[term.id];
      relations.columns[role("relations", "contains")] = &physical_relations.contains;
      relations.columns[role("relations", "supports")] = &physical_relations.supports;
      relations.columns[role("relations", "attached_to")] = &physical_relations.attached_to;
      relations.columns[role("relations", "in_zone")] = &physical_relations.in_zone;
      for (std::uint32_t slot = 1; slot < ids.model.size(); ++slot)
        if (ids.valid(ids.at(slot)))
          initial.push_back(ids.at(slot));
    } else {
      auto prepared_scripts =
          bi::start::prepare_scripts(plan, terms, registry.hash, settings.pins, settings.scripts);
      for (const auto &script : prepared_scripts)
        catalog::write_script(launch.data / "scripts" / script.artifact.hash, script.artifact);
      std::vector<bi::start::SeedRuleApproval> approvals;
      for (const auto &script : prepared_scripts)
        if (script.requested_rule)
          approvals.push_back(
              {script.key, "trusted local initial-world content selected at launch"});
      bi::start::install_scripts(prepared_scripts, scripts, vm, symbols, approvals);
      for (const auto &script : prepared_scripts)
        sources.scripts[symbols.scripts.at(script.key)] = script.artifact.source;
      auto prepared_assets = bi::start::prepare_assets(plan, launch.assets, settings.pins, terms);
      for (const auto &entry : prepared_assets)
        sources.assets[entry.asset.hash] = entry.source;
      bi::start::import_assets(std::move(prepared_assets), assets);
      bi::start::install_models(plan, assets, scripts, terms, models, model_text, model_settings,
                                symbols);
      for (const auto &script : scripts.rows)
        if (script.id && script.declaration.kind == react::ScriptDeclaration::Kind::Rule)
          install::add_rule(rules, scripts, terms, script.id);
      initial = bi::start::install_objects(plan, symbols, models, parameters, assets, terms,
                                           lifetimes, clock.step);
      bi::start::install_properties(plan, symbols, terms, properties, processes, instance_settings,
                                    world_settings, plot_settings, clock.step, hz);
      bi::start::install_relations(plan, symbols, terms, authored_relations, setup);
      bi::start::install_script_state(plan, symbols, states);
      bi::start::install_profiles(plan, symbols, profiles, players,
                                  std::filesystem::current_path());
      bi::start::install_land(plan, plots, groups, players);
      bi::start::install_money(plan, players, ledger);
      bi::start::install_grants(plan, symbols, values, auth, owner_limits, models, scripts,
                                installations, rules, plots, profiles, grants, players, clock.step,
                                hz);
    }
    install::RuleChange fixture_rule_change;
    if (fixture_case) {
      ScriptId candidate = 0;
      for (const auto &script : scripts.rows)
        if (script.id && script.artifact == launch.candidate)
          candidate = script.id;
      if (!candidate) {
        const react::RightsDeclaration rights{
            {"use", "take", "move", "edit", "copy", "transfer", "destroy"},
            {"spend", "build", "enter", "run_scripts", "animate", "camera", "issue"}};
        auto prepared =
            catalog::read_script(catalog_root / "scripts", launch.candidate, settings.pins,
                                 launch.candidate_registry, terms, rights);
        const auto compiled = prepared.compiled;
        const auto source = prepared.source;
        catalog::write_script(launch.data / "scripts" / prepared.hash, prepared);
        candidate = catalog::import_script(scripts, std::move(prepared), launch.candidate_author,
                                           clock.step / hz);
        const auto loaded = react::load_script(vm, compiled, scripts.rows[candidate].declaration);
        if (loaded.status != react::RunOutcome::Status::Ok)
          throw std::runtime_error(loaded.error);
        sources.scripts[candidate] = source;
      }
      auto &declaration = scripts.rows[candidate].declaration;
      if (declaration.kind != react::ScriptDeclaration::Kind::Rule)
        throw std::invalid_argument("fixture candidate must be a world rule");
      declaration.approved = launch.candidate_active;
      declaration.active = launch.candidate_active;
      react::set_activation(vm, candidate, declaration.approved, declaration.active);
      if (launch.candidate_active)
        fixture_rule_change = install::add_rule(rules, scripts, terms, candidate);
    }
    for (const auto &model : models.rows)
      if (model.id) {
        catalog::project_measurements(
            baked_measurements, model.id, 0, assets.rows.at(model.asset),
            {role("measurements", "volume"), role("measurements", "capacity")});
        install::project_model(triggers, models, scripts, terms, model.id);
      }
    for (const auto &block : parameters.rows)
      if (block.id)
        catalog::project_measurements(
            baked_measurements, block.model, block.id, assets.rows.at(block.asset),
            {role("measurements", "volume"), role("measurements", "capacity")});
    install::project_rules(triggers, rules);
    registry = catalog::read_registry(
        launch.data / "registry",
        catalog::save_registry(launch.data / "registry", terms, materials, rules, mechanical));
    for (const auto &[hash, entry] : assets.rows) {
      (void)hash;
      bi::import_geometry_asset(geometry, space, entry, launch.assets);
    }
    const bi::PhysicsRoles physics_roles{role("physics", "density"),
                                         role("physics", "transmission")};
    const bi::PhysicsDefaults physics_defaults{settings.default_friction,
                                               settings.default_restitution};
    bi::AssemblyPlacement placement{
        space,           ids, poses, parent, physical_flags, declared_parts, physical_relations,
        relation_changes};
    std::unordered_set<std::uint64_t> stored;
    for (const auto &player : players.rows)
      for (const auto *goods : {&player.inventory, &player.mailbox})
        for (auto root : *goods)
          for (auto h : lifecycle::collect(parts, ids, root))
            stored.insert(h.bits);
    std::vector<Handle> visible;
    for (auto h : initial) {
      if (stored.contains(h.bits))
        continue;
      const auto asset_hash =
          catalog::resolve_asset(models, parameters, ids.model[h.slot()], objects.params[h.slot()])
              .hash;
      const auto &entry = assets.rows.at(asset_hash);
      auto definition = bi::describe_physics(geometry, entry, values,
                                             {ids.model[h.slot()], objects.params[h.slot()],
                                              ids.material[h.slot()], objects.plot[h.slot()], h},
                                             ids.flags[h.slot()], physics_roles, physics_defaults,
                                             clock.step, hz);
      if (declared_parts.in.get(h.slot())) {
        definition.body.motion = space::Motion::Kinematic;
        definition.mode_flags = ScriptedMotion;
      }
      bi::install_physics(space, ids, poses, physical_flags, h, definition);
      visible.push_back(h);
    }
    if (!restored)
      bi::install_declared_attachments(placement, initial);
    if (restored) {
      space::rebuild_attachments(space, ids, poses, parent, physical_relations.attached_to);
      for (auto root : initial)
        if (!stored.contains(root.bits) && !(ids.flags[root.slot()] & Body) &&
            (ids.flags[root.slot()] & (Dynamic | Held)) && parts.get(root.slot()) &&
            !declared_parts.in.get(root.slot())) {
          const auto assembly = lifecycle::collect(parts, ids, root);
          const auto result =
              space::assemble(space, ids, poses, root, std::span<const Handle>(assembly).subspan(1),
                              space::Motion::Dynamic);
          if (result.status != space::Status::Ok)
            throw std::runtime_error(space::status_text(result.status));
        }
      space::restore_motion(space, ids, restored_motion);
      bi::restore_motion_causes(mechanical_causes, space, ids, restored_causes);
    }
    for (;;) {
      const auto navigation = space::install_ready_navigation(space);
      for (const auto &error : navigation.errors)
        std::cerr << "navigation: " << error << '\n';
      if (navigation.pending.empty())
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::vector<PlayerId> seed_authors;
    for (const auto &row : plan.at("objects"))
      seed_authors.push_back(row.at("maker").get<PlayerId>());
    std::sort(seed_authors.begin(), seed_authors.end());
    seed_authors.erase(std::unique(seed_authors.begin(), seed_authors.end()), seed_authors.end());
    record::Journal journal(settings.authors_max, seed_authors);
    journal.restore_next_id(next_record);
    bi::MetadataCredits credit_metadata{ids, attribution(objects), models, scripts, seed_authors};
    bi::CreditResolver credits{&credit_metadata, bi::resolve_credits};
    bi::PhysicalPublication physical_publication{
        mechanical_causes,
        queues,
        journal,
        {role("events", "contact"),
         {role("relations", "contains"), role("relations", "supports"),
          role("relations", "attached_to"), role("relations", "in_zone")}},
        settings.event_queue_max,
        hz,
        credits};
    recovery::rotate_record(launch.data / "events.jsonl.zst");
    record::Writer writer(launch.data / "events.jsonl.zst",
                          {{"spec", 1},
                           {"run", launch.data.filename().string()},
                           {"world", catalog::sha256(plan.dump())},
                           {"started", std::chrono::duration<double>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count()},
                           {"runtime", "ambergris-0.1"},
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
    if (!restored) {
      for (const auto &term : terms.rows)
        if (term.id) {
          record::Event event;
          event.kind = record::Kind::Create;
          event.actor = record::Actor::player(term.owner);
          event.name = "seed_term";
          event.detail = {{"of", "term"},
                          {"term", term.id},
                          {"seed", true},
                          {"agent", false},
                          {"maker", "p:" + std::to_string(term.owner)}};
          journal.append(std::move(event));
        }
      for (const auto &model : models.rows)
        if (model.id) {
          record::Event event;
          event.kind = record::Kind::Create;
          event.actor = record::Actor::player(model.author);
          event.name = "seed_model";
          event.detail = {{"of", "model"},
                          {"model", "m:" + std::to_string(model.id)},
                          {"maker", "p:" + std::to_string(model.author)},
                          {"agent", false},
                          {"seed", true}};
          journal.append(std::move(event));
        }
      for (const auto &script : scripts.rows)
        if (script.id) {
          record::Event event;
          event.kind = record::Kind::Create;
          event.actor = record::Actor::player(script.author);
          event.name = "seed_script";
          event.detail = {{"of", "script"},
                          {"script", "s:" + std::to_string(script.id)},
                          {"maker", "p:" + std::to_string(script.author)},
                          {"agent", false},
                          {"seed", true}};
          journal.append(std::move(event));
        }
      for (auto object : initial) {
        record::Event event;
        event.kind = record::Kind::Create;
        event.actor = record::Actor::player(objects.maker[object.slot()]);
        event.name = "seed_instance";
        event.things = {object};
        event.detail = {{"of", "instance"},
                        {"model", "m:" + std::to_string(ids.model[object.slot()])},
                        {"maker", "p:" + std::to_string(objects.maker[object.slot()])},
                        {"owner", "p:" + std::to_string(auth.owner[object.slot()])},
                        {"material", ids.material[object.slot()]},
                        {"agent", false},
                        {"seed", true}};
        journal.append(std::move(event));
      }
    }
    bi::Execution execution{vm, states, random, ids, declared_parts, 1, credits};
    react::rebuild_priorities(priorities, values);
    bi::InstallReactions reaction_birth{models,   scripts,       installations, rules,      values,
                                        triggers, subscriptions, watch_links,   allocations};
    bi::commerce::Custody commerce_custody{
        space,
        ids,
        poses,
        parent,
        physical_flags,
        physical_relations,
        parts,
        declared_parts,
        {role("relations", "contains"), role("relations", "supports"),
         role("relations", "attached_to"), role("relations", "in_zone")},
        scene_projection};
    bi::commerce::RetirementBinding commerce_retirement{offers, trades, permission_flags(objects),
                                                        commerce_custody};
    bi::commerce::RebindOffersBinding revised_goods{offers, trades, permission_flags(objects),
                                                    commerce_custody};
    bi::RetireReactions reaction_death{
        models,        scripts,     values,     installations, triggers,
        subscriptions, watch_links, properties, processes,     instance_settings,
        states,        random,      shared,     gives_to,      authored_relations,
        allocation};
    bi::SpatialRetirement spatial_death{
        space,   ids,           parent, physical_flags, physical_relations, relation_changes,
        intents, intent_endings};
    bi::NavigationDiagnostics navigation_diagnostics{space, ids, poses};
    bi::RebindPriorities rebound_priorities{values, allocation};
    bi::ObjectLifecycle object_lifecycle({lifetimes, scene_projection, reaction_birth,
                                          rebound_priorities, reaction_death, commerce_retirement,
                                          navigation_diagnostics, spatial_death, grants, players,
                                          owner_limits, auth, role("events", "start")});
    bi::InvocationInput initial_input;
    initial_input.step = clock.step;
    initial_input.hz = hz;
    bi::initialize_objects(object_lifecycle, initial, visible, restored.has_value(), initial_input,
                           setup);
    if (fixture_case) {
      bi::fixture::Setup given{properties, processes, shared, gives_to, values};
      bi::fixture::apply_given(given, *fixture_case, clock.step, hz, setup);
      bi::reconcile_allocations(values, allocation, setup, clock.step, hz);
      for (const auto &rule : fixture_rule_change.active) {
        auto work = install::catch_up(shared, ids, scripts, terms, rule, {});
        for (auto &item : work)
          step::enqueue_background(queues, std::move(item));
      }
    }
    bi::sample_illuminance(illuminance_sampling, clock.step, hz, setup);
    react::publish_effects(std::move(setup), {}, clock.step, queues, timers, journal,
                           settings.event_queue_max);
    if (!restored)
      bi::start::queue_bootstrap(plan, symbols, terms, queues, settings.event_queue_max);

    bi::ResourceCommit resource_commit{ids, properties, processes, states, terms, vm};
    bi::StructuralRights structural_rights{
        ids,     auth,   poses, declared_parts, owner_limits,
        players, grants, plots, space,          settings.perception.reach_m};
    bi::GeometryInstallation geometry_installation{
        space, geometry, values, poses, physical_flags, physics_roles, physics_defaults};
    bi::CreateOperation create{lifetimes,
                               {models, parameters, assets},
                               geometry_installation,
                               placement,
                               structural_rights,
                               resource_commit,
                               object_lifecycle,
                               credits};
    bi::RemoveOperation remove{lifetimes, structural_rights, resource_commit, object_lifecycle,
                               credits};
    bi::ReplaceOperation replace{lifetimes,
                                 parts,
                                 {models, parameters, assets},
                                 geometry_installation,
                                 poses,
                                 parent,
                                 physical_relations,
                                 relation_changes,
                                 structural_rights,
                                 resource_commit,
                                 object_lifecycle,
                                 credits};
    bi::CatalogPublication publication{models,
                                       scripts,
                                       terms,
                                       materials,
                                       model_text,
                                       instance_text,
                                       values,
                                       auth,
                                       parent,
                                       triggers,
                                       priorities,
                                       editing_changes,
                                       {role("appearance", "red"), role("appearance", "green"),
                                        role("appearance", "blue"), role("appearance", "alpha"),
                                        role("appearance", "metallic"),
                                        role("appearance", "roughness")}};
    bi::ModelRevisionOperation revision{
        revision_read,
        models,
        parameters,
        model_text,
        model_settings,
        baked_measurements,
        {role("measurements", "volume"), role("measurements", "capacity")},
        bi::replacement_edit(replace),
        publication,
        {&revised_goods, bi::commerce::preflight_rebound_offers, bi::commerce::rebind_offers}};
    drive::StructuralOperations structural{
        {{react::StructuralRequest::Kind::Create, {bi::create_instance, &create}},
         {react::StructuralRequest::Kind::Destroy, {bi::remove_instance, &remove}},
         {react::StructuralRequest::Kind::ReplaceBinding, {bi::replace_instance, &replace}},
         {react::StructuralRequest::Kind::ReviseModel, {bi::revise_model, &revision}}}};
    std::vector<FactKey> mutated;
    bi::MassReconciliation mass_reconciliation{masses, values, declared_parts,
                                               runtime_measurements};
    bi::HandlerTurns handlers{execution,
                              structural,
                              triggers,
                              values,
                              {subscriptions, watch_links, scripts, rules},
                              journal,
                              queues,
                              timers,
                              mutated,
                              clock,
                              hz,
                              settings.event_queue_max,
                              allocation,
                              &mass_reconciliation};
    std::vector<bi::GapDraft> gaps;
    bi::WatchTurns watches{handlers, gaps};
    bi::MaterialRefresh material_refresh{space,  geometry, models,        parameters,
                                         assets, values,   physics_roles, physics_defaults};
    bi::RegistryPersistence registry_publication{terms,
                                                 materials,
                                                 rules,
                                                 scripts,
                                                 mechanical,
                                                 registry,
                                                 launch.data / "registry",
                                                 material_refresh,
                                                 clock,
                                                 hz,
                                                 publication};
    const bi::editing::RegistryCommit registry_commit{&registry_publication, bi::commit_registry};
    const bi::editing::EditAccess edit_access{ids, auth, owner_limits, players, grants};
    bi::editing::ScriptEditing editing_scripts{
        installations,  triggers,    subscriptions, watch_links, states,   shared,
        gives_to,       allocations, values,        models,      scripts,  auth,
        owner_limits,   players,     grants,        plots,       profiles, role("events", "start"),
        editing_changes};
    bi::editing::TextEditing editing_text{instance_text, model_text,  authored_text,
                                          models,        edit_access, editing_changes};
    bi::editing::PropertyEditing editing_properties{
        instance_settings,
        model_settings,
        properties,
        processes,
        authored_relations,
        relations,
        values,
        models,
        profiles,
        edit_access,
        {role("relations", "contains"), role("relations", "supports"),
         role("relations", "attached_to"), role("relations", "in_zone")},
        editing_changes};
    bi::editing::RegistryEditing editing_registry{terms,    materials,       values,
                                                  profiles, registry_commit, editing_changes};
    bi::editing::register_actions(native_operations, editing_declarations, editing_scripts,
                                  editing_text, editing_properties, editing_registry);
    const auto native = [&](TermId term, auto validate, auto function, auto &local) {
      drive::register_native(native_operations, term, {validate, function, &local});
    };
    bi::commerce::PaymentBinding commerce_pay{ledger, grants, players, ids, auth};
    bi::commerce::BalanceBinding commerce_balance{ledger, players, ids, auth};
    bi::commerce::InventoryBinding commerce_inventory{players};
    bi::commerce::MailboxDelivery mailbox_delivery{players, auth, commerce_custody};
    bi::commerce::GiveBinding commerce_give{
        players,      grants,           authority(objects), permission_flags(objects),
        owner_limits, commerce_custody, delivery,           settings.mailbox_max};
    bi::commerce::OfferBinding commerce_offer{
        offers, permission_flags(objects), players, grants, auth, owner_limits, commerce_custody};
    bi::commerce::OfferExpiryBinding offer_expiry{offers, ids, permission_flags(objects)};
    bi::commerce::OffersBinding commerce_offers{offers, players, ids, auth};
    bi::commerce::AcceptBinding commerce_accept{offers,
                                                ledger,
                                                grants,
                                                players,
                                                authority(objects),
                                                permission_flags(objects),
                                                owner_limits,
                                                commerce_custody,
                                                delivery,
                                                settings.mailbox_max};
    bi::commerce::TradeOpenBinding commerce_trade_open{trades, players, ids, delivery};
    bi::commerce::TradeChangeBinding commerce_trade_change{
        trades, offers,       permission_flags(objects), players, grants,
        auth,   owner_limits, commerce_custody};
    bi::commerce::TradeConfirmBinding commerce_trade_confirm{trades,
                                                             offers,
                                                             ledger,
                                                             grants,
                                                             players,
                                                             authority(objects),
                                                             permission_flags(objects),
                                                             owner_limits,
                                                             commerce_custody,
                                                             delivery};
    bi::commerce::TradeCancelBinding commerce_trade_cancel{
        trades, offers, permission_flags(objects), commerce_custody};
    bi::commerce::ExistingPlacementBinding commerce_place{
        commerce_custody, poses,         plot_membership(objects),
        players,          grants,        auth,
        owner_limits,     plots,         geometry,
        models,           parameters,    assets,
        values,           physics_roles, physics_defaults,
        allocation};
    native(commerce_declarations.pay, bi::commerce::validate_pay, bi::commerce::pay, commerce_pay);
    native(commerce_declarations.balance, bi::commerce::validate_balance, bi::commerce::balance,
           commerce_balance);
    native(commerce_declarations.inventory, bi::commerce::validate_inventory,
           bi::commerce::inventory, commerce_inventory);
    native(commerce_declarations.give, bi::commerce::validate_give, bi::commerce::give,
           commerce_give);
    native(commerce_declarations.offer, bi::commerce::validate_offer, bi::commerce::offer,
           commerce_offer);
    native(commerce_declarations.offers, bi::commerce::validate_offers, bi::commerce::offers,
           commerce_offers);
    native(commerce_declarations.accept, bi::commerce::validate_accept, bi::commerce::accept,
           commerce_accept);
    native(commerce_declarations.cancel_offer, bi::commerce::validate_cancel_offer,
           bi::commerce::cancel_offer, commerce_offer);
    native(commerce_declarations.trade_open, bi::commerce::validate_trade_open,
           bi::commerce::trade_open, commerce_trade_open);
    native(commerce_declarations.trade_change, bi::commerce::validate_trade_change,
           bi::commerce::trade_change, commerce_trade_change);
    native(commerce_declarations.trade_confirm, bi::commerce::validate_trade_confirm,
           bi::commerce::trade_confirm, commerce_trade_confirm);
    native(commerce_declarations.trade_cancel, bi::commerce::validate_trade_cancel,
           bi::commerce::trade_cancel, commerce_trade_cancel);
    native(commerce_declarations.place_existing, bi::commerce::validate_place_existing,
           bi::commerce::place_existing, commerce_place);
    const bi::commerce::AdminInvalidation admin_invalidation{&permission_changes,
                                                             permissions_changed};
    const bi::authority::ScopeRead scopes{ids, auth, owner_limits, players, plots, profiles};
    bi::commerce::GrantBinding admin_grant{
        grants, players, scopes, models, scripts, installations, rules, values, admin_invalidation};
    bi::commerce::RevokeBinding admin_revoke{grants, players, profiles, admin_invalidation};
    bi::commerce::ListGrantsBinding admin_list_grants{grants, players};
    bi::commerce::GrantMaintenance grant_maintenance{grants, players, admin_invalidation};
    bi::commerce::PresetBinding admin_preset{{objects.group, objects.preset},
                                             permission_flags(objects),
                                             ids,
                                             auth,
                                             owner_limits,
                                             players,
                                             groups,
                                             admin_invalidation};
    bi::commerce::GroupCreateBinding admin_group_create{groups, players, admin_invalidation};
    bi::commerce::GroupMemberBinding admin_group_member{groups, players, admin_invalidation};
    bi::commerce::PlotPermissionsBinding admin_plot{plots, players, groups, admin_invalidation};
    native(admin_declarations.grant, bi::commerce::validate_grant, bi::commerce::grant,
           admin_grant);
    native(admin_declarations.revoke, bi::commerce::validate_revoke, bi::commerce::revoke,
           admin_revoke);
    native(admin_declarations.list_grants, bi::commerce::validate_list_grants,
           bi::commerce::list_grants, admin_list_grants);
    native(admin_declarations.preset, bi::commerce::validate_preset, bi::commerce::preset,
           admin_preset);
    native(admin_declarations.group_create, bi::commerce::validate_group_create,
           bi::commerce::group_create, admin_group_create);
    native(admin_declarations.group_member, bi::commerce::validate_group_member,
           bi::commerce::group_member, admin_group_member);
    native(admin_declarations.plot_permissions, bi::commerce::validate_plot_permissions,
           bi::commerce::plot_permissions, admin_plot);
    bi::invalidate_travel(travel_access, grants, clock.step);
    bi::ActionTurns actions{handlers,
                            native_declarations,
                            native_operations,
                            models,
                            scripts,
                            installations,
                            rules,
                            profiles,
                            auth,
                            poses,
                            owner_limits,
                            players,
                            grants,
                            space,
                            physical_relations.attached_to,
                            intents,
                            gaps,
                            settings.perception.reach_m};

    net::Server server({"127.0.0.1", launch.port, launch.client, launch.assets, settings.network,
                        settings.control_max});
    ViewTransport transport{server, replication, session_views, ids,
                            auth,   parent,      model_text,    instance_text};
    Welcome welcome_fields{clock, hz, settings.network.cell_m, settings.repl_delay_ms};
    control::Sessions sessions(profiles, {&transport, send, close, view},
                               {&welcome_fields, welcome});
    elaborate::FixtureWorker fixture_worker(std::filesystem::canonical("/proc/self/exe"),
                                            launch.data / "fixtures", settings.fixture_workers);
    bi::RuleActivation rule_activation{
        scripts,     rules,
        triggers,    subscriptions,
        watch_links, queues,
        vm,          values,
        allocations, {&registry_publication, bi::publish_rule_registry}};
    const bi::elaboration::RuleActivation activation{&rule_activation, activate_rule};
    ElaborationNotifications elaboration_notifications{sessions, requests, editing_changes};
    const bi::elaboration::Notification notify{&elaboration_notifications,
                                               elaboration_notification};
    bi::elaboration::RequestBinding elaboration_request{requests, last_gaps};
    bi::elaboration::RequestsBinding elaboration_requests{requests, gap_counts, profiles, 20};
    bi::elaboration::ClaimBinding elaboration_claim{requests, profiles};
    bi::elaboration::PatchBinding elaboration_patch{requests, profiles, notify};
    bi::elaboration::DetailBinding elaboration_detail{requests, authored_text, profiles, notify};
    bi::elaboration::RuleBinding elaboration_rule{
        requests, fixtures,       captures,        scripts,
        profiles, fixture_worker, "ambergris-0.1", settings.fixture_limits};
    bi::elaboration::AnswerDispatch elaboration_answer{
        {bi::elaboration::validate_answer_rule, bi::elaboration::answer_rule, &elaboration_rule},
        {bi::elaboration::validate_answer_patch, bi::elaboration::answer_patch, &elaboration_patch},
        {bi::elaboration::validate_answer_detail, bi::elaboration::answer_detail,
         &elaboration_detail}};
    bi::elaboration::ActivationBinding elaboration_activation{scripts, profiles, activation};
    bi::elaboration::CompletionBinding elaboration_completion{requests, fixtures, inputs,
                                                              scripts,  profiles, activation};
    native(elaboration_declarations.request_model, bi::elaboration::validate_request_model,
           bi::elaboration::request_model, elaboration_request);
    native(elaboration_declarations.requests, bi::elaboration::validate_requests,
           bi::elaboration::requests, elaboration_requests);
    native(elaboration_declarations.claim, bi::elaboration::validate_claim, bi::elaboration::claim,
           elaboration_claim);
    native(elaboration_declarations.answer, bi::elaboration::validate_answer,
           bi::elaboration::answer, elaboration_answer);
    native(elaboration_declarations.refuse, bi::elaboration::validate_refuse,
           bi::elaboration::refuse, elaboration_detail);
    native(elaboration_declarations.rule_activation, bi::elaboration::validate_rule_activation,
           bi::elaboration::rule_activation, elaboration_activation);
    for (const auto &[id, job] : fixtures.rows) {
      (void)id;
      fixture_worker.submit(job);
    }
    ObserverInterest observed_interest{sessions, transport};
    drive::InputOperations input_operations;
    bi::QueuedControl queued{inputs, queues};
    bi::InputDelivery input_delivery{inputs, input_operations, sessions};
    bi::DoInput do_input{actions, hearing, settings.perception.events_per_response};
    bi::CancelInput cancel_input{intents, intent_endings, journal,
                                 hearing, clock,          settings.perception.events_per_response};
    bi::authoring::Worker authoring({launch.assets, settings.pins, settings.scripts});
    bi::authoring::SubmissionQueue authoring_input{authoring, terms, registry};
    bi::authoring::FailedAuthoring failed_authoring{journal, clock};
    ImportGeometry geometry_import{geometry, space, launch.assets};
    bi::authoring::CatalogInstall catalog_install{
        assets,
        models,
        parameters,
        model_text,
        model_settings,
        baked_measurements,
        sources,
        scripts,
        terms,
        {role("measurements", "volume"), role("measurements", "capacity")},
        {&geometry_import, import_geometry}};
    bi::authoring::ScriptInstall script_install{scripts, vm, sources, native_declarations,
                                                launch.data / "scripts"};
    drive::register_input(input_operations, "do", {bi::do_input, &do_input});
    drive::register_input(input_operations, "cancel", {bi::cancel_input, &cancel_input});
    bi::authoring::register_inputs(input_operations, authoring_input);
    drive::register_input(input_operations, "authoring_failed",
                          {bi::authoring::failed_authoring, &failed_authoring});
    const auto play = control::modes(control::Mode::Play);
    sessions.register_operation({"do", "result", play, control::Lane::Mutation,
                                 control::validate_do, bi::queue_control, &queued});
    sessions.register_operation({"cancel", "result", play, control::Lane::Mutation,
                                 control::validate_cancel, bi::queue_control, &queued});
    sessions.register_operation({"register_model", "result", play, control::Lane::Mutation,
                                 control::validate_register_model, bi::queue_control, &queued});
    sessions.register_operation({"register_variant", "result", play, control::Lane::Mutation,
                                 control::validate_register_variant, bi::queue_control, &queued});
    sessions.register_operation({"register_script", "result", play, control::Lane::Mutation,
                                 control::validate_register_script, bi::queue_control, &queued});
    sessions.register_operation({"authoring_failed", "result", play, control::Lane::Mutation,
                                 control::validate_authoring_failed, bi::queue_control, &queued});
    bi::viewing::CatalogRead catalog_read{models,        parameters,
                                          assets,        scripts,
                                          model_text,    instance_text,
                                          installations, native_declarations,
                                          values,        publication.inspection_terms(),
                                          authored_text};
    std::vector<observe::MotionSample> motion;
    SparseColumn<std::pair<Handle, Vec3>> previous_positions;
    for (auto object : initial)
      previous_positions.set(object.slot(), std::pair{object, poses.position[object.slot()]});
    ObserveRecord observation_record{journal, clock, credits, hz};
    bi::viewing::LookBinding look{session_views,
                                  active_views,
                                  hearing,
                                  space,
                                  poses,
                                  declared_parts,
                                  physical_relations.attached_to,
                                  catalog_read,
                                  lights,
                                  zones,
                                  motion,
                                  intents,
                                  settings.perception,
                                  clock.step,
                                  hz,
                                  0,
                                  settings.scripts.handler_ms,
                                  {&observed_interest, observer_interest},
                                  {&observation_record, record_observation}};
    bi::viewing::InspectBinding inspect{session_views,
                                        hearing,
                                        catalog_read,
                                        poses,
                                        auth,
                                        parent,
                                        attribution(objects),
                                        relations,
                                        owner_limits,
                                        players,
                                        grants,
                                        plots,
                                        groups,
                                        ledger,
                                        intents,
                                        sources,
                                        clock.step,
                                        hz,
                                        settings.perception.events_per_response,
                                        states,
                                        rules};
    bi::viewing::ActionsBinding discovery{
        session_views, hearing,      catalog_read, 100,    settings.perception.events_per_response,
        auth,          owner_limits, players,      grants, clock.step};
    bi::viewing::RecordFeed feed{{}, 4096};
    bi::viewing::EventsBinding events{feed, session_views, settings.perception.sight_m,
                                      settings.perception.events_per_response};
    bi::viewing::DebugBinding debug{
        space, ids, clock.step, settings.scripts.handler_ms, settings.perception.sight_m, {}};
    sessions.register_operation({"look", "obs", control::all_modes, control::Lane::Query,
                                 control::validate_look, bi::viewing::look_handler, &look});
    sessions.register_operation({"inspect", "inspection", control::all_modes, control::Lane::Query,
                                 control::validate_inspect, bi::viewing::inspect_handler,
                                 &inspect});
    sessions.register_operation({"actions", "actions", control::all_modes, control::Lane::Query,
                                 control::validate_actions, bi::viewing::actions_handler,
                                 &discovery});
    sessions.register_operation({"events", "events", control::all_modes, control::Lane::Query,
                                 control::validate_events, bi::viewing::events_handler, &events});
    sessions.register_operation({"debug", "debug", control::modes(control::Mode::Debug),
                                 control::Lane::Query, control::validate_debug,
                                 bi::viewing::debug_handler, &debug});
    std::vector<space::MotionRow> snapshot_motion;
    std::vector<bi::BodyCauseRow> snapshot_causes;
    CheckpointFilter checkpoint_filter;
    const snap::SectionWriters snapshot_sections{
        [&objects, &checkpoint_filter](snap::Output &out) {
          snap::write_objects(out, objects, checkpoint_filter.handles());
        },
        [&properties, &processes, &instance_settings, &model_settings, &plot_settings,
         &world_settings, &checkpoint_filter](snap::Output &out) {
          bi::checkpoint::write_prop(out, properties, processes, instance_settings, model_settings,
                                     plot_settings, world_settings, checkpoint_filter.objects,
                                     checkpoint_filter.catalog ? &*checkpoint_filter.catalog
                                                               : nullptr,
                                     checkpoint_filter.land());
        },
        [&authored_relations, &physical_relations, &parts, ids,
         &checkpoint_filter](snap::Output &out) {
          bi::checkpoint::write_rels(out, authored_relations, physical_relations, parts, ids,
                                     checkpoint_filter.handles());
        },
        [&shared, &gives_to, &allocations, &meters, &checkpoint_filter](snap::Output &out) {
          react::write_shared(out, shared, gives_to, allocations, meters,
                              checkpoint_filter.objects);
        },
        [&models, &parameters, &scripts, &model_text, &instance_text, &authored_text,
         &checkpoint_filter](snap::Output &out) {
          bi::checkpoint::write_text(
              out, models, parameters, scripts, model_text, instance_text, authored_text,
              checkpoint_filter.catalog ? &*checkpoint_filter.catalog : nullptr,
              checkpoint_filter.handles());
        },
        [&players, &ledger, &plots, &groups, &profiles, &checkpoint_filter](snap::Output &out) {
          bi::checkpoint::write_players(out, players, ledger, plots, groups, profiles,
                                        checkpoint_filter.authority ? &*checkpoint_filter.authority
                                                                    : nullptr);
        },
        [&grants, &offers, &trades, &owner_limits, &checkpoint_filter](snap::Output &out) {
          perm::write_grants(out, grants, offers, trades, owner_limits,
                             checkpoint_filter.authority ? &*checkpoint_filter.authority : nullptr);
        },
        [&states, &subscriptions, &installations, ids, &checkpoint_filter](snap::Output &out) {
          bi::checkpoint::write_scst(out, states, subscriptions, installations, ids,
                                     checkpoint_filter.handles());
        },
        [&timers, &checkpoint_filter](snap::Output &out) {
          step::write_timers(out, timers, checkpoint_filter.objects);
        },
        [&queues, &intents, &journal, &inputs, &checkpoint_filter](snap::Output &out) {
          bi::checkpoint::write_queues(out, queues, intents, journal.next_id(), inputs,
                                       checkpoint_filter.objects);
        },
        [&requests, &gap_counts, &last_gaps, &captures, &fixtures,
         &checkpoint_filter](snap::Output &out) {
          bi::checkpoint::write_reqs(out, requests, gap_counts, last_gaps, captures, fixtures,
                                     checkpoint_filter.handles());
        },
        [&random, &checkpoint_filter](snap::Output &out) {
          react::write_random(out, random, checkpoint_filter.objects);
        },
        [&snapshot_motion, &snapshot_causes, &checkpoint_filter](snap::Output &out) {
          space::write_motion(out, snapshot_motion, checkpoint_filter.objects);
          bi::write_motion_causes(out, snapshot_causes, checkpoint_filter.objects);
        }};
    elaborate::CaptureWriter capture_writer(launch.data / "captures", settings.snapshot_zstd);
    bi::checkpoint::Encoding checkpoint_encoding{
        snapshot_sections, checkpoint_filter, snapshot_motion, snapshot_causes, space, ids,
        mechanical_causes};
    bi::checkpoint::GapCaptures gap_captures{
        {values, parent, poses, auth, parts, models, parameters, installations, rules, players,
         plots, groups, grants, offers, trades, session_views, profiles},
        gaps,
        gap_counts,
        last_gaps,
        requests,
        captures,
        journal,
        capture_writer,
        checkpoint_encoding,
        settings.gap_captures,
        settings.capture_m};
    snap::Writer snapshot_writer(launch.data / "snapshots", settings.snapshot_keep,
                                 settings.snapshot_zstd);
    const auto snapshot_every = static_cast<Step>(std::llround(settings.snapshot_seconds * hz));
    bi::checkpoint::SnapshotProgress snapshot_progress;
    if (launch.network) {
      const auto error = server.start();
      if (!error.empty())
        throw std::runtime_error(error);
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::cout << Json{{"status", "running"},
                      {"objects", initial.size()},
                      {"port", launch.network ? Json(launch.port) : Json(nullptr)},
                      {"step_hz", hz}}
                     .dump()
              << std::endl;
    drive::PhaseCosts costs(settings.clock.average_steps);
    const auto started = drive::Monotonic::now();
    const auto final_step = fixture_case   ? clock.step + bi::fixture::steps(*fixture_case)
                            : launch.steps ? clock.step + launch.steps
                                           : std::numeric_limits<Step>::max();
    if (fixture_case && fixture_case->test.at("when").contains("action"))
      (void)bi::fixture::perform_when(actions, *fixture_case);
    std::deque<net::Input> incoming;
    std::deque<bi::authoring::Completion> authored;
    std::deque<elaborate::FixtureResult> fixture_results;
    const auto remove_session = [&replication, &session_views, &active_views, &hearing, &listeners,
                                 &delivery, &sessions](net::SessionId session) {
      if (const auto found = session_views.rows.find(session); found != session_views.rows.end()) {
        const auto player = found->second.player;
        bool present = false;
        for (auto other : sessions.sessions_for(player))
          if (const auto principal = sessions.principal(other);
              principal && principal->mode == control::Mode::Play)
            present = true;
        if (!present)
          delivery.present_bodies.erase(player);
      }
      std::erase_if(listeners,
                    [session](const auto &listener) { return listener.session == session; });
      bi::remove_view_interest(replication, session);
      bi::viewing::remove_session(session_views, active_views, hearing, session);
    };
    std::size_t script_errors = 0;
    std::vector<double> phase_ms;
    constexpr std::size_t phase_samples_max = 4096;
    std::size_t phase_cursor = 0;
    while (!stopping && clock.step < final_step) {
      FrameMarkStart("step work");
      for (const auto &completed : capture_writer.take())
        elaborate::complete_capture(captures, completed);
      auto frame = drive::begin_frame(clock, settings.clock, costs, queues, timers, {},
                                      drive::Monotonic::now());
      auto grant_updates = bi::commerce::advance_grants(
          grant_maintenance, clock.step,
          static_cast<Step>(std::llround(settings.grant_reminder_seconds * hz)), hz,
          settings.deadlines_per_step);
      for (auto &notice : grant_updates.notices)
        sessions.event_for_player(notice.recipient, std::move(notice.event));
      for (auto &event : grant_updates.records)
        journal.append(std::move(event));
      for (const auto &expired : bi::commerce::advance_offer_expiry(offer_expiry, clock.step,
                                                                    settings.deadlines_per_step)) {
        bi::invalidate_metadata(editing_changes, expired.cleared);
        Json notice{{"kind", "offer_expired"},
                    {"offer", "offer:" + std::to_string(expired.offer)},
                    {"goods", to_string(expired.goods)},
                    {"say", "The sale offer expired; its goods are available again."}};
        sessions.event_for_player(expired.seller, notice);
        record::Event event;
        event.step = clock.step;
        event.kind = record::Kind::System;
        event.name = "offer_expired";
        event.changed = true;
        event.result = record::Result::Applied;
        event.things = expired.cleared;
        event.detail = std::move(notice);
        event.detail.erase("kind");
        event.detail["agent"] = false;
        journal.append(std::move(event));
      }
      drive::drain_foreground(queues, frame, {&handlers, bi::consume_event},
                              {&watches, bi::consume_watch}, {&actions, bi::consume_action},
                              {&input_delivery, bi::consume_input});
      publication.flush(sessions);
      bi::checkpoint::flush_gaps(gap_captures,
                                 {"ambergris-0.1", clock.step, hash_bytes(registry.hash)});
      for (auto &result : fixture_worker.take())
        fixture_results.push_back(std::move(result));
      while (!fixture_results.empty() && drive::admits(frame, drive::Monotonic::now())) {
        auto result = std::move(fixture_results.front());
        fixture_results.pop_front();
        auto completed = bi::elaboration::complete(elaboration_completion, result, clock.step, hz);
        bi::reconcile_effects(handlers, completed.effects, completed.origin);
        react::publish_effects(std::move(completed.effects), completed.origin, clock.step, queues,
                               timers, journal, settings.event_queue_max);
        if (completed.notification)
          elaboration_notification(&elaboration_notifications, *completed.notification);
        if (completed.input && completed.input->session)
          sessions.complete(completed.input->session, completed.input->sequence,
                            std::move(completed.input->fields));
      }
      for (auto &completion : authoring.poll())
        authored.push_back(std::move(completion));
      while (!authored.empty() && drive::admits(frame, drive::Monotonic::now())) {
        auto completion = std::move(authored.front());
        authored.pop_front();
        const auto accepted = inputs.rows.find(completion.input_id);
        if (accepted == inputs.rows.end())
          continue;
        const auto input = accepted->second;
        auto completed = bi::authoring::complete_authoring(
            input, std::move(completion),
            !input.session || sessions.principal(input.session).has_value(), catalog_install,
            script_install, journal, clock.step, hz, seed_authors);
        if (!completed.added_model_ids.empty()) {
          publication.definitions_changed(completed.added_model_ids);
          publication.flush(sessions);
        }
        const auto delivered = drive::complete_input(inputs, input.id, std::move(completed.reply));
        sessions.complete(delivered.session, delivered.sequence, delivered.fields);
      }
      std::vector<net::Input> network_inputs;
      if (launch.network)
        server.drain(network_inputs);
      for (auto &input : network_inputs)
        incoming.push_back(std::move(input));
      while (!incoming.empty() && drive::admits(frame, drive::Monotonic::now())) {
        auto input = std::move(incoming.front());
        incoming.pop_front();
        std::optional<net::SessionId> authenticated;
        if (const auto *control = std::get_if<net::Control>(&input);
            control && !sessions.principal(control->session))
          authenticated = control->session;
        sessions.accept(input);
        if (authenticated && sessions.principal(*authenticated)) {
          sessions.send(*authenticated, publication.initial_catalog());
          const auto principal = sessions.principal(*authenticated).value();
          auto &row = session_views.rows[*authenticated];
          row.player = principal.player;
          row.body = principal.body;
          if (ids.valid(principal.body))
            row.eye = poses.position[principal.body.slot()];
          if (principal.mode == control::Mode::Play && ids.valid(principal.body)) {
            delivery.present_bodies[principal.player] = principal.body;
            react::DeferredEffects effects;
            bi::commerce::deliver_mailbox(mailbox_delivery, principal.player, principal.body,
                                          clock.step, {}, effects);
            bi::reconcile_effects(handlers, effects, {});
            react::publish_effects(std::move(effects), {}, clock.step, queues, timers, journal,
                                   settings.event_queue_max);
          }
          if (ids.valid(principal.body))
            listeners.push_back({*authenticated, principal.body, row.eye});
          std::vector<net::SessionId> displaced;
          for (const auto &[session, view] : session_views.rows) {
            (void)view;
            if (!sessions.principal(session))
              displaced.push_back(session);
          }
          for (auto session : displaced)
            remove_session(session);
        }
        if (const auto *disconnect = std::get_if<net::Disconnected>(&input))
          remove_session(disconnect->session);
        else if (const auto *control = std::get_if<net::Control>(&input);
                 control && !sessions.principal(control->session))
          remove_session(control->session);
        drive::drain_foreground(queues, frame, {&handlers, bi::consume_event},
                                {&watches, bi::consume_watch}, {&actions, bi::consume_action},
                                {&input_delivery, bi::consume_input});
        publication.flush(sessions);
        bi::checkpoint::flush_gaps(gap_captures,
                                   {"ambergris-0.1", clock.step, hash_bytes(registry.hash)});
      }
      drive::drain_foreground(queues, frame, {&handlers, bi::consume_event},
                              {&watches, bi::consume_watch}, {&actions, bi::consume_action},
                              {&input_delivery, bi::consume_input});
      publication.flush(sessions);
      bi::checkpoint::flush_gaps(gap_captures,
                                 {"ambergris-0.1", clock.step, hash_bytes(registry.hash)});
      drive::drain_background(queues, frame, {&handlers, bi::consume_background},
                              {&handlers, bi::consume_event}, {&watches, bi::consume_watch},
                              {&actions, bi::consume_action}, {&input_delivery, bi::consume_input});
      publication.flush(sessions);
      bi::checkpoint::flush_gaps(gap_captures,
                                 {"ambergris-0.1", clock.step, hash_bytes(registry.hash)});
      const auto physics_start = drive::Monotonic::now();
      const auto installed_tiles = space::install_ready_navigation(space);
      for (auto &notice : bi::publish_navigation(
               navigation_diagnostics, installed_tiles,
               drive::Monotonic::now() +
                   std::chrono::microseconds(settings.navigation_diagnostic_budget_us)))
        sessions.event_for_player(notice.author, std::move(notice.event));
      for (const auto &error : installed_tiles.errors)
        std::cerr << "navigation: " << error << '\n';
      std::vector<space::MoveRequest> moving;
      moving.reserve(intents.moving.size());
      for (const auto &[bits, intent] : intents.moving) {
        space::Goal goal;
        if (const auto *point = std::get_if<Vec3>(&intent.goal))
          goal = space::PointGoal{*point, settings.goal_tolerance_m};
        else
          goal = space::ObjectGoal{std::get<Handle>(intent.goal), settings.perception.reach_m};
        moving.push_back({Handle{bits}, intent.id, std::move(goal),
                          bi::travel_boundaries(travel_access, ids, auth, Handle{bits}, plots,
                                                players, grants, clock.step),
                          intent.speed_m_s, mechanical_causes.intern(intent.origin)});
      }
      std::erase_if(travel_access.bodies,
                    [&](const auto &row) { return !intents.moving.contains(row.first); });
      space::StepResult physics;
      space::step(space, ids, poses, parent, physical_flags, physical_relations, clock.step,
                  1.0f / hz, moving, physics);
      physics.relations.insert(physics.relations.end(), relation_changes.begin(),
                               relation_changes.end());
      relation_changes.clear();
      bi::publish_physical(physical_publication, physics, clock.step);
      bi::collect_physical_causes(mechanical_causes, space);
      for (const auto &progress : physics.intents)
        if (progress.state == space::IntentState::Arrived ||
            progress.state == space::IntentState::Blocked ||
            progress.state == space::IntentState::TargetGone) {
          if (auto ending = step::finish_move(intents, progress.body,
                                              progress.state == space::IntentState::Arrived
                                                  ? step::IntentEnd::Arrived
                                                  : step::IntentEnd::Blocked))
            intent_endings.push_back(std::move(*ending));
        }
      for (const auto &ending : intent_endings) {
        const auto event = ending.reason == step::IntentEnd::Arrived ? role("events", "arrived")
                           : ending.reason == step::IntentEnd::Blocked
                               ? role("events", "blocked")
                               : role("events", "cancelled");
        step::enqueue_event(queues,
                            {ending.body,
                             {},
                             event,
                             std::vector<EventField>{
                                 {"intent", Value(static_cast<std::int64_t>(ending.intent.id))},
                                 {"reason", std::string(ending.reason == step::IntentEnd::Replaced
                                                            ? "replaced"
                                                            : terms.rows[event].name)}},
                             ending.intent.origin},
                            settings.event_queue_max);
        if (ids.valid(ending.body))
          sessions.event_for_player(
              auth.owner[ending.body.slot()],
              {{"kind", ending.reason == step::IntentEnd::Arrived   ? "arrived"
                        : ending.reason == step::IntentEnd::Blocked ? "blocked"
                                                                    : "cancelled"},
               {"body", to_string(ending.body)},
               {"intent", ending.intent.id},
               {"reason",
                ending.reason == step::IntentEnd::Replaced ? "replaced" : terms.rows[event].name}});
      }
      intent_endings.clear();
      motion.clear();
      for (auto object : physics.moved) {
        const auto position = poses.position[object.slot()];
        const auto *previous = previous_positions.get(object.slot());
        auto velocity =
            previous && previous->first == object ? (position - previous->second) * hz : Vec3{};
        if (const auto native = space::motion(space, ids, object);
            native && native->linear_velocity_m_s.length() > velocity.length())
          velocity = native->linear_velocity_m_s;
        motion.push_back({object, velocity});
        previous_positions.set(object.slot(), std::pair{object, position});
      }
      bi::scene_moved(scene_projection, physics.moved);
      bi::scene_values_changed(scene_projection, space, active_views, mutated);
      react::DeferredEffects light_effects;
      bi::advance_scene(scene_projection, illuminance_sampling, active_views, physics.dirty_bounds,
                        clock.step, hz, light_effects);
      const auto prior_mutations = mutated.size();
      bi::reconcile_effects(handlers, light_effects, {});
      bi::scene_values_changed(scene_projection, space, active_views,
                               std::span(mutated).subspan(prior_mutations));
      react::publish_effects(std::move(light_effects), {}, clock.step, queues, timers, journal,
                             settings.event_queue_max);
      mutated.clear();
      listeners.clear();
      delivery.present_bodies.clear();
      for (auto &[session, row] : session_views.rows)
        if (ids.valid(row.body)) {
          const auto effective = catalog::resolve_asset(
              models, parameters, ids.model[row.body.slot()], objects.params[row.body.slot()]);
          if (const auto binding = bi::observer_binding(assets.rows.at(effective.hash), terms))
            row.eye = bi::observer(*binding, row.body, poses, values, clock.step, hz).eye;
          listeners.push_back({session, row.body, row.eye});
          if (const auto principal = sessions.principal(session);
              principal && principal->mode == control::Mode::Play)
            delivery.present_bodies[row.player] = row.body;
        }
      for (auto session : observe::take_dirty_views(active_views))
        if (const auto principal = sessions.principal(session))
          bi::viewing::refresh_view(look, session, *principal, settings.perception.things_max);
      if (launch.network && clock.step % settings.repl_every == 0)
        server.publish(bi::publication(replication, ids, poses, values, clock.step, hz));
      auto records = journal.take();
      for (const auto &event : records)
        if (event.value("status", std::string{}) == "error" ||
            event.value("status", std::string{}) == "cutoff")
          ++script_errors;
      bi::viewing::append_records(feed, records, ids, poses);
      writer.submit(std::move(records));
      if ((snapshot_every && clock.step % snapshot_every == 0) ||
          (launch.snapshot_at_end && clock.step == final_step))
        (void)bi::checkpoint::save_snapshot(
            snapshot_writer, checkpoint_encoding, snapshot_progress,
            {"ambergris-0.1", clock.step, hash_bytes(registry.hash)});
      const auto now = drive::Monotonic::now();
      const auto duration = std::chrono::duration<double, std::milli>(now - frame.started).count();
      if (phase_ms.size() < phase_samples_max)
        phase_ms.push_back(duration);
      else {
        phase_ms[phase_cursor] = duration;
        phase_cursor = (phase_cursor + 1) % phase_samples_max;
      }
      const auto next = drive::finish_frame(
          clock, costs, frame,
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
      while (
          !bi::checkpoint::save_snapshot(snapshot_writer, checkpoint_encoding, snapshot_progress,
                                         {"ambergris-0.1", clock.step, hash_bytes(registry.hash)}))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    snapshot_writer.finish();
    capture_writer.finish();
    for (const auto &completed : capture_writer.take())
      elaborate::complete_capture(captures, completed);
    writer.submit(journal.take());
    writer.finish();
    const auto elapsed = std::chrono::duration<double>(drive::Monotonic::now() - started).count();
    if (fixture_case) {
      auto result = bi::fixture::observe(values, *fixture_case, clock.step, hz);
      if (script_errors) {
        result.passed = false;
        result.error = "fixture handlers reported execution errors";
      }
      std::ofstream output(launch.report);
      output << Json{{"version", 1},
                     {"passed", result.passed},
                     {"observed", result.observed},
                     {"error", result.error}}
                    .dump(2)
             << '\n';
      if (!output)
        throw std::runtime_error("cannot write fixture report");
    } else if (!launch.report.empty()) {
      Json rows = Json::array();
      for (std::uint32_t slot = 1; slot < ids.model.size(); ++slot) {
        const auto h = ids.at(slot);
        if (!ids.valid(h))
          continue;
        Json quantities = Json::object();
        for (const auto term : publication.inspection_terms()) {
          const auto answer = react::read(values, h, term, clock.step, hz);
          if (answer.present && answer.value.is_numeric())
            quantities[terms.rows[term].name] = answer.value.numeric();
        }
        rows.push_back(
            {{"handle", to_string(h)},
             {"model", ids.model[slot]},
             {"position", {poses.position[slot].x, poses.position[slot].y, poses.position[slot].z}},
             {"values", std::move(quantities)}});
      }
      std::ofstream output(launch.report);
      output << Json{{"step", clock.step},
                     {"objects", std::move(rows)},
                     {"script_errors", script_errors},
                     {"timers", step::timers(timers).size()},
                     {"allocations", allocations.values.size()}}
                    .dump(2)
             << '\n';
      if (!output)
        throw std::runtime_error("cannot write state report");
    }
    std::sort(phase_ms.begin(), phase_ms.end());
    const auto percentile = [&](double p) {
      return phase_ms.empty() ? 0 : phase_ms[static_cast<std::size_t>((phase_ms.size() - 1) * p)];
    };
    std::cout << Json{{"status", "stopped"},
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
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
