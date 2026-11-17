#include "catalog/catalog.hpp"
#include "replacement_private.hpp"
#include "step.hpp"

#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace amber::step {
namespace structural_detail {
perm::Holder permission_holder(Caller caller) {
  if (const auto *player = std::get_if<PlayerId>(&caller))
    return perm::Holder::player(*player);
  const auto &script = std::get<ScriptHolder>(caller);
  return perm::Holder::script(script.script, script.object);
}
bool declared_descendant(const Relation &parts, IdentityRead identity, Handle root, Handle child) {
  std::vector<Handle> pending{root};
  for (std::size_t i = 0; i < pending.size(); ++i) {
    if (pending[i] == child)
      return true;
    if (const auto *children = parts.out.get(pending[i].slot()))
      for (auto h : *children)
        if (identity.valid(h))
          pending.push_back(h);
  }
  return false;
}
react::InvocationInput native_input(const step::NativeCall &call) {
  react::InvocationInput input;
  input.receiver = std::holds_alternative<ScriptHolder>(call.request.caller)
                       ? std::get<ScriptHolder>(call.request.caller).object
                       : call.body;
  input.sender = input.receiver;
  input.trigger = call.request.action;
  input.step = call.step;
  input.hz = call.hz;
  input.origin = call.origin;
  input.action_caller = call.request.caller;
  input.action_body = call.body;
  input.action_target = call.request.target;
  return input;
}
lifecycle::Binding instance_binding(const nlohmann::json &value, const catalog::Models &models,
                                    const catalog::ParameterBlocks &parameters,
                                    const react::Terms &terms) {
  const auto text = value.at("model").get<std::string>();
  ModelId model = 0;
  if (!text.starts_with("m:"))
    throw std::invalid_argument("model must be an m:N identifier");
  const auto parsed = std::from_chars(text.data() + 2, text.data() + text.size(), model);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !model)
    throw std::invalid_argument("invalid model identifier");
  if (value.contains("params") &&
      (!value.at("params").is_number_integer() || value.at("params").get<std::int64_t>() < 0 ||
       value.at("params").get<std::uint64_t>() > UINT32_MAX))
    throw std::invalid_argument("params must be a nonnegative parameter id");
  if (value.contains("material") && value.at("material").is_number() &&
      !value.at("material").is_number_integer())
    throw std::invalid_argument("material must be a term id or material name");
  const auto params = value.value("params", std::uint32_t{0});
  (void)catalog::resolve_asset(models, parameters, model, params);
  const auto material = !value.contains("material") ? models.rows.at(model).material
                        : value.at("material").is_number()
                            ? value.at("material").get<TermId>()
                            : terms.by_name.at(value.at("material").get<std::string>());
  if (terms.rows.at(material).kind != react::Kind::Material)
    throw std::invalid_argument("binding material must name a material");
  return {model, material, params};
}
step::NativeCompletion structural_denied(std::string need, std::string on, std::string why,
                                         std::string fix) {
  step::NativeCompletion out;
  out.result = record::Result::Denied;
  out.reply = {{"ok", false},
               {"denied",
                {{"need", std::move(need)},
                 {"on", std::move(on)},
                 {"why", std::move(why)},
                 {"fix", std::move(fix)}}}};
  return out;
}
void created_objects(step::NativeCompletion &out, std::span<const Handle> objects) {
  if (objects.empty())
    return;
  if (!out.reply.contains("created"))
    out.reply["created"] = nlohmann::json::array();
  for (auto object : objects)
    out.reply["created"].push_back(to_string(object));
}
record::Event structural_record(const step::NativeCall &call, record::Kind kind, std::string name,
                                std::vector<Handle> objects) {
  record::Event event;
  event.step = call.step;
  event.kind = kind;
  event.actor = std::holds_alternative<PlayerId>(call.request.caller)
                    ? record::Actor::player(std::get<PlayerId>(call.request.caller))
                    : record::Actor::script(std::get<ScriptHolder>(call.request.caller).script);
  event.name = std::move(name);
  event.result = record::Result::Applied;
  event.changed = true;
  event.things = std::move(objects);
  return event;
}
void completed_effects(step::NativeCompletion &out, react::DeferredEffects effects) {
  out.effects.ordered.insert(out.effects.ordered.end(),
                             std::make_move_iterator(effects.ordered.begin()),
                             std::make_move_iterator(effects.ordered.end()));
}
space::Pose relative_pose(TransformRead transforms, Handle subject, Handle parent) {
  const auto rotation = transforms.rotation[parent.slot()];
  const Quat inverse{-rotation.x, -rotation.y, -rotation.z, rotation.w};
  return {rotate(inverse, transforms.position[subject.slot()] - transforms.position[parent.slot()]),
          compose(inverse, transforms.rotation[subject.slot()])};
}
record::Event birth_record(const step::NativeCall &call, Handle object,
                           const amber::ObjectState &objects_state,
                           const std::vector<PlayerId> &owners, const std::vector<PlotId> &plots,
                           PlayerId maker, const amber::catalog::State &catalog_state) {
  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto &params = objects_state.objects.params;
  auto attribution = amber::attribution(std::as_const(objects_state.objects));
  auto &models = catalog_state.models;
  auto &scripts = catalog_state.scripts;

  auto event = structural_record(call, record::Kind::Create, "instance", {object});
  const auto slot = object.slot();
  event.detail = {{"model", "m:" + std::to_string(identity.model[slot])},
                  {"params", params[slot]},
                  {"material", identity.material[slot]},
                  {"maker", "p:" + std::to_string(maker)},
                  {"owner", "p:" + std::to_string(owners[slot])},
                  {"plot", plots[slot]}};
  event.credits =
      record::credits(identity, attribution, models, scripts, std::span(&object, 1), 0, call.hz);
  return event;
}
} // namespace structural_detail
using namespace structural_detail;
void install_declared_attachments(amber::ObjectState &objects_state,
                                  amber::space::State &space_state,
                                  std::span<const Handle> objects) {

  auto transforms = amber::transforms(objects_state.objects);
  auto parents = amber::parents(objects_state.objects);

  auto &declared_parts = objects_state.declared_parts;

  for (auto h : objects) {
    const auto parent = parents.parent[h.slot()];
    const auto *declared = declared_parts.in.get(h.slot());
    if (!parent || !declared || std::ranges::find(*declared, parent) == declared->end())
      continue;
    const auto result = space::attach(objects_state, space_state,
                                      {h, parent, relative_pose(transforms, h, parent), false});
    if (result.status != space::Status::Ok)
      throw std::logic_error("declared attachment installation failed: " +
                             std::string(space::status_text(result.status)));
  }
}
NativeCompletion create_instance(amber::ObjectState &objects_state,
                                 const amber::catalog::State &catalog_state,
                                 const GeometryAssets &geometry, const react::ValueRead &values,
                                 amber::space::State &space_state,
                                 const amber::perm::State &perm_state, PhysicsRoles roles,
                                 PhysicsDefaults defaults, const NativeCall &call) {
  auto &space = space_state.space;

  auto &objects = objects_state.objects;
  auto &parts = objects_state.parts;
  auto &declared_parts = objects_state.declared_parts;
  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &assets = catalog_state.assets;
  auto transforms = amber::transforms(objects_state.objects);
  auto flags = amber::space_flags(objects_state.objects);

  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto &players = perm_state.players;
  auto &grants = perm_state.grants;
  auto &plots = perm_state.plots;

  const auto identity = values.identity;
  step::NativeCompletion out;
  const auto input = native_input(call);
  if (!identity.valid(input.receiver))
    return structural_denied("live_caller", to_string(input.receiver),
                             "creation requires a live caller", "enter the world with a body");
  const auto &point = call.arguments.at("position");
  if (!point.is_array() || point.size() != 3)
    throw std::invalid_argument("position must be [x,y,z]");
  const Vec3 position{point.at(0), point.at(1), point.at(2)};
  if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
    throw std::invalid_argument("position must be finite");
  Quat rotation{};
  if (call.arguments.contains("rotation")) {
    const auto &q = call.arguments.at("rotation");
    if (!q.is_array() || q.size() != 4)
      throw std::invalid_argument("rotation must be [x,y,z,w]");
    rotation = {q.at(0), q.at(1), q.at(2), q.at(3)};
    const auto norm = rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z +
                      rotation.w * rotation.w;
    if (!std::isfinite(norm) || std::abs(norm - 1.f) > 0.001f)
      throw std::invalid_argument("rotation must be a finite unit quaternion");
  }
  const auto *plot = perm::plot_at(plots, position);
  if (!plot)
    return structural_denied("build", "land", "creation position has no land parcel",
                             "choose a position inside a land parcel");
  const auto decision = perm::check_land(permission_holder(call.request.caller),
                                         perm::LandVerb::Build, *plot, players, grants, call.step);
  if (!decision)
    return structural_denied("build", "plot:" + std::to_string(plot->id),
                             std::string(perm::reason_text(decision.reason)),
                             "ask p:" + std::to_string(plot->owner) + " for Build on this plot");
  const auto owner = std::holds_alternative<PlayerId>(call.request.caller)
                         ? std::get<PlayerId>(call.request.caller)
                         : authority.owner[input.receiver.slot()];
  const auto binding = instance_binding(call.arguments, models, parameters, values.terms);
  auto assembly = lifecycle::expand(models, parameters, assets, binding.model, binding.params);
  assembly.material = binding.material;
  std::vector<PhysicalDefinition> definitions;
  std::vector<space::BodySpec> bodies;
  std::size_t sensors = 0;
  auto prepare = [&](ModelId model, std::uint32_t params, TermId material, bool part) {
    const auto effective = catalog::resolve_asset(models, parameters, model, params);
    const auto &asset = assets.rows.at(effective.hash);
    auto definition =
        describe_physics(geometry, asset, values, {model, params, material, plot->id, {}}, Static,
                         roles, defaults, amber::step::Clock{.step = call.step, .hz = call.hz});
    if (part) {
      definition.body.motion = space::Motion::Kinematic;
      definition.mode_flags = ScriptedMotion;
    }
    if (definition.body.geometry)
      bodies.push_back(definition.body);
    sensors += definition.volumes->size();
    definitions.push_back(definition);
  };
  prepare(assembly.model, assembly.params, assembly.material, false);
  for (const auto &part : assembly.parts)
    prepare(part.model, 0, part.material, true);
  const auto capacity = space::can_add(space, bodies, sensors);
  if (capacity.status != space::Status::Ok) {
    return structural_denied("physical_capacity", "land", space::status_text(capacity.status),
                             "choose a compatible model or remove unused physical objects");
  }
  ObjectInit init;
  init.position = position;
  init.rotation = rotation;
  init.owner = owner;
  init.maker = owner;
  init.created_step = call.step;
  init.plot = plot->id;
  init.preset = perm::all_rights << 16;
  auto created = lifecycle::instantiate(objects, parts, declared_parts, assembly, init);
  for (std::size_t i = 0; i < created.objects.size(); ++i)
    install_physics(space, identity, transforms, flags, created.objects[i], definitions[i]);
  install_declared_attachments(objects_state, space_state, created.objects);
  out.crossed_plots = perm::update_plots(objects, plots, created.objects, true);
  out.created_objects = created.objects;
  out.visible_objects = created.objects;
  out.changed = true;
  created_objects(out, created.objects);
  out.result = record::Result::Applied;
  out.reply["ok"] = true;
  out.reply["effect"] = "applied";
  for (auto h : created.objects) {
    auto event = structural_record(call, record::Kind::Create, "instance", {h});
    event.detail = {{"model", "m:" + std::to_string(identity.model[h.slot()])},
                    {"params", values.params[h.slot()]},
                    {"material", identity.material[h.slot()]},
                    {"maker", "p:" + std::to_string(owner)},
                    {"owner", "p:" + std::to_string(owner)},
                    {"plot", plots.rows[plot->id].id}};
    out.records.push_back(std::move(event));
  }

  return out;
}
NativeCompletion remove_instance(const lifecycle::Parts &parts, const react::ValueRead &values,
                                 AuthorityRead authority, AttributionRead attribution,
                                 const perm::Ledger &ledger, const catalog::Models &models,
                                 const catalog::Scripts &scripts, const NativeCall &call) {
  const auto identity = values.identity;
  auto involved = lifecycle::collect(parts, identity, call.request.target);
  const auto money = perm::can_refund_object_accounts(ledger, authority, involved);
  if (money != perm::Reason::Allowed)
    return structural_denied("retirement", to_string(call.request.target),
                             std::string(perm::reason_text(money)),
                             "reduce the recipient balance before destroying this account");
  NativeCompletion out;
  out.removed_objects = involved;
  out.changed = !involved.empty();
  out.result = out.changed ? record::Result::Applied : record::Result::NoEffect;
  out.reply = {{"ok", true}, {"effect", out.changed ? "applied" : "none"}};
  if (out.changed) {
    auto event = structural_record(call, record::Kind::Destroy, "instance", involved);
    event.credits = record::credits(identity, attribution, models, scripts, involved, 0, call.hz);
    out.records.push_back(std::move(event));
  }
  return out;
}
} // namespace amber::step
