#include "catalog/catalog.hpp"
#include "settings.hpp"
#include "space/private.hpp"
#include "space_fixture.hpp"
#include <toml++/toml.hpp>

#include "runtime_fixture.hpp"
#include "step/action_private.hpp"
#include "step/run_private.hpp"

namespace {
using namespace amber;
namespace cm = step;
using test_runtime::require;
template <class F> void rejects(F action, const char *message) {
  try {
    action();
  } catch (const std::invalid_argument &) {
    return;
  }
  throw std::runtime_error(message);
}
struct Fixture {
  amber::space::State space_state;
  amber::perm::State perm_state;
  amber::step::State step_state;

  test_runtime::Fixture f;
  amber::ObjectState &objects_state = f.objects_state;
  amber::react::State &react_state = f.react_state;
  amber::catalog::State &catalog_state = f.catalog_state;

  space::Space &world = space_state.space;
  lifecycle::Parts &parts = objects_state.parts;
  Relation &declared = objects_state.declared_parts;
  std::vector<space::RelationChange> &physical_changes = space_state.relation_changes;
  space::PhysicalRelations &physical = space_state.physical_relations;
  perm::Groups &groups = perm_state.groups;
  perm::Players &players = perm_state.players;
  perm::Grants &grants = perm_state.grants;
  perm::Ledger &ledger = perm_state.ledger;
  perm::Offers &offers = perm_state.offers;
  perm::Trades &trades = perm_state.trades;
  perm::OwnerLimits &limits = perm_state.owner_limits;
  perm::Plots &plots = perm_state.plots;
  catalog::Assets &assets = catalog_state.assets;
  catalog::ParameterBlocks &parameters = catalog_state.parameters;
  step::GeometryAssets &geometry = step_state.geometry;
  step::DeliveryTargets &delivery = step_state.delivery;
  react::NativeActions &declarations = react_state.native_declarations;
  cm::CommerceDeclarations ids;
  step::LandSettings land_settings{};
  std::size_t mailbox_max = 1;
  std::array<Handle, 2> bodies;
  explicit Fixture(std::uint32_t capacity = 64)
      : space_state(
            [&] {
              auto s = testspace::settings();
              s.max_bodies = capacity;
              return s;
            }(),
            float(toml::parse_file(AMBER_CONFIG_PATH)["perception"]["LIGHT_CELL_M"]
                      .value<double>()
                      .value())) {
    const auto config = toml::parse_file(AMBER_CONFIG_PATH);
    land_settings.lot_min_m = float(config["land"]["LOT_MIN_M"].value<double>().value());
    land_settings.port_spacing_m = float(config["land"]["PORT_SPACING_M"].value<double>().value());
    players.rows.resize(4);
    ledger.balance = {{perm::Account::player(1), 100}, {perm::Account::player(2), 100}};
    plots.rows.resize(3);
    plots.rows[1] = {1, 1, 0, 0, {-20, -20, 0, 20}, {}};
    plots.rows[2] = {2, 2, 0, 0, {0, -20, 20, 20}, {}};
    perm::rebuild_plot_owners(plots);
    f.objects.owner[f.a.slot()] = 1;
    f.objects.owner[f.b.slot()] = 1;
    f.terms.rows.resize(32);
    f.terms.rows[29].id = 29;
    f.terms.rows[29].name = "base.plot_changed";
    f.terms.rows[29].kind = react::Kind::Event;
    f.terms.by_name.emplace("base.plot_changed", 29);
    for (auto [id, role] : {std::pair{30u, react::MeasurementRole::Mass},
                            std::pair{31u, react::MeasurementRole::Runtime}}) {
      f.terms.rows[id].id = id;
      f.terms.rows[id].source = react::Source::Measured;
      f.terms.rows[id].measurement = role;
    }
    f.projection->density = 10;
    catalog_state.mechanical.values["measurements"] = {{"density", 10}};
    f.assets.rows[1].volume = 1;
    f.assets.rows[2].volume = .016;
    for (auto id : {10u, 11u}) {
      f.terms.rows[id].id = id;
      f.terms.rows[id].name = id == 10 ? "novel.density" : "novel.transmission";
      f.terms.rows[id].source = react::Source::Intrinsic;
      f.terms.by_name[f.terms.rows[id].name] = id;
    }
    f.materials.rows[8] = {{10, Value(1000.)}, {11, Value(0.)}};
    f.models.rows.resize(3);
    for (ModelId model = 1; model <= 2; ++model) {
      testspace::Box shape(model == 1 ? Vec3{1, 1, 1} : Vec3{.2f, .2f, .4f});
      const auto imported = space::import_geometry(world, shape.source());
      require(imported.geometry != 0, "real Jolt fixture geometry imports");
      auto &definition = f.models.rows[model];
      definition.id = model;
      definition.author = 1;
      definition.material = 8;
      definition.asset = std::string(64, model == 1 ? 'a' : 'b');
      catalog::Asset asset;
      asset.hash = definition.asset;
      asset.volume = model == 1 ? 1 : .016;
      asset.minimum = shape.bounds.min;
      asset.maximum = shape.bounds.max;
      asset.manifest = {{"bindings", nlohmann::json::object()}};
      if (model == 2)
        asset.manifest["bindings"]["motion"] = {{"mode", "kinematic"}};
      assets.rows[asset.hash] = asset;
      geometry.rows[asset.hash] = {imported.geometry, {}};
    }
    ids = cm::declare_commerce(f.terms, declarations);
    const auto again = cm::declare_commerce(f.terms, declarations);
    require(again.pay == ids.pay, "native declaration restore is idempotent");
    bodies = {single(1, {-10, 10, 0}, true), single(2, {10, 10, 0}, true)};
    delivery.present_bodies = {{1, bodies[0]}, {2, bodies[1]}};
  }
  Handle single(PlayerId owner, Vec3 position, bool body = false) {
    ObjectInit init;
    init.model = 1;
    init.material = 8;
    init.owner = owner;
    init.position = position;
    init.preset = perm::all_rights << 16;
    init.flags = body ? Body : Static;
    const auto h = create(f.objects, init);
    const auto geometry_id = geometry.rows.at(f.models.rows[1].asset).collision;
    const auto mode = body ? space::Motion::Character : space::Motion::Static;
    const std::optional<space::CharacterSpec> character =
        body ? std::optional<space::CharacterSpec>{{.3f, 1.8f, 70, 100, 1.8f, 8, .24f, .7f, .3f}}
             : std::nullopt;
    require(
        space::add(world, identity(f.objects), transforms(f.objects),
                   {h, geometry_id, mode, {.6f, 0, 0}, body ? 70.f : 10.f, false, false, character})
                .status == space::Status::Ok,
        "real single fixture body installs");
    return h;
  }
  lifecycle::Created item(PlayerId owner, Vec3 position, bool assembled = true) {
    lifecycle::AssemblyTemplate source;
    source.model = 1;
    source.material = 8;
    if (assembled)
      source.parts.push_back({"leaf", 2, 8, {1, 0, 0}, {}});
    ObjectInit init;
    init.owner = owner;
    init.material = 8;
    init.preset = perm::all_rights << 16;
    init.maker = 3;
    init.created_step = 17;
    init.position = position;
    init.plot = owner == 1 ? 1 : 2;
    auto created = lifecycle::instantiate(f.objects, parts, declared, source, init);
    for (auto h : created.objects) {
      const auto model = f.objects.model[h.slot()];
      const auto &asset = assets.rows.at(f.models.rows[model].asset);
      const auto definition = step::describe_physics(
          geometry, asset, *f.projection, {model, 0, 8, init.plot, h}, Static, {10, 11}, {.6f, 0},
          amber::step::Clock{.step = 0, .hz = 20});
      step::install_physics(world, identity(f.objects), transforms(f.objects),
                            space_flags(f.objects), h, definition);
      permission_flags(f.objects).set(h.slot(), NoCopy);
      if (h != created.root) {

        const auto relative =
            f.objects.position[h.slot()] - f.objects.position[created.root.slot()];
        require(
            space::attach(f.objects_state, space_state, {h, created.root, {relative, {}}, false})
                    .status == space::Status::Ok,
            "declared fixture part attaches");
      }
    }
    return created;
  }
  step::NativeCompletion call(TermId action, Caller actor, nlohmann::json args = {},
                              Handle target = {}) {
    if (args.is_null())
      args = nlohmann::json::object();
    const auto operation = std::get<react::ExchangeAction>(declarations.rows.at(action).operation);
    amber::Settings settings{};
    settings.mailbox_max = mailbox_max;
    settings.land = land_settings;
    auto out =
        step::native_exchange(objects_state, perm_state, space_state, delivery, settings, operation,
                              {{actor, action, target, {}}, std::move(args), {}, 20, 20, {}}, 29);
    const auto custody = std::move(out.custody);
    step::apply_custody(f.objects_state, space_state, {0, 0, 3, 0}, plots, custody, out);
    step::refresh_object_plots(world, objects_state, react_state, catalog_state, {20, 20}, {}, out);
    return out;
  }
  step::NativeCompletion place(Caller actor, nlohmann::json args, Handle target) {
    auto out = step::place_existing(
        f.objects_state, space_state, {0, 0, 3, 0}, perm_state, geometry, f.catalog_state, {10, 11},
        {.6f, 0}, f.react_state,
        {{actor, ids.place_existing, target, {}}, std::move(args), {}, 20, 20, {}});
    step::refresh_object_plots(world, objects_state, react_state, catalog_state, {20, 20}, {}, out);
    return out;
  }

