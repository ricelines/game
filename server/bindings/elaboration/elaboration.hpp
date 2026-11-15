#pragma once

#include "control/control.hpp"
#include "drive/drive.hpp"
#include "elaborate/elaborate.hpp"

namespace amber::bindings::elaboration {
using Json = nlohmann::json;
struct Declarations {
  TermId request_model = 0, requests = 0, claim = 0, answer = 0, refuse = 0, rule_activation = 0;
};
Declarations declare_elaboration(react::Terms &, command::NativeActions &);
struct Notification {
  void *local = nullptr;
  void (*send)(void *, const elaborate::CloseOutcome &) = nullptr;
};
struct RequestBinding {
  elaborate::Requests &requests;
  const elaborate::LastGaps &last;
};
struct RequestsBinding {
  const elaborate::Requests &requests;
  const elaborate::GapCounts &gaps;
  const control::Profiles &profiles;
  std::uint32_t page_size;
};
struct ClaimBinding {
  elaborate::Requests &requests;
  const control::Profiles &profiles;
};
struct PatchBinding {
  elaborate::Requests &requests;
  const control::Profiles &profiles;
  Notification notification;
};
struct DetailBinding {
  elaborate::Requests &requests;
  catalog::AuthoredTexts &texts;
  const control::Profiles &profiles;
  Notification notification;
};
struct RuleBinding {
  const elaborate::Requests &requests;
  elaborate::PendingFixtures &pending;
  const elaborate::CaptureRefs &captures;
  const catalog::Scripts &scripts;
  const control::Profiles &profiles;
  elaborate::FixtureWorker &worker;
  std::string runtime;
  elaborate::FixtureLimits limits;
};
// This is a three-operation dispatcher, carrying no live world tables.
struct AnswerDispatch {
  drive::NativeOperation rule, patch, detail;
};
enum class RuleUpdate { Validated, Enable, Disable };
struct RuleActivation {
  void *local = nullptr;
  void (*apply)(void *, ScriptId, RuleUpdate, Step, double hz, const record::Origin &,
                react::DeferredEffects &) = nullptr;
};
struct ActivationBinding {
  const catalog::Scripts &scripts;
  const control::Profiles &profiles;
  RuleActivation activation;
};
struct CompletionBinding {
  elaborate::Requests &requests;
  elaborate::PendingFixtures &pending;
  drive::Inputs &inputs;
  const catalog::Scripts &scripts;
  const control::Profiles &profiles;
  RuleActivation activation;
};
struct Completion {
  std::optional<drive::InputCompletion> input;
  std::optional<elaborate::CloseOutcome> notification;
  record::Origin origin;
  react::DeferredEffects effects;
};
Completion complete(CompletionBinding &, const elaborate::FixtureResult &, Step, double hz);
std::string validate_request_model(const Json &);
std::string validate_requests(const Json &);
std::string validate_claim(const Json &);
std::string validate_answer(const Json &);
std::string validate_answer_rule(const Json &);
std::string validate_answer_patch(const Json &);
std::string validate_answer_detail(const Json &);
std::string validate_refuse(const Json &);
std::string validate_rule_activation(const Json &);
drive::NativeCompletion request_model(void *, const drive::NativeCall &);
drive::NativeCompletion requests(void *, const drive::NativeCall &);
drive::NativeCompletion claim(void *, const drive::NativeCall &);
drive::NativeCompletion answer(void *, const drive::NativeCall &);
drive::NativeCompletion answer_rule(void *, const drive::NativeCall &);
drive::NativeCompletion answer_patch(void *, const drive::NativeCall &);
drive::NativeCompletion answer_detail(void *, const drive::NativeCall &);
drive::NativeCompletion refuse(void *, const drive::NativeCall &);
drive::NativeCompletion rule_activation(void *, const drive::NativeCall &);
} // namespace amber::bindings::elaboration
