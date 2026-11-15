#include "react/react.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace amber;
using namespace amber::catalog;
namespace {
void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class Fn> void rejects(Fn run, const char *message) {
  try {
    run();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error(message);
}
struct Fixture {
  amber::react::State react_state;
  amber::catalog::State catalog_state;

  react::Terms &terms = react_state.terms;
  react::Materials &materials = react_state.materials;
  react::RuleVersions &rules = react_state.rules;
  MechanicalBindings &bindings = catalog_state.mechanical;
  RuntimePins pins{"test-bake", "0.747", "ambergris-host-1", "5.6.0"};
  std::filesystem::path root = std::filesystem::current_path() / "catalog-test-artifacts";
  Fixture() {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    terms.rows.resize(12);
    for (TermId id = 1; id < 12; ++id) {
      auto &t = terms.rows[id];
      t.id = id;
      t.name = "invented.t" + std::to_string(id);
      terms.by_name[t.name] = id;
    }
    terms.rows[1].source = react::Source::Intrinsic;
    terms.rows[1].dimensions = {-3, 1, 0, 0, 0, 0, 0, 0};
    terms.rows[2].source = react::Source::Measured;
    terms.rows[2].measurement = react::MeasurementRole::Volume;
    terms.rows[2].dimensions = {3, 0, 0, 0, 0, 0, 0, 0};
    terms.rows[3].source = react::Source::Measured;
    terms.rows[3].dimensions = terms.rows[2].dimensions;
    terms.rows[4].source = react::Source::Measured;
    terms.rows[4].measurement = react::MeasurementRole::Mass;
    terms.rows[4].dimensions = {0, 1, 0, 0, 0, 0, 0, 0};
    terms.rows[5].kind = react::Kind::Action;
    terms.rows[6].kind = react::Kind::Flag;
    terms.rows[7].kind = react::Kind::Event;
    terms.rows[8].kind = react::Kind::Material;
    terms.rows[9].source = react::Source::Setting;
    terms.rows[10].kind = react::Kind::Relation;
    terms.rows[11].combine = react::Combine::Sum;
    materials.rows[8][1] = Value(2500.0);
  }
  ~Fixture() { std::filesystem::remove_all(root); }
  Hash registry() { return save_registry(root / "registry", terms, materials, rules, bindings); }
  Asset asset(std::string source, std::string canonical, double volume, double capacity) {
    std::string identity = source;
    identity.push_back('\0');
    identity += canonical;
    identity.push_back('\0');
    identity += pins.bake;
    const auto hash = sha256(identity);
    const auto directory = root / "assets" / hash;
    std::filesystem::create_directories(directory);
    nlohmann::json manifest = {{"version", 1},
                               {"hash", hash},
                               {"bake_version", pins.bake},
                               {"coordinates", "z_up_meters"},
                               {"params", nlohmann::json::parse(canonical)},
                               {"parameter_schema", {{"height", {1.0, 0.1, 3.0}}}},
                               {"default_material", "invented.t8"},
                               {"default_script", nullptr},
                               {"bounds", {{"min", {0, 0, 0}}, {"max", {1, 1, 1}}}},
                               {"volume", volume},
                               {"capacity", capacity},
                               {"parts", nlohmann::json::object()},
                               {"sites", nlohmann::json::object()},
                               {"bindings", nlohmann::json::object()},
                               {"replicated", {"invented.t6"}},
                               {"lods", nlohmann::json::array()},
                               {"collision",
                                {{"static", "collision.bin"},
                                 {"dynamic", "convex.bin"},
                                 {"mesh", "mesh.bin"},
                                 {"jolt", pins.jolt}}}};
    // This unit fixture exercises metadata/integrity, not geometry validation. The
    // bake tests validate real cgltf/Jolt artifacts before supervisor publication.
    for (const char *name :
         {"lod0.glb", "lod1.glb", "lod2.glb", "collision.bin", "convex.bin", "mesh.bin"}) {
      std::ofstream(directory / name) << "supervisor-verified fixture";
    }
    for (const char *name : {"lod0.glb", "lod1.glb", "lod2.glb"})
      manifest["lods"].push_back({{"file", name}, {"triangles", 12}});
    std::ofstream(directory / "source.py") << source;
    std::ofstream(directory / "manifest.json") << manifest.dump();
    auto prepared = read_asset(root / "assets", hash, canonical, pins);
    resolve_manifest_terms(prepared, terms);
    return prepared;
  }
};
void models_and_variants() {
  Fixture f;
  amber::catalog::State catalog_state;
  Assets &assets = catalog_state.assets;
  Models &models = catalog_state.models;
  Scripts &scripts = catalog_state.scripts;
  ModelTexts &texts = catalog_state.model_text;
  react::ModelSettings settings;
  ParameterBlocks &params = catalog_state.parameters;
  auto base = f.asset("same CAD program", "{\"height\":1.0}", 1, 0.5);
  auto variant = f.asset("same CAD program", "{\"height\":2.0}", 2, 1.5);
  const auto base_hash = base.hash, variant_hash = variant.hash;
  import_asset(assets, std::move(base));
  import_asset(assets, std::move(variant));
  ModelSubmission first;
  first.asset = base_hash;
  first.author = 1;
  first.text = {"first", "author one", {}};
  auto a = submit_model(models, texts, settings, assets, scripts, f.terms, first);
  auto second = first;
  second.author = 2;
  second.text = {"second", "author two", {}};
  auto b = submit_model(models, texts, settings, assets, scripts, f.terms, second);
  require(a.root != b.root && models.rows[a.root].asset == models.rows[b.root].asset,
          "geometry cache does not merge model identity");
  require(models.rows[a.root].author == 1 && models.rows[b.root].author == 2 &&
              texts.rows[b.root].name == "second",
          "submission attribution/text distinct");
  const auto block = add_variant(catalog_state, catalog_state, settings, f.terms, a.root,
                                 variant_hash, "{\"height\":2.0}");
  require(block != 0 && resolve_asset(models, params, a.root, block).hash == variant_hash,
          "global parameter block selects variant asset");
  rejects([&] { resolve_asset(models, params, b.root, block); },
          "another model's parameter block accepted");
  require(add_variant(catalog_state, catalog_state, settings, f.terms, a.root, variant_hash,
                      "{\"height\":2.0}") == block,
          "same model/asset block interned");
  react::AssetMeasurements &measured = catalog_state.baked_measurements;
  project_measurements(measured, a.root, 0, assets.rows.at(base_hash), {2, 3});
  project_measurements(measured, a.root, block, assets.rows.at(variant_hash), {2, 3});
  require(measured.rows.at(a.root).volume == 1 && measured.parameter_rows.at(block).volume == 2,
          "parameter measurements separate from model default");
  rejects([&] { read_asset(f.root / "assets", base_hash, "{\"height\":2.0}", f.pins); },
          "wrong canonical params accepted");
  auto wrong = f.pins;
  wrong.jolt = "wrong";
  rejects([&] { read_asset(f.root / "assets", base_hash, "{\"height\":1.0}", wrong); },
          "wrong native artifact version accepted");
  auto broken = assets.rows.at(base_hash);
  broken.manifest["replicated"] = {"unknown.term"};
  Asset prepared = broken;
  rejects([&] { resolve_manifest_terms(prepared, f.terms); }, "unknown replication term accepted");
  std::filesystem::remove(f.root / "assets" / base_hash / "lod1.glb");
  rejects([&] { read_asset(f.root / "assets", base_hash, "{\"height\":1.0}", f.pins); },
          "missing geometry artifact accepted");
}
void topology_changing_variants() {
  Fixture f;
  amber::catalog::State catalog_state;
  Assets &assets = catalog_state.assets;
  Models &models = catalog_state.models;
  Scripts &scripts = catalog_state.scripts;
  ModelTexts &texts = catalog_state.model_text;
  react::ModelSettings settings;
  ParameterBlocks &params = catalog_state.parameters;
  auto base = f.asset("parameterized assembly", "{\"height\":1.0}", 1, 0);
  auto changed = f.asset("parameterized assembly", "{\"height\":2.0}", 0, 0);
  auto no_parts = f.asset("parameterized assembly", "{\"height\":3.0}", 0, 0);
  auto part = [&](Asset &parent, const std::string &name, double volume) {
    auto child = parent;
    std::string identity = parent.hash;
    identity.push_back('\0');
    identity += "part";
    identity.push_back('\0');
    identity += name;
    child.hash = sha256(identity);
    child.parent_asset = parent.hash;
    child.part_name = name;
    child.volume = volume;
    child.manifest["hash"] = child.hash;
    child.manifest["parent_asset"] = child.parent_asset;
    child.manifest["part_name"] = name;
    child.manifest["volume"] = volume;
    child.manifest["parts"] = nlohmann::json::object();
    parent.manifest["parts"][name] = {
        {"asset", child.hash}, {"position", {0, 0, 0}}, {"rotation", {0, 0, 0, 1}}};
    import_asset(assets, std::move(child));
  };
  part(base, "old_leaf", 2);
  part(changed, "new_leaf", 3);
  part(changed, "knob", 0.5);
  const auto base_hash = base.hash, changed_hash = changed.hash, empty_hash = no_parts.hash;
  import_asset(assets, std::move(base));
  import_asset(assets, std::move(changed));
  import_asset(assets, std::move(no_parts));
  ModelSubmission submission;
  submission.asset = base_hash;
  submission.author = 17;
  submission.created_s = 42;
  submission.text = {"assembly", {}, {}};
  auto registered = submit_model(models, texts, settings, assets, scripts, f.terms, submission);
  auto variant = add_variant(catalog_state, catalog_state, settings, f.terms, registered.root,
                             changed_hash, "{\"height\":2.0}");
  const auto &parts = part_models(models, params, registered.root, variant);
  require(parts.size() == 2 && parts.contains("new_leaf") && parts.contains("knob") &&
              !parts.contains("old_leaf"),
          "parameter block supports added and removed part names");
  require(part_models(models, params, registered.root, 0).contains("old_leaf"),
          "default topology stays independent of variant");
  require(models.rows[parts.at("new_leaf")].author == 17 &&
              models.rows[parts.at("knob")].created_s == 42,
          "variant child identities retain original model attribution");
  require(assets.rows.at(resolve_asset(models, params, registered.root, variant).hash).volume == 0,
          "logical root transition preserves zero measured root mass");
  auto empty = add_variant(catalog_state, catalog_state, settings, f.terms, registered.root,
                           empty_hash, "{\"height\":3.0}");
  require(part_models(models, params, registered.root, empty).empty(),
          "all parts may disappear in another parameter variant");
  require(models.rows[registered.root].id == registered.root,
          "topology changes preserve root model identity");
}
void registry_roundtrip() {
  Fixture f;
  const auto hash = f.registry();
  auto document = read_registry(f.root / "registry", hash);
  react::Terms terms;
  react::Materials materials;
  load_terms(document, terms);
  load_materials(document, terms, materials);
  require(terms.rows.size() == f.terms.rows.size() && terms.by_name.at("invented.t8") == 8,
          "stable term identity roundtrip");
  require(materials.rows.at(8).at(1) == Value(2500.0), "material intrinsic value roundtrip");
  react::Term proposed = f.terms.rows[6];
  proposed.name = "invented.another_flag";
  proposed.id = 0;
  rejects([&] { append_term(terms, {proposed}, {}); }, "duplicate semantic term needs comparison");
  require(append_term(terms, {proposed}, {6, {}}) == 6, "explicit same_as resolves existing term");
  const auto appended = append_term(terms, {proposed}, {0, "different authored phenomenon"});
  require(appended == f.terms.rows.size(), "new term IDs append");
}
void script_worker_and_artifact() {
  Fixture f;
  const auto pin = f.registry();
  const react::RightsDeclaration rights{
      {"use", "take", "move", "edit", "copy", "transfer", "destroy"}};
  const std::string source = R"(return {
    kind='model', watches={'invented.t6'}, contributes={'invented.t11'},
    actions={['invented.t5']={right='use',reach=true}},
    on={['invented.t6']=function(self) self:contribute(self.handle,'invented.t11',3) end,
        ['invented.t5']=function(self) self:set('invented.t6',not self:get('invented.t6')) end,
        ['invented.t7']=function(self) end,
        ['invented.t1']=function(self) end}, tests={{given={subject='target',state={['invented.t6']=true}},
          when={steps=1},expect={subject='target',term='invented.t6',equals=true}}}
  })";
  auto prepared = prepare_script(source, f.terms, rights, pin, f.pins, {4096, 50, 5, 64});
  require(prepared.declaration.sets == std::vector<TermId>{6},
          "sets derived from actual literal host calls");
  require(prepared.declaration.watches.size() == 1 &&
              prepared.declaration.actions[0].right == "use",
          "top-level declaration derived");
  require(!prepared.declaration.approved, "source cannot approve itself");
  require(prepared.declaration.handlers == std::vector<TermId>{1, 5, 6, 7},
          "complete typed handler index comes from actual on entries");
  require(prepared.declaration.actions[0].reach, "declared action reach extracted from source");

