#include "bindings/scene.hpp"
#include "private.hpp"

namespace amber::bindings::commerce {
namespace {
space::Pose pose(TransformRead transforms, Handle h) {
  return {transforms.position[h.slot()], transforms.rotation[h.slot()]};
}
Quat inverse(Quat q) {
  return {-q.x, -q.y, -q.z, q.w};
}
space::Pose relative(space::Pose subject, space::Pose parent) {
  const auto inv = inverse(parent.rotation);
  return {rotate(inv, subject.position - parent.position), compose(inv, subject.rotation)};
}
} // namespace
drive::NativeCompletion place_existing(void *local, const drive::NativeCall &call) {
  auto &b = *static_cast<ExistingPlacementBinding *>(local);
  auto &custody = b.custody;
  const auto holder = detail::actor(call);
  const auto item = detail::goods(custody, b.authority, b.limits, call.request.target);
  const auto who = item.root.owner;
  if (who >= b.players.rows.size())
    return detail::denied(perm::Reason::UnknownHolder, "account", "p:" + std::to_string(who));
  const auto &player = b.players.rows[who];
  if (std::ranges::find(player.inventory, item.root.object) == player.inventory.end() &&
      std::ranges::find(player.mailbox, item.root.object) == player.mailbox.end())
    return detail::denied(perm::Reason::NotController, "inventory", to_string(item.root.object),
                          who);
  if (item.root.flags & Escrow)
    return detail::denied(perm::Reason::InEscrow, "available", to_string(item.root.object), who);
  if (item.root.flags & Body)
    return detail::denied(perm::Reason::NotGoods, "goods", to_string(item.root.object), who);
  if (!(item.root.flags & Held))
    throw std::invalid_argument("item is not in hidden inventory custody");
  for (const auto &part : item.parts)
    if (part.owner != who || (part.flags & (Body | Escrow)))
      return detail::denied(perm::Reason::StaleGoods, "goods", to_string(part.object), part.owner);
  const auto allowed =
      perm::check(holder, perm::Verb::Move, item.root, b.players, b.grants, call.step);
  if (!allowed)
    return detail::denied(allowed.reason, "move", to_string(item.root.object), who);
  const auto position = call.arguments.at("position").get<std::array<float, 3>>();
  const Vec3 destination{position[0], position[1], position[2]};
  auto rotation = b.transforms.rotation[item.root.object.slot()];
  if (call.arguments.contains("rotation")) {
    const auto values = call.arguments.at("rotation").get<std::array<double, 4>>();
    const auto norm = std::sqrt(values[0] * values[0] + values[1] * values[1] +
                                values[2] * values[2] + values[3] * values[3]);
    rotation = {float(values[0] / norm), float(values[1] / norm), float(values[2] / norm),
                float(values[3] / norm)};
  }
  const perm::Plot *plot = nullptr;
  for (const auto &candidate : b.plots.rows)
    if (candidate.id && destination.x >= candidate.bounds[0] &&
        destination.y >= candidate.bounds[1] && destination.x < candidate.bounds[2] &&
        destination.y < candidate.bounds[3]) {
      plot = &candidate;
      break;
    }
  if (!plot)
    throw std::invalid_argument("destination is outside the region's plots");
  const auto build =
      perm::check_land(holder, perm::LandVerb::Build, *plot, b.players, b.grants, call.step);
  if (!build)
    return detail::denied(build.reason, "build", "plot:" + std::to_string(plot->id), plot->owner);
  const auto old = pose(b.transforms, item.root.object);
  const auto delta = compose(rotation, inverse(old.rotation));
  std::unordered_map<std::uint64_t, space::Pose> poses;
  std::vector<space::BodySpec> bodies;
  std::vector<Handle> logical;
  std::vector<space::VolumeSpec> volumes;
  std::vector<space::EditedMotion> motion;
  std::vector<space::Attachment> internal;
  std::vector<std::pair<FactKey, react::Answer>> settings;
  space::Motion root_mode = space::Motion::Static;
  for (auto h : item.closure) {
    poses.emplace(h.bits, space::Pose{destination + rotate(delta, b.transforms.position[h.slot()] -
                                                                      old.position),
                                      compose(delta, b.transforms.rotation[h.slot()])});
    const auto model = custody.identity.model[h.slot()];
    const auto params = b.values.params[h.slot()];
    const auto hash = catalog::resolve_asset(b.models, b.parameters, model, params).hash;
    const auto physics =
        describe_physics(b.geometry, b.assets.rows.at(hash), b.values,
                         {model, params, custody.identity.material[h.slot()], plot->id, h},
                         custody.identity.flags[h.slot()], b.roles, b.defaults, call.step, call.hz);
    if (physics.body.geometry)
      bodies.push_back(physics.body);
    else
      logical.push_back(h);
    if (h == item.root.object)
      root_mode = physics.body.motion;
    motion.push_back({h, physics.body.motion, {}, {}, true});
    for (const auto &volume : *physics.volumes)
      volumes.push_back({h, volume.geometry, volume.local, volume.kind});
    for (const auto &term : b.values.terms.rows)
      if (term.id && term.source == react::Source::Setting)
        settings.push_back({{h, term.id}, react::read(b.values, h, term.id, call.step, call.hz)});
  }
  for (auto h : std::span{item.closure}.subspan(1)) {
    const auto parent = custody.parents.parent[h.slot()];
    if (!poses.contains(parent.bits))
      throw std::invalid_argument("declared assembly has a stale internal parent");
    internal.push_back({h, parent, relative(poses.at(h.bits), poses.at(parent.bits)), false});
  }
  const bool promoted = root_mode == space::Motion::Dynamic && item.closure.size() > 1;
  const auto capacity =
      space::can_rebuild_assembly(custody.space, bodies, volumes.size(), item.closure, promoted);
  if (capacity.status != space::Status::Ok)
    throw std::invalid_argument("placement: " + std::string(space::status_text(capacity.status)));
  // Capacity, rights and complete imported definitions are fixed before any authoritative write.
  auto edit = space::extract_assembly(custody.space, custody.identity, item.root.object,
                                      std::span{item.closure}.subspan(1));
  edit.in_scene = true;
  edit.promoted = promoted;
  edit.motion = std::move(motion);
  for (auto h : item.closure) {
    b.transforms.position[h.slot()] = poses.at(h.bits).position;
    b.transforms.rotation[h.slot()] = poses.at(h.bits).rotation;
  }
  custody.parents.parent[item.root.object.slot()] = {};
  std::vector<space::RelationChange> relations;
  const auto installed = space::rebuild_assembly(
      custody.space, custody.identity, b.transforms, custody.parents, custody.flags,
      custody.physical, edit, bodies, logical, volumes, internal, relations);
  if (installed.status != space::Status::Ok)
    throw std::logic_error("preflighted existing placement failed: " +
                           std::string(space::status_text(installed.status)));
  std::erase(b.players.rows[who].inventory, item.root.object);
  std::erase(b.players.rows[who].mailbox, item.root.object);
  auto out = detail::result(true, {{"goods", to_string(item.root.object)},
                                   {"plot", plot->id},
                                   {"position", {destination.x, destination.y, destination.z}}});
  perm::update_plots(custody.identity, b.transforms, b.membership, b.plots, item.closure, true);
  for (auto h : item.closure)
    reconcile_object_priorities(b.values, b.allocation, h, out.effects, call.step, call.hz);
  for (const auto &[key, before] : settings)
    if (before != react::read(b.values, key.subject, key.term, call.step, call.hz))
      out.effects.ordered.emplace_back(step::WatchWork{key, {}});
  for (const auto &row : relations) {
    const auto term = custody.relation_terms.at(static_cast<std::size_t>(row.kind));
    if (term) {
      out.effects.ordered.emplace_back(step::WatchWork{{row.from, term}, {}});
      out.effects.ordered.emplace_back(step::WatchWork{{row.to, term}, {}});
    }
  }
  scene_visible(custody.scene, item.closure, true);
  detail::record(out, call, "place_existing", item.closure, out.reply);
  return out;
}
} // namespace amber::bindings::commerce
