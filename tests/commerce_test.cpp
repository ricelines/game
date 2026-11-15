#include "space_fixture.hpp"

#include "bindings/commerce/commerce.hpp"
#include "runtime_fixture.hpp"
#include "scene_fixture.hpp"

namespace {
using namespace amber;
namespace cm = bindings::commerce;
using test_runtime::require;
struct Fixture {
  test_runtime::Fixture f;
  space::Space world;
  lifecycle::Parts parts;
  Relation declared;
  lifecycle::Instances instances{f.objects, parts, declared};
  std::vector<space::RelationChange> physical_changes;
  space::PhysicalRelations physical;
  perm::Players players;
  perm::Grants grants;
  perm::Ledger ledger;
  perm::Offers offers;
  perm::Trades trades;
  perm::OwnerLimits limits;
  perm::Plots plots;
  react::MeterRows meters;
  react::PriorityLinks priorities;
  bindings::AllocationReconciliation allocation{f.allocations, meters, priorities};
  catalog::Assets assets;
  catalog::ParameterBlocks parameters;
  bindings::GeometryAssets geometry;
  bindings::DeliveryTargets delivery;
  testscene::Projection projections;
  testscene::Lifecycle lifecycle;
  cm::Custody custody;
  command::NativeActions declarations;
  drive::NativeOperations operations;
  cm::Declarations ids;
  cm::PaymentBinding payment;
  cm::BalanceBinding balance;
  cm::InventoryBinding inventory;
  cm::GiveBinding giver;
  cm::OfferBinding seller;
  cm::OffersBinding offer_reads;
  cm::AcceptBinding buyer;
  cm::TradeOpenBinding opener;
  cm::TradeChangeBinding changer;
  cm::TradeConfirmBinding confirmer;
  cm::TradeCancelBinding canceller;
  cm::ExistingPlacementBinding placement;
  std::array<Handle, 2> bodies;
  explicit Fixture(std::uint32_t capacity = 64)
      : world([&] {
          auto s = testspace::settings();
          s.max_bodies = capacity;
          return s;
        }()),
        projections(f.objects, f.models, parameters, assets, *f.projection, f.runtime,
                    f.subscriptions, declared, {30, 31}),
        lifecycle(f, world, instances, parts, declared, physical, physical_changes, projections,
                  players, grants, limits, offers, trades),
        custody{world,
                identity(f.objects),
                transforms(f.objects),
                parents(f.objects),
                space_flags(f.objects),
                physical,
                parts,
                declared,
                {0, 0, 3, 0},
                projections.scene},
        payment{ledger, grants, players, identity(f.objects), authority(f.objects)},
        balance{ledger, players, identity(f.objects), authority(f.objects)}, inventory{players},
        giver{players,  grants, authority(f.objects), permission_flags(f.objects), limits, custody,
              delivery, 1},
        seller{offers, permission_flags(f.objects), players, grants, authority(f.objects), limits,
               custody},
        offer_reads{offers, players, identity(f.objects), authority(f.objects)},
        buyer{offers, ledger,  grants,   players, authority(f.objects), permission_flags(f.objects),
              limits, custody, delivery, 1},
        opener{trades, players, identity(f.objects), delivery},
        changer{trades, offers, permission_flags(f.objects), players, grants, authority(f.objects),
                limits, custody},
        confirmer{trades,
                  offers,
                  ledger,
                  grants,
                  players,
                  authority(f.objects),
                  permission_flags(f.objects),
                  limits,
                  custody,
                  delivery},
        canceller{trades, offers, permission_flags(f.objects), custody},
        placement{custody,
                  transforms(f.objects),
                  plot_membership(f.objects),
                  players,
                  grants,
                  authority(f.objects),
                  limits,
                  plots,
                  geometry,
                  f.models,
                  parameters,
                  assets,
                  *f.projection,
                  {10, 11},
                  {.6f, 0},
                  allocation} {
    players.rows.resize(4);
    ledger.balance = {0, 100, 100, 0};
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
    auto add = [&](TermId id, auto validate, auto run, auto *binding) {
      drive::register_native(operations, id, {validate, run, binding});
    };
    add(ids.pay, cm::validate_pay, cm::pay, &payment);
    add(ids.balance, cm::validate_balance, cm::balance, &balance);
    add(ids.inventory, cm::validate_inventory, cm::inventory, &inventory);
    add(ids.give, cm::validate_give, cm::give, &giver);
    add(ids.offer, cm::validate_offer, cm::offer, &seller);
    add(ids.offers, cm::validate_offers, cm::offers, &offer_reads);
    add(ids.accept, cm::validate_accept, cm::accept, &buyer);
    add(ids.cancel_offer, cm::validate_cancel_offer, cm::cancel_offer, &seller);
    add(ids.trade_open, cm::validate_trade_open, cm::trade_open, &opener);
    add(ids.trade_change, cm::validate_trade_change, cm::trade_change, &changer);
    add(ids.trade_confirm, cm::validate_trade_confirm, cm::trade_confirm, &confirmer);
    add(ids.trade_cancel, cm::validate_trade_cancel, cm::trade_cancel, &canceller);
    add(ids.place_existing, cm::validate_place_existing, cm::place_existing, &placement);
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
      const auto definition =
          bindings::describe_physics(geometry, asset, *f.projection, {model, 0, 8, init.plot, h},
                                     Static, {10, 11}, {.6f, 0}, 0, 20);
      bindings::install_physics(world, identity(f.objects), transforms(f.objects),
                                space_flags(f.objects), h, definition);
      permission_flags(f.objects).set(h.slot(), NoCopy);
      if (h != created.root) {
        std::vector<space::RelationChange> changes;
        const auto relative =
            f.objects.position[h.slot()] - f.objects.position[created.root.slot()];
        require(space::attach(world, identity(f.objects), transforms(f.objects), parents(f.objects),
                              space_flags(f.objects), physical.contains, physical.supports,
                              physical.attached_to, {h, created.root, {relative, {}}, false},
                              changes)
                        .status == space::Status::Ok,
                "declared fixture part attaches");
      }
    }
    return created;
  }
  drive::NativeCompletion call(TermId action, Caller actor, nlohmann::json args = {},
                               Handle target = {}) {
    if (args.is_null())
      args = nlohmann::json::object();
    return drive::invoke_native(operations,
                                {{actor, action, target, {}}, std::move(args), {}, 20, 20, {}});
  }
  bool visible(Handle h) { return space::bounds(world, identity(f.objects), h).has_value(); }
  void store(Handle h, PlayerId owner) {
    auto closure = lifecycle::collect(parts, identity(f.objects), h);
    std::vector<space::RelationChange> changes;
    require(space::custody(world, identity(f.objects), h, bodies[owner - 1], parents(f.objects),
                           space_flags(f.objects), physical, changes, std::span{closure}.subspan(1))
                    .status == space::Status::Ok,
            "fixture inventory custody");
    players.rows[owner].inventory.push_back(h);
  }
};
void wallet_native_authority_is_explicit() {
  Fixture f;
  const Caller script = ScriptHolder{77, f.f.a};
  require(!f.call(f.ids.pay, script, {{"to", "p:2"}, {"amount", 10}}).reply.at("ok"),
          "native script inherited owner Spend");
  const auto grant = perm::create_grant(
      f.grants, f.players,
      {1, perm::Holder::script(77, f.f.a), perm::Scope::account(1), 1, 20, 100, 500},
      {perm::Scope::account(1), 1, 1, 0, false}, {perm::Holder::script(77, f.f.a), 1, true}, 1);
  require(bool(grant), "native wallet fixture grant");
  require(f.call(f.ids.pay, script, {{"to", "p:2"}, {"amount", 15}}).changed &&
              f.ledger.balance[1] == 85 && f.ledger.balance[2] == 115,
          "native payment did not use player wallets");
  require(!f.call(f.ids.pay, script, {{"to", "p:2"}, {"amount", 6}}).changed &&
              f.grants.rows[grant.grant].spent == 15 && f.ledger.balance[1] == 85,
          "failed native payment consumed allowance or money");
  require(!f.call(f.ids.pay, ScriptHolder{77, f.f.b}, {{"to", "p:2"}, {"amount", 1}}).changed,
          "Spend leaked across installations");
  require(!f.call(f.ids.pay, PlayerId{1}, {{"to", "g:1"}, {"amount", 1}}).reply.at("ok"),
          "non-player wallet accepted");
  require(f.call(f.ids.balance, script).reply.at("balance") == 85,
          "script default wallet read selects current owner only");
}
void actual_assembly_sale_and_inventory_reveal_preserve_identity() {
  Fixture f(4);
  const auto item = f.item(1, {-4, 0, 0});
  const auto child = item.objects[1];
  const auto original_maker = f.f.objects.maker[item.root.slot()];
  const auto granted = perm::create_grant(
      f.grants, f.players,
      {1, perm::Holder::script(77, child), perm::Scope::account(1), 1, 20, 100, 500},
      {perm::Scope::account(1), 1, 1, 0, false}, {perm::Holder::script(77, child), 1, true}, 1);
  require(bool(granted), "installed part grant fixture");
  require(!f.call(f.ids.offer, PlayerId{1}, {{"price", 40}}, child).reply.at("ok"),
          "internal part sold separately");
  const auto offered = f.call(f.ids.offer, PlayerId{1}, {{"price", 40}}, item.root);
  require(offered.changed && f.visible(item.root) && f.visible(child) &&
              (f.f.objects.flags[child.slot()] & Escrow),
          "escrow hid actual goods or omitted part");
  const auto offer = offered.reply.at("offer");
  f.ledger.balance[2] = 39;
  require(!f.call(f.ids.accept, PlayerId{2}, {{"offer", offer}}).changed && f.visible(item.root) &&
              f.f.objects.owner[item.root.slot()] == 1 && f.ledger.balance[1] == 100,
          "poor acceptance changed valuables or scene");
  f.ledger.balance[2] = 100;
  f.delivery.present_bodies.erase(2);
  f.players.rows[2].mailbox.push_back(f.f.a);
  require(!f.call(f.ids.accept, PlayerId{2}, {{"offer", offer}}).changed && f.visible(child) &&
              f.ledger.balance[2] == 100,
          "mailbox overflow consumed offered goods or payment");
  f.delivery.present_bodies[2] = f.bodies[1];
  f.players.rows[2].mailbox.clear();
  const auto accepted = f.call(f.ids.accept, PlayerId{2}, {{"offer", offer}});
  require(accepted.changed && f.ledger.balance[1] == 140 && f.ledger.balance[2] == 60 &&
              f.f.objects.owner[child.slot()] == 2 && !f.grants.rows[granted.grant].live &&
              !f.visible(item.root) && !f.visible(child) &&
              f.players.rows[2].inventory == std::vector<Handle>{item.root},
          "native sale failed complete physical/financial/grant transfer");
  const auto blocker_a = f.single(1, {-10, -10, 0}), blocker_b = f.single(1, {-8, -10, 0});
  const auto before = f.f.objects.position[item.root.slot()];
  auto placed = f.call(f.ids.place_existing, PlayerId{2}, {{"position", {4, 5, 0}}}, item.root);
  require(!placed.changed && f.f.objects.position[item.root.slot()] == before &&
              !f.visible(item.root) && f.players.rows[2].inventory.size() == 1,
          "native capacity failure moved or lost inventory");
  std::vector<space::RelationChange> changes;
  for (auto h : {blocker_a, blocker_b})
    space::remove(f.world, identity(f.f.objects), h, parents(f.f.objects), space_flags(f.f.objects),
                  f.physical, changes);
  require(
      !f.call(f.ids.place_existing, PlayerId{2}, {{"position", {-4, 5, 0}}}, item.root).changed &&
          !f.visible(item.root),
      "foreign land Build denial changed custody");
  placed = f.call(f.ids.place_existing, PlayerId{2},
                  {{"position", {4, 5, 0}}, {"rotation", {0, 0, std::sqrt(.5), std::sqrt(.5)}}},
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
  const auto ray = space::raycast(f.world, identity(f.f.objects), {4, 4, .5f}, {4, 5.5f, .5f}, {},
                                  space::Deadline::max());
  require(ray.hit && ray.hit->object == item.root,
          "revealed inventory has no actual Jolt query shape");
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
  cm::OfferExpiryBinding expiry{f.offers, identity(f.f.objects), permission_flags(f.f.objects)};
  require(cm::advance_offer_expiry(expiry, 29, 256).empty(),
          "native sale expired before its due step");
  for (auto h : item.objects)
    require((f.f.objects.flags[h.slot()] & Escrow) && f.visible(h),
            "ordinary sale keeps its actual root and part visible in escrow");
  const auto scene_changes = f.projections.metadata.objects.size();
  const auto expired = cm::advance_offer_expiry(expiry, 30, 256);
  require(expired.size() == 1 && expired[0].offer == id && expired[0].seller == 1 &&
              expired[0].goods == item.root && expired[0].cleared == item.objects &&
              !f.offers.rows[id].live && f.offers.expiring.empty() &&
              f.projections.metadata.objects.size() == scene_changes &&
              cm::advance_offer_expiry(expiry, 31, 256).empty(),
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
void trade_changes_cancel_restore_and_complete_one_swap() {
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
              !f.visible(b.root) && f.f.objects.parent[b.root.slot()] == f.bodies[1] &&
              !(f.f.objects.flags[a.root.slot()] & Escrow),
          "cancel lost prior native custody");
  const auto next = f.call(f.ids.trade_open, PlayerId{1}, {{"other", "p:2"}}).reply.at("trade");
  f.call(f.ids.trade_change, PlayerId{1},
         {{"trade", next}, {"money", 10}, {"goods", to_string(a.root)}});
  f.call(f.ids.trade_change, PlayerId{2},
         {{"trade", next}, {"money", 6}, {"goods", to_string(b.root)}});
  f.call(f.ids.trade_confirm, PlayerId{1}, {{"trade", next}});
  f.ledger.balance[2] = 5;
  require(!f.call(f.ids.trade_confirm, PlayerId{2}, {{"trade", next}}).changed &&
              f.f.objects.owner[a.root.slot()] == 1 && f.visible(a.root),
          "failed second confirmation changed valuable side");
  f.ledger.balance[2] = 100;
  const auto completed = f.call(f.ids.trade_confirm, PlayerId{2}, {{"trade", next}});
  require(completed.changed && completed.reply.at("completed") && f.ledger.balance[1] == 96 &&
              f.ledger.balance[2] == 104 && f.f.objects.owner[a.root.slot()] == 2 &&
              f.f.objects.owner[b.root.slot()] == 1 && !f.visible(a.root) && !f.visible(b.root),
          "native trade did not atomically exchange player money and real custody");
}
void gifts_bounce_and_keep_unrelated_seated_bodies_alive() {
  Fixture f;
  const auto item = f.item(1, {-4, 0, 0});
  std::vector<space::RelationChange> changes;
  require(space::attach(f.world, identity(f.f.objects), transforms(f.f.objects),
                        parents(f.f.objects), space_flags(f.f.objects), f.physical.contains,
                        f.physical.supports, f.physical.attached_to,
                        {f.bodies[1], item.root, {{0, 0, 1}, {}}, false}, changes)
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

void cancelling_a_visible_held_trade_restores_actual_attachment() {
  Fixture f;
  const auto item = f.item(1, {-4, 0, 0}, false);
  std::vector<space::RelationChange> changes;
  require(space::attach(f.world, identity(f.f.objects), transforms(f.f.objects),
                        parents(f.f.objects), space_flags(f.f.objects), f.physical.contains,
                        f.physical.supports, f.physical.attached_to,
                        {item.root, f.bodies[0], {{.5f, 0, 1}, {}}, true}, changes)
                  .status == space::Status::Ok,
          "visible held fixture is a real physical grasp");
  const auto position = f.f.objects.position[item.root.slot()];
  const auto id = f.call(f.ids.trade_open, PlayerId{1}, {{"other", "p:2"}}).reply.at("trade");
  require(f.call(f.ids.trade_change, PlayerId{1},
                 {{"trade", id}, {"money", 0}, {"goods", to_string(item.root)}})
                  .changed &&
              f.visible(item.root) && !f.f.objects.parent[item.root.slot()] &&
              !(f.f.objects.flags[item.root.slot()] & Held),
          "visible grasped trade side did not enter placed escrow");
  require(f.call(f.ids.trade_cancel, PlayerId{1}, {{"trade", id}}).changed &&
              f.visible(item.root) && f.f.objects.parent[item.root.slot()] == f.bodies[0] &&
              (f.f.objects.flags[item.root.slot()] & Held) &&
              f.f.objects.position[item.root.slot()] == position,
          "trade cancellation lost actual held attachment or teleported the item");
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
  react::rebuild_priorities(f.priorities, *f.f.projection);
  bindings::reconcile_allocations(*f.f.projection, f.allocation, effects, 20, 20);
  require(react::get_share(f.f.allocations, f.f.a, demand, item.root) == 1,
          "initial priority allocation");
  f.plots.rows[2].preset = 1;
  require(f.call(f.ids.place_existing, PlayerId{1}, {{"position", {4, 5, 0}}}, item.root).changed,
          "authorized equal-priority placement");
  f.f.plots.rows[2][priority] = Value(3.);
  effects = {};
  bindings::reconcile_priority(*f.f.projection, f.allocation,
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
  const auto result =
      f.call(f.ids.place_existing, PlayerId{1}, {{"position", {-4, 5, 0}}}, item.root);
  require(result.changed && space::is_logical_root(f.world, identity(f.f.objects), item.root) &&
              f.visible(item.objects[1]) && !f.players.rows[1].inventory.size(),
          "existing logical root lost its visible physical part on reveal");
  const auto ray = space::raycast(f.world, identity(f.f.objects), {-3, 4, .2f}, {-3, 6, .2f}, {},
                                  space::Deadline::max());
  require(ray.hit && ray.hit->object == item.objects[1],
          "logical assembly child is absent from native queries");
}

void script_inventory_placement_uses_its_own_grants() {
  Fixture f;
  const auto item = f.item(1, {-4, 0, 0}, false);
  f.store(item.root, 1);
  const ScriptHolder script{99, f.f.a};
  const auto args = nlohmann::json{{"position", {-4, 5, 0}}};
  require(!f.call(f.ids.place_existing, script, args, item.root).changed && !f.visible(item.root),
          "script borrowed the inventory owner's Move/Build authority");
  const auto holder = perm::Holder::script(script.script, script.object);
  const auto move = perm::create_grant(
      f.grants, f.players,
      {1, holder, perm::Scope::object(item.root), perm::bit(perm::Verb::Move), 0, 0, 500},
      {perm::Scope::object(item.root), 1, perm::all_rights, 1, false}, {holder, 1, true}, 1);
  require(bool(move) && !f.call(f.ids.place_existing, script, args, item.root).changed &&
              !f.visible(item.root),
          "script Move grant borrowed owner's land Build");
  const auto build =
      perm::create_grant(f.grants, f.players, {1, holder, perm::Scope::plot(1), 1, 0, 0, 500},
                         {perm::Scope::plot(1), 1, 7, 1, false}, {holder, 1, true}, 1);
  require(bool(build) && f.call(f.ids.place_existing, script, args, item.root).changed &&
              f.visible(item.root) && f.f.objects.owner[item.root.slot()] == 1 &&
              f.players.rows[1].inventory.empty(),
          "authorized script did not place existing owner's item under its actual grants");
}

void retirement_restores_the_surviving_trade_side_before_free() {
  Fixture f;
  const auto a = f.item(1, {-4, 0, 0}), b = f.item(2, {4, 0, 0});
  f.store(b.root, 2);
  const auto id = f.call(f.ids.trade_open, PlayerId{1}, {{"other", "p:2"}}).reply.at("trade");
  f.call(f.ids.trade_change, PlayerId{1},
         {{"trade", id}, {"money", 0}, {"goods", to_string(a.root)}});
  f.call(f.ids.trade_change, PlayerId{2},
         {{"trade", id}, {"money", 0}, {"goods", to_string(b.root)}});
  const std::array<Handle, 1> doomed{a.root};
  const auto closure = f.instances.closure(doomed);
  std::string error;
  require(bindings::can_retire_objects(f.lifecycle.lifecycle, closure, error), error.c_str());
  react::DeferredEffects pending;
  bindings::retire_objects(f.lifecycle.lifecycle, closure, {}, pending);
  require(!identity(f.f.objects).valid(a.root) && identity(f.f.objects).valid(b.root) &&
              !f.trades.rows[1].live && !(f.f.objects.flags[b.root.slot()] & Escrow) &&
              !f.visible(b.root) && f.f.objects.parent[b.root.slot()] == f.bodies[1] &&
              f.players.rows[2].inventory == std::vector<Handle>{b.root} && pending.changed,
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
  std::vector<space::RelationChange> changes;
  require(space::attach(f.world, identity(f.f.objects), transforms(f.f.objects),
                        parents(f.f.objects), space_flags(f.f.objects), f.physical.contains,
                        f.physical.supports, f.physical.attached_to,
                        {added, item.root, {{0, 1, 0}, {}}, false}, changes)
                  .status == space::Status::Ok,
          "new definition part has real native geometry and attachment");
  permission_flags(f.f.objects).set(added.slot(), Escrow);
  const auto old_pose = f.f.objects.position[item.root.slot()];
  const auto balances = f.ledger.balance;
  const std::array<Handle, 1> rebound{item.root};
  cm::RebindOffersBinding binding{f.offers, f.trades, permission_flags(f.f.objects), f.custody};
  bindings::InvocationInput invocation;
  invocation.step = 30;
  react::DeferredEffects effects;
  cm::rebind_offers(&binding, rebound, invocation, effects);
  require(!f.offers.rows[1].live && f.offers.rows[2].live &&
              (f.f.objects.flags[untouched.root.slot()] & Escrow) &&
              !(f.f.objects.flags[item.root.slot()] & Escrow) &&
              !(f.f.objects.flags[item.objects[1].slot()] & Escrow) &&
              !(f.f.objects.flags[added.slot()] & Escrow) && f.visible(item.root) &&
              f.visible(added) && f.f.objects.position[item.root.slot()] == old_pose &&
              f.ledger.balance == balances && effects.changed,
          "definition invalidation used stale parts, moved goods, or cancelled unrelated sale");
  step::WorkQueues queues;
  step::TimerWheel timers;
  record::Journal journal(100, {});
  record::Origin origin;
  origin.cause = 91;
  react::publish_effects(std::move(effects), origin, 30, queues, timers, journal, 1000);
  const auto records = journal.take();
  require(records.size() == 1 && records[0]["name"] == "goods_offers_invalidated" &&
              records[0]["cause"] == 91 && records[0]["reason"] == "definition_changed" &&
              records[0]["offers"] == nlohmann::json::array({"offer:1"}) &&
              records[0]["things"].size() == 3,
          "changed sale lacks causal reason or actual new part closure");
  effects = {};
  cm::rebind_offers(&binding, rebound, invocation, effects);
  require(!effects.changed && effects.ordered.empty(), "rebind cancellation was replayed");
}
void rebind_trade_restores_visible_grasp_and_hidden_inventory_at_full_capacity() {
  Fixture f;
  const auto held = f.item(1, {-4, 0, 0});
  const auto stored = f.item(2, {4, 0, 0});
  f.store(stored.root, 2);
  std::vector<space::RelationChange> changes;
  require(space::attach(f.world, identity(f.f.objects), transforms(f.f.objects),
                        parents(f.f.objects), space_flags(f.f.objects), f.physical.contains,
                        f.physical.supports, f.physical.attached_to,
                        {held.root, f.bodies[0], {{.5f, 0, 1}, {}}, true}, changes)
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
  cm::RebindOffersBinding binding{f.offers, f.trades, permission_flags(f.f.objects), f.custody};
  react::DeferredEffects effects;
  std::vector<space::VisibleCustody> forecast;
  std::string error;
  const std::array<Handle, 2> affected{held.root, stored.root};
  require(cm::preflight_rebound_offers(&binding, affected, {}, forecast, error) &&
              forecast.size() == 1 && forecast[0].goods == held.root &&
              forecast[0].parent == f.bodies[0] && forecast[0].grasp && f.trades.rows[1].live &&
              f.trades.rows[1].sides[0].confirmed && !f.f.objects.parent[held.root.slot()] &&
              !f.f.objects.parent[stored.root.slot()] && effects.ordered.empty(),
          "custody forecast changed the quote or omitted its future physical grasp");
  cm::rebind_offers(&binding, rebound, {}, effects);
  require(
      !f.trades.rows[1].live && !f.trades.rows[1].sides[0].confirmed &&
          !f.trades.rows[1].sides[1].confirmed && !f.offers.rows[1].live &&
          !f.offers.rows[2].live && f.visible(held.root) && !f.visible(stored.root) &&
          f.f.objects.parent[held.root.slot()] == f.bodies[0] &&
          f.f.objects.parent[stored.root.slot()] == f.bodies[1] &&
          (f.f.objects.flags[held.root.slot()] & Held) &&
          !(f.f.objects.flags[held.root.slot()] & Escrow) &&
          !(f.f.objects.flags[stored.root.slot()] & Escrow) &&
          f.f.objects.position[held.root.slot()] == position &&
          f.players.rows[2].inventory == stored_inventory && f.ledger.balance == balance &&
          space::capacity(f.world).native_available == 0,
      "rebinding trade goods lost prior custody, confirmations, money, or needed native capacity");
}
void login_delivers_complete_mailbox_without_native_capacity_or_identity_changes() {
  Fixture f;
  const auto first = f.item(1, {-4, 0, 0}), second = f.item(1, {-8, 0, 0}, false);
  f.delivery.present_bodies.erase(2);
  f.giver.mailbox_max = 10;
  require(f.call(f.ids.give, PlayerId{1}, {{"recipient", "p:2"}}, first.root).changed &&
              f.call(f.ids.give, PlayerId{1}, {{"recipient", "p:2"}}, second.root).changed,
          "actual gifts create recipient's hidden mailbox custody");
  const auto mailbox = f.players.rows[2].mailbox;
  cm::MailboxDelivery delivery{f.players, authority(f.f.objects), f.custody};
  react::DeferredEffects effects;
  bool wrong_body = false;
  try {
    cm::deliver_mailbox(delivery, 2, f.bodies[0], 21, {}, effects);
  } catch (const std::invalid_argument &) {
    wrong_body = true;
  }
  require(wrong_body && f.players.rows[2].mailbox == mailbox &&
              f.players.rows[2].inventory.empty() && effects.ordered.empty(),
          "foreign body cannot collect recipient's mailbox");
  f.players.rows[2].mailbox.back() = f.bodies[1];
  bool custody_cycle = false;
  try {
    cm::deliver_mailbox(delivery, 2, f.bodies[1], 21, {}, effects);
  } catch (const std::invalid_argument &) {
    custody_cycle = true;
  }
  require(custody_cycle && f.players.rows[2].inventory.empty() &&
              f.players.rows[2].mailbox.front() == first.root &&
              !f.f.objects.parent[first.root.slot()] && effects.ordered.empty(),
          "real Space cycle preflight failure on a later entry cannot consume an earlier item");
  // A later bad full identity must not consume an earlier valid item.
  const auto bad = Handle::make(second.root.slot(), second.root.generation() + 1);
  f.players.rows[2].mailbox.back() = bad;
  bool failed = false;
  try {
    cm::deliver_mailbox(delivery, 2, f.bodies[1], 21, {}, effects);
  } catch (const std::invalid_argument &) {
    failed = true;
  }
  require(failed && f.players.rows[2].mailbox.front() == first.root &&
              f.players.rows[2].mailbox.back() == bad && f.players.rows[2].inventory.empty() &&
              !f.f.objects.parent[first.root.slot()] && effects.ordered.empty(),
          "failed whole-mailbox preflight preserves every earlier valuable and physical parent");
  f.players.rows[2].mailbox = mailbox;
  while (space::capacity(f.world).native_available)
    f.single(1, {15, 15, 0});
  const auto maker = f.f.objects.maker[first.root.slot()];
  const auto model = f.f.objects.model[first.root.slot()];
  const auto created = f.f.objects.created_step[first.root.slot()];
  const auto flags = f.f.objects.flags[first.root.slot()];
  record::Origin origin;
  origin.cause = 81;
  require(cm::deliver_mailbox(delivery, 2, f.bodies[1], 22, origin, effects) == 2 &&
              f.players.rows[2].mailbox.empty() && f.players.rows[2].inventory == mailbox &&
              f.f.objects.parent[first.root.slot()] == f.bodies[1] &&
              f.f.objects.parent[second.root.slot()] == f.bodies[1] && !f.visible(first.root) &&
              !f.visible(first.objects[1]) && f.f.objects.maker[first.root.slot()] == maker &&
              f.f.objects.model[first.root.slot()] == model &&
              f.f.objects.created_step[first.root.slot()] == created &&
              (f.f.objects.flags[first.root.slot()] & NoCopy) == (flags & NoCopy) &&
              space::capacity(f.world).native_available == 0,
          "login delivers full declared goods at full native capacity with immutable identities "
          "intact");
  step::WorkQueues queues;
  step::TimerWheel timers;
  record::Journal journal(100, {});
  react::publish_effects(std::move(effects), origin, 22, queues, timers, journal, 1000);
  const auto records = journal.take();
  require(records.size() == 1 && records[0]["name"] == "mailbox_delivery" &&
              records[0]["cause"] == 81 && records[0]["changed"] == true,
          "login delivery emits one causal completed System record");
  effects = {};
  require(cm::deliver_mailbox(delivery, 2, f.bodies[1], 23, {}, effects) == 0 &&
              effects.ordered.empty(),
          "repeated authenticated hello cannot duplicate delivered goods or records");
}
} // namespace
int main() {
  try {
    wallet_native_authority_is_explicit();
    actual_assembly_sale_and_inventory_reveal_preserve_identity();
    ordinary_offer_expiry_releases_actual_assembly_without_moving_it();
    trade_changes_cancel_restore_and_complete_one_swap();
    gifts_bounce_and_keep_unrelated_seated_bodies_alive();
    cancelling_a_visible_held_trade_restores_actual_attachment();
    moving_between_equal_priority_plots_rebinds_future_dependencies();
    logical_inventory_root_reveals_its_actual_declared_part();
    script_inventory_placement_uses_its_own_grants();
    retirement_restores_the_surviving_trade_side_before_free();
    rebind_invalidates_only_affected_sales_with_actual_new_parts();
    rebind_trade_restores_visible_grasp_and_hidden_inventory_at_full_capacity();
    login_delivers_complete_mailbox_without_native_capacity_or_identity_changes();
    std::cout << "native commerce tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
