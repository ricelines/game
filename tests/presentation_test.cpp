#include "bindings/illumination.hpp"
#include "bindings/metadata/metadata.hpp"
#include "bindings/presentation.hpp"
#include "bindings/replication.hpp"
#include "bindings/viewing/viewing.hpp"
#include "control/control.hpp"
#include "space_fixture.hpp"

#include <chrono>
#include <iostream>
#include <numbers>

using namespace amber;
namespace bi = amber::bindings;
namespace re = amber::react;
namespace ca = amber::catalog;
namespace ne = amber::net;
using testspace::require;
struct Fixture {
  Objects objects;
  ca::Models models;
  ca::ParameterBlocks parameters;
  ca::Assets assets;
  ca::ModelTexts texts;
  ca::InstanceTexts instance_texts;
  re::Terms terms;
  re::Materials materials;
  re::AssetMeasurements measurements;
  re::Properties properties;
  re::Processes processes;
  re::InstanceSettings instance;
  re::ModelSettings model_settings;
  re::PlotSettings plots;
  re::WorldSettings world;
  re::SharedColumns shared;
  re::RelationsRead relations;
  re::RuntimeMeasurements runtime;
  std::string first = ca::sha256("base"), variant = ca::sha256("variant");
  Fixture() {
    terms.rows.resize(10);
    properties.resize(10);
    processes.resize(10);
    for (TermId id = 1; id < 10; ++id) {
      terms.rows[id].id = id;
      terms.rows[id].name = "invented.q" + std::to_string(id);
      terms.by_name[terms.rows[id].name] = id;
    }
    terms.rows[3].kind = re::Kind::Material;
    terms.rows[4].source = re::Source::Intrinsic;
    terms.rows[5].dimensions = {2, 1, -3, 0, 0, 0, 0, 0};
    terms.rows[6].source = re::Source::Intrinsic;
    terms.rows[6].dimensions = {-3, 1, 0, 0, 0, 0, 0, 0};
    materials.rows[3][4] = Value(.25);
    materials.rows[3][6] = Value(7850.);
    ca::Asset a;
    a.hash = first;
    a.minimum = {0, 0, 0};
    a.maximum = {1, 2, 3};
    a.volume = .25;
    a.capacity = .1;
    a.canonical_params = "{\"height\":3}";
    a.replicated = {1};
    a.manifest = {{"params", {{"height", 3}}}, {"bindings", nlohmann::json::object()}};
    assets.rows[first] = a;
    a.hash = variant;
    a.maximum.z = 5;
    a.volume = .5;
    a.canonical_params = "{\"height\":5}";
    a.manifest["params"]["height"] = 5;
    a.manifest["bindings"]["face_motion"] = true;
    assets.rows[variant] = a;
    models.rows.resize(2);
    models.rows[1].id = 1;
    models.rows[1].asset = first;
    models.rows[1].material = 3;
    texts.rows[1] = {"model claim", "fiction", "label"};
    parameters.rows.push_back({1, 1, variant, a.canonical_params, {}});
  }
  Handle make(Vec3 at = {}, std::uint32_t params = 0) {
    ObjectInit init;
    init.model = 1;
    init.material = 3;
    init.owner = 9;
    init.position = at;
    init.params = params;
    return create(objects, init);
  }
  re::ValueRead values() {
    return {terms,     identity(objects), objects.plot, objects.params,
            materials, measurements,      properties,   processes,
            instance,  model_settings,    plots,        world,
            shared,    relations,         runtime,      6};
  }
  bi::ReplicationIndex index() { return bi::ReplicationIndex({32, 2, 3, 65536, 1048576}); }
  void add(bi::ReplicationIndex &index, Handle h) {
    bi::register_replication(index, h, identity(objects), transforms(objects), models, parameters,
                             assets, values());
  }
  ne::Publication publish(bi::ReplicationIndex &index, Step now) {
    return bi::publication(index, identity(objects), transforms(objects), values(), now, 20);
  }
};
void descriptions_and_roles() {
  Fixture f;
  auto h = f.make({}, 1);
  f.properties[1].set(h.slot(), Value(7.));
  f.properties[2].set(h.slot(), Value(99.));
  f.instance_texts.rows[h.bits] = {"", "instance fiction", ""};
  const std::array<TermId, 2> inspected{1, 2};
  const std::array<std::string, 1> actions{"invented.poke"};
  auto description =
      bi::describe_object(h, identity(f.objects), f.models, f.parameters, f.assets, f.texts,
                          f.instance_texts, f.values(), inspected, actions, 0, 20);
  require(description.measured["size_m"][2] == 5 && description.measured["params"]["height"] == 5,
          "effective parameter asset supplies actual geometry measurements");
  require(description.measured["mass_kg"] == 3925 && description.measured["capacity_l"] == 100 &&
              description.measured["material"] == "invented.q3" &&
              !description.measured.contains("capacity_m3"),
          "S5 mass, capacity units and material name come from the actual variant");
  f.materials.rows[3][6] = Value(1000.);
  require(bi::describe_object(h, identity(f.objects), f.models, f.parameters, f.assets, f.texts,
                              f.instance_texts, f.values(), inspected, actions, 0, 20)
                  .measured["mass_kg"] == 500,
          "material edits must change measured mass without rebaking");
  require(description.measured["values"]["invented.q1"] == 7 && description.says.name.empty() &&
              description.says.description == "instance fiction",
          "measured truth and intentional empty claims remain separate");
  auto objects = bi::object_metadata(identity(f.objects), authority(f.objects), parents(f.objects),
                                     f.texts, f.instance_texts, std::span(&h, 1));
  require(objects[0]["owner"] == "p:9" && objects[0]["material"] == 3 &&
              objects[0]["parent"].is_null(),
          "metadata preserves material and owner independently of asset hash");
  const std::array<ModelId, 1> model{1};
  require(bi::model_metadata(f.models, f.texts, model)[0]["bindings"].empty(),
          "default model cannot override variant manifest bindings");
  const std::array<TermId, 1> term{5}, material{3};
  require(bi::term_metadata(f.terms, term)[0]["unit"] == "m^2*kg*s^-3", "SI term units");
  auto appearance = bi::material_metadata(f.terms, f.materials, {4, 0, 0, 0, 0, 0}, material);
  require(appearance[0]["color"][0] == .25 && appearance[0]["roughness"] == .8,
          "material appearance uses explicit intrinsic roles");
  auto &asset = f.assets.rows.at(f.variant);
  asset.manifest["bindings"]["light"] = {
      {"intensity", "invented.q1"}, {"site", {0, 1, 2}}, {"range", 8}};
  asset.manifest["bindings"]["zone"] = {
      {"ambient", 23},
      {"priority", 4},
      {"shape", {{"kind", "box"}, {"size", {4, 6, 2}}, {"offset", {0, 0, 1}}}}};
  asset.manifest["bindings"]["observer"] = {
      {"eye", {0, 0, 1}}, {"forward", {0, 1, 0}}, {"dim_lux", "invented.q2"}};
  auto light = bi::light_binding(asset, f.terms, h, .05f);
  auto zone = bi::zone_binding(asset, f.terms);
  auto observer = bi::observer_binding(asset, f.terms);
  require(light && std::get<TermRef>(light->intensity).id == 1 && light->source_radius_m == .05f &&
              zone && zone->box->size.y == 6,
          "authored light and zone descriptors resolve registered terms");
  f.objects.position[h.slot()] = {4, 5, 6};
  f.objects.rotation[h.slot()] = {0, 0, std::sqrt(.5f), std::sqrt(.5f)};
  auto view = bi::observer(*observer, h, transforms(f.objects), f.values(), 0, 20);
  require((view.eye - Vec3{4, 5, 7}).length() < .001 && view.forward.x < -.99 && view.dim_lux == 99,
          "observer uses actual transformed eye, forward and live threshold");
  f.objects.rotation[h.slot()] = {std::sqrt(.5f), 0, 0, std::sqrt(.5f)};
  view = bi::observer(*observer, h, transforms(f.objects), f.values(), 0, 20);
  require(std::isfinite(view.forward.x) && std::isfinite(view.forward.y),
          "vertical observer forward remains finite");
  testspace::Scene scene;
  auto sensor = space::sensor_box_geometry(scene.space, zone->box->size);
  ObjectInit init;
  init.model = 1;
  auto sensor_object = create(scene.objects, init);
  require(
      space::add_volume(scene.space, identity(scene.objects), transforms(scene.objects),
                        {sensor_object, sensor, {zone->box->offset, {}}, space::VolumeKind::Zone})
              .status == space::Status::Ok,
      "authored zone creates real closed Jolt sensor");
  std::vector<Handle> volumes;
  space::volumes_at(scene.space, identity(scene.objects), {0, 0, 1}, space::VolumeKind::Zone,
                    space::Deadline::max(), volumes);
  require(volumes == std::vector<Handle>{sensor_object}, "authored sensor containment");
}
void interested_processes_and_lifecycle() {
  Fixture f;
  auto index = f.index();
  auto h = f.make();
  f.processes[1].set(h.slot(), re::Process{0, 0, 1, 0, 2, 1});
  f.properties[2].set(h.slot(), Value(42.));
  f.add(index, h);
  require(f.publish(index, 20).upserts.empty() && !index.rows.at(h.bits).sampled,
          "no viewers means no process sampling");
  bi::view_interest(index, 1, {});
  auto first = f.publish(index, 20);
  require(first.upserts.size() == 1 && first.upserts[0].states == std::vector<ne::State>{{1, 1}},
          "first interested frame samples current process and omits undeclared state");
  ne::Replicator wire(index.settings);
  wire.set_view(1, {});
  require(!wire.advance(std::move(first)).empty(), "first current keyframe reaches binary stream");
  require(f.publish(index, 20).upserts.empty(), "same step process sample is reused");
  require(f.publish(index, 40).upserts[0].states[0].value == 2 && index.active_processes.empty(),
          "terminal clamp removes recurring sampling");
  require(f.publish(index, 60).upserts.empty(), "clamped process is inert");
  f.processes[1].set(h.slot(), re::Process{2, 60, -1, 0, 2, 2});
  bi::value_changed(index, h, 1, f.processes);
  require(f.publish(index, 80).upserts[0].states[0].value == 1,
          "changed rate rearms sampling toward opposite bound");
  f.objects.position[h.slot()] = {10000, 0, 0};
  bi::moved(index, h, transforms(f.objects));
  require(f.publish(index, 81).removed == std::vector<Handle>{h} && index.active_processes.empty(),
          "leaving union interest removes baseline and process subscription");
  bi::view_interest(index, 2, {10000, 0, 0});
  require(f.publish(index, 90).upserts[0].states[0].value == .5f,
          "new viewer receives freshly evaluated dormant process");
  f.objects.params[h.slot()] = 1;
  f.add(index, h);
  auto rebound = f.publish(index, 90);
  require(rebound.upserts.size() == 1 && rebound.upserts[0].faces_motion &&
              rebound.upserts[0].asset != ne::AssetHash{},
          "parameter rebind updates actual asset, parameters and manifest bindings");
  bi::retire_replication(index, h);
  destroy(f.objects, h);
  auto replacement = f.make({10000, 0, 0});
  require(replacement.slot() == h.slot() && replacement != h, "fixture reuses retired slot");
  f.processes[1].erase(h.slot());
  f.add(index, replacement);
  auto born = f.publish(index, 91);
  require(born.removed == std::vector<Handle>{h} && born.upserts[0].object == replacement,
          "stale generation cannot inherit stream identity");
  bi::remove_view_interest(index, 1);
  bi::remove_view_interest(index, 2);
  f.publish(index, 92);
  require(index.viewers.empty() && index.active_processes.empty() && index.interests.empty(),
          "viewer removal prunes all derived interest");
}
void coherent_catalog_publication() {
  using Json = nlohmann::json;
  Fixture f;
  const auto object = f.make(), parent = f.make();
  const auto values = f.values();
  ca::Scripts scripts;
  const auto script = ca::import_script(
      scripts,
      ca::prepare_script("return {kind='model',watches={'invented.q1'},"
                         "on={['invented.q1']=function(self)self.state.seen=true end}}",
                         f.terms, {{"use"}}, ca::sha256("presentation-registry"), {},
                         {4096, 50, 5, 64}),
      9, 0);
  re::Triggers triggers;
  re::PriorityLinks priorities;
  re::Subscriptions subscriptions;
  observe::LightSubscriptions points{8};
  bi::viewing::SessionViews views;
  bi::IlluminanceReads illuminance{points, subscriptions, identity(f.objects),
                                   transforms(f.objects), 0};
  bi::MetadataChanges changes{views, illuminance, {}, {}};
  bi::CatalogPublication publication(
      f.models, scripts, f.terms, f.materials, f.texts, f.instance_texts, values,
      authority(f.objects), parents(f.objects), triggers, priorities, changes, {4, 0, 0, 0, 0, 0});
  const auto &inspection = publication.inspection_terms();
  const auto &initial = publication.initial_catalog();
  require(initial["models"].size() == 1 && initial["terms"].size() == 9 &&
              initial["materials"].size() == 1 && initial["materials"][0]["color"][0] == .25 &&
              std::find(inspection.begin(), inspection.end(), 1) != inspection.end() &&
              std::find(inspection.begin(), inspection.end(), 3) == inspection.end(),
          "initial publication omitted actual definitions or inspected material names as facts");
  struct Wire {
    std::vector<std::pair<ne::SessionId, Json>> messages;
    bool closed = false;
  } wire;
  control::Profiles profiles;
  control::put_profile(profiles, {9,
                                  "Presentation observer",
                                  object,
                                  {},
                                  control::modes(control::Mode::Watch),
                                  {control::hash_key("presentation-fixture")}});
  control::Sessions sessions(profiles, {&wire,
                                        [](void *local, ne::SessionId id, std::string text) {
                                          static_cast<Wire *>(local)->messages.emplace_back(
                                              id, Json::parse(text));
                                        },
                                        [](void *local, ne::SessionId, std::uint16_t, std::string) {
                                          static_cast<Wire *>(local)->closed = true;
                                        },
                                        [](void *, ne::SessionId, Vec3) {}});
  const auto connect = [&](ne::SessionId id) {
    sessions.accept(ne::Connected{id});
    sessions.accept(ne::Control{id, Json{{"t", "hello"},
                                         {"version", 1},
                                         {"key", "presentation-fixture"},
                                         {"mode", "watch"},
                                         {"binary", false}}
                                        .dump()});
    require(sessions.principal(id).has_value() && !wire.closed,
            "publication fixture did not authenticate through actual Sessions");
  };
  connect(101);
  connect(102);
  sessions.accept(ne::Connected{103}); // Connected transport has no authenticated principal.
  wire.messages.clear();
  publication.flush(sessions);
  require(wire.messages.empty(), "an unchanged initial catalog produced unsolicited deltas");
  sessions.send(101, publication.initial_catalog());
  require(wire.messages.size() == 1 && wire.messages.front().second == initial,
          "initial authenticated catalog was not the actual publication cache");
  wire.messages.clear();
  auto model = f.models.rows[1];
  model.id = static_cast<ModelId>(f.models.rows.size());
  model.asset = f.variant;
  model.script = script;
  f.models.rows.push_back(model);
  f.texts.rows[model.id] = {"New definition", "Authored default", ""};
  const std::array<ModelId, 2> model_changes{model.id, model.id};
  publication.definitions_changed(model_changes);
  require(triggers.at(1).by_model.at(model.id).script == script,
          "definition publication did not project the real validated default script");
  re::Term term;
  term.name = "p9.live_fact";
  term.owner = 9;
  term.dimensions[0] = 1;
  const auto added = ca::append_term(f.terms, {term}, {0, "Independent publication fact"});
  f.properties.resize(f.terms.rows.size());
  f.processes.resize(f.terms.rows.size());
  f.properties[added].set(object.slot(), Value(123.));
  ca::set_material(f.materials, f.terms, 3, {{4, Value(.75)}});
  const std::array<TermId, 2> term_changes{added, added}, material_changes{3, 3};
  publication.registry_changed(term_changes, material_changes);
  f.objects.model[object.slot()] = model.id;
  f.objects.owner[object.slot()] = 10;
  f.objects.parent[object.slot()] = parent;
  views.rows[101].body = object;
  views.rows[101].pending.ready = true;
  const std::array object_changes{object, object};
  bi::metadata_changed(changes, object_changes, {});
  require(!views.rows[101].pending.ready,
          "changed ownership/parentage left the actual pending observation cache valid");
  publication.flush(sessions);
  require(wire.messages.size() == 8,
          "one flush did not send four coherent deltas to each authenticated observer");
  const auto rows = [&](ne::SessionId session, std::string_view field) -> const Json & {
    const auto found = std::find_if(wire.messages.begin(), wire.messages.end(), [&](const auto &m) {
      return m.first == session && m.second.contains(field);
    });
    if (found == wire.messages.end())
      throw std::runtime_error("expected catalog/metadata delta missing");
    return found->second.at(field);
  };
  for (const auto session : {101u, 102u}) {
    require(rows(session, "models").size() == 1 && rows(session, "models")[0]["id"] == model.id &&
                rows(session, "models")[0]["asset"] == f.variant &&
                rows(session, "terms").size() == 1 && rows(session, "terms")[0]["id"] == added &&
                rows(session, "materials").size() == 1 &&
                rows(session, "materials")[0]["color"][0] == .75,
            "coalesced definition/registry publication duplicated or cached old rows");
    const auto &objects = rows(session, "objects");
    require(objects.size() == 1 && objects[0]["h"] == to_string(object) &&
                objects[0]["owner"] == "p:10" && objects[0]["parent"] == to_string(parent) &&
                objects[0]["text"]["name"] == "New definition",
            "object metadata did not use current authority, parent and model claims");
  }
  require(std::none_of(wire.messages.begin(), wire.messages.end(),
                       [](const auto &message) { return message.first == 103; }),
          "catalog publication reached an unauthenticated transport");
  require(publication.initial_catalog()["models"].size() == 2 &&
              publication.initial_catalog()["terms"].size() == 10 &&
              publication.initial_catalog()["materials"][0]["color"][0] == .75 &&
              std::count(inspection.begin(), inspection.end(), added) == 1,
          "later clients or existing inspectors retained a stale/duplicated catalog");
  const auto description =
      bi::describe_object(object, identity(f.objects), f.models, f.parameters, f.assets, f.texts,
                          f.instance_texts, values, inspection, {}, 0, 20);
  require(description.measured["values"]["p9.live_fact"] == 123,
          "live inspection did not consume the updated inspection-term reference");
  wire.messages.clear();
  publication.flush(sessions);
  require(wire.messages.empty() && changes.objects.empty() && changes.models.empty(),
          "a completed publication left duplicate work for the next boundary");
}
void benchmark() {
  Fixture f;
  auto index = f.index();
  for (unsigned i = 0; i < 100000; ++i) {
    auto h = f.make({10000.f + float(i % 1000) * 2, 10000.f + float(i / 1000) * 2, 0});
    f.processes[1].set(h.slot(), re::Process{0, 0, 1, 0, 100000, 1});
    f.add(index, h);
  }
  for (unsigned i = 0; i < 32; ++i) {
    auto h = f.make({float(i % 8), float(i / 8), 0});
    f.processes[1].set(h.slot(), re::Process{0, 0, 1, 0, 100000, 1});
    f.add(index, h);
  }
  f.publish(index, 0);
  for (unsigned i = 1; i <= 500; ++i)
    bi::view_interest(index, i, {});
  f.publish(index, 0);
  require(index.active_processes.size() == 32, "only union-interest processes are sampled");
  std::vector<double> samples;
  std::size_t sampled = 0;
  for (unsigned step = 1; step <= 1000; ++step) {
    auto begin = std::chrono::steady_clock::now();
    auto frame = f.publish(index, step);
    samples.push_back(
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin)
            .count());
    sampled += frame.upserts.size();
  }
  require(sampled == 32000, "100k offscreen processes cause zero sampling reads");
  std::sort(samples.begin(), samples.end());
  std::cout << "replication_100k_offscreen_processes_32_visible_500_viewers us p50=" << samples[500]
            << " p95=" << samples[950] << " max=" << samples.back() << '\n';
}
int main(int argc, char **argv) {
  descriptions_and_roles();
  interested_processes_and_lifecycle();
  coherent_catalog_publication();
  if (argc > 1 && std::string_view(argv[1]) == "--bench")
    benchmark();
  std::cout << "presentation and interested replication checks passed\n";
}
