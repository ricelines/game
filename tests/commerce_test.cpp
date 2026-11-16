#include "catalog/catalog.hpp"
#include "space_fixture.hpp"
#include <toml++/toml.hpp>

#include "runtime_fixture.hpp"
#include "step/action_private.hpp"

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
  amber::step::State step_state{8};

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
  react::MeterRows &meters = react_state.meters;
  catalog::Assets &assets = catalog_state.assets;
  catalog::ParameterBlocks &parameters = catalog_state.parameters;
  step::GeometryAssets &geometry = step_state.geometry;
  step::DeliveryTargets &delivery = step_state.delivery;
  react::NativeActions &declarations = react_state.native_declarations;
  cm::CommerceDeclarations ids;
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
    players.rows.resize(4);
    ledger.balance = {{perm::Account::player(1), 100}, {perm::Account::player(2), 100}};
    plots.rows.resize(3);
    plots.rows[1] = {1, 1, 0, 0, {-20, -20, 0, 20}, {}};
    plots.rows[2] = {2, 2, 0, 0, {0, -20, 20, 20}, {}};
    f.objects.owner[f.a.slot()] = 1;
    f.objects.owner[f.b.slot()] = 1;
    f.terms.rows.resize(32);
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
    auto out =
        step::native_exchange(objects_state, perm_state, world, delivery, mailbox_max, operation,
                              {{actor, action, target, {}}, std::move(args), {}, 20, 20, {}});
    const auto custody = std::move(out.custody);
    step::apply_custody(f.objects_state, space_state, {0, 0, 3, 0}, plots, custody, out);
    return out;
  }
  step::NativeCompletion place(Caller actor, nlohmann::json args, Handle target) {
    return step::place_existing(
        f.objects_state, space_state, {0, 0, 3, 0}, perm_state, geometry, f.catalog_state, {10, 11},
        {.6f, 0}, f.react_state,
        {{actor, ids.place_existing, target, {}}, std::move(args), {}, 20, 20, {}});
  }

  bool visible(Handle h) { return space::bounds(world, identity(f.objects), h).has_value(); }
  void store(Handle h, PlayerId owner) {
    auto closure = lifecycle::collect(parts, identity(f.objects), h);

    require(space::custody(f.objects_state, h, bodies[owner - 1], space_state,
                           std::span{closure}.subspan(1))
                    .status == space::Status::Ok,
            "fixture inventory custody");
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
  require(!f.call(f.ids.accept, PlayerId{2}, {{"offer", offer}}).changed && f.visible(item.root) &&
              f.f.objects.owner[item.root.slot()] == 1 &&
              f.ledger.balance[perm::Account::player(1)] == 100,
          "poor acceptance changed valuables or scene");
  f.ledger.balance[perm::Account::player(2)] = 100;
  f.delivery.present_bodies.erase(2);
  f.players.rows[2].mailbox.push_back(f.f.a);
  require(!f.call(f.ids.accept, PlayerId{2}, {{"offer", offer}}).changed && f.visible(child) &&
              f.ledger.balance[perm::Account::player(2)] == 100,
          "mailbox overflow consumed offered goods or payment");
  f.delivery.present_bodies[2] = f.bodies[1];
  f.players.rows[2].mailbox.clear();
  const auto accepted = f.call(f.ids.accept, PlayerId{2}, {{"offer", offer}});
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

void moving_between_equal_priority_plots_rebinds_future_dependencies() {
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
  f.f.plots.rows[1][priority] = Value(5.);
  f.f.plots.rows[2][priority] = Value(5.);
  f.f.objects.plot[f.f.b.slot()] = 3;
  f.f.instance_settings.columns.resize(f.f.terms.rows.size());
  f.f.instance_settings.columns[priority].set(f.f.b.slot(), Value(4.));
  react::GivesTo gives;
  react::DeferredEffects effects;
  react::set_contribution(f.f.shared, gives, f.f.terms.rows[supply], f.f.c, f.f.a, 1, effects);
  react::set_contribution(f.f.shared, gives, f.f.terms.rows[demand], item.root, f.f.a, 1, effects);
  react::set_contribution(f.f.shared, gives, f.f.terms.rows[demand], f.f.b, f.f.a, 1, effects);
  react::reconcile_allocations(*f.f.projection, f.f.react_state, effects, 20, 20);
  require(react::get_share(f.f.allocations, f.f.a, demand, item.root) == 1,
          "initial priority allocation");
  f.plots.rows[2].preset = 1;
  require(f.place(PlayerId{1}, {{"position", {4, 5, 0}}}, item.root).changed,
          "authorized equal-priority placement");
  f.f.plots.rows[2][priority] = Value(3.);
  effects = {};
  react::reconcile_priority(*f.f.projection, f.f.react_state,
                            {react::SettingKey::Scope::Plot, 2, priority}, effects, 21, 20);
  require(react::get_share(f.f.allocations, f.f.a, demand, item.root) == 0 &&
              react::get_share(f.f.allocations, f.f.a, demand, f.f.b) == 1,
          "same effective priority on placement left a stale old-plot dependency");
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
  f.plots.rows[2].owner = 1;
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
} // namespace
int main() {
  try {
    offline_notices_survive_restore_and_do_not_fill_the_goods_mailbox();
    wallet_native_authority_is_explicit();
    actual_assembly_sale_and_inventory_reveal_preserve_identity();
    ordinary_offer_expiry_releases_actual_assembly_without_moving_it();
    trade_changes_cancel_and_complete_one_swap();
    gifts_bounce_and_keep_unrelated_seated_bodies_alive();
    cancelling_a_visible_held_trade_keeps_its_escrow_pose();
    moving_between_equal_priority_plots_rebinds_future_dependencies();
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
