#include "catalog/snapshot.hpp"
#include "react/snapshot.hpp"

#include <iostream>
#include <stdexcept>
using namespace amber;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
void reaction_roundtrip_and_capture() {
  using namespace react;
  const Handle a = Handle::make(1, 4), b = Handle::make(2, 7), outside = Handle::make(3, 8);
  Properties props(3);
  props[1].set(a.slot(), Value(3.0));
  props[1].set(outside.slot(), Value(90.0));
  Processes processes(3);
  processes[1].set(a.slot(), Process{3, 20, -1, 0, 5, 17});
  ScriptStates states;
  states[{a, 4}][std::string("handle")] = b;
  states[{a, 4}][std::int64_t{1}] = 4.0;
  states[{a, 4}][std::string("1")] = std::string("different");
  states[{outside, 4}][std::string("x")] = 99.0;
  Subscriptions subscriptions;
  bind_watch(subscriptions, {a, 1},
             {{b, 4}, 0, 21, Answer{true, Value(3.0)}, {{Threshold::Predicate::Below, 2}}});
  RandomStates random{{a.bits, 345}, {outside.bits, 567}};
  AuthoredRelations relations;
  DeferredEffects effects;
  set_relation(relations[2], 2, a, b, true, effects);
  SharedColumns shared(3);
  shared[2].set(a.slot(), Shared{10, {{b, 10}}});
  GivesTo gives;
  gives.set(b.slot(), std::vector<SubjectTerm>{{a, 2}});
  AllocationRows allocations;
  MeterRows meters;
  allocations.values[{a, b, 2, false}] = 10;
  rebuild_allocation_index(allocations);
  meters[{a, b, 2, false}] = {4, 20, 10};
  const std::array<Handle, 2> selected{a, b};
  SnapshotSelection selection(selected);
  snap::Buffer bytes;
  snap::Output out{bytes};
  write_properties(out, props, processes, selection);
  write_relations(out, relations, selection);
  write_shared(out, shared, gives, allocations, meters, selection);
  write_script_states(out, states, subscriptions, selection);
  write_random(out, random, selection);
  out.adapter().flush();
  bytes.resize(out.adapter().writtenBytesCount());
  Properties loaded_props;
  Processes loaded_processes;
  ScriptStates loaded_states;
  Subscriptions loaded_subscriptions;
  RandomStates loaded_random;
  AuthoredRelations loaded_relations;
  SharedColumns loaded_shared;
  GivesTo loaded_gives;
  AllocationRows loaded_allocations;
  MeterRows loaded_meters;
  snap::Input in{bytes.data(), bytes.size()};
  read_properties(in, loaded_props, loaded_processes, 4);
  read_relations(in, loaded_relations, 4);
  read_shared(in, loaded_shared, loaded_gives, loaded_allocations, loaded_meters, 4);
  read_script_states(in, loaded_states, loaded_subscriptions);
  read_random(in, loaded_random);
  snap::check_complete(in);
  require(loaded_props[1].get(a.slot()) && !loaded_props[1].get(outside.slot()),
          "capture includes selected state only");
  require(loaded_processes[1].get(a.slot())->revision == 17, "process revision survives snapshot");
  require(loaded_states.size() == 1 && loaded_states.at({a, 4}) == states.at({a, 4}),
          "flat state tags/keys/handles survive snapshot");
  require(loaded_random.size() == 1 && loaded_random.at(a.bits) == 345,
          "RNG selected full handle restored");
  require(loaded_subscriptions.rows.at({a, 1})[0].last == subscriptions.rows.at({a, 1})[0].last &&
              loaded_subscriptions.next_revision == subscriptions.next_revision,
          "watch answer/revision authoritative");
  require(loaded_subscriptions.by_consumer.contains({b, 4}), "watch derived index rebuilt");
  require(loaded_relations.at(2).in.get(b.slot())->front() == a, "bidirectional relation restored");
  require(loaded_gives.get(b.slot())->front().subject == a, "contribution reverse restored");
  require(metered(loaded_meters.at({a, b, 2, false}), 40, 20) == 14,
          "allocation integral restored without double metering");
  snap::Buffer empty;
  snap::Output empty_out{empty};
  SnapshotSelection none(std::span<const Handle>{});
  write_properties(empty_out, props, processes, none);
  empty_out.adapter().flush();
  empty.resize(empty_out.adapter().writtenBytesCount());
  snap::Input empty_in{empty.data(), empty.size()};
  read_properties(empty_in, loaded_props, loaded_processes, 4);
  require(loaded_props[1].value.empty() && loaded_processes[1].value.empty(),
          "empty selection does not clone world");
}
void catalog_roundtrip_and_capture() {
  using namespace catalog;
  Models models;
  Model first;
  first.id = 1;
  first.asset = std::string(64, 'a');
  first.source_hash = std::string(64, 'b');
  first.canonical_params = "{\"height\":1.0}";
  first.material = 3;
  first.author = 2;
  first.parts["leaf"] = 2;
  models.rows.push_back(first);
  auto second = first;
  second.id = 2;
  second.part_name = "leaf";
  models.rows.push_back(second);
  auto outside = first;
  outside.id = 3;
  models.rows.push_back(outside);
  ParameterBlocks params;
  params.rows.push_back({1, 1, std::string(64, 'c'), "{\"height\":2.0}", {{"variant_leaf", 2}}});
  rebuild_parameter_index(params);
  Scripts scripts;
  Script script;
  script.id = 1;
  script.artifact = std::string(64, 'd');
  script.registry_pin = std::string(64, 'e');
  script.declaration.id = 1;
  script.declaration.kind = react::ScriptDeclaration::Kind::Rule;
  script.declaration.approved = true;
  script.declaration.active = true;
  script.declaration.sets = {3};
  scripts.rows.push_back(script);
  ModelTexts texts;
  texts.rows[1] = {"root", "a real author", {}};
  texts.rows[3] = {"outside", {}, {}};
  InstanceTexts instances;
  const auto object = Handle::make(5, 18);
  instances.rows[object.bits] = {"placed", {}, "writing"};
  const std::array<ModelId, 2> selected_models{1, 2};
  const std::array<ScriptId, 1> selected_scripts{1};
  const std::array<std::uint32_t, 1> selected_params{1};
  const std::array<Handle, 1> selected_objects{object};
  snap::Buffer bytes;
  snap::Output out{bytes};
  write_models(out, models, std::span<const ModelId>(selected_models));
  write_parameters(out, params, std::span<const std::uint32_t>(selected_params));
  scripts.rows[1].tests = nlohmann::json::array(
      {{{"given", {{"subject", "target"}}},
        {"when", {{"steps", 1}}},
        {"expect", {{"subject", "target"}, {"term", "known.quantity"}, {"equals", 2}}}}});
  write_scripts(out, scripts, std::span<const ScriptId>(selected_scripts));
  write_text(out, texts, instances, std::span<const ModelId>(selected_models),
             std::span<const Handle>(selected_objects));
  out.adapter().flush();
  bytes.resize(out.adapter().writtenBytesCount());
  Models loaded_models;
  ParameterBlocks loaded_params;
  Scripts loaded_scripts;
  ModelTexts loaded_text;
  InstanceTexts loaded_instances;
  snap::Input in{bytes.data(), bytes.size()};
  read_models(in, loaded_models);
  read_parameters(in, loaded_params);
  read_scripts(in, loaded_scripts);
  require(loaded_scripts.rows[1].tests == scripts.rows[1].tests,
          "TEXT preserves verified fixture tests");
  read_text(in, loaded_text, loaded_instances);
  snap::check_complete(in);
  require(
      loaded_models.rows.size() == 4 && loaded_models.rows[3].id == 0 &&
          loaded_models.rows[1].canonical_params == first.canonical_params,
      "capture retains ID allocator/default parameter identity while omitting unrelated models");
  require(loaded_params.by_model_asset.at({1, std::string(64, 'c')}) == 1,
          "parameter block derived index restored");
  require(loaded_scripts.rows[1].declaration.approved &&
              loaded_scripts.rows[1].declaration.sets == std::vector<TermId>{3},
          "script metadata/pins restored");
  require(loaded_text.rows.size() == 1 &&
              loaded_instances.rows.at(object.bits).writing == "writing",
          "separate selected model/instance text restored");
}
} // namespace
int main() {
  reaction_roundtrip_and_capture();
  catalog_roundtrip_and_capture();
  std::cout << "reaction/catalog snapshot and selective capture checks passed\n";
}
