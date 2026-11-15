#include "bindings/authoring/authoring.hpp"
#include "bindings/authoring/failures.hpp"
#include "bindings/start/start.hpp"
#include "drive/drive.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

using namespace amber;
using namespace amber::bindings::authoring;
namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class Fn> void rejects(Fn fn, const char *message) {
  try {
    fn();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error(message);
}
const Json &named(const Json &rows, std::string_view name) {
  for (const auto &row : rows)
    if (row.at("key") == name)
      return row;
  throw std::runtime_error("missing actual content fixture");
}
std::vector<Completion> wait_for(Worker &worker, std::size_t count) {
  std::vector<Completion> completed;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (completed.size() < count && std::chrono::steady_clock::now() < deadline) {
    for (auto &row : worker.poll())
      completed.push_back(std::move(row));
    if (completed.size() < count)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(completed.size() == count, "background authoring did not complete");
  return completed;
}
struct GeometryProbe {
  std::size_t calls = 0;
  bool fail = false;
};
void geometry(void *local, const catalog::Asset &asset) {
  auto &probe = *static_cast<GeometryProbe *>(local);
  require(catalog::valid_hash(asset.hash) && asset.manifest.at("hash") == asset.hash,
          "geometry hook did not receive an actual verified asset");
  ++probe.calls;
  if (probe.fail)
    throw std::runtime_error("test geometry unavailable");
}
void failed_submissions_have_authenticated_records_without_catalog_ids() {
  record::Journal journal(8, {});
  drive::Clock clock;
  clock.step = 91;
  FailedAuthoring binding{journal, clock};
  drive::Input input;
  input.caller = PlayerId{7};
  input.fields = {{"job", "failed-cad-job"},
                  {"operation", "model"},
                  {"source_hash", std::string(64, 'a')},
                  {"errors", {"Actual worker rejected CAD"}}};
  const auto reply = failed_authoring(&binding, input);
  const auto records = journal.take();
  require(reply.fields.at("record_id") == 1 && records.size() == 1,
          "failed authoring did not return its actual journal receipt");
  const auto &event = records.front();
  require(event.at("actor") == "p:7" && event.at("maker") == "p:7" && event.at("agent") == true &&
              event.at("changed") == false && event.at("result") == "denied" &&
              event.at("kind") == "create" && event.at("of") == "model" &&
              !event.contains("model") && !event.contains("script") && event.at("things").empty(),
          "failed CAD fabricated an identity, lost authenticated attribution or became a change");
  input.fields["operation"] = "variant";
  input.fields["job"] = "failed-variant-job";
  (void)failed_authoring(&binding, input);
  const auto variant = journal.take().front();
  require(variant.at("kind") == "action" && variant.at("of") == "model_variant" &&
              variant.at("id") == 2,
          "failed variant lost its actual attempt type/sequence");
}
} // namespace
int main(int argc, char **argv) {
  try {
    failed_submissions_have_authenticated_records_without_catalog_ids();
    if (argc != 3)
      throw std::invalid_argument("authoring_test PLAN ASSET_ROOT");
    const auto started = std::chrono::steady_clock::now();
    const auto plan = bindings::start::read_plan(argv[1]);
    const auto directory = std::filesystem::current_path() / ".cache/authoring-test";
    std::filesystem::remove_all(directory);
    react::Terms terms;
    react::Materials materials;
    catalog::MechanicalBindings mechanical;
    const auto registry =
        bindings::start::load_registry(plan, directory / "registry", terms, materials, mechanical);
    const auto &model = named(plan.at("models"), "door");
    std::ifstream metadata(std::filesystem::path(argv[2]) / model.at("asset").get<std::string>() /
                           "manifest.json");
    const catalog::RuntimePins pins{Json::parse(metadata).at("bake_version")};
    const react::VmSettings limits{4096, 50, 5, 64};
    Json request = {{"asset", model.at("asset")},
                    {"canonical_params", model.at("canonical_params")},
                    {"material", model.at("material")},
                    {"text", {{"name", "An authored assembly"}, {"description", "Real baked CAD"}}},
                    {"manifest", {{"volume", 999999}, {"parts", Json::object()}}}};
    const auto decoded = model_request(request, terms);
    auto invalid = request;
    invalid["text"]["name"] = 7;
    rejects([&] { (void)model_request(invalid, terms); }, "numeric authored name accepted");
    invalid = request;
    invalid["material"] = "missing.material";
    rejects([&] { (void)model_request(invalid, terms); }, "unknown material accepted");
    invalid = request;
    invalid["canonical_params"] = "[]";
    rejects([&] { (void)model_request(invalid, terms); }, "non-object parameters accepted");
    Worker worker({argv[2], pins, limits});
    SubmissionQueue submission{worker, terms, registry};
    drive::InputOperations operations;
    register_inputs(operations, submission);
    require(operations.rows.size() == 3, "authoring operations were not registered together");
    drive::Inputs pending;
    pending.next_id = 100;
    drive::Input input;
    input.operation = "register_model";
    input.caller = PlayerId{9};
    input.fields = request;
    input.fields["material"] = "missing.material";
    const auto malformed =
        drive::invoke_input(pending, operations, drive::enqueue_input(pending, input));
    require(malformed && !malformed->fields.at("ok").get<bool>() && pending.rows.empty(),
            "submission decode failure did not preserve immediate input rejection");
    input.fields = request;
    require(!drive::invoke_input(pending, operations, drive::enqueue_input(pending, input)) &&
                pending.rows.at(101).awaiting,
            "model submission did not retain its accepted deferred identity");
    input.operation = "register_script";
    input.fields = {{"source", named(plan.at("scripts"), "lamp").at("source")}};
    require(!drive::invoke_input(pending, operations, drive::enqueue_input(pending, input)) &&
                pending.rows.at(102).awaiting,
            "script submission did not use the shared registered queue");
    worker.submit(103, ScriptRequest{"return {on={['missing.event']=function() end}}"},
                  {terms, registry.hash});
    auto wrong_params = decoded;
    wrong_params.canonical_params += " ";
    worker.submit(104, std::move(wrong_params), {terms, registry.hash});
    worker.submit(105, ScriptRequest{named(plan.at("scripts"), "wood_impact").at("source")},
                  {terms, registry.hash});
    auto completed = wait_for(worker, 5);
    for (std::size_t index = 0; index < completed.size(); ++index)
      require(completed[index].input_id == 101 + index, "worker changed accepted FIFO identity");
    require(std::holds_alternative<PreparedModel>(completed[0].result) &&
                std::holds_alternative<catalog::PreparedScript>(completed[1].result) &&
                std::holds_alternative<Failure>(completed[2].result) &&
                std::holds_alternative<Failure>(completed[3].result),
            "real artifact/VM validation outcomes incorrect");
    auto prepared = std::get<PreparedModel>(std::move(completed[0].result));
    require(prepared.assets.size() == 2 && prepared.assets[0].asset.volume != 999999,
            "transported manifest replaced real root/part geometry");
    catalog::Assets assets;
    catalog::Models models;
    catalog::ModelTexts texts;
    react::ModelSettings settings;
    react::AssetMeasurements measured;
    bindings::viewing::PublicSources sources;
    catalog::Scripts scripts;
    catalog::ParameterBlocks parameters;
    GeometryProbe probe;
    CatalogInstall install{assets,
                           models,
                           parameters,
                           texts,
                           settings,
                           measured,
                           sources,
                           scripts,
                           terms,
                           {terms.by_name.at("base.volume"), terms.by_name.at("base.capacity")},
                           {&probe, geometry}};
    auto failing = prepared;
    probe.fail = true;
    require(!install_model(std::move(failing), 9, 12, install).at("ok").get<bool>() &&
                models.rows.size() == 1,
            "failed geometry exposed a usable model");
    probe.fail = false;
    probe.calls = 0;
    const auto first = install_model(prepared, 9, 12, install);
    const auto second = install_model(prepared, 10, 15, install);
    require(first.at("ok") && second.at("ok") && first.at("model") != second.at("model"),
            "duplicate geometry merged model identity");
    require(models.rows.size() == 5 && probe.calls == 2 && sources.assets.size() == 2 &&
                measured.rows.size() == 4 && models.rows[1].author == 9 &&
                models.rows[3].author == 10 && models.rows[1].created_s == 12,
            "actual parts, public source, measurements or authorship lost");
    require(first.at("measured") == assets.rows.at(decoded.asset).manifest,
            "reply measurements came from submitted claims");
    Json variant_fields = {{"model", "m:1"},
                           {"expected_source_hash", models.rows[1].source_hash},
                           {"asset", decoded.asset},
                           {"canonical_params", decoded.canonical_params}};
    const auto variant_request_value = variant_request(variant_fields);
    pending.next_id = 106;
    input.operation = "register_variant";
    input.fields = variant_fields;
    require(!drive::invoke_input(pending, operations, drive::enqueue_input(pending, input)) &&
                pending.rows.at(106).awaiting,
            "variant submission did not use the shared registered queue");
    auto wrong_source = variant_request_value;
    wrong_source.expected_source_hash = catalog::sha256("different model source");
    worker.submit(107, wrong_source, {terms, registry.hash});
    auto variants = wait_for(worker, 2);
    require(std::holds_alternative<PreparedVariant>(variants[0].result) &&
                std::holds_alternative<Failure>(variants[1].result),
            "variant worker must derive the actual source identity from verified artifacts");
    auto variant = std::get<PreparedVariant>(std::move(variants[0].result));
    const auto completed_variant = variant;
    auto stale_variant = variant;
    stale_variant.expected_source_hash = wrong_source.expected_source_hash;
    const auto count_before_variant = models.rows.size();
    require(!install_variant(std::move(stale_variant), install).at("ok").get<bool>() &&
                models.rows.size() == count_before_variant && parameters.rows.size() == 1,
            "stale variant completion must preserve current model/parameter identities");
    const auto registered_variant = install_variant(std::move(variant), install);
    require(registered_variant.at("ok") && registered_variant.at("params") == 1 &&
                registered_variant.at("model") == "m:1" && parameters.rows[1].model == 1 &&
                parameters.rows[1].parts.size() == 1 &&
                measured.parameter_rows.at(1).volume == assets.rows.at(decoded.asset).volume &&
                models.rows[1].author == 9,
            "live variant completion must retain the family and publish its measured part graph");
    auto batch_request = request;
    const auto &base = named(plan.at("models"), "forecourt");
    const auto &other = named(plan.at("models"), "counter");
    batch_request["asset"] = base.at("asset");
    batch_request["canonical_params"] = base.at("canonical_params");
    batch_request["material"] = base.at("material");
    batch_request["variants"] = Json::array({{{"asset", other.at("asset")},
                                              {"canonical_params", other.at("canonical_params")},
                                              {"manifest", {{"volume", 888888}}}}});
    worker.submit(108, model_request(batch_request, terms), {terms, registry.hash});
    auto foreign_batch = batch_request;
    foreign_batch["variants"][0]["asset"] = model.at("asset");
    foreign_batch["variants"][0]["canonical_params"] = model.at("canonical_params");
    worker.submit(109, model_request(foreign_batch, terms), {terms, registry.hash});
    auto repeated_batch = batch_request;
    repeated_batch["variants"].push_back(repeated_batch["variants"][0]);
    worker.submit(110, model_request(repeated_batch, terms), {terms, registry.hash});
    auto staged = wait_for(worker, 3);
    require(std::holds_alternative<PreparedModel>(staged[0].result) &&
                std::holds_alternative<Failure>(staged[1].result) &&
                std::holds_alternative<Failure>(staged[2].result),
            "batched worker lost actual source or unique canonical-set checks");
    auto staged_model = std::get<PreparedModel>(std::move(staged[0].result));
    const auto completed_model = staged_model;
    require(staged_model.variants.size() == 1 && staged_model.variants[0][0].asset.volume != 888888,
            "batch transported manifest replaced actual variant measurements");
    auto invalid_staged = staged_model;
    // A late invalid prepared row exercises catalog preflight, independently of
    // the worker's earlier source validation, without creating live family IDs.
    invalid_staged.variants.push_back(prepared.assets);
    const auto models_before_batch = models.rows.size();
    const auto params_before_batch = parameters.rows.size();
    require(!install_model(std::move(invalid_staged), 9, 22, install).at("ok").get<bool>() &&
                models.rows.size() == models_before_batch &&
                parameters.rows.size() == params_before_batch,
            "bad final staged variant exposed a partial model family");
    const auto staged_reply = install_model(std::move(staged_model), 9, 22, install);
    require(staged_reply.at("ok") && staged_reply.at("variants").size() == 1 &&
                staged_reply.at("variants")[0].at("params") == params_before_batch &&
                models.rows.size() == models_before_batch + 1 &&
                parameters.rows.size() == params_before_batch + 1 &&
                measured.parameter_rows.contains(params_before_batch),
            "one batched model install did not expose its complete measured parameter set");
    react::Vm vm(limits);
    command::NativeActions native;
    ScriptInstall script_install{scripts, vm, sources, native, directory / "scripts"};
    const auto actual_script = std::get<catalog::PreparedScript>(std::move(completed[1].result));
    native.rows.emplace(terms.by_name.at("base.switch"), command::NativeDeclaration{});
    require(!install_script(actual_script, 9, 12, script_install).at("ok").get<bool>() &&
                scripts.rows.size() == 1,
            "reserved native action collision accepted");
    native.rows.clear();
    const auto script_result = install_script(actual_script, 9, 12, script_install);
    require(script_result.at("ok") && sources.scripts.at(1) == actual_script.source &&
                scripts.rows[1].author == 9,
            "actual compiled script installation or public source missing");
    auto rule = std::get<catalog::PreparedScript>(std::move(completed[4].result));
    require(!rule.declaration.active && !rule.declaration.approved,
            "worker approved a remote world rule");
    rule.declaration.active = true;
    rule.declaration.approved = true;
    require(install_script(std::move(rule), 9, 12, script_install).at("ok") &&
                !scripts.rows[2].declaration.active && !scripts.rows[2].declaration.approved,
            "completion bypassed remote approval workflow");
    const react::RightsDeclaration rights{
        {"use", "take", "move", "edit", "copy", "transfer", "destroy"}};
    const auto persisted = catalog::read_script(directory / "scripts", actual_script.hash, pins,
                                                registry.hash, terms, rights);
    require(persisted.source == actual_script.source && persisted.compiled.bytecode.size() > 0,
            "persisted compiled artifact did not verify");
    record::Journal journal(8, {});
    input.id = 2000;
    input.caller = PlayerId{11};
    input.operation = "register_model";
    input.origin.cause = 701;
    input.origin.root = 700;
    const auto before_completion = models.rows.size();
    const auto applied = complete_authoring(input, {input.id, completed_model}, true, install,
                                            script_install, journal, 500, 20, {});
    auto records = journal.take();
    require(applied.reply.at("ok") && applied.reply.at("record_id") == 1 &&
                applied.added_model_ids ==
                    std::vector<ModelId>{static_cast<ModelId>(before_completion)} &&
                applied.reply.at("variants").size() == 1 && records.size() == 1 &&
                records[0].at("kind") == "create" && records[0].at("of") == "model" &&
                records[0].at("actor") == "p:11" && records[0].at("root") == 700 &&
                records[0].at("cause") == 701 && records[0].at("changed") == true &&
                models.rows[before_completion].author == 11 &&
                models.rows[before_completion].created_s == 25,
            "batched completion lost one chosen record, accepted cause or added definitions");
    input.operation = "register_variant";
    input.fields = variant_fields;
    const auto unchanged = complete_authoring(input, {input.id, completed_variant}, true, install,
                                              script_install, journal, 501, 20, {});
    records = journal.take();
    require(unchanged.reply.at("ok") && unchanged.reply.at("record_id") == 2 &&
                unchanged.added_model_ids.empty() && records.size() == 1 &&
                records[0].at("kind") == "action" && records[0].at("of") == "model_variant" &&
                records[0].at("result") == "no_effect" && records[0].at("changed") == false &&
                records[0].at("authors") == Json::array({"p:11", "p:9"}) &&
                records[0].at("oldest") == 12,
            "duplicate variant completion lost its source credit or became another change");
    input.operation = "register_script";
    const auto completed_script = complete_authoring(input, {input.id, actual_script}, true,
                                                     install, script_install, journal, 502, 20, {});
    records = journal.take();
    require(completed_script.reply.at("ok") && completed_script.added_model_ids.empty() &&
                records.size() == 1 && records[0].at("of") == "script" &&
                records[0].at("result") == "applied",
            "script completion did not use its distinct VM capability");
    const auto before_closed = models.rows.size();
    input.operation = "register_model";
    const auto closed = complete_authoring(input, {input.id, completed_model}, false, install,
                                           script_install, journal, 503, 20, {});
    records = journal.take();
    require(!closed.reply.at("ok").get<bool>() &&
                closed.reply.at("errors")[0] == "session_closed" &&
                closed.reply.at("record_id") == 4 && closed.added_model_ids.empty() &&
                models.rows.size() == before_closed && records.size() == 1 &&
                records[0].at("result") == "denied" && records[0].at("changed") == false &&
                !records[0].contains("model"),
            "closed-session completion installed a model or lost the one failed-attempt receipt");
    const auto rejected = complete_authoring(input, {input.id, Failure{"actual worker rejection"}},
                                             true, install, script_install, journal, 504, 20, {});
    records = journal.take();
    require(!rejected.reply.at("ok").get<bool>() && rejected.reply.at("record_id") == 5 &&
                rejected.added_model_ids.empty() && records.size() == 1 &&
                records[0].at("errors")[0] == "actual worker rejection" &&
                records[0].at("result") == "denied" && records[0].at("changed") == false,
            "worker failure fabricated identity or lost its single completion receipt");
    const auto batch_started = std::chrono::steady_clock::now();
    for (std::uint64_t index = 0; index < 50; ++index)
      worker.submit(1000 + index, decoded, {terms, registry.hash});
    const auto batch_enqueued = std::chrono::steady_clock::now();
    auto batch = wait_for(worker, 50);
    for (std::size_t index = 0; index < batch.size(); ++index)
      require(batch[index].input_id == 1000 + index &&
                  std::holds_alternative<PreparedModel>(batch[index].result),
              "warm real-asset batch failed");
    const auto batch_finished = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    const auto enqueue_ms =
        std::chrono::duration<double, std::milli>(batch_enqueued - batch_started).count();
    const auto batch_ms =
        std::chrono::duration<double, std::milli>(batch_finished - batch_started).count();
    std::cout << "Authoring: actual CAD assembly, fresh VM validation, FIFO identity, distinct "
                 "models, collision rejection, persisted sources and unapproved rules passed; "
              << elapsed << " ms total; fixture " << pins.bake
              << "; 50 warm real-assembly jobs: " << enqueue_ms << " ms writer queue time, "
              << batch_ms << " ms total\n";
    std::filesystem::remove_all(directory);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
