#pragma once

#include "net_fixture.hpp"

#include "runtime_fixture.hpp"
#include "space_fixture.hpp"
#include "step/step.hpp"

namespace amber::checkpoint_test {
namespace cp = step::checkpoint;
struct Fixture {
  space::State space_state{testspace::settings(), 8};
  amber::perm::State perm_state;
  amber::net::State net_state{test_net::options()};
  amber::step::State step_state{8};
  amber::elaborate::State elaborate_state;

  test_runtime::Fixture f;
  amber::ObjectState &objects_state = f.objects_state;
  amber::catalog::State &catalog_state = f.catalog_state;
  amber::react::State &react_state = f.react_state;

  lifecycle::Parts &parts = objects_state.parts;
  Relation &declared = objects_state.declared_parts;
  space::PhysicalRelations &physical = space_state.physical_relations;
  catalog::ParameterBlocks &parameters = catalog_state.parameters;
  catalog::ModelTexts &model_text = catalog_state.model_text;
  catalog::InstanceTexts &instance_text = catalog_state.instance_text;
  catalog::AuthoredTexts &authored_text = catalog_state.authored_text;
  react::GivesTo &gives = react_state.gives_to;
  react::AllocationRows &allocations = react_state.allocations;
  react::MeterRows &meters = react_state.meters;
  perm::Players &players = perm_state.players;
  perm::Ledger &ledger = perm_state.ledger;
  perm::Plots &plots = perm_state.plots;
  perm::Groups &groups = perm_state.groups;
  perm::Grants &grants = perm_state.grants;
  perm::Offers &offers = perm_state.offers;
  perm::Trades &trades = perm_state.trades;
  perm::OwnerLimits &limits = perm_state.owner_limits;
  net::Profiles &profiles = net_state.profiles;
  step::TimerWheel &timers = step_state.timers;
  step::WorkQueues &queues = step_state.queues;
  step::Intents &intents = step_state.intents;
  step::Inputs &inputs = step_state.inputs;
  elaborate::Requests &requests = elaborate_state.requests;
  elaborate::GapCounts &gaps = elaborate_state.gap_counts;
  elaborate::LastGaps &last = elaborate_state.last_gaps;
  elaborate::CaptureRefs &captures = elaborate_state.captures;
  std::vector<space::MotionRow> motion;
  std::vector<step::BodyCauseRow> causes;
  std::uint64_t next_record = 31;
  Handle part;
  ScriptId script = 0, unrelated = 0;
  snap::Header header{"checkpoint-fixture", 12, {}};
  void populate(std::size_t extra = 0) {
    using namespace react;
    script = f.add(
        R"(return {kind='model',watches={{term='novel.heat',thresholds={{predicate='above',value=2}}}},
      on={['novel.heat']=function(self)self:set('novel.result',self:get('novel.heat')+3)end,
      ['novel.cue']=function(self)
        self.state.count=(self.state.count or 0)+1
        self.state.sample=self:get('novel.heat');self.state.draw=self:random()
      end}})");
    unrelated = f.add(R"(return {kind='model',watches={'novel.heat'},
      on={['novel.heat']=function(self)self.state.other=true end}})");
    f.models.rows.resize(4);
    f.models.rows[1].script = script;
    f.models.rows[1].asset = std::string(64, 'a');
    f.models.rows[1].parts = {{"piece", 3}};
    f.models.rows[2].id = 2;
    f.models.rows[2].script = unrelated;
    f.models.rows[2].asset = std::string(64, 'b');
    f.models.rows[3].id = 3;
    f.models.rows[3].asset = std::string(64, 'c');
    ObjectInit init;
    init.model = 3;
    init.owner = 2;
    part = create(f.objects, init);
    f.objects.model[f.b.slot()] = 3;
    f.objects.model[f.c.slot()] = 2;
    f.objects.owner[f.a.slot()] = 1;
    f.objects.owner[f.b.slot()] = 2;
    f.objects.owner[f.c.slot()] = 3;
    for (auto object : {f.a, f.b, part}) {
      f.objects.plot[object.slot()] = 1;
      f.objects.group[object.slot()] = 1;
    }
    f.objects.plot[f.c.slot()] = 2;
    f.objects.parent[part.slot()] = f.b;
    parts.set(f.b.slot(), std::vector<lifecycle::NamedPart>{{"piece", part}});
    lifecycle::rebuild_declared_parts(parts, identity(f.objects), declared);
    react::set_relation(f.relations.at(3), 3, f.a, f.b, true, f.effects);
    react::set_relation(f.relations.at(3), 3, f.b, f.c, true, f.effects);
    physical.attached_to.out.set(part.slot(), std::vector<Handle>{f.b});
    physical.attached_to.in.set(f.b.slot(), std::vector<Handle>{part});
    f.state(f.a, 1, 0);
    f.state(f.c, 1, 20);
    set_rate(f.react_state, f.terms.rows[1], f.a, 1, 0, 100, 0, 20, f.effects);
    auto armed = react::sync_watches(f.react_state, f.values(), {f.a, script},
                                     f.scripts.rows[script].declaration, 0, 20, {},
                                     react::SyncMode::Initial);
    for (auto &timer : armed.timers)
      step::schedule(timers, std::move(timer));
    f.bind(unrelated, f.c);
    f.states[{f.c, unrelated}][std::string("private")] = std::string("unrelated");
    const std::array<ScriptId, 1> explicit_scripts{script};
    react::set_instances(f.installed, f.b, explicit_scripts, f.scripts);
    f.shared.resize(5);
    f.shared[4].set(f.a.slot(), Shared{9, {{f.b, 4}, {f.c, 5}}});
    gives.set(f.b.slot(), std::vector<SubjectTerm>{{f.a, 4}});
    gives.set(f.c.slot(), std::vector<SubjectTerm>{{f.a, 4}});
    allocations.values[{f.a, f.b, 4, 0}] = 4;
    meters[{f.a, f.b, 4, 0}] = {1, 0, 2};
    f.instance_settings.columns.resize(2);
    f.instance_settings.columns[1].set(f.a.slot(), Value(2.0));
    f.model_settings.rows[1][1] = Value(3.0);
    f.model_settings.rows[2][1] = Value(4.0);
    f.plots.rows[1][1] = Value(5.0);
    f.plots.rows[2][1] = Value(6.0);
    f.world.values[1] = Value(7.0);
    parameters.rows.resize(3);
    parameters.rows[1] = {1, 1, std::string(64, 'd'), "{\"size\":2}", {{"piece", 3}}};
    parameters.rows[2] = {2, 2, std::string(64, 'e'), "{\"size\":4}", {}};