  bool visible(Handle h) { return space::bounds(world, identity(f.objects), h).has_value(); }
  void store(Handle h, PlayerId owner) {
    step::NativeCompletion out;
    const std::array custody{perm::CustodyChange{h, bodies[owner - 1], false}};
    step::apply_custody(objects_state, space_state, {0, 0, 3, 0}, plots, custody, out);
    step::refresh_object_plots(world, objects_state, react_state, catalog_state, {20, 20}, {}, out);
    players.rows[owner].inventory.push_back(h);
  }
};
void wallet_native_authority_is_explicit() {
  Fixture f;
  const Caller script = ScriptHolder{77, f.f.a};
  require(
      !f.call(f.ids.pay, script, {{"from", "p:1"}, {"to", "p:2"}, {"amount", 10}}).reply.at("ok"),
      "native script inherited owner Spend");
  const auto grant =
      perm::create_grant(f.grants, f.players, perm::Holder::player(1),
                         {1, perm::Holder::script(77, f.f.a),
                          perm::Scope::account(perm::Account::player(1)), 1, 20, 100, 500},
                         {perm::Scope::account(perm::Account::player(1)), 1, 1, 0, false},
                         {perm::Holder::script(77, f.f.a), 1, true}, 1);
  require(bool(grant), "native wallet fixture grant");
  require(f.call(f.ids.pay, script, {{"from", "p:1"}, {"to", "p:2"}, {"amount", 15}}).changed &&
              f.ledger.balance[perm::Account::player(1)] == 85 &&
              f.ledger.balance[perm::Account::player(2)] == 115,
          "native payment did not use player wallets");
  require(!f.call(f.ids.pay, script, {{"from", "p:1"}, {"to", "p:2"}, {"amount", 6}}).changed &&
              f.grants.rows[grant.grant].spent == 15 &&
              f.ledger.balance[perm::Account::player(1)] == 85,
          "failed native payment consumed allowance or money");
  require(
      !f.call(f.ids.pay, ScriptHolder{77, f.f.b}, {{"from", "p:1"}, {"to", "p:2"}, {"amount", 1}})
           .changed,
      "Spend leaked across installations");
  rejects([&] { f.call(f.ids.pay, PlayerId{1}, {{"to", "group:1"}, {"amount", 1}}); },
          "unknown group account accepted");
  require(f.call(f.ids.balance, script).reply.at("balance") == 0,
          "script default balance is its distinct installed-object account");
}
void actual_assembly_sale_and_inventory_reveal_preserve_identity() {
  Fixture f(4);
  const auto item = f.item(1, {-4, 0, 0});
  const auto child = item.objects[1];
  const auto original_maker = f.f.objects.maker[item.root.slot()];
  const auto granted =
      perm::create_grant(f.grants, f.players, perm::Holder::player(1),
                         {1, perm::Holder::script(77, child),
                          perm::Scope::account(perm::Account::player(1)), 1, 20, 100, 500},
                         {perm::Scope::account(perm::Account::player(1)), 1, 1, 0, false},
                         {perm::Holder::script(77, child), 1, true}, 1);
  require(bool(granted), "installed part grant fixture");
  rejects([&] { f.call(f.ids.offer, PlayerId{1}, {{"price", 40}}, child); },
          "internal part sold separately");
  const auto offered = f.call(f.ids.offer, PlayerId{1}, {{"price", 40}}, item.root);
  require(offered.changed && f.visible(item.root) && f.visible(child) &&
              (f.f.objects.flags[child.slot()] & Escrow),
          "escrow hid actual goods or omitted part");
  const auto offer = offered.reply.at("offer");
  f.ledger.balance[perm::Account::player(2)] = 39;
  require(!f.call(f.ids.accept, PlayerId{2}, {{"offer", offer}, {"max_price", 50}}).changed &&
              f.visible(item.root) && f.f.objects.owner[item.root.slot()] == 1 &&
              f.ledger.balance[perm::Account::player(1)] == 100,
          "poor acceptance changed valuables or scene");
  f.ledger.balance[perm::Account::player(2)] = 100;
  f.delivery.present_bodies.erase(2);
  f.players.rows[2].mailbox.push_back(f.f.a);
  require(!f.call(f.ids.accept, PlayerId{2}, {{"offer", offer}, {"max_price", 50}}).changed &&
              f.visible(child) && f.ledger.balance[perm::Account::player(2)] == 100,
          "mailbox overflow consumed offered goods or payment");
  f.delivery.present_bodies[2] = f.bodies[1];
  f.players.rows[2].mailbox.clear();
  const auto accepted = f.call(f.ids.accept, PlayerId{2}, {{"offer", offer}, {"max_price", 50}});
  require(accepted.changed && f.ledger.balance[perm::Account::player(1)] == 140 &&
              f.ledger.balance[perm::Account::player(2)] == 60 &&
              f.f.objects.owner[child.slot()] == 2 && !f.grants.rows[granted.grant].live &&
              !f.visible(item.root) && !f.visible(child) &&
              f.players.rows[2].inventory == std::vector<Handle>{item.root},
          "native sale failed complete physical/financial/grant transfer");
  require(!accepted.reply.contains("revoked") && accepted.notices.size() == 1 &&
              accepted.notices[0].first == 1 &&
              accepted.notices[0].second ==
                  nlohmann::json{{"kind", "changed"},
                                 {"on", to_string(item.root)},
                                 {"cleared", {"g:" + std::to_string(granted.grant)}}} &&
              accepted.records.back().detail["cleared"] == accepted.notices[0].second["cleared"],
          "sale sends cleared grant ids to the prior owner and journal, not the buyer");
  const auto blocker_a = f.single(1, {-10, -10, 0}), blocker_b = f.single(1, {-8, -10, 0});
  const auto before = f.f.objects.position[item.root.slot()];
  rejects([&] { f.place(PlayerId{2}, {{"position", {4, 5, 0}}}, item.root); },
          "full native body pool accepted an inventory reveal");
  require(f.f.objects.position[item.root.slot()] == before && !f.visible(item.root) &&
              f.players.rows[2].inventory.size() == 1,
          "native capacity failure moved or lost inventory");
  std::vector<space::RelationChange> changes;
  for (auto h : {blocker_a, blocker_b})
    space::remove(f.world, identity(f.f.objects), h, parents(f.f.objects), space_flags(f.f.objects),
                  f.physical, changes);
  require(!f.place(PlayerId{2}, {{"position", {-4, 5, 0}}}, item.root).changed &&
              !f.visible(item.root),
          "foreign land Build denial changed custody");
  const auto placed = f.place(
      PlayerId{2}, {{"position", {4, 5, 0}}, {"rotation", {0, 0, std::sqrt(.5), std::sqrt(.5)}}},
      item.root);
  require(placed.changed && f.visible(item.root) && f.visible(child) &&
              f.players.rows[2].inventory.empty() &&
              f.f.objects.maker[item.root.slot()] == original_maker &&
              f.f.objects.created_step[item.root.slot()] == 17 &&
              (f.f.objects.flags[item.root.slot()] & NoCopy) &&
              f.f.objects.model[item.root.slot()] == 1 &&
              (f.f.objects.position[child.slot()] - Vec3{4, 6, 0}).length() < .001f &&
              f.f.objects.plot[child.slot()] == 2,
          "existing placement failed rigid closure or changed identity/attribution");
  const auto ray = space::raycast(f.world, identity(f.f.objects), {4, 4, .5f}, {4, 5.5f, .5f}, {});
  require(ray && ray->object == item.root, "revealed inventory has no actual Jolt query shape");
  require(space::body_motion(f.world, identity(f.f.objects), child)->motion ==
              space::Motion::Kinematic,
          "authored moving part became static on reveal");
}

void ordinary_offer_expiry_releases_actual_assembly_without_moving_it() {
  Fixture f;
  const auto item = f.item(1, {-4, 0, 0});
  const auto before = f.f.objects;
  const auto offered =
      f.call(f.ids.offer, PlayerId{1}, {{"price", 40}, {"expires_s", 0.5}}, item.root);
  require(offered.changed && f.offers.expiring.size() == 1,
          "actual native ordinary offer registers its finite deadline");
  const auto id = f.offers.expiring.begin()->second;
  require(cm::advance_offer_expiry(f.offers, identity(f.f.objects), permission_flags(f.f.objects),
                                   29, 256)
              .empty(),
          "native sale expired before its due step");
  for (auto h : item.objects)
    require((f.f.objects.flags[h.slot()] & Escrow) && f.visible(h),
            "ordinary sale keeps its actual root and part visible in escrow");
  const auto expired = cm::advance_offer_expiry(f.offers, identity(f.f.objects),
                                                permission_flags(f.f.objects), 30, 256);
  require(expired.size() == 1 && expired[0].offer == id && expired[0].seller == 1 &&
              expired[0].goods == item.root && expired[0].cleared == item.objects &&
              !f.offers.rows[id].live && f.offers.expiring.empty() &&
              cm::advance_offer_expiry(f.offers, identity(f.f.objects),
                                       permission_flags(f.f.objects), 31, 256)
                  .empty(),
          "exact deadline returns one narrow seller notice and never changes scene custody");
  for (auto h : item.objects) {
    const auto slot = h.slot();
    require(!(f.f.objects.flags[slot] & Escrow) && (f.f.objects.flags[slot] & NoCopy) &&
                f.f.objects.owner[slot] == before.owner[slot] &&
                f.f.objects.maker[slot] == before.maker[slot] &&
                f.f.objects.model[slot] == before.model[slot] &&
                f.f.objects.parent[slot] == before.parent[slot] &&
                f.f.objects.position[slot] == before.position[slot] && f.visible(h),
            "expiry clears only escrow on the original Jolt assembly without moving or copying it");
  }
  require(f.call(f.ids.offer, PlayerId{1}, {{"price", 10}}, item.root).changed,
          "expired actual goods are immediately available for a new ordinary offer");
}
void trade_changes_cancel_and_complete_one_swap() {
  Fixture f;
  const auto a = f.item(1, {-4, 0, 0}, false), b = f.item(2, {4, 0, 0}, false);
  f.store(b.root, 2);
  const auto opened = f.call(f.ids.trade_open, PlayerId{1}, {{"other", "p:2"}});
  const auto id = opened.reply.at("trade");
  require(f.call(f.ids.trade_change, PlayerId{1},
                 {{"trade", id}, {"money", 10}, {"goods", to_string(a.root)}})
                  .changed &&
              f.call(f.ids.trade_change, PlayerId{2},
                     {{"trade", id}, {"money", 5}, {"goods", to_string(b.root)}})
                  .changed,
          "native trade side setup");
  require(f.visible(a.root) && !f.visible(b.root),
          "trade escrow changed placed/inventory visibility");
  require(f.call(f.ids.trade_confirm, PlayerId{1}, {{"trade", id}}).changed &&
              f.trades.rows[1].sides[0].confirmed,
          "first confirmation missing");
  f.call(f.ids.trade_change, PlayerId{2},
         {{"trade", id}, {"money", 6}, {"goods", to_string(b.root)}});
  require(!f.trades.rows[1].sides[0].confirmed && !f.trades.rows[1].sides[1].confirmed,
          "native edit retained confirmation");
  require(f.call(f.ids.trade_cancel, PlayerId{1}, {{"trade", id}}).changed && f.visible(a.root) &&
              !f.visible(b.root) && !f.f.objects.parent[b.root.slot()] &&
              !(f.f.objects.flags[a.root.slot()] & Escrow),
          "cancel must keep the escrow custody effect and clear its quote");
  const auto next = f.call(f.ids.trade_open, PlayerId{1}, {{"other", "p:2"}}).reply.at("trade");
  f.call(f.ids.trade_change, PlayerId{1},
         {{"trade", next}, {"money", 10}, {"goods", to_string(a.root)}});
  f.call(f.ids.trade_change, PlayerId{2},
         {{"trade", next}, {"money", 6}, {"goods", to_string(b.root)}});
  f.call(f.ids.trade_confirm, PlayerId{1}, {{"trade", next}});
  f.ledger.balance[perm::Account::player(2)] = 5;
  require(!f.call(f.ids.trade_confirm, PlayerId{2}, {{"trade", next}}).changed &&
              f.f.objects.owner[a.root.slot()] == 1 && f.visible(a.root),
          "failed second confirmation changed valuable side");
  const auto old_grant =
      perm::create_grant(f.grants, f.players, perm::Holder::player(1),
                         {1, perm::Holder::player(3), perm::Scope::object(a.root),
                          perm::bit(perm::Verb::Use), 0, 0, 0},
                         {perm::Scope::object(a.root), 1, perm::all_rights, 0, false},
                         {perm::Holder::player(3), 3, true}, 20);
  require(bool(old_grant), "trade goods have a grant to clear");
  f.ledger.balance[perm::Account::player(2)] = 100;
  const auto completed = f.call(f.ids.trade_confirm, PlayerId{2}, {{"trade", next}});
  require(completed.changed && completed.reply.at("completed") &&
              f.ledger.balance[perm::Account::player(1)] == 96 &&
              f.ledger.balance[perm::Account::player(2)] == 104 &&
              f.f.objects.owner[a.root.slot()] == 2 && f.f.objects.owner[b.root.slot()] == 1 &&
              !f.visible(a.root) && !f.visible(b.root),
          "native trade did not atomically exchange player money and real custody");
  require(!completed.reply.contains("revoked") && completed.notices.size() == 1 &&
              completed.notices[0].first == 1 &&
              completed.notices[0].second["on"] == to_string(a.root) &&
              completed.notices[0].second["cleared"] ==
                  nlohmann::json::array({"g:" + std::to_string(old_grant.grant)}),
          "trade reports only the former owner's cleared goods grants");
}
void gifts_bounce_and_keep_unrelated_seated_bodies_alive() {
  Fixture f;
  const auto item = f.item(1, {-4, 0, 0});

  require(space::attach(f.f.objects_state, f.space_state,
                        {f.bodies[1], item.root, {{0, 0, 1}, {}}, false})
                  .status == space::Status::Ok,
          "unrelated occupant really attaches to gifted furniture");
  f.players.rows[3].mailbox = {f.f.a};
  require(!f.call(f.ids.give, PlayerId{1}, {{"recipient", "p:3"}}, item.root).changed &&
              f.visible(item.root) && f.f.objects.parent[f.bodies[1].slot()] == item.root &&
              f.f.objects.owner[item.root.slot()] == 1,
          "bounced native gift changed ownership or occupant");
  f.players.rows[3].mailbox.clear();
  require(f.call(f.ids.give, PlayerId{1}, {{"recipient", "p:3"}}, item.root).changed &&
              !f.visible(item.root) && f.visible(f.bodies[1]) &&
              identity(f.f.objects).valid(f.bodies[1]) && !f.f.objects.parent[f.bodies[1].slot()] &&
              f.f.objects.owner[f.bodies[1].slot()] == 2 &&
              f.players.rows[3].mailbox == std::vector<Handle>{item.root},
          "gift removed an ordinary seated occupant or lost offline delivery");
}

void cancelling_a_visible_held_trade_keeps_its_escrow_pose() {
  Fixture f;
  const auto item = f.item(1, {-4, 0, 0}, false);

  require(space::attach(f.f.objects_state, f.space_state,
                        {item.root, f.bodies[0], {{.5f, 0, 1}, {}}, true})
                  .status == space::Status::Ok,
          "visible held fixture is a real physical grasp");
  const auto position = f.f.objects.position[item.root.slot()];
  const auto id = f.call(f.ids.trade_open, PlayerId{1}, {{"other", "p:2"}}).reply.at("trade");
  require(f.call(f.ids.trade_change, PlayerId{1},
                 {{"trade", id}, {"money", 0}, {"goods", to_string(item.root)}})
                  .changed &&
              f.visible(item.root) && !f.f.objects.parent[item.root.slot()] &&
              !(f.f.objects.flags[item.root.slot()] & Held),
          "quoting visible goods clears the grasp without changing their world pose");
  require(f.call(f.ids.trade_cancel, PlayerId{1}, {{"trade", id}}).changed &&
              f.visible(item.root) && !f.f.objects.parent[item.root.slot()] &&
              !(f.f.objects.flags[item.root.slot()] & Held) &&
              f.f.objects.position[item.root.slot()] == position,
          "trade cancellation must leave the completed detach applied without teleporting goods");
}

void placement_and_custody_use_current_priority_fallback() {
  Fixture f;
  const auto item = f.item(1, {-4, 0, 0}, false);
  f.store(item.root, 1);
  auto term = [&](const char *name, react::Source source, react::Combine combine) {
    const auto id = TermId(f.f.terms.rows.size());
    react::Term row;
    row.id = id;
    row.name = name;
    row.source = source;
    row.combine = combine;
    f.f.terms.rows.push_back(row);
    f.f.terms.by_name[name] = id;
    return id;
  };
  const auto priority = term("novel.priority", react::Source::Setting, react::Combine::None);
  const auto supply = term("novel.supply", react::Source::State, react::Combine::Sum);
  const auto demand = term("novel.demand", react::Source::State, react::Combine::SharePriority);
  f.f.terms.rows[demand].priority = priority;
  f.f.terms.rows[demand].paired = supply;
  f.f.world.values[priority] = Value(5.);
  f.f.plots.rows[2][priority] = Value(3.);
  f.f.instance_settings.columns.resize(f.f.terms.rows.size());
  f.f.instance_settings.columns[priority].set(f.f.b.slot(), Value(4.));
  react::DeferredEffects effects;
  react::set_contribution(f.f.shared, f.f.gives_to, f.f.terms.rows[supply], f.f.c, f.f.a, 1,
                          effects);
  react::set_contribution(f.f.shared, f.f.gives_to, f.f.terms.rows[demand], item.root, f.f.a, 1,
                          effects);
  react::set_contribution(f.f.shared, f.f.gives_to, f.f.terms.rows[demand], f.f.b, f.f.a, 1,
                          effects);
  react::reconcile_allocations(*f.f.projection, f.f.react_state, effects, 20, 20);
  require(react::get_share(f.f.allocations, f.f.a, demand, item.root) == 1,
          "initial priority allocation");
  f.plots.rows[2].preset = 1;
  require(f.place(PlayerId{1}, {{"position", {4, 5, 0}}}, item.root).changed &&
              react::get_share(f.f.allocations, f.f.a, demand, item.root) == 0 &&
              react::get_share(f.f.allocations, f.f.a, demand, f.f.b) == 1,
          "placement immediately uses the destination plot's lower priority");
  f.store(item.root, 1);
  require(react::get_share(f.f.allocations, f.f.a, demand, item.root) == 1 &&
              react::get_share(f.f.allocations, f.f.a, demand, f.f.b) == 0,
          "inventory custody immediately restores the world priority fallback");
  f.f.plots.rows[2][priority] = Value(5.);
  require(f.place(PlayerId{1}, {{"position", {4, 5, 0}}}, item.root).changed &&
              react::get_share(f.f.allocations, f.f.a, demand, item.root) == 1,
          "equal-priority placement preserves the current allocation");
  f.f.plots.rows[2][priority] = Value(3.);
  effects = {};
  react::reconcile_priority(*f.f.projection, f.f.react_state,
                            {react::SettingKey::Scope::Plot, 2, priority}, effects, 21, 20);
  require(react::get_share(f.f.allocations, f.f.a, demand, item.root) == 0 &&
              react::get_share(f.f.allocations, f.f.a, demand, f.f.b) == 1,
          "same effective priority on placement left a stale old-plot dependency");
}

void placement_enables_a_plot_selected_process_rule() {
  Fixture f;
  const auto item = f.item(1, {-4, 0, 0}, false);
  f.store(item.root, 1);
  f.f.terms.rows[5].source = react::Source::Setting;
  f.f.world.values[5] = Value(false);
  f.f.plots.rows[2][5] = Value(true);
  const auto rule = f.f.add(R"(return {kind='rule',applies_to={flag='novel.ready'},
    watches={{term='novel.heat',thresholds={{predicate='above',value=2}}}},on={
    ['novel.heat']=function(self,_,value)
      self.state.count=(self.state.count or 0)+1;self.state.last=value end},
    tests={{when={steps=0},expect={subject='target',term='novel.heat',equals=0}}}})");
  react::add_rule(f.f.rules, f.f.scripts, f.f.terms, rule);
  react::project_rules(f.f.triggers, f.f.rules);
  f.f.state(item.root, 1, 0);
  react::set_rate(f.react_state, f.f.terms.rows[1], item.root, 1, 0, 4, 0, 20, f.f.effects);
  record::Journal journal(100, {});
  const auto publish = [&](react::DeferredEffects effects, Step now) {
    step::publish_effects(std::move(effects), {}, now, f.step_state.queues, f.step_state.timers,
                          journal, 1000);
  };
  const auto drain = [&](Step now) {
    step::promote_due(f.step_state.timers, now, f.step_state.queues);
    while (auto work = step::pop_foreground(f.step_state.queues)) {
      auto plans = step::prepare_reactive(f.react_state, f.f.values(), f.f.scripts, f.step_state,
                                          std::move(*work), now, 20);
      if (!plans)
        continue;
      f.f.run(*plans, now);
      publish(std::move(f.f.effects), now);
      f.f.effects = {};
    }
  };
  publish(std::move(f.f.effects), 0);
  f.f.effects = {};
  drain(0);
  require(!f.f.states.contains({item.root, rule}),
          "the running process is silent while the inherited selector is false");
  f.plots.rows[2].preset = 1;
  auto placed = f.place(PlayerId{1}, {{"position", {4, 5, 0}}}, item.root);
  require(placed.changed && f.f.objects.plot[item.root.slot()] == 2 &&
              react::read(f.f.values(), item.root, 5, 20, 20).value == Value(true),
          "real inventory placement enables the destination plot's selector flag");
  publish(std::move(placed.effects), 20);
  drain(20);
  require(!f.f.states.contains({item.root, rule}),
          "placement changes rule eligibility without invoking the quantity handler");
  drain(40);
  require(!f.f.states.contains({item.root, rule}), "the strict threshold is silent at equality");
  drain(41);
  drain(80);
  const auto &state = f.f.states.at({item.root, rule}).values;
  require(std::get<double>(state.at(std::string("count"))) == 1 &&
              std::abs(std::get<double>(state.at(std::string("last"))) - 2.05) < 1e-9,
          "the plot-selected rule receives its real process crossing once after placement");
}

