#include "step/step.hpp"

namespace amber::step {
InputReply failed_authoring(record::Journal &journal, Step step, const Input &input) {
  const auto player = std::get<PlayerId>(input.caller);
  const bool variant = input.fields.at("operation") == "variant";
  record::Event event;
  event.step = step;
  event.kind = variant ? record::Kind::Action : record::Kind::Create;
  event.actor = record::Actor::player(player);
  event.name = "bake_" + input.fields.at("operation").get<std::string>();
  event.result = record::Result::Denied;
  event.detail = input.fields;
  event.detail["of"] = variant ? "model_variant" : "model";
  event.detail["maker"] = "p:" + std::to_string(player);
  event.detail["agent"] = true;
  const auto origin = journal.append(std::move(event), input.origin);
  return {false, {{"ok", true}, {"record_id", origin.cause}}, origin};
}
} // namespace amber::step
