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
  snap::Buffer bytes;
  original.write(bytes);
  const std::array<std::string_view, 1> versions{"checkpoint-fixture"};
  auto view = snap::parse(bytes, versions);
  Fixture loaded;
  loaded.read(view);
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
  require(queued && std::get<step::EventWork>(queued->work).origin ==
                        record::Origin{9, 4, {1, 2}, 1.5, false},
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
    priority_capture_keeps_only_the_required_mechanical_edge();
    std::cout
        << "Thirteen-section assembly, local capture and real VM continuation checks passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
