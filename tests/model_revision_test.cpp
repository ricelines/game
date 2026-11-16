#include "react/react.hpp"

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
  amber::catalog::State catalog_state;
  amber::react::State react_state{{4096, 50, 5, 64}};

  Models &models = catalog_state.models;
  ParameterBlocks &parameters = catalog_state.parameters;
  Assets &assets = catalog_state.assets;
  ModelTexts &texts = catalog_state.model_text;
  react::ModelSettings &settings = react_state.model_settings;
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
  }
};
void preserved_defaults_and_variants() {
  Fixture f;
  const auto mappings = definition_parameters(f.models, f.parameters, f.assets, 1, 4);
  require(mappings.size() == 2 && mappings[0].after == 0 && mappings[1].after == 2,
          "default adopts new defaults and explicit values map to their actual baked variant");
  const auto old_model = f.models.rows[1];
  const auto old_count = f.parameters.rows.size();
  const auto edited = update_definition(f.models, f.parameters, f.texts, f.assets, 1, 4);
  inherit_definition_settings(f.settings, edited);
  require(edited.changed && edited.parameters.size() == 2 && edited.models.size() == 4,
          "direct edit deduplicates shared nested child definitions");
  const auto &current = f.models.rows[1];
  require(current.id == 1 && current.author == old_model.author &&
              current.created_s == old_model.created_s && current.script == old_model.script &&
              current.material == old_model.material && current.source_hash == f.new_source,
          "model identity, author, text and authored defaults survive foreign proposal geometry");
  const auto retained = current.parts.at("leaf");
  const auto &leaf = f.models.rows.at(retained);
  require(leaf.script == f.models.rows[2].script && leaf.author == f.models.rows[2].author &&
              leaf.asset == f.models.rows[5].asset && f.texts.rows[retained] == f.texts.rows[2] &&
              f.settings.rows[retained] == f.settings.rows[2] && current.parts.at("foot") == 7,
          "retained named children keep their defaults while new parts use proposal defaults");
  const auto pin = leaf.parts.at("pin");
  require(f.models.rows[pin].script == f.models.rows[3].script &&
              f.models.rows[pin].asset == f.models.rows[6].asset,
          "nested retained defaults survive recursively");
  require(f.parameters.rows.size() == old_count &&
              f.parameters.rows[1].canonical_params == R"({"added":5,"length":2})" &&
              resolve_asset(f.models, f.parameters, 1, 1).hash == f.parameters.rows[2].asset &&
              f.models.rows[4].parts.at("leaf") == 5,
          "existing explicit block IDs select the new definition, without mutating proposal rows");
  react::AssetMeasurements measured;
  project_definition_measurements(measured, f.models, f.parameters, f.assets, edited, {1, 2});
  require(measured.rows.at(1).volume == 2 && measured.parameter_rows.at(1).volume == 4 &&
              measured.rows.contains(retained) && measured.rows.contains(pin),
          "new measured facts cover current root, children and explicit parameter blocks");
  const auto unchanged = update_definition(f.models, f.parameters, f.texts, f.assets, 1, 4);
  require(!unchanged.changed && unchanged.models.empty(),
          "repeating the current definition does not create extra child identities");
}
void missing_parameter_rejects_before_writes() {
  Fixture f;
  f.parameters.rows[2].canonical_params = R"({"added":7,"length":2})";
  f.assets.rows.at(f.parameters.rows[2].asset).canonical_params =
      f.parameters.rows[2].canonical_params;
  const auto old_hash = f.models.rows[1].asset;
  rejects([&] { update_definition(f.models, f.parameters, f.texts, f.assets, 1, 4); },
          "a missing baked parameter set is not guessed");
  require(f.models.rows[1].asset == old_hash && f.models.rows.size() == 9 &&
              f.parameters.rows.size() == 3,
          "incompatible parameters do not mutate the live definition");
}
void dropped_keys_and_new_defaults() {
  Fixture f;
  for (auto &[hash, asset] : f.assets.rows) {
    (void)hash;
    if (asset.source_hash != f.new_source)
      continue;
    asset.manifest["parameter_schema"].erase("length");
    asset.canonical_params = R"({"added":5})";
  }
  f.models.rows[4].canonical_params = R"({"added":5})";
  f.parameters.rows[2].canonical_params = R"({"added":5})";
  const auto edited = update_definition(f.models, f.parameters, f.texts, f.assets, 1, 4);
  require(edited.changed && f.parameters.rows[1].canonical_params == R"({"added":5})",
          "removed keys are dropped and new keys use actual source defaults");
}
} // namespace
int main() {
  try {
    preserved_defaults_and_variants();
    missing_parameter_rejects_before_writes();
    dropped_keys_and_new_defaults();
    std::cout << "direct model definition checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