void logical_inventory_root_reveals_its_actual_declared_part() {
  Fixture f;
  auto &asset = f.assets.rows.at(f.f.models.rows[1].asset);
  asset.volume = 0;
  f.geometry.rows[asset.hash].collision = 0;
  const auto item = f.item(1, {-4, 0, 0});
  f.store(item.root, 1);
  const auto result = f.place(PlayerId{1}, {{"position", {-4, 5, 0}}}, item.root);
  require(result.changed && space::is_logical_root(f.world, identity(f.f.objects), item.root) &&
              f.visible(item.objects[1]) && !f.players.rows[1].inventory.size(),
          "existing logical root lost its visible physical part on reveal");
  const auto ray = space::raycast(f.world, identity(f.f.objects), {-3, 4, .2f}, {-3, 6, .2f}, {});
  require(ray && ray->object == item.objects[1],
          "logical assembly child is absent from native queries");
}

void script_inventory_placement_uses_its_own_grants() {
  Fixture f;
  const auto item = f.item(1, {-4, 0, 0}, false);
  f.store(item.root, 1);
  const ScriptHolder script{99, f.f.a};
  const auto args = nlohmann::json{{"position", {-4, 5, 0}}};
  require(!f.place(script, args, item.root).changed && !f.visible(item.root),
          "script borrowed the inventory owner's Move/Build authority");
  const auto holder = perm::Holder::script(script.script, script.object);
  const auto move = perm::create_grant(
      f.grants, f.players, perm::Holder::player(1),
      {1, holder, perm::Scope::object(item.root), perm::bit(perm::Verb::Move), 0, 0, 500},
      {perm::Scope::object(item.root), 1, perm::all_rights, 1, false}, {holder, 1, true}, 1);
  require(bool(move) && !f.place(script, args, item.root).changed && !f.visible(item.root),
          "script Move grant borrowed owner's land Build");
  const auto build = perm::create_grant(
      f.grants, f.players, perm::Holder::player(1), {1, holder, perm::Scope::plot(1), 1, 0, 0, 500},
      {perm::Scope::plot(1), 1, 7, 1, false}, {holder, 1, true}, 1);
  require(bool(build) && f.place(script, args, item.root).changed && f.visible(item.root) &&
              f.f.objects.owner[item.root.slot()] == 1 && f.players.rows[1].inventory.empty(),
          "authorized script did not place existing owner's item under its actual grants");
}

