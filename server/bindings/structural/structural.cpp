#include "bindings/scene.hpp"
#include "private.hpp"

#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace amber::bindings {
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
std::string destroy_right(const StructuralRights &rights, const drive::NativeCall &call) {
  const auto target = call.request.target;
  const auto decision =
      perm::check(permission_holder(call.request.caller), perm::Verb::Destroy,
                  perm::target(rights.identity, rights.authority, rights.limits, target),
                  rights.players, rights.grants, call.step);
  return decision ? "" : "destroy: " + std::string(perm::reason_text(decision.reason));
}
InvocationInput native_input(const drive::NativeCall &call) {
  InvocationInput input;
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
lifecycle::Binding instance_binding(const nlohmann::json &value, const ConstructionCatalog &catalog,
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
  (void)catalog::resolve_asset(catalog.models, catalog.parameters, model, params);
  const auto material = !value.contains("material") ? catalog.models.rows.at(model).material
                        : value.at("material").is_number()
                            ? value.at("material").get<TermId>()
                            : terms.by_name.at(value.at("material").get<std::string>());
  if (terms.rows.at(material).kind != react::Kind::Material)
    throw std::invalid_argument("binding material must name a material");
  return {model, material, params};
}
drive::NativeCompletion structural_denied(std::string need, std::string on, std::string why,
                                          std::string fix) {
  drive::NativeCompletion out;
  out.result = record::Result::Denied;
  out.reply = {{"ok", false},
               {"denied",
                {{"need", std::move(need)},
                 {"on", std::move(on)},
                 {"why", std::move(why)},
                 {"fix", std::move(fix)}}}};
  return out;
}
void created_objects(drive::NativeCompletion &out, std::span<const Handle> objects) {
  if (objects.empty())
    return;
  if (!out.reply.contains("created"))
    out.reply["created"] = nlohmann::json::array();
  for (auto object : objects)
    out.reply["created"].push_back(to_string(object));
}
const perm::Plot *placement_plot(const StructuralRights &rights, Vec3 position) {
  for (const auto &plot : rights.plots.rows)
    if (plot.id && position.x >= plot.bounds[0] && position.y >= plot.bounds[1] &&
        position.x < plot.bounds[2] && position.y < plot.bounds[3])
      return &plot;
  return nullptr;
}
record::Event structural_record(const drive::NativeCall &call, record::Kind kind, std::string name,
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
void completed_effects(drive::NativeCompletion &out, react::DeferredEffects effects) {
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
record::Event birth_record(const drive::NativeCall &call, Handle object, IdentityRead identity,
                           const std::vector<PlayerId> &owners, const std::vector<PlotId> &plots,
                           const std::vector<std::uint32_t> &params, PlayerId maker,
                           CreditResolver credits) {
  auto event = structural_record(call, record::Kind::Create, "instance", {object});
  const auto slot = object.slot();
  event.detail = {{"model", "m:" + std::to_string(identity.model[slot])},
                  {"params", params[slot]},
                  {"material", identity.material[slot]},
                  {"maker", "p:" + std::to_string(maker)},
                  {"owner", "p:" + std::to_string(owners[slot])},
                  {"plot", plots[slot]}};
  if (credits.resolve)
    event.credits = credits.resolve(credits.context, std::span(&object, 1), 0, call.hz);
  return event;
}
} // namespace structural_detail
using namespace structural_detail;
void install_declared_attachments(AssemblyPlacement &placement, std::span<const Handle> objects) {
  for (auto h : objects) {
    const auto parent = placement.parents.parent[h.slot()];
    const auto *declared = placement.declared_parts.in.get(h.slot());
    if (!parent || !declared || std::ranges::find(*declared, parent) == declared->end())
      continue;
    const auto result = space::attach(
        placement.space, placement.identity, placement.transforms, placement.parents,
        placement.flags, placement.physical.contains, placement.physical.supports,
        placement.physical.attached_to,
        {h, parent, relative_pose(placement.transforms, h, parent), false}, placement.changes);
    if (result.status != space::Status::Ok)
      throw std::logic_error("declared attachment installation failed: " +
                             std::string(space::status_text(result.status)));
  }
}
drive::NativeCompletion create_instance(void *context, const drive::NativeCall &call) {
  auto &operation = *static_cast<CreateOperation *>(context);
  drive::NativeCompletion out;
  const auto &rights = operation.rights;
  const auto input = native_input(call);
  if (!rights.identity.valid(input.receiver))
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
  const auto *plot = placement_plot(rights, position);
  if (!plot)
    return structural_denied("build", "land", "creation position has no land parcel",
                             "choose a position inside a land parcel");
  const auto decision =
      perm::check_land(permission_holder(call.request.caller), perm::LandVerb::Build, *plot,
                       rights.players, rights.grants, call.step);
  if (!decision)
    return structural_denied("build", "plot:" + std::to_string(plot->id),
                             std::string(perm::reason_text(decision.reason)),
                             "ask p:" + std::to_string(plot->owner) + " for Build on this plot");
  const auto owner = std::holds_alternative<PlayerId>(call.request.caller)
                         ? std::get<PlayerId>(call.request.caller)
                         : rights.authority.owner[input.receiver.slot()];
  const auto binding =
      instance_binding(call.arguments, operation.catalog, operation.geometry.values.terms);
  auto assembly = lifecycle::expand(operation.catalog.models, operation.catalog.parameters,
                                    operation.catalog.assets, binding.model, binding.params);
  assembly.material = binding.material;
  std::vector<PhysicalDefinition> definitions;
  std::vector<space::BodySpec> bodies;
  std::size_t sensors = 0;
  auto prepare = [&](ModelId model, std::uint32_t params, TermId material, bool part) {
    const auto effective = catalog::resolve_asset(operation.catalog.models,
                                                  operation.catalog.parameters, model, params);
    const auto &asset = operation.catalog.assets.rows.at(effective.hash);
    auto definition =
        describe_physics(operation.geometry.geometry, asset, operation.geometry.values,
                         {model, params, material, plot->id, {}}, Static, operation.geometry.roles,
                         operation.geometry.defaults, call.step, call.hz);
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
  const auto capacity = space::can_add(operation.geometry.space, bodies, sensors);
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
  auto created = operation.instances.create(assembly, init);
  for (std::size_t i = 0; i < created.objects.size(); ++i)
    install_physics(operation.geometry.space, rights.identity, operation.geometry.transforms,
                    operation.geometry.flags, created.objects[i], definitions[i]);
  install_declared_attachments(operation.placement, created.objects);
  perm::update_plots(rights.identity, operation.geometry.transforms, operation.membership,
                     rights.plots, created.objects, true);
  react::DeferredEffects effects;
  objects_created(operation.lifecycle, created.objects, true, input, effects);
  out.changed = true;
  created_objects(out, created.objects);
  out.result = record::Result::Applied;
  out.reply["ok"] = true;
  out.reply["effect"] = "applied";
  for (auto h : created.objects)
    out.records.push_back(birth_record(call, h, rights.identity, rights.authority.owner,
                                       rights.authority.plot, operation.geometry.values.params,
                                       owner, operation.credits));
  completed_effects(out, std::move(effects));
  return out;
}
drive::NativeCompletion remove_instance(void *context, const drive::NativeCall &call) {
  auto &operation = *static_cast<RemoveOperation *>(context);
  auto error = destroy_right(operation.rights, call);
  if (!error.empty())
    return structural_denied(
        "destroy", to_string(call.request.target), std::move(error),
        "ask p:" + std::to_string(operation.rights.authority.owner[call.request.target.slot()]) +
            " for Destroy on this object");
  auto involved = operation.instances.closure(std::span{&call.request.target, 1});
  if (!can_retire_objects(operation.lifecycle, involved, error))
    return structural_denied("retirement", to_string(call.request.target), std::move(error),
                             "cancel the object's active offers or attachments before retrying");
  std::vector<record::Credit> object_credits;
  if (operation.credits.resolve)
    object_credits = operation.credits.resolve(operation.credits.context, involved, 0, call.hz);
  drive::NativeCompletion out;
  retire_objects(operation.lifecycle, involved, native_input(call), out.effects);
  out.changed = !involved.empty();
  out.result = out.changed ? record::Result::Applied : record::Result::NoEffect;
  out.reply = {{"ok", true}, {"effect", out.changed ? "applied" : "none"}};
  if (out.changed) {
    auto event = structural_record(call, record::Kind::Destroy, "instance", involved);
    event.credits = std::move(object_credits);
    out.records.push_back(std::move(event));
  }
  return out;
}
} // namespace amber::bindings
