#include "checkpoint_fixture.hpp"
#include <iostream>

using namespace amber;
using namespace amber::checkpoint_test;
using test_runtime::require;
namespace {
void round_trip_and_real_vm_continuation() {
  Fixture original;
  original.populate();
  require(step::timers(original.timers).size() == 1 && step::timers(original.timers)[0].due == 41,
          "snapshot fixture includes an actual pending process-threshold timer");
  original.plots.rows[1].kind = perm::PlotKind::Street;
  const auto root_grant = perm::create_root_grant(
      original.grants, original.players,
      {0, perm::Holder::player(1), perm::Scope::account(perm::Account::player(0)), 1, 10, 100, 200},
      {}, 0);
  require(root_grant && perm::pay(original.ledger, original.grants, perm::Holder::player(1),
                                  perm::AccountTarget::player(0), perm::Account::player(3), 3, 5),
          "snapshot fixture spends part of an actual root allowance");
  original.players.rows[1].notices = {{{"kind", "changed"}, {"cleared", {"g:3"}}}};
  original.players.rows[1].notices_dropped = 4;
  snap::Buffer bytes;
  original.write(bytes);
  const std::array<std::string_view, 1> versions{"checkpoint-fixture"};
  auto view = snap::parse(bytes, versions);
  Fixture loaded;
  loaded.read(view);
  require(loaded.plots.rows[1].kind == perm::PlotKind::Street &&
              loaded.players.rows[0].grants_given == std::vector<GrantId>{root_grant.grant} &&
              perm::allowance(loaded.grants, root_grant.grant, 12) == 7 &&
              !loaded.ledger.balance.contains(perm::Account::player(0)) &&
              !perm::pay(loaded.ledger, loaded.grants, perm::Holder::player(1),
                         perm::AccountTarget::player(0), perm::Account::player(3), 8, 12),
          "plot kinds, root grant ownership and consumed root budget survive a snapshot");
  require(loaded.players.rows[1].notices == original.players.rows[1].notices &&
              loaded.players.rows[1].notices_dropped == 4,
          "the complete checkpoint preserves pending player notices and overflow count");
  require(view.sections.size() == 13 && loaded.next_record == 31 &&
              loaded.inputs.rows.size() == 1 && loaded.motion.size() == 2 &&
              loaded.parts.get(original.f.b.slot()),
          "all thirteen owner sections restore their actual nonempty tables");
  require(loaded.f.processes[1].get(original.f.a.slot())->rate == 1 &&
              loaded.f.processes[1].get(original.f.a.slot())->step0 == 0 &&
              react::read_state(loaded.f.properties, loaded.f.processes, original.f.a, 1, 20, 20)
                      .value.numeric() == 1 &&
              step::timers(loaded.timers).size() == step::timers(original.timers).size() &&
              loaded.f.subscriptions.rows.at({original.f.a, 1}).front().last ==
                  original.f.subscriptions.rows.at({original.f.a, 1}).front().last &&
              loaded.profiles.by_key == original.profiles.by_key &&
              loaded.grants.by_key == original.grants.by_key,
          "restore preserves the live closed-form process, pending crossing and last watch answer");
  require(loaded.parameters.rows.size() == original.parameters.rows.size() &&
              loaded.parameters.rows[1].model == original.parameters.rows[1].model &&
              loaded.parameters.rows[1].asset == original.parameters.rows[1].asset &&
              loaded.parameters.rows[1].parts == original.parameters.rows[1].parts &&
              loaded.offers.rows[1].parts == original.offers.rows[1].parts &&
              loaded.trades.rows[1].live && loaded.limits.get(original.f.a.slot()) &&
              loaded.authored_text.rows.size() == original.authored_text.rows.size() &&
              loaded.authored_text.rows[1].target == original.f.a &&
              loaded.authored_text.rows[2].target == original.f.c,
          "parameter topology, complete offers/trades, owner limits and attributed text survive");
  for (const auto &[id, code] : original.f.code) {
    auto outcome = react::load_script(loaded.f.vm, code, loaded.f.scripts.rows[id].declaration);
    require(outcome.status == react::RunOutcome::Status::Ok,
            "fresh VM loads the same immutable compiled fixture artifacts");
  }
  const auto before = loaded.f.subscriptions.rows.at({original.f.a, 1}).front().last;
  auto changes = react::sync_watches(
      loaded.f.react_state, loaded.f.values(), {original.f.a, original.script},
      loaded.f.scripts.rows[original.script].declaration, 12, 20, {}, react::SyncMode::Restore);
  require(changes.dirty.empty() && changes.timers.empty() &&
              loaded.f.subscriptions.rows.at({original.f.a, 1}).front().last == before,
          "runtime watch rehydration preserves saved last answers and does not duplicate timers");
  const react::InvocationPlan cue{
      {original.script, "novel.cue", 1, 1, true}, original.f.a, original.f.b, 6, {}, {}};
  original.f.run(std::span<const react::InvocationPlan>(&cue, 1), 13);
  loaded.f.run(std::span<const react::InvocationPlan>(&cue, 1), 13);
  require(original.f.states.at({original.f.a, original.script}) ==
                  loaded.f.states.at({original.f.a, original.script}) &&
              original.f.random == loaded.f.random,
          "actual Luau continuation produces identical persistent state and next random draw after "
          "restore");
  auto queued = step::pop_foreground(loaded.queues);
  require(queued && std::get<react::EventWork>(queued->work).origin ==
                        amber::Origin{9, 4, {1, 2}, 1.5, false},
          "restored causal work retains full carried provenance");
}
void local_capture_filters_authority_and_runtime_rows() {
  Fixture original;
  original.populate();
  const std::array<const Relation *, 2> relations{&original.f.relations.at(3), &original.declared};
  const std::array<Handle, 1> seeds{original.f.a};
  auto objects = cp::neighborhood(identity(original.f.objects), parents(original.f.objects),
                                  original.parts, relations, seeds);
  require(
      objects.size() == 3 &&
          std::find(objects.begin(), objects.end(), original.part) != objects.end() &&
          std::find(objects.begin(), objects.end(), original.f.c) == objects.end(),
      "one ordinary graph edge plus full assembly closure does not expand through the whole world");
  auto catalog = cp::catalog_selection(original.f.models, original.parameters, original.f.installed,
                                       identity(original.f.objects), original.f.objects.params,
                                       original.f.rules, objects);
  const std::array<PlayerId, 1> actors{1};
  auto permission =
      perm::capture_authority(authority(original.f.objects), objects, actors, original.perm_state);
  snap::Buffer bytes;
  original.write(bytes, std::span<const Handle>(objects), &catalog, &permission);
  const std::array<std::string_view, 1> versions{"checkpoint-fixture"};
  Fixture captured;
  captured.read(snap::parse(bytes, versions));
  require(identity(captured.f.objects).valid(original.f.a) &&
              !identity(captured.f.objects).valid(original.f.c) &&
              !captured.f.properties[1].get(original.f.c.slot()) &&
              !captured.f.states.contains({original.f.c, original.unrelated}) &&
              !captured.instance_text.rows.contains(original.f.c.bits) &&
              !captured.f.random.contains(original.f.c.bits),
          "unrelated object, sparse state, script state, text and RNG are absent");
  require(
      captured.f.shared[4].get(original.f.a.slot())->total == 9 &&
          captured.f.shared[4].get(original.f.a.slot())->parts.size() == 2 &&
          !identity(captured.f.objects)
               .valid(captured.f.shared[4].get(original.f.a.slot())->parts[1].from),
      "captured shared answer preserves external contribution provenance without phantom objects");
  require(captured.f.relations.at(3).out.get(original.f.b.slot()) == nullptr &&
              captured.f.relations.at(3).in.get(original.f.c.slot()) == nullptr &&
              captured.parts.get(original.f.b.slot())->at(0).object == original.part,
          "boundary relation endpoints are pruned while named assembly identity remains complete");
  require(captured.f.models.rows[2].id == 0 && captured.parameters.rows[2].id == 0 &&
              captured.f.scripts.rows[original.unrelated].id == 0 &&
              captured.f.models.rows.size() == original.f.models.rows.size() &&
              captured.parameters.rows[1].parts.at("piece") == 3,
          "catalog capture keeps original ID high water and parameter topology without unrelated "
          "submissions");
  require(captured.ledger.balance.empty() && captured.profiles.by_key.empty(),
          "captures contain no balances or authentication key index");
  for (const auto &row : original.profiles.rows) {
    if (!row.id)
      continue;
    const auto &copy = captured.profiles.rows.at(row.id);
    if (permission.objects.contains(row.body))
      require(copy.id == row.id && copy.name == row.name && copy.body == row.body &&
                  copy.roles == row.roles && copy.connection_modes == 0 && copy.key_hashes.empty(),
              "captured body keeps player identity and roles without authentication data");
    else
      require(copy.id == 0, "profiles whose bodies are outside the capture are absent");
  }
  require(captured.grants.rows[3].id == 0 && captured.grants.rows[2].parent == 1 &&
              captured.grants.by_key.size() == 2 &&
              captured.players.rows[1].inventory == std::vector<Handle>{original.f.b} &&
              captured.offers.rows[1].live && captured.offers.rows[2].id == 0 &&
              captured.trades.rows[1].live,
          "captured local permission and goods rows remain scoped to selected objects");
  require(captured.queues.causal.size() == 1 && captured.queues.turns.empty() &&
              captured.inputs.rows.empty() && captured.intents.moving.size() == 1 &&
              captured.motion.size() == 1 && captured.requests.rows[2].id == 0 &&
              captured.captures.rows.size() == 1 && captured.gaps.rows.size() == 1,
          "capture filters local queued work, motions, requests and capture refs and starts no "
          "prior external jobs");
  require(captured.f.model_settings.rows.size() == 1 && captured.f.plots.rows.size() == 1,
          "only referenced model/plot settings remain while world settings stay available");
}
void world_runtime_survives_checkpoint_but_not_local_capture() {
  Fixture original;
  original.populate();
  const auto script = original.f.add(R"(return {kind='world',on={
    ['novel.cue']=function(self)
      assert(self.handle==nil)
      self.state.calls=(self.state.calls or 0)+1
      self.state.draw=self:random()
      self:after(0.5,'novel.cue')
    end}})");
  original.react_state.world_scripts.push_back(
      {script, "civic", "Public services", "The current service notice."});
  react::project_world(original.react_state, original.f.scripts);
  record::Journal journal{8, {}};
  auto run = [&](Fixture &fixture, const react::EventWork &event, Step now) {
    auto plans = step::plan_event(fixture.f.triggers, fixture.f.values(), event, now, 20);
    require(plans.size() == 1 && plans.front().handler.script == script,
            "world timer dispatch selects its installed script");
    react::Invocation invocation;
    react::CompletedInvocation completed;
    require(react::start_invocation(fixture.react_state, fixture.objects_state, 23,
                                    fixture.catalog_state, journal, plans.front().handler,
                                    std::move(plans.front().input), invocation, completed) &&
                completed.outcome.status == react::RunOutcome::Status::Ok,
            "checkpoint world handler executes in the ordinary VM");
    step::publish_invocation(journal, fixture.queues, fixture.timers, std::move(completed), {},
                             1000);
  };
  run(original, {{}, {}, 6, {}, {}, script}, 12);
  const auto saved_state = original.f.states.at({{}, script});
  const auto saved_random = original.f.random.at(0);
  snap::Buffer bytes;
  original.write(bytes);
  const std::array<std::string_view, 1> versions{"checkpoint-fixture"};
  Fixture loaded;
  loaded.read(snap::parse(bytes, versions));
  require(
      loaded.react_state.world_scripts.size() == 1 &&
          loaded.react_state.world_scripts.front().script == script &&
          loaded.react_state.world_scripts.front().space == "civic" &&
          loaded.react_state.world_scripts.front().description == "Public services" &&
          loaded.react_state.world_scripts.front().writing == "The current service notice." &&
          loaded.f.scripts.rows[script].declaration.kind == react::ScriptDeclaration::Kind::World &&
          loaded.f.states.at({{}, script}) == saved_state && loaded.f.random.at(0) == saved_random,
      "complete checkpoint retains world installation, declaration, writing, state and RNG");
  const auto timers = step::timers(loaded.timers);
  const auto timer = std::ranges::find_if(timers, [&](const auto &row) {
    return !row.event.receiver && row.event.recipient_script == script;
  });
  require(timer != timers.end() && timer->due == 22,
          "complete checkpoint retains the world timer's due time and script recipient");
  require(react::load_script(loaded.f.vm, original.f.code.at(script),
                             loaded.f.scripts.rows[script].declaration)
                  .status == react::RunOutcome::Status::Ok,
          "restored world declaration loads its original immutable script");
  react::project_world(loaded.react_state, loaded.f.scripts);
  run(loaded, timer->event, timer->due);
  require(std::get<double>(loaded.f.states.at({{}, script}).values.at(std::string("calls"))) == 2,
          "restored world timer continues the saved persistent state");

  const std::array<Handle, 1> selected{original.f.a};
  const auto catalog = cp::catalog_selection(original.f.models, original.parameters,
                                             original.f.installed, identity(original.f.objects),
                                             original.f.objects.params, original.f.rules, selected);
  const std::array<PlayerId, 1> actors{1};
  const auto permission =
      perm::capture_authority(authority(original.f.objects), selected, actors, original.perm_state);
  bytes.clear();
  original.write(bytes, std::span<const Handle>(selected), &catalog, &permission);
  Fixture captured;
  captured.read(snap::parse(bytes, versions));
  require(identity(captured.f.objects).valid(original.f.a) &&
              captured.react_state.world_scripts.empty() &&
              captured.f.scripts.rows[script].id == 0 &&
              !captured.f.states.contains({{}, script}) && !captured.f.random.contains(0),
          "local capture retains its selected object without world installation or runtime state");
  for (const auto &row : step::timers(captured.timers))
    require(row.event.receiver && row.event.recipient_script != script,
            "local capture must not restart a world service timer");
}
void local_capture_keeps_world_made_offers_and_their_authority() {
  Fixture original;
  original.populate();
  const auto maker_script = original.f.add("return {kind='world',on={}}");
  const auto parent_script =
      original.f.add("return {kind='world',on={['novel.cue']=function(self)end}}");
  const auto other_script = original.f.add(
      "return {kind='world',on={['novel.cue']=function(self)self.state.other=true end}}");
  const auto maker = perm::Holder::script(maker_script, {});
  const auto parent_holder = perm::Holder::script(parent_script, {});
  const auto other_holder = perm::Holder::script(other_script, {});
  original.react_state.world_scripts = {{maker_script, "market", "Local offers", {}},
                                        {parent_script, "land", "Land authority", {}},
                                        {other_script, "other", "Unrelated service", {}}};
  original.f.states[{{}, maker_script}].insert(std::string("offers"), 2.0);
  step::schedule(original.timers, {30, {{}, {}, 6, {}, {}, maker_script}, {}});
  const auto grant_to = [&](perm::Holder holder, PlayerId owner, perm::Scope scope,
                            perm::Rights rights) {
    const auto result =
        perm::create_grant(original.grants, original.players, perm::Holder::player(owner),
                           {owner, holder, scope, rights, 0, 0, 500},
                           {scope, owner, perm::all_rights}, {holder, 0, true}, 1);
    require(bool(result), "capture fixture creates an explicit world grant");
    return result.grant;
  };
  const auto parent =
      perm::create_root_grant(original.grants, original.players,
                              {0, parent_holder, perm::Scope::all_land(), 8u << 8, 0, 0, 1000},
                              {parent_holder, 0, true}, 1);
  require(bool(parent), "capture fixture has a root land delegation parent");
  const auto child =
      perm::create_grant(original.grants, original.players, parent_holder,
                         {0, maker, perm::Scope::plot(1), 8, 0, 0, 500},
                         {perm::Scope::plot(1), 1, 31, 1, true}, {maker, 0, true}, 2);
  require(bool(child), "world offer maker receives bounded plot transfer authority");
  const auto goods_grant =
      grant_to(maker, 1, perm::Scope::object(original.f.a), perm::bit(perm::Verb::Transfer));
  const auto remote_grant =
      grant_to(maker, 3, perm::Scope::object(original.f.c), perm::bit(perm::Verb::Use));
  const auto unrelated_grant = grant_to(other_holder, 1, perm::Scope::plot(1), 8);
  grant_to(other_holder, 3, perm::Scope::plot(2), 8);
  const auto target = [&](Fixture &fixture) {
    return perm::target(identity(fixture.f.objects), authority(fixture.f.objects), fixture.limits,
                        fixture.plots, original.f.a);
  };
  const auto goods = perm::offer(original.perm_state, permission_flags(original.f.objects), maker,
                                 target(original), 0, 2, 400, true, 3);
  const auto plot = perm::offer_plot(original.perm_state, maker, 1, 0, 2, 400, 3);
  const auto remote = perm::offer_plot(original.perm_state, other_holder, 2, 0, 2, 400, 3);
  require(goods && plot && remote && (original.f.objects.flags[original.f.a.slot()] & Escrow),
          "world makers create actual goods and plot offers before capture");

  const std::array selected{original.f.a};
  const std::array<PlayerId, 1> actors{2};
  const auto permission =
      perm::capture_authority(authority(original.f.objects), selected, actors, original.perm_state);
  const auto catalog = cp::catalog_selection(original.f.models, original.parameters,
                                             original.f.installed, identity(original.f.objects),
                                             original.f.objects.params, original.f.rules, selected);
  snap::Buffer bytes;
  original.write(bytes, std::span<const Handle>(selected), &catalog, &permission);
  const std::array<std::string_view, 1> versions{"checkpoint-fixture"};
  Fixture captured;
  captured.read(snap::parse(bytes, versions));
  require(captured.offers.rows[goods.offer].live && captured.offers.rows[plot.offer].live &&
              captured.offers.rows[goods.offer].maker == maker &&
              captured.offers.rows[plot.offer].maker == maker &&
              (captured.f.objects.flags[original.f.a.slot()] & Escrow) &&
              captured.grants.rows[goods_grant].live && captured.grants.rows[child.grant].live &&
              captured.grants.rows[child.grant].parent == parent.grant &&
              captured.grants.rows[parent.grant].live,
          "local capture retains world-made offers and the explicit grant chain for their land");
  require(!captured.offers.rows[remote.offer].id && !captured.grants.rows[remote_grant].id &&
              !captured.grants.rows[unrelated_grant].id &&
              captured.react_state.world_scripts.empty() &&
              !captured.f.scripts.rows[maker_script].id &&
              !captured.f.states.contains({{}, maker_script}) && captured.ledger.balance.empty(),
          "selected offers do not pull unrelated world grants, service runtime or balances into "
          "a local capture");
  for (const auto &timer : step::timers(captured.timers))
    require(bool(timer.event.receiver), "local offer capture restarted a world service timer");
  const perm::Buyer buyer{perm::Holder::player(2), 2, perm::AccountTarget::player(2), {}, false};
  require(perm::accept_plot(captured.perm_state, buyer, plot.offer, 0, 12) &&
              captured.plots.rows[1].owner == 2 &&
              perm::accept(captured.perm_state, captured.objects_state, buyer, target(captured),
                           goods.offer, 0, 10, 12) &&
              captured.f.objects.owner[original.f.a.slot()] == 2 &&
              !(captured.f.objects.flags[original.f.a.slot()] & Escrow) &&
              !captured.offers.rows[plot.offer].live && !captured.offers.rows[goods.offer].live &&
              captured.ledger.balance.empty(),
          "captured free offers can complete locally without restoring the world service or "
          "account balances");
}
void priority_capture_keeps_only_the_required_mechanical_edge() {
  Fixture original;
  original.populate();
  ObjectInit init;
  init.model = 3;
  auto required_part = create(original.f.objects, init);
  auto remote = create(original.f.objects, init);
  original.f.objects.parent[required_part.slot()] = original.f.c;
  original.parts.set(original.f.c.slot(),
                     std::vector<lifecycle::NamedPart>{{"dependent", required_part}});
  original.f.terms.rows.resize(12);
  original.f.terms.rows[10].id = 10;
  original.f.terms.rows[10].combine = react::Combine::Sum;
  original.f.terms.rows[11].id = 11;
  original.f.terms.rows[11].source = react::Source::Setting;
  original.f.model_settings.rows[2][11] = Value(3.0);
  auto &term = original.f.terms.rows[4];
  term.combine = react::Combine::SharePriority;
  term.paired = 10;
  term.priority = 11;
  original.f.shared[4].set(original.f.a.slot(), react::Shared{7, {{original.f.c, 7}}});
  original.f.shared[4].set(original.f.c.slot(), react::Shared{3, {{remote, 3}}});
  react::set_relation(original.f.relations.at(3), 3, original.f.c, remote, true,
                      original.f.effects);
  const std::array<Handle, 1> seeds{original.f.a};
  const std::array<const Relation *, 1> relations{&original.f.relations.at(3)};
  auto initial = cp::neighborhood(identity(original.f.objects), parents(original.f.objects),
                                  original.parts, relations, seeds);
  auto selected =
      cp::priority_neighborhood(identity(original.f.objects), parents(original.f.objects),
                                original.parts, original.f.terms, original.f.shared, initial);
  require(std::find(initial.begin(), initial.end(), original.f.c) == initial.end() &&
              std::find(selected.begin(), selected.end(), original.f.c) != selected.end() &&
              std::find(selected.begin(), selected.end(), required_part) != selected.end() &&
              std::find(selected.begin(), selected.end(), remote) == selected.end(),
          "capture includes competing priority demander and assembly without recursive shared or "
          "ordinary graph expansion");
}
} // namespace
int main() {
  try {
    round_trip_and_real_vm_continuation();
    local_capture_filters_authority_and_runtime_rows();
    world_runtime_survives_checkpoint_but_not_local_capture();
    local_capture_keeps_world_made_offers_and_their_authority();
    priority_capture_keeps_only_the_required_mechanical_edge();
    std::cout
        << "Thirteen-section assembly, local capture and real VM continuation checks passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