void retirement_keeps_the_surviving_trade_side_before_free() {
  Fixture f;
  const auto a = f.item(1, {-4, 0, 0}), b = f.item(2, {4, 0, 0});
  f.store(b.root, 2);
  const auto id = f.call(f.ids.trade_open, PlayerId{1}, {{"other", "p:2"}}).reply.at("trade");
  f.call(f.ids.trade_change, PlayerId{1},
         {{"trade", id}, {"money", 0}, {"goods", to_string(a.root)}});
  f.call(f.ids.trade_change, PlayerId{2},
         {{"trade", id}, {"money", 0}, {"goods", to_string(b.root)}});
  const std::array<Handle, 1> doomed{a.root};
  const auto closure = lifecycle::collect(f.parts, identity(f.f.objects), doomed);
  step::NativeCompletion cleanup;
  step::retire_commerce(f.perm_state, f.f.objects_state, closure, 20, cleanup);
  std::vector<space::RelationChange> changes;
  for (auto h : closure)
    space::remove(f.world, identity(f.f.objects), h, parents(f.f.objects), space_flags(f.f.objects),
                  f.physical, changes);
  lifecycle::release_rows(f.f.objects, f.parts, f.declared, closure);
  require(!identity(f.f.objects).valid(a.root) && identity(f.f.objects).valid(b.root) &&
              !f.trades.rows[1].live && !(f.f.objects.flags[b.root.slot()] & Escrow) &&
              !f.visible(b.root) && !f.f.objects.parent[b.root.slot()] &&
              f.players.rows[2].inventory == std::vector<Handle>{b.root} && cleanup.changed,
          "retirement cancelled money rows but lost surviving physical custody");
}
void rebind_invalidates_only_affected_sales_with_actual_new_parts() {
  Fixture f;
  const auto item = f.item(1, {-4, 0, 0});
  const auto untouched = f.item(1, {-8, 0, 0}, false);
  require(f.call(f.ids.offer, PlayerId{1}, {{"price", 7}}, item.root).changed &&
              f.call(f.ids.offer, PlayerId{1}, {{"price", 11}}, untouched.root).changed,
          "actual sale fixtures enter visible escrow");
  const auto added = f.single(1, {-4, 1, 0});
  auto parts = *f.parts.get(item.root.slot());
  parts.push_back({"new_part", added});
  f.parts.set(item.root.slot(), std::move(parts));
  lifecycle::rebuild_declared_parts(f.parts, identity(f.f.objects), f.declared);

  require(
      space::attach(f.f.objects_state, f.space_state, {added, item.root, {{0, 1, 0}, {}}, false})
              .status == space::Status::Ok,
      "new definition part has real native geometry and attachment");
  permission_flags(f.f.objects).set(added.slot(), Escrow);
  const auto old_pose = f.f.objects.position[item.root.slot()];
  const auto balances = f.ledger.balance;
  const std::array<Handle, 1> rebound{item.root};
  auto cancellation = cm::rebind_offers(f.offers, f.trades, permission_flags(f.f.objects),
                                        identity(f.f.objects), f.parts, rebound, 30);
  require(!f.offers.rows[1].live && f.offers.rows[2].live &&
              (f.f.objects.flags[untouched.root.slot()] & Escrow) &&
              !(f.f.objects.flags[item.root.slot()] & Escrow) &&
              !(f.f.objects.flags[item.objects[1].slot()] & Escrow) &&
              !(f.f.objects.flags[added.slot()] & Escrow) && f.visible(item.root) &&
              f.visible(added) && f.f.objects.position[item.root.slot()] == old_pose &&
              f.ledger.balance == balances && cancellation.changed,
          "definition invalidation used stale parts, moved goods, or cancelled unrelated sale");
  record::Journal journal(100, {});
  amber::Origin origin;
  origin.cause = 91;
  for (auto &row : cancellation.records)
    journal.append(std::move(row), origin);
  const auto records = journal.take();
  require(records.size() == 1 && records[0]["name"] == "goods_offers_invalidated" &&
              records[0]["cause"] == 91 && records[0]["reason"] == "definition_changed" &&
              records[0]["offers"] == nlohmann::json::array({"offer:1"}) &&
              records[0]["things"].size() == 3,
          "changed sale lacks causal reason or actual new part closure");
  cancellation = cm::rebind_offers(f.offers, f.trades, permission_flags(f.f.objects),
                                   identity(f.f.objects), f.parts, rebound, 31);
  require(!cancellation.changed && cancellation.records.empty(), "quote cancellation was replayed");
}
void rebind_trade_keeps_current_custody_at_full_capacity() {
  Fixture f;
  const auto held = f.item(1, {-4, 0, 0});
  const auto stored = f.item(2, {4, 0, 0});
  f.store(stored.root, 2);

  require(space::attach(f.f.objects_state, f.space_state,
                        {held.root, f.bodies[0], {{.5f, 0, 1}, {}}, true})
                  .status == space::Status::Ok,
          "trade fixture uses a visible physical grasp");
  const auto position = f.f.objects.position[held.root.slot()];
  const auto id = f.call(f.ids.trade_open, PlayerId{1}, {{"other", "p:2"}}).reply.at("trade");
  f.call(f.ids.trade_change, PlayerId{1},
         {{"trade", id}, {"money", 5}, {"goods", to_string(held.root)}});
  f.call(f.ids.trade_change, PlayerId{2},
         {{"trade", id}, {"money", 3}, {"goods", to_string(stored.root)}});
  require(f.call(f.ids.trade_confirm, PlayerId{1}, {{"trade", id}}).changed &&
              f.trades.rows[1].sides[0].confirmed && f.visible(held.root) &&
              !f.visible(stored.root),
          "quoted trade does not preserve each side's actual scene membership");
  while (space::capacity(f.world).native_available)
    f.single(1, {15, 15, 0});
  const auto balance = f.ledger.balance;
  const auto stored_inventory = f.players.rows[2].inventory;
  const std::array<Handle, 2> rebound{held.root, stored.root};
  const auto cancellation = cm::rebind_offers(f.offers, f.trades, permission_flags(f.f.objects),
                                              identity(f.f.objects), f.parts, rebound, 20);
  require(cancellation.changed && !f.trades.rows[1].live && !f.trades.rows[1].sides[0].confirmed &&
              !f.trades.rows[1].sides[1].confirmed && !f.offers.rows[1].live &&
              !f.offers.rows[2].live && f.visible(held.root) && !f.visible(stored.root) &&
              !f.f.objects.parent[held.root.slot()] && !f.f.objects.parent[stored.root.slot()] &&
              !(f.f.objects.flags[held.root.slot()] & Held) &&
              !(f.f.objects.flags[held.root.slot()] & Escrow) &&
              !(f.f.objects.flags[stored.root.slot()] & Escrow) &&
              f.f.objects.position[held.root.slot()] == position &&
              f.players.rows[2].inventory == stored_inventory && f.ledger.balance == balance &&
              space::capacity(f.world).native_available == 0,
          "rebinding trade goods undid custody, retained confirmations or changed money/capacity");
}
void offline_notices_survive_restore_and_do_not_fill_the_goods_mailbox() {
  using Json = nlohmann::json;
  Fixture f;
  net::Profiles profiles;
  net::put_profile(profiles, {2,
                              "Mailbox recipient",
                              f.bodies[1],
                              {},
                              net::Mode::Play | net::Mode::Watch,
                              {net::hash_key("mailbox-fixture")}});
  net::Sessions sessions(profiles);
  const auto connect = [&](net::SessionId id, const char *mode) {
    sessions.accept(net::Connected{id});
    sessions.accept(net::Control{id, Json{{"t", "hello"},
                                          {"version", 1},
                                          {"key", "mailbox-fixture"},
                                          {"mode", mode},
                                          {"binary", false}}
                                         .dump()});
    require(sessions.principal(id).has_value(), "mailbox fixture authenticates actual sessions");
  };
  connect(10, "watch");
  cm::notify(f.players, sessions, 2, 0, {{"kind", "ignored"}});
  for (int i = 0; i < 3; ++i)
    cm::notify(f.players, sessions, 2, 2,
               {{"kind", "changed"}, {"plot", 2}, {"sequence", i}, {"cleared", {"g:1"}}});
  const auto grant = perm::create_grant(
      f.grants, f.players, perm::Holder::player(2),
      {2, perm::Holder::player(1), perm::Scope::plot(2), 1, 0, 0, 500},
      {perm::Scope::plot(2), 2, 31, 2, true}, {perm::Holder::player(1), 1, false}, 1);
  require(bool(grant), "offline former owner has an actual land grant to remove");
  // The market's plot transfer is Phase 5. Exercise its mailbox contract with
  // the plain owner write and actual grant revocation, without adding a market here.
  const auto former_owner = f.plots.rows[2].owner;
  perm::set_plot_owner(f.plots, 2, 1);
  const auto removed = perm::revoke(f.grants, f.players, grant.grant);
  require(removed == std::vector<GrantId>{grant.grant}, "plot transition removes the actual grant");
  cm::notify(f.players, sessions, 2, former_owner,
             {{"kind", "changed"},
              {"plot", 2},
              {"sequence", 3},
              {"cleared", {"g:" + std::to_string(removed[0])}}});
  require(f.players.rows[0].notices.empty() && f.players.rows[2].notices.size() == 2 &&
              f.players.rows[2].notices[0]["sequence"] == 2 &&
              f.players.rows[2].notices_dropped == 2,
          "watch sessions do not consume offline notices; overflow keeps the newest notices");
  const auto gift = f.item(1, {-4, 0, 0}, false);
  f.delivery.present_bodies.erase(2);
  require(f.call(f.ids.give, PlayerId{1}, {{"recipient", "p:2"}}, gift.root).changed &&
              f.players.rows[2].mailbox == std::vector<Handle>{gift.root},
          "a full notice mailbox must not prevent a valuable gift");
  snap::Buffer bytes;
  snap::Output output{bytes};
  perm::write_players(output, f.players, f.ledger, f.plots, f.groups);
  bytes.resize(output.adapter().writtenBytesCount());
  perm::Players restored;
  perm::Ledger ledger;
  perm::Plots plots;
  perm::Groups groups;
  snap::Input input{bytes.data(), bytes.size()};
  perm::read_players(input, restored, ledger, plots, groups);
  snap::check_complete(input);
  require(restored.rows[2].notices == f.players.rows[2].notices &&
              restored.rows[2].notices_dropped == 2 &&
              restored.rows[2].mailbox == f.players.rows[2].mailbox,
          "waiting notices, overflow count and valuable goods survive snapshot restore");
  f.players = std::move(restored);
  connect(11, "play");
  const auto delivered =
      cm::deliver_mailbox(f.players, sessions, f.f.objects_state, f.world, 2, f.bodies[1], 22);
  require(delivered.reply["delivered"] == 1 && f.players.rows[2].mailbox.empty() &&
              f.players.rows[2].inventory == std::vector<Handle>{gift.root} &&
              f.players.rows[2].notices.empty() && f.players.rows[2].notices_dropped == 0,
          "login drains notices and goods independently and clears their waiting state");
  cm::notify(f.players, sessions, 2, 2, {{"kind", "live"}});
  std::vector<net::Invocation> ready;
  std::vector<net::SessionNotice> logins;
  std::vector<net::SessionOutput> outputs;
  sessions.drain(ready, logins, outputs);
  std::vector<Json> events;
  for (const auto &out : outputs)
    if (const auto *sent = std::get_if<net::Send>(&out);
        sent && sent->message.value("t", "") == "event") {
      require(sent->session == 11, "waiting notices must only go to the play session");
      events.push_back(sent->message);
    }
  require(events.size() == 3 && events[0]["sequence"] == 2 && events[0]["dropped"] == 2 &&
              events[0]["cleared"] == Json::array({"g:1"}) && events[1]["sequence"] == 3 &&
              events[1]["plot"] == 2 &&
              events[1]["cleared"] == Json::array({"g:" + std::to_string(grant.grant)}) &&
              !events[1].contains("dropped") && events[2]["kind"] == "live" &&
              f.players.rows[2].notices.empty(),
          "login sends restored notices oldest first, reports overflow once, then sends live");
}
void login_delivers_complete_mailbox_without_native_capacity_or_identity_changes() {
  Fixture f;
  const auto first = f.item(1, {-4, 0, 0}), second = f.item(1, {-8, 0, 0}, false);
  f.delivery.present_bodies.erase(2);
  f.mailbox_max = 10;
  require(f.call(f.ids.give, PlayerId{1}, {{"recipient", "p:2"}}, first.root).changed &&
              f.call(f.ids.give, PlayerId{1}, {{"recipient", "p:2"}}, second.root).changed,
          "actual gifts create recipient's hidden mailbox custody");
  const auto mailbox = f.players.rows[2].mailbox;
  net::Profiles profiles;
  net::Sessions sessions(profiles);
  step::NativeCompletion delivered;
  auto deliver = [&](Handle body, Step now) {
    delivered = cm::deliver_mailbox(f.players, sessions, f.f.objects_state, f.world, 2, body, now);
    const auto custody = std::move(delivered.custody);
    cm::apply_custody(f.f.objects_state, f.space_state, {0, 0, 3, 0}, f.plots, custody, delivered);
    return delivered.reply.at("delivered").get<unsigned>();
  };
  bool wrong_body = false;
  try {
    deliver(f.bodies[0], 21);
  } catch (const std::invalid_argument &) {
    wrong_body = true;
  }
  require(wrong_body && f.players.rows[2].mailbox == mailbox &&
              f.players.rows[2].inventory.empty() && delivered.records.empty(),
          "foreign body cannot collect recipient's mailbox");
  f.players.rows[2].mailbox.back() = f.bodies[1];
  bool custody_cycle = false;
  try {
    deliver(f.bodies[1], 21);
  } catch (const std::invalid_argument &) {
    custody_cycle = true;
  }
  require(custody_cycle && f.players.rows[2].inventory.empty() &&
              f.players.rows[2].mailbox.front() == first.root &&
              !f.f.objects.parent[first.root.slot()] && delivered.records.empty(),
          "real Space cycle preflight failure on a later entry cannot consume an earlier item");
  // A later bad full identity must not consume an earlier valid item.
  const auto bad = Handle::make(second.root.slot(), second.root.generation() + 1);
  f.players.rows[2].mailbox.back() = bad;
  bool failed = false;
  try {
    deliver(f.bodies[1], 21);
  } catch (const std::invalid_argument &) {
    failed = true;
  }
  require(failed && f.players.rows[2].mailbox.front() == first.root &&
              f.players.rows[2].mailbox.back() == bad && f.players.rows[2].inventory.empty() &&
              !f.f.objects.parent[first.root.slot()] && delivered.records.empty(),
          "failed whole-mailbox preflight preserves every earlier valuable and physical parent");
  f.players.rows[2].mailbox = mailbox;
  while (space::capacity(f.world).native_available)
    f.single(1, {15, 15, 0});
  const auto maker = f.f.objects.maker[first.root.slot()];
  const auto model = f.f.objects.model[first.root.slot()];
  const auto created = f.f.objects.created_step[first.root.slot()];
  const auto flags = f.f.objects.flags[first.root.slot()];
  amber::Origin origin;
  origin.cause = 81;
  require(deliver(f.bodies[1], 22) == 2 && f.players.rows[2].mailbox.empty() &&
              f.players.rows[2].inventory == mailbox &&
              f.f.objects.parent[first.root.slot()] == f.bodies[1] &&
              f.f.objects.parent[second.root.slot()] == f.bodies[1] && !f.visible(first.root) &&
              !f.visible(first.objects[1]) && f.f.objects.maker[first.root.slot()] == maker &&
              f.f.objects.model[first.root.slot()] == model &&
              f.f.objects.created_step[first.root.slot()] == created &&
              (f.f.objects.flags[first.root.slot()] & NoCopy) == (flags & NoCopy) &&
              space::capacity(f.world).native_available == 0,
          "login delivers full declared goods at full native capacity with immutable identities "
          "intact");
  record::Journal journal(100, {});
  for (auto &row : delivered.records)
    journal.append(std::move(row), origin);
  const auto records = journal.take();
  require(records.size() == 1 && records[0]["name"] == "mailbox_delivery" &&
              records[0]["cause"] == 81 && records[0]["changed"] == true && !records[0]["agent"],
          "login delivery emits one causal derived System record");
  require(deliver(f.bodies[1], 23) == 0 && delivered.records.empty(),
          "repeated authenticated hello cannot duplicate delivered goods or records");
}
void script_notices_need_a_live_recipient_grant_and_keep_unicode() {
  Fixture f;
  const auto caller = ScriptHolder{1, f.f.a};
  const auto args = nlohmann::json{{"player", "p:2"}, {"text", "hello"}};
  auto refused = [&](Caller actor) {
    const auto out = f.call(f.ids.notify, actor, args);
    require(!out.reply.at("ok").get<bool>() && out.result == record::Result::Denied &&
                out.reply.at("denied").at("fix") ==
                    "ask the player to give this script a grant first" &&
                out.notices.empty(),
            "notice escaped grant requirement");
  };
  refused(caller);
  refused(PlayerId{2});
  const auto holder = perm::Holder::script(1, f.f.a);
  const auto grant =
      perm::create_grant(f.grants, f.players, perm::Holder::player(2),
                         {2, holder, perm::Scope::owned(2), perm::bit(perm::Verb::Use), 0, 0, 100},
                         {perm::Scope::owned(2), 2, perm::all_rights}, {holder, 1, true}, 1);
  require(bool(grant), "notice grant fixture");
  std::string text;
  for (int i = 0; i < 281; ++i)
    text += "界";
  auto out = f.call(f.ids.notify, caller, {{"player", "p:2"}, {"text", text}});
  require(out.reply.at("ok") == true && out.records.size() == 1 &&
              out.records[0].name == "notify" && out.notices.size() == 1 &&
              out.notices[0].second.at("text").get<std::string>() == text.substr(0, 280 * 3) &&
              out.notices[0].second.at("on") == to_string(f.f.a),
          "notice text was not cut at code points");
  net::Profiles profiles;
  net::Sessions sessions(profiles);
  step::notify(f.players, sessions, 1, out.notices[0].first, out.notices[0].second);
  require(f.players.rows[2].notices.size() == 1 &&
              f.players.rows[2].notices[0] == out.notices[0].second,
          "absent recipient did not retain script notice");
  perm::revoke(f.grants, f.players, grant.grant);
  refused(caller);
}
void returns_use_inventory_with_static_occupants_and_ignore_full_mailboxes() {
  Fixture f;
  f.players.rows[2].mailbox.push_back(Handle::make(900, 1));
  f.delivery.present_bodies.erase(2);
  auto absent = f.item(2, {-4, 0, 0});
  for (auto h : absent.objects)
    set_plot(f.f.objects, h.slot(), 1);
  const auto returned = f.call(f.ids.return_goods, PlayerId{1}, {}, absent.root);
  require(returned.reply.at("location") == "mailbox" && f.players.rows[2].mailbox.size() == 2,
          "an absent owner with a full mailbox cannot receive a return");
  f.delivery.present_bodies[2] = f.bodies[1];
  auto present = f.item(2, {-8, 0, 0});
  for (auto h : present.objects)
    set_plot(f.f.objects, h.slot(), 1);
  testspace::Box mesh({1, 1, 1});
  const auto geometry =
      space::import_geometry(f.world, {mesh.mesh, {}, mesh.triangles, mesh.bounds});
  ObjectInit init;
  init.model = 1;
  init.owner = 2;
  init.position = {-6, 0, 0};
  init.flags = Static;
  const auto ornament = create(f.f.objects, init);
  require(
      space::add(
          f.world, identity(f.f.objects), transforms(f.f.objects),
          {ornament, geometry.geometry, space::Motion::Static, {.6f, 0, 0}, 1, false, false, {}})
              .status == space::Status::Ok,
      "triangle-only return occupant");
  f.world.native->attachments.emplace(
      ornament.bits, space::Attachment{ornament, present.root, {{2, 0, 0}, {}}, false});
  f.f.objects.parent[ornament.slot()] = present.root;
  const auto inventory = f.call(f.ids.return_goods, PlayerId{1}, {}, present.root);
  require(inventory.reply.at("location") == "inventory" &&
              f.players.rows[2].inventory == std::vector<Handle>{present.root} &&
              f.players.rows[2].mailbox.size() == 2 && !f.f.objects.parent[ornament.slot()] &&
              space::body_motion(f.world, identity(f.f.objects), ornament)->motion ==
                  space::Motion::Static,
          "a present owner's return fell back to the mailbox for a static occupant");
}
void land_refusals_have_fixes_and_denial_records() {
  Fixture f;
  auto denied = [&](const step::NativeCompletion &out, std::string_view fix) {
    require(out.reply.at("ok") == false && out.reply.at("denied").at("fix") == fix,
            "land refusal has no useful fix");
    record::Journal journal(100, {});
    step::ActionRun run;
    record::Event event;
    event.name = "land refusal";
    run.call.origin = journal.append(event);
    step::finish_native(run, out, identity(f.f.objects), attribution(f.f.objects), f.f.models,
                        f.f.scripts, journal);
    const auto records = journal.take();
    require(records.size() == 1 && records[0]["result"] == "denied",
            "land refusal was not recorded as denied");
  };
  denied(f.call(f.ids.abandon_plot, PlayerId{2}, {{"plot", 1}}), "ask p:1 for transfer on plot:1");
  f.plots.rows[2].kind = perm::PlotKind::Street;
  denied(f.call(f.ids.abandon_plot, PlayerId{1}, {{"plot", 2}}),
         "correct the supplied transfer for plot:2");
  denied(f.call(f.ids.split_plot, PlayerId{1}, {{"plot", 1}, {"x", -20}}),
         "cut at a whole meter strictly inside the lot");
  denied(f.call(f.ids.split_plot, PlayerId{1}, {{"plot", 1}, {"x", -19}}),
         "cut at least LOT_MIN_M m from each edge");
  f.plots.rows[2].kind = perm::PlotKind::Lot;
  perm::set_plot_owner(f.plots, 2, 1);
  f.plots.rows[2].bounds[0] = 1;
  denied(f.call(f.ids.join_plots, PlayerId{1}, {{"plots", {1, 2}}}),
         "choose two lots of one owner that share a whole edge and form a rectangle");
  auto own = f.item(1, {-4, 0, 0});
  denied(f.call(f.ids.return_goods, PlayerId{1}, {}, own.root), "pick it up instead");
  auto other = f.item(2, {-6, 0, 0});
  for (auto h : other.objects)
    set_plot(f.f.objects, h.slot(), 1);
  const auto grant =
      perm::create_grant(f.grants, f.players, perm::Holder::player(1),
                         {1, perm::Holder::player(2), perm::Scope::plot(1), 1, 0, 0, 100},
                         {perm::Scope::plot(1), 1, 31}, {perm::Holder::player(2), 2, false}, 1);
  require(bool(grant), "protected builder grant");
  denied(f.call(f.ids.return_goods, PlayerId{1}, {}, other.root),
         "ask p:2 to remove it, or revoke their build grant");
  perm::revoke(f.grants, f.players, grant.grant);
  denied(f.call(f.ids.return_goods, PlayerId{3}, {}, other.root), "ask p:1 for return on plot:1");
  const auto offer = f.call(f.ids.offer_plot, PlayerId{1}, {{"plot", 1}, {"price", 10}});
  const auto name = offer.reply.at("offer");
  denied(f.call(f.ids.set_offer_price, PlayerId{2}, {{"offer", name}, {"price", 20}}),
         "ask the offer's maker or seller");
  denied(f.call(f.ids.accept, PlayerId{2}, {{"offer", name}, {"max_price", 9}}),
         "inspect the offer and accept again with a max_price of at least its price");
  perm::set_plot_owner(f.plots, 1, 0);
  require(bool(perm::create_root_grant(f.grants, f.players,
                                       {0, perm::Holder::player(1), perm::Scope::all_land(),
                                        perm::Rights{8} << 8, 0, 0, 100},
                                       {perm::Holder::player(1), 1, false}, 1)),
          "pool transfer grant");
  denied(f.call(f.ids.abandon_plot, PlayerId{1}, {{"plot", 1}}),
         "this lot already belongs to the pool");
}
void plot_changes_dispatch_before_authority_is_removed() {
  Fixture f;
  f.plots.rows[1].bounds = {990, -20, 1010, 20};
  f.plots.rows[2].bounds = {990, -25, 1010, -20};
  f.plots.rows[2].kind = perm::PlotKind::Street;
  perm::set_plot_owner(f.plots, 2, 0);
  const auto admin = step::declare_admin(f.f.terms, f.declarations);
  const auto permissions = [&](nlohmann::json arguments) {
    return step::native_permissions(
        f.catalog_state, f.react_state, f.perm_state, f.objects_state, f.land_settings, false,
        std::get<react::PermissionAction>(f.declarations.rows.at(admin.plot_permissions).operation),
        {{PlayerId{1}, admin.plot_permissions, {}, {}}, std::move(arguments), {}, 20, 20, {}});
  };
  const auto observer = R"(return {kind='instance',on={['base.plot_changed']=function(self,_,event)
    self.state.count=(self.state.count or 0)+1
    self.state.op=event.op;self.state.plot=event.plot;self.state.other=event.other
    self.state.owner=event.owner;self.state.old_owner=event.old_owner
    self.state.area=event.area;self.state.other_area=event.other_area
    for _,prefix in ipairs({'','other_','old_'}) do
      local min,max=event[prefix..'min'],event[prefix..'max']
      self.state[prefix..'min_x']=if min then min.x else nil
      self.state[prefix..'min_y']=if min then min.y else nil
      self.state[prefix..'max_x']=if max then max.x else nil
      self.state[prefix..'max_y']=if max then max.y else nil
      for _,field in ipairs({'fronts','port','port_x','open','build_open'}) do
        self.state[prefix..field]=event[prefix..field]
      end
    end
  end}})";
  const auto land = f.f.add(observer), lease = f.f.add(observer), builder = f.f.add(observer);
  for (const auto &[script, object, rights] :
       {std::tuple{land, f.f.a, perm::Rights{8} << 8}, {builder, f.f.c, perm::Rights{1} << 8}}) {
    require(bool(perm::create_root_grant(f.grants, f.players,
                                         {0, perm::Holder::script(script, object),
                                          perm::Scope::all_land(), rights, 0, 0, 1000},
                                         {perm::Holder::script(script, object), 1, true}, 1)),
            "observer root grant");
  }
  for (const auto &[script, object] : {std::pair{land, f.f.a}, {lease, f.f.b}, {builder, f.f.c}}) {
    const std::array installed{script};
    react::set_instances(f.f.installed, object, installed, f.f.scripts);
    react::project_instance(f.f.triggers, f.f.installed, f.f.scripts, f.f.terms, object);
  }
  const auto grant_lease = [&](PlayerId owner) {
    const auto holder = perm::Holder::script(lease, f.f.b);
    const auto made = perm::create_grant(f.grants, f.players, perm::Holder::player(owner),
                                         {owner, holder, perm::Scope::plot(1), 16, 0, 0, 1000},
                                         {perm::Scope::plot(1), owner, 31}, {holder, 1, true}, 1);
    require(bool(made), "lease observer grant");
    return made.grant;
  };
  const auto initial_grant = grant_lease(1);
  unsigned count = 0;
  const auto deliver = [&](step::NativeCompletion out, std::string op, PlotId other,
                           std::string owner, std::string old_owner, double area, double other_area,
                           const nlohmann::json &facts) {
    require(out.changed, "plot mutation failed");
    step::WorkQueues queues;
    step::TimerWheel timers;
    record::Journal journal{100, {}};
    step::publish_effects(std::move(out.effects), {}, 20, queues, timers, journal, 100);
    unsigned received = 0;
    while (auto item = step::pop_foreground(queues)) {
      const auto &event = std::get<react::EventWork>(item->work);
      for (const auto &handler :
           react::dispatch(f.f.triggers, f.f.values(), event.receiver, event.event, 20, 20))
        if (handler.script == event.recipient_script) {
          react::PlannedInvocation plan{handler, {}};
          plan.input.receiver = event.receiver;
          plan.input.sender = event.sender;
          plan.input.trigger = event.event;
          plan.input.payload = event.payload;
          f.f.run(std::span(&plan, 1), 20);
          ++received;
        }
    }
    require(received == 2, "plot event did not reach exactly land and lease");
    ++count;
    for (const auto &[script, object] : {std::pair{land, f.f.a}, {lease, f.f.b}}) {
      const auto &state = f.f.states.at({object, script});
      const auto number = [&](const char *key) {
        return std::get<double>(state.values.at(std::string(key)));
      };
      require(number("count") == count && number("plot") == 1 && number("other") == other &&
                  number("area") == area && number("other_area") == other_area &&
                  std::get<std::string>(state.values.at(std::string("op"))) == op &&
                  std::get<std::string>(state.values.at(std::string("owner"))) == owner &&
                  std::get<std::string>(state.values.at(std::string("old_owner"))) == old_owner,
              "dispatched plot change has the wrong fields or was duplicated");
      for (const auto &[key, value] : facts.items()) {
        const auto &actual = state.values.at(key);
        require(value.is_boolean()  ? std::get<bool>(actual) == value.get<bool>()
                : value.is_string() ? std::get<std::string>(actual) == value.get<std::string>()
                                    : std::get<double>(actual) == value.get<double>(),
                "dispatched plot facts changed after the native action completed");
      }
      require(state.values.contains(std::string("other_min_x")) ==
                      (op == "split" || op == "join") &&
                  state.values.contains(std::string("old_min_x")) == (op == "join"),
              "plot change retained facts for an unrelated operation");
    }
    require(!f.f.states.contains({f.f.c, builder}), "build-only observer received a plot event");
  };
  const auto facts = [](double min_x, double max_x, bool open, bool build_open) {
    return nlohmann::json{{"min_x", min_x}, {"min_y", -20},       {"max_x", max_x},
                          {"max_y", 20},    {"fronts", "street"}, {"port", 1},
                          {"port_x", 1000}, {"open", open},       {"build_open", build_open}};
  };
  const auto with = [](nlohmann::json result, std::string_view prefix,
                       const nlohmann::json &other) {
    for (const auto &[key, value] : other.items())
      result[std::string(prefix) + key] = value;
    return result;
  };
  auto opened = permissions({{"plot", 1}, {"everyone", {"enter"}}});
  auto unchanged = permissions({{"plot", 1}, {"everyone", {"enter"}}});
  require(unchanged.reply.at("ok") && !unchanged.changed && unchanged.effects.ordered.empty(),
          "unchanged public entry emitted a plot event");
  for (auto arguments : {nlohmann::json{{"plot", 1}, {"everyone", {"build", "enter"}}},
                         nlohmann::json{{"plot", 1}, {"group_rights", {"enter"}}},
                         nlohmann::json{{"plot", 1}, {"banned", {"p:3"}}}}) {
    const auto unrelated = permissions(std::move(arguments));
    require(unrelated.changed && unrelated.effects.ordered.empty(),
            "administrative edits without a public-entry change emitted a plot event");
  }
  deliver(std::move(opened), "permissions", 0, "p:1", "p:1", 800, 0, facts(990, 1010, true, false));
  auto closed = permissions({{"plot", 1}, {"everyone", {"build"}}});
  unchanged = permissions({{"plot", 1}, {"everyone", {"build"}}});
  require(unchanged.reply.at("ok") && !unchanged.changed && unchanged.effects.ordered.empty(),
          "unchanged closed entry emitted a plot event");
  auto split = f.call(f.ids.split_plot, PlayerId{1}, {{"plot", 1}, {"x", 995}});
  auto joined = f.call(f.ids.join_plots, PlayerId{1}, {{"plots", {1, 3}}});
  const auto offer = f.call(f.ids.offer_plot, PlayerId{1}, {{"plot", 1}, {"price", 0}});
  auto sold =
      f.call(f.ids.accept, PlayerId{2}, {{"offer", offer.reply.at("offer")}, {"max_price", 0}});
  require(!f.grants.rows[initial_grant].live, "sale retained the previous lease grant");
  grant_lease(2);
  auto abandoned = f.call(f.ids.abandon_plot, PlayerId{2}, {{"plot", 1}});
  require(!f.plots.rows[3].id && !f.plots.rows[1].owner,
          "plot changes were not completed before their notifications were dispatched");
  const auto full = facts(990, 1010, false, true);
  const auto lower = facts(990, 995, false, true), upper = facts(995, 1010, false, true);
  deliver(std::move(closed), "permissions", 0, "p:1", "p:1", 800, 0, full);
  deliver(std::move(split), "split", 3, "p:1", "p:1", 200, 600, with(lower, "other_", upper));
  deliver(std::move(joined), "join", 3, "p:1", "p:1", 200, 600,
          with(with(full, "other_", upper), "old_", lower));
  deliver(std::move(sold), "owner", 0, "p:2", "p:1", 800, 0, full);
  deliver(std::move(abandoned), "owner", 0, "p:0", "p:2", 800, 0, full);
  const auto record = perm::plot_record(f.plots, f.grants, 1, f.land_settings.port_spacing_m, 20);
  require(record.at("build_open") == true && record.at("open") == false &&
              record.at("fronts") == "street" && record.at("port") == 1 &&
              record.at("port_x") == 1000,
          "plot inspection omitted public build or disagreed with the captured plot facts");
}

