#include "catalog/catalog.hpp"
#include "settings.hpp"
#include "space_fixture.hpp"
#include "step/step.hpp"
#include <toml++/toml.hpp>

#include <chrono>

using namespace amber;
namespace vi = amber::net;
using Json = vi::Json;
using testspace::require;
namespace {
void land_browse_and_actions() {
  catalog::State catalog_state;
  react::State react_state{{4096, 50, 5, 64}};
  ObjectState objects;
  perm::State permissions;
  permissions.players.rows.resize(4);
  const auto native = step::declare_commerce(react_state.terms, react_state.native_declarations);
  const auto admin = step::declare_admin(react_state.terms, react_state.native_declarations);
  react::Term quote;
  quote.id = TermId(react_state.terms.rows.size());
  quote.name = "base.land_quote";
  quote.kind = react::Kind::Action;
  quote.inputs = {{"plot", react::InputType::Integer}};
  react_state.terms.rows.push_back(quote);
  react_state.terms.by_name[quote.name] = quote.id;
  catalog::Script world;
  world.id = 1;
  world.declaration.id = 1;
  world.declaration.kind = react::ScriptDeclaration::Kind::World;
  world.declaration.actions = {{quote.id, "", quote.name, false}};
  world.declaration.handlers = {quote.id};
  world.source = "public land rule source";
  catalog_state.scripts.rows.push_back(world);
  react_state.world_scripts.push_back({1, "land", "Land and its tax", "Current tax rules"});
  react::project_world(react_state, catalog_state.scripts);
  auto &plots = permissions.plots.rows;
  plots.push_back({1, 0, 0, 0, {0, -50, 72, 50}, {}, perm::PlotKind::Street});
  plots.push_back({2, 2, 0, 0, {8, 50, 24, 82}, {}});
  plots.push_back({3, 3, 0, 0, {100008, 50, 100024, 82}, {}});
  plots.push_back({}); // A joined-away ID must not appear in a later page.
  plots.push_back({5, 2, 0, 0, {24, 50, 40, 82}, {}});
  plots.push_back({6, 2, 0, 0, {40, 50, 56, 82}, {}});
  permissions.plots.cell_m = 72;
  perm::rebuild_plot_index(permissions.plots);
  const auto offer = [&](PlotId plot, PlayerId buyer, Step expiry) {
    perm::Offer row;
    row.plot = plot;
    row.seller = plots[plot].owner;
    row.buyer = buyer;
    row.price = 19;
    row.expires_step = expiry;
    perm::open_offer(permissions.offers, row);
  };
  offer(3, 0, 0);
  offer(5, 3, 0);
  offer(6, 0, 5);
  Settings settings{};
  settings.perception.things_max = 1;
  settings.perception.sight_m = 30;
  settings.perception.events_per_response = 40;
  settings.actions_page = 50;
  settings.land = {100, 1000, 72, 8, 16, 4, 4};
  observe::Hearing hearing;
  net::Invocation call;
  call.session = 1;
  call.principal.player = 2;
  const step::Clock clock{.step = 10, .hz = 20};
  const auto inspect = [&] {
    return net::read_inspect(catalog_state, react_state, hearing, objects, permissions, clock,
                             settings, call)
        .fields;
  };
  const auto actions = [&] {
    return net::read_actions(catalog_state, react_state, objects, hearing, settings, permissions,
                             clock.step, call)
        .fields;
  };
  call.request.fields = {{"target", "land"}, {"owner", "self"}};
  auto page = inspect();
  require(page["ok"] == true && page["land"].size() == 1 && page["land"][0]["plot"] == 2 &&
              page["next_cursor"] == 2 && page["more"] == true &&
              page["rules"][0]["writing"] == "Current tax rules" &&
              page["rules"][0]["holder"] == Json{{"script", "s:1"}, {"on", "world"}} &&
              !page["land"][0].contains("dist_m"),
          "remote land browsing uses default page size, owner filter and published world rules");
  call.request.fields["cursor"] = page["next_cursor"];
  page = inspect();
  require(page["land"][0]["plot"] == 5 && page["next_cursor"] == 5,
          "owner filtering runs before pagination and ignores retired plot IDs");
  call.request.fields["cursor"] = page["next_cursor"];
  page = inspect();
  require(page["land"][0]["plot"] == 6 && page["next_cursor"].is_null() && page["more"] == false,
          "final land page has no continuation cursor");
  call.request.fields = {
      {"target", "land"}, {"owner", "p:2"}, {"cursor", 4}, {"for_sale", false}, {"limit", 10}};
  page = inspect();
  require(page["land"].size() == 2 && page["land"][0]["plot"] == 5 &&
              page["land"][1]["plot"] == 6 && page["next_cursor"].is_null(),
          "owner and sale filters resume after a cursor that is not a live plot");
  call.request.fields["owner"] = "p:3";
  call.request.fields["cursor"] = 0;
  call.request.fields["for_sale"] = true;
  page = inspect();
  require(page["land"].size() == 1 && page["land"][0]["plot"] == 3 && !page["more"].get<bool>(),
          "explicit owner filtering includes that owner's visible sale offers");
  call.request.fields = {{"target", "land"}, {"for_sale", true}, {"limit", 10}};
  page = inspect();
  require(page["land"].size() == 1 && page["land"][0]["plot"] == 3 &&
              page["land"][0]["for_sale"]["price"] == 19,
          "sale browsing includes remote visible offers, excluding private and expired offers");
  require(page["land"][0]["accept"] == Json{{"action", "engine.accept"},
                                            {"args", {{"offer", "offer:1"}, {"max_price", 19}}}},
          "a buyable row names the exact acceptance call");
  call.request.fields = {{"target", "land"}};
  require(inspect().contains("error"), "plots in sight need a live body");
  ObjectInit init;
  init.model = 1;
  init.position = {30, 60, 0};
  call.principal.body = create(objects.objects, init);
  page = inspect();
  std::vector<int> nearby;
  for (const auto &plot : page["land"])
    nearby.push_back(plot["plot"]);
  require(page["mode"] == "in_sight" && nearby == std::vector<int>{5} && page["more"] == true &&
              page["land"][0]["dist_m"] == 0 &&
              page["land"][0]["center"] == Json::array({32, 66, 0}),
          "land in sight starts at the nearest plot and pages");
  call.request.fields["limit"] = 10;
  nearby.clear();
  page = inspect();
  for (const auto &plot : page["land"])
    nearby.push_back(plot["plot"]);
  require(nearby == std::vector<int>{5, 2, 1, 6},
          "land in sight lists plots within SIGHT_M nearest first, never the distant lot");
  call.request.fields["fronts"] = "street";
  nearby.clear();
  page = inspect();
  for (const auto &plot : page["land"])
    nearby.push_back(plot["plot"]);
  require(nearby == std::vector<int>{5, 2, 6}, "the fronts filter narrows plots in sight");
  call.request.fields = {{"target", "land"}, {"for_sale", true}, {"sort", "distance"}};
  require(inspect()["land"][0]["plot"] == 3, "sale search sorts by distance when asked");
  call.principal.body = {};
  call.request.fields = {{"target", "plot:3"}};
  const auto remote = inspect();
  require(remote["plot"] == 3 && remote["actions"]["target"] == "plot:3" &&
              !remote["action_signatures"].empty(),
          "remote plot inspection retains full facts and contextual action discovery");
  call.request.fields["name"] = "engine.accept";
  auto action = actions().at("action");
  require(action["example"]["args"]["offer"] == "offer:1" &&
              action["example"]["args"]["max_price"] == 19 && !action["example"].contains("target"),
          "plot acceptance example carries the current offer and asking price");
  call.request.fields["name"] = "base.land_quote";
  action = actions().at("action");
  require(action["example"]["args"]["plot"] == 3 && !action["example"].contains("target"),
          "world land action example carries the plot argument without an object target");
  call.request.fields["name"] = "engine.grant";
  action = actions().at("action");
  require(action["example"]["args"]["scope"] == Json{{"plot", 3}} &&
              action["example"]["args"]["rights"] == Json::array({"enter"}),
          "contextual grants use the plot scope and land rights");
  call.request.fields["name"] = "engine.revoke";
  action = actions().at("action");
  require(action["example"]["args"]["scope"] == Json{{"plot", 3}} &&
              action["example"]["args"].contains("holder") &&
              !action["example"]["args"].contains("grant"),
          "contextual revocation uses the holder/scope form rather than mixing selectors");
  call.request.fields = {{"target", "land"}};
  auto listed = actions();
  std::set<std::string> land_actions;
  for (const auto &action : listed["actions"])
    land_actions.insert(action["name"].get<std::string>());
  require(listed["ok"] == true &&
              land_actions == std::set<std::string>{"base.land_quote", "engine.abandon_plot",
                                                    "engine.accept", "engine.cancel_offer",
                                                    "engine.grant", "engine.join_plots",
                                                    "engine.offer_plot", "engine.plot_permissions",
                                                    "engine.return", "engine.revoke",
                                                    "engine.set_offer_price", "engine.split_plot"},
          "land action space includes land verbs, shared commerce/grants and the world rule");
  const auto ordinary = net::player_action_ids(
      react_state.native_declarations, react::value_read(objects, catalog_state, react_state));
  for (auto id : {native.offer_plot, native.split_plot, native.join_plots, native.abandon_plot,
                  admin.plot_permissions})
    require(std::ranges::find(ordinary, id) == ordinary.end(),
            "ordinary player discovery does not repeat the land-only commands");
  require(std::ranges::find(ordinary, native.accept) != ordinary.end(),
          "shared commerce actions remain available outside the land space");
  call.request.fields = Json::object();
  require(actions()["action_spaces"][0]["target"] == "land",
          "ordinary discovery links to the available action spaces");
  const auto holder = perm::Holder::script(world.id, {});
  const auto scope = perm::Scope::account(perm::Account::player(2));
  const auto grant = perm::create_grant(permissions.grants, permissions.players,
                                        perm::Holder::player(2), {2, holder, scope, 1, 50, 20, 100},
                                        {scope, 2, perm::all_rights}, {holder, 0, true}, 10);
  require(bool(grant), "land grant inspection uses a real capped world-script grant");
  call.request.fields = {{"target", "grants"}};
  require(inspect()["given"][0]["holder"] == Json{{"script", "s:1"}, {"on", "world"}},
          "grant inspection identifies world scripts without a fictitious object handle");
  const auto child = perm::create_grant(permissions.grants, permissions.players, holder,
                                        {0, perm::Holder::player(2), scope, 1, 20, 20, 80},
                                        {scope, 2, perm::all_rights}, {}, 10);
  require(child && inspect()["held"][0]["issuer"] == Json{{"script", "s:1"}, {"on", "world"}} &&
              inspect()["held"][0]["parent"] == "g:" + std::to_string(grant.grant),
          "grant inspection attributes a delegated world grant to its script rather than root");
  require(step::check_input(react_state.native_declarations.rows.at(admin.grant).inputs_schema,
                            {{"holder", {{"script", "s:1"}, {"on", "world"}}},
                             {"scope", {{"account", "p:2"}}},
                             {"rights", {"spend"}},
                             {"cap", 50},
                             {"period_s", 1},
                             {"expires_s", 5}})
              .empty(),
          "world-script grant syntax accepts its published holder identifier");
  require(
      net::validate_inspect(
          {{"target", "land"}, {"owner", "self"}, {"for_sale", true}, {"cursor", 0}, {"limit", 2}})
              .empty() &&
          !net::validate_inspect({{"target", "land"}, {"for_sale", 1}}).empty() &&
          net::validate_inspect({{"target", "land"},
                                 {"for_sale", true},
                                 {"fronts", "street"},
                                 {"max_price", 30},
                                 {"min_area", 100},
                                 {"sort", "price"}})
              .empty() &&
          !net::validate_inspect({{"target", "land"}, {"max_price", -1}}).empty(),
      "land browse wire inputs preserve typed filters and paging");
  permissions.offers.rows[1].expires_step = clock.step;
  offer(3, 0, 0);
  permissions.offers.rows.back().price = 23;
  call.request.fields = {{"target", "land"}, {"for_sale", true}, {"limit", 10}};
  page = inspect();
  require(page["land"].size() == 1 && page["land"][0]["for_sale"]["offer"] == "offer:4" &&
              page["land"][0]["for_sale"]["price"] == 23,
          "browsing selects the current offer after an earlier offer expires");
  call.request.fields = {{"target", "plot:3"}};
  require(inspect()["for_sale"] == page["land"][0]["for_sale"],
          "individual inspection and browsing disagree about the current visible offer");
}

void events_projection(bool benchmark) {
  vi::SessionViews views;
  vi::Profiles profiles;
  profiles.rows.resize(3);
  profiles.rows[2].id = 2;
  views.rows[1].eye = {};
  vi::RecordFeed feed{{}, 4096};
  const std::string thing = "o:7.1";
  const auto add = [&](std::uint64_t id, std::uint64_t root, const char *actor,
                       const std::string &object, Vec3 position) {
    feed.rows.push_back(
        {{{"id", id}, {"root", root}, {"actor", actor}, {"things", Json::array({object})}},
         {position}});
  };
  add(1, 7, "p:9", thing, {});
  add(2, 7, "p:9", thing, {100, 0, 0});
  add(3, 7, "p:2", thing, {100, 0, 0});
  add(4, 8, "p:9", thing, {});
  add(5, 7, "p:9", "o:8.1", {});
  add(6, 7, "p:9", thing, {});
  add(7, 7, "p:9", thing, {});
  feed.rows.back().record["authors"] = {"p:2", "p:9"};
  feed.rows.back().record["cause"] = 6;
  feed.rows.back().record["result"] = "applied";
  feed.rows.back().record["measured"] = {{"volume", 1}};
  feed.rows.back().record["source"] = "authored source";
  net::Invocation call;
  call.session = 1;
  call.principal.player = 2;
  call.principal.mode = net::Mode::Watch;
  call.request.fields = {{"since", 1}, {"root", 7}, {"thing", thing}, {"limit", 2}};
  auto result = vi::read_events(feed, views, profiles, 30, 40, call).fields;
  require(result["events"].size() == 2 && result["events"][0]["id"] == 6 &&
              result["events"][1]["id"] == 7 && result["events_omitted"] == 1 &&
              result["oldest_available"] == 1 && result["newest"] == 7 &&
              !result["truncated"].get<bool>(),
          "events ring changed chronological order, filters, privacy or omitted count");
  const auto full = result;
  call.request.fields["detail"] = false;
  result = vi::read_events(feed, views, profiles, 30, 40, call).fields;
  require(result["events_omitted"] == full["events_omitted"] &&
              result["oldest_available"] == full["oldest_available"] &&
              result["newest"] == full["newest"] && result["events"][0] == full["events"][0] &&
              result["events"][1]["id"] == 7 &&
              result["events"][1]["authors"] == full["events"][1]["authors"] &&
              result["events"][1]["cause"] == 6 && result["events"][1]["result"] == "applied" &&
              !result["events"][1].contains("measured") && !result["events"][1].contains("source"),
          "summary query changed privacy/filter/causal attribution or copied heavy details");
  call.request.fields.erase("detail");
  call.request.fields["since"] = 6;
  result = vi::read_events(feed, views, profiles, 30, 40, call).fields;
  require(result["events"].size() == 1 && result["events"][0]["id"] == 7 &&
              result["events_omitted"] == 0,
          "events since filter changed");
  call.request.fields["since"] = 1;
  call.request.fields["limit"] = 10;
  profiles.rows[2].roles = {"architect"};
  require(vi::read_events(feed, views, profiles, 30, 40, call).fields["events"].size() == 4,
          "an existing session immediately gains the profile's operator-assigned event visibility");
  profiles.rows[2].roles.clear();
  require(vi::read_events(feed, views, profiles, 30, 40, call).fields["events"].size() == 3,
          "role removal immediately removes remote event visibility without reconnecting");
  call.principal.mode = net::Mode::Debug;
  call.request.fields["since"] = 1;
  call.request.fields["limit"] = 1;
  result = vi::read_events(feed, views, profiles, 30, 40, call).fields;
  require(result["events"].size() == 1 && result["events"][0]["id"] == 7 &&
              result["events_omitted"] == 3,
          "debug visibility or single-result wrap changed");
  call.request.fields["detail"] = false;
  const auto debug_summary = vi::read_events(feed, views, profiles, 30, 40, call).fields;
  require(debug_summary["events"][0]["id"] == 7 &&
              debug_summary["events_omitted"] == result["events_omitted"],
          "summary query changed debug visibility or tail selection");
  call.request.fields.erase("detail");
  call.request.fields["root"] = 999;
  result = vi::read_events(feed, views, profiles, 30, 40, call).fields;
  require(result["events"].empty() && result["events_omitted"] == 0,
          "empty filtered events query invented rows");
  if (!benchmark)
    return;
  feed.rows.clear();
  const Json details{{"text", std::string(1024, 'x')}, {"values", std::vector<double>(64, 1)}};
  for (std::uint64_t id = 1; id <= 4096; ++id) {
    add(id, id, "p:2", thing, {});
    feed.rows.back().record["detail"] = details;
  }
  call.principal.mode = net::Mode::Watch;
  call.request.fields = {{"limit", 1}};
  std::vector<double> samples;
  for (int i = 0; i < 110; ++i) {
    const auto started = std::chrono::steady_clock::now();
    result = vi::read_events(feed, views, profiles, 30, 40, call).fields;
    const auto us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started)
            .count();
    require(result["events"][0]["id"] == 4096 && result["events_omitted"] == 4095,
            "full event retention benchmark returned an incorrect page");
    if (i >= 10)
      samples.push_back(us);
  }
  std::sort(samples.begin(), samples.end());
  std::cout << "events4096_limit1_us p50=" << samples[50] << " p95=" << samples[95] << '\n';
}
} // namespace
int main(int argc, char **argv) {
  try {
    land_browse_and_actions();
    events_projection(argc == 2 && std::string_view(argv[1]) == "--events-bench");
    testspace::Scene scene;
    const auto body = scene.character({0, 0, 0});
    const auto visible = scene.box({1, 1, 2}, {0, 5, 0});
    const auto far = scene.box({12, 2, 10}, {10, 60, 0}, space::Motion::Static, 0, false);
    const auto held = scene.box({.2f, .2f, .2f}, {2, 0, 0}, space::Motion::Dynamic, 0, false);
    auto &space_state = scene.space_state;

    require(
        space::attach(scene.objects_state, scene.space_state, {held, body, {{0, .3f, 1}, {}}, true})
                .status == space::Status::Ok,
        "actual grasp for held observation");
    scene.objects.owner[body.slot()] = 2;
    scene.objects.owner[visible.slot()] = 3;
    scene.objects.model[far.slot()] = 3;
    scene.objects.model[held.slot()] = 4;
    for (const auto h : {body, visible, far, held})
      scene.objects.material[h.slot()] = 2;
    amber::react::State react_state{{4096, 50, 5, 64}};
    react::Terms &terms = react_state.terms;
    terms.rows.resize(17);
    for (TermId id = 1; id < 5; ++id) {
      terms.rows[id].id = id;
      terms.rows[id].name = "test.t" + std::to_string(id);
      terms.by_name[terms.rows[id].name] = id;
    }
    for (TermId id = 5; id <= 16; ++id) {
      auto &term = terms.rows[id];
      term.id = id;
      term.name = "novel.observed" + std::to_string(id);
      term.source = id <= 10 ? react::Source::Setting : react::Source::State;
      terms.by_name[term.name] = id;
    }
    terms.rows[13].combine = terms.rows[14].combine = react::Combine::Sum;
    terms.rows[15].source = react::Source::Intrinsic;
    terms.rows[16].source = react::Source::Measured;
    terms.rows[16].measurement = react::MeasurementRole::Volume;
    terms.rows[2].kind = react::Kind::Material;
    terms.rows[3].kind = react::Kind::Action;
    terms.rows[1].dimensions[1] = 1;
    terms.rows[3].inputs = {{"amount", react::InputType::Number, 1, false},
                            {"note", react::InputType::String, 0, true}};
    terms.rows[4].kind = react::Kind::Event;
    react::Materials &materials = react_state.materials;
    amber::catalog::State catalog_state;
    react::AssetMeasurements &measurements = catalog_state.baked_measurements;
    react::Properties &properties = (react_state.properties = react::Properties(17));
    react::Processes &processes = (react_state.processes = react::Processes(17));
    react::InstanceSettings &settings = react_state.instance_settings;
    react::ModelSettings &model_settings = react_state.model_settings;
    react::PlotSettings &plot_settings = react_state.plot_settings;
    react::WorldSettings &world = react_state.world_settings;
    react::SharedColumns &shared = (react_state.shared = react::SharedColumns(17));
    settings.columns.resize(17);
    settings.columns[6].set(visible.slot(), Value(0.0));
    model_settings.rows[1][7] = Value(7.0);
    set_plot(scene.objects, visible.slot(), 1);
    plot_settings.rows[1][8] = Value(8.0);
    world.values[5] = Value(99.0);
    world.values[9] = Value(9.0);
    world.values[10] = Value(10.0);
    world.values[12] = Value(12.0);
    world.values[14] = Value(14.0);
    properties[11].set(visible.slot(), Value(11.0));
    shared[13].set(visible.slot(), react::Shared{13.0, {{held, 13.0}}});
    materials.rows[2][15] = Value(15.0);
    measurements.rows[1].volume = 1;
    react::RelationsRead &relations = react_state.relations;
    react::RuntimeMeasurements &runtime = react_state.runtime_measurements;
    const react::ValueRead values{terms,
                                  identity(scene.objects),
                                  scene.objects.plot,
                                  scene.objects.params,
                                  materials,
                                  measurements,
                                  properties,
                                  processes,
                                  settings,
                                  model_settings,
                                  plot_settings,
                                  world,
                                  shared,
                                  relations,
                                  runtime,
                                  0};
    catalog::Models &models = catalog_state.models;
    models.rows.resize(5);
    catalog::Assets &assets = catalog_state.assets;
    catalog::ParameterBlocks &parameters = catalog_state.parameters;
    for (ModelId id = 1; id < 5; ++id) {
      const auto hash = catalog::sha256(std::to_string(id));
      catalog::Asset asset;
      asset.hash = hash;
      asset.minimum = {-.5f, -.5f, 0};
      asset.maximum = {.5f, .5f, 2};
      asset.volume = 1;
      asset.capacity = .1;
      asset.manifest = {
          {"params", Json::object()}, {"sites", Json::object()}, {"bindings", Json::object()}};
      if (id == 2)
        asset.manifest["bindings"]["observer"] = {
            {"eye", {0, 0, 1.55}}, {"forward", {0, 1, 0}}, {"dim_lux", 10}};
      if (id == 1) {
        asset.replicated = {10};
        asset.manifest["bindings"]["render"] = {
            {{"term", "novel.observed9"}, {"property", "emission"}}};
      }
      assets.rows[hash] = asset;
      models.rows[id].id = id;
      models.rows[id].asset = hash;
      models.rows[id].material = 2;
      models.rows[id].script = 1;
    }
    catalog::ModelTexts &model_text = catalog_state.model_text;
    model_text.rows[1] = {"Claimed giant", "Maker text does not alter measured dimensions", ""};

    catalog::Scripts &scripts = catalog_state.scripts;
    const react::VmSettings vm_settings{4096, 50, 5, 64};
    auto &vm = react_state.vm;
    observe::Hearing &hearing = space_state.hearing;
    std::vector<observe::Listener> &listeners =
        (space_state.listeners =
             std::vector<observe::Listener>{{11, body, {0, 0, 1.55f}}, {12, body, {100, 100, 0}}});

    react::bind_values(vm, values, react_state);
    auto prepared = catalog::prepare_script(
        R"(return {kind='model',actions={['test.t3']={right='use'}},on={
      ['test.t3']=function(self) end,
      ['test.t4']=function(self)
        self:set('test.t1',73)
        self.state[123]=42
        self.state['123']='distinct string key'
        self.state.retained=self.handle
        self.state.text='o:1.1'
      end}})",
        terms, {{"use", "take", "move", "edit", "copy", "transfer", "destroy"}},
        catalog::sha256("registry"), {}, vm_settings);
    auto compiled = prepared.compiled;
    catalog::import_script(scripts, std::move(prepared), 2, 0);
    require(react::load_script(vm, compiled, scripts.rows[1].declaration).status ==
                react::RunOutcome::Status::Ok,
            "real VM module load");

    react::NativeActions &native = react_state.native_declarations;
    catalog::declare_native(
        native, terms, "engine.example", "An actual native descriptor", "", false,
        {{"type", "object"},
         {"required", {"amount"}},
         {"properties", {{"amount", {{"type", "integer"}}}, {"note", {{"type", "string"}}}}}},
        react::WritingAction::EditText);
    const auto targeted =
        catalog::declare_native(native, terms, "engine.targeted", "Edit a selected object", "edit",
                                false, {{"type", "object"}}, react::WritingAction::EditText);
    react_state.inspection_terms = {1, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    catalog::AuthoredTexts &authored_texts = catalog_state.authored_text;
    const auto detail_id = catalog::append_authored_text(
        authored_texts, visible, 1, 12, catalog::AuthoredTextKind::Detail, "A worn brass edge");
    catalog::append_authored_text(authored_texts, visible, 1, 13,
                                  catalog::AuthoredTextKind::Refusal, "private refusal reason");
    catalog::append_authored_text(authored_texts, far, 1, 14, catalog::AuthoredTextKind::Detail,
                                  "Other object");
    catalog::append_authored_text(authored_texts,
                                  Handle::make(visible.slot(), visible.generation() + 1), 1, 15,
                                  catalog::AuthoredTextKind::Detail, "Different generation");
    vi::SessionViews views;
    vi::Profiles profiles;
    profiles.rows.resize(3);
    profiles.rows[2].id = 2;
    space_state.lights = observe::Lights(8);

    step::State step_state;

    Step now = 0;
    double ambient_lux = 120;
    const observe::Settings perception{30, 1.5f, 256, 1, .1f, .1, 40, 8, 40};
    const auto look = [&](const net::Invocation &request) {
      step_state.clock.step = now;
      step_state.clock.hz = 20;
      return vi::read_look(catalog_state, react_state, views, space_state, scene.objects_state,
                           step_state, perception, ambient_lux, request);
    };
    net::Invocation call;
    call.session = 11;
    call.principal = {2, body, "Observer", net::Mode::Play, false};
    call.request.kind = net::ControlKind::Look;
    call.request.fields = Json::object();
    auto first = look(call).fields;
    const std::string signature = "test.t3(amount: number [kg], note?: string)";
    require(first["body_actions"] == Json::array({signature}) &&
                first["player_actions"] ==
                    Json::array(
                        {"engine.example(amount: integer, note?: string)", "engine.targeted()"}),
            "full observation omitted one-line authored/native input signatures");
    amber::perm::State perm_state;
    perm::Players &players = perm_state.players;
    players.rows.resize(4);
    perm::Grants &grants = perm_state.grants;

    const auto offered = [&](const net::Invocation &request) {
      return vi::read_actions(
          catalog_state, react_state, scene.objects_state, hearing,
          [&] {
            amber::Settings s{};
            s.actions_page = 10;
            s.perception.events_per_response = 40;
            return s;
          }(),
          perm_state, now, request);
    };
    auto action_call = call;
    action_call.request.fields = {{"name", "test.t3"}};
    const auto offered_action = offered(action_call).fields.at("action");
    require(offered_action["name"] == "test.t3" && offered_action["signature"] == signature &&
                offered_action["inputs"][0]["quantity"]["unit"] == "kg",
            "rich action discovery lost callable name, canonical signature or units");
    action_call.request.fields = {{"name", "engine.targeted"}};
    const auto example = offered(action_call).fields.at("action").at("example");
    require(example.at("target") == to_string(body) &&
                bool(react::resolve(react_state, catalog_state, values.identity,
                                    {PlayerId{2}, targeted, body, {}}, body)),
            "named native help omitted the target needed to resolve its example");
    auto &declared_action = scripts.rows[1].declaration.actions[0];
    const auto body_model = scene.objects.model[body.slot()];
    models.rows[body_model].script = 0;
    action_call.request.fields = {{"target", to_string(visible)}};
    auto available = offered(action_call).fields;
    require(available["total"] == 0 && available["actions"].empty() && available["more"] == false,
            "source discovery offered an action without the player's current selected right");
    action_call.request.fields["name"] = "test.t3";
    require(offered(action_call).fields.at("action")["name"] == "test.t3",
            "rights-filtered lists hid detailed help for an open-use action term");
    action_call.request.fields.erase("name");
    const auto scope = perm::Scope::object(visible);
    const auto use = perm::bit(perm::Verb::Use);
    const auto use_grant = perm::create_grant(grants, players, perm::Holder::player(3),
                                              {3, perm::Holder::player(2), scope, use, 0, 0, 10},
                                              {scope, 3, perm::all_rights, 0, false},
                                              {perm::Holder::player(2), 2, false}, now);
    require(bool(use_grant), "source discovery fixture needs a real explicit Use grant");
    now = 9;
    available = offered(action_call).fields;
    require(available["total"] == 1 && available["actions"][0]["name"] == "test.t3",
            "current explicit source permission did not expose its declared action");
    now = 10;
    action_call.request.fields["page"] = 1;
    available = offered(action_call).fields;
    require(available["total"] == 0 && available["actions"].empty() && available["more"] == false,
            "action permissions were stale or pagination ran before current rights filtering");
    action_call.request.fields.erase("page");
    scene.objects.preset[visible.slot()] = use;
    require(offered(action_call).fields["total"] == 1,
            "public object preset did not make the actual action available");
    scene.objects.preset[visible.slot()] = 0;
    models.rows[body_model].script = 1;
    require(
        offered(action_call).fields["total"] == 1,
        "body-first action filtering checked the foreign selection instead of its self endpoint");
    declared_action.reach = true;
    scene.objects.preset[far.slot()] = use;
    action_call.request.fields = {{"target", to_string(far)}};
    require(offered(action_call).fields["total"] == 1,
            "requested action discovery added a reach filter absent from the source contract");
    scene.objects.preset[far.slot()] = 0;
    declared_action.reach = false;
    now = 0;
    action_call.request.fields = Json::object();
    available = offered(action_call).fields;
    require(available["total"] == 3 && available["actions"][0]["name"] == "engine.example" &&
                available["actions"][1]["name"] == "engine.targeted" &&
                available["actions"][2]["name"] == "test.t3",
            "self discovery lost owned body actions or available native actions");
    auto find = [](const Json &rows, Handle h) -> const Json * {
      for (const auto &row : rows)
        if (row["h"] == to_string(h))
          return &row;
      return nullptr;
    };
    const auto *seen = find(first["things"], visible);
    const auto *held_row = find(first["things"], held);
    require(held_row && (*held_row)["actions"] == Json::array({signature}),
            "object observation lists bare names instead of input signatures");
    require(seen && seen->contains("measured") && (*seen)["says"]["name"] == "Claimed giant",
            "structured full row and separate maker claim");
    require(!seen->contains("actions"),
            "unreachable visible objects must omit action signatures from observations");
    const auto &observed_values = (*seen)["measured"]["values"];
    require(
        !observed_values.contains("novel.observed5") &&
            !observed_values.contains("novel.observed12") &&
            !observed_values.contains("novel.observed14") &&
            observed_values["novel.observed6"] == 0 && observed_values["novel.observed7"] == 7 &&
            observed_values["novel.observed8"] == 8 && observed_values["novel.observed9"] == 9 &&
            observed_values["novel.observed10"] == 10 &&
            observed_values["novel.observed11"] == 11 &&
            observed_values["novel.observed13"] == 13 &&
            observed_values["novel.observed15"] == 15 && observed_values["novel.observed16"] == 1,
        "compact look hid arbitrary relevant values or exposed absent/global-only defaults");
    require((*seen)["elaborated"].size() == 1 && (*seen)["elaborated"][0]["id"] == detail_id &&
                (*seen)["elaborated"][0]["text"] == "A worn brass edge" &&
                (*seen)["elaborated"][0]["author"] == "p:1" &&
                (*seen)["elaborated"][0]["created_s"] == 12,
            "public detail lost its provenance, merged maker claims or exposed a refusal");
    require(find(first["things"], held) && (*find(first["things"], held))["held"].get<bool>(),
            "held object included by real custody relationship");
    const auto *far_row = find(first["far"], far);
    require(far_row && !far_row->contains("actions"),
            "salient distant geometry is included without unreachable action signatures");
    const auto second = look(call).fields;
    require(find(second["things"], visible)->contains("measured"),
            "native observations must remain full for gateway compaction");
    require(views.rows.at(11).last_observation.size() >= 3,
            "capture retains handles from short and far rows");
    require(first.contains("body_actions") && !first["body_actions"].empty() &&
                first.contains("player_actions") &&
                second["body_actions"] == first["body_actions"] &&
                second["player_actions"] == first["player_actions"],
            "native projection lost action lists needed by a fresh gateway session");
    const auto positions = scene.objects.position;
    const auto flags = scene.objects.flags;
    const auto model_ids = scene.objects.model;
    const auto full_again = look(call).fields;
    require(find(full_again["things"], visible) && *find(full_again["things"], visible) == *seen &&
                full_again["body_actions"] == first["body_actions"] &&
                full_again["player_actions"] == first["player_actions"] &&
                full_again["far"] == first["far"],
            "successive native looks must supply full descriptions and actions");
    require(scene.objects.position == positions && scene.objects.flags == flags &&
                scene.objects.model == model_ids,
            "look mutated world state");
    call.request.fields["limit"] = 1;
    const auto limited = look(call).fields;
    require(limited["things"].size() == 1 && limited["far"] == first["far"],
            "smaller same-step limit ignored the requested near-row cap");
    const auto &reported = views.rows.at(11).last_observation;
    require(reported.size() == 1 + limited["things"].size() + limited["far"].size() &&
                std::ranges::all_of(reported,
                                    [&](Handle h) {
                                      return h == body || find(limited["things"], h) ||
                                             find(limited["far"], h);
                                    }),
            "limited observation capture includes handles not returned to the player");
    call.request.fields.erase("limit");
    const auto started = step::start_move(step_state.intents, body, Vec3{0, 10, 0}, {});
    require(look(call).fields["self"]["intent"]["id"] == started.id,
            "same-step look did not report the new movement intent");
    require(step::cancel_move(step_state.intents, body).has_value(), "cancel actual intent");
    require(look(call).fields["self"]["intent"].is_null(),
            "same-step look retained a cancelled movement intent");
    models.rows[body_model].script = 0;
    require(look(call).fields["body_actions"].empty(),
            "same-step model edit left removed body actions visible");
    models.rows[body_model].script = 1;
    require(look(call).fields["body_actions"] == first["body_actions"],
            "same-step model edit failed to restore declared body actions");
    ambient_lux = 1;
    const auto dark = look(call).fields;
    require(!find(dark["things"], visible)->at("lit").get<bool>(),
            "next look did not recompute lighting after ambient changed in the same step");
    ambient_lux = 120;
    const auto lit = look(call).fields;
    require(find(lit["things"], visible)->at("lit").get<bool>(),
            "next look did not restore current lighting");
    model_settings.rows[1][7] = Value(17.0);
    const auto setting_changed = look(call).fields;
    require(find(setting_changed["things"], visible)
                    ->at("measured")
                    .at("values")
                    .at("novel.observed7") == 17,
            "compact look did not replace a short row after arbitrary authored setting change");
    authored_texts.rows[detail_id].text = "A polished brass edge";
    const auto rewritten = look(call).fields;
    const auto *rewritten_row = find(rewritten["things"], visible);
    require(rewritten_row && (*rewritten_row)["elaborated"][0]["text"] == "A polished brass edge" &&
                (*rewritten_row)["says"]["name"] == "Claimed giant",
            "same-step detail rewrite remained hidden behind an unchanged observation row");
    react::ScriptStates &states = react_state.states;
    react::RuleVersions &rules = react_state.rules;
    react::DeferredEffects effects;
    react::Invocation invocation;
    invocation.self = {visible, 1};
    invocation.state = &states[{visible, 1}];
    invocation.effects = &effects;
    require(react::run_handler(vm, {1, "test.t4"}, invocation).status ==
                react::RunOutcome::Status::Ok,
            "actual VM changes inspected object's own state");
    scripts.rows[1].source = "verified public source";
    perm::Plots &plots = perm_state.plots;
    plots.cell_m = 72;
    plots.rows.push_back({1, 2, 0, 0, {80, 30, 90, 40}, {}});
    plots.rows.push_back({2, 3, 0, 0, {-20, -10, -10, 0}, {}});
    plots.rows.push_back({3, 2, 0, 0, {95, 30, 105, 40}, {}});
    perm::rebuild_plot_index(plots);

    const auto inspect = [&](const net::Invocation &request) {
      return vi::read_inspect(
          catalog_state, react_state, hearing, scene.objects_state, perm_state,
          amber::step::Clock{.step = now, .hz = 20},
          [&] {
            amber::Settings loaded{};
            loaded.perception = perception;
            loaded.land = {100, 1000, 72, 8, 16, 4, 4};
            return loaded;
          }(),
          request);
    };
    auto owner_call = call;
    owner_call.request.fields = {{"target", to_string(visible)}};
    require(inspect(owner_call).fields["owner"] == "p:3", "inspect reports current owner");
    scene.objects.owner[visible.slot()] = 2;
    const auto transferred = inspect(owner_call).fields;
    require(transferred["owner"] == "p:2" && transferred["rights"]["take"]["allowed"] == true,
            "same-step ownership change left inspection or authority stale");
    scene.objects.owner[visible.slot()] = 3;
    auto grant_call = call;
    grant_call.request.fields = {{"target", "grants"}};
    const auto grant_inspection = inspect(grant_call).fields;
    require(grant_inspection["held"].size() == 1 &&
                grant_inspection["held"][0]["id"] == "g:" + std::to_string(use_grant.grant) &&
                grant_inspection["held"][0]["parent"].is_null(),
            "grant inspection must return the same public identifier used for revocation");
    const auto current_source = catalog::sha256("verified current CAD");
    models.rows[1].source_hash = current_source;
    models.rows[1].canonical_params = "{}";
    assets.rows[models.rows[1].asset].source = "verified current CAD";
    auto current_variant = assets.rows.at(models.rows[1].asset);
    current_variant.hash = catalog::sha256("current variant");
    current_variant.source_hash = current_source;
    assets.rows[current_variant.hash] = current_variant;
    parameters.rows.push_back({1, 1, current_variant.hash, R"({"width":2.0})", {}});
    auto historical_variant = current_variant;
    historical_variant.hash = catalog::sha256("historical variant");
    historical_variant.source_hash = catalog::sha256("previous source");
    assets.rows[historical_variant.hash] = historical_variant;
    parameters.rows.push_back({2, 1, historical_variant.hash, R"({"old_parameter":3.0})", {}});
    call.request.fields = {{"target", "m:1"}};
    const auto model_inspection = inspect(call).fields;
    require(model_inspection["source"] == "verified current CAD" &&
                model_inspection["source_hash"] == current_source &&
                model_inspection["canonical_params"] == "{}" &&
                model_inspection["variants"].size() == 1 &&
                model_inspection["variants"][0]["params"] == 1,
            "live authoring inspection lost exact source/default or included historical variants");
    call.request.fields = {{"target", to_string(visible)}};
    const auto current = inspect(call).fields;
    require(current["actions"] == Json::array({signature}),
            "full inspection retains declared actions beyond reach");
    require(current["values"]["test.t1"] == 73,
            "inspect reads current VM-mutated state after an earlier observation");
    require(current["values"]["novel.observed5"] == 99 &&
                !current["values"].contains("novel.observed12") &&
                !current["values"].contains("novel.observed14"),
            "inspect lost exhaustive inherited settings or invented state/shared defaults");
    require(current["elaborated"].size() == 1 &&
                current["elaborated"][0]["text"] == "A polished brass edge",
            "public inspection omitted the shared elaborated detail");
    require(!current.contains("script_states"),
            "inspection leaked another owner's private flat state");
    call.principal.player = 3;
    const auto private_state = inspect(call).fields;
    const auto &entries = private_state["script_states"][0]["state"];
    auto entry = [&](const Json &key) -> const Json & {
      for (const auto &row : entries)
        if (row["key"] == key)
          return row;
      throw std::runtime_error("missing typed script state key");
    };
    require(entry(123)["type"] == "number" && entry(123)["value"] == 42 &&
                entry("123")["type"] == "string" &&
                entry("123")["value"] == "distinct string key" &&
                entry("retained")["type"] == "handle" &&
                entry("retained")["value"] == to_string(visible) &&
                entry("text")["type"] == "string" && entry("text")["value"] == "o:1.1",
            "actual VM flat state lost integer/string key or typed-handle distinctions");
    call.principal.player = 2;
    scene.objects.preset[visible.slot()] = perm::bit(perm::Verb::Edit);
    require(inspect(call).fields.contains("script_states"),
            "current Edit did not permit generic installed state inspection");
    scene.objects.preset[visible.slot()] = 0;
    auto rule_prepared = catalog::prepare_script(
        R"(return {kind='rule',applies_to={material='test.t2'},
          on={['test.t4']=function(self)self.state[2]=19 end},
          tests={{when={steps=0},expect={subject='target',term='test.t1',equals=73}}}})",
        terms, {{"use", "edit"}}, catalog::sha256("registry"), {}, vm_settings);
    const auto rule_code = rule_prepared.compiled;
    const auto rule_id = catalog::import_script(scripts, std::move(rule_prepared), 1, 0);
    scripts.rows[rule_id].declaration.approved = true;
    scripts.rows[rule_id].declaration.active = true;
    react::add_rule(rules, scripts, terms, rule_id);
    react::project_rules(react_state.triggers, rules);
    require(react::load_script(vm, rule_code, scripts.rows[rule_id].declaration).status ==
                react::RunOutcome::Status::Ok,
            "actual applicable rule VM load");
    react::Invocation rule_invocation;
    rule_invocation.self = {visible, rule_id};
    rule_invocation.state = &states[{visible, rule_id}];
    rule_invocation.effects = &effects;
    require(react::run_handler(vm, {rule_id, "test.t4"}, rule_invocation).status ==
                react::RunOutcome::Status::Ok,
            "actual rule writes its numeric flat state");
    call.principal.player = 3;
    const auto with_rule = inspect(call).fields;
    require(with_rule["script_states"].size() == 2 &&
                with_rule["script_states"][1]["script"] == "s:" + std::to_string(rule_id) &&
                with_rule["script_states"][1]["state"][0]["key"] == 2 &&
                with_rule["script_states"][1]["state"][0]["value"] == 19,
            "owner inspection omitted actual applicable world-rule state");
    react::set_rule_enabled(rules, {4, 2, false}, false);
    react::project_rules(react_state.triggers, rules);
    require(inspect(call).fields["script_states"].size() == 1,
            "inspection exposed a no-longer-applicable rule state as an installed script");
    call.principal.player = 2;
    call.request.fields = {{"target", "land"}};
    const auto in_sight = inspect(call).fields;
    require(in_sight["land"].size() == 1 && in_sight["land"][0]["plot"] == 2 &&
                in_sight["land"][0]["dist_m"] == 10,
            "land in sight lists the nearby plot and leaves out distant lots");
    call.request.fields = {{"target", "land"}, {"owner", "p:2"}};
    const auto owned = inspect(call).fields;
    require(owned["land"].size() == 2 && owned["land"][0]["plot"] == 1 &&
                owned["land"][1]["plot"] == 3,
            "an owner's lots are listed from anywhere in plot order");
    call.request.fields = {{"target", "plot:2"}};
    const auto land = inspect(call).fields;
    require(land["owner"] == "p:3" && land["plot"] == 2 &&
                land["bounds"] == Json::array({-20.f, -10.f, -10.f, 0.f}),
            "public plot inspection invented ownership or failed by actual ID");
    call.request.fields = {{"target", "plot:0"}};
    require(!inspect(call).fields["ok"].get<bool>(), "plot lookup accepted reserved ID zero");
    call.request.fields = {{"target", "plot:999"}};
    require(!inspect(call).fields["ok"].get<bool>(), "plot lookup accepted nonexistent ID");
    call.request.fields = Json::object();
    const auto refreshed = look(call).fields;
    require(find(refreshed["things"], visible)->at("measured").at("values").at("test.t1") == 73,
            "next look reads a changed visible value in the same step");
    require(!current["rights"]["take"]["allowed"].get<bool>(),
            "inspect exposes actual denial without requiring Use to inspect");
    observe::emit_sound(
        hearing, {body, {0, 0, 1.55f}, 12, std::string("audible"), 2, {}, "Nearby speaker", true},
        listeners);
    call.request.fields = Json::object();
    const auto heard = look(call).fields;
    require(heard["heard"].size() == 1 && heard["heard"][0]["text"] == "audible" &&
                heard["heard"][0]["from"] == "p:2" && heard["heard"][0]["name"] == "Nearby speaker",
            "native response retains real nearby hearing");
    require(!hearing.queues.contains(12) || hearing.queues.at(12).empty(),
            "distant listener receives no sound");
    call.request.fields = {
        {"target", to_string(Handle::make(visible.slot(), visible.generation() + 1))}};
    require(!inspect(call).fields["ok"].get<bool>(), "stale generation rejected");
    vi::RecordFeed feed{{}, 2};
    const std::array<Json, 3> records{
        {{{"id", 1}, {"root", 1}, {"actor", "p:9"}, {"things", Json::array({to_string(visible)})}},
         {{"id", 2}, {"root", 1}, {"actor", "p:9"}, {"things", Json::array({to_string(far)})}},
         {{"id", 3}, {"root", 3}, {"actor", "p:2"}, {"things", Json::array()}}}};
    vi::append_records(feed, records, identity(scene.objects), transforms(scene.objects));
    call.request.fields = {{"since", 0}};
    auto filtered = vi::read_events(feed, views, profiles, 30, 40, call).fields;
    require(filtered["events"].size() == 1 && filtered["events"][0]["id"] == 3,
            "ordinary feed excludes remote world records");
    call.principal.mode = net::Mode::Debug;
    require(vi::read_events(feed, views, profiles, 30, 40, call).fields["events"].size() == 2,
            "explicit debug feed can show retained global records");
    call.request.fields = Json::object();
    const auto previous_position = scene.objects.position[visible.slot()];
    const auto previous_rotation = scene.objects.rotation[visible.slot()];
    require(space::set_pose(scene.space, identity(scene.objects), transforms(scene.objects),
                            visible, {{10000, 0, 0}, previous_rotation})
                    .status == space::Status::Ok,
            "move observed object beyond sight");
    require(!find(look(call).fields["things"], visible),
            "next look retained an object moved out of sight in the same step");
    require(space::set_pose(scene.space, identity(scene.objects), transforms(scene.objects),
                            visible, {previous_position, previous_rotation})
                    .status == space::Status::Ok,
            "return observed object to sight");
    require(find(look(call).fields["things"], visible),
            "next look omitted an object returned to sight in the same step");
    ObjectInit zone_init;
    zone_init.model = 4;
    zone_init.material = 2;
    const auto zone = create(scene.objects, zone_init);
    const auto volume = space::sensor_box_geometry(scene.space, {10, 10, 6});
    require(space::add_volume(scene.space, identity(scene.objects), transforms(scene.objects),
                              {zone, volume, {{0, 0, 2}, {}}, space::VolumeKind::Zone})
                    .status == space::Status::Ok,
            "zone-name observation uses an actual containing volume");
    observe::set_zone(space_state.zones, {zone, 120, 1});
    model_text.rows[4] = {"Workshop", "", ""};
    require(look(call).fields["self"]["zone"] == "Workshop",
            "zone display did not inherit its model name");
    catalog_state.instance_text.rows[zone.bits] = {"Copper room", "", ""};
    require(look(call).fields["self"]["zone"] == "Copper room",
            "zone display did not use its current instance name");
    catalog_state.instance_text.rows[zone.bits].name.clear();
    require(look(call).fields["self"]["zone"] == "",
            "intentional empty instance name fell back to the model name");
    catalog_state.instance_text.rows.erase(zone.bits);
    require(look(call).fields["self"]["zone"] == "Workshop",
            "removing instance text did not restore the model name");
    model_text.rows.erase(4);
    require(look(call).fields["self"]["zone"] == "",
            "zone without authored text did not use an empty name");
    vi::remove_session(views, hearing, 11);
    require(!views.rows.contains(11) && !hearing.queues.contains(11),
            "close erases transient session read state");
    std::cout << "Viewing actual Space/VM boundary checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
