#include "step.hpp"
#include <algorithm>

namespace amber::step {
namespace {
Handle handle(const nlohmann::json &value, Handle body) {
  const auto &text = value.get_ref<const std::string &>();
  if (text == "self")
    return body;
  const auto object = parse_handle(text);
  if (!object)
    throw std::invalid_argument("expected a complete object handle");
  return *object;
}
EventAtom atom(const react::Input &input, const nlohmann::json &value, Handle body) {
  if (value.is_null())
    return Value{};
  switch (input.type) {
  case react::InputType::Handle:
    return Value(handle(value, body));
  case react::InputType::Vector: {
    if (!value.is_array() || value.size() != 3)
      throw std::invalid_argument("expected a three-component vector");
    const auto v = value.get<std::array<float, 3>>();
    return Vec3{v[0], v[1], v[2]};
  }
  case react::InputType::String:
    return value.get<std::string>();
  case react::InputType::Boolean:
    return Value(value.get<bool>());
  case react::InputType::Term:
    if (value.is_string())
      return value.get<std::string>();
    return Value(value.get<double>());
  case react::InputType::Integer:
    if (value.is_number_integer())
      return Value(value.get<std::int64_t>());
    return Value(value.get<double>());
  case react::InputType::Number:
    return Value(value.get<double>());
  }
  std::terminate();
}
} // namespace
EventPayload decode_arguments(const react::Term &action, const nlohmann::json &arguments,
                              Handle body) {
  if (!arguments.is_object())
    throw std::invalid_argument("action args must be an input record");
  std::vector<EventField> fields;
  fields.reserve(arguments.size());
  for (const auto &[key, value] : arguments.items()) {
    const auto input = std::find_if(action.inputs.begin(), action.inputs.end(),
                                    [&](const auto &input) { return input.name == key; });
    if (input == action.inputs.end())
      throw std::invalid_argument("unknown action input: " + key);
    try {
      fields.push_back({key, atom(*input, value, body)});
    } catch (const std::exception &error) {
      throw std::invalid_argument(key + ": " + error.what());
    }
  }
  return fields;
}

std::variant<NativeCall, InputReply> parse_action_input(const Input &input,
                                                        const react::Terms &terms,
                                                        const react::NativeActions &native,
                                                        record::Journal &journal, Step now,
                                                        double hz) {
  NativeCall call;
  call.request.caller = input.caller;
  call.body = input.body;
  call.step = now;
  call.hz = hz;
  call.origin = input.origin;
  call.order = InputOrder{input.id, input.session, input.sequence, input.request};
  const auto name = input.fields.at("action").get<std::string>();
  call.arguments = input.fields.value("args", nlohmann::json::object());
  try {
    const auto registered = terms.by_name.find(name);
    if (registered == terms.by_name.end())
      throw std::invalid_argument("unknown registered action: " + name);
    call.request.action = registered->second;
    if (input.fields.contains("target"))
      call.request.target = handle(input.fields.at("target"), input.body);
    if (!native.rows.contains(call.request.action))
      call.request.args =
          decode_arguments(terms.rows[call.request.action], call.arguments, input.body);
    return call;
  } catch (const std::exception &error) {
    nlohmann::json reply{{"ok", false}, {"error", "bad_input"}, {"problem", error.what()}};
    if (call.request.action && terms.rows[call.request.action].kind == react::Kind::Action)
      reply["signature"] = react::action_signature(terms, native, call.request.action);
    record::Event event;
    event.step = now;
    event.kind = record::Kind::Action;
    event.actor = record::Actor::player(std::get<PlayerId>(input.caller));
    event.name = name;
    event.result = record::Result::Denied;
    event.detail = {{"args", call.arguments}, {"agent", true}, {"response", reply}};
    if (input.fields.contains("target"))
      event.detail["raw_target"] = input.fields.at("target");
    const auto origin = journal.append(std::move(event), input.origin);
    return InputReply{false, std::move(reply), origin};
  }
}

ActionOutcome cancel_input(Intents &intents, std::vector<IntentEnding> &endings,
                           record::Journal &journal, const Input &input, Step now) {
  const auto ending = cancel_move(intents, input.body);
  if (ending)
    endings.push_back(*ending);
  record::Event event;
  event.step = now;
  event.kind = record::Kind::Action;
  event.actor = record::Actor::player(std::get<PlayerId>(input.caller));
  event.name = "cancel";
  event.result = record::Result::NoEffect;
  event.things = {input.body};
  event.detail = {{"agent", true}};
  const auto origin = journal.append(std::move(event), input.origin);
  return {{{"ok", true}, {"effect", "none"}, {"reason", ending ? "cancelled" : "no_intent"}},
          origin};
}

InputReply submit_authoring(react::authoring::Worker &worker, const react::Terms &terms,
                            const catalog::RegistryDocument &registry, Input &input) {
  try {
    const react::authoring::RegistrySnapshot snapshot{terms, registry.hash};
    switch (input.kind) {
    case net::ControlKind::RegisterModel:
      worker.submit(input.id, react::authoring::model_request(input.fields, terms), snapshot);
      break;
    case net::ControlKind::RegisterVariant:
      worker.submit(input.id, react::authoring::variant_request(input.fields), snapshot);
      break;
    case net::ControlKind::RegisterScript:
      worker.submit(input.id, react::authoring::script_request(input.fields), snapshot);
      break;
    default:
      throw std::logic_error("non-authoring input reached the authoring worker");
    }
    input.awaiting = true;
    return {true, {}, {}};
  } catch (const std::exception &error) {
    return {false, {{"ok", false}, {"error", "bad_input"}, {"problem", error.what()}}, {}};
  }
}

void deliver_input(Inputs &inputs, net::Sessions &sessions, observe::Hearing &hearing,
                   std::uint32_t event_limit, std::uint64_t id, ActionOutcome outcome) {
  const auto found = inputs.rows.find(id);
  if (found == inputs.rows.end())
    return;
  if (outcome.deferred) {
    found->second.awaiting = true;
    found->second.origin = outcome.origin;
    return;
  }
  net::append_heard(outcome.reply, hearing, found->second.session,
                    found->second.fields.value("events_limit", event_limit));
  const auto completed = complete_input(inputs, id, std::move(outcome.reply));
  if (completed.session)
    sessions.complete(completed.session, completed.sequence, completed.fields);
}

std::pair<NativeCompletion, record::Origin>
complete_authoring(amber::catalog::State &catalog_state, amber::react::State &react_state,
                   amber::step::State &step_state, space::Space &space, record::Journal &journal,
                   const Input &input, react::authoring::Completion completion, bool session_live,
                   const std::filesystem::path &asset_root,
                   const std::filesystem::path &script_root) {
  auto &geometry = step_state.geometry;
  const auto &clock = step_state.clock;

  auto &vm = react_state.vm;

  const auto now = clock.step;
  const auto hz = clock.hz;

  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;

  auto &mechanical = catalog_state.mechanical;
  auto &native_declarations = react_state.native_declarations;

  const auto player = std::get<PlayerId>(input.caller);
  const bool variant = input.kind == net::ControlKind::RegisterVariant;
  const bool script = input.kind == net::ControlKind::RegisterScript;
  NativeCompletion installed;
  try {
    if (input.session && !session_live)
      throw std::invalid_argument("session_closed");
    if (const auto *failure = std::get_if<react::authoring::Failure>(&completion.result))
      throw std::invalid_argument(failure->error);
    const auto role = [&](const char *group, const char *name) {
      return mechanical.values.at(group).at(name).get<TermId>();
    };
    const catalog::MeasurementBindings measured{role("measurements", "volume"),
                                                role("measurements", "capacity")};
    const auto geometry_ready = [&](const auto &rows) {
      for (const auto &asset : rows)
        import_geometry_asset(geometry, space, asset, asset_root);
    };
    if (auto *prepared = std::get_if<react::authoring::PreparedModel>(&completion.result)) {
      geometry_ready(prepared->assets);
      for (const auto &rows : prepared->variants)
        geometry_ready(rows);
      installed = react::authoring::install_model(std::move(*prepared), player, now / hz,
                                                  catalog_state, react_state, measured);
    } else if (auto *prepared =
                   std::get_if<react::authoring::PreparedVariant>(&completion.result)) {
      geometry_ready(prepared->assets);
      installed = react::authoring::install_variant(std::move(*prepared), catalog_state,
                                                    react_state, measured);
    } else {
      installed = react::authoring::install_script(
          std::move(std::get<catalog::PreparedScript>(completion.result)), player, now / hz,
          scripts, vm, native_declarations, script_root);
    }
  } catch (const std::exception &error) {
    installed.result = record::Result::Denied;
    installed.reply = react::authoring::failure(error.what());
  }
  record::Event event;
  event.step = now;
  event.kind = variant ? record::Kind::Action : record::Kind::Create;
  event.actor = record::Actor::player(player);
  event.name = variant ? "register_variant" : script ? "register_script" : "register_model";
  event.result = installed.result;
  event.changed = installed.changed;
  event.detail = {{"of", variant  ? "model_variant"
                         : script ? "script"
                                  : "model"},
                  {"maker", "p:" + std::to_string(player)},
                  {"agent", true},
                  {"response", installed.reply}};
  if (installed.reply.contains("model"))
    event.detail["model"] = installed.reply["model"];
  if (installed.reply.contains("script"))
    event.detail["script"] = installed.reply["script"];
  std::vector<record::Credit> credits;
  if (variant) {
    const auto model_name = input.fields.at("model").get<std::string>();
    event.detail["model"] = model_name;
    const auto id = identifier(model_name, "m:");
    if (id < models.rows.size()) {
      const auto &model = models.rows[id];
      credits.push_back({model.author, model.created_s, false});
    }
  }
  const auto origin = journal.append(std::move(event), input.origin, credits);
  installed.reply["record_id"] = origin.cause;
  return {std::move(installed), origin};
}
std::variant<NativeCall, InputReply>
prepare_input(react::authoring::Worker &worker, const amber::react::State &react_state,
              const catalog::RegistryDocument &registry, amber::step::State &step_state,
              record::Journal &journal, Input &input, Step now, double hz) {
  auto &terms = react_state.terms;
  auto &native = react_state.native_declarations;
  auto &intents = step_state.intents;
  auto &endings = step_state.intent_endings;

  switch (input.kind) {
  case net::ControlKind::Do:
    return parse_action_input(input, terms, native, journal, now, hz);
  case net::ControlKind::Cancel: {
    auto outcome = cancel_input(intents, endings, journal, input, now);
    return InputReply{false, std::move(outcome.reply), outcome.origin};
  }
  case net::ControlKind::RegisterModel:
  case net::ControlKind::RegisterVariant:
  case net::ControlKind::RegisterScript:
    return submit_authoring(worker, terms, registry, input);
  case net::ControlKind::AuthoringFailed:
    return failed_authoring(journal, now, input);
  default:
    throw std::logic_error("query reached the mutation lane");
  }
}
} // namespace amber::step
