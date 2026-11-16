#include "commerce_private.hpp"

namespace amber::step {
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
step::NativeCompletion
place_existing(amber::ObjectState &objects_state, amber::space::State &space_state,
               std::array<TermId, 4> relation_terms, amber::perm::State &perm_state,
               const GeometryAssets &geometry, const amber::catalog::State &catalog_state,
               PhysicsRoles roles, PhysicsDefaults defaults, amber::react::State &react_state,
               const step::NativeCall &call) {
  const auto values = amber::react::value_read(objects_state, catalog_state, react_state);

  auto &space = space_state.space;

  auto identity = amber::identity(std::as_const(objects_state.objects));
  auto transforms = amber::transforms(objects_state.objects);
  auto parents = amber::parents(objects_state.objects);

  auto &parts = objects_state.parts;
  auto &declared_parts = objects_state.declared_parts;
  auto membership = amber::plot_membership(objects_state.objects);
  auto &plots = perm_state.plots;
  auto &players = perm_state.players;
  auto &grants = perm_state.grants;
  auto authority = amber::authority(std::as_const(objects_state.objects));
  auto &limits = perm_state.owner_limits;
  auto &models = catalog_state.models;
  auto &parameters = catalog_state.parameters;
  auto &assets = catalog_state.assets;

  const auto holder = commerce_detail::actor(call);
  const auto item = commerce_detail::goods(identity, parts, declared_parts, authority, limits,
                                           perm_state.plots, call.request.target);
  const auto who = item.root.owner;
  if (who >= players.rows.size())
    return commerce_detail::denied(perm::Reason::UnknownHolder, "account",
                                   "p:" + std::to_string(who));
  const auto &player = players.rows[who];
  if (std::ranges::find(player.inventory, item.root.object) == player.inventory.end() &&
      std::ranges::find(player.mailbox, item.root.object) == player.mailbox.end())
    return commerce_detail::denied(perm::Reason::NotController, "inventory",
                                   to_string(item.root.object), who);
  if (item.root.flags & Escrow)
    return commerce_detail::denied(perm::Reason::InEscrow, "available", to_string(item.root.object),
                                   who);
  if (item.root.flags & Body)
    return commerce_detail::denied(perm::Reason::NotGoods, "goods", to_string(item.root.object),
                                   who);
  if (!(item.root.flags & Held))
    throw std::invalid_argument("item is not in hidden inventory custody");
  for (const auto &part : item.parts)
    if (part.owner != who || (part.flags & (Body | Escrow)))
      return commerce_detail::denied(perm::Reason::StaleGoods, "goods", to_string(part.object),
                                     part.owner);
  const auto allowed = perm::check(holder, perm::Verb::Move, item.root, players, grants, call.step);
  if (!allowed)
    return commerce_detail::denied(allowed.reason, "move", to_string(item.root.object), who);
  const auto position = call.arguments.at("position").get<std::array<float, 3>>();
  const Vec3 destination{position[0], position[1], position[2]};
  auto rotation = transforms.rotation[item.root.object.slot()];
  if (call.arguments.contains("rotation")) {
    const auto values = call.arguments.at("rotation").get<std::array<double, 4>>();
    const auto norm = std::sqrt(values[0] * values[0] + values[1] * values[1] +
                                values[2] * values[2] + values[3] * values[3]);
    rotation = {float(values[0] / norm), float(values[1] / norm), float(values[2] / norm),
                float(values[3] / norm)};
  }
  const perm::Plot *plot = nullptr;
  for (const auto &candidate : plots.rows)
    if (candidate.id && destination.x >= candidate.bounds[0] &&
        destination.y >= candidate.bounds[1] && destination.x < candidate.bounds[2] &&
        destination.y < candidate.bounds[3]) {
      plot = &candidate;
      break;
    }
  if (!plot)
    throw std::invalid_argument("destination is outside the region's plots");
  const auto build =
      perm::check_land(holder, perm::LandVerb::Build, *plot, players, grants, call.step);
  if (!build)
    return commerce_detail::denied(build.reason, "build", "plot:" + std::to_string(plot->id),
                                   plot->owner);
  const auto old = pose(transforms, item.root.object);
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
    poses.emplace(h.bits, space::Pose{destination + rotate(delta, transforms.position[h.slot()] -
                                                                      old.position),
                                      compose(delta, transforms.rotation[h.slot()])});
    const auto model = identity.model[h.slot()];
    const auto params = values.params[h.slot()];
    const auto hash = catalog::resolve_asset(models, parameters, model, params).hash;
    const auto physics = describe_physics(geometry, assets.rows.at(hash), values,
                                          {model, params, identity.material[h.slot()], plot->id, h},
                                          identity.flags[h.slot()], roles, defaults,
                                          amber::step::Clock{.step = call.step, .hz = call.hz});
    if (physics.body.geometry)
      bodies.push_back(physics.body);
    else
      logical.push_back(h);
    if (h == item.root.object)
      root_mode = physics.body.motion;
    motion.push_back({h, physics.body.motion, {}, {}, true});
    for (const auto &volume : *physics.volumes)
      volumes.push_back({h, volume.geometry, volume.local, volume.kind});
    for (const auto &term : values.terms.rows)
      if (term.id && term.source == react::Source::Setting)
        settings.push_back({{h, term.id}, react::read(values, h, term.id, call.step, call.hz)});
  }
  for (auto h : std::span{item.closure}.subspan(1)) {
    const auto parent = parents.parent[h.slot()];
    if (!poses.contains(parent.bits))
      throw std::invalid_argument("declared assembly has a stale internal parent");
    internal.push_back({h, parent, relative(poses.at(h.bits), poses.at(parent.bits)), false});
  }
  const bool promoted = root_mode == space::Motion::Dynamic && item.closure.size() > 1;
  const auto capacity =
      space::can_rebuild_assembly(space, bodies, volumes.size(), item.closure, promoted);
  if (capacity.status != space::Status::Ok)
    throw std::invalid_argument("placement: " + std::string(space::status_text(capacity.status)));
  // Capacity, rights and complete imported definitions are fixed before any authoritative write.
  auto edit = space::extract_assembly(space, identity, item.root.object,
                                      std::span{item.closure}.subspan(1));
  edit.in_scene = true;
  edit.promoted = promoted;
  edit.motion = std::move(motion);
  for (auto h : item.closure) {
    transforms.position[h.slot()] = poses.at(h.bits).position;
    transforms.rotation[h.slot()] = poses.at(h.bits).rotation;
  }
  parents.parent[item.root.object.slot()] = {};
  const auto changes_begin = space_state.relation_changes.size();
  const auto installed =
      space::rebuild_assembly(objects_state, space_state, edit, bodies, logical, volumes, internal);
  if (installed.status != space::Status::Ok)
    throw std::invalid_argument(space::status_text(installed.status));
  std::erase(players.rows[who].inventory, item.root.object);
  std::erase(players.rows[who].mailbox, item.root.object);
  auto out =
      commerce_detail::result(true, {{"goods", to_string(item.root.object)},
                                     {"plot", plot->id},
                                     {"position", {destination.x, destination.y, destination.z}}});
  perm::update_plots(identity, transforms, membership, plots, item.closure, true);
  for (auto h : item.closure)
    react::reconcile_object_priorities(values, react_state, h, out.effects, call.step, call.hz);
  for (const auto &[key, before] : settings)
    if (before != react::read(values, key.subject, key.term, call.step, call.hz))
      out.effects.ordered.emplace_back(step::WatchWork{key, {}});
  for (const auto &row : std::span(space_state.relation_changes).subspan(changes_begin)) {
    const auto term = relation_terms.at(static_cast<std::size_t>(row.kind));
    if (term) {
      out.effects.ordered.emplace_back(step::WatchWork{{row.from, term}, {}});
      out.effects.ordered.emplace_back(step::WatchWork{{row.to, term}, {}});
    }
  }
  space_state.relation_changes.resize(changes_begin);
  out.changed_objects = item.closure;
  out.visible_objects = item.closure;
  commerce_detail::record(out, call, "place_existing", item.closure, out.reply);
  return out;
}
} // namespace amber::step