  const auto hash = prepared.hash;
  write_script(f.root / "scripts" / hash, prepared);
  auto loaded = read_script(f.root / "scripts", hash, f.pins, pin, f.terms, rights);
  require(loaded.compiled.bytecode == prepared.compiled.bytecode, "immutable bytecode roundtrip");
  require(loaded.declaration.actions[0].reach == prepared.declaration.actions[0].reach,
          "action metadata survives artifact normalization");
  require(loaded.declaration.handlers == prepared.declaration.handlers,
          "handler index survives immutable artifact normalization");
  Scripts scripts;
  const auto a = import_script(scripts, prepared, 1, 0), b = import_script(scripts, prepared, 2, 0);
  require(a != b && scripts.rows[a].artifact == scripts.rows[b].artifact &&
              scripts.rows[b].author == 2,
          "script submission attribution separate from code identity");
  require(scripts.rows[a].tests == prepared.metadata.at("tests") &&
              scripts.rows[a].tests.size() == 1,
          "catalog retains only actual prepared declaration tests");
  rejects(
      [&] {
        prepare_script("return {kind='model',on={['invented.t8']=function(self) end}}", f.terms,
                       rights, pin, f.pins, {4096, 50, 5, 64});
      },
      "material term cannot be a handler trigger");
  rejects(
      [&] {
        prepare_script("return {kind='rule',on={}}", f.terms, rights, pin, f.pins,
                       {4096, 50, 5, 64});
      },
      "rule without selector/tests accepted");
  rejects(
      [&] {
        prepare_script("return {kind='model',on={['invented.t5']=function(self) "
                       "self:set('invented.t2',2) end},actions={['invented.t5']={right='use'}}}",
                       f.terms, rights, pin, f.pins, {4096, 50, 5, 64});
      },
      "measured writes accepted");
  rejects(
      [&] {
        prepare_script(
            "local n=0; return {kind='model',on={['invented.t7']=function(self) n+=1 end}}",
            f.terms, rights, pin, f.pins, {4096, 50, 5, 64});
      },
      "hidden scalar state accepted");
  std::ofstream(f.root / "scripts" / hash / "bytecode.luau", std::ios::app) << "corrupt";
  rejects([&] { read_script(f.root / "scripts", hash, f.pins, pin, f.terms, rights); },
          "corrupt bytecode accepted");
}
void presentation_role_validation() {
  Fixture f;
  f.terms.rows.resize(14);
  f.terms.rows[12].id = 12;
  f.terms.rows[12].name = "invented.lux";
  f.terms.by_name["invented.lux"] = 12;
  f.terms.rows[12].dimensions = {-2, 0, 0, 0, 0, 0, 1, 0};
  f.terms.rows[13].id = 13;
  f.terms.rows[13].name = "invented.candela";
  f.terms.by_name["invented.candela"] = 13;
  f.terms.rows[13].dimensions = {0, 0, 0, 0, 0, 0, 1, 0};
  Asset prepared;
  prepared.manifest = {
      {"replicated", nlohmann::json::array()},
      {"bindings",
       {{"observer", {{"eye", {0, 0, 1.6}}, {"forward", {0, 1, 0}}, {"dim_lux", "invented.lux"}}},
        {"light",
         {{"site", {0, 0, 2}},
          {"intensity", "invented.candela"},
          {"color", {1, 1, 1}},
          {"range", 12},
          {"source_radius_m", 0.05}}},
        {"zone",
         {{"ambient", "invented.lux"},
          {"priority", 3},
          {"shape", {{"kind", "box"}, {"size", {10, 8, 3}}, {"offset", {0, 0, 1.5}}}}}}}}};
  resolve_manifest_terms(prepared, f.terms);
  const auto valid = prepared;
  prepared.manifest["bindings"]["observer"]["dim_lux"] = -1;
  rejects([&] { resolve_manifest_terms(prepared, f.terms); },
          "negative observer threshold rejected");
  prepared = valid;
  prepared.manifest["bindings"]["observer"]["forward"] = {0, 0, 1};
  rejects([&] { resolve_manifest_terms(prepared, f.terms); },
          "vertical-only observer forward rejected");
  prepared = valid;
  prepared.manifest["bindings"]["observer"]["dim_lux"] = "invented.candela";
  rejects([&] { resolve_manifest_terms(prepared, f.terms); },
          "observer threshold uses lux dimensions");
  prepared = valid;
  prepared.manifest["bindings"]["light"]["source_radius_m"] = 0;
  rejects([&] { resolve_manifest_terms(prepared, f.terms); }, "light radius must be positive");
  prepared = valid;
  prepared.manifest["bindings"]["zone"]["shape"]["size"] = {1, -1, 1};
  rejects([&] { resolve_manifest_terms(prepared, f.terms); }, "zone dimensions must be positive");
  prepared = valid;
  prepared.manifest["bindings"]["observer"]["dim_lux"] = 2.5;
  resolve_manifest_terms(prepared, f.terms);
}
} // namespace
int main() {
  models_and_variants();
  topology_changing_variants();
  registry_roundtrip();
  script_worker_and_artifact();
  presentation_role_validation();
  std::cout << "catalog and declaration-worker checks passed\n";
}
