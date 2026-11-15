#include "drive.hpp"

#include <stdexcept>

namespace amber::drive {
void register_native(NativeOperations &operations, TermId term, NativeOperation operation) {
  if (!term || !operation.run || !operation.validate ||
      !operations.rows.emplace(term, operation).second)
    throw std::invalid_argument("invalid or duplicate native action registration");
}
NativeCompletion invoke_native(const NativeOperations &operations, NativeCall call) {
  NativeCompletion completion;
  try {
    auto operation = operations.rows.find(call.request.action);
    if (operation == operations.rows.end())
      throw std::invalid_argument("native action unavailable");
    const auto error = operation->second.validate(call.arguments);
    if (!error.empty())
      throw std::invalid_argument(error);
    if (!std::holds_alternative<PlayerId>(call.request.caller)) {
      call.body = {};
      call.order.reset();
    }
    completion = operation->second.run(operation->second.context, call);
    if (completion.reply.value("ok", false) && completion.reply.value("effect", "") == "none" &&
        !completion.reply.contains("reason"))
      completion.reply["reason"] = "no_effect";
  } catch (const std::exception &error) {
    completion.result = record::Result::Denied;
    completion.reply = {{"ok", false}, {"error", "bad_input"}, {"problem", error.what()}};
  }
  return completion;
}
} // namespace amber::drive