    f.objects.params[f.a.slot()] = 1;
    f.objects.params[f.c.slot()] = 2;
    model_text.rows[1].name = "selected model";
    model_text.rows[2].name = "unrelated model";
    instance_text.rows[f.a.bits].writing = "selected writing";
    instance_text.rows[f.c.bits].writing = "unrelated writing";
    catalog::append_authored_text(authored_text, f.a, 2, 1, catalog::AuthoredTextKind::Detail,
                                  "an authored local detail");
    catalog::append_authored_text(authored_text, f.c, 3, 2, catalog::AuthoredTextKind::Detail,
                                  "an unrelated detail");
    players.rows.resize(4);
    players.rows[1].inventory = {f.b, f.c};
    players.rows[1].mailbox = {part, f.c};
    players.rows[1].groups = {1};
    players.rows[1].grants_held = {1};
    players.rows[1].grants_given = {2};
    players.rows[3].groups = {2};
    plots.rows.resize(3);
    plots.rows[1] = {1, 1, 1, perm::all_rights, {-10, -10, 10, 10}, {}};
    plots.rows[2] = {2, 3, 2, 0, {20, 20, 30, 30}, {}};
    groups.rows.resize(3);
    groups.rows[1] = {1, 2, "local group"};
    groups.rows[2] = {2, 3, "remote group"};
    ledger.balance = {{perm::Account::player(1), 100},
                      {perm::Account::player(3), 900},
                      {perm::Account::object(f.a), 17},
                      {perm::Account::object(f.c), 23},
                      {perm::Account::group(1), 29}};
    grants.rows.resize(4);
    grants.rows[1] = {1,
                      2,
                      0,
                      perm::Holder::player(1),
                      perm::Scope::object(f.a),
                      perm::bit(perm::Verb::Use),
                      100,
                      10,
                      100,
                      0,
                      1000,
                      true};
    grants.rows[2] = {2,
                      1,
                      1,
                      perm::Holder::script(script, f.a),
                      perm::Scope::object(f.a),
                      perm::bit(perm::Verb::Use),
                      80,
                      4,
                      100,
                      0,
                      1000,
                      true};
    grants.rows[3] = {
        3, 3, 0,   perm::Holder::player(3), perm::Scope::object(f.c), perm::all_rights, 0, 0, 0,
        0, 0, true};
    perm::rebuild_grant_lookup(grants, players);
    offers.rows.resize(3);
    offers.rows[1] = {1, 1, 2, f.b, 5, 500, 1, true, {part}};
    offers.rows[2] = {2, 3, 0, f.c, 2, 500, 0, true, {}};
    trades.rows.resize(2);
    trades.rows[1] = {1, {1, 2}, {{{1, 0, false}, {0, 5, false}}}, true};
    limits.set(f.a.slot(), perm::bit(perm::Verb::Transfer));
    limits.set(f.c.slot(), perm::bit(perm::Verb::Copy));
    for (PlayerId id = 1; id <= 3; ++id)
      net::put_profile(
          profiles, {id,
                     "player" + std::to_string(id),
                     id == 1   ? f.a
                     : id == 2 ? f.c
                               : Handle{},
                     id == 1 ? std::vector<std::string>{"elaborator"} : std::vector<std::string>{},
                     net::all_modes,
                     {std::string(64, char('a' + id))}});
    const amber::Origin origin{9, 4, {1, 2}, 1.5, false};
    step::enqueue_event(queues, {f.a, f.b, 6, Value(true), origin}, 1000);
    step::enqueue_event(queues, {f.c, f.b, 6, Value(false), origin}, 1000);
    step::enqueue_background(queues, {f.a, 4, script, origin, 2});
    step::start_move(intents, f.a, f.b, origin, 1.2f);
    step::start_move(intents, f.c, Vec3{50, 50, 0}, origin);
    step::Input input;
    input.kind = net::ControlKind::RegisterScript;
    input.caller = PlayerId{1};
    input.body = f.a;
    input.origin = origin;
    auto input_work = step::enqueue_input(inputs, input);
    step::append_inputs(queues, {input_work});
    elaborate::note_gap(gaps, last, requests, {7, {4, 8}, f.a, 1, 1, "capture-local", origin}, 3);
    elaborate::note_gap(gaps, last, requests, {7, {1, 9}, f.c, 3, 1, "capture-remote", origin}, 3);
    captures.rows["capture-local"] = {
        "capture-local", "local.amb.zst", elaborate::CaptureStatus::Ready, {}};
    captures.rows["capture-remote"] = {
        "capture-remote", "remote.amb.zst", elaborate::CaptureStatus::Ready, {}};
    motion = {{f.a, {1, 2, 3}, {.1f, .2f, .3f}, false}, {f.c, {9, 9, 9}, {}, true}};
    const react::InvocationPlan cue{{script, "novel.cue", 1, 1, true}, f.a, f.b, 6, {}, {}};
    f.run(std::span<const react::InvocationPlan>(&cue, 1), 12);
    for (std::size_t i = 0; i < extra; ++i) {
      ObjectInit object;
      object.model = 2;
      auto handle = create(f.objects, object);
      f.properties[1].set(handle.slot(), Value(double(i)));
    }
  }
  void write(snap::Buffer &bytes, std::optional<std::span<const Handle>> selected = {},
             const cp::CatalogSelection *catalog = nullptr,
             const perm::SnapshotSelection *permission = nullptr) const {
    std::optional<cp::CaptureSelection> filter;
    if (selected) {
      filter.emplace();
      filter->objects.assign(selected->begin(), selected->end());
      filter->catalog = *catalog;
      filter->permissions = *permission;
    }
    cp::encode_checkpoint(bytes, header, f.objects_state, f.react_state, space_state,
                          f.catalog_state, perm_state, net_state, step_state, elaborate_state,
                          motion, causes, next_record, filter ? &*filter : nullptr);
  }
  void read(const snap::View &view) {
    auto decoded =
        cp::decode_checkpoint(view, f.objects_state, f.react_state, space_state, f.catalog_state,
                              perm_state, net_state, step_state, elaborate_state);
    next_record = decoded.next_record;
    motion = std::move(decoded.motion);
    causes = std::move(decoded.causes);
    f.relation_view.columns.clear();
    for (const auto &[term, relation] : f.relations)
      f.relation_view.columns[term] = &relation;
  }
};
} // namespace amber::checkpoint_test