void land_editing_preserves_grants_and_refreshes_objects() {
  Fixture f;
  auto item = f.item(1, {-4, 0, 0});
  const auto old = f.objects_state.objects.plot[item.root.slot()];
  require(old == 1, "land fixture plot");
  const auto grant =
      perm::create_grant(f.grants, f.players, perm::Holder::player(1),
                         {1, perm::Holder::player(3), perm::Scope::plot(1), 1, 0, 0, 1000},
                         {perm::Scope::plot(1), 1, 31}, {perm::Holder::player(3), 3, false}, 1);
  require(bool(grant), "split grant fixture");
  f.call(f.ids.offer_plot, PlayerId{1}, {{"plot", 1}, {"price", 10}});
  const auto split = f.call(f.ids.split_plot, PlayerId{1}, {{"plot", 1}, {"x", -10}});
  require(
      split.changed && f.plots.rows.size() == 4 && f.plots.rows[1].bounds[2] == -10 &&
          f.plots.rows[3].bounds[0] == -10 && f.objects_state.objects.plot[item.root.slot()] == 3 &&
          step::plot_things(f.objects_state, 3) == std::vector{item.root} &&
          !f.offers.rows[1].live &&
          bool(perm::check_scope(perm::Holder::player(3), perm::Scope::plot(3), 1, f.grants, 20)),
      "split failed bounds, offers, grant copy or membership");
  const auto joined = f.call(f.ids.join_plots, PlayerId{1}, {{"plots", {3, 1}}});
  require(joined.changed && f.plots.rows[3].id == 0 && f.plots.rows[1].bounds[2] == 0 &&
              f.objects_state.objects.plot[item.root.slot()] == 1 && !joined.notices.empty() &&
              !f.objects_state.objects.on_plot.contains(3) &&
              !perm::check_scope(perm::Holder::player(3), perm::Scope::plot(3), 1, f.grants, 20),
          "join failed tombstone, grants, notice or membership");
  const auto abandoned = f.call(f.ids.abandon_plot, PlayerId{1}, {{"plot", 1}});
  require(abandoned.changed && f.plots.rows[1].owner == 0 && !f.visible(item.root) &&
              !f.grants.rows[grant.grant].live,
          "abandon did not return goods or clear authority");
}
} // namespace
int main() {
  try {
    script_notices_need_a_live_recipient_grant_and_keep_unicode();
    returns_use_inventory_with_static_occupants_and_ignore_full_mailboxes();
    land_refusals_have_fixes_and_denial_records();
    plot_changes_dispatch_before_authority_is_removed();
    land_editing_preserves_grants_and_refreshes_objects();
    offline_notices_survive_restore_and_do_not_fill_the_goods_mailbox();
    wallet_native_authority_is_explicit();
    actual_assembly_sale_and_inventory_reveal_preserve_identity();
    ordinary_offer_expiry_releases_actual_assembly_without_moving_it();
    trade_changes_cancel_and_complete_one_swap();
    gifts_bounce_and_keep_unrelated_seated_bodies_alive();
    cancelling_a_visible_held_trade_keeps_its_escrow_pose();
    placement_and_custody_use_current_priority_fallback();
    placement_enables_a_plot_selected_process_rule();
    logical_inventory_root_reveals_its_actual_declared_part();
    script_inventory_placement_uses_its_own_grants();
    retirement_keeps_the_surviving_trade_side_before_free();
    rebind_invalidates_only_affected_sales_with_actual_new_parts();
    rebind_trade_keeps_current_custody_at_full_capacity();
    login_delivers_complete_mailbox_without_native_capacity_or_identity_changes();
    std::cout << "native commerce tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
