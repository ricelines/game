#include "catalog/revision.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace amber;
using namespace amber::catalog;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void rejects(F run, const char *message) {
  try {
    run();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error(message);
}
struct Fixture {
  Models models;
  ParameterBlocks parameters;
  Assets assets;
  ModelTexts texts;
  react::ModelSettings settings;
  Hash old_source = sha256("old source"), new_source = sha256("new source");
  Hash asset(std::string name, bool revised, std::string canonical, double volume = 1) {
    const auto id = sha256(name);
    Asset row;
    row.hash = id;
    row.source_hash = revised ? new_source : old_source;
    row.canonical_params = std::move(canonical);
    row.volume = volume;
    row.capacity = volume / 2;
    row.manifest = {{"parameter_schema", {{"length", {1, .1, 10}}}}};
    if (revised)
      row.manifest["parameter_schema"]["added"] = {5, 0, 10};
    assets.rows.emplace(id, std::move(row));
    return id;
  }
  void add(ModelId id, Hash hash, bool revised, std::string name = {}) {
    const auto &a = assets.rows.at(hash);
    models.rows.push_back({id,
                           hash,
                           a.source_hash,
                           name,
                           a.canonical_params,
                           revised ? TermId{6} : TermId{4},
                           revised ? PlayerId{8} : PlayerId{2},
                           revised ? 9.0 : 1.0,
                           revised ? ScriptId{21} : ScriptId{11},
                           {}});
    texts.rows[id] = {name.empty() ? "family" : name, revised ? "new claims" : "old claims", ""};
    settings.rows[id][99] = Value(revised ? 8.0 : 2.0);
  }
  Fixture() {
    add(1, asset("old root", false, R"({"length":1})"), false);
    add(2, asset("old leaf", false, R"({"length":1})"), false, "leaf");
    add(3, asset("old pin", false, R"({"length":1})"), false, "pin");
    add(4, asset("proposal root", true, R"({"added":5,"length":1})", 2), true);
    add(5, asset("proposal leaf", true, R"({"added":5,"length":1})"), true, "leaf");
    add(6, asset("proposal pin", true, R"({"added":5,"length":1})"), true, "pin");
    add(7, asset("new foot", true, R"({"added":5,"length":1})"), true, "foot");
    add(8, asset("variant leaf", true, R"({"added":5,"length":2})"), true, "leaf");
    models.rows[1].parts = {{"leaf", 2}};
    models.rows[2].parts = {{"pin", 3}};
    models.rows[4].parts = {{"leaf", 5}, {"foot", 7}};
    models.rows[5].parts = {{"pin", 6}};
    models.rows[8].parts = {{"pin", 6}};
    const auto old_variant = asset("old variant", false, R"({"length":2})", 2);
    const auto proposal_variant = asset("proposal variant", true, R"({"added":5,"length":2})", 4);
    parameters.rows.push_back(
        {1, 1, old_variant, assets.rows.at(old_variant).canonical_params, {{"leaf", 2}}});
    parameters.rows.push_back({2,
                               4,
                               proposal_variant,
                               assets.rows.at(proposal_variant).canonical_params,
                               {{"leaf", 8}, {"foot", 7}}});
    rebuild_parameter_index(parameters);
  }
};
void preserved_defaults_and_variants() {
  Fixture f;
  const std::array<std::uint32_t, 2> selected{1, 1};
  const auto plan = plan_revision(f.models, f.parameters, f.assets, 1, 4, selected);
  require(plan.changed && plan.variants.size() == 2 && plan.aliases.size() == 3,
          "one plan deduplicates parameter sets and shared nested aliases");
  const auto &root = revision_variant(plan, 0);
  const auto &variant = revision_variant(plan, 1);
  require(root.after_params == 0 && variant.proposal_params == 2 && variant.after_params == 3,
          "default and explicit parameters retain their distinct semantics");
  const auto retained = root.parts.at("leaf"), added = root.parts.at("foot");
  require(retained >= plan.model_count && added == 7,
          "retained geometry gets a fresh historical alias; new parts use proposal defaults");
  const auto old_model = f.models.rows[1];
  const auto old_block = f.parameters.rows[1];
  commit_revision(f.models, f.parameters, f.texts, f.settings, plan);
  const auto &current = f.models.rows[1];
  require(current.id == 1 && current.author == old_model.author &&
              current.created_s == old_model.created_s && current.script == old_model.script &&
              current.material == old_model.material && current.source_hash == f.new_source,
          "root identity and authored defaults survive a foreign proposal's geometry");
  const auto &leaf = f.models.rows.at(retained);
  require(leaf.script == f.models.rows[2].script && leaf.author == f.models.rows[2].author &&
              leaf.created_s == f.models.rows[2].created_s &&
              leaf.asset == f.models.rows[5].asset && f.texts.rows[retained] == f.texts.rows[2] &&
              f.settings.rows[retained] == f.settings.rows[2],
          "retained named-child text/settings/script/material lineage remains its own");
  const auto pin = leaf.parts.at("pin");
  require(f.models.rows[pin].script == f.models.rows[3].script &&
              f.models.rows[pin].asset == f.models.rows[6].asset,
          "nested retained parts preserve defaults recursively");
  require(f.parameters.rows[1].asset == old_block.asset &&
              f.parameters.rows[1].parts == old_block.parts &&
              f.models.rows[2].asset != leaf.asset && f.models.rows[4].parts.at("leaf") == 5,
          "old blocks, old child rows and the proposal are immutable history");
  require(f.parameters.rows[3].model == 1 &&
              f.parameters.rows[3].canonical_params == R"({"added":5,"length":2})",
          "explicit values survive a newly added default parameter");
  react::AssetMeasurements measured;
  project_revision_measurements(measured, plan, f.assets, {1, 2});
  require(measured.rows.at(1).volume == 2 && measured.parameter_rows.at(3).volume == 4 &&
              measured.rows.contains(retained) && measured.rows.contains(pin),
          "prospective measurements cover aliases and every current parameter mapping");
  rejects([&] { commit_revision(f.models, f.parameters, f.texts, f.settings, plan); },
          "a prepared definition plan cannot silently replay");
}
void reject_unprepared_changes() {
  Fixture f;
  const std::array<std::uint32_t, 1> selected{1};
  auto plan = plan_revision(f.models, f.parameters, f.assets, 1, 4, selected);
  auto stale = f.models;
  stale.rows[1].source_hash = sha256("interleaved source");
  rejects([&] { commit_revision(stale, f.parameters, f.texts, f.settings, plan); },
          "source changes invalidate a prepared definition edit");
  require(f.parameters.rows.size() == 3 && stale.rows.size() == 9,
          "stale rejection has no partial aliases or parameter writes");
  f.assets.rows.at(f.parameters.rows[2].asset).canonical_params = R"({"added":7,"length":2})";
  f.parameters.rows[2].canonical_params = R"({"added":7,"length":2})";
  rejects([&] { (void)plan_revision(f.models, f.parameters, f.assets, 1, 4, selected); },
          "new parameter values cannot be guessed instead of their authored defaults");
  f.assets.rows.at(f.parameters.rows[2].asset).canonical_params = R"({"added":5})";
  f.parameters.rows[2].canonical_params = R"({"added":5})";
  rejects([&] { (void)plan_revision(f.models, f.parameters, f.assets, 1, 4, selected); },
          "removing a currently used parameter fails explicitly");
  const auto unchanged = plan_revision(f.models, f.parameters, f.assets, 1, 1, selected);
  require(!unchanged.changed && unchanged.aliases.empty() &&
              revision_variant(unchanged, 1).after_params == 1,
          "an unchanged definition does not manufacture child or variant identities");
}
void benchmark() {
  Fixture f;
  f.assets.rows.at(f.models.rows[4].asset).manifest["parameter_schema"]["length"][2] = 1000;
  std::vector<std::uint32_t> selected{1};
  for (unsigned value = 3; value <= 100; ++value) {
    const auto old = std::string("{\"length\":") + std::to_string(value) + "}";
    const auto next = std::string("{\"added\":5,\"length\":") + std::to_string(value) + "}";
    const auto old_hash = f.asset("old" + std::to_string(value), false, old, value);
    const auto next_hash = f.asset("next" + std::to_string(value), true, next, value * 2);
    auto id = static_cast<std::uint32_t>(f.parameters.rows.size());
    f.parameters.rows.push_back({id, 1, old_hash, old, {{"leaf", 2}}});
    selected.push_back(id);
    ++id;
    f.parameters.rows.push_back({id, 4, next_hash, next, {{"leaf", 8}, {"foot", 7}}});
  }
  std::vector<double> elapsed;
  for (unsigned sample = 0; sample < 200; ++sample) {
    const auto begin = std::chrono::steady_clock::now();
    const auto plan = plan_revision(f.models, f.parameters, f.assets, 1, 4, selected);
    auto models = f.models;
    auto parameters = f.parameters;
    auto texts = f.texts;
    auto settings = f.settings;
    react::AssetMeasurements measurements;
    commit_revision(models, parameters, texts, settings, plan);
    project_revision_measurements(measurements, plan, f.assets, {1, 2});
    require(plan.variants.size() == 100 && models.rows[1].source_hash == f.new_source &&
                measurements.parameter_rows.size() == 99,
            "benchmark must preserve complete variant and mutation semantics");
    elapsed.push_back(
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin)
            .count());
  }
  std::sort(elapsed.begin(), elapsed.end());
  std::cout << "100-set catalog revision plan + staged table copies/commit/measurements us: p50="
            << elapsed[99] << " p95=" << elapsed[189] << " max=" << elapsed.back() << '\n';
}
} // namespace
int main(int argc, char **argv) {
  try {
    preserved_defaults_and_variants();
    reject_unprepared_changes();
    if (argc == 2 && std::string_view(argv[1]) == "--bench")
      benchmark();
    std::cout << "model revision checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
